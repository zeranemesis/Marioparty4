#include "hands_view.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include <sys/system_properties.h>

namespace quest {
namespace {

// Eye pixels of the layer: what it shows is small and near, so a square of
// this side per eye, stretched over its narrowed field of view, is enough.
constexpr uint32_t kEyePixels = 1024;
constexpr float kNear = 0.03f;
constexpr float kFar = 6.0f;
// Thumb and index tips closer than this pinch; apart by more, they let go.
constexpr float kPinchOnMeters = 0.018f;
constexpr float kPinchOffMeters = 0.035f;
// The field of view drawn: what is shown, plus this margin (radians).
constexpr float kFovMargin = 0.03f;

const char* const kVertexShader = R"(#version 300 es
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aColor;
uniform mat4 uViewProj;
uniform mat4 uModel;
uniform vec3 uInvScale;
uniform float uMesh;
out vec3 vNormal;
out vec3 vWorld;
out vec4 vColor;
invariant gl_Position;
void main() {
  vec4 world = uModel * vec4(aPosition, 1.0);
  vWorld = world.xyz;
  // A model's own normals and colors; else the unit sphere's normal through
  // the ellipsoid's scale and turn.
  vNormal = uMesh > 0.5 ? mat3(uModel) * aNormal : mat3(uModel) * (aPosition * uInvScale * uInvScale);
  vColor = uMesh > 0.5 ? aColor : vec4(1.0);
  gl_Position = uViewProj * world;
}
)";

const char* const kFragmentShader = R"(#version 300 es
precision mediump float;
uniform vec4 uColor;
uniform vec3 uEye;
in vec3 vNormal;
in vec3 vWorld;
in vec4 vColor;
out vec4 oColor;
void main() {
  vec3 n = normalize(vNormal);
  vec3 v = normalize(uEye - vWorld);
  vec3 l = normalize(vec3(0.3, 1.0, 0.45));
  float diffuse = 0.45 + 0.55 * max(dot(n, l), 0.0);
  float rim = pow(1.0 - max(abs(dot(n, v)), 0.0), 3.0);
  vec4 base = uColor * vColor;
  vec3 color = base.rgb * diffuse + vec3(rim * 0.25);
  oColor = vec4(color * base.a, base.a);
}
)";

using Mat4 = std::array<float, 16>; // column-major

Mat4 pose_matrix(const XrPosef& pose, XrVector3f scale) {
  const auto& q = pose.orientation;
  const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
  const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
  const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
  return {
      (1 - 2 * (yy + zz)) * scale.x, 2 * (xy + wz) * scale.x, 2 * (xz - wy) * scale.x, 0,
      2 * (xy - wz) * scale.y, (1 - 2 * (xx + zz)) * scale.y, 2 * (yz + wx) * scale.y, 0,
      2 * (xz + wy) * scale.z, 2 * (yz - wx) * scale.z, (1 - 2 * (xx + yy)) * scale.z, 0,
      pose.position.x, pose.position.y, pose.position.z, 1,
  };
}

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

Mat4 projection(const XrFovf& fov) {
  const float left = std::tan(fov.angleLeft), right = std::tan(fov.angleRight);
  const float down = std::tan(fov.angleDown), up = std::tan(fov.angleUp);
  const float width = right - left, height = up - down;
  return {
      2.0f / width, 0, 0, 0,
      0, 2.0f / height, 0, 0,
      (right + left) / width, (up + down) / height, -(kFar + kNear) / (kFar - kNear), -1,
      0, 0, -2.0f * kFar * kNear / (kFar - kNear), 0,
  };
}

XrVector3f sub(XrVector3f a, XrVector3f b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float length(XrVector3f v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// The turn taking +Z to `dir` (a unit vector).
XrQuaternionf turn_z_to(XrVector3f dir) {
  const float d = dir.z;
  if (d < -0.9999f) return {0.0f, 1.0f, 0.0f, 0.0f};
  const XrVector3f axis = cross({0.0f, 0.0f, 1.0f}, dir);
  const float w = 1.0f + d;
  const float norm = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z + w * w);
  return {axis.x / norm, axis.y / norm, axis.z / norm, w / norm};
}

XrQuaternionf turn_x(float angle) { return {std::sin(angle * 0.5f), 0.0f, 0.0f, std::cos(angle * 0.5f)}; }

std::array<float, 4> mix(const std::array<float, 4>& a, const std::array<float, 4>& b, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t, a[3] + (b[3] - a[3]) * t};
}

