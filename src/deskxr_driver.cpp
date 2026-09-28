#include "protocol.hpp"

#include <openvr_driver.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

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
constexpr std::uint64_t kDeskXrUniverseId = 0x44585201ULL; // stable, nonzero, not Oculus-reserved ID 1

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

bool ReadBool(const char* section, const char* key, bool fallback) {
    vr::EVRSettingsError err = vr::VRSettingsError_None;
    const bool value = vr::VRSettings()->GetBool(section, key, &err);
    return err == vr::VRSettingsError_None ? value : fallback;
}

bool ValidateAndNormalizeHand(protocol::HandV1& hand) {
    const auto finite = [](float value) { return std::isfinite(value); };

    if (!finite(hand.trigger) || !finite(hand.grip) ||
        !finite(hand.joystick[0]) || !finite(hand.joystick[1])) {
        return false;
    }

    if ((hand.flags & protocol::kPoseValid) == 0) {
        return true;
    }

    for (float value : hand.position) {
        if (!finite(value) || std::fabs(value) > 100.0f) {
            return false;
        }
    }

    for (float value : hand.orientation) {
        if (!finite(value)) {
            return false;
        }
    }

    const float normSquared =
        hand.orientation[0] * hand.orientation[0] +
        hand.orientation[1] * hand.orientation[1] +
        hand.orientation[2] * hand.orientation[2] +
        hand.orientation[3] * hand.orientation[3];

    if (!finite(normSquared) || normSquared < 0.01f || normSquared > 100.0f) {
        return false;
    }

    const float inverseNorm = 1.0f / std::sqrt(normSquared);
    for (float& value : hand.orientation) {
        value *= inverseNorm;
    }

    return true;
}

bool ValidateAndNormalizePacket(protocol::PacketV1& packet) {
    return ValidateAndNormalizeHand(packet.left) &&
           ValidateAndNormalizeHand(packet.right);
}

struct HandKinematics {
    std::array<double, 3> linear{};
    std::array<double, 3> angular{};
};

struct Snapshot {
    protocol::PacketV1 packet{};
    std::array<HandKinematics, 2> kinematics{};
    bool fresh = false;
};

class SharedState {
public:
    void Store(const protocol::PacketV1& packet) {
        const auto now = std::chrono::steady_clock::now();

        std::scoped_lock lock(mutex_);

        if (hasPacket_) {
            const double dt = std::chrono::duration<double>(now - receivedAt_).count();

            // At normal Quest rates this is roughly 11-14 ms. Ignore very
            // small/large intervals so a restart or hitch cannot create a
            // huge synthetic velocity that SteamVR would then predict from.
            if (dt >= 0.002 && dt <= 0.100) {
                UpdateKinematics(packet_.left, packet.left, dt, kinematics_[0]);
                UpdateKinematics(packet_.right, packet.right, dt, kinematics_[1]);
            } else {
                kinematics_ = {};
            }
        } else {
            kinematics_ = {};
        }

        packet_ = packet;
        receivedAt_ = now;
        hasPacket_ = true;
    }

    Snapshot Load() const {
        std::scoped_lock lock(mutex_);
        Snapshot out{};
        if (!hasPacket_) return out;
        out.packet = packet_;
        out.kinematics = kinematics_;
        out.fresh = (std::chrono::steady_clock::now() - receivedAt_) < 1000ms;
        return out;
    }

private:
    static double ClampMagnitudeComponent(double value, double limit) {
        return std::clamp(value, -limit, limit);
    }

