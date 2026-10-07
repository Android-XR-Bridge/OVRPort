// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "abi.h"
#include <EGL/egl.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <android/log.h>
#include <array>
#include <mutex>
#include <vector>
#define OVP_LOG(...) __android_log_print(ANDROID_LOG_INFO, "OVRPortVrApi", __VA_ARGS__)
#define OVP_ERROR(...) __android_log_print(ANDROID_LOG_ERROR, "OVRPortVrApi", __VA_ARGS__)
#define OVP_XR_FUNCTIONS(X) \
 X(xrEnumerateInstanceExtensionProperties) X(xrCreateInstance) X(xrDestroyInstance) \
 X(xrGetSystem) X(xrGetSystemProperties) X(xrEnumerateViewConfigurationViews) \
 X(xrGetVulkanGraphicsRequirementsKHR) X(xrGetVulkanInstanceExtensionsKHR) \
 X(xrGetVulkanDeviceExtensionsKHR) X(xrGetVulkanGraphicsDeviceKHR) \
 X(xrCreateSession) X(xrDestroySession) X(xrBeginSession) X(xrEndSession) X(xrRequestExitSession) \
 X(xrPollEvent) X(xrCreateReferenceSpace) X(xrDestroySpace) X(xrLocateSpace) \
 X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) X(xrLocateViews) \
 X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrDestroySwapchain) \
 X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) \
 X(xrStringToPath) X(xrCreateActionSet) X(xrDestroyActionSet) X(xrCreateAction) \
 X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets) X(xrCreateActionSpace) \
 X(xrSyncActions) X(xrGetActionStateBoolean) X(xrGetActionStateFloat) X(xrGetActionStateVector2f) \
 X(xrGetActionStatePose) X(xrApplyHapticFeedback) X(xrStopHapticFeedback)
namespace ovp {
struct XrApi {
    PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr = nullptr;
#define OVP_FIELD(name) PFN_##name name = nullptr;
    OVP_XR_FUNCTIONS(OVP_FIELD)
#undef OVP_FIELD
};
typedef XrResult (XRAPI_PTR *PFN_axrbGetSystemDisplayRefreshRate)(
    XrInstance instance, XrSystemId systemId, float* displayRefreshRate);
enum class DisplayRefreshRateSource : uint8_t {
    Unqueried,
    Unavailable,
    AxrbSystem,
    FbSession,
    FramePeriod,
};
struct Runtime {
    std::recursive_mutex mutex;
    XrApi xr;
    void* loader = nullptr;
    JavaVM* vm = nullptr;
    jobject activity = nullptr;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE, stageSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE, appSpace = XR_NULL_HANDLE;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    ovrMobile mobile{};
    bool inVr = false, running = false, frameBegun = false;
    int64_t frameIndex = -1;
    XrFrameState frame{XR_TYPE_FRAME_STATE};
    std::array<XrViewConfigurationView, 2> viewConfig{};
    std::array<XrViewConfigurationViewFovEPIC, 2> viewConfigFov{};
    std::array<float, 2> recommendedFovDegrees{};
    std::array<float, 2> locatedFovDegrees{};
    std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
    ovrSystemCreateInfoVulkan vk{};
    // GLES apps (no vrapi_CreateSystemVulkan): EGL objects from ovrModeParms.
    bool gles = false, glesExtension = false, vulkanExtension = false;
    EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    EGLContext eglContext = EGL_NO_CONTEXT;
    EGLConfig eglConfig = nullptr;
    PFN_xrGetOpenGLESGraphicsRequirementsKHR getGlesRequirements = nullptr;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    double timeOffset = 0;
    bool timeMapped = false, recommendedFovValid = false, locatedFovValid = false;
    bool viewConfigurationFovExtension = false;
    bool colorSpaceExtension = false, performanceExtension = false, threadExtension = false;
    bool systemDisplayRefreshRateExtension = false, displayRefreshRateExtension = false;
    PFN_xrSetColorSpaceFB setColorSpace = nullptr;
    PFN_xrPerfSettingsSetPerformanceLevelEXT setPerformance = nullptr;
    PFN_xrSetAndroidApplicationThreadKHR setThread = nullptr;
    PFN_axrbGetSystemDisplayRefreshRate getSystemDisplayRefreshRate = nullptr;
    PFN_xrGetDisplayRefreshRateFB getDisplayRefreshRate = nullptr;
    PFN_xrRequestDisplayRefreshRateFB requestDisplayRefreshRate = nullptr;
    DisplayRefreshRateSource loggedDisplayRefreshRateSource = DisplayRefreshRateSource::Unqueried;
    float loggedDisplayRefreshRate = 0;
    ovrPosef trackingTransform{{0, 0, 0, 1}, {0, 0, 0}};
    ovrPosef centerEyeTransform{{0, 0, 0, 1}, {0, 0, 0}};
    bool initialRecenterPending = false;
    uint32_t recenterCount = 0;
};
Runtime& runtime();
bool xrOk(XrResult result, const char* operation);
double secondsNow();
XrTime toXrTime(double seconds);
double toSeconds(XrTime time);
XrPosef toXrPose(const ovrPosef& pose);
ovrPosef fromXrPose(const XrPosef& pose);
bool validMobile(const ovrMobile* mobile);
bool ensureGraphicsQueue(VkQueue preferredQueue = VK_NULL_HANDLE);
bool ensureSession(VkQueue preferredQueue = VK_NULL_HANDLE);
bool pollEvents();
bool beginFrame(int64_t index);
XrResult endFrame(const XrCompositionLayerBaseHeader* const* layers, uint32_t count);
bool locate(XrSpace space, XrTime time, ovrRigidBodyPosef& pose, uint32_t& status);
namespace input {
bool initialize();
void shutdown();
void sync();
}
namespace graphics {
void shutdownSession();
void shutdown();
}
}
