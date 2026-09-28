// Host check of Aurora's GX shader generator for the headset's instanced
// stereo: writes the WGSL of a set of shader configs, each plain and stereo,
// for naga to validate.
#include "gx/gx.hpp"
#include "gx/pipeline.hpp"
#include "gx/shader_info.hpp"
#include "gfx/stereo.hpp"

#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include <cstdio>
#include <fstream>
#include <string>

using namespace aurora::gx;

namespace aurora::gx {
std::string build_shader_source(const ShaderConfig& config) noexcept;
}

static void write(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
}

static ShaderConfig base_config() {
  ShaderConfig config{};
  for (auto& attr : config.attrs) {
    attr.attrType = GX_NONE;
  }
  for (auto& tcg : config.tcgs) {
    tcg.src = GX_MAX_TEXGENSRC;
  }
  config.attrs[GX_VA_POS].attrType = GX_DIRECT;
  config.attrs[GX_VA_POS].cnt = 3;
  config.attrs[GX_VA_POS].compType = GX_F32;
  config.vtxStride = 12;
  config.tevStageCount = 1;
  auto& stage = config.tevStages[0];
  stage.colorPass = {GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC};
  stage.alphaPass = {GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA};
  stage.channelId = GX_COLOR0A0;
  stage.texMapId = GX_TEXMAP_NULL;
  stage.texCoordId = GX_TEXCOORD_NULL;
  return config;
}

namespace aurora::gfx {
extern std::vector<std::vector<uint8_t>> g_pushedUniforms;
}

static void write_bytes(const std::string& path, const void* data, size_t size) {
  std::ofstream out(path, std::ios::binary);
  out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}

