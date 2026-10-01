#include <cstdio>
#include <cstdlib>
#include <memory>

#include <EGL/egl.h>
#include <glad/glad.h>
#include <switch.h>

#include <dawn/dawn_proc.h>
#include <dawn/native/DawnNative.h>
#include <dawn/native/OpenGLBackend.h>
#include <webgpu/webgpu_cpp.h>

#include "partyboard_switch/egl.hpp"

namespace {

bool runDawnProbe() {
    if (!PartyBoardSwitch_EglHasDawnRequirements()) {
        std::printf("Dawn probe skipped: required EGL extensions missing\n");
        return false;
    }

    // The native synchronous API deliberately avoids TimedWaitAny/SystemEvent.
    // Dawn's generic POSIX WaitAny implementation depends on Unix pipes, which
    // libnx does not provide as a native platform primitive.
    dawnProcSetProcs(&dawn::native::GetProcs());

    auto instance = std::make_unique<dawn::native::Instance>();
    instance->SetBackendValidationLevel(dawn::native::BackendValidationLevel::Disabled);

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

    auto adapters = instance->EnumerateAdapters(&options);
    if (adapters.empty()) {
        std::printf("Dawn OpenGLES adapter discovery failed\n");
        return false;
    }

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

    // A real GPU allocation proves the native OpenGLES device is usable, not
    // merely discoverable.
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

    std::printf("Dawn OpenGLES adapter/device/texture probe PASS\n");
    return true;
}

} // namespace

int main(int, char**) {
    if (!PartyBoardSwitch_EglInitialize())
        return EXIT_FAILURE;
    if (!gladLoadGL()) {
        PartyBoardSwitch_EglShutdown();
        return EXIT_FAILURE;
    }

    const bool ok = runDawnProbe();

    // Keep the result visible without requiring nxlink: green means the Dawn
    // OpenGLES adapter/device/texture path works, red means it failed.
    glViewport(0, 0,
               static_cast<GLsizei>(PartyBoardSwitch_FramebufferWidth()),
               static_cast<GLsizei>(PartyBoardSwitch_FramebufferHeight()));
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

    PartyBoardSwitch_EglShutdown();
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