    static void UpdateKinematics(
            const protocol::HandV1& previous,
            const protocol::HandV1& current,
            double dt,
            HandKinematics& state) {
        const bool previousValid = (previous.flags & protocol::kPoseValid) != 0;
        const bool currentValid = (current.flags & protocol::kPoseValid) != 0;

        if (!previousValid || !currentValid) {
            state = {};
            return;
        }

        HandKinematics measured{};

        for (std::size_t axis = 0; axis < 3; ++axis) {
            measured.linear[axis] = ClampMagnitudeComponent(
                (static_cast<double>(current.position[axis]) -
                 static_cast<double>(previous.position[axis])) / dt,
                15.0);
        }

        // q_delta = q_current * inverse(q_previous). This produces an
        // angular velocity expressed in the same tracking-space axes as the
        // position data, which is what OpenVR DriverPose_t expects.
        const double px = previous.orientation[0];
        const double py = previous.orientation[1];
        const double pz = previous.orientation[2];
        const double pw = previous.orientation[3];

        const double cx = current.orientation[0];
        const double cy = current.orientation[1];
        const double cz = current.orientation[2];
        const double cw = current.orientation[3];

        double dx = -cw * px + cx * pw - cy * pz + cz * py;
        double dy = -cw * py + cx * pz + cy * pw - cz * px;
        double dz = -cw * pz - cx * py + cy * px + cz * pw;
        double dw =  cw * pw + cx * px + cy * py + cz * pz;

        // Quaternions q and -q represent the same orientation. Select the
        // shortest arc so sign flips do not become 360-degree spikes.
        if (dw < 0.0) {
            dx = -dx;
            dy = -dy;
            dz = -dz;
            dw = -dw;
        }

        dw = std::clamp(dw, -1.0, 1.0);
        const double sinHalf = std::sqrt(dx * dx + dy * dy + dz * dz);

        if (sinHalf > 1e-6) {
            const double angle = 2.0 * std::atan2(sinHalf, dw);
            const double scale = angle / (sinHalf * dt);
            measured.angular[0] = ClampMagnitudeComponent(dx * scale, 40.0);
            measured.angular[1] = ClampMagnitudeComponent(dy * scale, 40.0);
            measured.angular[2] = ClampMagnitudeComponent(dz * scale, 40.0);
        }

        // A small low-pass filter reduces Wi-Fi/USB timestamp jitter while
        // still reacting quickly enough for hand motion.
        constexpr double kAlpha = 0.35;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            state.linear[axis] =
                state.linear[axis] * (1.0 - kAlpha) +
                measured.linear[axis] * kAlpha;
            state.angular[axis] =
                state.angular[axis] * (1.0 - kAlpha) +
                measured.angular[axis] * kAlpha;
        }
    }

    mutable std::mutex mutex_;
    protocol::PacketV1 packet_{};
    std::array<HandKinematics, 2> kinematics_{};
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
            hasSequence_ = false;
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
            if ((std::chrono::steady_clock::now() - lastPeerAccepted_) > 1500ms) {
                return false;
            }
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
            if (!ValidateAndNormalizePacket(packet)) continue;

            const auto now = std::chrono::steady_clock::now();
            {
                std::scoped_lock lock(peerMutex_);

                const bool peerFresh =
                    hasPeer_ && (now - lastPeerAccepted_) < 1500ms;
                const bool samePeer =
                    hasPeer_ &&
                    peer_.sin_addr.s_addr == from.sin_addr.s_addr &&
                    peer_.sin_port == from.sin_port;

                // While a Quest peer is actively streaming, ignore another
                // sender trying to take over the controller stream.
                if (peerFresh && !samePeer) {
                    continue;
                }

                if (!peerFresh || !samePeer) {
                    hasSequence_ = false;
                }

                if (hasSequence_) {
                    const auto delta =
                        static_cast<std::int32_t>(packet.sequence - lastSequence_);

                    // A fresh sender can restart its sequence counter at zero.
                    const bool explicitRestart =
                        packet.sequence == 0u && lastSequence_ > 32u;

                    if (delta <= 0 && !explicitRestart) {
                        continue;
                    }
                }

                peer_ = from;
                hasPeer_ = true;
                lastPeerAccepted_ = now;
                lastSequence_ = packet.sequence;
                hasSequence_ = true;
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
    std::chrono::steady_clock::time_point lastPeerAccepted_{};
    std::uint32_t lastSequence_ = 0;
    bool hasSequence_ = false;
    std::atomic_uint32_t hapticSequence_{0};
};

