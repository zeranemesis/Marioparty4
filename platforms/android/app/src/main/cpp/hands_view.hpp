#pragma once

// The player's controllers and hands, drawn over the game: the board and the
// screen hide the real ones the room's view would show. A projection layer of
// its own, drawn every headset frame at the controllers' current poses (the
// game's images come more slowly), its field of view narrowed to what it
// shows so the compositor only blends that part.
//
// The hands (XR_EXT_hand_tracking) also pinch: thumb and index together.

#include "xr_util.hpp"
#include "gltf_mesh.hpp"

#include <array>
#include <chrono>
#include <vector>

namespace quest {

struct ControllerPose {
  bool tracked = false;
  XrPosef grip{};        // the grip pose (OpenXR's /input/grip/pose)
  XrPosef aim{};         // the aim pose, when known
  bool aimKnown = false;
  float trigger = 0.0f;  // 0..1, lights the trigger
  float squeeze = 0.0f;  // 0..1, lights the handle
};

struct HandPinch {
  bool tracked = false;
  bool pinching = false;
  XrPosef pose{}; // between thumb and index tip, oriented like the palm
};

class HandsView {
public:
  static constexpr const char* kExtension = XR_EXT_HAND_TRACKING_EXTENSION_NAME;
  static constexpr const char* kRenderModelExtension = XR_FB_RENDER_MODEL_EXTENSION_NAME;

  // `handTracking`: the extension was enabled. False when GL lacks what it needs.
  // `renderModels`: XR_FB_render_model enabled, the controllers' own models.
  bool init(XrInstance instance, XrSystemId system, XrSession session, int64_t swapchainFormat, bool handTracking,
            bool renderModels = false);
  void destroy(); // GL objects: while the context is current

  // The hands at `time` (call once a frame, before pinch() and layer()).
  void track_hands(XrSpace base, XrTime time);
  const HandPinch& pinch(int hand) const { return mPinch[hand]; }
  bool hand_tracked(int hand) const { return mHands[hand].active; }
  // The hands lit up as holding something.
  void set_grabbing(int hand, bool grabbing) { mGrabbing[hand] = grabbing; }

  // Draws them; nullptr when nothing is in view. `views`: the eyes at the
  // frame's display time, in `space`.
  const XrCompositionLayerBaseHeader* layer(XrSpace space, const XrView (&views)[2],
                                            const std::array<ControllerPose, 2>& controllers);

private:
  struct Part {
    XrPosef pose;       // center and orientation
    XrVector3f size;    // the ellipsoid's half-axes (a model: its radius, for the field of view)
    std::array<float, 4> color; // linear, alpha not premultiplied (a model: a tint)
    int model = -1;     // a controller's render model (mModels) instead of a sphere
  };
  // A controller's model from the headset (XR_FB_render_model).
  struct Model {
    GLuint vao = 0, vertices = 0, indices = 0;
    GLsizei count = 0;
    float radius = 0.0f;
    bool loaded = false;
    int attempts = 0;
  };
  struct Hand {
    XrHandTrackerEXT tracker = XR_NULL_HANDLE;
    bool active = false;
    std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT> joints{};
  };

  void add_controller(int hand, const ControllerPose& controller);
  void load_models();
  void add_hand(int hand);
  bool create_program();
  void create_sphere();

  XrInstance mInstance = XR_NULL_HANDLE;
  XrSession mSession = XR_NULL_HANDLE;
  PFN_xrCreateHandTrackerEXT mCreateTracker = nullptr;
  PFN_xrDestroyHandTrackerEXT mDestroyTracker = nullptr;
  PFN_xrLocateHandJointsEXT mLocateJoints = nullptr;
  std::array<Hand, 2> mHands{};
  std::array<Model, 2> mModels{};
  PFN_xrGetRenderModelPropertiesFB mModelProperties = nullptr;
  PFN_xrLoadRenderModelFB mLoadModel = nullptr;
  std::chrono::steady_clock::time_point mModelsTriedAt{};
  bool mModelOnAim = false; // debug.partyboard.controller_space aim
  std::chrono::steady_clock::time_point mModelSpaceReadAt{};
  std::array<HandPinch, 2> mPinch{};
  std::array<bool, 2> mGrabbing{};

  XrSwapchain mSwapchain = XR_NULL_HANDLE;
  std::vector<XrSwapchainImageOpenGLESKHR> mImages;
  uint32_t mEyePixels = 0;
  GLuint mProgram = 0, mVao = 0, mVertices = 0, mIndices = 0, mFramebuffer = 0, mDepth = 0;
  GLsizei mIndexCount = 0;
  GLint mViewProjLoc = -1, mModelLoc = -1, mInvScaleLoc = -1, mColorLoc = -1, mEyeLoc = -1, mMeshLoc = -1;
  GLsizei mSamples = 1;
  using FramebufferTexture2DMultisample = void (*)(GLenum, GLenum, GLenum, GLuint, GLint, GLsizei);
  using RenderbufferStorageMultisample = void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
  FramebufferTexture2DMultisample mTextureMultisample = nullptr;
  RenderbufferStorageMultisample mStorageMultisample = nullptr;

  std::vector<Part> mParts;
  XrCompositionLayerProjection mLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  std::array<XrCompositionLayerProjectionView, 2> mViews{
      {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}}};
};

} // namespace quest
