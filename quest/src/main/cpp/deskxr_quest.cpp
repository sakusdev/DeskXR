#include <jni.h>
#include <android/log.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "protocol.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
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
constexpr float kVirtualHeadHeight = 1.65f;

namespace protocol = deskxr::protocol;

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
    bool Start(
            JNIEnv* env,
            jobject activity,
            const std::string& host,
            int port,
            float headHeight,
            float cameraDistance,
            float cameraYOffset,
            bool facingUser) {
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
        headHeight_ = headHeight;
        cameraDistance_ = cameraDistance;
        cameraYOffset_ = cameraYOffset;
        facingUser_ = facingUser;

        transformYawRad_ = facingUser_
                ? 3.14159265358979323846f
                : 0.0f;
        transformOffset_[0] = 0.0f;
        transformOffset_[1] = headHeight_ + cameraYOffset_;
        transformOffset_[2] = -cameraDistance_;
        quickCalibrated_ = false;
        calibrationPending_ = false;
        calibrationRequested_ = false;
        calibrationChordActive_ = false;
        calibrationChordLatched_ = false;
        lastRttMs_ = -1.0;
        for (auto& stamp : sentStamps_) {
            stamp.valid = false;
        }

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

    bool RequestQuickCalibration() {
        if (!running_.load() || !sessionRunning_) {
            return false;
        }

        calibrationRequested_.store(true);
        return true;
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
        if (!Check(
                    xrCreateReferenceSpace(session_, &viewInfo, &viewSpace_),
                    "xrCreateReferenceSpace(VIEW)")) {
            return false;
        }

        XrReferenceSpaceCreateInfo localInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        localInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        localInfo.poseInReferenceSpace.orientation.w = 1.0f;
        return Check(
                xrCreateReferenceSpace(session_, &localInfo, &localSpace_),
                "xrCreateReferenceSpace(LOCAL)");
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
            !CreateAction("primary_touch", "Primary touch", XR_ACTION_TYPE_BOOLEAN_INPUT, &primaryTouchAction_, true) ||
            !CreateAction("secondary_touch", "Secondary touch", XR_ACTION_TYPE_BOOLEAN_INPUT, &secondaryTouchAction_, true) ||
            !CreateAction("trigger_touch", "Trigger touch", XR_ACTION_TYPE_BOOLEAN_INPUT, &triggerTouchAction_, true) ||
            !CreateAction("thumb_touch", "Thumbstick touch", XR_ACTION_TYPE_BOOLEAN_INPUT, &thumbstickTouchAction_, true) ||
            !CreateAction("thumbrest_touch", "Thumbrest touch", XR_ACTION_TYPE_BOOLEAN_INPUT, &thumbrestTouchAction_, true) ||
            !CreateAction("thumb_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT, &thumbClickAction_, true) ||
            !CreateAction("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, &menuAction_, false) ||
            !CreateAction("haptic", "Haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, &hapticAction_, true)) {
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
        bind(primaryTouchAction_, "/user/hand/left/input/x/touch");
        bind(primaryTouchAction_, "/user/hand/right/input/a/touch");
        bind(secondaryTouchAction_, "/user/hand/left/input/y/touch");
        bind(secondaryTouchAction_, "/user/hand/right/input/b/touch");
        bind(triggerTouchAction_, "/user/hand/left/input/trigger/touch");
        bind(triggerTouchAction_, "/user/hand/right/input/trigger/touch");
        bind(thumbstickTouchAction_, "/user/hand/left/input/thumbstick/touch");
        bind(thumbstickTouchAction_, "/user/hand/right/input/thumbstick/touch");
        bind(thumbrestTouchAction_, "/user/hand/left/input/thumbrest/touch");
        bind(thumbrestTouchAction_, "/user/hand/right/input/thumbrest/touch");
        bind(thumbClickAction_, "/user/hand/left/input/thumbstick/click");
        bind(thumbClickAction_, "/user/hand/right/input/thumbstick/click");
        bind(menuAction_, "/user/hand/left/input/menu/click");
        bind(hapticAction_, "/user/hand/left/output/haptic");
        bind(hapticAction_, "/user/hand/right/output/haptic");

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

            if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
                const auto* changed =
                        reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(&event);

                if (changed->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                    quickCalibrated_ = false;
                    calibrationPending_ = false;
                    calibrationRequested_.store(false);
                    SetStatus("OpenXR recenter detected: quick calibration is required again.");
                }
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

    void PulseCalibrationHaptic() {
        for (std::size_t hand = 0; hand < handPaths_.size(); ++hand) {
            XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
            info.action = hapticAction_;
            info.subactionPath = handPaths_[hand];

            XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
            vibration.amplitude = 0.65f;
            vibration.duration = static_cast<XrDuration>(80'000'000);
            vibration.frequency = XR_FREQUENCY_UNSPECIFIED;

            xrApplyHapticFeedback(
                    session_,
                    &info,
                    reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
        }
    }

    void ProcessCalibrationChord(XrTime time) {
        const bool chord =
                FloatState(triggerAction_, handPaths_[0]) >= 0.80f &&
                FloatState(triggerAction_, handPaths_[1]) >= 0.80f &&
                BoolState(thumbClickAction_, handPaths_[0]) &&
                BoolState(thumbClickAction_, handPaths_[1]);

        const auto now = std::chrono::steady_clock::now();

        if (!chord) {
            calibrationChordActive_ = false;
            calibrationChordLatched_ = false;
            return;
        }

        if (!calibrationChordActive_) {
            calibrationChordActive_ = true;
            calibrationChordSince_ = now;
            return;
        }

        if (!calibrationChordLatched_ &&
            (now - calibrationChordSince_) >= std::chrono::milliseconds(1200)) {
            calibrationChordLatched_ = true;
            if (QuickCalibrateFromHands(time)) {
                PulseCalibrationHaptic();
            }
        }
    }

    bool QuickCalibrateFromHands(XrTime time) {
        std::array<XrSpaceLocation, 2> locations{
                XrSpaceLocation{XR_TYPE_SPACE_LOCATION},
                XrSpaceLocation{XR_TYPE_SPACE_LOCATION}};

        const XrSpaceLocationFlags needed =
                XR_SPACE_LOCATION_POSITION_VALID_BIT |
                XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;

        for (std::size_t hand = 0; hand < locations.size(); ++hand) {
            const XrResult result =
                    xrLocateSpace(handSpaces_[hand], localSpace_, time, &locations[hand]);
            if (XR_FAILED(result) ||
                (locations[hand].locationFlags & needed) != needed) {
                SetStatus("Quick calibration failed: both controller poses must be tracked.");
                return false;
            }
        }

        const auto& left = locations[0].pose.position;
        const auto& right = locations[1].pose.position;

        const float dx = right.x - left.x;
        const float dz = right.z - left.z;
        const float horizontalSpan = std::sqrt(dx * dx + dz * dz);

        if (horizontalSpan < 0.18f) {
            SetStatus("Quick calibration failed: hold controllers farther apart.");
            return false;
        }

        // When the controllers are held shoulder-width apart, the vector from
        // left to right approximates the user's +X axis. Rotating that vector
        // onto +X gives the yaw from Quest VIEW space into DeskXR user space.
        transformYawRad_ = std::atan2(dz, dx);

        const float midX = (left.x + right.x) * 0.5f;
        const float midY = (left.y + right.y) * 0.5f;
        const float midZ = (left.z + right.z) * 0.5f;

        const float cosYaw = std::cos(transformYawRad_);
        const float sinYaw = std::sin(transformYawRad_);
        const float rotatedMidX = cosYaw * midX + sinYaw * midZ;
        const float rotatedMidZ = -sinYaw * midX + cosYaw * midZ;

        // Calibration pose: controllers shoulder-width apart, held in front
        // of the upper chest. This makes the midpoint a predictable anchor.
        constexpr float kChestDropFromHeadMeters = 0.30f;
        constexpr float kHandsForwardFromHeadMeters = 0.35f;

        const float targetX = 0.0f;
        const float targetY = headHeight_ - kChestDropFromHeadMeters;
        const float targetZ = -kHandsForwardFromHeadMeters;

        transformOffset_[0] = targetX - rotatedMidX;
        transformOffset_[1] = targetY - midY;
        transformOffset_[2] = targetZ - rotatedMidZ;

        calibrationPending_ = false;
        calibrationRequested_.store(false);
        quickCalibrated_ = true;
        SetStatus("Quick calibration captured.");
        return true;
    }

    void FillHand(std::size_t index, XrTime time, protocol::HandV1& out) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        const XrSpace baseSpace =
                quickCalibrated_ && localSpace_ != XR_NULL_HANDLE
                        ? localSpace_
                        : viewSpace_;
        const XrResult locateResult =
                xrLocateSpace(handSpaces_[index], baseSpace, time, &location);

        const XrSpaceLocationFlags needed =
                XR_SPACE_LOCATION_POSITION_VALID_BIT |
                XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;

        const bool poseValid =
                XR_SUCCEEDED(locateResult) &&
                (location.locationFlags & needed) == needed;

        if (poseValid) {
            const float cosYaw = std::cos(transformYawRad_);
            const float sinYaw = std::sin(transformYawRad_);

            const float sourceX = location.pose.position.x;
            const float sourceY = location.pose.position.y;
            const float sourceZ = location.pose.position.z;

            out.position[0] =
                    transformOffset_[0] + cosYaw * sourceX + sinYaw * sourceZ;
            out.position[1] =
                    transformOffset_[1] + sourceY;
            out.position[2] =
                    transformOffset_[2] - sinYaw * sourceX + cosYaw * sourceZ;

            const float qx = location.pose.orientation.x;
            const float qy = location.pose.orientation.y;
            const float qz = location.pose.orientation.z;
            const float qw = location.pose.orientation.w;

            const float halfYaw = transformYawRad_ * 0.5f;
            const float yawY = std::sin(halfYaw);
            const float yawW = std::cos(halfYaw);

            // q_virtual = q_yaw * q_source. After quick calibration the
            // source is LOCAL space, so headset pitch/roll no longer tilts
            // the controller coordinate frame.
            out.orientation[0] = yawW * qx + yawY * qz;
            out.orientation[1] = yawW * qy + yawY * qw;
            out.orientation[2] = yawW * qz - yawY * qx;
            out.orientation[3] = yawW * qw - yawY * qy;
            out.flags = protocol::kPoseValid;
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
            out.buttons |= protocol::ButtonThumbstick;
        }
        if (BoolState(triggerTouchAction_, hand)) {
            out.buttons |= protocol::TouchTrigger;
        }
        if (BoolState(thumbstickTouchAction_, hand)) {
            out.buttons |= protocol::TouchThumbstick;
        }
        if (BoolState(thumbrestTouchAction_, hand)) {
            out.buttons |= protocol::TouchThumbrest;
        }

        if (index == 0) {
            if (BoolState(primaryAction_, hand)) out.buttons |= protocol::ButtonX;
            if (BoolState(secondaryAction_, hand)) out.buttons |= protocol::ButtonY;
            if (BoolState(primaryTouchAction_, hand)) out.buttons |= protocol::TouchX;
            if (BoolState(secondaryTouchAction_, hand)) out.buttons |= protocol::TouchY;
            if (BoolState(menuAction_)) out.buttons |= protocol::ButtonMenu;
        } else {
            if (BoolState(primaryAction_, hand)) out.buttons |= protocol::ButtonA;
            if (BoolState(secondaryAction_, hand)) out.buttons |= protocol::ButtonB;
            if (BoolState(primaryTouchAction_, hand)) out.buttons |= protocol::TouchA;
            if (BoolState(secondaryTouchAction_, hand)) out.buttons |= protocol::TouchB;
        }
    }

    void ProcessInboundControl() {
        while (true) {
            std::array<std::uint8_t, 64> buffer{};
            sockaddr_in from{};
            socklen_t fromLength = sizeof(from);

            const ssize_t received = recvfrom(
                    socket_,
                    buffer.data(),
                    buffer.size(),
                    MSG_DONTWAIT,
                    reinterpret_cast<sockaddr*>(&from),
                    &fromLength);

            if (received < 0) {
                break;
            }

            if (from.sin_addr.s_addr != destination_.sin_addr.s_addr ||
                from.sin_port != destination_.sin_port) {
                continue;
            }

            if (received == static_cast<ssize_t>(sizeof(protocol::AckPacketV1))) {
                const auto* ack = reinterpret_cast<const protocol::AckPacketV1*>(buffer.data());
                if (std::memcmp(ack->magic, protocol::kAckMagic, 4) == 0 &&
                    ack->version == protocol::kVersion &&
                    ack->size == sizeof(protocol::AckPacketV1)) {
                    const auto ackNow = std::chrono::steady_clock::now();
                    lastAckReceived_ = ackNow;
                    lastAckSequence_ = ack->sequence;
                    ackCount_.fetch_add(1);

                    auto& stamp = sentStamps_[ack->sequence % sentStamps_.size()];
                    if (stamp.valid && stamp.sequence == ack->sequence) {
                        lastRttMs_ =
                                std::chrono::duration<double, std::milli>(
                                        ackNow - stamp.sentAt)
                                        .count();
                    }

                    continue;
                }
            }

            if (received != static_cast<ssize_t>(sizeof(protocol::HapticPacketV1))) {
                continue;
            }

            const auto* packet = reinterpret_cast<const protocol::HapticPacketV1*>(buffer.data());
            if (std::memcmp(packet->magic, protocol::kHapticMagic, 4) != 0) continue;
            if (packet->version != protocol::kVersion) continue;
            if (packet->size != sizeof(protocol::HapticPacketV1)) continue;
            if (!std::isfinite(packet->durationSeconds) ||
                !std::isfinite(packet->frequencyHz) ||
                !std::isfinite(packet->amplitude)) {
                continue;
            }

            const auto handIndex = static_cast<std::size_t>(packet->hand);
            if (handIndex >= handPaths_.size()) continue;

            XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
            info.action = hapticAction_;
            info.subactionPath = handPaths_[handIndex];

            XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
            vibration.amplitude = std::clamp(packet->amplitude, 0.0f, 1.0f);
            vibration.frequency = packet->frequencyHz > 0.0f
                    ? packet->frequencyHz
                    : XR_FREQUENCY_UNSPECIFIED;

            if (packet->durationSeconds <= 0.0f) {
                vibration.duration = XR_MIN_HAPTIC_DURATION;
            } else {
                const double nanos =
                        static_cast<double>(packet->durationSeconds) * 1'000'000'000.0;
                vibration.duration = static_cast<XrDuration>(
                        std::clamp(nanos, 1.0, 5'000'000'000.0));
            }

            const XrResult result = xrApplyHapticFeedback(
                    session_,
                    &info,
                    reinterpret_cast<const XrHapticBaseHeader*>(&vibration));

            if (XR_SUCCEEDED(result)) {
                hapticCount_.fetch_add(1);
            }
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

        protocol::PacketV1 packet{};
        std::memcpy(packet.magic, protocol::kTrackingMagic, 4);
        packet.version = protocol::kVersion;
        packet.size = sizeof(protocol::PacketV1);
        packet.sequence = sequence_++;

        const XrResult syncResult = xrSyncActions(session_, &syncInfo);
        if (XR_SUCCEEDED(syncResult)) {
            ProcessCalibrationChord(frameState.predictedDisplayTime);

            if (calibrationRequested_.exchange(false)) {
                calibrationPending_ = true;
                quickCalibrated_ = false;
                calibrationDue_ =
                        std::chrono::steady_clock::now() + std::chrono::seconds(3);
                SetStatus(
                        "Quick calibration in 3s: hold controllers shoulder-width apart at upper chest.");
            }

            if (calibrationPending_ &&
                std::chrono::steady_clock::now() >= calibrationDue_) {
                calibrationPending_ = false;
                QuickCalibrateFromHands(frameState.predictedDisplayTime);
            }

            FillHand(0, frameState.predictedDisplayTime, packet.left);
            FillHand(1, frameState.predictedDisplayTime, packet.right);
        } else {
            packet.left.orientation[3] = 1.0f;
            packet.right.orientation[3] = 1.0f;
        }

        auto& stamp = sentStamps_[packet.sequence % sentStamps_.size()];
        stamp.sequence = packet.sequence;
        stamp.sentAt = std::chrono::steady_clock::now();
        stamp.valid = true;

        sendto(
                socket_,
                &packet,
                sizeof(packet),
                0,
                reinterpret_cast<const sockaddr*>(&destination_),
                sizeof(destination_));

        ProcessInboundControl();

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

                const bool pcLinked =
                        lastAckReceived_.time_since_epoch().count() != 0 &&
                        (now - lastAckReceived_) < std::chrono::seconds(2);

                std::string calibrationText =
                        quickCalibrated_ ? "quick" : "manual";

                if (calibrationPending_) {
                    const auto remaining =
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                    calibrationDue_ - now)
                                    .count();
                    const auto seconds =
                            std::max<std::int64_t>(0, (remaining + 999) / 1000);
                    calibrationText = "pending-" + std::to_string(seconds) + "s";
                }

                const std::string rttText =
                        lastRttMs_ >= 0.0
                                ? std::to_string(static_cast<int>(std::lround(lastRttMs_))) + "ms"
                                : "--";

                SetStatus(
                        std::string("Streaming to ") + host_ + ":" + std::to_string(port_) +
                        " | OpenXR " + SessionStateName(sessionState_) +
                        " | " + std::to_string(fps) + " packets/s" +
                        " | PC " + (pcLinked ? "linked" : "no-ack") +
                        " | RTT " + rttText +
                        " | calib " + calibrationText +
                        " | haptics " + std::to_string(hapticCount_.load()));
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

        if (localSpace_ != XR_NULL_HANDLE) {
            xrDestroySpace(localSpace_);
            localSpace_ = XR_NULL_HANDLE;
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
    float headHeight_ = kVirtualHeadHeight;
    float cameraDistance_ = 0.70f;
    float cameraYOffset_ = -0.30f;
    bool facingUser_ = true;

    float transformYawRad_ = 0.0f;
    std::array<float, 3> transformOffset_{0.0f, kVirtualHeadHeight, -0.70f};
    bool quickCalibrated_ = false;
    bool calibrationPending_ = false;
    std::chrono::steady_clock::time_point calibrationDue_{};
    std::atomic_bool calibrationRequested_{false};
    bool calibrationChordActive_ = false;
    bool calibrationChordLatched_ = false;
    std::chrono::steady_clock::time_point calibrationChordSince_{};

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
    std::atomic_bool sessionRunning_{false};

    XrSpace viewSpace_ = XR_NULL_HANDLE;
    XrSpace localSpace_ = XR_NULL_HANDLE;
    std::array<XrPath, 2> handPaths_{XR_NULL_PATH, XR_NULL_PATH};
    std::array<XrSpace, 2> handSpaces_{XR_NULL_HANDLE, XR_NULL_HANDLE};

    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrAction poseAction_ = XR_NULL_HANDLE;
    XrAction triggerAction_ = XR_NULL_HANDLE;
    XrAction squeezeAction_ = XR_NULL_HANDLE;
    XrAction thumbstickAction_ = XR_NULL_HANDLE;
    XrAction primaryAction_ = XR_NULL_HANDLE;
    XrAction secondaryAction_ = XR_NULL_HANDLE;
    XrAction primaryTouchAction_ = XR_NULL_HANDLE;
    XrAction secondaryTouchAction_ = XR_NULL_HANDLE;
    XrAction triggerTouchAction_ = XR_NULL_HANDLE;
    XrAction thumbstickTouchAction_ = XR_NULL_HANDLE;
    XrAction thumbrestTouchAction_ = XR_NULL_HANDLE;
    XrAction thumbClickAction_ = XR_NULL_HANDLE;
    XrAction menuAction_ = XR_NULL_HANDLE;
    XrAction hapticAction_ = XR_NULL_HANDLE;

    EGLDisplay eglDisplay_ = EGL_NO_DISPLAY;
    EGLConfig eglConfig_ = nullptr;
    EGLContext eglContext_ = EGL_NO_CONTEXT;
    EGLSurface eglSurface_ = EGL_NO_SURFACE;

    struct SentStamp {
        std::uint32_t sequence = 0;
        std::chrono::steady_clock::time_point sentAt{};
        bool valid = false;
    };

    std::uint32_t sequence_ = 0;
    std::array<SentStamp, 256> sentStamps_{};
    std::chrono::steady_clock::time_point lastAckReceived_{};
    std::uint32_t lastAckSequence_ = 0;
    double lastRttMs_ = -1.0;
    std::atomic_uint64_t ackCount_{0};
    std::atomic_uint64_t hapticCount_{0};
};

Bridge gBridge;

} // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_org_sakus_deskxr_MainActivity_nativeStart(
        JNIEnv* env,
        jclass,
        jobject activity,
        jstring host,
        jint port,
        jfloat headHeight,
        jfloat cameraDistance,
        jfloat cameraYOffset,
        jboolean facingUser) {
    const char* rawHost = env->GetStringUTFChars(host, nullptr);
    const std::string hostText = rawHost != nullptr ? rawHost : "";
    if (rawHost != nullptr) {
        env->ReleaseStringUTFChars(host, rawHost);
    }

    return gBridge.Start(
            env,
            activity,
            hostText,
            port,
            headHeight,
            cameraDistance,
            cameraYOffset,
            facingUser == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_sakus_deskxr_MainActivity_nativeStop(JNIEnv* env, jclass) {
    gBridge.Stop(env);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_sakus_deskxr_MainActivity_nativeRequestQuickCalibration(JNIEnv*, jclass) {
    return gBridge.RequestQuickCalibration() ? JNI_TRUE : JNI_FALSE;
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