bool joint_valid(const XrHandJointLocationEXT& joint) {
  constexpr XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  return (joint.locationFlags & valid) == valid;
}

GLuint compile(GLenum type, const char* source) {
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    LOGW("Hands: shader failed: %s", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

} // namespace

bool HandsView::init(XrInstance instance, XrSystemId system, XrSession session, int64_t swapchainFormat,
                     bool handTracking, bool renderModels) {
  if (renderModels) {
    mModelProperties = proc<PFN_xrGetRenderModelPropertiesFB>(instance, "xrGetRenderModelPropertiesFB");
    mLoadModel = proc<PFN_xrLoadRenderModelFB>(instance, "xrLoadRenderModelFB");
  }
  mInstance = instance;
  mSession = session;
  if (!create_program()) return false;
  create_sphere();

  const auto* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
  if (extensions != nullptr && std::strstr(extensions, "GL_EXT_multisampled_render_to_texture") != nullptr) {
    mTextureMultisample =
        reinterpret_cast<FramebufferTexture2DMultisample>(eglGetProcAddress("glFramebufferTexture2DMultisampleEXT"));
    mStorageMultisample =
        reinterpret_cast<RenderbufferStorageMultisample>(eglGetProcAddress("glRenderbufferStorageMultisampleEXT"));
  }
  mSamples = mTextureMultisample != nullptr && mStorageMultisample != nullptr ? 4 : 1;

  mEyePixels = kEyePixels;
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  info.format = swapchainFormat;
  info.sampleCount = 1;
  info.width = mEyePixels * 2;
  info.height = mEyePixels;
  info.faceCount = 1;
  info.arraySize = 1;
  info.mipCount = 1;
  if (!check(instance, xrCreateSwapchain(session, &info, &mSwapchain), "xrCreateSwapchain (hands)")) {
    mSwapchain = XR_NULL_HANDLE;
    destroy();
    return false;
  }
  uint32_t count = 0;
  xrEnumerateSwapchainImages(mSwapchain, 0, &count, nullptr);
  mImages.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
  xrEnumerateSwapchainImages(mSwapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(mImages.data()));

  glGenFramebuffers(1, &mFramebuffer);
  glGenRenderbuffers(1, &mDepth);
  glBindRenderbuffer(GL_RENDERBUFFER, mDepth);
  if (mSamples > 1) {
    mStorageMultisample(GL_RENDERBUFFER, mSamples, GL_DEPTH_COMPONENT24, mEyePixels * 2, mEyePixels);
  } else {
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, mEyePixels * 2, mEyePixels);
  }
  glBindRenderbuffer(GL_RENDERBUFFER, 0);

  if (handTracking) {
    XrSystemHandTrackingPropertiesEXT handProperties{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
    properties.next = &handProperties;
    mCreateTracker = proc<PFN_xrCreateHandTrackerEXT>(instance, "xrCreateHandTrackerEXT");
    mDestroyTracker = proc<PFN_xrDestroyHandTrackerEXT>(instance, "xrDestroyHandTrackerEXT");
    mLocateJoints = proc<PFN_xrLocateHandJointsEXT>(instance, "xrLocateHandJointsEXT");
    if (XR_SUCCEEDED(xrGetSystemProperties(instance, system, &properties)) && handProperties.supportsHandTracking &&
        mCreateTracker && mDestroyTracker && mLocateJoints) {
      for (int hand = 0; hand < 2; ++hand) {
        XrHandTrackerCreateInfoEXT create{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
        create.hand = hand == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
        create.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        if (!check(instance, mCreateTracker(session, &create, &mHands[hand].tracker), "xrCreateHandTrackerEXT")) {
          mHands[hand].tracker = XR_NULL_HANDLE;
        }
      }
    }
  }
  LOGI("Hands: controllers and hands drawn (%ux%u per eye, %dx MSAA), hand tracking %s", mEyePixels, mEyePixels,
       static_cast<int>(mSamples), mHands[0].tracker != XR_NULL_HANDLE ? "on" : "unavailable");
  return true;
}

void HandsView::destroy() {
  for (auto& hand : mHands) {
    if (hand.tracker != XR_NULL_HANDLE && mDestroyTracker) mDestroyTracker(hand.tracker);
    hand = {};
  }
  if (mSwapchain != XR_NULL_HANDLE) xrDestroySwapchain(mSwapchain);
  mSwapchain = XR_NULL_HANDLE;
  mImages.clear();
  if (mFramebuffer) glDeleteFramebuffers(1, &mFramebuffer);
  if (mDepth) glDeleteRenderbuffers(1, &mDepth);
  if (mVertices) glDeleteBuffers(1, &mVertices);
  if (mIndices) glDeleteBuffers(1, &mIndices);
  if (mVao) glDeleteVertexArrays(1, &mVao);
  if (mProgram) glDeleteProgram(mProgram);
  for (Model& model : mModels) {
    if (model.vertices) glDeleteBuffers(1, &model.vertices);
    if (model.indices) glDeleteBuffers(1, &model.indices);
    if (model.vao) glDeleteVertexArrays(1, &model.vao);
    model = {};
  }
  mFramebuffer = mDepth = mVertices = mIndices = mVao = mProgram = 0;
}

bool HandsView::create_program() {
  const GLuint vertex = compile(GL_VERTEX_SHADER, kVertexShader);
  const GLuint fragment = compile(GL_FRAGMENT_SHADER, kFragmentShader);
  if (!vertex || !fragment) {
    if (vertex) glDeleteShader(vertex);
    if (fragment) glDeleteShader(fragment);
    return false;
  }
  mProgram = glCreateProgram();
  glAttachShader(mProgram, vertex);
  glAttachShader(mProgram, fragment);
  glLinkProgram(mProgram);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint ok = 0;
  glGetProgramiv(mProgram, GL_LINK_STATUS, &ok);
  if (!ok) {
    LOGW("Hands: program link failed");
    glDeleteProgram(mProgram);
    mProgram = 0;
    return false;
  }
  mViewProjLoc = glGetUniformLocation(mProgram, "uViewProj");
  mModelLoc = glGetUniformLocation(mProgram, "uModel");
  mInvScaleLoc = glGetUniformLocation(mProgram, "uInvScale");
  mColorLoc = glGetUniformLocation(mProgram, "uColor");
  mEyeLoc = glGetUniformLocation(mProgram, "uEye");
  mMeshLoc = glGetUniformLocation(mProgram, "uMesh");
  return true;
}

// The controllers' own models (XR_FB_render_model), once the runtime has
// them: a glTF binary each, read as one mesh (gltf_mesh.cpp). Tried every
// 2 s while a controller shows, a few times; the drawn shape stays meanwhile
// and if they never come.
void HandsView::load_models() {
  if (mModelProperties == nullptr || mLoadModel == nullptr) return;
  const auto now = std::chrono::steady_clock::now();
  if (now - mModelsTriedAt < std::chrono::seconds(2)) return;
  mModelsTriedAt = now;
  static const char* const kPaths[2] = {"/model_fb/controller/left", "/model_fb/controller/right"};
  for (int hand = 0; hand < 2; ++hand) {
    Model& model = mModels[hand];
    if (model.loaded || model.attempts >= 10) continue;
    ++model.attempts;
    XrPath path = XR_NULL_PATH;
    if (XR_FAILED(xrStringToPath(mInstance, kPaths[hand], &path))) continue;
    XrRenderModelCapabilitiesRequestFB request{XR_TYPE_RENDER_MODEL_CAPABILITIES_REQUEST_FB};
    request.flags = XR_RENDER_MODEL_SUPPORTS_GLTF_2_0_SUBSET_2_BIT_FB;
    XrRenderModelPropertiesFB properties{XR_TYPE_RENDER_MODEL_PROPERTIES_FB};
    properties.next = &request;
    if (XR_FAILED(mModelProperties(mSession, path, &properties)) || properties.modelKey == XR_NULL_RENDER_MODEL_KEY_FB) {
      continue;
    }
    XrRenderModelLoadInfoFB load{XR_TYPE_RENDER_MODEL_LOAD_INFO_FB};
    load.modelKey = properties.modelKey;
    XrRenderModelBufferFB buffer{XR_TYPE_RENDER_MODEL_BUFFER_FB};
    if (XR_FAILED(mLoadModel(mSession, &load, &buffer)) || buffer.bufferCountOutput == 0) continue;
    std::vector<uint8_t> bytes(buffer.bufferCountOutput);
    buffer.bufferCapacityInput = static_cast<uint32_t>(bytes.size());
    buffer.buffer = bytes.data();
    if (XR_FAILED(mLoadModel(mSession, &load, &buffer))) continue;
    GltfMesh mesh;
    std::string error;
    if (!read_glb(bytes.data(), bytes.size(), mesh, error)) {
      LOGW("Hands: %s model %s unreadable (%s), the drawn shape stays", hand == 0 ? "left" : "right",
           properties.modelName, error.c_str());
      model.attempts = 10;
      continue;
    }
    glGenVertexArrays(1, &model.vao);
    glBindVertexArray(model.vao);
    glGenBuffers(1, &model.vertices);
    glBindBuffer(GL_ARRAY_BUFFER, model.vertices);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(mesh.vertices.size() * sizeof(float)),
                 mesh.vertices.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &model.indices);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, model.indices);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(mesh.indices.size() * sizeof(uint32_t)),
                 mesh.indices.data(), GL_STATIC_DRAW);
    constexpr GLsizei stride = GltfMesh::kFloatsPerVertex * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(6 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    model.count = static_cast<GLsizei>(mesh.indices.size());
    for (int corner = 0; corner < 8; ++corner) {
      const float x = (corner & 1) ? mesh.high[0] : mesh.low[0];
      const float y = (corner & 2) ? mesh.high[1] : mesh.low[1];
      const float z = (corner & 4) ? mesh.high[2] : mesh.low[2];
      model.radius = std::max(model.radius, std::sqrt(x * x + y * y + z * z));
    }
    model.loaded = true;
    LOGI("Hands: %s controller model %s, %zu vertices, %zu triangles, %.3f m", hand == 0 ? "left" : "right",
         properties.modelName, mesh.vertex_count(), mesh.indices.size() / 3, model.radius);
  }
}

