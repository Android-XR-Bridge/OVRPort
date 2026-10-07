// SPDX-License-Identifier: GPL-3.0-only
#include "runtime.h"

#include <EGL/egl.h>
#include <GLES3/gl32.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>

namespace {
constexpr uint32_t kMaxBuffers = 16;
constexpr uint32_t kMaxLayers = 16;
constexpr uint32_t kMaxCopyJobs = kMaxLayers * 2;
constexpr uint32_t kMaxXrFormats = 64;
constexpr uint64_t kFrameWaitNs = 1000000000ULL;
constexpr uint64_t kShutdownWaitNs = 5000000000ULL;
constexpr uintptr_t kDefaultSwapchain = 1;
constexpr uintptr_t kLoadingSwapchain = 2;

constexpr int32_t kTexture2D = 0;
constexpr int32_t kTexture2DArray = 2;
constexpr int32_t kProjectionLayer = 1;
constexpr uint32_t kLayerChromatic = 1u << 1;
constexpr uint32_t kFrameFlush = 1u << 1;
constexpr uint32_t kLayerFixedToView = 1u << 2;
constexpr uint32_t kLayerClipRect = 1u << 4;
constexpr uint32_t kLayerInhibitSrgb = 1u << 8;
constexpr int32_t kBlendZero = 0;
constexpr int32_t kBlendOne = 1;
constexpr int32_t kBlendSrcAlpha = 2;
constexpr int32_t kBlendOneMinusSrcAlpha = 5;

struct AppImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};
}

struct ovrTextureSwapChain {
    ovrTextureSwapChain* next = nullptr;
    int32_t type = kTexture2D;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levels = 0;
    uint32_t bufferCount = 0;
    uint32_t arrayLayers = 1;
    std::array<AppImage, kMaxBuffers> appImages{};
    AppImage fullRateImage{};
    bool fullRateInitialized = false;

    XrSwapchain output = XR_NULL_HANDLE;
    uint32_t outputImageCount = 0;
    std::array<XrSwapchainImageVulkanKHR, kMaxBuffers> outputImages{};
    std::array<bool, kMaxBuffers> outputInitialized{};

    // GLES applications: the app renders into these textures; frames are copied to glOutputImages.
    bool gl = false;
    GLenum glInternalFormat = 0;
    void* glContext = nullptr;  // EGL context current when the textures were created
    std::array<GLuint, kMaxBuffers> glTextures{};
    std::array<XrSwapchainImageOpenGLESKHR, kMaxBuffers> glOutputImages{};
};

namespace {
ovrTextureSwapChain* gChains = nullptr;
ovrTextureSwapChain* gRetired = nullptr;

struct CopyContext {
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool submitted = false;
    uint32_t pendingCount = 0;
    std::array<XrSwapchain, kMaxCopyJobs> pending{};
};
CopyContext gCopy;

bool isBuiltin(const ovrTextureSwapChain* chain) {
    const uintptr_t value = reinterpret_cast<uintptr_t>(chain);
    return value == kDefaultSwapchain || value == kLoadingSwapchain;
}

ovrTextureSwapChain* findChain(const ovrTextureSwapChain* candidate) {
    for (ovrTextureSwapChain* chain = gChains; chain; chain = chain->next) {
        if (chain == candidate) return chain;
    }
    return nullptr;
}

bool supportedFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return true;
        default:
            return false;
    }
}

bool findMemoryType(VkPhysicalDevice physicalDevice, uint32_t bits,
                    VkMemoryPropertyFlags required, uint32_t& index) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) != 0 &&
            (properties.memoryTypes[i].propertyFlags & required) == required) {
            index = i;
            return true;
        }
    }
    return false;
}

void destroyAppImage(AppImage& image) {
    const VkDevice device = ovp::runtime().vk.Device;
    if (device == VK_NULL_HANDLE) return;
    if (image.image != VK_NULL_HANDLE) vkDestroyImage(device, image.image, nullptr);
    if (image.memory != VK_NULL_HANDLE) vkFreeMemory(device, image.memory, nullptr);
    image = {};
}

void destroyAppImages(ovrTextureSwapChain* chain) {
    if (chain->gl) {
        glDeleteTextures(static_cast<GLsizei>(chain->bufferCount), chain->glTextures.data());
        chain->glTextures.fill(0);
        return;
    }
    for (uint32_t i = 0; i < chain->bufferCount; ++i) destroyAppImage(chain->appImages[i]);
    destroyAppImage(chain->fullRateImage);
    chain->fullRateInitialized = false;
}

void destroyOutput(ovrTextureSwapChain* chain) {
    if (chain->output != XR_NULL_HANDLE) {
        ovp::Runtime& s = ovp::runtime();
        if (s.xr.xrDestroySwapchain) {
            ovp::xrOk(s.xr.xrDestroySwapchain(chain->output), "xrDestroySwapchain");
        }
        chain->output = XR_NULL_HANDLE;
    }
    chain->outputImageCount = 0;
    chain->outputInitialized.fill(false);
    for (auto& image : chain->outputImages) {
        image = {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR};
    }
}

void destroyChain(ovrTextureSwapChain* chain) {
    destroyOutput(chain);
    destroyAppImages(chain);
    delete chain;
}

void releasePending() {
    ovp::Runtime& s = ovp::runtime();
    for (uint32_t i = 0; i < gCopy.pendingCount; ++i) {
        if (gCopy.pending[i] != XR_NULL_HANDLE && s.xr.xrReleaseSwapchainImage) {
            const XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            ovp::xrOk(s.xr.xrReleaseSwapchainImage(gCopy.pending[i], &release),
                      "xrReleaseSwapchainImage");
        }
        gCopy.pending[i] = XR_NULL_HANDLE;
    }
    gCopy.pendingCount = 0;
    gCopy.submitted = false;
}

