#include "gltf_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace quest {
namespace {

// --- A small JSON reader: what glTF's JSON chunk needs. ---

struct Json {
  enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
  double number = 0.0;
  bool boolean = false;
  std::string string;
  std::vector<Json> array;
  std::vector<std::pair<std::string, Json>> object;

  const Json* get(const char* key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& [name, value] : object) {
      if (name == key) return &value;
    }
    return nullptr;
  }
  const Json* at(size_t index) const {
    return type == Type::Array && index < array.size() ? &array[index] : nullptr;
  }
  double num(const char* key, double fallback) const {
    const Json* v = get(key);
    return v != nullptr && v->type == Type::Number ? v->number : fallback;
  }
  int integer(const char* key, int fallback) const { return static_cast<int>(num(key, fallback)); }
};

class Parser {
public:
  Parser(const char* text, size_t size) : mText(text), mEnd(text + size) {}

  bool parse(Json& out) {
    if (!value(out, 0)) return false;
    space();
    return true;
  }

private:
  void space() {
    while (mText < mEnd && (*mText == ' ' || *mText == '\n' || *mText == '\r' || *mText == '\t')) ++mText;
  }

  bool literal(const char* word) {
    const size_t n = std::strlen(word);
    if (static_cast<size_t>(mEnd - mText) < n || std::memcmp(mText, word, n) != 0) return false;
    mText += n;
    return true;
  }

  bool string(std::string& out) {
    if (mText >= mEnd || *mText != '"') return false;
    ++mText;
    while (mText < mEnd && *mText != '"') {
      char c = *mText++;
      if (c == '\\') {
        if (mText >= mEnd) return false;
        const char e = *mText++;
        switch (e) {
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'r': c = '\r'; break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'u':
          if (mEnd - mText < 4) return false;
          mText += 4;
          c = '?'; // names only: not decoded
          break;
        default: c = e; break;
        }
      }
      out.push_back(c);
    }
    if (mText >= mEnd) return false;
    ++mText;
    return true;
  }

  bool value(Json& out, int depth) {
    if (depth > 64) return false;
    space();
    if (mText >= mEnd) return false;
    const char c = *mText;
    if (c == '{') {
      out.type = Json::Type::Object;
      ++mText;
      space();
      if (mText < mEnd && *mText == '}') {
        ++mText;
        return true;
      }
      while (true) {
        space();
        std::string key;
        if (!string(key)) return false;
        space();
        if (mText >= mEnd || *mText != ':') return false;
        ++mText;
        Json item;
        if (!value(item, depth + 1)) return false;
        out.object.emplace_back(std::move(key), std::move(item));
        space();
        if (mText < mEnd && *mText == ',') {
          ++mText;
          continue;
        }
        if (mText < mEnd && *mText == '}') {
          ++mText;
          return true;
        }
        return false;
      }
    }
    if (c == '[') {
      out.type = Json::Type::Array;
      ++mText;
      space();
      if (mText < mEnd && *mText == ']') {
        ++mText;
        return true;
      }
      while (true) {
        Json item;
        if (!value(item, depth + 1)) return false;
        out.array.push_back(std::move(item));
        space();
        if (mText < mEnd && *mText == ',') {
          ++mText;
          continue;
        }
        if (mText < mEnd && *mText == ']') {
          ++mText;
          return true;
        }
        return false;
      }
    }
    if (c == '"') {
      out.type = Json::Type::String;
      return string(out.string);
    }
    if (literal("true")) {
      out.type = Json::Type::Bool;
      out.boolean = true;
      return true;
    }
    if (literal("false")) {
      out.type = Json::Type::Bool;
      return true;
    }
    if (literal("null")) return true;
    // A number.
    const char* start = mText;
    while (mText < mEnd && (std::strchr("+-0123456789.eE", *mText) != nullptr)) ++mText;
    if (mText == start) return false;
    out.type = Json::Type::Number;
    out.number = std::strtod(std::string(start, mText).c_str(), nullptr);
    return true;
  }

  const char* mText;
  const char* mEnd;
};

// --- glTF ---

using Mat4 = std::array<float, 16>; // column-major

Mat4 identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

Mat4 multiply(const Mat4& a, const Mat4& b) {
  Mat4 out{};
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) sum += a[k * 4 + r] * b[c * 4 + k];
      out[c * 4 + r] = sum;
    }
  }
  return out;
}