// A scene for validate.exe's render check: a vertex-coloured grid wider than
// each eye (so the cut at the eye's edge shows) in perspective (w varies),
// drawn per eye and as one instanced draw, with Aurora's own uniform blocks.
static int render_scene(const std::string& dir) {
  auto config = base_config();
  config.attrs[GX_VA_CLR0].attrType = GX_DIRECT;
  config.attrs[GX_VA_CLR0].cnt = 4;
  config.attrs[GX_VA_CLR0].compType = GX_RGBA8;
  config.attrs[GX_VA_CLR0].offset = 12;
  config.vtxStride = 16;
  config.colorChannels[0].matSrc = GX_SRC_VTX;
  config.colorChannels[1].matSrc = GX_SRC_VTX;
  config.stereo = StereoOff;
  write(dir + "/scene.wgsl", build_shader_source(config));
  config.stereo = StereoClipDistance;
  write(dir + "/scene-stereo.wgsl", build_shader_source(config));
  config.stereo = StereoDiscard;
  write(dir + "/scene-stereo-discard.wgsl", build_shader_source(config));
  config.stereo = StereoOff;

  // The grid: x in [-1.8, 1.8], y in [-0.95, 0.95], depth 0..2 across x.
  std::vector<uint8_t> verts;
  const auto vertex = [&](float x, float y, float z) {
    // GameCube data: big-endian floats.
    for (const float v : {x, y, z}) {
      uint32_t bits;
      std::memcpy(&bits, &v, 4);
      for (int shift = 24; shift >= 0; shift -= 8) verts.push_back(static_cast<uint8_t>(bits >> shift));
    }
    verts.push_back(static_cast<uint8_t>(std::lround((x + 1.8f) / 3.6f * 255.f)));
    verts.push_back(static_cast<uint8_t>(std::lround((y + 0.95f) / 1.9f * 255.f)));
    verts.push_back(static_cast<uint8_t>(std::lround(z / 2.f * 255.f)));
    verts.push_back(255);
  };
  constexpr int cols = 7, rows = 5;
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      const float x0 = -1.8f + 3.6f * c / cols, x1 = -1.8f + 3.6f * (c + 1) / cols;
      const float y0 = -0.95f + 1.9f * r / rows, y1 = -0.95f + 1.9f * (r + 1) / rows;
      const float z0 = 2.f * c / cols, z1 = 2.f * (c + 1) / cols;
      if ((r + c) % 3 == 2) continue; // holes: edges everywhere
      vertex(x0, y0, z0), vertex(x1, y0, z1), vertex(x1, y1, z1);
      vertex(x0, y0, z0), vertex(x1, y1, z1), vertex(x0, y1, z0);
    }
  }
  write_bytes(dir + "/scene-verts.bin", verts.data(), verts.size());

  // GX state: identity model-view; each eye's clip matrix (rows, as the game
  // gives them): shifted sideways, w = 1 + z/2, GX z/w = -0.5.
  auto& state = g_gxState;
  state.currentPnMtx = 0;
  for (auto& channel : state.colorChannelState) channel.matColor = aurora::Vec4<float>{1, 1, 1, 1};
  state.pnMtx[0].pos = aurora::Mat3x4<float>{aurora::Vec4<float>{1, 0, 0, 0}, aurora::Vec4<float>{0, 1, 0, 0}, aurora::Vec4<float>{0, 0, 1, 0}};
  const auto eye_clip = [](float shift) {
    return aurora::Mat4x4<float>{aurora::Vec4<float>{1.1f, 0, 0.2f * shift, shift}, aurora::Vec4<float>{0, 1.2f, 0.1f, 0},
                         aurora::Vec4<float>{0, 0, -0.25f, -0.5f}, aurora::Vec4<float>{0, 0, 0.5f, 1}};
  };
  const auto left = eye_clip(0.35f), right = eye_clip(-0.35f);
  const auto info = build_shader_info(config);
  const BindGroupRanges ranges{};
  auto& pushed = aurora::gfx::g_pushedUniforms;
  pushed.clear();
  build_uniform_stereo(info, 0, ranges, left, right);
  write_bytes(dir + "/scene-left.bin", pushed[0].data(), pushed[0].size());
  write_bytes(dir + "/scene-right.bin", pushed[1].data(), pushed[1].size());

  // Two layouts: eyes filling their halves, and halves wider than the drawn
  // eyes (the interface below is wider than both, at low resolution).
  const int eyeWidth = 160, height = 120;
  const int strides[2]{160, 200};
  for (int i = 0; i < 2; ++i) {
    config.stereo = 1;
    aurora::Vec4<float> eyeX, eyeClip;
    aurora::gfx::stereo::instanced_placement(static_cast<float>(strides[i]), static_cast<float>(eyeWidth), eyeX, eyeClip);
    pushed.clear();
    build_uniform_stereo_instanced(build_shader_info(config), 0, ranges, left, right, eyeX, eyeClip);
    write_bytes(dir + "/scene-both-" + std::to_string(strides[i]) + ".bin", pushed[0].data(), pushed[0].size());
  }
  std::ofstream params(dir + "/scene.txt");
  params << verts.size() / 16 << ' ' << eyeWidth << ' ' << height << ' ' << strides[0] << ' ' << strides[1] << '\n';
  std::printf("scene: %zu vertices\n", verts.size() / 16);
  return 0;
}

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : ".";
  if (argc > 2 && std::string{argv[2]} == "scene") {
    return render_scene(dir);
  }
  int written = 0;
  const auto emit = [&](const char* name, ShaderConfig config) {
    for (const auto [stereo, suffix] : {std::pair{StereoOff, ".wgsl"}, std::pair{StereoClipDistance, "-stereo.wgsl"},
                                        std::pair{StereoDiscard, "-stereo-discard.wgsl"}}) {
      config.stereo = stereo;
      write(dir + "/" + name + suffix, build_shader_source(config));
      ++written;
    }
  };

  emit("color", base_config());

  {
    auto config = base_config();
    config.attrs[GX_VA_CLR0].attrType = GX_DIRECT;
    config.attrs[GX_VA_CLR0].cnt = 4;
    config.attrs[GX_VA_CLR0].compType = GX_RGBA8;
    config.attrs[GX_VA_CLR0].offset = 12;
    config.attrs[GX_VA_TEX0].attrType = GX_DIRECT;
    config.attrs[GX_VA_TEX0].cnt = 2;
    config.attrs[GX_VA_TEX0].compType = GX_F32;
    config.attrs[GX_VA_TEX0].offset = 16;
    config.vtxStride = 12 + 4 + 8;
    config.tcgs[0].src = GX_TG_TEX0;
    config.tcgs[0].type = GX_TG_MTX2x4;
    config.tcgs[0].mtx = GX_IDENTITY;
    config.tcgs[0].postMtx = GX_PTIDENTITY;
    auto& stage = config.tevStages[0];
    stage.colorPass = {GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO};
    stage.alphaPass = {GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO};
    stage.texMapId = GX_TEXMAP0;
    stage.texCoordId = GX_TEXCOORD0;
    emit("textured", config);

    config.fogType = GX_FOG_PERSP_EXP;
    config.alphaCompare = {GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0};
    emit("textured-fog-alpha", config);
  }

  {
    auto config = base_config();
    config.attrs[GX_VA_PNMTXIDX].attrType = GX_DIRECT;
    config.attrs[GX_VA_NRM].attrType = GX_INDEX16;
    config.attrs[GX_VA_NRM].cnt = 3;
    config.attrs[GX_VA_NRM].compType = GX_F32;
    config.attrs[GX_VA_NRM].stride = 12;
    config.vtxStride = 1 + 12 + 2;
    config.colorChannels[0].lightingEnabled = true;
    config.colorChannels[0].matSrc = GX_SRC_REG;
    config.colorChannels[0].ambSrc = GX_SRC_REG;
    emit("lit", config);
  }

  {
    auto config = base_config();
    config.lineMode = 1; // lines stay per eye: the stereo bit is never set with them
    config.stereo = 0;
    write(dir + "/lines.wgsl", build_shader_source(config));
    ++written;
  }
  std::printf("%d shaders written\n", written);
  return 0;
}
