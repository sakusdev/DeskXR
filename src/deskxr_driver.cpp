#include "protocol.hpp"

#include <openvr_driver.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace deskxr {

using namespace std::chrono_literals;

constexpr const char* kDriverSection = "driver_deskxr";
constexpr const char* kDisplaySection = "deskxr_display";
constexpr const char* kHmdSerial = "DESKXR-HMD-001";
constexpr const char* kLeftSerial = "DESKXR-L-001";
constexpr const char* kRightSerial = "DESKXR-R-001";
constexpr std::uint16_t kDefaultPort = 39742;

void Log(const char* fmt, ...) {
    if (vr::VRDriverLog() == nullptr) return;
    std::array<char, 1024> buffer{};
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer.data(), buffer.size(), fmt, args);
    va_end(args);
    vr::VRDriverLog()->Log(buffer.data());
}

float ReadFloat(const char* section, const char* key, float fallback) {
    vr::EVRSettingsError err = vr::VRSettingsError_None;
    const float value = vr::VRSettings()->GetFloat(section, key, &err);
    return err == vr::VRSettingsError_None ? value : fallback;
}

std::int32_t ReadInt(const char* section, const char* key, std::int32_t fallback) {
    vr::EVRSettingsError err = vr::VRSettingsError_None;
    const auto value = vr::VRSettings()->GetInt32(section, key, &err);
    return err == vr::VRSettingsError_None ? value : fallback;
}

struct Snapshot {
    protocol::PacketV1 packet{};
    bool fresh = false;
};

class SharedState {
public:
    void Store(const protocol::PacketV1& packet) {
        std::scoped_lock lock(mutex_);
        packet_ = packet;
        receivedAt_ = std::chrono::steady_clock::now();
        hasPacket_ = true;
    }

    Snapshot Load() const {
        std::scoped_lock lock(mutex_);
        Snapshot out{};
        if (!hasPacket_) return out;
        out.packet = packet_;
        out.fresh = (std::chrono::steady_clock::now() - receivedAt_) < 1000ms;
        return out;
    }

private:
    mutable std::mutex mutex_;
    protocol::PacketV1 packet_{};
    std::chrono::steady_clock::time_point receivedAt_{};
    bool hasPacket_ = false;
};

class UdpReceiver {
public:
    explicit UdpReceiver(SharedState& state) : state_(state) {}
    ~UdpReceiver() { Stop(); }

    bool Start(std::uint16_t port) {
        if (running_.exchange(true)) return true;

        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            running_ = false;
            Log("[DeskXR] WSAStartup failed");
            return false;
        }
        wsaStarted_ = true;

        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET) {
            Log("[DeskXR] socket() failed: %d", WSAGetLastError());
            Stop();
            return false;
        }

        DWORD timeoutMs = 100;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(port);

        if (bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
            Log("[DeskXR] bind UDP %u failed: %d", port, WSAGetLastError());
            Stop();
            return false;
        }

        thread_ = std::thread([this] { ThreadMain(); });
        Log("[DeskXR] listening on UDP %u", port);
        return true;
    }

    void Stop() {
        running_ = false;
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
        if (thread_.joinable()) thread_.join();

        {
            std::scoped_lock lock(peerMutex_);
            hasPeer_ = false;
        }

        if (wsaStarted_) {
            WSACleanup();
            wsaStarted_ = false;
        }
    }

    bool SendHaptic(protocol::Hand hand, float durationSeconds, float frequencyHz, float amplitude) {
        if (socket_ == INVALID_SOCKET) return false;

        sockaddr_in peer{};
        {
            std::scoped_lock lock(peerMutex_);
            if (!hasPeer_) return false;
            peer = peer_;
        }

        protocol::HapticPacketV1 packet{};
        std::memcpy(packet.magic, protocol::kHapticMagic, sizeof(packet.magic));
        packet.version = protocol::kVersion;
        packet.size = sizeof(packet);
        packet.sequence = hapticSequence_.fetch_add(1);
        packet.hand = hand;
        packet.durationSeconds = std::clamp(durationSeconds, 0.0f, 5.0f);
        packet.frequencyHz = std::max(0.0f, frequencyHz);
        packet.amplitude = std::clamp(amplitude, 0.0f, 1.0f);

        const int sent = sendto(
            socket_,
            reinterpret_cast<const char*>(&packet),
            static_cast<int>(sizeof(packet)),
            0,
            reinterpret_cast<const sockaddr*>(&peer),
            sizeof(peer));

        return sent == static_cast<int>(sizeof(packet));
    }