// A node's own transform: its matrix, or translation * rotation * scale.
Mat4 local_matrix(const Json& node) {
  if (const Json* m = node.get("matrix"); m != nullptr && m->array.size() == 16) {
    Mat4 out{};
    for (int i = 0; i < 16; ++i) out[i] = static_cast<float>(m->array[i].number);
    return out;
  }
  float t[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
  if (const Json* v = node.get("translation"); v != nullptr && v->array.size() == 3) {
    for (int i = 0; i < 3; ++i) t[i] = static_cast<float>(v->array[i].number);
  }
  if (const Json* v = node.get("rotation"); v != nullptr && v->array.size() == 4) {
    for (int i = 0; i < 4; ++i) q[i] = static_cast<float>(v->array[i].number);
  }
  if (const Json* v = node.get("scale"); v != nullptr && v->array.size() == 3) {
    for (int i = 0; i < 3; ++i) s[i] = static_cast<float>(v->array[i].number);
  }
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  return {
      (1 - 2 * (y * y + z * z)) * s[0], 2 * (x * y + w * z) * s[0], 2 * (x * z - w * y) * s[0], 0,
      2 * (x * y - w * z) * s[1], (1 - 2 * (x * x + z * z)) * s[1], 2 * (y * z + w * x) * s[1], 0,
      2 * (x * z + w * y) * s[2], 2 * (y * z - w * x) * s[2], (1 - 2 * (x * x + y * y)) * s[2], 0,
      t[0], t[1], t[2], 1,
  };
}

struct Reader {
  const Json& root;
  const uint8_t* bin;
  size_t binSize;
  GltfMesh& mesh;
  std::string& error;

  // An accessor's elements as floats (`components` each); integers are
  // normalized when the accessor says so.
  bool accessor(int index, std::vector<float>& out, int& components) const {
    const Json* acc = root.get("accessors") ? root.get("accessors")->at(static_cast<size_t>(index)) : nullptr;
    if (acc == nullptr) return fail("accessor missing");
    const Json* typeName = acc->get("type");
    const std::string type = typeName != nullptr ? typeName->string : "";
    components = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 : type == "VEC4" ? 4 : 0;
    if (components == 0) return fail("accessor type unsupported");
    const int componentType = acc->integer("componentType", 0);
    const size_t count = static_cast<size_t>(acc->num("count", 0));
    const bool normalized = acc->get("normalized") != nullptr && acc->get("normalized")->boolean;
    size_t size = 0;
    switch (componentType) {
    case 5120: case 5121: size = 1; break; // byte, unsigned byte
    case 5122: case 5123: size = 2; break; // short, unsigned short
    case 5125: case 5126: size = 4; break; // unsigned int, float
    default: return fail("component type unsupported");
    }
    const Json* views = root.get("bufferViews");
    const Json* view = views != nullptr ? views->at(static_cast<size_t>(acc->integer("bufferView", -1))) : nullptr;
    if (view == nullptr) return fail("accessor without a buffer view");
    if (view->integer("buffer", 0) != 0) return fail("a second buffer");
    const size_t base = static_cast<size_t>(view->num("byteOffset", 0) + acc->num("byteOffset", 0));
    const size_t stride = view->get("byteStride") ? static_cast<size_t>(view->num("byteStride", 0)) : size * components;
    if (count == 0) {
      out.clear();
      return true;
    }
    if (base + stride * (count - 1) + size * components > binSize) return fail("accessor past the buffer");
    out.resize(count * components);
    for (size_t i = 0; i < count; ++i) {
      const uint8_t* element = bin + base + stride * i;
      for (int c = 0; c < components; ++c) {
        const uint8_t* p = element + size * c;
        float v = 0.0f;
        switch (componentType) {
        case 5120: { int8_t x; std::memcpy(&x, p, 1); v = normalized ? std::fmax(x / 127.0f, -1.0f) : x; break; }
        case 5121: v = normalized ? *p / 255.0f : *p; break;
        case 5122: { int16_t x; std::memcpy(&x, p, 2); v = normalized ? std::fmax(x / 32767.0f, -1.0f) : x; break; }
        case 5123: { uint16_t x; std::memcpy(&x, p, 2); v = normalized ? x / 65535.0f : x; break; }
        case 5125: { uint32_t x; std::memcpy(&x, p, 4); v = static_cast<float>(x); break; }
        case 5126: std::memcpy(&v, p, 4); break;
        }
        out[i * components + c] = v;
      }
    }
    return true;
  }

  bool fail(const char* what) const {
    error = what;
    return false;
  }

  std::array<float, 4> color(const Json& primitive) const {
    const Json* materials = root.get("materials");
    const Json* material = materials != nullptr ? materials->at(static_cast<size_t>(primitive.integer("material", -1)))
                                                : nullptr;
    std::array<float, 4> c{0.5f, 0.5f, 0.5f, 1.0f};
    if (material == nullptr) return c;
    const Json* pbr = material->get("pbrMetallicRoughness");
    if (pbr == nullptr) return c;
    if (const Json* f = pbr->get("baseColorFactor"); f != nullptr && f->array.size() == 4) {
      for (int i = 0; i < 4; ++i) c[i] = static_cast<float>(f->array[i].number);
    }
    // Its colors are in a texture, left out: the controllers are dark grey.
    if (pbr->get("baseColorTexture") != nullptr) {
      c = {c[0] * 0.06f, c[1] * 0.06f, c[2] * 0.065f, c[3]};
    }
    return c;
  }

  bool primitive(const Json& prim, const Mat4& world) const {
    const int mode = prim.integer("mode", 4);
    if (mode != 4) return true; // triangles only
    const Json* attributes = prim.get("attributes");
    if (attributes == nullptr || attributes->get("POSITION") == nullptr) return true;
    std::vector<float> positions, normals, indices;
    int pc = 0, nc = 0, ic = 0;
    if (!accessor(attributes->integer("POSITION", -1), positions, pc)) return false;
    if (pc != 3) return fail("positions not 3D");
    const bool hasNormals = attributes->get("NORMAL") != nullptr;
    if (hasNormals && (!accessor(attributes->integer("NORMAL", -1), normals, nc) || nc != 3)) return fail("normals");
    const size_t count = positions.size() / 3;
    if (prim.get("indices") != nullptr) {
      if (!accessor(prim.integer("indices", -1), indices, ic) || ic != 1) return fail("indices");
    } else {
      indices.resize(count);
      for (size_t i = 0; i < count; ++i) indices[i] = static_cast<float>(i);
    }
    const auto c = color(prim);
    const size_t first = mesh.vertex_count();
    std::vector<float> faceNormals;
    if (!hasNormals) faceNormals.assign(count * 3, 0.0f);
    for (size_t i = 0; i < count; ++i) {
      const float* p = &positions[i * 3];
      float wp[3];
      for (int r = 0; r < 3; ++r) wp[r] = world[r] * p[0] + world[4 + r] * p[1] + world[8 + r] * p[2] + world[12 + r];
      float n[3] = {0, 1, 0};
      if (hasNormals) {
        const float* m = &normals[i * 3];
        for (int r = 0; r < 3; ++r) n[r] = world[r] * m[0] + world[4 + r] * m[1] + world[8 + r] * m[2];
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 1e-6f) for (float& x : n) x /= len;
      }
      mesh.vertices.insert(mesh.vertices.end(), {wp[0], wp[1], wp[2], n[0], n[1], n[2], c[0], c[1], c[2], c[3]});
      for (int r = 0; r < 3; ++r) {
        mesh.low[r] = std::fmin(mesh.low[r], wp[r]);
        mesh.high[r] = std::fmax(mesh.high[r], wp[r]);
      }
    }
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
      const auto a = static_cast<uint32_t>(indices[i]), b = static_cast<uint32_t>(indices[i + 1]),
                 d = static_cast<uint32_t>(indices[i + 2]);
      if (a >= count || b >= count || d >= count) return fail("index past the vertices");
      mesh.indices.insert(mesh.indices.end(), {static_cast<uint32_t>(first + a), static_cast<uint32_t>(first + b),
                                               static_cast<uint32_t>(first + d)});
      if (!hasNormals) {
        // A flat normal per triangle, summed at its corners.
        const float* pa = &mesh.vertices[(first + a) * GltfMesh::kFloatsPerVertex];
        const float* pb = &mesh.vertices[(first + b) * GltfMesh::kFloatsPerVertex];
        const float* pd = &mesh.vertices[(first + d) * GltfMesh::kFloatsPerVertex];
        const float u[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
        const float v[3] = {pd[0] - pa[0], pd[1] - pa[1], pd[2] - pa[2]};
        const float fn[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        for (uint32_t corner : {a, b, d}) {
          for (int r = 0; r < 3; ++r) faceNormals[corner * 3 + r] += fn[r];
        }
      }
    }
    if (!hasNormals) {
      for (size_t i = 0; i < count; ++i) {
        float* n = &faceNormals[i * 3];
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        float* out = &mesh.vertices[(first + i) * GltfMesh::kFloatsPerVertex + 3];
        for (int r = 0; r < 3; ++r) out[r] = len > 1e-9f ? n[r] / len : (r == 1 ? 1.0f : 0.0f);
      }
    }
    return true;
  }

  bool node(int index, const Mat4& parent, int depth) const {
    if (depth > 32) return fail("nodes too deep");
    const Json* nodes = root.get("nodes");
    const Json* n = nodes != nullptr ? nodes->at(static_cast<size_t>(index)) : nullptr;
    if (n == nullptr) return fail("node missing");
    const Mat4 world = multiply(parent, local_matrix(*n));
    if (n->get("mesh") != nullptr) {
      const Json* meshes = root.get("meshes");
      const Json* m = meshes != nullptr ? meshes->at(static_cast<size_t>(n->integer("mesh", -1))) : nullptr;
      if (m == nullptr) return fail("mesh missing");
      if (const Json* prims = m->get("primitives")) {
        for (const Json& prim : prims->array) {
          if (!primitive(prim, world)) return false;
        }
      }
    }
    if (const Json* children = n->get("children")) {
      for (const Json& child : children->array) {
        if (!node(static_cast<int>(child.number), world, depth + 1)) return false;
      }
    }
    return true;
  }
};

uint32_t u32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

} // namespace

