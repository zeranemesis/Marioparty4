#pragma once

// The game's 3D world as a model on the table, seen in stereo (quest_stereo.cpp
// in the game draws it; this shows it).
//
// Images go around a ring shared with the game through AHardwareBuffers:
//   free -> drawing (the game took it for a frame, with the eye poses of that
//   moment) -> ready (Aurora submitted it, with a fence) -> copied into the
//   OpenXR swapchain and shown with those same poses -> copying -> free
//   (only when the GPU has finished reading the source).
// Showing each image with the poses it was drawn for lets the compositor
// correct the head's movement since then, so the model stays put on the table.

#include "xr_util.hpp"

#include <EGL/eglext.h>
#include <android/hardware_buffer.h>

#include <array>
#include <cstdint>
#include <chrono>
#include <mutex>
#include <vector>

namespace quest {

// One game frame's view: same layout as QuestStereoFrame in src/port/quest_stereo.cpp.
struct StereoFrame {
  uint32_t image;
  uint64_t tag;
  float eyeView[2][16]; // room (STAGE) -> eye, row-major
  float eyeProj[2][16]; // eye -> GX clip space (z/w from -1 at near to 0 at far)
  float world[16];      // game world -> room
  float hudWidth, hudHeight;
};

class StereoView {
public:
  // `scale`: the eye images' size relative to the headset's recommended one.
  bool init(XrInstance instance, XrSession session, XrSystemId system, EGLDisplay display, float scale);
  void destroy();

  // XR thread, each frame: where the eyes are, and the model (game world ->
  // room: `world` pose, `scale` meters per game unit). Disabled: the game draws no eyes.
  void update(const XrView (&views)[2], const XrPosef& world, float scale, bool enabled, float hudWidth = 0.76f, float hudHeight = 0.57f);
  // XR thread: the newest image the game drew, copied into the swapchain; the
  // layer showing it (or the previous one), nullptr before the first.
  const XrCompositionLayerBaseHeader* layer(XrSpace space, const void* next);
  const XrCompositionLayerBaseHeader* hud_layer(XrSpace viewSpace, const void* next);

  // Game thread (PartyBoardQuest_StereoFrame): an image and the view to draw it with.
  bool game_frame(StereoFrame& out);
  // Game thread: the images, to register with Aurora.
  bool images(void** buffers, uint32_t capacity, uint32_t& count, uint32_t& width, uint32_t& height,
              uint32_t& generation, uint32_t& eyeHeight, uint32_t& hudWidth, uint32_t& hudHeight);
  uint32_t generation() const;
  // Aurora's render thread: the frame drawing `image` went to the GPU.
  void submitted(uint32_t image, uint64_t tag, int syncFd, bool hasWorld = false);
  void set_screen_required(bool required);
  void set_board_mode(bool board);
  bool screen_required() const;
  bool world_only() const;
  bool world_visible() const { return mShown && mShownHasWorld; }
  // A camera with no view never enqueues GPU work: return its lease explicitly.
  void cancelled(uint32_t image, uint64_t tag);

private:
  friend struct StereoViewTestAccess;
  enum class State { Free, Drawing, Ready, Copying };
  struct Slot {
    AHardwareBuffer* buffer = nullptr;
    EGLImageKHR eglImage = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
    State state = State::Free;
    uint64_t tag = 0;
    int fence = -1;
    bool hasWorld = false;
    bool board = false;
    GLsync copyFence = nullptr;
    XrView views[2]{};
  };

  void release_slot(Slot& slot);

  EGLDisplay mDisplay = EGL_NO_DISPLAY;
  XrInstance mInstance = XR_NULL_HANDLE;
  XrSwapchain mSwapchain = XR_NULL_HANDLE;
  XrSwapchain mHudSwapchain = XR_NULL_HANDLE;
  std::vector<XrSwapchainImageOpenGLESKHR> mHudImages;
  uint32_t mHudPixelsWidth = 1280, mHudPixelsHeight = 960;
  bool mHudShown = false;
  XrCompositionLayerQuad mHudLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
  std::vector<XrSwapchainImageOpenGLESKHR> mSwapchainImages;
  uint32_t mEyeWidth = 0;
  uint32_t mEyeHeight = 0;

  mutable std::mutex mMutex;
  std::array<Slot, 3> mSlots;
  uint32_t mGeneration = 0;
  uint64_t mNextTag = 1;
  uint32_t mLeaseCount = 0, mRingFullCount = 0, mPresentedCount = 0;
  double mCopyMaxMs = 0;
  std::chrono::steady_clock::time_point mStatsAt = std::chrono::steady_clock::now();
  bool mEnabled = false;
  bool mScreenRequired = false;
  bool mBoardMode = false;
  bool mShownIsBoard = false;
  float mHudWidth = 0.76f, mHudHeight = 0.57f;
  XrView mViews[2]{};
  float mWorld[16]{};

  // The last image shown, resubmitted until the game draws the next one.
  bool mShown = false;
  bool mShownHasWorld = false;
  XrView mShownViews[2]{};
  XrCompositionLayerProjectionView mProjectionViews[2]{};
  XrCompositionLayerProjection mLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
};

// The one the exported functions (PartyBoardQuest_Stereo*) reach; null without a session.
extern StereoView* g_stereoView;

} // namespace quest