private:
    void ThreadMain() {
        while (running_) {
            protocol::PacketV1 packet{};
            sockaddr_in from{};
            int fromLen = sizeof(from);

            const int received = recvfrom(
                socket_,
                reinterpret_cast<char*>(&packet),
                static_cast<int>(sizeof(packet)),
                0,
                reinterpret_cast<sockaddr*>(&from),
                &fromLen);

            if (received == SOCKET_ERROR) {
                const int error = WSAGetLastError();
                if (!running_) break;
                if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) continue;
                Log("[DeskXR] recvfrom failed: %d", error);
                continue;
            }

            if (received != static_cast<int>(sizeof(packet))) continue;
            if (std::memcmp(packet.magic, protocol::kTrackingMagic, 4) != 0) continue;
            if (packet.version != protocol::kVersion) continue;
            if (packet.size != sizeof(protocol::PacketV1)) continue;

            {
                std::scoped_lock lock(peerMutex_);
                peer_ = from;
                hasPeer_ = true;
            }

            state_.Store(packet);

            if ((packet.sequence & 0x0Fu) == 0u) {
                protocol::AckPacketV1 ack{};
                std::memcpy(ack.magic, protocol::kAckMagic, sizeof(ack.magic));
                ack.version = protocol::kVersion;
                ack.size = sizeof(ack);
                ack.sequence = packet.sequence;

                sendto(
                    socket_,
                    reinterpret_cast<const char*>(&ack),
                    static_cast<int>(sizeof(ack)),
                    0,
                    reinterpret_cast<const sockaddr*>(&from),
                    sizeof(from));
            }
        }
    }

    SharedState& state_;
    std::atomic_bool running_{false};
    std::thread thread_;
    SOCKET socket_ = INVALID_SOCKET;
    bool wsaStarted_ = false;

    mutable std::mutex peerMutex_;
    sockaddr_in peer_{};
    bool hasPeer_ = false;
    std::atomic_uint32_t hapticSequence_{0};
};

struct DisplayConfig {
    std::int32_t windowX = 0;
    std::int32_t windowY = 0;
    std::uint32_t windowWidth = 1920;
    std::uint32_t windowHeight = 1080;
    std::uint32_t renderWidth = 1600;
    std::uint32_t renderHeight = 1600;
};

class VirtualDisplay final : public vr::IVRDisplayComponent {
public:
    explicit VirtualDisplay(DisplayConfig config) : config_(config) {}

    bool IsDisplayOnDesktop() override { return true; }
    bool IsDisplayRealDisplay() override { return false; }

    void GetRecommendedRenderTargetSize(std::uint32_t* width, std::uint32_t* height) override {
        *width = config_.renderWidth;
        *height = config_.renderHeight;
    }

    void GetEyeOutputViewport(vr::EVREye eye, std::uint32_t* x, std::uint32_t* y,
                              std::uint32_t* width, std::uint32_t* height) override {
        *y = 0;
        *width = config_.windowWidth / 2;
        *height = config_.windowHeight;
        *x = eye == vr::Eye_Left ? 0 : config_.windowWidth / 2;
    }

    void GetProjectionRaw(vr::EVREye, float* left, float* right, float* top, float* bottom) override {
        *left = -1.0f;
        *right = 1.0f;
        *top = -1.0f;
        *bottom = 1.0f;
    }

    vr::DistortionCoordinates_t ComputeDistortion(vr::EVREye, float u, float v) override {
        vr::DistortionCoordinates_t out{};
        out.rfRed[0] = out.rfGreen[0] = out.rfBlue[0] = u;
        out.rfRed[1] = out.rfGreen[1] = out.rfBlue[1] = v;
        return out;
    }

    void GetWindowBounds(std::int32_t* x, std::int32_t* y,
                         std::uint32_t* width, std::uint32_t* height) override {
        *x = config_.windowX;
        *y = config_.windowY;
        *width = config_.windowWidth;
        *height = config_.windowHeight;
    }

    bool ComputeInverseDistortion(vr::HmdVector2_t*, vr::EVREye, std::uint32_t, float, float) override {
        return false;
    }

private:
    DisplayConfig config_;
};