struct DisplayConfig {
    std::int32_t windowX = 0;
    std::int32_t windowY = 0;
    std::uint32_t windowWidth = 1920;
    std::uint32_t windowHeight = 1080;
    std::uint32_t renderWidth = 1600;
    std::uint32_t renderHeight = 1600;
    bool desktopMono = true;
};

class VirtualDisplay final : public vr::IVRDisplayComponent {
public:
    explicit VirtualDisplay(DisplayConfig config) : config_(config) {}

    bool IsDisplayOnDesktop() override { return true; }
    bool IsDisplayRealDisplay() override { return false; }

    void GetRecommendedRenderTargetSize(std::uint32_t* width, std::uint32_t* height) override {
        // DeskXR does not use SteamVR's Headset Window as the user's monitor
        // output. VRChat's own companion window is the intended desktop view.
        // Keep a conventional per-eye render target here so application
        // projection stays HMD-like and is not warped to the monitor aspect.
        *width = config_.renderWidth;
        *height = config_.renderHeight;
    }

    void GetEyeOutputViewport(vr::EVREye eye, std::uint32_t* x, std::uint32_t* y,
                              std::uint32_t* width, std::uint32_t* height) override {
        if (config_.desktopMono) {
            // Mono mode: both eyes resolve to the exact same viewport. Keep
            // the eye render-target aspect ratio and letterbox/pillarbox it
            // instead of stretching it to the desktop window.
            const double renderAspect =
                static_cast<double>(std::max<std::uint32_t>(1, config_.renderWidth)) /
                static_cast<double>(std::max<std::uint32_t>(1, config_.renderHeight));
            const double windowAspect =
                static_cast<double>(std::max<std::uint32_t>(1, config_.windowWidth)) /
                static_cast<double>(std::max<std::uint32_t>(1, config_.windowHeight));

            if (windowAspect > renderAspect) {
                *height = config_.windowHeight;
                *width = static_cast<std::uint32_t>(
                    std::max(1.0, static_cast<double>(*height) * renderAspect));
                *x = (config_.windowWidth - *width) / 2;
                *y = 0;
            } else {
                *width = config_.windowWidth;
                *height = static_cast<std::uint32_t>(
                    std::max(1.0, static_cast<double>(*width) / renderAspect));
                *x = 0;
                *y = (config_.windowHeight - *height) / 2;
            }
            return;
        }

        // Optional stereo debug output.
        *y = 0;
        *height = config_.windowHeight;
        *width = std::max<std::uint32_t>(1, config_.windowWidth / 2);
        *x = eye == vr::Eye_Left ? 0 : *width;
    }

    void GetProjectionRaw(vr::EVREye, float* left, float* right, float* top, float* bottom) override {
        // Match the camera frustum to the configured render-target aspect.
        // DeskXR mono commonly uses 16:9 or 16:10 instead of a square HMD
        // texture. A square 90x90-degree frustum on a widescreen target makes
        // the companion view look stretched.
        const float aspect =
            static_cast<float>(std::max<std::uint32_t>(1, config_.renderWidth)) /
            static_cast<float>(std::max<std::uint32_t>(1, config_.renderHeight));

        constexpr float verticalTan = 1.0f; // 90-degree vertical FOV
        const float horizontalTan = verticalTan * aspect;

        *left = -horizontalTan;
        *right = horizontalTan;
        *top = -verticalTan;
        *bottom = verticalTan;
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
        config.desktopMono = ReadBool(kDisplaySection, "desktop_mono", true);
        desktopMono_ = config.desktopMono;
        display_ = std::make_unique<VirtualDisplay>(config);

        Log("[DeskXR] virtual HMD mode=%s display=%ux%u render=%ux%u; intended desktop view is the VRChat companion window",
            config.desktopMono ? "mono" : "stereo",
            config.windowWidth,
            config.windowHeight,
            config.renderWidth,
            config.renderHeight);

        position_[0] = ReadFloat(kDriverSection, "hmd_x", 0.0f);
        position_[1] = ReadFloat(kDriverSection, "hmd_y", 1.65f);
        position_[2] = ReadFloat(kDriverSection, "hmd_z", 0.0f);

        const float sensitivityDegrees =
            ReadFloat(kDriverSection, "mouse_sensitivity_deg_per_pixel", 0.08f);
        mouseSensitivityRadPerPixel_ =
            sensitivityDegrees * 3.14159265358979323846f / 180.0f;

        const float pitchLimitDegrees =
            ReadFloat(kDriverSection, "mouse_pitch_limit_deg", 80.0f);
        mousePitchLimitRad_ =
            std::clamp(pitchLimitDegrees, 1.0f, 89.0f) *
            3.14159265358979323846f / 180.0f;

        displayFrequencyHz_ = std::clamp(
            ReadFloat(kDriverSection, "display_frequency_hz", 72.0f),
            30.0f,
            144.0f);
    }