bool settleCopy(uint64_t timeoutNs, bool allowQueueIdle) {
    if (!gCopy.submitted) return true;
    VkResult result = vkWaitForFences(gCopy.device, 1, &gCopy.fence, VK_TRUE, timeoutNs);
    if (result == VK_TIMEOUT && allowQueueIdle) {
        result = vkQueueWaitIdle(gCopy.queue);
    }
    if (result != VK_SUCCESS) {
        OVP_ERROR("Vulkan copy completion failed: %d", static_cast<int>(result));
        return false;
    }
    releasePending();
    return true;
}

void reapRetired() {
    if (gCopy.submitted) return;
    while (gRetired) {
        ovrTextureSwapChain* chain = gRetired;
        gRetired = chain->next;
        destroyChain(chain);
    }
}

void destroyCopyContext() {
    if (gCopy.device != VK_NULL_HANDLE) {
        if (gCopy.fence != VK_NULL_HANDLE) vkDestroyFence(gCopy.device, gCopy.fence, nullptr);
        if (gCopy.pool != VK_NULL_HANDLE) vkDestroyCommandPool(gCopy.device, gCopy.pool, nullptr);
    }
    gCopy = {};
}

bool ensureCopyContext() {
    ovp::Runtime& s = ovp::runtime();
    if (!ovp::ensureGraphicsQueue()) return false;
    if (gCopy.device == s.vk.Device && gCopy.queue == s.queue &&
        gCopy.queueFamily == s.queueFamily && gCopy.command != VK_NULL_HANDLE) {
        return true;
    }
    if (gCopy.submitted && !settleCopy(kShutdownWaitNs, true)) return false;
    destroyCopyContext();

    gCopy.device = s.vk.Device;
    gCopy.queue = s.queue;
    gCopy.queueFamily = s.queueFamily;
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = s.queueFamily;
    if (vkCreateCommandPool(s.vk.Device, &poolInfo, nullptr, &gCopy.pool) != VK_SUCCESS) {
        destroyCopyContext();
        return false;
    }
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = gCopy.pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(s.vk.Device, &allocate, &gCopy.command) != VK_SUCCESS) {
        destroyCopyContext();
        return false;
    }
    const VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(s.vk.Device, &fenceInfo, nullptr, &gCopy.fence) != VK_SUCCESS) {
        destroyCopyContext();
        return false;
    }
    return true;
}