class VirtualHmd final : public vr::ITrackedDeviceServerDriver {
public:
    VirtualHmd() {
        DisplayConfig config{};
        config.windowX = ReadInt(kDisplaySection, "window_x", 0);
        config.windowY = ReadInt(kDisplaySection, "window_y", 0);
        config.windowWidth = static_cast<std::uint32_t>(std::max(2, ReadInt(kDisplaySection, "window_width", 1920)));
        config.windowHeight = static_cast<std::uint32_t>(std::max(2, ReadInt(kDisplaySection, "window_height", 1080)));
        config.renderWidth = static_cast<std::uint32_t>(std::max(2, ReadInt(kDisplaySection, "render_width", 1600)));
        config.renderHeight = static_cast<std::uint32_t>(std::max(2, ReadInt(kDisplaySection, "render_height", 1600)));
        display_ = std::make_unique<VirtualDisplay>(config);

        position_[0] = ReadFloat(kDriverSection, "hmd_x", 0.0f);
        position_[1] = ReadFloat(kDriverSection, "hmd_y", 1.65f);
        position_[2] = ReadFloat(kDriverSection, "hmd_z", 0.0f);
    }

    const char* Serial() const { return kHmdSerial; }

    vr::EVRInitError Activate(std::uint32_t objectId) override {
        index_ = objectId;
        const auto container = vr::VRProperties()->TrackedDeviceToPropertyContainer(index_);

        vr::VRProperties()->SetStringProperty(container, vr::Prop_ModelNumber_String, "DeskXR Virtual HMD");
        vr::VRProperties()->SetStringProperty(container, vr::Prop_ManufacturerName_String, "DeskXR");
        vr::VRProperties()->SetStringProperty(container, vr::Prop_SerialNumber_String, kHmdSerial);
        vr::VRProperties()->SetFloatProperty(container, vr::Prop_UserIpdMeters_Float, 0.064f);
        vr::VRProperties()->SetFloatProperty(container, vr::Prop_DisplayFrequency_Float, 90.0f);
        vr::VRProperties()->SetFloatProperty(container, vr::Prop_SecondsFromVsyncToPhotons_Float, 0.011f);
        vr::VRProperties()->SetFloatProperty(container, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.0f);
        vr::VRProperties()->SetBoolProperty(container, vr::Prop_IsOnDesktop_Bool, false);
        vr::VRProperties()->SetBoolProperty(container, vr::Prop_DisplayDebugMode_Bool, true);

        PushPose();
        Log("[DeskXR] virtual HMD activated");
        return vr::VRInitError_None;
    }

    void Deactivate() override { index_ = vr::k_unTrackedDeviceIndexInvalid; }
    void EnterStandby() override {}

    void* GetComponent(const char* nameAndVersion) override {
        if (std::strcmp(nameAndVersion, vr::IVRDisplayComponent_Version) == 0) return display_.get();
        return nullptr;
    }

    void DebugRequest(const char*, char* response, std::uint32_t responseSize) override {
        if (responseSize > 0) response[0] = '\0';
    }

    vr::DriverPose_t GetPose() override {
        vr::DriverPose_t pose{};
        pose.qWorldFromDriverRotation.w = 1.0;
        pose.qDriverFromHeadRotation.w = 1.0;
        pose.qRotation.w = 1.0;
        pose.vecPosition[0] = position_[0];
        pose.vecPosition[1] = position_[1];
        pose.vecPosition[2] = position_[2];
        pose.poseIsValid = true;
        pose.deviceIsConnected = true;
        pose.result = vr::TrackingResult_Running_OK;
        pose.shouldApplyHeadModel = false;
        return pose;
    }

    void RunFrame() { PushPose(); }

private:
    void PushPose() {
        if (index_ == vr::k_unTrackedDeviceIndexInvalid) return;
        const auto pose = GetPose();
        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(index_, pose, sizeof(pose));
    }

    std::unique_ptr<VirtualDisplay> display_;
    std::array<float, 3> position_{0.0f, 1.65f, 0.0f};
    vr::TrackedDeviceIndex_t index_ = vr::k_unTrackedDeviceIndexInvalid;
};

enum class Input : std::size_t {
    TriggerValue,
    TriggerClick,
    TriggerTouch,
    GripValue,
    GripClick,
    JoystickX,
    JoystickY,
    JoystickClick,
    JoystickTouch,
    AClick,
    ATouch,
    BClick,
    BTouch,
    XClick,
    XTouch,
    YClick,
    YTouch,
    MenuClick,
    Haptic,
    Count
};

constexpr std::size_t InputIndex(Input input) {
    return static_cast<std::size_t>(input);
}