    const char* Serial() const { return kHmdSerial; }

    vr::EVRInitError Activate(std::uint32_t objectId) override {
        index_ = objectId;
        const auto container = vr::VRProperties()->TrackedDeviceToPropertyContainer(index_);

        vr::VRProperties()->SetStringProperty(container, vr::Prop_ModelNumber_String, "DeskXR Virtual HMD");
        vr::VRProperties()->SetStringProperty(container, vr::Prop_ManufacturerName_String, "DeskXR");
        vr::VRProperties()->SetStringProperty(container, vr::Prop_SerialNumber_String, kHmdSerial);
        vr::VRProperties()->SetStringProperty(
            container,
            vr::Prop_TrackingSystemName_String,
            "deskxr");
        vr::VRProperties()->SetStringProperty(
            container,
            vr::Prop_ActualTrackingSystemName_String,
            "deskxr");
        vr::VRProperties()->SetUint64Property(
            container,
            vr::Prop_CurrentUniverseId_Uint64,
            kDeskXrUniverseId);
        // Mono mode collapses both virtual eyes to the same camera pose.
        // OpenVR applications still submit Eye_Left/Eye_Right, but there is
        // no stereo parallax when DeskXR mono mode is enabled.
        vr::VRProperties()->SetFloatProperty(
            container,
            vr::Prop_UserIpdMeters_Float,
            desktopMono_ ? 0.0f : 0.064f);
        vr::VRProperties()->SetFloatProperty(
            container,
            vr::Prop_DisplayFrequency_Float,
            displayFrequencyHz_);
        vr::VRProperties()->SetFloatProperty(
            container,
            vr::Prop_SecondsFromVsyncToPhotons_Float,
            1.0f / displayFrequencyHz_);
        vr::VRProperties()->SetFloatProperty(container, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.0f);
        vr::VRProperties()->SetBoolProperty(container, vr::Prop_IsOnDesktop_Bool, false);
        vr::VRProperties()->SetBoolProperty(container, vr::Prop_DisplayDebugMode_Bool, true);

        // DeskXR's HMD is intentionally stationary, so SteamVR cannot use
        // head motion as an "in use" signal. Expose a virtual proximity
        // sensor that is always worn instead. OpenVR explicitly uses the
        // /proximity boolean for HMD standby state.
        vr::VRProperties()->SetBoolProperty(
            container,
            vr::Prop_ContainsProximitySensor_Bool,
            true);
        vr::VRProperties()->SetBoolProperty(
            container,
            vr::Prop_DeviceCanPowerOff_Bool,
            false);

        const vr::EVRInputError proximityError =
            vr::VRDriverInput()->CreateBooleanComponent(
                container,
                "/proximity",
                &proximityHandle_);

        if (proximityError == vr::VRInputError_None) {
            UpdateProximity();
        } else {
            Log("[DeskXR] failed to create HMD /proximity input: %d",
                static_cast<int>(proximityError));
        }

        PushPose();
        Log("[DeskXR] virtual HMD activated (always-worn proximity enabled, universe=%llu)",
            static_cast<unsigned long long>(kDeskXrUniverseId));
        return vr::VRInitError_None;
    }