// A unit sphere: every part is one, stretched and turned.
void HandsView::create_sphere() {
  constexpr int kRings = 12, kSegments = 20;
  std::vector<float> vertices;
  std::vector<uint16_t> indices;
  for (int ring = 0; ring <= kRings; ++ring) {
    const float theta = static_cast<float>(M_PI) * ring / kRings;
    for (int segment = 0; segment <= kSegments; ++segment) {
      const float phi = 2.0f * static_cast<float>(M_PI) * segment / kSegments;
      vertices.push_back(std::sin(theta) * std::cos(phi));
      vertices.push_back(std::cos(theta));
      vertices.push_back(std::sin(theta) * std::sin(phi));
    }
  }
  for (int ring = 0; ring < kRings; ++ring) {
    for (int segment = 0; segment < kSegments; ++segment) {
      const auto a = static_cast<uint16_t>(ring * (kSegments + 1) + segment);
      const auto b = static_cast<uint16_t>(a + kSegments + 1);
      indices.insert(indices.end(), {a, b, static_cast<uint16_t>(a + 1), static_cast<uint16_t>(a + 1), b,
                                     static_cast<uint16_t>(b + 1)});
    }
  }
  mIndexCount = static_cast<GLsizei>(indices.size());
  glGenVertexArrays(1, &mVao);
  glBindVertexArray(mVao);
  glGenBuffers(1, &mVertices);
  glBindBuffer(GL_ARRAY_BUFFER, mVertices);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)), vertices.data(),
               GL_STATIC_DRAW);
  glGenBuffers(1, &mIndices);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mIndices);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(uint16_t)), indices.data(),
               GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void HandsView::track_hands(XrSpace base, XrTime time) {
  for (int hand = 0; hand < 2; ++hand) {
    Hand& h = mHands[hand];
    HandPinch& pinch = mPinch[hand];
    h.active = false;
    if (h.tracker != XR_NULL_HANDLE) {
      XrHandJointsLocateInfoEXT info{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
      info.baseSpace = base;
      info.time = time;
      XrHandJointLocationsEXT locations{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
      locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
      locations.jointLocations = h.joints.data();
      h.active = XR_SUCCEEDED(mLocateJoints(h.tracker, &info, &locations)) && locations.isActive &&
                 joint_valid(h.joints[XR_HAND_JOINT_PALM_EXT]);
    }
    pinch.tracked = h.active && joint_valid(h.joints[XR_HAND_JOINT_THUMB_TIP_EXT]) &&
                    joint_valid(h.joints[XR_HAND_JOINT_INDEX_TIP_EXT]);
    if (!pinch.tracked) {
      pinch.pinching = false;
      continue;
    }
    const XrVector3f thumb = h.joints[XR_HAND_JOINT_THUMB_TIP_EXT].pose.position;
    const XrVector3f index = h.joints[XR_HAND_JOINT_INDEX_TIP_EXT].pose.position;
    const float apart = length(sub(thumb, index));
    pinch.pinching = apart < (pinch.pinching ? kPinchOffMeters : kPinchOnMeters);
    pinch.pose.position = {(thumb.x + index.x) * 0.5f, (thumb.y + index.y) * 0.5f, (thumb.z + index.z) * 0.5f};
    pinch.pose.orientation = h.joints[XR_HAND_JOINT_PALM_EXT].pose.orientation;
  }
}

// A Touch controller, roughly: in the grip pose's frame (OpenXR), -Z runs up
// the handle (the rod held in the fist), +Y over the thumb. The handle is
// centered on the grip; the face caps its top, turned halfway between the
// handle's axis and +Y (held, it faces up while the handle leans forward);
// the trigger under it, where the index rests. The first version had the
// face tilted the other way and the handle behind the hand (2026-10-09).
void HandsView::add_controller(int hand, const ControllerPose& controller) {
  // The headset's own model when it has given it: in the grip pose's frame,
  // or the aim pose's (`adb shell setprop debug.partyboard.controller_space
  // aim`, read every 2 s), which Meta's documents do not settle.
  if (mModels[hand].loaded) {
    const auto now = std::chrono::steady_clock::now();
    if (now - mModelSpaceReadAt >= std::chrono::seconds(2)) {
      mModelSpaceReadAt = now;
      char value[PROP_VALUE_MAX] = {};
      __system_property_get("debug.partyboard.controller_space", value);
      mModelOnAim = std::strcmp(value, "aim") == 0;
    }
    const XrPosef& pose = mModelOnAim && controller.aimKnown ? controller.aim : controller.grip;
    const float r = mModels[hand].radius;
    const std::array<float, 4> lit{1.0f + controller.trigger * 0.6f, 1.0f + controller.trigger * 0.9f,
                                   1.0f + controller.trigger * 1.6f, 1.0f};
    mParts.push_back({pose, {r, r, r}, lit, hand});
    return;
  }
  const std::array<float, 4> body{0.018f, 0.018f, 0.022f, 1.0f};
  const std::array<float, 4> face{0.03f, 0.03f, 0.036f, 1.0f};
  const std::array<float, 4> stick{0.006f, 0.006f, 0.007f, 1.0f};
  const std::array<float, 4> lit{0.12f, 0.35f, 1.0f, 1.0f};
  const auto part = [&](XrVector3f at, XrQuaternionf turn, XrVector3f size, std::array<float, 4> color) {
    XrPosef local{turn, at};
    mParts.push_back({compose(controller.grip, local), size, color});
  };
  const XrQuaternionf none{0.0f, 0.0f, 0.0f, 1.0f};
  part({0.0f, -0.002f, 0.008f}, none, {0.017f, 0.02f, 0.05f}, mix(body, lit, controller.squeeze * 0.6f));
  const XrQuaternionf faceTurn = turn_x(-0.75f);
  const XrVector3f faceAt{0.0f, 0.01f, -0.05f};
  part(faceAt, faceTurn, {0.034f, 0.012f, 0.036f}, face);
  const XrVector3f up = rotate(faceTurn, {0.0f, 0.012f, 0.0f});
  part(add(faceAt, up), faceTurn, {0.008f, 0.005f, 0.008f}, stick);
  part({0.0f, -0.024f, -0.042f}, faceTurn, {0.008f, 0.01f, 0.014f}, mix(body, lit, controller.trigger));
}

// A tracked hand: its joints and bones, a light translucent glove.
void HandsView::add_hand(int hand) {
  const Hand& h = mHands[hand];
  const std::array<float, 4> skin{0.78f, 0.82f, 0.9f, 0.55f};
  const std::array<float, 4> held{0.35f, 0.6f, 1.0f, 0.7f};
  const std::array<float, 4> tip{0.25f, 0.55f, 1.0f, 0.9f};
  const auto color = mGrabbing[hand] ? held : skin;
  const auto& joints = h.joints;
  if (joint_valid(joints[XR_HAND_JOINT_PALM_EXT])) {
    mParts.push_back({joints[XR_HAND_JOINT_PALM_EXT].pose, {0.04f, 0.012f, 0.045f}, color});
  }
  const bool pinching = mPinch[hand].pinching;
  for (int joint = XR_HAND_JOINT_THUMB_METACARPAL_EXT; joint < XR_HAND_JOINT_COUNT_EXT; ++joint) {
    if (!joint_valid(joints[joint])) continue;
    const float radius = std::clamp(joints[joint].radius, 0.004f, 0.015f);
    const bool fingertip = joint == XR_HAND_JOINT_THUMB_TIP_EXT || joint == XR_HAND_JOINT_INDEX_TIP_EXT;
    mParts.push_back({joints[joint].pose, {radius, radius, radius}, pinching && fingertip ? tip : color});
  }
  // The bones: each finger from its first joint past the palm to its tip.
  const auto bone = [&](int from, int to) {
    if (!joint_valid(joints[from]) || !joint_valid(joints[to])) return;
    const XrVector3f a = joints[from].pose.position, b = joints[to].pose.position;
    const XrVector3f d = sub(b, a);
    const float len = length(d);
    if (len < 1e-4f) return;
    const float radius = std::clamp((joints[from].radius + joints[to].radius) * 0.45f, 0.004f, 0.012f);
    XrPosef pose;
    pose.position = {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f};
    pose.orientation = turn_z_to({d.x / len, d.y / len, d.z / len});
    mParts.push_back({pose, {radius, radius, len * 0.5f + radius * 0.3f}, color});
  };
  for (int joint = XR_HAND_JOINT_THUMB_METACARPAL_EXT; joint < XR_HAND_JOINT_THUMB_TIP_EXT; ++joint) bone(joint, joint + 1);
  for (int first : {XR_HAND_JOINT_INDEX_PROXIMAL_EXT, XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT, XR_HAND_JOINT_RING_PROXIMAL_EXT,
                    XR_HAND_JOINT_LITTLE_PROXIMAL_EXT}) {
    for (int joint = first; joint < first + 3; ++joint) bone(joint, joint + 1);
  }
}

const XrCompositionLayerBaseHeader* HandsView::layer(XrSpace space, const XrView (&views)[2],
                                                     const std::array<ControllerPose, 2>& controllers) {
  if (mSwapchain == XR_NULL_HANDLE || mProgram == 0) return nullptr;
  if ((controllers[0].tracked && !mModels[0].loaded) || (controllers[1].tracked && !mModels[1].loaded)) load_models();
  mParts.clear();
  for (int hand = 0; hand < 2; ++hand) {
    // A hand holding its controller is shown as the controller.
    if (controllers[hand].tracked) {
      add_controller(hand, controllers[hand]);
    } else if (mHands[hand].active) {
      add_hand(hand);
    }
  }
  if (mParts.empty()) return nullptr;

  // Each eye's field of view narrowed to the parts it sees.
  std::array<XrFovf, 2> fovs{};
  bool anyVisible = false;
  for (int eye = 0; eye < 2; ++eye) {
    const XrPosef toEye = inverse(views[eye].pose);
    float left = 1e9f, right = -1e9f, down = 1e9f, up = -1e9f;
    bool whole = false;
    for (const Part& p : mParts) {
      const XrVector3f c = add(toEye.position, rotate(toEye.orientation, p.pose.position));
      const float r = std::max({p.size.x, p.size.y, p.size.z});
      const float depth = -c.z;
      if (depth < r + kNear) {
        whole = true; // around the eye: all of its view
        break;
      }
      const float spreadX = std::asin(std::min(r / std::sqrt(c.x * c.x + depth * depth), 1.0f));
      const float spreadY = std::asin(std::min(r / std::sqrt(c.y * c.y + depth * depth), 1.0f));
      const float ax = std::atan2(c.x, depth), ay = std::atan2(c.y, depth);
      left = std::min(left, ax - spreadX);
      right = std::max(right, ax + spreadX);
      down = std::min(down, ay - spreadY);
      up = std::max(up, ay + spreadY);
    }
    const XrFovf& full = views[eye].fov;
    XrFovf fov = full;
    if (!whole) {
      fov.angleLeft = std::max(full.angleLeft, left - kFovMargin);
      fov.angleRight = std::min(full.angleRight, right + kFovMargin);
      fov.angleDown = std::max(full.angleDown, down - kFovMargin);
      fov.angleUp = std::min(full.angleUp, up + kFovMargin);
    }
    if (fov.angleRight - fov.angleLeft < 0.01f || fov.angleUp - fov.angleDown < 0.01f) {
      // Out of this eye's view: a sliver at its center, drawn empty.
      fov = {-0.005f, 0.005f, 0.005f, -0.005f};
    } else {
      anyVisible = true;
    }
    fovs[eye] = fov;
  }
  if (!anyVisible) return nullptr;

  uint32_t index = 0;
  XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait.timeout = XR_INFINITE_DURATION;
  if (XR_FAILED(xrAcquireSwapchainImage(mSwapchain, &acquire, &index))) return nullptr;
  if (XR_FAILED(xrWaitSwapchainImage(mSwapchain, &wait))) {
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(mSwapchain, &release);
    return nullptr;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, mFramebuffer);
  if (mSamples > 1) {
    mTextureMultisample(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mImages[index].image, 0, mSamples);
  } else {
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mImages[index].image, 0);
  }
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, mDepth);
  const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  if (complete) {
    const auto size = static_cast<GLsizei>(mEyePixels);
    glViewport(0, 0, size * 2, size);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(mProgram);
    glUniform1f(mMeshLoc, 0.0f);
    glBindVertexArray(mVao);
    glEnable(GL_DEPTH_TEST);
    for (int eye = 0; eye < 2; ++eye) {
      glViewport(eye * size, 0, size, size);
      const Mat4 view = pose_matrix(inverse(views[eye].pose), {1.0f, 1.0f, 1.0f});
      const Mat4 viewProj = multiply(projection(fovs[eye]), view);
      glUniformMatrix4fv(mViewProjLoc, 1, GL_FALSE, viewProj.data());
      const XrVector3f eyeAt = views[eye].pose.position;
      glUniform3f(mEyeLoc, eyeAt.x, eyeAt.y, eyeAt.z);
      // Depth first, then each pixel's nearest part once: the translucent
      // hands show no inner overlaps.
      for (int pass = 0; pass < 2; ++pass) {
        if (pass == 0) {
          glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
          glDepthFunc(GL_LESS);
          glDepthMask(GL_TRUE);
          glDisable(GL_BLEND);
        } else {
          glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
          glDepthFunc(GL_EQUAL);
          glDepthMask(GL_FALSE);
        }
        for (const Part& p : mParts) {
          if (p.model >= 0) {
            const Model& m = mModels[p.model];
            const Mat4 model = pose_matrix(p.pose, {1.0f, 1.0f, 1.0f});
            glBindVertexArray(m.vao);
            glUniform1f(mMeshLoc, 1.0f);
            glUniformMatrix4fv(mModelLoc, 1, GL_FALSE, model.data());
            glUniform3f(mInvScaleLoc, 1.0f, 1.0f, 1.0f);
            glUniform4f(mColorLoc, p.color[0], p.color[1], p.color[2], p.color[3]);
            glDrawElements(GL_TRIANGLES, m.count, GL_UNSIGNED_INT, nullptr);
            glBindVertexArray(mVao);
            glUniform1f(mMeshLoc, 0.0f);
            continue;
          }
          const Mat4 model = pose_matrix(p.pose, p.size);
          glUniformMatrix4fv(mModelLoc, 1, GL_FALSE, model.data());
          glUniform3f(mInvScaleLoc, 1.0f / p.size.x, 1.0f / p.size.y, 1.0f / p.size.z);
          glUniform4f(mColorLoc, p.color[0], p.color[1], p.color[2], p.color[3]);
          glDrawElements(GL_TRIANGLES, mIndexCount, GL_UNSIGNED_SHORT, nullptr);
        }
      }
    }
    const GLenum discard = GL_DEPTH_ATTACHMENT;
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &discard);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(0);
    glUseProgram(0);
  }
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glFlush();
  XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  xrReleaseSwapchainImage(mSwapchain, &release);
  if (!complete) {
    static bool logged = false;
    if (!logged) LOGW("Hands: framebuffer incomplete");
    logged = true;
    return nullptr;
  }

  for (int eye = 0; eye < 2; ++eye) {
    auto& v = mViews[eye];
    v = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    v.pose = views[eye].pose;
    v.fov = fovs[eye];
    v.subImage.swapchain = mSwapchain;
    v.subImage.imageRect = {{static_cast<int32_t>(eye * mEyePixels), 0},
                            {static_cast<int32_t>(mEyePixels), static_cast<int32_t>(mEyePixels)}};
  }
  mLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  mLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  mLayer.space = space;
  mLayer.viewCount = 2;
  mLayer.views = mViews.data();
  return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&mLayer);
}

} // namespace quest
