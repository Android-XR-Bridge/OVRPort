// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <jni.h>
#include <vulkan/vulkan.h>

// Original declarations of the public, 64-bit VrApi calling convention.
// This is an interoperability implementation, not the Oculus SDK.
#define VRAPI extern "C" __attribute__((visibility("default")))
using ovrResult = int32_t;
constexpr ovrResult Success = 0, NotInitialized = -1004, InvalidParameter = -1005,
    DeviceUnavailable = -1010, InvalidOperation = -1015, Unsupported = -1050, NoDevice = -1051;
struct ovrEventHeader { int32_t EventType; };  // VrApi_Types.h: followed by event-specific data
struct ovrJava { JavaVM* Vm; JNIEnv* Env; jobject ActivityObject; };
struct ovrVector2f { float x, y; };
struct ovrVector3f { float x, y, z; };
struct ovrVector4f { float x, y, z, w; };
using ovrQuatf = ovrVector4f;
struct ovrMatrix4f { float M[4][4]; };
struct ovrPosef { ovrQuatf Orientation; ovrVector3f Position; };
struct ovrRectf { float x, y, width, height; };
struct ovrInitParms {
    int32_t Type, ProductVersion, MajorVersion, MinorVersion, PatchVersion, GraphicsAPI;
    ovrJava Java;
};
struct ovrModeParms {
    int32_t Type;
    uint32_t Flags;
    ovrJava Java;
    uint64_t Display, WindowSurface, ShareContext;
};
struct ovrModeParmsVulkan { ovrModeParms ModeParms; uint64_t SynchronizationQueue; };
struct ovrSystemCreateInfoVulkan { VkInstance Instance; VkPhysicalDevice PhysicalDevice; VkDevice Device; };
struct ovrRigidBodyPosef {
    ovrPosef Pose;
    ovrVector3f AngularVelocity, LinearVelocity, AngularAcceleration, LinearAcceleration;
    uint32_t Padding;
    double TimeInSeconds, PredictionInSeconds;
};
struct ovrTracking { uint32_t Status, Padding; ovrRigidBodyPosef HeadPose; };
struct ovrTracking2 {
    uint32_t Status, Padding;
    ovrRigidBodyPosef HeadPose;
    struct { ovrMatrix4f ProjectionMatrix, ViewMatrix; } Eye[2];
};
struct ovrTextureSwapChain;
struct ovrMobile { uint64_t generation; };
struct ovrLayerHeader2 {
    int32_t Type;
    uint32_t Flags;
    ovrVector4f ColorScale;
    int32_t SrcBlend, DstBlend;
    void* Reserved;
};
struct ovrLayerProjection2 {
    ovrLayerHeader2 Header;
    ovrRigidBodyPosef HeadPose;
    struct {
        ovrTextureSwapChain* ColorSwapChain;
        int32_t SwapChainIndex;
        ovrMatrix4f TexCoordsFromTanAngles;
        ovrRectf TextureRect;
    } Textures[2];
};
struct ovrSubmitFrameDescription2 {
    uint32_t Flags, SwapInterval;
    uint64_t FrameIndex;
    double DisplayTime;
    uint8_t Pad[8];
    uint32_t LayerCount;
    const ovrLayerHeader2* const* Layers;
};
struct ovrHmdColorDesc { int32_t ColorSpace; uint32_t Padding; };
static_assert(sizeof(void*) == 8, "The experimental VrApi adapter supports arm64 only");
static_assert(sizeof(ovrInitParms) == 48 && sizeof(ovrModeParmsVulkan) == 64);
static_assert(sizeof(ovrRigidBodyPosef) == 96 && sizeof(ovrTracking2) == 360);
static_assert(sizeof(ovrTracking) == 104 && sizeof(ovrLayerHeader2) == 40);
static_assert(sizeof(ovrLayerProjection2) == 328 && sizeof(ovrSubmitFrameDescription2) == 48);
static_assert(offsetof(ovrLayerProjection2, Textures) == 136);