    void Deactivate() override {
        index_ = vr::k_unTrackedDeviceIndexInvalid;
        proximityHandle_ = vr::k_ulInvalidInputComponentHandle;
    }

    void EnterStandby() override {
        // SteamVR may still call this during a power-state transition. DeskXR
        // has no physical display to power down, so immediately reaffirm the
        // virtual "headset worn" state.
        UpdateProximity();
        PushPose();
        Log("[DeskXR] standby requested; keeping virtual HMD awake");
    }

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
        const float halfYaw = yawRad_ * 0.5f;
        const float halfPitch = pitchRad_ * 0.5f;
        const float sy = std::sin(halfYaw);
        const float cy = std::cos(halfYaw);
        const float sp = std::sin(halfPitch);
        const float cp = std::cos(halfPitch);

        pose.qRotation.x = cy * sp;
        pose.qRotation.y = sy * cp;
        pose.qRotation.z = -sy * sp;
        pose.qRotation.w = cy * cp;

        pose.vecPosition[0] = position_[0];
        pose.vecPosition[1] = position_[1];
        pose.vecPosition[2] = position_[2];
        pose.poseIsValid = true;
        pose.deviceIsConnected = true;
        pose.result = vr::TrackingResult_Running_OK;
        pose.shouldApplyHeadModel = false;
        return pose;
    }

    void RunFrame() {
        UpdateMouseLook();
        UpdateProximity();
        PushPose();
    }

