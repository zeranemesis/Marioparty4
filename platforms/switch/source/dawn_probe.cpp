#include <cstdio>
#include <cstdlib>
#include <memory>

#include <EGL/egl.h>
#include <switch.h>

#include <dawn/native/DawnNative.h>
#include <dawn/native/OpenGLBackend.h>
#include <webgpu/webgpu_cpp.h>

#include "partyboard_switch/egl.hpp"

namespace {

bool clearAndPresent(const wgpu::Adapter& adapter,
                     const wgpu::Device& device,
                     const wgpu::Surface& surface) {
    wgpu::SurfaceCapabilities capabilities{};
    if (surface.GetCapabilities(adapter, &capabilities) != wgpu::Status::Success ||
        capabilities.formatCount == 0 ||
        capabilities.alphaModeCount == 0) {
        std::printf("Dawn surface capabilities query failed\n");
        return false;
    }

    wgpu::SurfaceConfiguration config{};
    config.device = device;
    config.usage = wgpu::TextureUsage::RenderAttachment;
    config.format = capabilities.formats[0];
    config.width = PartyBoardSwitch_FramebufferWidth();
    config.height = PartyBoardSwitch_FramebufferHeight();
    config.presentMode = wgpu::PresentMode::Fifo;
    config.alphaMode = capabilities.alphaModes[0];
    config.viewFormatCount = 0;
    config.viewFormats = nullptr;

    surface.Configure(&config);

    wgpu::SurfaceTexture surfaceTexture{};
    surface.GetCurrentTexture(&surfaceTexture);
    if (surfaceTexture.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal ||
        !surfaceTexture.texture) {
        std::printf("Dawn GetCurrentTexture failed: %d\n",
                    static_cast<int>(surfaceTexture.status));
        surface.Unconfigure();
        return false;
    }

    wgpu::TextureView view = surfaceTexture.texture.CreateView();

    wgpu::RenderPassColorAttachment colorAttachment{};
    colorAttachment.view = view;
    colorAttachment.loadOp = wgpu::LoadOp::Clear;
    colorAttachment.storeOp = wgpu::StoreOp::Store;
    colorAttachment.clearValue = {0.04, 0.48, 0.08, 1.0};

    wgpu::RenderPassDescriptor passDescriptor{};
    passDescriptor.colorAttachmentCount = 1;
    passDescriptor.colorAttachments = &colorAttachment;

    wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDescriptor);
    pass.End();

    wgpu::CommandBuffer commands = encoder.Finish();
    device.GetQueue().Submit(1, &commands);

    const wgpu::Status presentStatus = surface.Present();
    surface.Unconfigure();
    if (presentStatus != wgpu::Status::Success) {
        std::printf("Dawn Surface.Present failed: %d\n",
                    static_cast<int>(presentStatus));
        return false;
    }

    return true;
}

bool runDawnProbe() {
    if (!PartyBoardSwitch_EglHasDawnRequirements()) {
        std::printf("Dawn probe skipped: required EGL extensions missing\n");
        return false;
    }

    // Native synchronous Dawn avoids TimedWaitAny/SystemEvent. This is
    // intentionally console-friendly: no Unix pipe is needed to discover the
    // adapter or create the device. The monolithic webgpu_dawn library
    // implements the wgpu* entry points itself, so no proc table is set.

    auto instance = std::make_unique<dawn::native::Instance>();
    instance->SetBackendValidationLevel(dawn::native::BackendValidationLevel::Disabled);

    WGPUSurface rawSurface =
        dawn::native::opengl::CreateSurfaceFromEGLNativeWindow(
            instance->Get(),
            PartyBoardSwitch_NativeWindow());
    if (rawSurface == nullptr) {
        std::printf("Dawn native EGL-window surface creation failed\n");
        return false;
    }
    wgpu::Surface surface = wgpu::Surface::Acquire(rawSurface);

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
    options.compatibleSurface = surface;

    auto adapters = instance->EnumerateAdapters(&options);
    if (adapters.empty()) {
        std::printf("Dawn OpenGLES adapter discovery failed\n");
        return false;
    }

    // This constructor takes an additional reference; the native Adapter also
    // stays alive in 'adapters' for the lifetime of this probe.
    wgpu::Adapter adapter(adapters.front().Get());

    wgpu::AdapterInfo info{};
    if (adapter.GetInfo(&info) != wgpu::Status::Success) {
        std::printf("Dawn adapter info query failed\n");
        return false;
    }

    wgpu::DeviceDescriptor deviceDescriptor{};
    WGPUDevice rawDevice = adapters.front().CreateDevice(&deviceDescriptor);
    if (rawDevice == nullptr) {
        std::printf("Dawn synchronous device creation failed\n");
        return false;
    }
    wgpu::Device device = wgpu::Device::Acquire(rawDevice);

    // Prove both ordinary WebGPU allocation and the complete presentation path.
    const wgpu::TextureDescriptor textureDescriptor{
        .usage = wgpu::TextureUsage::RenderAttachment,
        .dimension = wgpu::TextureDimension::e2D,
        .size = {16, 16, 1},
        .format = wgpu::TextureFormat::RGBA8Unorm,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    const wgpu::Texture texture = device.CreateTexture(&textureDescriptor);
    if (!texture) {
        std::printf("Dawn WebGPU texture allocation failed\n");
        return false;
    }

    if (!clearAndPresent(adapter, device, surface)) {
        std::printf("Dawn native NWindow presentation probe failed\n");
        return false;
    }

    std::printf("Dawn OpenGLES -> Surface -> NWindow presentation PASS\n");
    return true;
}

} // namespace

int main(int, char**) {
    // Dawn must own the EGLContext and EGLSurface. PartyBoard only initializes
    // the libnx NWindow and EGLDisplay for this probe.
    if (!PartyBoardSwitch_EglInitializeDisplay())
        return EXIT_FAILURE;

    const bool ok = runDawnProbe();

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
    }

    PartyBoardSwitch_EglShutdown();
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