bool ensureFullRateImage(ovrTextureSwapChain* chain) {
    if (!settleCopy(kFrameWaitNs, false)) return false;
    if (chain->fullRateInitialized) return true;
    if (!ensureCopyContext()) return false;
    const auto& s = ovp::runtime();
    AppImage& resource = chain->fullRateImage;
    if (resource.image == VK_NULL_HANDLE) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(s.vk.PhysicalDevice, VK_FORMAT_R8G8_UNORM, &properties);
        const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                            VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((properties.optimalTilingFeatures & needed) != needed) return false;
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = VK_FORMAT_R8G8_UNORM;
        image.extent = {1, 1, 1};
        image.mipLevels = 1;
        image.arrayLayers = chain->arrayLayers;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(s.vk.Device, &image, nullptr, &resource.image) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(s.vk.Device, resource.image, &requirements);
        uint32_t memoryType = 0;
        if (!findMemoryType(s.vk.PhysicalDevice, requirements.memoryTypeBits,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memoryType)) {
            destroyAppImage(resource);
            return false;
        }
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(s.vk.Device, &allocate, nullptr, &resource.memory) != VK_SUCCESS ||
            vkBindImageMemory(s.vk.Device, resource.image, resource.memory, 0) != VK_SUCCESS) {
            destroyAppImage(resource);
            return false;
        }
    }

    if (vkResetCommandBuffer(gCopy.command, 0) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(gCopy.command, &begin) != VK_SUCCESS) return false;
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = resource.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, chain->arrayLayers};
    vkCmdPipelineBarrier(gCopy.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    // Constant (1,1) density means full-rate shading. This ordinary sampled
    // texture supports consumers which query the resource even with VRS off;
    // it is not a hardware fragment-density attachment or an enabled feature.
    const VkClearColorValue fullRate{{1.0f, 1.0f, 1.0f, 1.0f}};
    vkCmdClearColorImage(gCopy.command, resource.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        &fullRate, 1, &barrier.subresourceRange);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(gCopy.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    if (vkEndCommandBuffer(gCopy.command) != VK_SUCCESS ||
        vkResetFences(gCopy.device, 1, &gCopy.fence) != VK_SUCCESS) return false;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &gCopy.command;
    if (vkQueueSubmit(gCopy.queue, 1, &submit, gCopy.fence) != VK_SUCCESS) return false;
    gCopy.submitted = true;
    gCopy.pendingCount = 0;
    // Keep ownership if the wait times out; retirement already waits for this
    // fence, and a later query cannot publish the image until it has completed.
    chain->fullRateInitialized = true;
    if (!settleCopy(kFrameWaitNs, false)) return false;
    OVP_LOG("Created full-rate compatibility texture (%u layers); hardware foveation disabled",
            chain->arrayLayers);
    return true;
}

bool ensureOutputGl(ovrTextureSwapChain* chain) {
    ovp::Runtime& s = ovp::runtime();
    uint32_t formatCount = 0;
    std::array<int64_t, kMaxXrFormats> formats{};
    if (XR_FAILED(s.xr.xrEnumerateSwapchainFormats(s.session, 0, &formatCount, nullptr)) || !formatCount ||
        formatCount > kMaxXrFormats ||
        XR_FAILED(s.xr.xrEnumerateSwapchainFormats(s.session, formatCount, &formatCount, formats.data()))) {
        OVP_ERROR("xrEnumerateSwapchainFormats(GLES) failed");
        return false;
    }
    auto exposed = [&](int64_t f) { return std::find(formats.begin(), formats.begin() + formatCount, f) != formats.begin() + formatCount; };
    int64_t wanted = chain->glInternalFormat;
    if (!exposed(wanted)) {
        OVP_ERROR("OpenXR runtime does not expose GL format 0x%llx", (long long)wanted);
        return false;
    }
    XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    create.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    create.format = wanted;
    create.sampleCount = 1;
    create.width = chain->width;
    create.height = chain->height;
    create.faceCount = 1;
    create.arraySize = chain->arrayLayers;
    create.mipCount = 1;
    if (!ovp::xrOk(s.xr.xrCreateSwapchain(s.session, &create, &chain->output), "xrCreateSwapchain(GLES)")) {
        chain->output = XR_NULL_HANDLE;
        return false;
    }
    uint32_t count = 0;
    if (XR_FAILED(s.xr.xrEnumerateSwapchainImages(chain->output, 0, &count, nullptr)) || !count || count > kMaxBuffers) {
        destroyOutput(chain);
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) chain->glOutputImages[i] = {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR};
    if (XR_FAILED(s.xr.xrEnumerateSwapchainImages(chain->output, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(chain->glOutputImages.data())))) {
        destroyOutput(chain);
        return false;
    }
    chain->outputImageCount = count;
    OVP_LOG("Created GLES output swapchain %ux%u x%u format 0x%llx", chain->width, chain->height,
            chain->arrayLayers, (long long)wanted);
    return true;
}

bool ensureOutput(ovrTextureSwapChain* chain) {
    if (chain->output != XR_NULL_HANDLE) return true;
    ovp::Runtime& s = ovp::runtime();
    if (s.session == XR_NULL_HANDLE) return false;
    if (chain->gl) return ensureOutputGl(chain);

    uint32_t formatCount = 0;
    XrResult result = s.xr.xrEnumerateSwapchainFormats(s.session, 0, &formatCount, nullptr);
    if (XR_FAILED(result) || formatCount == 0 || formatCount > kMaxXrFormats) {
        ovp::xrOk(result, "xrEnumerateSwapchainFormats(count)");
        return false;
    }
    std::array<int64_t, kMaxXrFormats> formats{};
    result = s.xr.xrEnumerateSwapchainFormats(s.session, formatCount, &formatCount,
                                               formats.data());
    if (XR_FAILED(result)) {
        ovp::xrOk(result, "xrEnumerateSwapchainFormats");
        return false;
    }
    const int64_t wanted = static_cast<int64_t>(chain->format);
    if (std::find(formats.begin(), formats.begin() + formatCount, wanted) ==
        formats.begin() + formatCount) {
        OVP_ERROR("OpenXR runtime does not expose requested VkFormat %lld",
                  static_cast<long long>(wanted));
        return false;
    }

    XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    create.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                        XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    create.format = wanted;
    create.sampleCount = 1;
    create.width = chain->width;
    create.height = chain->height;
    create.faceCount = 1;
    create.arraySize = chain->arrayLayers;
    create.mipCount = chain->levels;
    result = s.xr.xrCreateSwapchain(s.session, &create, &chain->output);
    if (XR_FAILED(result)) {
        ovp::xrOk(result, "xrCreateSwapchain");
        chain->output = XR_NULL_HANDLE;
        return false;
    }

    uint32_t imageCount = 0;
    result = s.xr.xrEnumerateSwapchainImages(chain->output, 0, &imageCount, nullptr);
    if (XR_FAILED(result) || imageCount == 0 || imageCount > kMaxBuffers) {
        ovp::xrOk(result, "xrEnumerateSwapchainImages(count)");
        destroyOutput(chain);
        return false;
    }
    for (uint32_t i = 0; i < imageCount; ++i) {
        chain->outputImages[i] = {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR};
    }
    result = s.xr.xrEnumerateSwapchainImages(
        chain->output, imageCount, &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(chain->outputImages.data()));
    if (XR_FAILED(result)) {
        ovp::xrOk(result, "xrEnumerateSwapchainImages");
        destroyOutput(chain);
        return false;
    }
    chain->outputImageCount = imageCount;
    return true;
}

bool finite(float value) { return std::isfinite(value); }

bool allColor(const ovrVector4f& color, float value) {
    return color.x == value && color.y == value && color.z == value && color.w == value;
}

XrQuaternionf normalize(XrQuaternionf q) {
    const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(length > 0.000001f) || !finite(length)) return {0, 0, 0, 0};
    const float inverse = 1.0f / length;
    return {q.x * inverse, q.y * inverse, q.z * inverse, q.w * inverse};
}

XrQuaternionf conjugate(const XrQuaternionf& q) { return {-q.x, -q.y, -q.z, q.w}; }

