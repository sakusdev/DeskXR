#include <jni.h>
#include <android/log.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr const char* kTag = "DeskXR";
constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::uint16_t kPoseValid = 1u << 0;
constexpr float kVirtualHeadHeight = 1.65f;

enum Button : std::uint32_t {
    ButtonA = 1u << 0,
    ButtonB = 1u << 1,
    ButtonX = 1u << 2,
    ButtonY = 1u << 3,
    ButtonThumbstick = 1u << 4,
    ButtonMenu = 1u << 5,
};

#pragma pack(push, 1)
struct HandV1 {
    float position[3];
    float orientation[4];
    float trigger;
    float grip;
    float joystick[2];
    std::uint32_t buttons;
    std::uint32_t flags;
};

struct PacketV1 {
    char magic[4];
    std::uint16_t version;
    std::uint16_t size;
    std::uint32_t sequence;
    HandV1 left;
    HandV1 right;
};
#pragma pack(pop)

static_assert(sizeof(HandV1) == 52);
static_assert(sizeof(PacketV1) == 116);

void LogI(const char* text) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "%s", text);
}

void LogE(const char* text) {
    __android_log_print(ANDROID_LOG_ERROR, kTag, "%s", text);
}

const char* SessionStateName(XrSessionState state) {
    switch (state) {
        case XR_SESSION_STATE_UNKNOWN: return "unknown";
        case XR_SESSION_STATE_IDLE: return "idle";
        case XR_SESSION_STATE_READY: return "ready";
        case XR_SESSION_STATE_SYNCHRONIZED: return "synchronized";
        case XR_SESSION_STATE_VISIBLE: return "visible";
        case XR_SESSION_STATE_FOCUSED: return "focused";
        case XR_SESSION_STATE_STOPPING: return "stopping";
        case XR_SESSION_STATE_LOSS_PENDING: return "loss-pending";
        case XR_SESSION_STATE_EXITING: return "exiting";
        default: return "other";
    }
}

class Bridge {
public:
    bool Start(JNIEnv* env, jobject activity, const std::string& host, int port) {
        if (running_.load()) {
            return true;
        }

        in_addr addr{};
        if (inet_pton(AF_INET, host.c_str(), &addr) != 1) {
            SetStatus("Invalid PC IPv4 address: " + host);
            return false;
        }

        if (env->GetJavaVM(&javaVm_) != JNI_OK || javaVm_ == nullptr) {
            SetStatus("Could not get JavaVM.");
            return false;
        }

        activity_ = env->NewGlobalRef(activity);
        if (activity_ == nullptr) {
            SetStatus("Could not retain Android Activity.");
            return false;
        }

        host_ = host;
        port_ = port;
        stopRequested_ = false;
        running_ = true;
        worker_ = std::thread([this] { ThreadMain(); });
        return true;
    }

    void Stop(JNIEnv* env) {
        stopRequested_ = true;
        if (worker_.joinable()) {
            worker_.join();
        }

        if (activity_ != nullptr && env != nullptr) {
            env->DeleteGlobalRef(activity_);
            activity_ = nullptr;
        }

        running_ = false;
        if (Status().rfind("Error:", 0) != 0) {
            SetStatus("Idle");
        }
    }

    bool IsRunning() const {
        return running_.load();
    }

    std::string Status() const {
        std::scoped_lock lock(statusMutex_);
        return status_;
    }

private:
    void SetStatus(std::string value) {
        {
            std::scoped_lock lock(statusMutex_);
            status_ = std::move(value);
        }
        LogI(status_.c_str());
    }

    void Fail(const std::string& message) {
        LogE(message.c_str());
        SetStatus("Error: " + message);
        stopRequested_ = true;
    }

    bool Check(XrResult result, const char* what) {
        if (XR_SUCCEEDED(result)) {
            return true;
        }

        char buffer[XR_MAX_RESULT_STRING_SIZE] = {};
        if (instance_ != XR_NULL_HANDLE) {
            xrResultToString(instance_, result, buffer);
        } else {
            std::snprintf(buffer, sizeof(buffer), "XrResult %d", static_cast<int>(result));
        }

        Fail(std::string(what) + " failed: " + buffer);
        return false;
    }