class VirtualController final : public vr::ITrackedDeviceServerDriver {
public:
    VirtualController(
            vr::ETrackedControllerRole role,
            SharedState& state,
            UdpReceiver& transport)
        : role_(role), state_(state), transport_(transport) {}

    const char* Serial() const {
        return role_ == vr::TrackedControllerRole_LeftHand ? kLeftSerial : kRightSerial;
    }

    vr::EVRInitError Activate(std::uint32_t objectId) override {
        index_ = objectId;
        const auto container = vr::VRProperties()->TrackedDeviceToPropertyContainer(index_);

        vr::VRProperties()->SetStringProperty(container, vr::Prop_ModelNumber_String, "DeskXR Touch Bridge");
        vr::VRProperties()->SetStringProperty(container, vr::Prop_ManufacturerName_String, "DeskXR");
        vr::VRProperties()->SetStringProperty(container, vr::Prop_SerialNumber_String, Serial());
        vr::VRProperties()->SetStringProperty(container, vr::Prop_ControllerType_String, "oculus_touch");
        vr::VRProperties()->SetInt32Property(container, vr::Prop_ControllerRoleHint_Int32, role_);
        vr::VRProperties()->SetStringProperty(
            container, vr::Prop_InputProfilePath_String,
            "{deskxr}/input/deskxr_controller_profile.json");

        MakeBoolean(container, "/input/trigger/click", Input::TriggerClick);
        MakeBoolean(container, "/input/trigger/touch", Input::TriggerTouch);
        MakeScalar(container, "/input/trigger/value", Input::TriggerValue, vr::VRScalarUnits_NormalizedOneSided);

        MakeBoolean(container, "/input/grip/click", Input::GripClick);
        MakeScalar(container, "/input/grip/value", Input::GripValue, vr::VRScalarUnits_NormalizedOneSided);

        MakeScalar(container, "/input/joystick/x", Input::JoystickX, vr::VRScalarUnits_NormalizedTwoSided);
        MakeScalar(container, "/input/joystick/y", Input::JoystickY, vr::VRScalarUnits_NormalizedTwoSided);
        MakeBoolean(container, "/input/joystick/click", Input::JoystickClick);
        MakeBoolean(container, "/input/joystick/touch", Input::JoystickTouch);

        MakeBoolean(container, "/input/a/click", Input::AClick);
        MakeBoolean(container, "/input/a/touch", Input::ATouch);
        MakeBoolean(container, "/input/b/click", Input::BClick);
        MakeBoolean(container, "/input/b/touch", Input::BTouch);
        MakeBoolean(container, "/input/x/click", Input::XClick);
        MakeBoolean(container, "/input/x/touch", Input::XTouch);
        MakeBoolean(container, "/input/y/click", Input::YClick);
        MakeBoolean(container, "/input/y/touch", Input::YTouch);
        MakeBoolean(container, "/input/menu/click", Input::MenuClick);

        vr::VRDriverInput()->CreateHapticComponent(
            container, "/output/haptic", &inputs_[InputIndex(Input::Haptic)]);

        Log("[DeskXR] %s controller activated",
            role_ == vr::TrackedControllerRole_LeftHand ? "left" : "right");
        return vr::VRInitError_None;
    }

    void Deactivate() override { index_ = vr::k_unTrackedDeviceIndexInvalid; }
    void EnterStandby() override {}
    void* GetComponent(const char*) override { return nullptr; }

    void DebugRequest(const char*, char* response, std::uint32_t responseSize) override {
        if (responseSize > 0) response[0] = '\0';
    }

    vr::DriverPose_t GetPose() override {
        vr::DriverPose_t pose{};
        pose.qWorldFromDriverRotation.w = 1.0;
        pose.qDriverFromHeadRotation.w = 1.0;
        pose.deviceIsConnected = true;

        const Snapshot snapshot = state_.Load();
        if (!snapshot.fresh) {
            pose.qRotation.w = 1.0;
            pose.poseIsValid = false;
            pose.result = vr::TrackingResult_Running_OutOfRange;
            return pose;
        }

        const auto& hand = role_ == vr::TrackedControllerRole_LeftHand
            ? snapshot.packet.left : snapshot.packet.right;

        const bool valid = (hand.flags & protocol::kPoseValid) != 0;
        pose.poseIsValid = valid;
        pose.result = valid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
        pose.vecPosition[0] = hand.position[0];
        pose.vecPosition[1] = hand.position[1];
        pose.vecPosition[2] = hand.position[2];
        pose.qRotation.x = hand.orientation[0];
        pose.qRotation.y = hand.orientation[1];
        pose.qRotation.z = hand.orientation[2];
        pose.qRotation.w = hand.orientation[3];
        return pose;
    }