XrQuaternionf multiply(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

XrVector3f rotate(const XrQuaternionf& q, const XrVector3f& v) {
    const XrQuaternionf p{v.x, v.y, v.z, 0};
    const XrQuaternionf r = multiply(multiply(q, p), conjugate(q));
    return {r.x, r.y, r.z};
}

bool eyePose(const ovrPosef& headPose, uint32_t eye, XrPosef& result) {
    const ovp::Runtime& s = ovp::runtime();
    if (!finite(headPose.Position.x) || !finite(headPose.Position.y) ||
        !finite(headPose.Position.z)) return false;
    XrQuaternionf head = normalize(ovp::toXrPose(headPose).orientation);
    XrQuaternionf q0 = normalize(s.views[0].pose.orientation);
    XrQuaternionf q1 = normalize(s.views[1].pose.orientation);
    if (head.w == 0 && head.x == 0 && head.y == 0 && head.z == 0) return false;
    if (q0.w == 0 && q0.x == 0 && q0.y == 0 && q0.z == 0) return false;
    if (q1.w == 0 && q1.x == 0 && q1.y == 0 && q1.z == 0) return false;
    if (q0.x * q1.x + q0.y * q1.y + q0.z * q1.z + q0.w * q1.w < 0) {
        q1 = {-q1.x, -q1.y, -q1.z, -q1.w};
    }
    const XrQuaternionf center = normalize(
        {q0.x + q1.x, q0.y + q1.y, q0.z + q1.z, q0.w + q1.w});
    if (center.w == 0 && center.x == 0 && center.y == 0 && center.z == 0) return false;
    const XrVector3f centerPosition{
        (s.views[0].pose.position.x + s.views[1].pose.position.x) * 0.5f,
        (s.views[0].pose.position.y + s.views[1].pose.position.y) * 0.5f,
        (s.views[0].pose.position.z + s.views[1].pose.position.z) * 0.5f,
    };
    const XrVector3f worldOffset{
        s.views[eye].pose.position.x - centerPosition.x,
        s.views[eye].pose.position.y - centerPosition.y,
        s.views[eye].pose.position.z - centerPosition.z,
    };
    const XrVector3f localOffset = rotate(conjugate(center), worldOffset);
    const XrVector3f offset = rotate(head, localOffset);
    const XrQuaternionf localOrientation = multiply(conjugate(center),
                                                     eye == 0 ? q0 : q1);
    result.orientation = normalize(multiply(head, localOrientation));
    result.position = {headPose.Position.x + offset.x,
                       headPose.Position.y + offset.y,
                       headPose.Position.z + offset.z};
    return true;
}

bool projectionGeometry(const ovrMatrix4f& matrix, const ovrRectf& rect,
                        uint32_t width, uint32_t height, XrFovf& fov,
                        XrRect2Di& imageRect) {
    const float sx = matrix.M[0][0];
    const float sy = matrix.M[1][1];
    const float tx = matrix.M[0][2];
    const float ty = matrix.M[1][2];
    if (!finite(sx) || !finite(sy) || !finite(tx) || !finite(ty) ||
        sx <= 0.000001f || std::fabs(sy) <= 0.000001f ||
        std::fabs(matrix.M[0][1]) > 0.000001f ||
        std::fabs(matrix.M[1][0]) > 0.000001f ||
        !finite(rect.x) || !finite(rect.y) || !finite(rect.width) ||
        !finite(rect.height) || rect.width <= 0 || rect.height <= 0 ||
        rect.x < 0 || rect.y < 0 || rect.x + rect.width > 1.00001f ||
        rect.y + rect.height > 1.00001f) {
        return false;
    }
    const float x0 = (rect.x + tx) / sx;
    const float x1 = (rect.x + rect.width + tx) / sx;
    const float y0 = (rect.y + ty) / sy;
    const float y1 = (rect.y + rect.height + ty) / sy;
    fov.angleLeft = std::atan(std::min(x0, x1));
    fov.angleRight = std::atan(std::max(x0, x1));
    // Vulkan images start at the top-left. Preserve the tangent direction at
    // each image edge: OpenXR reverses the image when angleDown > angleUp.
    fov.angleUp = std::atan(y0);
    fov.angleDown = std::atan(y1);

    const int32_t left = std::max(0, static_cast<int32_t>(std::floor(rect.x * width)));
    const int32_t top = std::max(0, static_cast<int32_t>(std::floor(rect.y * height)));
    const int32_t right = std::min(static_cast<int32_t>(width),
        static_cast<int32_t>(std::ceil((rect.x + rect.width) * width)));
    const int32_t bottom = std::min(static_cast<int32_t>(height),
        static_cast<int32_t>(std::ceil((rect.y + rect.height) * height)));
    if (right <= left || bottom <= top) return false;
    imageRect.offset = {left, top};
    imageRect.extent = {right - left, bottom - top};
    return true;
}

struct PreparedLayer {
    const ovrLayerProjection2* source = nullptr;
    bool black = false;
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::array<XrCompositionLayerProjectionView, 2> views{{
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
    }};
};

struct CopyJob {
    ovrTextureSwapChain* chain = nullptr;
    uint32_t sourceIndex = 0;
    uint32_t outputIndex = 0;
    bool acquired = false;
};

ovrResult failFrame(ovrResult error) {
    ovp::Runtime& s = ovp::runtime();
    if (s.frameBegun) {
        const XrResult end = ovp::endFrame(nullptr, 0);
        if (XR_FAILED(end)) return DeviceUnavailable;
    }
    return error;
}

void releaseJobs(std::array<CopyJob, kMaxCopyJobs>& jobs, uint32_t count) {
    ovp::Runtime& s = ovp::runtime();
    for (uint32_t i = 0; i < count; ++i) {
        if (!jobs[i].acquired) continue;
        const XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        ovp::xrOk(s.xr.xrReleaseSwapchainImage(jobs[i].chain->output, &release),
                  "xrReleaseSwapchainImage(error cleanup)");
        jobs[i].acquired = false;
    }
}

bool acquireJobs(std::array<CopyJob, kMaxCopyJobs>& jobs, uint32_t count) {
    ovp::Runtime& s = ovp::runtime();
    for (uint32_t i = 0; i < count; ++i) {
        const XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrResult result = s.xr.xrAcquireSwapchainImage(jobs[i].chain->output, &acquire,
                                                        &jobs[i].outputIndex);
        if (result != XR_SUCCESS ||
            jobs[i].outputIndex >= jobs[i].chain->outputImageCount) {
            ovp::xrOk(result, "xrAcquireSwapchainImage");
            releaseJobs(jobs, i);
            return false;
        }
        jobs[i].acquired = true;
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = static_cast<XrDuration>(kFrameWaitNs);
        result = s.xr.xrWaitSwapchainImage(jobs[i].chain->output, &wait);
        if (result != XR_SUCCESS) {
            ovp::xrOk(result, "xrWaitSwapchainImage");
            releaseJobs(jobs, i + 1);
            return false;
        }
    }
    return true;
}

bool submitCopiesGl(std::array<CopyJob, kMaxCopyJobs>& jobs, uint32_t count) {
    ovp::Runtime& s = ovp::runtime();
    if (count == 0) return true;
    if (!acquireJobs(jobs, count)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        ovrTextureSwapChain* chain = jobs[i].chain;
        const GLuint source = chain->glTextures[jobs[i].sourceIndex];
        const GLuint destination = chain->glOutputImages[jobs[i].outputIndex].image;
        const GLenum target = chain->arrayLayers > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
        while (glGetError() != GL_NO_ERROR) {}  // don't blame the copy for errors the app left behind
        glCopyImageSubData(source, target, 0, 0, 0, 0, destination, target, 0, 0, 0, 0,
                           static_cast<GLsizei>(chain->width), static_cast<GLsizei>(chain->height),
                           static_cast<GLsizei>(chain->arrayLayers));
        const GLenum error = glGetError();
        static int errors;
        if (error != GL_NO_ERROR && errors++ < 5) OVP_ERROR("glCopyImageSubData failed: 0x%x", error);
    }
    glFlush();
    for (uint32_t i = 0; i < count; ++i) {
        const XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        ovp::xrOk(s.xr.xrReleaseSwapchainImage(jobs[i].chain->output, &release), "xrReleaseSwapchainImage(GLES)");
        jobs[i].acquired = false;
    }
    return true;
}

bool submitCopies(std::array<CopyJob, kMaxCopyJobs>& jobs, uint32_t count) {
    if (ovp::runtime().gles) return submitCopiesGl(jobs, count);
    if (!settleCopy(kFrameWaitNs, false)) return false;
    reapRetired();
    if (count == 0) return true;
    if (!ensureCopyContext()) return false;
    if (!acquireJobs(jobs, count)) return false;

    if (vkResetCommandBuffer(gCopy.command, 0) != VK_SUCCESS) {
        releaseJobs(jobs, count);
        return false;
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(gCopy.command, &begin) != VK_SUCCESS) {
        releaseJobs(jobs, count);
        return false;
    }

    for (uint32_t i = 0; i < count; ++i) {
        ovrTextureSwapChain* chain = jobs[i].chain;
        const VkImage source = chain->appImages[jobs[i].sourceIndex].image;
        const VkImage destination = chain->outputImages[jobs[i].outputIndex].image;
        const bool initialized = chain->outputInitialized[jobs[i].outputIndex];

        std::array<VkImageMemoryBarrier, 2> before{};
        before[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        before[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        before[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        before[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        before[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        before[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before[0].image = source;
        before[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, chain->levels,
                                      0, chain->arrayLayers};
        before[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        before[1].srcAccessMask = initialized ?
            (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT) : 0;
        before[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        before[1].oldLayout = initialized ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                                            VK_IMAGE_LAYOUT_UNDEFINED;
        before[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        before[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before[1].image = destination;
        before[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, chain->levels,
                                      0, chain->arrayLayers};
        vkCmdPipelineBarrier(gCopy.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
            static_cast<uint32_t>(before.size()), before.data());

        for (uint32_t level = 0; level < chain->levels; ++level) {
            VkImageCopy copy{};
            copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, chain->arrayLayers};
            copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, chain->arrayLayers};
            copy.extent = {std::max(1u, chain->width >> level),
                           std::max(1u, chain->height >> level), 1};
            vkCmdCopyImage(gCopy.command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        }

        std::array<VkImageMemoryBarrier, 2> after{};
        after[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        after[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        after[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        after[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        after[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        after[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        after[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        after[0].image = source;
        after[0].subresourceRange = before[0].subresourceRange;
        after[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        after[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        after[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        after[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        after[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        after[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        after[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        after[1].image = destination;
        after[1].subresourceRange = before[1].subresourceRange;
        vkCmdPipelineBarrier(gCopy.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
            static_cast<uint32_t>(after.size()), after.data());
    }

    if (vkEndCommandBuffer(gCopy.command) != VK_SUCCESS ||
        vkResetFences(gCopy.device, 1, &gCopy.fence) != VK_SUCCESS) {
        releaseJobs(jobs, count);
        return false;
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &gCopy.command;
    const VkResult submitted = vkQueueSubmit(gCopy.queue, 1, &submit, gCopy.fence);
    if (submitted != VK_SUCCESS) {
        OVP_ERROR("vkQueueSubmit failed: %d", static_cast<int>(submitted));
        releaseJobs(jobs, count);
        return false;
    }
    gCopy.submitted = true;
    gCopy.pendingCount = count;
    for (uint32_t i = 0; i < count; ++i) {
        gCopy.pending[i] = jobs[i].chain->output;
        jobs[i].chain->outputInitialized[jobs[i].outputIndex] = true;
        jobs[i].acquired = false;
    }
    return settleCopy(kFrameWaitNs, false);
}
} // namespace

VRAPI ovrTextureSwapChain* vrapi_CreateTextureSwapChain3(int32_t type, int64_t format,
                                                          int width, int height,
                                                          int levels, int bufferCount) {
    ovp::Runtime& s = ovp::runtime();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    if (s.vk.Device == VK_NULL_HANDLE || s.vk.PhysicalDevice == VK_NULL_HANDLE ||
        (type != kTexture2D && type != kTexture2DArray) || width <= 0 || height <= 0 ||
        width > 16384 || height > 16384 || levels <= 0 || bufferCount <= 0 ||
        bufferCount > static_cast<int>(kMaxBuffers) ||
        format < std::numeric_limits<int32_t>::min() ||
        format > std::numeric_limits<int32_t>::max()) {
        OVP_ERROR("Invalid Vulkan texture swapchain parameters");
        return nullptr;
    }
    const uint32_t maxDimension = static_cast<uint32_t>(std::max(width, height));
    uint32_t maxLevels = 1;
    for (uint32_t dimension = maxDimension; dimension > 1; dimension >>= 1) ++maxLevels;
    const VkFormat vkFormat = static_cast<VkFormat>(format);
    if (levels > static_cast<int>(maxLevels) || !supportedFormat(vkFormat)) {
        OVP_ERROR("Unsupported Vulkan texture swapchain type/format/mip count");
        return nullptr;
    }
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(s.vk.PhysicalDevice, vkFormat, &properties);
    const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                        VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((properties.optimalTilingFeatures & needed) != needed) {
        OVP_ERROR("VkFormat %d lacks required attachment, sampling, or transfer support",
                  static_cast<int>(vkFormat));
        return nullptr;
    }

    ovrTextureSwapChain* chain = new (std::nothrow) ovrTextureSwapChain();
    if (!chain) return nullptr;
    chain->type = type;
    chain->format = vkFormat;
    chain->width = static_cast<uint32_t>(width);
    chain->height = static_cast<uint32_t>(height);
    chain->levels = static_cast<uint32_t>(levels);
    chain->bufferCount = static_cast<uint32_t>(bufferCount);
    chain->arrayLayers = type == kTexture2DArray ? 2u : 1u;

    for (uint32_t i = 0; i < chain->bufferCount; ++i) {
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = chain->format;
        image.extent = {chain->width, chain->height, 1};
        image.mipLevels = chain->levels;
        image.arrayLayers = chain->arrayLayers;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(s.vk.Device, &image, nullptr, &chain->appImages[i].image) !=
            VK_SUCCESS) {
            destroyAppImages(chain);
            delete chain;
            return nullptr;
        }
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(s.vk.Device, chain->appImages[i].image, &requirements);
        uint32_t memoryType = 0;
        if (!findMemoryType(s.vk.PhysicalDevice, requirements.memoryTypeBits,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memoryType)) {
            destroyAppImages(chain);
            delete chain;
            return nullptr;
        }
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(s.vk.Device, &allocate, nullptr,
                             &chain->appImages[i].memory) != VK_SUCCESS ||
            vkBindImageMemory(s.vk.Device, chain->appImages[i].image,
                              chain->appImages[i].memory, 0) != VK_SUCCESS) {
            destroyAppImages(chain);
            delete chain;
            return nullptr;
        }
    }
    chain->next = gChains;
    gChains = chain;
    reapRetired();
    return chain;
}

VRAPI int vrapi_GetTextureSwapChainLength(ovrTextureSwapChain* candidate) {
    ovp::Runtime& s = ovp::runtime();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    ovrTextureSwapChain* chain = findChain(candidate);
    return chain ? static_cast<int>(chain->bufferCount) : 0;
}

VRAPI VkImage vrapi_GetTextureSwapChainBufferVulkan(ovrTextureSwapChain* candidate,
                                                     int index) {
    ovp::Runtime& s = ovp::runtime();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    ovrTextureSwapChain* chain = findChain(candidate);
    if (!chain || index < 0 || index >= static_cast<int>(chain->bufferCount)) {
        return VK_NULL_HANDLE;
    }
    return chain->appImages[static_cast<uint32_t>(index)].image;
}

VRAPI ovrResult vrapi_GetTextureSwapChainBufferFoveationVulkan(
    ovrTextureSwapChain* candidate, int index, VkImage* image,
    uint32_t* imageWidth, uint32_t* imageHeight) {
    ovp::Runtime& s = ovp::runtime();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    if (image) *image = VK_NULL_HANDLE;
    if (imageWidth) *imageWidth = 0;
    if (imageHeight) *imageHeight = 0;
    ovrTextureSwapChain* chain = findChain(candidate);
    if (!chain || index < 0 || index >= static_cast<int>(chain->bufferCount) ||
        !image || !imageWidth || !imageHeight) {
        return InvalidParameter;
    }
    if (!ensureFullRateImage(chain)) {
        OVP_ERROR("Failed to initialize full-rate compatibility texture");
        return InvalidOperation;
    }
    *image = chain->fullRateImage.image;
    *imageWidth = *imageHeight = 1;
    return Success;
}

VRAPI void vrapi_DestroyTextureSwapChain(ovrTextureSwapChain* candidate) {
    ovp::Runtime& s = ovp::runtime();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    ovrTextureSwapChain** link = &gChains;
    while (*link && *link != candidate) link = &(*link)->next;
    if (!*link) {
        if (candidate && !isBuiltin(candidate)) OVP_ERROR("Destroying unknown texture swapchain");
        return;
    }
    ovrTextureSwapChain* chain = *link;
    *link = chain->next;
    if (gCopy.submitted && !settleCopy(kFrameWaitNs, false)) {
        chain->next = gRetired;
        gRetired = chain;
        return;
    }
    reapRetired();
    destroyChain(chain);
}

VRAPI ovrResult vrapi_SubmitFrame2(ovrMobile* mobile,
                                    const ovrSubmitFrameDescription2* description) {
    ovp::Runtime& s = ovp::runtime();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    if (!ovp::validMobile(mobile) || !description ||
        description->LayerCount > kMaxLayers ||
        (description->LayerCount != 0 && !description->Layers)) {
        return failFrame(InvalidParameter);
    }
    if ((description->Flags & ~kFrameFlush) != 0 || description->SwapInterval != 1) {
        OVP_ERROR("Unsupported frame flags or swap interval");
        return failFrame(Unsupported);
    }

    std::array<PreparedLayer, kMaxLayers> prepared{};
    for (uint32_t i = 0; i < description->LayerCount; ++i) {
        const ovrLayerHeader2* header = description->Layers[i];
        if (!header) return failFrame(InvalidParameter);
        if (header->Type != kProjectionLayer) {
            OVP_ERROR("Unsupported VrApi layer type %d", header->Type);
            return failFrame(Unsupported);
        }
        if (header->Reserved ||
            (header->Flags & ~(kLayerChromatic | kLayerFixedToView |
                               kLayerClipRect | kLayerInhibitSrgb)) != 0) {
            OVP_ERROR("Unsupported projection layer flags=0x%x reserved=%p",
                      header->Flags, header->Reserved);
            return failFrame(Unsupported);
        }
        PreparedLayer& output = prepared[i];
        output.source = reinterpret_cast<const ovrLayerProjection2*>(header);
        const bool black = allColor(header->ColorScale, 0.0f);
        const bool normalColor = allColor(header->ColorScale, 1.0f);
        if (!black && !normalColor) {
            OVP_ERROR("Projection color scaling is unsupported");
            return failFrame(Unsupported);
        }
        if (header->SrcBlend == kBlendOne && header->DstBlend == kBlendZero) {
            output.projection.layerFlags = 0;
        } else if (header->SrcBlend == kBlendSrcAlpha &&
                   header->DstBlend == kBlendOneMinusSrcAlpha) {
            output.projection.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                           XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        } else if (header->SrcBlend == kBlendOne &&
                   header->DstBlend == kBlendOneMinusSrcAlpha) {
            output.projection.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        } else {
            OVP_ERROR("Unsupported projection blend function");
            return failFrame(Unsupported);
        }

        bool allDefault = true;
        for (uint32_t eye = 0; eye < 2; ++eye) {
            const ovrTextureSwapChain* candidate = output.source->Textures[eye].ColorSwapChain;
            if (reinterpret_cast<uintptr_t>(candidate) == kLoadingSwapchain) {
                OVP_ERROR("Built-in loading-icon swapchain has no OpenXR equivalent");
                return failFrame(Unsupported);
            }
            if (reinterpret_cast<uintptr_t>(candidate) == kDefaultSwapchain) continue;
            allDefault = false;
            ovrTextureSwapChain* chain = findChain(candidate);
            const int index = output.source->Textures[eye].SwapChainIndex;
            if (!chain || index < 0 || index >= static_cast<int>(chain->bufferCount)) {
                return failFrame(InvalidParameter);
            }
        }
        if (allDefault) {
            if (!black) {
                OVP_ERROR("Default swapchain is only supported for an explicit black layer");
                return failFrame(Unsupported);
            }
            output.black = true;
        } else if (black) {
            OVP_ERROR("Color-scaled application swapchains are unsupported");
            return failFrame(Unsupported);
        } else {
            for (uint32_t eye = 0; eye < 2; ++eye) {
                if (isBuiltin(output.source->Textures[eye].ColorSwapChain)) {
                    OVP_ERROR("Mixed built-in and application projection textures unsupported");
                    return failFrame(Unsupported);
                }
            }
        }
    }

    if (!ovp::ensureSession(s.queue) || !ovp::beginFrame(static_cast<int64_t>(description->FrameIndex))) {
        return failFrame(DeviceUnavailable);
    }
    if (!s.frame.shouldRender) {
        const XrResult result = ovp::endFrame(nullptr, 0);
        return XR_SUCCEEDED(result) ? Success : DeviceUnavailable;
    }

    std::array<CopyJob, kMaxCopyJobs> jobs{};
    uint32_t jobCount = 0;
    std::array<const XrCompositionLayerBaseHeader*, kMaxLayers> xrLayers{};
    uint32_t xrLayerCount = 0;
    std::array<XrView, 2> headLockedViews{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
    bool haveHeadLockedViews = false;
    for (uint32_t i = 0; i < description->LayerCount; ++i) {
        PreparedLayer& output = prepared[i];
        if (output.black) continue;
        const bool headLocked = (output.source->Header.Flags & kLayerFixedToView) != 0;
        if (headLocked && !haveHeadLockedViews) {
            XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
            locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locate.displayTime = s.frame.predictedDisplayTime;
            locate.space = s.viewSpace;
            XrViewState state{XR_TYPE_VIEW_STATE};
            uint32_t count = 0;
            constexpr XrViewStateFlags validPose =
                XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
            if (!ovp::xrOk(s.xr.xrLocateViews(s.session, &locate, &state, 2, &count,
                                            headLockedViews.data()), "xrLocateViews(VIEW)") ||
                count != 2 || (state.viewStateFlags & validPose) != validPose) {
                return failFrame(DeviceUnavailable);
            }
            haveHeadLockedViews = true;
        }
        output.projection.space = headLocked ? s.viewSpace : s.appSpace;
        output.projection.viewCount = 2;
        output.projection.views = output.views.data();
        for (uint32_t eye = 0; eye < 2; ++eye) {
            const auto& sourceTexture = output.source->Textures[eye];
            ovrTextureSwapChain* chain = findChain(sourceTexture.ColorSwapChain);
            if (!chain || !ensureOutput(chain)) return failFrame(DeviceUnavailable);
            // Fixed-to-view images are not timewarped using the application's
            // render HeadPose. Keep the runtime's actual eye offsets/cant in VIEW.
            if (headLocked) output.views[eye].pose = headLockedViews[eye].pose;
            if ((!headLocked && !eyePose(output.source->HeadPose.Pose, eye, output.views[eye].pose)) ||
                !projectionGeometry(sourceTexture.TexCoordsFromTanAngles,
                                    sourceTexture.TextureRect, chain->width, chain->height,
                                    output.views[eye].fov,
                                    output.views[eye].subImage.imageRect)) {
                const auto& matrix = sourceTexture.TexCoordsFromTanAngles;
                const auto& rect = sourceTexture.TextureRect;
                OVP_ERROR("Invalid projection geometry or pose: eye=%u flags=0x%x "
                          "scale=(%g,%g) shear=(%g,%g) offset=(%g,%g) rect=(%g,%g,%g,%g)",
                          eye, output.source->Header.Flags, matrix.M[0][0], matrix.M[1][1],
                          matrix.M[0][1], matrix.M[1][0], matrix.M[0][2], matrix.M[1][2],
                          rect.x, rect.y, rect.width, rect.height);
                return failFrame(InvalidParameter);
            }
            if (chain->gl) {  // GL images start at the bottom row: reverse the vertical tangent mapping
                std::swap(output.views[eye].fov.angleUp, output.views[eye].fov.angleDown);
            }
            output.views[eye].subImage.swapchain = chain->output;
            output.views[eye].subImage.imageArrayIndex =
                chain->type == kTexture2DArray ? eye : 0;

            uint32_t job = 0;
            for (; job < jobCount; ++job) {
                if (jobs[job].chain != chain) continue;
                if (jobs[job].sourceIndex != static_cast<uint32_t>(sourceTexture.SwapChainIndex)) {
                    OVP_ERROR("One XR swapchain cannot present multiple app indices in one frame");
                    return failFrame(Unsupported);
                }
                break;
            }
            if (job == jobCount) {
                if (jobCount == kMaxCopyJobs) return failFrame(Unsupported);
                jobs[jobCount].chain = chain;
                jobs[jobCount].sourceIndex =
                    static_cast<uint32_t>(sourceTexture.SwapChainIndex);
                ++jobCount;
            }
        }
        xrLayers[xrLayerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(
            &output.projection);
    }

    if (!submitCopies(jobs, jobCount)) return failFrame(DeviceUnavailable);
    const XrResult result = ovp::endFrame(xrLayers.data(), xrLayerCount);
    return XR_SUCCEEDED(result) ? Success : DeviceUnavailable;
}

namespace ovp::graphics {
void shutdownSession() {
    Runtime& s = runtime();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    settleCopy(kShutdownWaitNs, true);
    for (ovrTextureSwapChain* chain = gChains; chain; chain = chain->next) destroyOutput(chain);
    for (ovrTextureSwapChain* chain = gRetired; chain; chain = chain->next) destroyOutput(chain);
}

void shutdown() {
    Runtime& s = runtime();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    settleCopy(kShutdownWaitNs, true);
    while (gChains) {
        ovrTextureSwapChain* chain = gChains;
        gChains = chain->next;
        destroyChain(chain);
    }
    while (gRetired) {
        ovrTextureSwapChain* chain = gRetired;
        gRetired = chain->next;
        destroyChain(chain);
    }
    destroyCopyContext();
}
} // namespace ovp::graphics

// ---------------------------------------------------------------- GLES texture swapchains
namespace {
constexpr int64_t kFormat8888 = 4, kFormat8888Srgb = 5, kFormatRgba16f = 6;
}

// format: VrApi's 32-bit ovrTextureFormat enum (only vrapi_CreateTextureSwapChain3 takes a 64-bit format)
VRAPI ovrTextureSwapChain* vrapi_CreateTextureSwapChain2(int32_t type, int32_t format, int width, int height,
                                                          int levels, int bufferCount) {
    ovp::Runtime& s = ovp::runtime();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    GLenum internal = 0;
    if (format == kFormat8888) internal = GL_RGBA8;
    else if (format == kFormat8888Srgb) internal = GL_SRGB8_ALPHA8;
    else if (format == kFormatRgba16f) internal = GL_RGBA16F;
    if (!internal || (type != kTexture2D && type != kTexture2DArray) || width <= 0 || height <= 0 ||
        width > 16384 || height > 16384 || levels <= 0 || bufferCount <= 0 ||
        bufferCount > static_cast<int>(kMaxBuffers)) {
        OVP_ERROR("Unsupported GLES texture swapchain type=%d format=%lld %dx%d levels=%d buffers=%d", type,
                  (long long)format, width, height, levels, bufferCount);
        return nullptr;
    }
    ovrTextureSwapChain* chain = new (std::nothrow) ovrTextureSwapChain();
    if (!chain) return nullptr;
    chain->gl = true;
    chain->glInternalFormat = internal;
    chain->glContext = eglGetCurrentContext();
    chain->type = type;
    chain->width = static_cast<uint32_t>(width);
    chain->height = static_cast<uint32_t>(height);
    chain->levels = 1;
    chain->bufferCount = static_cast<uint32_t>(bufferCount);
    chain->arrayLayers = type == kTexture2DArray ? 2u : 1u;
    const GLenum target = chain->arrayLayers > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
    glGenTextures(static_cast<GLsizei>(chain->bufferCount), chain->glTextures.data());
    for (uint32_t i = 0; i < chain->bufferCount; ++i) {
        glBindTexture(target, chain->glTextures[i]);
        if (target == GL_TEXTURE_2D_ARRAY) glTexStorage3D(target, 1, internal, width, height, 2);
        else glTexStorage2D(target, 1, internal, width, height);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(target, 0);
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        OVP_ERROR("GLES texture swapchain creation failed: 0x%x (is a GL context current?)", error);
        destroyChain(chain);
        return nullptr;
    }
    chain->next = gChains;
    gChains = chain;
    OVP_LOG("Created GLES texture swapchain %dx%d x%u buffers=%d format=0x%x ctx=%p names=%u..%u", width, height,
            chain->arrayLayers, bufferCount, internal, chain->glContext, chain->glTextures[0],
            chain->glTextures[chain->bufferCount - 1]);
    return chain;
}

VRAPI ovrTextureSwapChain* vrapi_CreateTextureSwapChain(int32_t type, int32_t format, int width, int height,
                                                         int levels, bool buffered) {
    return vrapi_CreateTextureSwapChain2(type, format, width, height, levels, buffered ? 3 : 1);
}

VRAPI unsigned int vrapi_GetTextureSwapChainHandle(ovrTextureSwapChain* candidate, int index) {
    ovp::Runtime& s = ovp::runtime();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    ovrTextureSwapChain* chain = findChain(candidate);
    if (!chain || !chain->gl || index < 0 || index >= static_cast<int>(chain->bufferCount)) return 0;
    return chain->glTextures[static_cast<uint32_t>(index)];
}
