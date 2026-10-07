// SPDX-License-Identifier: GPL-3.0-only
#include "runtime.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <thread>

namespace ovp {
static constexpr char SystemDisplayRefreshRateExtension[] =
    "XR_AXRB_system_display_refresh_rate";
Runtime& runtime() { static Runtime state; return state; }
bool xrOk(XrResult result, const char* operation) {
    if (XR_SUCCEEDED(result)) return true;
    OVP_ERROR("%s failed: OpenXR result %d", operation, result);
    return false;
}
double secondsNow() {
    timespec value{};
    clock_gettime(CLOCK_MONOTONIC, &value);
    return double(value.tv_sec) + double(value.tv_nsec) * 1e-9;
}
XrTime toXrTime(double seconds) {
    auto& s = runtime();
    if (!std::isfinite(seconds) || seconds <= 0) return s.frame.predictedDisplayTime;
    return static_cast<XrTime>((seconds + s.timeOffset) * 1e9);
}
double toSeconds(XrTime time) { return double(time) * 1e-9 - runtime().timeOffset; }
XrPosef toXrPose(const ovrPosef& pose) {
    return {{pose.Orientation.x, pose.Orientation.y, pose.Orientation.z, pose.Orientation.w},
            {pose.Position.x, pose.Position.y, pose.Position.z}};
}
ovrPosef fromXrPose(const XrPosef& pose) {
    return {{pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w},
            {pose.position.x, pose.position.y, pose.position.z}};
}
bool validMobile(const ovrMobile* mobile) {
    const auto& s = runtime();
    return mobile == &s.mobile && s.inVr && s.session != XR_NULL_HANDLE;
}
static bool symmetricFovDegrees(const XrFovf& first, const XrFovf& second,
                                std::array<float, 2>& degrees);
static bool load(const char* name, PFN_xrVoidFunction* target) {
    auto& s = runtime();
    return xrOk(s.xr.xrGetInstanceProcAddr(s.instance, name, target), name) && *target;
}
static bool referenceSpace(XrReferenceSpaceType type, const XrPosef& pose, XrSpace& space) {
    auto& s = runtime();
    XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    info.referenceSpaceType = type;
    info.poseInReferenceSpace = pose;
    return xrOk(s.xr.xrCreateReferenceSpace(s.session, &info, &space), "xrCreateReferenceSpace");
}
static void destroySession() {
    auto& s = runtime();
    if (!s.session) return;
    if (s.frameBegun) endFrame(nullptr, 0);
    if (s.running) {
        s.xr.xrRequestExitSession(s.session);
        pollEvents();
    }
    graphics::shutdownSession();
    input::shutdown();
    for (auto* space : {&s.appSpace, &s.viewSpace, &s.stageSpace, &s.localSpace}) {
        if (*space) s.xr.xrDestroySpace(*space);
        *space = XR_NULL_HANDLE;
    }
    s.xr.xrDestroySession(s.session);
    s.session = XR_NULL_HANDLE;
    s.sessionState = XR_SESSION_STATE_UNKNOWN;
    s.running = s.inVr = s.frameBegun = false;
    s.frameIndex = -1;
    s.frame = {XR_TYPE_FRAME_STATE};
    s.timeMapped = false;
    s.locatedFovValid = false;
    s.initialRecenterPending = false;
    s.timeOffset = 0;
}
static void shutdown() {
    auto& s = runtime();
    destroySession();
    graphics::shutdown();
    if (s.instance && s.xr.xrDestroyInstance) s.xr.xrDestroyInstance(s.instance);
    s.instance = XR_NULL_HANDLE;
    s.system = XR_NULL_SYSTEM_ID;
    s.viewConfigurationFovExtension = false;
    s.colorSpaceExtension = false;
    s.performanceExtension = false;
    s.threadExtension = false;
    s.systemDisplayRefreshRateExtension = false;
    s.displayRefreshRateExtension = false;
    s.recommendedFovValid = false;
    s.locatedFovValid = false;
    s.recommendedFovDegrees = {};
    s.locatedFovDegrees = {};
    s.recenterCount = 0;
    s.loggedDisplayRefreshRateSource = DisplayRefreshRateSource::Unqueried;
    s.loggedDisplayRefreshRate = 0;
    if (s.activity && s.vm) {
        JNIEnv* env = nullptr;
        bool attached = false;
        if (s.vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
            attached = s.vm->AttachCurrentThread(&env, nullptr) == JNI_OK;
        }
        if (env) env->DeleteGlobalRef(s.activity);
        if (attached) s.vm->DetachCurrentThread();
    }
    s.activity = nullptr;
    s.vm = nullptr;
    s.vk = {};
    s.queue = VK_NULL_HANDLE;
    s.setColorSpace = nullptr;
    s.setPerformance = nullptr;
    s.setThread = nullptr;
    s.getSystemDisplayRefreshRate = nullptr;
    s.getDisplayRefreshRate = nullptr;
    if (s.loader) dlclose(s.loader);
    s.loader = nullptr;
    s.xr = {};
}
bool pollEvents() {
    auto& s = runtime();
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    for (;;) {
        XrResult result = s.xr.xrPollEvent(s.instance, &event);
        if (result == XR_EVENT_UNAVAILABLE) return true;
        if (!xrOk(result, "xrPollEvent")) return false;
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& change = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
            if (change.session == s.session) {
                s.sessionState = change.state;
                OVP_LOG("session state=%d", change.state);
                if (change.state == XR_SESSION_STATE_READY && !s.running) {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (!xrOk(s.xr.xrBeginSession(s.session, &begin), "xrBeginSession")) return false;
                    s.running = true;
                } else if (change.state == XR_SESSION_STATE_STOPPING && s.running) {
                    if (s.frameBegun) endFrame(nullptr, 0);
                    xrOk(s.xr.xrEndSession(s.session), "xrEndSession");
                    s.running = false;
                } else if (change.state == XR_SESSION_STATE_EXITING || change.state == XR_SESSION_STATE_LOSS_PENDING) {
                    s.running = false;
                    return false;
                }
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            s.running = false;
            return false;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}
bool ensureGraphicsQueue(VkQueue preferredQueue) {
    auto& s = runtime();
    if (!s.vk.Device || !s.vk.PhysicalDevice) return false;
    if (s.queue) {
        if (preferredQueue) s.queue = preferredQueue;
        return true;
    }
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(s.vk.PhysicalDevice, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(s.vk.PhysicalDevice, &count, families.data());
    auto selected = std::find_if(families.begin(), families.end(), [](const auto& f) { return f.queueCount && (f.queueFlags & VK_QUEUE_GRAPHICS_BIT); });
    if (selected == families.end()) return false;
    s.queueFamily = static_cast<uint32_t>(selected - families.begin());
    // Submit on the application's synchronization queue. Emulated Vulkan drivers
    // can return distinct guest handles for repeated requests for the same queue.
    s.queue = preferredQueue;
    if (!s.queue) vkGetDeviceQueue(s.vk.Device, s.queueFamily, 0, &s.queue);
    if (!s.queue) {
        OVP_ERROR("VrApi adapter could not obtain a graphics synchronization queue");
        return false;
    }
    return true;
}
bool ensureSession(VkQueue preferredQueue) {
    auto& s = runtime();
    if (s.session) return !preferredQueue || preferredQueue == s.queue;
    if (!s.instance || !s.vk.Device) return false;
    VkPhysicalDevice expected = VK_NULL_HANDLE;
    if (!xrOk(s.xr.xrGetVulkanGraphicsDeviceKHR(s.instance, s.system, s.vk.Instance, &expected), "xrGetVulkanGraphicsDeviceKHR") || expected != s.vk.PhysicalDevice) {
        OVP_ERROR("Application Vulkan device does not match the OpenXR graphics device");
        return false;
    }
    if (!ensureGraphicsQueue(preferredQueue)) return false;
    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance = s.vk.Instance;
    binding.physicalDevice = s.vk.PhysicalDevice;
    binding.device = s.vk.Device;
    binding.queueFamilyIndex = s.queueFamily;
    XrSessionCreateInfo create{XR_TYPE_SESSION_CREATE_INFO};
    create.next = &binding;
    create.systemId = s.system;
    if (!xrOk(s.xr.xrCreateSession(s.instance, &create, &s.session), "xrCreateSession")) return false;
    const XrPosef identity{{0, 0, 0, 1}, {0, 0, 0}};
    if (!referenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, identity, s.localSpace) ||
        !referenceSpace(XR_REFERENCE_SPACE_TYPE_VIEW, identity, s.viewSpace) ||
        !referenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, identity, s.appSpace)) {
        destroySession();
        return false;
    }
    // STAGE is optional. Eye-level tracking remains available without a floor origin.
    XrReferenceSpaceCreateInfo stage{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    stage.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    stage.poseInReferenceSpace = identity;
    s.xr.xrCreateReferenceSpace(s.session, &stage, &s.stageSpace);
    if (!input::initialize()) { destroySession(); return false; }
    s.trackingTransform = {{0, 0, 0, 1}, {0, 0, 0}};
    s.centerEyeTransform = s.trackingTransform;
    s.initialRecenterPending = true;
    return true;
}
bool beginFrame(int64_t index) {
    auto& s = runtime();
    if (!s.session || !pollEvents()) return false;
    if (!s.running) return false;
    if (s.frameBegun) return true;
    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
    s.frame = {XR_TYPE_FRAME_STATE};
    if (!xrOk(s.xr.xrWaitFrame(s.session, &wait, &s.frame), "xrWaitFrame")) return false;
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
    if (!xrOk(s.xr.xrBeginFrame(s.session, &begin), "xrBeginFrame")) return false;
    s.frameBegun = true;
    s.frameIndex = index;
    if (!s.timeMapped) {
        // Map the runtime's opaque clock to VrApi's CLOCK_MONOTONIC seconds.
        // Prefer the standard time-conversion extension; estimate only on runtimes without it.
        PFN_xrConvertTimeToTimespecTimeKHR convert = nullptr;
        if (XR_SUCCEEDED(s.xr.xrGetInstanceProcAddr(s.instance, "xrConvertTimeToTimespecTimeKHR", reinterpret_cast<PFN_xrVoidFunction*>(&convert))) && convert) {
            timespec time{};
            if (XR_SUCCEEDED(convert(s.instance, s.frame.predictedDisplayTime, &time))) {
                s.timeOffset = double(s.frame.predictedDisplayTime) * 1e-9 - (double(time.tv_sec) + double(time.tv_nsec) * 1e-9);
            } else {
                endFrame(nullptr, 0);
                return false;
            }
        } else {
            s.timeOffset = double(s.frame.predictedDisplayTime - s.frame.predictedDisplayPeriod) * 1e-9 - secondsNow();
        }
        s.timeMapped = true;
    }
    if (s.initialRecenterPending) {
        // OpenXR LOCAL need not be eye-level. Establish VrApi's initial yaw/position
        // origin from a tracked, visible head before exposing it to the application.
        XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
        constexpr XrSpaceLocationFlags tracked =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
            XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        if ((s.sessionState != XR_SESSION_STATE_VISIBLE && s.sessionState != XR_SESSION_STATE_FOCUSED) ||
            !xrOk(s.xr.xrLocateSpace(s.viewSpace, s.localSpace, s.frame.predictedDisplayTime, &head), "initial eye-level origin") ||
            (head.locationFlags & tracked) != tracked) {
            endFrame(nullptr, 0);
            return false;
        }
        const auto& q = head.pose.orientation;
        const auto& p = head.pose.position;
        const float yaw = std::atan2(2*(q.w*q.y + q.x*q.z), 1-2*(q.x*q.x+q.y*q.y));
        if (!std::isfinite(yaw) || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            endFrame(nullptr, 0);
            return false;
        }
        const XrPosef origin{{0, std::sin(yaw/2), 0, std::cos(yaw/2)}, p};
        XrSpace next = XR_NULL_HANDLE;
        if (!referenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, origin, next)) {
            endFrame(nullptr, 0);
            return false;
        }
        s.xr.xrDestroySpace(s.appSpace);
        s.appSpace = next;
        s.centerEyeTransform = s.trackingTransform = fromXrPose(origin);
        s.initialRecenterPending = false;
        ++s.recenterCount;
    }
    XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
    info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    info.displayTime = s.frame.predictedDisplayTime;
    info.space = s.appSpace;
    XrViewState state{XR_TYPE_VIEW_STATE};
    uint32_t count = 0;
    if (!xrOk(s.xr.xrLocateViews(s.session, &info, &state, 2, &count, s.views.data()), "xrLocateViews") || count != 2) {
        endFrame(nullptr, 0);
        return false;
    }
    s.locatedFovValid = symmetricFovDegrees(
        s.views[0].fov, s.views[1].fov, s.locatedFovDegrees);
    input::sync();
    return true;
}
XrResult endFrame(const XrCompositionLayerBaseHeader* const* layers, uint32_t count) {
    auto& s = runtime();
    if (!s.frameBegun) return XR_ERROR_CALL_ORDER_INVALID;
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
    info.displayTime = s.frame.predictedDisplayTime;
    info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    info.layerCount = count;
    info.layers = layers;
    XrResult result = s.xr.xrEndFrame(s.session, &info);
    s.frameBegun = false;
    xrOk(result, "xrEndFrame");
    return result;
}
bool locate(XrSpace space, XrTime time, ovrRigidBodyPosef& pose, uint32_t& status) {
    auto& s = runtime();
    pose = {};
    pose.Pose.Orientation.w = 1;
    status = 0;
    if (!space || !s.appSpace || time <= 0) return false;
    XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION, &velocity};
    if (!xrOk(s.xr.xrLocateSpace(space, s.appSpace, time, &location), "xrLocateSpace")) return false;
    if (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) status |= 4;
    if (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) status |= 8;
    if (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) status |= 1;
    if (location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) status |= 2;
    if (status) status |= 128;
    pose.Pose = fromXrPose(location.pose);
    if (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) pose.AngularVelocity = {velocity.angularVelocity.x, velocity.angularVelocity.y, velocity.angularVelocity.z};
    if (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) pose.LinearVelocity = {velocity.linearVelocity.x, velocity.linearVelocity.y, velocity.linearVelocity.z};
    pose.TimeInSeconds = toSeconds(time);
    pose.PredictionInSeconds = std::max(0.0, pose.TimeInSeconds - secondsNow());
    return true;
}
static ovrMatrix4f viewMatrix(const XrPosef& p) {
    // Inverse rigid transform, row-major. Both APIs use right-handed -Z forward.
    const float x = p.orientation.x, y = p.orientation.y, z = p.orientation.z, w = p.orientation.w;
    ovrMatrix4f m{{{1-2*y*y-2*z*z, 2*x*y+2*z*w, 2*x*z-2*y*w, 0},
                    {2*x*y-2*z*w, 1-2*x*x-2*z*z, 2*y*z+2*x*w, 0},
                    {2*x*z+2*y*w, 2*y*z-2*x*w, 1-2*x*x-2*y*y, 0}, {0,0,0,1}}};
    for (int row = 0; row < 3; ++row) m.M[row][3] = -(m.M[row][0]*p.position.x + m.M[row][1]*p.position.y + m.M[row][2]*p.position.z);
    return m;
}
static ovrMatrix4f projection(const XrFovf& f) {
    float l = std::tan(f.angleLeft), r = std::tan(f.angleRight), d = std::tan(f.angleDown), u = std::tan(f.angleUp);
    // VrApi supplies the conventional GL-depth projection; CryEngine converts for Vulkan.
    return {{{2/(r-l), 0, (r+l)/(r-l), 0}, {0, 2/(u-d), (u+d)/(u-d), 0}, {0, 0, -1, -0.2f}, {0, 0, -1, 0}}};
}
static bool symmetricFovDegrees(const XrFovf& first, const XrFovf& second,
                                std::array<float, 2>& degrees) {
    auto valid = [](const XrFovf& fov) {
        constexpr float limit = 1.57079632679f;
        return std::isfinite(fov.angleLeft) && std::isfinite(fov.angleRight) &&
               std::isfinite(fov.angleUp) && std::isfinite(fov.angleDown) &&
               fov.angleLeft > -limit && fov.angleRight < limit &&
               fov.angleDown > -limit && fov.angleUp < limit &&
               fov.angleLeft < fov.angleRight && fov.angleDown < fov.angleUp;
    };
    if (!valid(first) || !valid(second)) return false;
    const float horizontalHalf = std::max({
        std::abs(first.angleLeft), std::abs(first.angleRight),
        std::abs(second.angleLeft), std::abs(second.angleRight)});
    const float verticalHalf = std::max({
        std::abs(first.angleDown), std::abs(first.angleUp),
        std::abs(second.angleDown), std::abs(second.angleUp)});
    constexpr float degreesPerRadian = 57.295779513f;
    degrees = {2.0f * horizontalHalf * degreesPerRadian,
               2.0f * verticalHalf * degreesPerRadian};
    return true;
}
static const char* displayRefreshRateSourceName(DisplayRefreshRateSource source) {
    switch (source) {
        case DisplayRefreshRateSource::AxrbSystem: return "XR_AXRB_system";
        case DisplayRefreshRateSource::FbSession: return "XR_FB_session";
        case DisplayRefreshRateSource::FramePeriod: return "frame_period";
        default: return "unavailable";
    }
}
static float queryDisplayRefreshRate() {
    auto& s = runtime();
    float rate = 0;
    DisplayRefreshRateSource source = DisplayRefreshRateSource::Unavailable;
    if (s.systemDisplayRefreshRateExtension) {
        source = DisplayRefreshRateSource::AxrbSystem;
        float queried = 0;
        if (s.getSystemDisplayRefreshRate && s.instance && s.system != XR_NULL_SYSTEM_ID &&
            XR_SUCCEEDED(s.getSystemDisplayRefreshRate(s.instance, s.system, &queried)) &&
            std::isfinite(queried) && queried > 0) {
            rate = queried;
        }
    } else if (s.displayRefreshRateExtension) {
        source = DisplayRefreshRateSource::FbSession;
        float queried = 0;
        if (s.getDisplayRefreshRate && s.session &&
            XR_SUCCEEDED(s.getDisplayRefreshRate(s.session, &queried)) &&
            std::isfinite(queried) && queried > 0) {
            rate = queried;
        }
    } else if (s.frame.predictedDisplayPeriod > 0) {
        source = DisplayRefreshRateSource::FramePeriod;
        rate = float(1e9 / double(s.frame.predictedDisplayPeriod));
    }
    if (s.loggedDisplayRefreshRateSource == DisplayRefreshRateSource::Unqueried ||
        s.loggedDisplayRefreshRateSource != source ||
        std::abs(s.loggedDisplayRefreshRate - rate) >= 0.001f) {
        OVP_LOG("Display refresh rate=%.3f Hz source=%s",
                rate, displayRefreshRateSourceName(source));
        s.loggedDisplayRefreshRateSource = source;
        s.loggedDisplayRefreshRate = rate;
    }
    return rate;
}
}
using namespace ovp;
#define LOCK_STATE auto& s = runtime(); std::lock_guard<std::recursive_mutex> lock(s.mutex)
VRAPI const char* vrapi_GetVersionString() { return "OVRPort VrApi OpenXR experimental 1"; }
VRAPI double vrapi_GetTimeInSeconds() { return secondsNow(); }
VRAPI int vrapi_Initialize(const ovrInitParms* parms) {
    LOCK_STATE;
    if (s.instance) return -3;
    if (!parms || parms->Type != 1 || parms->GraphicsAPI != 0x40100 || !parms->Java.Vm || !parms->Java.Env || !parms->Java.ActivityObject) {
        OVP_ERROR("Initialization requires Vulkan 1.x, arm64, and a valid Android activity");
        return -1;
    }
    s.vm = parms->Java.Vm;
    s.activity = parms->Java.Env->NewGlobalRef(parms->Java.ActivityObject);
    if (!s.activity) { shutdown(); return -1; }
    s.loader = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
    if (!s.loader) { OVP_ERROR("OpenXR loader: %s", dlerror()); shutdown(); return -4; }
    s.xr.xrGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(dlsym(s.loader, "xrGetInstanceProcAddr"));
    if (!s.xr.xrGetInstanceProcAddr) { shutdown(); return -4; }
    PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
    if (XR_SUCCEEDED(s.xr.xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader))) && initializeLoader) {
        XrLoaderInitInfoAndroidKHR init{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        init.applicationVM = s.vm;
        init.applicationContext = s.activity;
        if (!xrOk(initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&init)), "xrInitializeLoaderKHR")) { shutdown(); return -4; }
    }
    if (!load("xrEnumerateInstanceExtensionProperties", reinterpret_cast<PFN_xrVoidFunction*>(&s.xr.xrEnumerateInstanceExtensionProperties)) ||
        !load("xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&s.xr.xrCreateInstance))) { shutdown(); return -4; }
    uint32_t count = 0;
    if (!xrOk(s.xr.xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "enumerate extensions")) { shutdown(); return -4; }
    std::vector<XrExtensionProperties> supported(count, {XR_TYPE_EXTENSION_PROPERTIES});
    if (!xrOk(s.xr.xrEnumerateInstanceExtensionProperties(nullptr, count, &count, supported.data()), "enumerate extensions")) { shutdown(); return -4; }
    auto has = [&](const char* name) { return std::any_of(supported.begin(), supported.end(), [&](const auto& e) { return std::strcmp(e.extensionName, name) == 0; }); };
    std::vector<const char*> extensions;
    for (const char* required : {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME}) {
        if (!has(required)) { OVP_ERROR("Required OpenXR extension missing: %s", required); shutdown(); return -4; }
        extensions.push_back(required);
    }
    s.viewConfigurationFovExtension = has(XR_EPIC_VIEW_CONFIGURATION_FOV_EXTENSION_NAME);
    if (s.viewConfigurationFovExtension) extensions.push_back(XR_EPIC_VIEW_CONFIGURATION_FOV_EXTENSION_NAME);
    s.systemDisplayRefreshRateExtension = has(SystemDisplayRefreshRateExtension);
    if (s.systemDisplayRefreshRateExtension) extensions.push_back(SystemDisplayRefreshRateExtension);
    s.displayRefreshRateExtension = has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (s.displayRefreshRateExtension) extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    for (const char* optional : {XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME, XR_FB_COLOR_SPACE_EXTENSION_NAME, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME, XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME}) if (has(optional)) extensions.push_back(optional);
    XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android.applicationVM = s.vm;
    android.applicationActivity = s.activity;
    XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO, &android};
    std::strcpy(create.applicationInfo.applicationName, "OVRPort VrApi");
    std::strcpy(create.applicationInfo.engineName, "VrApi compatibility");
    create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    create.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    create.enabledExtensionNames = extensions.data();
    if (!xrOk(s.xr.xrCreateInstance(&create, &s.instance), "xrCreateInstance")) { shutdown(); return -4; }
#define OVP_LOAD(name) if (!load(#name, reinterpret_cast<PFN_xrVoidFunction*>(&s.xr.name))) { shutdown(); return -4; }
    OVP_XR_FUNCTIONS(OVP_LOAD)
#undef OVP_LOAD
    XrSystemGetInfo system{XR_TYPE_SYSTEM_GET_INFO};
    system.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!xrOk(s.xr.xrGetSystem(s.instance, &system, &s.system), "xrGetSystem")) { shutdown(); return -4; }
    XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    if (!xrOk(s.xr.xrGetVulkanGraphicsRequirementsKHR(s.instance, s.system, &requirements), "xrGetVulkanGraphicsRequirementsKHR")) { shutdown(); return -4; }
    for (size_t eye = 0; eye < s.viewConfig.size(); ++eye) {
        s.viewConfigFov[eye] = {XR_TYPE_VIEW_CONFIGURATION_VIEW_FOV_EPIC};
        s.viewConfig[eye] = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
        if (s.viewConfigurationFovExtension) s.viewConfig[eye].next = &s.viewConfigFov[eye];
    }
    if (!xrOk(s.xr.xrEnumerateViewConfigurationViews(s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, s.viewConfig.data()), "xrEnumerateViewConfigurationViews") || count != 2) { shutdown(); return -4; }
    s.recommendedFovValid = s.viewConfigurationFovExtension &&
        symmetricFovDegrees(s.viewConfigFov[0].recommendedFov,
                            s.viewConfigFov[1].recommendedFov,
                            s.recommendedFovDegrees);
    s.colorSpaceExtension = has(XR_FB_COLOR_SPACE_EXTENSION_NAME);
    s.performanceExtension = has(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    s.threadExtension = has(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
    if (s.colorSpaceExtension) load("xrSetColorSpaceFB", reinterpret_cast<PFN_xrVoidFunction*>(&s.setColorSpace));
    if (s.performanceExtension) load("xrPerfSettingsSetPerformanceLevelEXT", reinterpret_cast<PFN_xrVoidFunction*>(&s.setPerformance));
    if (s.threadExtension) load("xrSetAndroidApplicationThreadKHR", reinterpret_cast<PFN_xrVoidFunction*>(&s.setThread));
    if (s.systemDisplayRefreshRateExtension) load("xrGetSystemDisplayRefreshRateAXRB", reinterpret_cast<PFN_xrVoidFunction*>(&s.getSystemDisplayRefreshRate));
    if (s.displayRefreshRateExtension) load("xrGetDisplayRefreshRateFB", reinterpret_cast<PFN_xrVoidFunction*>(&s.getDisplayRefreshRate));
    if (s.displayRefreshRateExtension) load("xrRequestDisplayRefreshRateFB", reinterpret_cast<PFN_xrVoidFunction*>(&s.requestDisplayRefreshRate));
    OVP_LOG("Initialized Vulkan/OpenXR adapter, stereo=%ux%u", s.viewConfig[0].recommendedImageRectWidth, s.viewConfig[0].recommendedImageRectHeight);
    return 0;
}
VRAPI void vrapi_Shutdown() { LOCK_STATE; shutdown(); }
VRAPI ovrResult vrapi_GetInstanceExtensionsVulkan(char* names, uint32_t* size) {
    LOCK_STATE;
    if (!s.instance) return NotInitialized;
    if (!size) return InvalidParameter;
    uint32_t capacity = names ? *size : 0;
    return xrOk(s.xr.xrGetVulkanInstanceExtensionsKHR(s.instance, s.system, capacity, size, names), "xrGetVulkanInstanceExtensionsKHR") ? Success : InvalidOperation;
}
VRAPI ovrResult vrapi_GetDeviceExtensionsVulkan(char* names, uint32_t* size) {
    LOCK_STATE;
    if (!s.instance) return NotInitialized;
    if (!size) return InvalidParameter;
    uint32_t capacity = names ? *size : 0;
    return xrOk(s.xr.xrGetVulkanDeviceExtensionsKHR(s.instance, s.system, capacity, size, names), "xrGetVulkanDeviceExtensionsKHR") ? Success : InvalidOperation;
}
VRAPI ovrResult vrapi_CreateSystemVulkan(ovrSystemCreateInfoVulkan* info) {
    LOCK_STATE;
    if (!s.instance) return NotInitialized;
    if (!info || !info->Instance || !info->PhysicalDevice || !info->Device) return InvalidParameter;
    if (s.vk.Device) return InvalidOperation;
    s.vk = *info;
    OVP_LOG("Application Vulkan system registered");
    return Success;
}
VRAPI void vrapi_DestroySystemVulkan() {
    LOCK_STATE;
    destroySession();
    graphics::shutdown();
    s.vk = {};
    s.queue = VK_NULL_HANDLE;
}
VRAPI ovrMobile* vrapi_EnterVrMode(const ovrModeParms* parms) {
    LOCK_STATE;
    if (!parms || !s.vk.Device || s.inVr) return nullptr;
    const auto* vk = reinterpret_cast<const ovrModeParmsVulkan*>(parms);
    if (!ensureSession(reinterpret_cast<VkQueue>(vk->SynchronizationQueue))) return nullptr;
    // Runtime READY can arrive asynchronously. Wait only for a bounded lifecycle transition.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!s.running) {
        if (!pollEvents() || std::chrono::steady_clock::now() >= deadline) { destroySession(); return nullptr; }
        if (!s.running) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    while (s.initialRecenterPending) {
        if (beginFrame(s.frameIndex + 1)) break;
        if (std::chrono::steady_clock::now() >= deadline) { destroySession(); return nullptr; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ++s.mobile.generation;
    s.inVr = true;
    OVP_LOG("Entered VR mode");
    return &s.mobile;
}
VRAPI void vrapi_LeaveVrMode(ovrMobile* mobile) {
    LOCK_STATE;
    if (validMobile(mobile)) { destroySession(); OVP_LOG("Left VR mode"); }
}
VRAPI double vrapi_GetPredictedDisplayTime(ovrMobile* mobile, int64_t index) {
    LOCK_STATE;
    if (!validMobile(mobile) || !beginFrame(index)) return 0;
    return toSeconds(s.frame.predictedDisplayTime);
}
VRAPI ovrTracking2 vrapi_GetPredictedTracking2(ovrMobile* mobile, double time) {
    LOCK_STATE;
    ovrTracking2 result{};
    result.HeadPose.Pose.Orientation.w = 1;
    if (!validMobile(mobile) || !s.timeMapped) return result;
    const auto xrTime = toXrTime(time);
    if (!locate(s.viewSpace, xrTime, result.HeadPose, result.Status)) return result;
    XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
    info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    info.displayTime = xrTime;
    info.space = s.appSpace;
    XrViewState state{XR_TYPE_VIEW_STATE};
    uint32_t count = 0;
    if (xrOk(s.xr.xrLocateViews(s.session, &info, &state, 2, &count, s.views.data()), "xrLocateViews") && count == 2) {
        for (int eye = 0; eye < 2; ++eye) {
            result.Eye[eye].ProjectionMatrix = projection(s.views[eye].fov);
            result.Eye[eye].ViewMatrix = viewMatrix(s.views[eye].pose);
        }
    }
    return result;
}
VRAPI ovrPosef vrapi_GetTrackingTransform(ovrMobile* mobile, int which) {
    LOCK_STATE;
    ovrPosef result{{0, 0, 0, 1}, {0, 0, 0}};
    if (!validMobile(mobile)) return result;
    if (which == 1) return s.trackingTransform;
    if (which == 2) return s.centerEyeTransform;
    if (which == 3 && s.stageSpace) {
        if (!s.frameBegun && !beginFrame(s.frameIndex + 1)) return result;
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (xrOk(s.xr.xrLocateSpace(s.stageSpace, s.localSpace, s.frame.predictedDisplayTime, &location), "floor transform") && (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) return fromXrPose(location.pose);
    }
    return result;
}
VRAPI void vrapi_SetTrackingTransform(ovrMobile* mobile, ovrPosef pose) {
    LOCK_STATE;
    if (!validMobile(mobile)) return;
    auto& q = pose.Orientation;
    if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w) || !std::isfinite(pose.Position.x) || !std::isfinite(pose.Position.y) || !std::isfinite(pose.Position.z)) return;
    float yaw = std::atan2(2*(q.w*q.y + q.x*q.z), 1-2*(q.x*q.x+q.y*q.y));
    q = {0, std::sin(yaw/2), 0, std::cos(yaw/2)};
    XrSpace next = XR_NULL_HANDLE;
    if (referenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, toXrPose(pose), next)) {
        s.xr.xrDestroySpace(s.appSpace);
        s.appSpace = next;
        s.trackingTransform = pose;
    }
}
VRAPI int vrapi_GetSystemPropertyInt(const ovrJava*, int property) {
    LOCK_STATE;
    if (!s.instance) return 0;
    switch (property) {
        case 0: return 259; // VRAPI_DEVICE_TYPE_OCULUSQUEST, not the 256 range-start marker.
        case 1: return 1; // Adapter swapchains are single-sampled; application MSAA resolves into them.
        case 2: return int(s.viewConfig[0].recommendedImageRectWidth * 2);
        case 3: return int(s.viewConfig[0].recommendedImageRectHeight);
        case 5: return int(s.viewConfig[0].recommendedImageRectWidth);
        case 6: return int(s.viewConfig[0].recommendedImageRectHeight);
        case 128: return 1; // Stereo array texture path.
        case 129: return 1; // sRGB VkFormats retain their color encoding.
        case 130: return 0; // No foveation image implementation is advertised.
        default: OVP_LOG("Unsupported integer system property %d", property); return 0;
    }
}
VRAPI float vrapi_GetSystemPropertyFloat(const ovrJava*, int property) {
    LOCK_STATE;
    if (property == 4) return queryDisplayRefreshRate();
    if (property == 7 || property == 8) {
        const size_t axis = static_cast<size_t>(property - 7);
        // Live optical FOV supersedes a provisional startup recommendation.
        if (s.locatedFovValid) return s.locatedFovDegrees[axis];
        if (s.recommendedFovValid) return s.recommendedFovDegrees[axis];
        return 90.0f;
    }
    OVP_LOG("Unsupported float system property %d", property);
    return 0;
}
VRAPI int vrapi_GetSystemStatusInt(const ovrJava*, int status) {
    LOCK_STATE;
    if (status == 0) return s.instance != XR_NULL_HANDLE;
    if (status == 1) return s.sessionState == XR_SESSION_STATE_FOCUSED;
    if (status == 13) return static_cast<int>(s.recenterCount);
    if (status == 14) return s.running && s.sessionState != XR_SESSION_STATE_FOCUSED;
    if (status == 130) return 1;
    return 0;
}
VRAPI void vrapi_SetPropertyInt(const ovrJava*, int property, int value) {
    // VrApi properties have no error return. Do not claim foveation or a system recenter policy.
    OVP_LOG("Optional VrApi property unsupported: %d=%d", property, value);
}
VRAPI ovrResult vrapi_SetClockLevels(ovrMobile* mobile, int32_t cpu, int32_t gpu) {
    LOCK_STATE;
    if (!validMobile(mobile)) return NotInitialized;
    if (!s.setPerformance) return Unsupported;
    const XrPerfSettingsLevelEXT levels[] = {XR_PERF_SETTINGS_LEVEL_POWER_SAVINGS_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT, XR_PERF_SETTINGS_LEVEL_BOOST_EXT};
    if (!xrOk(s.setPerformance(s.session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, levels[std::clamp(cpu, 0, 3)]), "CPU performance") ||
        !xrOk(s.setPerformance(s.session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, levels[std::clamp(gpu, 0, 3)]), "GPU performance")) return InvalidOperation;
    return Success;
}
VRAPI ovrResult vrapi_SetPerfThread(ovrMobile* mobile, int type, uint32_t tid) {
    LOCK_STATE;
    if (!validMobile(mobile)) return NotInitialized;
    if (type < 0 || type > 1 || !tid) return InvalidParameter;
    if (!s.setThread) return Unsupported;
    return xrOk(s.setThread(s.session, type == 0 ? XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR : XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, tid), "thread priority") ? Success : InvalidOperation;
}
VRAPI ovrResult vrapi_SetExtraLatencyMode(ovrMobile* mobile, int mode) {
    LOCK_STATE;
    if (!validMobile(mobile)) return NotInitialized;
    return mode == 0 ? Success : Unsupported; // OpenXR owns frame pacing; no invented extra pipeline.
}
VRAPI ovrHmdColorDesc vrapi_GetHmdColorDesc(ovrMobile*) { return {0, 0}; }
VRAPI ovrResult vrapi_SetClientColorDesc(ovrMobile* mobile, const ovrHmdColorDesc* color) {
    LOCK_STATE;
    if (!validMobile(mobile)) return NotInitialized;
    if (!color) return InvalidParameter;
    if (!s.setColorSpace) return color->ColorSpace == 0 ? Success : Unsupported;
    return xrOk(s.setColorSpace(s.session, static_cast<XrColorSpaceFB>(color->ColorSpace)), "xrSetColorSpaceFB") ? Success : InvalidParameter;
}

VRAPI ovrResult vrapi_SetTrackingSpace(ovrMobile* mobile, int32_t space) {
    // Tracking-space changes (eye/floor level) are handled by the app's own poses; accept and ignore. VrApi returns an
    // ovrResult here: a void function left the caller reading whatever was in w0.
    (void)mobile;
    OVP_LOG("vrapi_SetTrackingSpace(%d) ignored", space);
    return Success;
}
VRAPI bool vrapi_ShowSystemUI(const ovrJava* java, int32_t type) {
    (void)java;
    OVP_LOG("vrapi_ShowSystemUI(%d) unsupported", type);
    return false;
}

// Functions some engines import even when they never rely on them (e.g. BlazeRush's libtargemapp.so): a missing one
// stops the game from loading at all ("cannot locate symbol").
VRAPI ovrResult vrapi_PollEvent(ovrEventHeader* event) {
    // No VrApi events are produced: VRAPI_EVENT_NONE + ovrSuccess_EventUnavailable ends the app's polling loop.
    if (event) event->EventType = 0;
    return 1002;  // ovrSuccess_EventUnavailable
}
VRAPI void vrapi_RecenterPose(ovrMobile* mobile) {
    // Deprecated in VrApi; OpenXR has no application recenter (the runtime's own recenter is reported via status 13).
    (void)mobile;
    OVP_LOG("vrapi_RecenterPose ignored");
}
VRAPI ovrResult vrapi_SetDisplayRefreshRate(ovrMobile* mobile, float rate) {
    LOCK_STATE;
    if (!validMobile(mobile)) return NotInitialized;
    if (!s.requestDisplayRefreshRate || !s.session) return Unsupported;
    return xrOk(s.requestDisplayRefreshRate(s.session, rate), "xrRequestDisplayRefreshRateFB") ? Success : InvalidParameter;
}
VRAPI int vrapi_GetSystemPropertyFloatArray(const ovrJava*, int property, float* values, int count) {
    // The only array property is the supported refresh rates; its count property is 0 here, so report none.
    (void)values;
    OVP_LOG("Float array system property %d (%d values) unsupported", property, count);
    return 0;
}