    bool HasExtension(const char* wanted) {
        std::uint32_t count = 0;
        if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr))) {
            return false;
        }

        std::vector<XrExtensionProperties> props(count, {XR_TYPE_EXTENSION_PROPERTIES});
        if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data()))) {
            return false;
        }

        for (const auto& prop : props) {
            if (std::strcmp(prop.extensionName, wanted) == 0) {
                return true;
            }
        }
        return false;
    }

    bool InitUdp() {
        socket_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_ < 0) {
            Fail("Could not create UDP socket.");
            return false;
        }

        destination_ = {};
        destination_.sin_family = AF_INET;
        destination_.sin_port = htons(static_cast<std::uint16_t>(port_));
        if (inet_pton(AF_INET, host_.c_str(), &destination_.sin_addr) != 1) {
            Fail("PC IPv4 address became invalid.");
            return false;
        }
        return true;
    }

    bool InitLoader() {
        PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
        const XrResult getResult = xrGetInstanceProcAddr(
                XR_NULL_HANDLE,
                "xrInitializeLoaderKHR",
                reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader));

        if (XR_FAILED(getResult) || initializeLoader == nullptr) {
            Fail("OpenXR loader does not expose xrInitializeLoaderKHR.");
            return false;
        }

        XrLoaderInitInfoAndroidKHR loaderInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        loaderInfo.applicationVM = javaVm_;
        loaderInfo.applicationContext = activity_;

        return Check(
                initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInfo)),
                "xrInitializeLoaderKHR");
    }

    bool InitInstance() {
        if (!HasExtension(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME)) {
            Fail("XR_KHR_android_create_instance is unavailable.");
            return false;
        }
        if (!HasExtension(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) {
            Fail("XR_KHR_opengl_es_enable is unavailable.");
            return false;
        }

        const char* extensions[] = {
                XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
                XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
        };

        XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
        androidInfo.applicationVM = javaVm_;
        androidInfo.applicationActivity = activity_;

        XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
        createInfo.next = &androidInfo;
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(std::size(extensions));
        createInfo.enabledExtensionNames = extensions;
        std::strncpy(createInfo.applicationInfo.applicationName, "DeskXR", XR_MAX_APPLICATION_NAME_SIZE - 1);
        std::strncpy(createInfo.applicationInfo.engineName, "DeskXR Native", XR_MAX_ENGINE_NAME_SIZE - 1);
        createInfo.applicationInfo.applicationVersion = 1;
        createInfo.applicationInfo.engineVersion = 1;
        createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

        if (!Check(xrCreateInstance(&createInfo, &instance_), "xrCreateInstance")) {
            return false;
        }

        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        return Check(xrGetSystem(instance_, &systemInfo, &systemId_), "xrGetSystem");
    }

    bool InitEgl() {
        PFN_xrGetOpenGLESGraphicsRequirementsKHR getRequirements = nullptr;
        if (!Check(
                    xrGetInstanceProcAddr(
                            instance_,
                            "xrGetOpenGLESGraphicsRequirementsKHR",
                            reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)),
                    "xrGetInstanceProcAddr(xrGetOpenGLESGraphicsRequirementsKHR)") ||
            getRequirements == nullptr) {
            return false;
        }

        XrGraphicsRequirementsOpenGLESKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
        if (!Check(getRequirements(instance_, systemId_, &requirements), "xrGetOpenGLESGraphicsRequirementsKHR")) {
            return false;
        }

        eglDisplay_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (eglDisplay_ == EGL_NO_DISPLAY || !eglInitialize(eglDisplay_, nullptr, nullptr)) {
            Fail("eglInitialize failed.");
            return false;
        }

        if (!eglBindAPI(EGL_OPENGL_ES_API)) {
            Fail("eglBindAPI(EGL_OPENGL_ES_API) failed.");
            return false;
        }

        const EGLint configAttributes[] = {
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                EGL_RED_SIZE, 8,
                EGL_GREEN_SIZE, 8,
                EGL_BLUE_SIZE, 8,
                EGL_ALPHA_SIZE, 8,
                EGL_DEPTH_SIZE, 0,
                EGL_NONE
        };

        EGLint configCount = 0;
        if (!eglChooseConfig(eglDisplay_, configAttributes, &eglConfig_, 1, &configCount) ||
            configCount == 0) {
            Fail("eglChooseConfig failed.");
            return false;
        }

        const EGLint contextAttributes[] = {
                EGL_CONTEXT_CLIENT_VERSION, 3,
                EGL_NONE
        };
        eglContext_ = eglCreateContext(eglDisplay_, eglConfig_, EGL_NO_CONTEXT, contextAttributes);
        if (eglContext_ == EGL_NO_CONTEXT) {
            Fail("eglCreateContext failed.");
            return false;
        }

        const EGLint pbufferAttributes[] = {
                EGL_WIDTH, 16,
                EGL_HEIGHT, 16,
                EGL_NONE
        };
        eglSurface_ = eglCreatePbufferSurface(eglDisplay_, eglConfig_, pbufferAttributes);
        if (eglSurface_ == EGL_NO_SURFACE) {
            Fail("eglCreatePbufferSurface failed.");
            return false;
        }

        if (!eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_)) {
            Fail("eglMakeCurrent failed.");
            return false;
        }

        return true;
    }

    bool InitSession() {
        XrGraphicsBindingOpenGLESAndroidKHR graphicsBinding{
                XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
        graphicsBinding.display = eglDisplay_;
        graphicsBinding.config = eglConfig_;
        graphicsBinding.context = eglContext_;

        XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
        sessionInfo.next = &graphicsBinding;
        sessionInfo.systemId = systemId_;

        if (!Check(xrCreateSession(instance_, &sessionInfo, &session_), "xrCreateSession")) {
            return false;
        }

        XrReferenceSpaceCreateInfo viewInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        viewInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        viewInfo.poseInReferenceSpace.orientation.w = 1.0f;
        return Check(xrCreateReferenceSpace(session_, &viewInfo, &viewSpace_), "xrCreateReferenceSpace(VIEW)");
    }

    XrPath Path(const char* text) {
        XrPath path = XR_NULL_PATH;
        if (!Check(xrStringToPath(instance_, text, &path), text)) {
            return XR_NULL_PATH;
        }
        return path;
    }

    bool CreateAction(
            const char* name,
            const char* localizedName,
            XrActionType type,
            XrAction* action,
            bool perHand) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        info.actionType = type;
        std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
        std::strncpy(info.localizedActionName, localizedName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);

        if (perHand) {
            info.countSubactionPaths = static_cast<std::uint32_t>(handPaths_.size());
            info.subactionPaths = handPaths_.data();
        }

        return Check(xrCreateAction(actionSet_, &info, action), name);
    }

    bool InitActions() {
        handPaths_[0] = Path("/user/hand/left");
        handPaths_[1] = Path("/user/hand/right");
        if (handPaths_[0] == XR_NULL_PATH || handPaths_[1] == XR_NULL_PATH) {
            return false;
        }

        XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strncpy(setInfo.actionSetName, "deskxr", XR_MAX_ACTION_SET_NAME_SIZE - 1);
        std::strncpy(setInfo.localizedActionSetName, "DeskXR", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
        setInfo.priority = 0;

        if (!Check(xrCreateActionSet(instance_, &setInfo, &actionSet_), "xrCreateActionSet")) {
            return false;
        }

        if (!CreateAction("hand_pose", "Hand pose", XR_ACTION_TYPE_POSE_INPUT, &poseAction_, true) ||
            !CreateAction("trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT, &triggerAction_, true) ||
            !CreateAction("squeeze", "Grip", XR_ACTION_TYPE_FLOAT_INPUT, &squeezeAction_, true) ||
            !CreateAction("thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, &thumbstickAction_, true) ||
            !CreateAction("primary", "Primary button", XR_ACTION_TYPE_BOOLEAN_INPUT, &primaryAction_, true) ||
            !CreateAction("secondary", "Secondary button", XR_ACTION_TYPE_BOOLEAN_INPUT, &secondaryAction_, true) ||
            !CreateAction("thumb_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT, &thumbClickAction_, true) ||
            !CreateAction("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, &menuAction_, false)) {
            return false;
        }

        const XrPath profile = Path("/interaction_profiles/oculus/touch_controller");
        if (profile == XR_NULL_PATH) {
            return false;
        }

        std::vector<XrActionSuggestedBinding> bindings;
        auto bind = [&](XrAction action, const char* pathText) {
            const XrPath path = Path(pathText);
            if (path != XR_NULL_PATH) {
                bindings.push_back({action, path});
            }
        };

        bind(poseAction_, "/user/hand/left/input/grip/pose");
        bind(poseAction_, "/user/hand/right/input/grip/pose");
        bind(triggerAction_, "/user/hand/left/input/trigger/value");
        bind(triggerAction_, "/user/hand/right/input/trigger/value");
        bind(squeezeAction_, "/user/hand/left/input/squeeze/value");
        bind(squeezeAction_, "/user/hand/right/input/squeeze/value");
        bind(thumbstickAction_, "/user/hand/left/input/thumbstick");
        bind(thumbstickAction_, "/user/hand/right/input/thumbstick");
        bind(primaryAction_, "/user/hand/left/input/x/click");
        bind(primaryAction_, "/user/hand/right/input/a/click");
        bind(secondaryAction_, "/user/hand/left/input/y/click");
        bind(secondaryAction_, "/user/hand/right/input/b/click");
        bind(thumbClickAction_, "/user/hand/left/input/thumbstick/click");
        bind(thumbClickAction_, "/user/hand/right/input/thumbstick/click");
        bind(menuAction_, "/user/hand/left/input/menu/click");

        XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggested.interactionProfile = profile;
        suggested.countSuggestedBindings = static_cast<std::uint32_t>(bindings.size());
        suggested.suggestedBindings = bindings.data();

        if (!Check(xrSuggestInteractionProfileBindings(instance_, &suggested),
                   "xrSuggestInteractionProfileBindings")) {
            return false;
        }

        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets = 1;
        attach.actionSets = &actionSet_;
        if (!Check(xrAttachSessionActionSets(session_, &attach), "xrAttachSessionActionSets")) {
            return false;
        }

        for (std::size_t hand = 0; hand < handSpaces_.size(); ++hand) {
            XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            spaceInfo.action = poseAction_;
            spaceInfo.subactionPath = handPaths_[hand];
            spaceInfo.poseInActionSpace.orientation.w = 1.0f;

            if (!Check(
                        xrCreateActionSpace(session_, &spaceInfo, &handSpaces_[hand]),
                        hand == 0 ? "xrCreateActionSpace(left)" : "xrCreateActionSpace(right)")) {
                return false;
            }
        }

        return true;
    }

    bool PollEvents() {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};

        while (xrPollEvent(instance_, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                Fail("OpenXR instance loss pending.");
                return false;
            }

            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                sessionState_ = changed->state;

                if (sessionState_ == XR_SESSION_STATE_READY && !sessionRunning_) {
                    XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (!Check(xrBeginSession(session_, &beginInfo), "xrBeginSession")) {
                        return false;
                    }
                    sessionRunning_ = true;
                } else if (sessionState_ == XR_SESSION_STATE_STOPPING && sessionRunning_) {
                    if (!Check(xrEndSession(session_), "xrEndSession")) {
                        return false;
                    }
                    sessionRunning_ = false;
                } else if (sessionState_ == XR_SESSION_STATE_EXITING ||
                           sessionState_ == XR_SESSION_STATE_LOSS_PENDING) {
                    stopRequested_ = true;
                    return false;
                }
            }

            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }

        return true;
    }

    float FloatState(XrAction action, XrPath hand) {
        XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
        getInfo.action = action;
        getInfo.subactionPath = hand;

        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_SUCCEEDED(xrGetActionStateFloat(session_, &getInfo, &state)) && state.isActive) {
            return state.currentState;
        }
        return 0.0f;
    }

    XrVector2f VectorState(XrAction action, XrPath hand) {
        XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
        getInfo.action = action;
        getInfo.subactionPath = hand;

        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(session_, &getInfo, &state)) && state.isActive) {
            return state.currentState;
        }
        return {0.0f, 0.0f};
    }

    bool BoolState(XrAction action, XrPath hand = XR_NULL_PATH) {
        XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
        getInfo.action = action;
        getInfo.subactionPath = hand;

        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        return XR_SUCCEEDED(xrGetActionStateBoolean(session_, &getInfo, &state)) &&
               state.isActive &&
               state.currentState;
    }

    void FillHand(std::size_t index, XrTime time, HandV1& out) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        const XrResult locateResult = xrLocateSpace(handSpaces_[index], viewSpace_, time, &location);

        const XrSpaceLocationFlags needed =
                XR_SPACE_LOCATION_POSITION_VALID_BIT |
                XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;

        const bool poseValid =
                XR_SUCCEEDED(locateResult) &&
                (location.locationFlags & needed) == needed;

        if (poseValid) {
            out.position[0] = location.pose.position.x;
            out.position[1] = location.pose.position.y + kVirtualHeadHeight;
            out.position[2] = location.pose.position.z;

            out.orientation[0] = location.pose.orientation.x;
            out.orientation[1] = location.pose.orientation.y;
            out.orientation[2] = location.pose.orientation.z;
            out.orientation[3] = location.pose.orientation.w;
            out.flags = kPoseValid;
        } else {
            out.orientation[3] = 1.0f;
            out.flags = 0;
        }

        const XrPath hand = handPaths_[index];
        out.trigger = FloatState(triggerAction_, hand);
        out.grip = FloatState(squeezeAction_, hand);

        const XrVector2f stick = VectorState(thumbstickAction_, hand);
        out.joystick[0] = stick.x;
        out.joystick[1] = stick.y;

        if (BoolState(thumbClickAction_, hand)) {
            out.buttons |= ButtonThumbstick;
        }

        if (index == 0) {
            if (BoolState(primaryAction_, hand)) out.buttons |= ButtonX;
            if (BoolState(secondaryAction_, hand)) out.buttons |= ButtonY;
            if (BoolState(menuAction_)) out.buttons |= ButtonMenu;
        } else {
            if (BoolState(primaryAction_, hand)) out.buttons |= ButtonA;
            if (BoolState(secondaryAction_, hand)) out.buttons |= ButtonB;
        }
    }

    bool RunFrame() {
        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        if (!Check(xrWaitFrame(session_, &waitInfo, &frameState), "xrWaitFrame")) {
            return false;
        }

        XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        if (!Check(xrBeginFrame(session_, &beginInfo), "xrBeginFrame")) {
            return false;
        }

        XrActiveActionSet active{};
        active.actionSet = actionSet_;

        XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
        syncInfo.countActiveActionSets = 1;
        syncInfo.activeActionSets = &active;

        PacketV1 packet{};
        std::memcpy(packet.magic, "DXR1", 4);
        packet.version = kProtocolVersion;
        packet.size = sizeof(PacketV1);
        packet.sequence = sequence_++;

        const XrResult syncResult = xrSyncActions(session_, &syncInfo);
        if (XR_SUCCEEDED(syncResult)) {
            FillHand(0, frameState.predictedDisplayTime, packet.left);
            FillHand(1, frameState.predictedDisplayTime, packet.right);
        } else {
            packet.left.orientation[3] = 1.0f;
            packet.right.orientation[3] = 1.0f;
        }

        sendto(
                socket_,
                &packet,
                sizeof(packet),
                0,
                reinterpret_cast<const sockaddr*>(&destination_),
                sizeof(destination_));

        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = 0;
        endInfo.layers = nullptr;

        return Check(xrEndFrame(session_, &endInfo), "xrEndFrame");
    }

    void ThreadMain() {
        SetStatus("Starting OpenXR bridge...");

        bool ok =
                InitUdp() &&
                InitLoader() &&
                InitInstance() &&
                InitEgl() &&
                InitSession() &&
                InitActions();

        if (!ok) {
            Cleanup();
            running_ = false;
            return;
        }

        SetStatus("OpenXR initialized. Waiting for session...");

        auto lastStatus = std::chrono::steady_clock::now();
        std::uint64_t frameCounter = 0;
        std::uint64_t previousCounter = 0;

        while (!stopRequested_) {
            PollEvents();

            if (sessionRunning_) {
                if (!RunFrame()) {
                    break;
                }
                ++frameCounter;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            const auto now = std::chrono::steady_clock::now();
            if (now - lastStatus >= std::chrono::seconds(1)) {
                const std::uint64_t fps = frameCounter - previousCounter;
                previousCounter = frameCounter;
                lastStatus = now;

                SetStatus(
                        std::string("Streaming to ") + host_ + ":" + std::to_string(port_) +
                        " | OpenXR " + SessionStateName(sessionState_) +
                        " | " + std::to_string(fps) + " packets/s");
            }
        }

        Cleanup();
        running_ = false;

        if (Status().rfind("Error:", 0) != 0) {
            SetStatus("Bridge stopped.");
        }
    }

    void Cleanup() {
        if (session_ != XR_NULL_HANDLE && sessionRunning_) {
            xrEndSession(session_);
            sessionRunning_ = false;
        }

        for (auto& space : handSpaces_) {
            if (space != XR_NULL_HANDLE) {
                xrDestroySpace(space);
                space = XR_NULL_HANDLE;
            }
        }

        if (viewSpace_ != XR_NULL_HANDLE) {
            xrDestroySpace(viewSpace_);
            viewSpace_ = XR_NULL_HANDLE;
        }

        if (actionSet_ != XR_NULL_HANDLE) {
            xrDestroyActionSet(actionSet_);
            actionSet_ = XR_NULL_HANDLE;
        }

        if (session_ != XR_NULL_HANDLE) {
            xrDestroySession(session_);
            session_ = XR_NULL_HANDLE;
        }

        if (instance_ != XR_NULL_HANDLE) {
            xrDestroyInstance(instance_);
            instance_ = XR_NULL_HANDLE;
        }

        if (eglDisplay_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (eglSurface_ != EGL_NO_SURFACE) {
                eglDestroySurface(eglDisplay_, eglSurface_);
                eglSurface_ = EGL_NO_SURFACE;
            }
            if (eglContext_ != EGL_NO_CONTEXT) {
                eglDestroyContext(eglDisplay_, eglContext_);
                eglContext_ = EGL_NO_CONTEXT;
            }
            eglTerminate(eglDisplay_);
            eglDisplay_ = EGL_NO_DISPLAY;
        }

        if (socket_ >= 0) {
            close(socket_);
            socket_ = -1;
        }
    }

    JavaVM* javaVm_ = nullptr;
    jobject activity_ = nullptr;

    std::string host_;
    int port_ = 39742;

    mutable std::mutex statusMutex_;
    std::string status_ = "Idle";

    std::atomic_bool running_{false};
    std::atomic_bool stopRequested_{false};
    std::thread worker_;

    int socket_ = -1;
    sockaddr_in destination_{};

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSessionState sessionState_ = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning_ = false;

    XrSpace viewSpace_ = XR_NULL_HANDLE;
    std::array<XrPath, 2> handPaths_{XR_NULL_PATH, XR_NULL_PATH};
    std::array<XrSpace, 2> handSpaces_{XR_NULL_HANDLE, XR_NULL_HANDLE};

    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrAction poseAction_ = XR_NULL_HANDLE;
    XrAction triggerAction_ = XR_NULL_HANDLE;
    XrAction squeezeAction_ = XR_NULL_HANDLE;
    XrAction thumbstickAction_ = XR_NULL_HANDLE;
    XrAction primaryAction_ = XR_NULL_HANDLE;
    XrAction secondaryAction_ = XR_NULL_HANDLE;
    XrAction thumbClickAction_ = XR_NULL_HANDLE;
    XrAction menuAction_ = XR_NULL_HANDLE;

    EGLDisplay eglDisplay_ = EGL_NO_DISPLAY;
    EGLConfig eglConfig_ = nullptr;
    EGLContext eglContext_ = EGL_NO_CONTEXT;
    EGLSurface eglSurface_ = EGL_NO_SURFACE;

    std::uint32_t sequence_ = 0;
};

Bridge gBridge;

} // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_org_sakus_deskxr_MainActivity_nativeStart(
        JNIEnv* env,
        jclass,
        jobject activity,
        jstring host,
        jint port) {
    const char* rawHost = env->GetStringUTFChars(host, nullptr);
    const std::string hostText = rawHost != nullptr ? rawHost : "";
    if (rawHost != nullptr) {
        env->ReleaseStringUTFChars(host, rawHost);
    }

    return gBridge.Start(env, activity, hostText, port) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_sakus_deskxr_MainActivity_nativeStop(JNIEnv* env, jclass) {
    gBridge.Stop(env);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_sakus_deskxr_MainActivity_nativeIsRunning(JNIEnv*, jclass) {
    return gBridge.IsRunning() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_sakus_deskxr_MainActivity_nativeGetStatus(JNIEnv* env, jclass) {
    const std::string status = gBridge.Status();
    return env->NewStringUTF(status.c_str());
}
