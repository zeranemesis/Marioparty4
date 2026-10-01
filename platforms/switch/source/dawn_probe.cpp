#include <array>
#include <cstdio>
#include <cstdlib>

#include <EGL/egl.h>
#include <dawn/native/DawnNative.h>
#include <dawn/native/OpenGLBackend.h>
#include <webgpu/webgpu_cpp.h>

#include "partyboard_switch/egl.hpp"

namespace {

wgpu::Adapter g_adapter;
wgpu::Device g_device;

bool createAdapter(const wgpu::Instance& instance) {
    dawn::native::opengl::RequestAdapterOptionsGetGLProc glOptions;
    glOptions.getProc =
        reinterpret_cast<dawn::native::opengl::EGLGetProcProc>(eglGetProcAddress);
    glOptions.display =
        reinterpret_cast<dawn::native::opengl::EGLDisplay>(PartyBoardSwitch_EglDisplay());

    wgpu::RequestAdapterOptions options{};
    options.nextInChain = &glOptions;
    options.featureLevel = wgpu::FeatureLevel::Compatibility;
    options.powerPreference = wgpu::PowerPreference::HighPerformance;
    options.backendType = wgpu::BackendType::OpenGLES;

    const auto future = instance.RequestAdapter(
        &options,
        wgpu::CallbackMode::WaitAnyOnly,
        [](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView message) {
            if (status == wgpu::RequestAdapterStatus::Success) {
                g_adapter = std::move(adapter);
            } else {
                std::printf("Dawn adapter failure: %.*s\n",
                            static_cast<int>(message.length), message.data);
            }
        });

    return instance.WaitAny(future, 5'000'000'000ULL) == wgpu::WaitStatus::Success &&
           static_cast<bool>(g_adapter);
}

bool createDevice(const wgpu::Instance& instance) {
    wgpu::DeviceDescriptor descriptor{};
    descriptor.SetUncapturedErrorCallback(
        [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message) {
            std::printf("Dawn device error %d: %.*s\n",
                        static_cast<int>(type),
                        static_cast<int>(message.length), message.data);
        });

    const auto future = g_adapter.RequestDevice(
        &descriptor,
        wgpu::CallbackMode::WaitAnyOnly,
        [](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView message) {
            if (status == wgpu::RequestDeviceStatus::Success) {
                g_device = std::move(device);
            } else {
                std::printf("Dawn device failure: %.*s\n",
                            static_cast<int>(message.length), message.data);
            }
        });

    return instance.WaitAny(future, 5'000'000'000ULL) == wgpu::WaitStatus::Success &&
           static_cast<bool>(g_device);
}

bool runDawnProbe() {
    if (!PartyBoardSwitch_EglHasDawnRequirements()) {
        std::printf("Dawn probe skipped: required EGL extensions missing\n");
        return false;
    }

    const std::array requiredFeatures{
        wgpu::InstanceFeatureName::TimedWaitAny,
    };
    wgpu::InstanceDescriptor descriptor{
        .requiredFeatureCount = requiredFeatures.size(),
        .requiredFeatures = requiredFeatures.data(),
    };

    dawn::native::DawnInstanceDescriptor nativeDescriptor;
    nativeDescriptor.backendValidationLevel =
        dawn::native::BackendValidationLevel::Disabled;
    descriptor.nextInChain = &nativeDescriptor;

    const wgpu::Instance instance = wgpu::CreateInstance(&descriptor);
    if (!instance) {
        std::printf("Dawn instance creation failed\n");
        return false;
    }

    if (!createAdapter(instance))
        return false;
    if (!createDevice(instance))
        return false;

    wgpu::AdapterInfo info{};
    g_adapter.GetInfo(&info);
    std::printf("Dawn OpenGLES adapter ready: %.*s\n",
                static_cast<int>(info.device.length), info.device.data);

    // A tiny allocation proves the WebGPU device is actually usable rather than
    // merely discoverable.
    const wgpu::TextureDescriptor textureDescriptor{
        .usage = wgpu::TextureUsage::RenderAttachment,
        .dimension = wgpu::TextureDimension::e2D,
        .size = {16, 16, 1},
        .format = wgpu::TextureFormat::RGBA8Unorm,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    const wgpu::Texture texture = g_device.CreateTexture(&textureDescriptor);
    return static_cast<bool>(texture);
}

} // namespace

int main(int, char**) {
    if (!PartyBoardSwitch_EglInitialize())
        return EXIT_FAILURE;

    const bool ok = runDawnProbe();

    // Keep the result visible without requiring nxlink: green means the Dawn
    // OpenGLES adapter/device/texture path works, red means it failed.
    glClearColor(ok ? 0.05f : 0.55f, ok ? 0.45f : 0.04f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    PartyBoardSwitch_SwapBuffers();

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
    }

    g_device = {};
    g_adapter = {};
    PartyBoardSwitch_EglShutdown();
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