    void RunFrame() {
        if (index_ == vr::k_unTrackedDeviceIndexInvalid) return;

        const auto pose = GetPose();
        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(index_, pose, sizeof(pose));

        const Snapshot snapshot = state_.Load();
        if (!snapshot.fresh) {
            ResetInputs();
            return;
        }

        const auto& hand = role_ == vr::TrackedControllerRole_LeftHand
            ? snapshot.packet.left : snapshot.packet.right;
        const auto button = [&](std::uint32_t mask) { return (hand.buttons & mask) != 0; };

        UpdateScalar(Input::TriggerValue, std::clamp(hand.trigger, 0.0f, 1.0f));
        UpdateBoolean(Input::TriggerClick, hand.trigger >= 0.75f);
        UpdateBoolean(Input::TriggerTouch, hand.trigger >= 0.02f);

        UpdateScalar(Input::GripValue, std::clamp(hand.grip, 0.0f, 1.0f));
        UpdateBoolean(Input::GripClick, hand.grip >= 0.75f);

        UpdateScalar(Input::JoystickX, std::clamp(hand.joystick[0], -1.0f, 1.0f));
        UpdateScalar(Input::JoystickY, std::clamp(hand.joystick[1], -1.0f, 1.0f));
        UpdateBoolean(Input::JoystickClick, button(protocol::ButtonThumbstick));
        UpdateBoolean(Input::JoystickTouch,
                      std::fabs(hand.joystick[0]) > 0.02f || std::fabs(hand.joystick[1]) > 0.02f);

        UpdateBoolean(Input::AClick, button(protocol::ButtonA));
        UpdateBoolean(Input::ATouch, button(protocol::ButtonA));
        UpdateBoolean(Input::BClick, button(protocol::ButtonB));
        UpdateBoolean(Input::BTouch, button(protocol::ButtonB));
        UpdateBoolean(Input::XClick, button(protocol::ButtonX));
        UpdateBoolean(Input::XTouch, button(protocol::ButtonX));
        UpdateBoolean(Input::YClick, button(protocol::ButtonY));
        UpdateBoolean(Input::YTouch, button(protocol::ButtonY));
        UpdateBoolean(Input::MenuClick, button(protocol::ButtonMenu));
    }

    void ProcessEvent(const vr::VREvent_t& event) {
        if (event.eventType != vr::VREvent_Input_HapticVibration) return;
        if (event.data.hapticVibration.componentHandle != inputs_[InputIndex(Input::Haptic)]) return;

        const auto hand = role_ == vr::TrackedControllerRole_LeftHand
            ? protocol::Hand::Left
            : protocol::Hand::Right;

        const bool forwarded = transport_.SendHaptic(
            hand,
            event.data.hapticVibration.fDurationSeconds,
            event.data.hapticVibration.fFrequency,
            event.data.hapticVibration.fAmplitude);

        Log("[DeskXR] haptic %s duration=%.3f freq=%.1f amp=%.2f %s",
            role_ == vr::TrackedControllerRole_LeftHand ? "left" : "right",
            event.data.hapticVibration.fDurationSeconds,
            event.data.hapticVibration.fFrequency,
            event.data.hapticVibration.fAmplitude,
            forwarded ? "forwarded" : "not-forwarded (no Quest peer yet)");
    }

private:
    void MakeBoolean(vr::PropertyContainerHandle_t container, const char* path, Input input) {
        vr::VRDriverInput()->CreateBooleanComponent(container, path, &inputs_[InputIndex(input)]);
    }

    void MakeScalar(vr::PropertyContainerHandle_t container, const char* path, Input input,
                    vr::EVRScalarUnits units) {
        vr::VRDriverInput()->CreateScalarComponent(
            container, path, &inputs_[InputIndex(input)],
            vr::VRScalarType_Absolute, units);
    }

    void UpdateBoolean(Input input, bool value) {
        vr::VRDriverInput()->UpdateBooleanComponent(inputs_[InputIndex(input)], value, 0.0);
    }

    void UpdateScalar(Input input, float value) {
        vr::VRDriverInput()->UpdateScalarComponent(inputs_[InputIndex(input)], value, 0.0);
    }