bool read_glb(const uint8_t* data, size_t size, GltfMesh& mesh, std::string& error) {
  mesh = {};
  mesh.low = {INFINITY, INFINITY, INFINITY};
  mesh.high = {-INFINITY, -INFINITY, -INFINITY};
  if (data == nullptr || size < 20 || u32(data) != 0x46546C67u /* glTF */ || u32(data + 4) != 2) {
    error = "not a glTF 2 binary";
    return false;
  }
  const size_t total = std::min<size_t>(u32(data + 8), size);
  size_t at = 12;
  const char* json = nullptr;
  size_t jsonSize = 0;
  const uint8_t* bin = nullptr;
  size_t binSize = 0;
  while (at + 8 <= total) {
    const uint32_t length = u32(data + at), type = u32(data + at + 4);
    if (at + 8 + length > total) break;
    if (type == 0x4E4F534Au /* JSON */) {
      json = reinterpret_cast<const char*>(data + at + 8);
      jsonSize = length;
    } else if (type == 0x004E4942u /* BIN */) {
      bin = data + at + 8;
      binSize = length;
    }
    at += 8 + length;
  }
  if (json == nullptr) {
    error = "no JSON chunk";
    return false;
  }
  Json root;
  if (!Parser(json, jsonSize).parse(root) || root.type != Json::Type::Object) {
    error = "JSON unreadable";
    return false;
  }
  Reader reader{root, bin, binSize, mesh, error};
  // The default scene's nodes; without scenes, every node no other names as a child.
  std::vector<int> roots;
  const Json* scenes = root.get("scenes");
  if (const Json* scene = scenes != nullptr ? scenes->at(static_cast<size_t>(root.integer("scene", 0))) : nullptr) {
    if (const Json* nodes = scene->get("nodes")) {
      for (const Json& n : nodes->array) roots.push_back(static_cast<int>(n.number));
    }
  } else if (const Json* nodes = root.get("nodes")) {
    std::vector<bool> child(nodes->array.size(), false);
    for (const Json& n : nodes->array) {
      if (const Json* children = n.get("children")) {
        for (const Json& c : children->array) {
          if (c.number >= 0 && c.number < child.size()) child[static_cast<size_t>(c.number)] = true;
        }
      }
    }
    for (size_t i = 0; i < child.size(); ++i) {
      if (!child[i]) roots.push_back(static_cast<int>(i));
    }
  }
  for (int index : roots) {
    if (!reader.node(index, identity(), 0)) return false;
  }
  if (mesh.indices.empty()) {
    error = "no triangles";
    return false;
  }
  return true;
}

} // namespace quest
