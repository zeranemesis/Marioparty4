// Draws harness.exe's scene twice with Dawn: each eye on its own (the
// per-eye path: its viewport, its uniform block) and both eyes in one
// instanced draw (the stereo shader, one block), for each image layout, and
// compares the two images pixel by pixel. Writes both as PPM for a look.
#include <dawn/webgpu_cpp.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static std::string dir;

static std::vector<uint8_t> read_bytes(const std::string& name) {
  std::ifstream in(dir + "/" + name, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

static wgpu::Instance instance;
static wgpu::Device device;
static int g_errors = 0;

static wgpu::Buffer make_buffer(const std::vector<uint8_t>& data, wgpu::BufferUsage usage) {
  const uint64_t size = (std::max<size_t>(data.size(), 16) + 255) & ~uint64_t{255};
  wgpu::BufferDescriptor desc{.usage = usage | wgpu::BufferUsage::CopyDst, .size = size};
  auto buffer = device.CreateBuffer(&desc);
  std::vector<uint8_t> padded(size, 0);
  std::memcpy(padded.data(), data.data(), data.size());
  device.GetQueue().WriteBuffer(buffer, 0, padded.data(), size);
  return buffer;
}

static wgpu::RenderPipeline make_pipeline(const std::string& file, wgpu::PipelineLayout layout) {
  const auto code = read_bytes(file);
  const std::string text(code.begin(), code.end());
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = text.c_str();
  wgpu::ShaderModuleDescriptor moduleDesc{.nextInChain = &wgsl};
  auto module = device.CreateShaderModule(&moduleDesc);
  wgpu::ColorTargetState target{.format = wgpu::TextureFormat::RGBA8Unorm};
  wgpu::FragmentState fragment{.module = module, .entryPoint = "fs_main", .targetCount = 1, .targets = &target};
  wgpu::RenderPipelineDescriptor desc{
      .layout = layout,
      .vertex = {.module = module, .entryPoint = "vs_main"},
      .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
      .fragment = &fragment,
  };
  return device.CreateRenderPipeline(&desc);
}

static std::vector<uint8_t> read_back(wgpu::Texture texture, uint32_t width, uint32_t height) {
  const uint32_t rowBytes = (width * 4 + 255) & ~255u;
  wgpu::BufferDescriptor desc{.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
                              .size = uint64_t{rowBytes} * height};
  auto buffer = device.CreateBuffer(&desc);
  auto encoder = device.CreateCommandEncoder();
  wgpu::TexelCopyTextureInfo src{.texture = texture};
  wgpu::TexelCopyBufferInfo dst{.layout = {.bytesPerRow = rowBytes, .rowsPerImage = height}, .buffer = buffer};
  wgpu::Extent3D extent{width, height, 1};
  encoder.CopyTextureToBuffer(&src, &dst, &extent);
  auto commands = encoder.Finish();
  device.GetQueue().Submit(1, &commands);
  instance.WaitAny(buffer.MapAsync(wgpu::MapMode::Read, 0, desc.size, wgpu::CallbackMode::WaitAnyOnly,
                                   [](wgpu::MapAsyncStatus, wgpu::StringView) {}),
                   UINT64_MAX);
  const auto* mapped = static_cast<const uint8_t*>(buffer.GetConstMappedRange());
  std::vector<uint8_t> pixels(size_t{width} * height * 4);
  for (uint32_t y = 0; y < height; ++y) {
    std::memcpy(pixels.data() + size_t{y} * width * 4, mapped + size_t{y} * rowBytes, width * 4);
  }
  buffer.Unmap();
  return pixels;
}

static void write_ppm(const std::string& name, const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
  std::ofstream out(dir + "/" + name, std::ios::binary);
  out << "P6\n" << width << ' ' << height << "\n255\n";
  for (size_t i = 0; i < rgba.size(); i += 4) out.write(reinterpret_cast<const char*>(&rgba[i]), 3);
}

int main(int argc, char** argv) {
  dir = argc > 1 ? argv[1] : ".";
  const wgpu::InstanceFeatureName instanceFeatures[] = {wgpu::InstanceFeatureName::TimedWaitAny};
  wgpu::InstanceDescriptor instanceDesc{.requiredFeatureCount = 1, .requiredFeatures = instanceFeatures};
  instance = wgpu::CreateInstance(&instanceDesc);
  wgpu::Adapter adapter;
  wgpu::RequestAdapterOptions options{};
  instance.WaitAny(instance.RequestAdapter(&options, wgpu::CallbackMode::WaitAnyOnly,
                                           [&](wgpu::RequestAdapterStatus s, wgpu::Adapter a, wgpu::StringView) {
                                             if (s == wgpu::RequestAdapterStatus::Success) adapter = std::move(a);
                                           }),
                   UINT64_MAX);
  if (!adapter || !adapter.HasFeature(wgpu::FeatureName::ClipDistances)) {
    std::printf("no adapter with clip distances\n");
    return 100;
  }
  wgpu::FeatureName features[] = {wgpu::FeatureName::ClipDistances};
  wgpu::DeviceDescriptor deviceDesc{};
  deviceDesc.requiredFeatureCount = 1;
  deviceDesc.requiredFeatures = features;
  int& errors = g_errors;
  deviceDesc.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView message) {
    std::printf("WebGPU error: %.*s\n", static_cast<int>(message.length), message.data);
    ++g_errors;
  });
  instance.WaitAny(adapter.RequestDevice(&deviceDesc, wgpu::CallbackMode::WaitAnyOnly,
                                         [&](wgpu::RequestDeviceStatus s, wgpu::Device d, wgpu::StringView) {
                                           if (s == wgpu::RequestDeviceStatus::Success) device = std::move(d);
                                         }),
                   UINT64_MAX);
  if (!device) return 101;

  unsigned vertexCount = 0, eyeWidth = 0, height = 0, strides[2]{};
  const auto sceneText = read_bytes("scene.txt");
  std::istringstream(std::string(sceneText.begin(), sceneText.end())) >> vertexCount >>
      eyeWidth >> height >> strides[0] >> strides[1];

  // Aurora's groups: 0 the vertex and array storage, 1 the draw's uniforms.
  const auto both = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
  wgpu::BindGroupLayoutEntry storageEntries[2]{
      {.binding = 0, .visibility = both, .buffer = {.type = wgpu::BufferBindingType::ReadOnlyStorage}},
      {.binding = 1, .visibility = both, .buffer = {.type = wgpu::BufferBindingType::ReadOnlyStorage}},
  };
  wgpu::BindGroupLayoutDescriptor storageLayoutDesc{.entryCount = 2, .entries = storageEntries};
  auto storageLayout = device.CreateBindGroupLayout(&storageLayoutDesc);
  wgpu::BindGroupLayoutEntry uniformEntry{.binding = 0, .visibility = both,
                                          .buffer = {.type = wgpu::BufferBindingType::Uniform}};
  wgpu::BindGroupLayoutDescriptor uniformLayoutDesc{.entryCount = 1, .entries = &uniformEntry};
  auto uniformLayout = device.CreateBindGroupLayout(&uniformLayoutDesc);
  wgpu::BindGroupLayout layouts[2]{storageLayout, uniformLayout};
  wgpu::PipelineLayoutDescriptor layoutDesc{.bindGroupLayoutCount = 2, .bindGroupLayouts = layouts};
  auto layout = device.CreatePipelineLayout(&layoutDesc);

  auto perEye = make_pipeline("scene.wgsl", layout);
  // Both instanced variants: clip distances, and the fragment test drivers
  // that fail clip-distance pipelines get instead (StereoDiscard).
  wgpu::RenderPipeline instancedVariants[2]{make_pipeline("scene-stereo.wgsl", layout),
                                            make_pipeline("scene-stereo-discard.wgsl", layout)};
  const char* variantNames[2]{"clip distances", "fragment test "};

  auto verts = make_buffer(read_bytes("scene-verts.bin"), wgpu::BufferUsage::Storage);
  auto arrays = make_buffer({}, wgpu::BufferUsage::Storage);
  wgpu::BindGroupEntry storageBind[2]{{.binding = 0, .buffer = verts}, {.binding = 1, .buffer = arrays}};
  wgpu::BindGroupDescriptor storageGroupDesc{.layout = storageLayout, .entryCount = 2, .entries = storageBind};
  auto storageGroup = device.CreateBindGroup(&storageGroupDesc);
  const auto uniform_group = [&](const std::string& file) {
    auto buffer = make_buffer(read_bytes(file), wgpu::BufferUsage::Uniform);
    wgpu::BindGroupEntry entry{.binding = 0, .buffer = buffer};
    wgpu::BindGroupDescriptor desc{.layout = uniformLayout, .entryCount = 1, .entries = &entry};
    return device.CreateBindGroup(&desc);
  };
  auto leftGroup = uniform_group("scene-left.bin");
  auto rightGroup = uniform_group("scene-right.bin");

  int failures = 0;
  for (unsigned variant = 0; variant < 2; ++variant)
  for (unsigned stride : strides) {
    const unsigned width = stride * 2;
    const auto& instanced = instancedVariants[variant];
    auto bothGroup = uniform_group("scene-both-" + std::to_string(stride) + ".bin");
    const auto draw = [&](bool useInstanced) {
      wgpu::TextureDescriptor desc{.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc,
                                   .size = {width, height, 1},
                                   .format = wgpu::TextureFormat::RGBA8Unorm};
      auto texture = device.CreateTexture(&desc);
      wgpu::RenderPassColorAttachment color{.view = texture.CreateView(),
                                            .loadOp = wgpu::LoadOp::Clear,
                                            .storeOp = wgpu::StoreOp::Store,
                                            .clearValue = {0, 0, 0, 0}};
      wgpu::RenderPassDescriptor passDesc{.colorAttachmentCount = 1, .colorAttachments = &color};
      auto encoder = device.CreateCommandEncoder();
      auto pass = encoder.BeginRenderPass(&passDesc);
      pass.SetBindGroup(0, storageGroup);
      if (useInstanced) {
        // common.cpp's view 2: both eyes' rectangles.
        pass.SetPipeline(instanced);
        pass.SetViewport(0, 0, static_cast<float>(stride + eyeWidth), static_cast<float>(height), 0, 1);
        pass.SetScissorRect(0, 0, stride + eyeWidth, height);
        pass.SetBindGroup(1, bothGroup);
        pass.Draw(vertexCount, 2);
      } else {
        pass.SetPipeline(perEye);
        for (unsigned eye = 0; eye < 2; ++eye) {
          pass.SetViewport(static_cast<float>(eye * stride), 0, static_cast<float>(eyeWidth), static_cast<float>(height), 0, 1);
          pass.SetScissorRect(eye * stride, 0, eyeWidth, height);
          pass.SetBindGroup(1, eye == 0 ? leftGroup : rightGroup);
          pass.Draw(vertexCount, 1);
        }
      }
      pass.End();
      auto commands = encoder.Finish();
      device.GetQueue().Submit(1, &commands);
      return read_back(texture, width, height);
    };
    const auto reference = draw(false);
    const auto candidate = draw(true);
    unsigned differing = 0, maxDelta = 0, drawnLeft = 0, drawnRight = 0, leftEdge = 0, gap = 0;
    for (unsigned y = 0; y < height; ++y) {
      for (unsigned x = 0; x < width; ++x) {
        const size_t i = (size_t{y} * width + x) * 4;
        unsigned delta = 0;
        for (int c = 0; c < 4; ++c) delta = std::max<unsigned>(delta, std::abs(reference[i + c] - candidate[i + c]));
        differing += delta > 1;
        maxDelta = std::max(maxDelta, delta);
        const bool drawn = (reference[i] | reference[i + 1] | reference[i + 2]) != 0;
        drawnLeft += drawn && x < eyeWidth;
        drawnRight += drawn && x >= stride;
        leftEdge += drawn && x == eyeWidth - 1;
        gap += (candidate[i] | candidate[i + 1] | candidate[i + 2]) != 0 && x >= eyeWidth && x < stride; // nothing between the eyes
      }
    }
    const bool ok = differing == 0 && gap == 0 && errors == 0 && drawnLeft > 0 && drawnRight > 0 && leftEdge > 0;
    std::printf("%s %s stride %u: %u of %u pixels differ (max delta %u), between eyes %u, drawn %u/%u, left edge %u\n",
                ok ? "OK  " : "FAIL", variantNames[variant], stride, differing, width * height, maxDelta, gap, drawnLeft,
                drawnRight, leftEdge);
    write_ppm("scene-per-eye-" + std::to_string(stride) + ".ppm", reference, width, height);
    write_ppm("scene-both-" + std::to_string(variant) + "-" + std::to_string(stride) + ".ppm", candidate, width, height);
    failures += !ok;
  }
  return failures + errors;
}