private:
    void UpdateProximity() {
        if (proximityHandle_ == vr::k_ulInvalidInputComponentHandle) {
            return;
        }

        const vr::EVRInputError error =
            vr::VRDriverInput()->UpdateBooleanComponent(
                proximityHandle_,
                true,
                0.0);

        if (error != vr::VRInputError_None &&
            error != vr::VRInputError_InvalidHandle) {
            Log("[DeskXR] /proximity update failed: %d",
                static_cast<int>(error));
        }
    }

    void UpdateMouseLook() {
        if ((GetAsyncKeyState(VK_F8) & 1) != 0) {
            mouseLookActive_ = !mouseLookActive_;
            Log("[DeskXR] mouse look %s (F8 toggles, F9 resets)",
                mouseLookActive_ ? "enabled" : "disabled");

            if (mouseLookActive_) {
                const int centerX = GetSystemMetrics(SM_CXSCREEN) / 2;
                const int centerY = GetSystemMetrics(SM_CYSCREEN) / 2;
                SetCursorPos(centerX, centerY);
            }
        }

        if ((GetAsyncKeyState(VK_F9) & 1) != 0) {
            yawRad_ = 0.0f;
            pitchRad_ = 0.0f;
            Log("[DeskXR] mouse-look orientation reset");
        }

        if (!mouseLookActive_) {
            return;
        }

        const int centerX = GetSystemMetrics(SM_CXSCREEN) / 2;
        const int centerY = GetSystemMetrics(SM_CYSCREEN) / 2;

        POINT point{};
        if (!GetCursorPos(&point)) {
            return;
        }

        const int deltaX = point.x - centerX;
        const int deltaY = point.y - centerY;

        if (deltaX != 0 || deltaY != 0) {
            yawRad_ -= static_cast<float>(deltaX) * mouseSensitivityRadPerPixel_;
            pitchRad_ -= static_cast<float>(deltaY) * mouseSensitivityRadPerPixel_;
            pitchRad_ = std::clamp(pitchRad_, -mousePitchLimitRad_, mousePitchLimitRad_);
        }

        SetCursorPos(centerX, centerY);
    }

    void PushPose() {
        if (index_ == vr::k_unTrackedDeviceIndexInvalid) return;
        const auto pose = GetPose();
        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(index_, pose, sizeof(pose));
    }

    std::unique_ptr<VirtualDisplay> display_;
    std::array<float, 3> position_{0.0f, 1.65f, 0.0f};
    float yawRad_ = 0.0f;
    float pitchRad_ = 0.0f;
    float mouseSensitivityRadPerPixel_ = 0.0013962634f;
    float mousePitchLimitRad_ = 1.3962634f;
    float displayFrequencyHz_ = 72.0f;
    bool mouseLookActive_ = false;
    bool desktopMono_ = true;
    vr::VRInputComponentHandle_t proximityHandle_ = vr::k_ulInvalidInputComponentHandle;
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
    ThumbrestTouch,
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

        // SteamVR does not infer a good visual render model from
        // Prop_ControllerType_String alone. Without this property it falls
        // back to the ugly grey generic controller model seen in vrmonitor.
        //
        // SteamVR currently ships built-in Quest 2 Touch render models for
        // the oculus_touch emulation profile. Touch Plus has a different
        // physical shell, but these are a much closer temporary visual
        // representation than the generic fallback and require no bundled
        // copyrighted model assets.
        vr::VRProperties()->SetStringProperty(
            container,
            vr::Prop_RenderModelName_String,
            role_ == vr::TrackedControllerRole_LeftHand
                ? "oculus_quest2_controller_left"
                : "oculus_quest2_controller_right");
        // Keep the controller type as oculus_touch so OVR Advanced Settings
        // and SteamVR can reuse Touch bindings, but report the actual tracking
        // system as DeskXR. This avoids third-party tools mistaking the whole
        // device stack for the native Oculus runtime.
        vr::VRProperties()->SetStringProperty(
            container,
            vr::Prop_TrackingSystemName_String,
            "deskxr");
        vr::VRProperties()->SetStringProperty(
            container,
            vr::Prop_ActualTrackingSystemName_String,
            "deskxr");
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
        MakeBoolean(container, "/input/thumbrest/touch", Input::ThumbrestTouch);

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

        const std::size_t handIndex =
            role_ == vr::TrackedControllerRole_LeftHand ? 0u : 1u;
        const auto& kinematics = snapshot.kinematics[handIndex];

        pose.vecVelocity[0] = kinematics.linear[0];
        pose.vecVelocity[1] = kinematics.linear[1];
        pose.vecVelocity[2] = kinematics.linear[2];

        pose.vecAngularVelocity[0] = kinematics.angular[0];
        pose.vecAngularVelocity[1] = kinematics.angular[1];
        pose.vecAngularVelocity[2] = kinematics.angular[2];

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
        UpdateBoolean(Input::TriggerTouch,
                      button(protocol::TouchTrigger) || hand.trigger >= 0.02f);

        UpdateScalar(Input::GripValue, std::clamp(hand.grip, 0.0f, 1.0f));
        UpdateBoolean(Input::GripClick, hand.grip >= 0.75f);

        UpdateScalar(Input::JoystickX, std::clamp(hand.joystick[0], -1.0f, 1.0f));
        UpdateScalar(Input::JoystickY, std::clamp(hand.joystick[1], -1.0f, 1.0f));
        UpdateBoolean(Input::JoystickClick, button(protocol::ButtonThumbstick));
        UpdateBoolean(Input::JoystickTouch,
                      button(protocol::TouchThumbstick) ||
                      std::fabs(hand.joystick[0]) > 0.02f ||
                      std::fabs(hand.joystick[1]) > 0.02f);
        UpdateBoolean(Input::ThumbrestTouch, button(protocol::TouchThumbrest));

        UpdateBoolean(Input::AClick, button(protocol::ButtonA));
        UpdateBoolean(Input::ATouch, button(protocol::TouchA) || button(protocol::ButtonA));
        UpdateBoolean(Input::BClick, button(protocol::ButtonB));
        UpdateBoolean(Input::BTouch, button(protocol::TouchB) || button(protocol::ButtonB));
        UpdateBoolean(Input::XClick, button(protocol::ButtonX));
        UpdateBoolean(Input::XTouch, button(protocol::TouchX) || button(protocol::ButtonX));
        UpdateBoolean(Input::YClick, button(protocol::ButtonY));
        UpdateBoolean(Input::YTouch, button(protocol::TouchY) || button(protocol::ButtonY));
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
        UpdateBoolean(Input::ThumbrestTouch, false);
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
