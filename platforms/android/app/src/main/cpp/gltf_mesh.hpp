#pragma once

// A binary glTF 2.0 (.glb) reduced to one triangle mesh: every triangle
// primitive of the default scene, its node transforms applied, positions and
// normals in the model's space, with the material's base color factor per
// vertex. What the headset gives as a controller's render model
// (XR_FB_render_model) is drawn from it (hands_view.cpp); textures,
// animations and skins are left out.
//
// No OpenXR or GL here, so the PC tests read it too
// (tools/tests/quest_gltf_mesh_test.cpp).

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace quest {

struct GltfMesh {
  std::vector<float> vertices; // x y z, nx ny nz, r g b a per vertex
  std::vector<uint32_t> indices;
  std::array<float, 3> low{}, high{}; // bounds of the positions

  static constexpr size_t kFloatsPerVertex = 10;
  size_t vertex_count() const { return vertices.size() / kFloatsPerVertex; }
};

// False (with `error` set) when the data is not a glTF binary it can read.
bool read_glb(const uint8_t* data, size_t size, GltfMesh& mesh, std::string& error);

} // namespace quest
