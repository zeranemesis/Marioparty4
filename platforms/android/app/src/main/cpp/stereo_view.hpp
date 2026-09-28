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
#include "adaptive_quality.hpp"

#include <EGL/eglext.h>
#include <android/hardware_buffer.h>

#include <array>
#include <cstdint>
#include <chrono>
#include <mutex>
#include <utility>
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
  uint32_t eyeWidth, eyeHeight; // drawn part of each eye's half (dynamic resolution)
  uint32_t generation;          // of the images the lease is in
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
  // The game's interface (scores, messages) as a quad at `pose` in `space`:
  // quest_xr.cpp stands it at the back of the model. nullptr while hidden.
  const XrCompositionLayerBaseHeader* hud_layer(XrSpace space, const XrPosef& pose, XrExtent2Df size,
                                                const void* next);

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

  // XR thread, each frame before update(): the display time being prepared.
  void set_frame_time(XrTime time);
  // How far ahead of this frame the game's next image will be seen: measured
  // from the images already shown, for the eye poses given to the game.
  XrDuration prediction() const;
  // The flat screen is not on show (the model replaces it): the game skips presenting it.
  void set_screen_hidden(bool hidden);
  bool screen_hidden() const;
  void set_quality_sample(const QualitySample& sample);
  void set_quality_cap(float recommendedScale);

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
    uint32_t renderWidth = 0, renderHeight = 0; // drawn part of each eye's half
    XrTime leaseTime = 0;                       // the XR frame the game took it in
  };

  // Dynamic resolution, once a second (caller holds mMutex).
  void adapt_resolution();
  // The drawn eye size for a render scale, and the ring's images at a size
  // (XR thread, GL context current, every image free).
  std::pair<uint32_t, uint32_t> eye_size(float renderScale) const;
  bool allocate_images(uint32_t eyeWidth, uint32_t eyeHeight, uint32_t hudWidth = 0);
  void free_images();
  bool resize_ready();
  void free_images(std::array<Slot, 3>& slots);

public:
  // The eyes' resolution, in % of the headset's recommended size.
  float resolution_percent() const;
  std::array<float, 3> render_info() const; // actual eye width/height, fresh world Hz

private:

  void release_slot(Slot& slot);
  Slot* newest_completed(); // Caller holds mMutex.

  EGLDisplay mDisplay = EGL_NO_DISPLAY;
  XrInstance mInstance = XR_NULL_HANDLE;
  XrSession mSession = XR_NULL_HANDLE;
  XrSwapchain mSwapchain = XR_NULL_HANDLE;
  XrSwapchain mHudSwapchain = XR_NULL_HANDLE;
  std::vector<XrSwapchainImageOpenGLESKHR> mHudImages;
  uint32_t mHudPixelsWidth = 1600, mHudPixelsHeight = 1200;
  bool mHudShown = false;
  XrCompositionLayerQuad mHudLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
  std::vector<XrSwapchainImageOpenGLESKHR> mSwapchainImages;
  uint32_t mEyeWidth = 0; // an eye's half of the swapchain: the largest drawn size
  uint32_t mEyeHeight = 0;
  uint32_t mRecommendedEyeWidth = 0;
  uint32_t mImageEyeWidth = 0, mImageEyeHeight = 0; // the ring's images: the drawn size
  bool mResizePending = false;
  uint32_t mDesiredHudWidth = 1600;
  int64_t mSwapchainFormat = 0;

  mutable std::mutex mMutex;
  std::array<Slot, 3> mSlots;
  uint32_t mGeneration = 0;
  uint64_t mNextTag = 1;
  uint32_t mLeaseCount = 0, mRingFullCount = 0, mPresentedCount = 0;
  uint32_t mNewWorldCount = 0, mNewHudCount = 0;
  float mWorldRate = 0;
  double mCopyMaxMs = 0;
  double mAcquireMaxMs = 0;
  std::array<double, 2> mDestinationAcquireMaxMs{}; // world, HUD; XR calls only
  std::array<double, 2> mDestinationWaitMaxMs{};
  struct CopyTimer { GLuint queries[2]{}; bool pending = false; };
  std::array<CopyTimer, 4> mCopyTimers;
  bool mGpuTiming = false;
  double mCopyGpuMaxMs = -1;
  uint32_t mCopyGpuSamples = 0;
  std::chrono::steady_clock::time_point mStatsAt = std::chrono::steady_clock::now();
  bool mEnabled = false;
  bool mScreenRequired = false;
  bool mBoardMode = false;
  bool mShownIsBoard = false;
  float mHudWidth = 0.76f, mHudHeight = 0.57f;
  XrView mViews[2]{};
  float mWorld[16]{};

  // Dynamic resolution: the drawn part of the eyes' images, 0.5 to 1 of
  // their size, lowered when the GPU falls behind 120 Hz, raised back slowly.
  float mRenderScale = 0.8f; // of 125% of the recommended size: 100% of it, then up as the GPU allows
  float mMaxScale = 1.0f;    // the images' size, relative to the recommended one
  AdaptiveQuality mQuality;
  QualitySample mQualitySample;
  uint32_t mAdaptLeases = 0, mAdaptRingFull = 0, mCalmSeconds = 0;
  // The intervals between the game's image requests this second, and the
  // hitches counted in the last one (adapt_resolution()).
  std::chrono::steady_clock::time_point mLastRequestAt{};
  std::vector<float> mRequestIntervals;
  uint32_t mLastSlow = 0;
  std::chrono::steady_clock::time_point mAdaptAt = std::chrono::steady_clock::now();
  // Pose prediction: this XR frame's display time, and the measured delay
  // between taking an image and showing it (moving average, nanoseconds).
  XrTime mFrameTime = 0;
  double mLatencyNs = 0;
  bool mScreenHidden = false;
  // The interface is copied at 30 Hz: text and scores need no more.
  std::chrono::steady_clock::time_point mHudCopiedAt{};

  // The last image shown, resubmitted until the game draws the next one.
  uint64_t mPresentedTag = 0;
  bool mShown = false;
  bool mShownHasWorld = false;
  XrView mShownViews[2]{};
  uint32_t mShownWidth = 0, mShownHeight = 0;
  XrCompositionLayerProjectionView mProjectionViews[2]{};
  XrCompositionLayerProjection mLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
};

// The one the exported functions (PartyBoardQuest_Stereo*) reach; null without a session.
extern StereoView* g_stereoView;

} // namespace quest
