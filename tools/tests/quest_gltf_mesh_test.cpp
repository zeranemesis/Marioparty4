// The controllers' render models (XR_FB_render_model) read as one mesh:
// platforms/android/app/src/main/cpp/gltf_mesh.cpp, on binaries built here.
#include "../../platforms/android/app/src/main/cpp/gltf_mesh.cpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

void put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// A .glb from its JSON and binary chunks (each padded to 4 bytes).
std::vector<uint8_t> glb(std::string json, std::vector<uint8_t> bin) {
  while (json.size() % 4) json.push_back(' ');
  while (bin.size() % 4) bin.push_back(0);
  std::vector<uint8_t> out;
  put32(out, 0x46546C67u);
  put32(out, 2);
  put32(out, static_cast<uint32_t>(12 + 8 + json.size() + 8 + bin.size()));
  put32(out, static_cast<uint32_t>(json.size()));
  put32(out, 0x4E4F534Au);
  out.insert(out.end(), json.begin(), json.end());
  put32(out, static_cast<uint32_t>(bin.size()));
  put32(out, 0x004E4942u);
  out.insert(out.end(), bin.begin(), bin.end());
  return out;
}

template <typename T>
void append(std::vector<uint8_t>& bin, const std::vector<T>& values) {
  const auto* p = reinterpret_cast<const uint8_t*>(values.data());
  bin.insert(bin.end(), p, p + values.size() * sizeof(T));
}

} // namespace

int main() {
  // One triangle under a translated, scaled child node, with a material.
  {
    std::vector<uint8_t> bin;
    append(bin, std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0});
    append(bin, std::vector<float>{0, 0, 1, 0, 0, 1, 0, 0, 1});
    append(bin, std::vector<uint16_t>{0, 1, 2});
    const std::string json = R"({
      "asset": {"version": "2.0"}, "scene": 0,
      "scenes": [{"nodes": [0]}],
      "nodes": [{"translation": [1, 2, 3], "children": [1]}, {"scale": [2, 2, 2], "mesh": 0}],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}]}],
      "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [0.25, 0.5, 0.75, 1.0]}}],
      "buffers": [{"byteLength": 78}],
      "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                      {"buffer": 0, "byteOffset": 36, "byteLength": 36},
                      {"buffer": 0, "byteOffset": 72, "byteLength": 6}],
      "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
                    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
                    {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"}]
    })";
    const auto data = glb(json, bin);
    quest::GltfMesh mesh;
    std::string error;
    const bool ok = quest::read_glb(data.data(), data.size(), mesh, error);
    check(ok, ("node transforms: read (" + error + ")").c_str());
    if (ok) {
      check(mesh.vertex_count() == 3 && mesh.indices.size() == 3, "node transforms: one triangle");
      const float* v1 = &mesh.vertices[1 * quest::GltfMesh::kFloatsPerVertex];
      check(near(v1[0], 3) && near(v1[1], 2) && near(v1[2], 3), "node transforms: (1,0,0) scaled by 2, moved by (1,2,3)");
      check(near(v1[5], 1), "node transforms: normal kept unit length");
      check(near(v1[6], 0.25f) && near(v1[7], 0.5f) && near(v1[8], 0.75f), "material color");
      check(near(mesh.low[0], 1) && near(mesh.high[1], 4), "bounds");
    }
  }
  // Positions as normalized unsigned shorts (mesh quantization), no normals,
  // no indices, no scenes: computed normals, every node a root.
  {
    std::vector<uint8_t> bin;
    append(bin, std::vector<uint16_t>{0, 0, 0, 0, 0, 65535, 0, 0, 0, 0, 65535, 0});
    const std::string json = R"({
      "asset": {"version": "2.0"},
      "nodes": [{"mesh": 0}],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
      "bufferViews": [{"buffer": 0, "byteLength": 24, "byteStride": 8}],
      "accessors": [{"bufferView": 0, "componentType": 5123, "normalized": true, "count": 3, "type": "VEC3"}]
    })";
    const auto data = glb(json, bin);
    quest::GltfMesh mesh;
    std::string error;
    const bool ok = quest::read_glb(data.data(), data.size(), mesh, error);
    check(ok, ("quantized: read (" + error + ")").c_str());
    if (ok) {
      const float* v1 = &mesh.vertices[1 * quest::GltfMesh::kFloatsPerVertex];
      check(near(v1[0], 0) && near(v1[1], 1), "quantized: (0, 65535, 0) normalized to (0, 1, 0)");
      check(near(std::fabs(v1[3]), 1.0f), "quantized: computed normal along x");
    }
  }
  // Rejected cleanly: not a glTF, broken JSON, an index past the vertices.
  {
    quest::GltfMesh mesh;
    std::string error;
    const uint8_t junk[32] = {1, 2, 3};
    check(!quest::read_glb(junk, sizeof(junk), mesh, error), "junk rejected");
    const auto broken = glb("{\"nodes\": [", {});
    check(!quest::read_glb(broken.data(), broken.size(), mesh, error), "broken JSON rejected");
    std::vector<uint8_t> bin;
    append(bin, std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0});
    append(bin, std::vector<uint16_t>{0, 1, 7});
    const auto bad = glb(R"({"nodes": [{"mesh": 0}],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
      "bufferViews": [{"byteLength": 36}, {"byteOffset": 36, "byteLength": 6}],
      "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
                    {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}]})", bin);
    check(!quest::read_glb(bad.data(), bad.size(), mesh, error), "index past the vertices rejected");
  }
  if (failures == 0) std::printf("controller render models (glTF binary to one mesh): PASS\n");
  return failures == 0 ? 0 : 1;
}