    void ResetInputs() {
        UpdateScalar(Input::TriggerValue, 0.0f);
        UpdateBoolean(Input::TriggerClick, false);
        UpdateBoolean(Input::TriggerTouch, false);
        UpdateScalar(Input::GripValue, 0.0f);
        UpdateBoolean(Input::GripClick, false);
        UpdateScalar(Input::JoystickX, 0.0f);
        UpdateScalar(Input::JoystickY, 0.0f);
        UpdateBoolean(Input::JoystickClick, false);
        UpdateBoolean(Input::JoystickTouch, false);
        UpdateBoolean(Input::AClick, false);
        UpdateBoolean(Input::ATouch, false);
        UpdateBoolean(Input::BClick, false);
        UpdateBoolean(Input::BTouch, false);
        UpdateBoolean(Input::XClick, false);
        UpdateBoolean(Input::XTouch, false);
        UpdateBoolean(Input::YClick, false);
        UpdateBoolean(Input::YTouch, false);
        UpdateBoolean(Input::MenuClick, false);
    }

    vr::ETrackedControllerRole role_;
    SharedState& state_;
    UdpReceiver& transport_;
    vr::TrackedDeviceIndex_t index_ = vr::k_unTrackedDeviceIndexInvalid;
    std::array<vr::VRInputComponentHandle_t, InputIndex(Input::Count)> inputs_{};
};

class Provider final : public vr::IServerTrackedDeviceProvider {
public:
    vr::EVRInitError Init(vr::IVRDriverContext* context) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(context);

        const auto portSetting = ReadInt(kDriverSection, "udp_port", kDefaultPort);
        const auto port = static_cast<std::uint16_t>(std::clamp(portSetting, 1, 65535));

        receiver_ = std::make_unique<UdpReceiver>(state_);
        if (!receiver_->Start(port)) return vr::VRInitError_Driver_Failed;

        hmd_ = std::make_unique<VirtualHmd>();
        left_ = std::make_unique<VirtualController>(
            vr::TrackedControllerRole_LeftHand, state_, *receiver_);
        right_ = std::make_unique<VirtualController>(
            vr::TrackedControllerRole_RightHand, state_, *receiver_);

        if (!vr::VRServerDriverHost()->TrackedDeviceAdded(
                hmd_->Serial(), vr::TrackedDeviceClass_HMD, hmd_.get())) {
            return vr::VRInitError_Driver_Unknown;
        }

        if (!vr::VRServerDriverHost()->TrackedDeviceAdded(
                left_->Serial(), vr::TrackedDeviceClass_Controller, left_.get())) {
            return vr::VRInitError_Driver_Unknown;
        }

        if (!vr::VRServerDriverHost()->TrackedDeviceAdded(
                right_->Serial(), vr::TrackedDeviceClass_Controller, right_.get())) {
            return vr::VRInitError_Driver_Unknown;
        }

        Log("[DeskXR] provider initialized");
        return vr::VRInitError_None;
    }

    void Cleanup() override {
        right_.reset();
        left_.reset();
        hmd_.reset();

        if (receiver_) {
            receiver_->Stop();
            receiver_.reset();
        }

        VR_CLEANUP_SERVER_DRIVER_CONTEXT();
    }

    const char* const* GetInterfaceVersions() override {
        return vr::k_InterfaceVersions;
    }

    void RunFrame() override {
        if (hmd_) hmd_->RunFrame();
        if (left_) left_->RunFrame();
        if (right_) right_->RunFrame();

        vr::VREvent_t event{};
        while (vr::VRServerDriverHost()->PollNextEvent(&event, sizeof(event))) {
            if (left_) left_->ProcessEvent(event);
            if (right_) right_->ProcessEvent(event);
        }
    }

    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}

private:
    SharedState state_;
    std::unique_ptr<UdpReceiver> receiver_;
    std::unique_ptr<VirtualHmd> hmd_;
    std::unique_ptr<VirtualController> left_;
    std::unique_ptr<VirtualController> right_;
};

Provider g_provider;

} // namespace deskxr

#define DESKXR_EXPORT extern "C" __declspec(dllexport)

DESKXR_EXPORT void* HmdDriverFactory(const char* interfaceName, int* returnCode) {
    if (std::strcmp(vr::IServerTrackedDeviceProvider_Version, interfaceName) == 0) {
        return &deskxr::g_provider;
    }

    if (returnCode) *returnCode = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
