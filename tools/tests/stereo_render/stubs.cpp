// What the shader generator's files refer to outside them: the harness only
// generates WGSL, so the GPU side is inert and logging goes to stdout.
#include "gx/gx.hpp"
#include "gfx/common.hpp"
#include "gfx/texture.hpp"
#include "webgpu/gpu.hpp"

#include <cstdio>

AuroraConfig aurora::g_config{};

namespace aurora {
void log_internal(AuroraLogLevel level, const char* module, const char* message, unsigned int len) noexcept {
  std::fprintf(level >= LOG_ERROR ? stderr : stdout, "[%s] %.*s\n", module, static_cast<int>(len), message);
  if (level >= LOG_FATAL) {
    std::abort();
  }
}
} // namespace aurora

namespace aurora::webgpu {
wgpu::Device g_device;
}

namespace aurora::gx {
GXState g_gxState{};

u8 comp_type_size(GXAttr attr, GXCompType type) noexcept {
  switch (attr) {
  case GX_VA_PNMTXIDX:
  case GX_VA_TEX0MTXIDX:
  case GX_VA_TEX1MTXIDX:
  case GX_VA_TEX2MTXIDX:
  case GX_VA_TEX3MTXIDX:
  case GX_VA_TEX4MTXIDX:
  case GX_VA_TEX5MTXIDX:
  case GX_VA_TEX6MTXIDX:
  case GX_VA_TEX7MTXIDX:
    return 1;
  default:
    return type == GX_U8 || type == GX_S8 ? 1 : type == GX_U16 || type == GX_S16 ? 2 : 4;
  }
}

const gfx::TextureBind& get_texture(GXTexMapID) noexcept {
  static gfx::TextureBind bind{};
  return bind;
}
} // namespace aurora::gx

namespace aurora::gfx {
uint32_t align_uniform(uint32_t value) { return (value + 255) & ~255u; }
// Every uniform block built, for the harness to write out.
std::vector<std::vector<uint8_t>> g_pushedUniforms;
Range push_uniform(const uint8_t* data, size_t length) {
  g_pushedUniforms.emplace_back(data, data + length);
  return {0, static_cast<uint32_t>(length)};
}
} // namespace aurora::gfx

extern "C" {
WGPUShaderModule wgpuDeviceCreateShaderModule(WGPUDevice, const WGPUShaderModuleDescriptor*) { return nullptr; }
void wgpuShaderModuleRelease(WGPUShaderModule) {}
void wgpuDeviceRelease(WGPUDevice) {}
}
