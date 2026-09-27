// Compiles each WGSL file given with Dawn (the version Aurora ships) and
// builds a render pipeline from its vs_main/fs_main, with clip distances when
// the adapter has them. Prints every error; exit code = files that failed.
#include <dawn/webgpu_cpp.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static std::string read(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

int main(int argc, char** argv) {
  const wgpu::InstanceFeatureName instanceFeatures[] = {wgpu::InstanceFeatureName::TimedWaitAny};
  wgpu::InstanceDescriptor instanceDesc{.requiredFeatureCount = 1, .requiredFeatures = instanceFeatures};
  wgpu::Instance instance = wgpu::CreateInstance(&instanceDesc);

  wgpu::Adapter adapter;
  wgpu::RequestAdapterOptions options{.backendType = wgpu::BackendType::Vulkan};
  instance.WaitAny(instance.RequestAdapter(&options, wgpu::CallbackMode::WaitAnyOnly,
                                           [&](wgpu::RequestAdapterStatus status, wgpu::Adapter a, wgpu::StringView) {
                                             if (status == wgpu::RequestAdapterStatus::Success) adapter = std::move(a);
                                           }),
                   UINT64_MAX);
  if (!adapter) {
    options.backendType = wgpu::BackendType::Undefined;
    instance.WaitAny(instance.RequestAdapter(&options, wgpu::CallbackMode::WaitAnyOnly,
                                             [&](wgpu::RequestAdapterStatus status, wgpu::Adapter a, wgpu::StringView) {
                                               if (status == wgpu::RequestAdapterStatus::Success) adapter = std::move(a);
                                             }),
                     UINT64_MAX);
  }
  if (!adapter) {
    std::printf("no adapter\n");
    return 100;
  }
  wgpu::AdapterInfo info;
  adapter.GetInfo(&info);
  const bool clip = adapter.HasFeature(wgpu::FeatureName::ClipDistances);
  std::printf("adapter: %.*s (backend %d), clip distances: %s\n", static_cast<int>(info.device.length), info.device.data,
              static_cast<int>(info.backendType), clip ? "yes" : "no");

  wgpu::FeatureName features[] = {wgpu::FeatureName::ClipDistances};
  wgpu::DeviceDescriptor deviceDesc{};
  deviceDesc.requiredFeatureCount = clip ? 1 : 0;
  deviceDesc.requiredFeatures = features;
  deviceDesc.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView message) {
    std::printf("  uncaptured: %.*s\n", static_cast<int>(message.length), message.data);
  });
  wgpu::Device device;
  instance.WaitAny(adapter.RequestDevice(&deviceDesc, wgpu::CallbackMode::WaitAnyOnly,
                                         [&](wgpu::RequestDeviceStatus status, wgpu::Device d, wgpu::StringView message) {
                                           if (status == wgpu::RequestDeviceStatus::Success) {
                                             device = std::move(d);
                                           } else {
                                             std::printf("device: %.*s\n", static_cast<int>(message.length), message.data);
                                           }
                                         }),
                   UINT64_MAX);
  if (!device) {
    return 101;
  }

  int failed = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string code = read(argv[i]);
    device.PushErrorScope(wgpu::ErrorFilter::Validation);
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = code.c_str();
    wgpu::ShaderModuleDescriptor moduleDesc{.nextInChain = &wgsl};
    wgpu::ShaderModule module = device.CreateShaderModule(&moduleDesc);
    std::string messages;
    instance.WaitAny(module.GetCompilationInfo(wgpu::CallbackMode::WaitAnyOnly,
                                               [&](wgpu::CompilationInfoRequestStatus, const wgpu::CompilationInfo* ci) {
                                                 for (size_t m = 0; ci != nullptr && m < ci->messageCount; ++m) {
                                                   const auto& msg = ci->messages[m];
                                                   if (msg.type == wgpu::CompilationMessageType::Error) {
                                                     messages += "    " + std::to_string(msg.lineNum) + ": " +
                                                                 std::string(msg.message.data, msg.message.length) + "\n";
                                                   }
                                                 }
                                               }),
                     UINT64_MAX);

    wgpu::ColorTargetState target{.format = wgpu::TextureFormat::RGBA8Unorm};
    wgpu::FragmentState fragment{.module = module, .entryPoint = "fs_main", .targetCount = 1, .targets = &target};
    wgpu::DepthStencilState depth{.format = wgpu::TextureFormat::Depth32Float,
                                  .depthWriteEnabled = wgpu::OptionalBool::True,
                                  .depthCompare = wgpu::CompareFunction::Always};
    wgpu::RenderPipelineDescriptor pipelineDesc{
        .vertex = {.module = module, .entryPoint = "vs_main"},
        .depthStencil = &depth,
        .multisample = {.count = 4},
        .fragment = &fragment,
    };
    wgpu::RenderPipeline pipeline = device.CreateRenderPipeline(&pipelineDesc);
    std::string scope;
    instance.WaitAny(device.PopErrorScope(wgpu::CallbackMode::WaitAnyOnly,
                                          [&](wgpu::PopErrorScopeStatus, wgpu::ErrorType type, wgpu::StringView message) {
                                            if (type != wgpu::ErrorType::NoError) {
                                              scope = std::string(message.data, message.length);
                                            }
                                          }),
                     UINT64_MAX);
    const bool ok = messages.empty() && scope.empty();
    std::printf("%s %s\n", ok ? "OK  " : "FAIL", argv[i]);
    if (!ok) {
      ++failed;
      std::printf("%s", messages.c_str());
      if (!scope.empty()) std::printf("    %s\n", scope.substr(0, 1500).c_str());
    }
  }
  return failed;
}
