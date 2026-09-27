#include "stereo_view.hpp"

#include <GLES2/gl2ext.h>
#include <unistd.h>
#include <poll.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace quest {

StereoView* g_stereoView = nullptr;

namespace {

// The eyes' depth range, in meters: from a hand's width to across the room.
constexpr float kNear = 0.05f;
constexpr float kFar = 100.0f;

// Dynamic resolution never draws less than half of the eyes' images.

// The head is predicted at most this far ahead (nanoseconds).
constexpr double kMaxPredictionNs = 50e6;

constexpr GLenum kSrgb8Alpha8 = 0x8C43; // GL_SRGB8_ALPHA8
constexpr GLenum kRgba8 = 0x8058;       // GL_RGBA8

using CopyImageSubData = void(GL_APIENTRYP)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint,
                                            GLint, GLint, GLsizei, GLsizei, GLsizei);

struct Gl {
  PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC getNativeClientBuffer = nullptr;
  PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTargetTexture = nullptr;
  PFNEGLCREATESYNCKHRPROC createSync = nullptr;
  PFNEGLDESTROYSYNCKHRPROC destroySync = nullptr;
  PFNEGLWAITSYNCKHRPROC waitSync = nullptr;
  CopyImageSubData copyImage = nullptr;
  PFNGLGETQUERYOBJECTUI64VEXTPROC queryResult64 = nullptr;
  PFNGLQUERYCOUNTEREXTPROC queryCounter = nullptr;

  bool load() {
    getNativeClientBuffer =
        reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    imageTargetTexture =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    createSync = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
    destroySync = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
    waitSync = reinterpret_cast<PFNEGLWAITSYNCKHRPROC>(eglGetProcAddress("eglWaitSyncKHR"));
    copyImage = reinterpret_cast<CopyImageSubData>(eglGetProcAddress("glCopyImageSubData"));
    queryResult64 = reinterpret_cast<PFNGLGETQUERYOBJECTUI64VEXTPROC>(eglGetProcAddress("glGetQueryObjectui64vEXT"));
    queryCounter = reinterpret_cast<PFNGLQUERYCOUNTEREXTPROC>(eglGetProcAddress("glQueryCounterEXT"));
    return getNativeClientBuffer && createImage && destroyImage && imageTargetTexture && createSync && destroySync &&
           waitSync && copyImage;
  }
};
Gl g_gl;

// Row-major 4x4, column vectors.
void pose_matrix(const XrPosef& pose, float scale, float out[16]) {
  const XrQuaternionf q = pose.orientation;
  const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
  const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
  const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
  const float r[3][3] = {
      {1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy)},
      {2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx)},
      {2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)},
  };
  const float t[3] = {pose.position.x, pose.position.y, pose.position.z};
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      out[row * 4 + col] = r[row][col] * scale;
    }
    out[row * 4 + 3] = t[row];
  }
  out[12] = out[13] = out[14] = 0.0f;
  out[15] = 1.0f;
}

void view_matrix(const XrPosef& eye, float out[16]) { pose_matrix(inverse(eye), 1.0f, out); }

// GX's perspective form (MTXFrustum) for the eye's field of view.
void projection_matrix(const XrFovf& fov, float out[16]) {
  const float l = kNear * std::tan(fov.angleLeft);
  const float r = kNear * std::tan(fov.angleRight);
  const float b = kNear * std::tan(fov.angleDown);
  const float t = kNear * std::tan(fov.angleUp);
  std::fill(out, out + 16, 0.0f);
  out[0] = 2.0f * kNear / (r - l);
  out[2] = (r + l) / (r - l);
  out[5] = 2.0f * kNear / (t - b);
  out[6] = (t + b) / (t - b);
  out[10] = -kNear / (kFar - kNear);
  out[11] = -(kFar * kNear) / (kFar - kNear);
  out[14] = -1.0f;
}

} // namespace

bool StereoView::init(XrInstance instance, XrSession session, XrSystemId system, EGLDisplay display, float scale) {
  mInstance = instance;
  mSession = session;
  mDisplay = display;
  mMaxScale = scale;
  if (!g_gl.load()) {
    LOGW("Stereo: GL extensions missing");
    return false;
  }

  char timerProperty[PROP_VALUE_MAX]{};
  __system_property_get("debug.partyboard.gpu_timing", timerProperty);
  const auto* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
  mGpuTiming = timerProperty[0] == '1' && g_gl.queryResult64 && g_gl.queryCounter && extensions &&
      std::strstr(extensions, "GL_EXT_disjoint_timer_query");
  if (mGpuTiming) for (auto& timer : mCopyTimers) glGenQueries(2, timer.queries);

  uint32_t viewCount = 0;
  XrViewConfigurationView configViews[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
  if (!check(instance,
             xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                               &viewCount, configViews),
             "xrEnumerateViewConfigurationViews") ||
      viewCount != 2) {
    return false;
  }
  const auto round8 = [](float v) { return static_cast<uint32_t>(std::lround(v / 8.0f)) * 8u; };
  mRecommendedEyeWidth = configViews[0].recommendedImageRectWidth;
  mEyeWidth = std::min(round8(configViews[0].recommendedImageRectWidth * scale),
                       configViews[0].maxImageRectWidth / 8u * 8u);
  mEyeHeight = std::min(round8(configViews[0].recommendedImageRectHeight * scale),
                        configViews[0].maxImageRectHeight / 8u * 8u);

  // The swapchain the images are copied into: sRGB, so the compositor reads
  // the game's colors as they are (RGBA8 bytes, already gamma-encoded).
  uint32_t formatCount = 0;
  xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr);
  std::vector<int64_t> formats(formatCount);
  xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data());
  const int64_t format =
      std::ranges::find(formats, kSrgb8Alpha8) != formats.end() ? kSrgb8Alpha8 : static_cast<int64_t>(kRgba8);
  mSwapchainFormat = format;
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  info.format = format;
  info.sampleCount = 1;
  info.width = mEyeWidth * 2;
  info.height = mEyeHeight;
  info.faceCount = 1;
  info.arraySize = 1;
  info.mipCount = 1;
  if (!check(instance, xrCreateSwapchain(session, &info, &mSwapchain), "xrCreateSwapchain (stereo)")) {
    return false;
  }
  uint32_t imageCount = 0;
  xrEnumerateSwapchainImages(mSwapchain, 0, &imageCount, nullptr);
  mSwapchainImages.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
  xrEnumerateSwapchainImages(mSwapchain, imageCount, &imageCount,
                             reinterpret_cast<XrSwapchainImageBaseHeader*>(mSwapchainImages.data()));

  // The interface's text: 2.5 pixels per game pixel, sharp on the quad.
  mHudPixelsWidth = std::min(1600u, mEyeWidth * 2);
  mHudPixelsHeight = mHudPixelsWidth * 3 / 4;
  info.width = mHudPixelsWidth;
  info.height = mHudPixelsHeight;
  if (!check(instance, xrCreateSwapchain(session, &info, &mHudSwapchain), "xrCreateSwapchain (HUD)")) {
    destroy();
    return false;
  }
  xrEnumerateSwapchainImages(mHudSwapchain, 0, &imageCount, nullptr);
  mHudImages.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
  xrEnumerateSwapchainImages(mHudSwapchain, imageCount, &imageCount,
                            reinterpret_cast<XrSwapchainImageBaseHeader*>(mHudImages.data()));

  // The images the game draws into, exactly the size it draws.
  const auto [width, height] = eye_size(mRenderScale);
  if (!allocate_images(width, height)) {
    destroy();
    return false;
  }
  g_stereoView = this;
  LOGI("Stereo: eyes %ux%u, swapchain format 0x%llx", mEyeWidth, mEyeHeight, static_cast<unsigned long long>(format));
  return true;
}

std::pair<uint32_t, uint32_t> StereoView::eye_size(float renderScale) const {
  const auto round8 = [](float v) { return std::max(8u, static_cast<uint32_t>(std::lround(v / 8.0f)) * 8u); };
  return {std::min(mEyeWidth, round8(static_cast<float>(mEyeWidth) * renderScale)),
          std::min(mEyeHeight, round8(static_cast<float>(mEyeHeight) * renderScale))};
}

// The ring's images at exactly the size the game draws (the eyes side by
// side, the interface below): the GPU writes whole images out at the end of
// its pass, so a larger image would cost bandwidth for pixels nobody drew.
// XR thread, GL context current, every slot free.
bool StereoView::allocate_images(uint32_t eyeWidth, uint32_t eyeHeight, uint32_t hudWidth) {
  if (!hudWidth) hudWidth = mHudPixelsWidth;
  XrSwapchain hudReplacement = XR_NULL_HANDLE;
  std::vector<XrSwapchainImageOpenGLESKHR> hudImages;
  if (hudWidth != mHudPixelsWidth) {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = mSwapchainFormat;
    info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
    info.width = hudWidth;
    info.height = hudWidth * 3 / 4;
    if (XR_FAILED(xrCreateSwapchain(mSession, &info, &hudReplacement))) return false;
    uint32_t count = 0;
    if (XR_FAILED(xrEnumerateSwapchainImages(hudReplacement, 0, &count, nullptr)) || count == 0) {
      xrDestroySwapchain(hudReplacement);
      return false;
    }
    hudImages.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    if (XR_FAILED(xrEnumerateSwapchainImages(hudReplacement, count, &count,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(hudImages.data())))) {
      xrDestroySwapchain(hudReplacement);
      return false;
    }
  }
  std::array<Slot, 3> replacement;
  for (auto& slot : replacement) {
    AHardwareBuffer_Desc desc{};
    desc.width = std::max(eyeWidth * 2, hudWidth);
    desc.height = eyeHeight + hudWidth * 3 / 4;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    if (AHardwareBuffer_allocate(&desc, &slot.buffer) != 0) {
      LOGW("Stereo: image allocation failed");
      free_images(replacement);
      if (hudReplacement != XR_NULL_HANDLE) xrDestroySwapchain(hudReplacement);
      return false;
    }
    const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    slot.eglImage = g_gl.createImage(mDisplay, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                     g_gl.getNativeClientBuffer(slot.buffer), attributes);
    if (slot.eglImage == EGL_NO_IMAGE_KHR) {
      free_images(replacement);
      if (hudReplacement != XR_NULL_HANDLE) xrDestroySwapchain(hudReplacement);
      return false;
    }
    glGenTextures(1, &slot.texture);
    glBindTexture(GL_TEXTURE_2D, slot.texture);
    g_gl.imageTargetTexture(GL_TEXTURE_2D, static_cast<GLeglImageOES>(slot.eglImage));
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  std::lock_guard lock{mMutex};
  free_images();
  mSlots = std::move(replacement);
  if (hudReplacement != XR_NULL_HANDLE) {
    xrDestroySwapchain(mHudSwapchain);
    mHudSwapchain = hudReplacement;
    mHudImages = std::move(hudImages);
    mHudPixelsWidth = hudWidth;
    mHudPixelsHeight = hudWidth * 3 / 4;
    mHudShown = false;
  }
  mImageEyeWidth = eyeWidth;
  mImageEyeHeight = eyeHeight;
  ++mGeneration; // the game imports the new images (quest_stereo.cpp)
  return true;
}

void StereoView::free_images() { free_images(mSlots); }

void StereoView::free_images(std::array<Slot, 3>& slots) {
  for (auto& slot : slots) {
    if (slot.texture != 0) {
      glDeleteTextures(1, &slot.texture);
      slot.texture = 0;
    }
    if (slot.eglImage != EGL_NO_IMAGE_KHR) {
      g_gl.destroyImage(mDisplay, slot.eglImage);
      slot.eglImage = EGL_NO_IMAGE_KHR;
    }
    if (slot.buffer != nullptr) {
      AHardwareBuffer_release(slot.buffer); // Dawn keeps its own reference while it uses it
      slot.buffer = nullptr;
    }
  }
}

// A resize waits for every image to be free: none drawn, shown or copied.
bool StereoView::resize_ready() {
  std::lock_guard lock{mMutex};
  return mResizePending && std::ranges::all_of(mSlots, [](const Slot& slot) { return slot.state == State::Free; });
}

void StereoView::release_slot(Slot& slot) {
  if (slot.fence >= 0) {
    close(slot.fence);
    slot.fence = -1;
  }
  slot.state = State::Free;
}

void StereoView::destroy() {
  g_stereoView = nullptr;
  // Session shutdown may wait; the frame loop must not wait for copies.
  glFinish();
  for (auto& timer : mCopyTimers) {
    if (timer.queries[0]) glDeleteQueries(2, timer.queries);
    timer = {};
  }
  mGpuTiming = false;
  std::lock_guard lock{mMutex};
  for (auto& slot : mSlots) {
    if (slot.copyFence != nullptr) {
      glDeleteSync(slot.copyFence);
      slot.copyFence = nullptr;
    }
    release_slot(slot);
  }
  free_images();
  if (mSwapchain != XR_NULL_HANDLE) {
    xrDestroySwapchain(mSwapchain);
    mSwapchain = XR_NULL_HANDLE;
  }
  if (mHudSwapchain != XR_NULL_HANDLE) {
    xrDestroySwapchain(mHudSwapchain);
    mHudSwapchain = XR_NULL_HANDLE;
  }
  mHudShown = false;
  mShown = false;
  mPresentedTag = 0;
}

void StereoView::update(const XrView (&views)[2], const XrPosef& world, float scale, bool enabled, float hudWidth, float hudHeight) {
  std::lock_guard lock{mMutex};
  mViews[0] = views[0];
  mViews[1] = views[1];
  pose_matrix(world, scale, mWorld);
  mEnabled = enabled && mSwapchain != XR_NULL_HANDLE;
  mHudWidth = hudWidth;
  mHudHeight = hudHeight;
}

bool StereoView::game_frame(StereoFrame& out) {
  std::lock_guard lock{mMutex};
  if (!mEnabled || mScreenRequired || mResizePending) {
    return false; // resizing: the ring drains, then new images come (layer())
  }
  // Never reclaim Drawing by age: a queued GPU frame can still own it.
  // Cameras without a view cancel explicitly; empty eye passes clear and submit.
  const auto free = std::ranges::find_if(mSlots, [](const Slot& slot) { return slot.state == State::Free; });
  if (free == mSlots.end()) {
    ++mRingFullCount;
    ++mAdaptRingFull;
    return false; // the headset is behind: this frame goes to the flat screen only
  }
  ++mLeaseCount;
  ++mAdaptLeases;
  free->renderWidth = mImageEyeWidth;
  free->renderHeight = mImageEyeHeight;
  free->leaseTime = mFrameTime;
  out.generation = mGeneration;
  free->state = State::Drawing;
  free->tag = mNextTag++;
  free->views[0] = mViews[0];
  free->views[1] = mViews[1];
  free->board = mBoardMode;
  out.image = static_cast<uint32_t>(free - mSlots.begin());
  out.tag = free->tag;
  for (int eye = 0; eye < 2; ++eye) {
    view_matrix(mViews[eye].pose, out.eyeView[eye]);
    projection_matrix(mViews[eye].fov, out.eyeProj[eye]);
  }
  std::copy(std::begin(mWorld), std::end(mWorld), out.world);
  out.hudWidth = mHudWidth;
  out.hudHeight = mHudHeight;
  out.eyeWidth = free->renderWidth;
  out.eyeHeight = free->renderHeight;
  return true;
}

void StereoView::set_frame_time(XrTime time) {
  std::lock_guard lock{mMutex};
  mFrameTime = time;
}

XrDuration StereoView::prediction() const {
  std::lock_guard lock{mMutex};
  return static_cast<XrDuration>(std::clamp(mLatencyNs, 0.0, kMaxPredictionNs));
}

void StereoView::set_screen_hidden(bool hidden) {
  std::lock_guard lock{mMutex};
  mScreenHidden = hidden;
}

bool StereoView::screen_hidden() const {
  std::lock_guard lock{mMutex};
  return mScreenHidden;
}

void StereoView::set_quality_sample(const QualitySample& sample) {
  std::lock_guard lock{mMutex};
  mQualitySample = sample;
}

void StereoView::set_quality_cap(float recommendedScale) {
  std::lock_guard lock{mMutex};
  mQuality.set_cap(recommendedScale);
  mDesiredHudWidth = recommendedScale <= 0.8f ? 1280 : recommendedScale <= 1.0f ? 1600 : 1920;
  mRenderScale = std::min(1.0f, mQuality.scale() / mMaxScale);
  if (eye_size(mRenderScale) != std::make_pair(mImageEyeWidth, mImageEyeHeight) ||
      mDesiredHudWidth != mHudPixelsWidth) mResizePending = true;
}

// One decision per fresh one-second metric window; do not lower resolution
// merely because the CPU or the image bridge is late while the GPU has headroom.
void StereoView::adapt_resolution() {
  const auto now = std::chrono::steady_clock::now();
  if (now - mAdaptAt < std::chrono::seconds(1)) {
    return;
  }
  mAdaptAt = now;
  const float before = mRenderScale;
  const uint32_t wanted = mAdaptLeases + mAdaptRingFull;
  const float measuredScale = mQuality.update(mQualitySample, wanted, mAdaptRingFull);
  mRenderScale = std::min(1.0f, measuredScale / mMaxScale);
  mAdaptLeases = mAdaptRingFull = 0;
  if (mRenderScale != before && eye_size(mRenderScale) != std::make_pair(mImageEyeWidth, mImageEyeHeight)) {
    mResizePending = true; // layer() reallocates once the ring is empty
  }
  if (mRenderScale != before) {
    LOGI("Stereo: resolution %.0f%% of recommended (%ux%u per eye)", mRenderScale * mMaxScale * 100.0f,
         static_cast<unsigned>(mEyeWidth * mRenderScale), static_cast<unsigned>(mEyeHeight * mRenderScale));
  }
}

bool StereoView::images(void** buffers, uint32_t capacity, uint32_t& count, uint32_t& width, uint32_t& height,
                        uint32_t& generation, uint32_t& eyeHeight, uint32_t& hudWidth, uint32_t& hudHeight) {
  std::lock_guard lock{mMutex};
  if (capacity < mSlots.size() || mSlots[0].buffer == nullptr) {
    return false;
  }
  for (size_t i = 0; i < mSlots.size(); ++i) {
    buffers[i] = mSlots[i].buffer;
  }
  count = static_cast<uint32_t>(mSlots.size());
  width = std::max(mImageEyeWidth * 2, mHudPixelsWidth);
  height = mImageEyeHeight + mHudPixelsHeight;
  eyeHeight = mImageEyeHeight;
  hudWidth = mHudPixelsWidth;
  hudHeight = mHudPixelsHeight;
  generation = mGeneration;
  return true;
}

float StereoView::resolution_percent() const {
  std::lock_guard lock{mMutex};
  const auto width = mShownHasWorld && mShownWidth ? mShownWidth : mImageEyeWidth;
  return mRecommendedEyeWidth ? 100.0f * width / mRecommendedEyeWidth : 0;
}

uint32_t StereoView::generation() const {
  std::lock_guard lock{mMutex};
  return mGeneration;
}

void StereoView::set_screen_required(bool required) {
  std::lock_guard lock{mMutex};
  mScreenRequired = required;
}

void StereoView::set_board_mode(bool board) {
  std::lock_guard lock{mMutex};
  mBoardMode = board;
}

bool StereoView::world_only() const {
  std::lock_guard lock{mMutex};
  return mEnabled && !mScreenRequired;
}

bool StereoView::screen_required() const {
  std::lock_guard lock{mMutex};
  return mScreenRequired;
}

void StereoView::submitted(uint32_t image, uint64_t tag, int syncFd, bool hasWorld) {
  std::lock_guard lock{mMutex};
  if (image >= mSlots.size() || mSlots[image].state != State::Drawing || mSlots[image].tag != tag) {
    if (syncFd >= 0) {
      close(syncFd);
    }
    return;
  }
  mSlots[image].state = State::Ready;
  mSlots[image].fence = syncFd;
  mSlots[image].hasWorld = hasWorld;
}

void StereoView::cancelled(uint32_t image, uint64_t tag) {
  std::lock_guard lock{mMutex};
  if (image < mSlots.size() && mSlots[image].state == State::Drawing && mSlots[image].tag == tag) {
    release_slot(mSlots[image]);
  }
}

std::array<float, 3> StereoView::render_info() const {
  std::lock_guard lock{mMutex};
  return {static_cast<float>(mShownHasWorld && mShownWidth ? mShownWidth : mImageEyeWidth),
          static_cast<float>(mShownHasWorld && mShownHeight ? mShownHeight : mImageEyeHeight),
          mEnabled ? mWorldRate : 0};
}

StereoView::Slot* StereoView::newest_completed() {
  Slot* newest = nullptr;
  for (auto& slot : mSlots) {
    if (slot.state != State::Ready || slot.tag <= mPresentedTag) continue;
    pollfd finished{slot.fence, POLLIN, 0};
    const bool complete = slot.fence < 0 ||
        (poll(&finished, 1, 0) > 0 && (finished.revents & POLLIN));
    if (complete && (!newest || slot.tag > newest->tag)) newest = &slot;
  }
  return newest;
}

const XrCompositionLayerBaseHeader* StereoView::layer(XrSpace space, const void* next) {
  if (mSwapchain == XR_NULL_HANDLE) {
    return nullptr;
  }
  CopyTimer* copyTimer = nullptr;
  if (mGpuTiming) {
    GLint disjoint = 0;
    glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
    for (auto& timer : mCopyTimers) {
      if (timer.pending) {
        GLuint available = 0;
        glGetQueryObjectuiv(timer.queries[1], GL_QUERY_RESULT_AVAILABLE, &available);
        if (available) {
          GLuint64 start = 0, end = 0;
          g_gl.queryResult64(timer.queries[0], GL_QUERY_RESULT, &start);
          g_gl.queryResult64(timer.queries[1], GL_QUERY_RESULT, &end);
          if (!disjoint && end >= start) {
            mCopyGpuMaxMs = std::max(mCopyGpuMaxMs, (end - start) / 1e6);
            ++mCopyGpuSamples;
          }
          timer.pending = false;
        }
      }
      if (!timer.pending && !copyTimer) copyTimer = &timer;
    }
    if (disjoint) { mCopyGpuMaxMs = -1; mCopyGpuSamples = 0; }
  }
  Slot* newest = nullptr;
  int fence = -1;
  {
    std::lock_guard lock{mMutex};
    for (auto& slot : mSlots) {
      if (slot.state == State::Copying && slot.copyFence != nullptr) {
        const GLenum status = glClientWaitSync(slot.copyFence, 0, 0);
        if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) {
          glDeleteSync(slot.copyFence);
          slot.copyFence = nullptr;
          release_slot(slot);
        }
      }

    }
    // Do not queue a GPU wait on unfinished game work into the XR copy stream.
    // Resubmit the previous layer until at least one source is complete.
    newest = newest_completed();
    // Older finished images will never be shown now.
    for (auto& slot : mSlots) {
      if (slot.state == State::Ready &&
          (slot.tag <= mPresentedTag || (newest && slot.tag < newest->tag))) {
        // Ready means submitted, not completed. Dropping a fence does not
        // make its image reusable while Aurora's GPU can still write to it.
        pollfd finished{slot.fence, POLLIN, 0};
        if (slot.fence < 0 || (poll(&finished, 1, 0) > 0 && (finished.revents & POLLIN))) {
          release_slot(slot);
        }
      }
    }
    if (newest != nullptr) {
      fence = newest->fence;
      newest->fence = -1;
    }
  }

  // Dynamic resolution changed: new images at the new size, once every image
  // is back (game_frame() gives no lease meanwhile).
  if (newest == nullptr && resize_ready()) {
    const auto [width, height] = eye_size(mRenderScale);
    if (allocate_images(width, height, mDesiredHudWidth)) {
      LOGI("Stereo: images now %ux%u per eye, HUD %ux%u", width, height, mHudPixelsWidth, mHudPixelsHeight);
    } else {
      // Retain the previous valid allocation instead of leaving a dead ring.
      mRenderScale = static_cast<float>(mImageEyeWidth) / mEyeWidth;
      LOGW("Stereo: resize failed; retaining previous eye images");
    }
    std::lock_guard lock{mMutex};
    mResizePending = false;
  }

  if (newest != nullptr) {
    const auto copyStart = std::chrono::steady_clock::now();
    GLsync copyFence = nullptr;
    bool accepted = false;
    // The GPU waits for Aurora's drawing; no CPU stall here.
    if (fence >= 0) {
      const EGLint attributes[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fence, EGL_NONE};
      EGLSyncKHR sync = g_gl.createSync(mDisplay, EGL_SYNC_NATIVE_FENCE_ANDROID, attributes);
      if (sync != EGL_NO_SYNC_KHR) {
        g_gl.waitSync(mDisplay, sync, 0);
        g_gl.destroySync(mDisplay, sync);
      } else {
        close(fence);
      }
    }
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    const bool keepBoard = mShown && newest->board && !newest->hasWorld && mShownHasWorld && mShownIsBoard;
    const auto acquireImage = [&](XrSwapchain swapchain, uint32_t& image) {
      const auto start = std::chrono::steady_clock::now();
      const bool ready = XR_SUCCEEDED(xrAcquireSwapchainImage(swapchain, &acquire, &image)) &&
                         XR_SUCCEEDED(xrWaitSwapchainImage(swapchain, &wait));
      mAcquireMaxMs = std::max(mAcquireMaxMs,
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
      return ready;
    };
    if (keepBoard || acquireImage(mSwapchain, index)) {
      uint32_t hudIndex = 0;
      const auto hudNow = std::chrono::steady_clock::now();
      const bool copyHud = !mHudShown || hudNow - mHudCopiedAt >= std::chrono::milliseconds(33);
      const bool hudReady = copyHud && acquireImage(mHudSwapchain, hudIndex);
      // Acquire both destinations before queuing copies, then flush once.
      if (copyTimer) g_gl.queryCounter(copyTimer->queries[0], GL_TIMESTAMP_EXT);

      if (!keepBoard) {
        // Only the drawn part of each eye's half (dynamic resolution): at the
        // left, centered vertically (Aurora's common.cpp draws it there).
        // The image holds just what was drawn (Aurora puts each eye at the
        // left of its half, a half being the image's width / 2).
        const auto y = static_cast<GLint>((mEyeHeight - newest->renderHeight) / 2);
        const uint32_t srcStride = std::max(newest->renderWidth * 2, mHudPixelsWidth) / 2;
        for (uint32_t eye = 0; eye < 2; ++eye) {
          g_gl.copyImage(newest->texture, GL_TEXTURE_2D, 0, static_cast<GLint>(eye * srcStride), 0, 0,
                         mSwapchainImages[index].image, GL_TEXTURE_2D, 0, static_cast<GLint>(eye * mEyeWidth), y, 0,
                         static_cast<GLsizei>(newest->renderWidth), static_cast<GLsizei>(newest->renderHeight), 1);
        }
      }
      if (hudReady) {
        mHudCopiedAt = hudNow;
        g_gl.copyImage(newest->texture, GL_TEXTURE_2D, 0, 0, static_cast<GLint>(newest->renderHeight), 0,
                      mHudImages[hudIndex].image, GL_TEXTURE_2D, 0, 0, 0, 0,
                      static_cast<GLsizei>(mHudPixelsWidth), static_cast<GLsizei>(mHudPixelsHeight), 1);

      }
      // Keep the source lease until this GPU copy completes. The compositor
      // synchronizes the released destination; the game must not reuse its source.
      if (copyTimer) {
        g_gl.queryCounter(copyTimer->queries[1], GL_TIMESTAMP_EXT);
        copyTimer->pending = true;
      }
      copyFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
      if (copyFence != nullptr) {
        glFlush();
      } else {
        LOGW("Stereo: copy fence unavailable, using synchronous fallback");
        glFinish();
      }
      XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      if (hudReady) {
        mHudShown = XR_SUCCEEDED(xrReleaseSwapchainImage(mHudSwapchain, &release));
        if (mHudShown) ++mNewHudCount;
      }
      accepted = keepBoard || XR_SUCCEEDED(xrReleaseSwapchainImage(mSwapchain, &release));
      if (accepted && !keepBoard) {
        ++mNewWorldCount;
        mShownViews[0] = newest->views[0];
        mShownViews[1] = newest->views[1];
        mShownHasWorld = newest->hasWorld;
        mShownIsBoard = newest->board;
        mShownWidth = newest->renderWidth;
        mShownHeight = newest->renderHeight;
      }
      // How long images take from the game's lease to the display.
      if (newest->leaseTime > 0 && mFrameTime > newest->leaseTime) {
        const double sample = static_cast<double>(mFrameTime - newest->leaseTime);
        mLatencyNs = mLatencyNs == 0.0 ? sample : mLatencyNs * 0.9 + sample * 0.1;
      }
      if (accepted) {
        mShown = true;
        mPresentedTag = newest->tag;
      }
      static bool loggedFirstImage = false;
      if (!loggedFirstImage) {
        LOGI("Stereo: first eye image copied to OpenXR (tag %llu)", static_cast<unsigned long long>(newest->tag));
        loggedFirstImage = true;
      }
    }
    std::lock_guard lock{mMutex};
    mCopyMaxMs = std::max(mCopyMaxMs,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - copyStart).count());
    if (copyFence != nullptr) {
      newest->copyFence = copyFence;
      newest->state = State::Copying;
    } else {
      release_slot(*newest);
    }
    if (accepted) ++mPresentedCount;
  }

  {
    std::lock_guard lock{mMutex};
    const auto now = std::chrono::steady_clock::now();
    adapt_resolution();
    const double elapsed = std::chrono::duration<double>(now - mStatsAt).count();
    if (elapsed >= 2.0) {
      unsigned drawing = 0, ready = 0, copying = 0;
      for (const auto& slot : mSlots) {
        drawing += slot.state == State::Drawing;
        ready += slot.state == State::Ready;
        copying += slot.state == State::Copying;
      }
      LOGI("Stereo perf: source=%.1fHz presented=%.1fHz ringFull=%u copyMax=%.2fms slots=%u/%u/%u world=%d "
           "res=%.0f%% latency=%.1fms screenHidden=%d acquireCpuMax=%.2fms worldNew=%.1fHz hudNew=%.1fHz copyGpuMax=%.3fms gpuSamples=%u eye=%ux%u",
          mLeaseCount / elapsed, mPresentedCount / elapsed, mRingFullCount, mCopyMaxMs,
          drawing, ready, copying, mShownHasWorld, mRenderScale * mMaxScale * 100.0f, mLatencyNs / 1e6, mScreenHidden, mAcquireMaxMs, mNewWorldCount / elapsed, mNewHudCount / elapsed, mCopyGpuMaxMs, mCopyGpuSamples,
          mShownWidth, mShownHeight);
      mWorldRate = static_cast<float>(mNewWorldCount / elapsed);
      mStatsAt = now;
      mLeaseCount = mRingFullCount = mPresentedCount = 0;
      mCopyMaxMs = mAcquireMaxMs = 0;
      mNewWorldCount = mNewHudCount = 0;
      mCopyGpuMaxMs = -1; mCopyGpuSamples = 0;
    }
  }
  if (!mShown) {
    return nullptr;
  }
  for (uint32_t eye = 0; eye < 2; ++eye) {
    auto& view = mProjectionViews[eye];
    view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    view.pose = mShownViews[eye].pose;
    view.fov = mShownViews[eye].fov;
    view.subImage.swapchain = mSwapchain;
    const uint32_t shownWidth = mShownWidth != 0 ? mShownWidth : mEyeWidth;
    const uint32_t shownHeight = mShownHeight != 0 ? mShownHeight : mEyeHeight;
    view.subImage.imageRect = {{static_cast<int32_t>(eye * mEyeWidth), static_cast<int32_t>((mEyeHeight - shownHeight) / 2)},
                               {static_cast<int32_t>(shownWidth), static_cast<int32_t>(shownHeight)}};
  }
  mLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  mLayer.next = next;
  // Where nothing of the model was drawn, the room shows through.
  mLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  mLayer.space = space;
  mLayer.viewCount = 2;
  mLayer.views = mProjectionViews;
  return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&mLayer);
}

const XrCompositionLayerBaseHeader* StereoView::hud_layer(XrSpace viewSpace, const void* next) {
  if (!world_visible() || !mHudShown) return nullptr;
  mHudLayer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
  mHudLayer.next = next;
  mHudLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  mHudLayer.space = viewSpace;
  mHudLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  mHudLayer.subImage.swapchain = mHudSwapchain;
  mHudLayer.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(mHudPixelsWidth), static_cast<int32_t>(mHudPixelsHeight)}};
  mHudLayer.pose.orientation.w = 1.f;
  mHudLayer.pose.position = {0.f, -0.12f, -0.7f};
  mHudLayer.size = {0.72f, 0.54f};
  return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&mHudLayer);
}

} // namespace quest

// The game side (src/port/quest_stereo.cpp) finds these with dlsym.
extern "C" {

__attribute__((visibility("default"))) bool PartyBoardQuest_StereoScreenHidden() {
  return quest::g_stereoView != nullptr && quest::g_stereoView->screen_hidden();
}

__attribute__((visibility("default"))) bool PartyBoardQuest_StereoWorldOnly() {
  return quest::g_stereoView != nullptr && quest::g_stereoView->world_only();
}

__attribute__((visibility("default"))) void PartyBoardQuest_StereoScreenRequired(bool required) {
  if (quest::g_stereoView != nullptr) quest::g_stereoView->set_screen_required(required);
}

__attribute__((visibility("default"))) void PartyBoardQuest_StereoBoardMode(bool board) {
  if (quest::g_stereoView != nullptr) quest::g_stereoView->set_board_mode(board);
}

__attribute__((visibility("default"))) void PartyBoardQuest_StereoCancelled(uint32_t image, uint64_t tag) {
  if (quest::g_stereoView != nullptr) {
    quest::g_stereoView->cancelled(image, tag);
  }
}

__attribute__((visibility("default"))) bool PartyBoardQuest_StereoFrame(quest::StereoFrame* out) {
  quest::StereoView* view = quest::g_stereoView;
  return view != nullptr && out != nullptr && view->game_frame(*out);
}

__attribute__((visibility("default"))) bool PartyBoardQuest_StereoImages(void** buffers, uint32_t capacity,
                                                                         uint32_t* count, uint32_t* width,
                                                                         uint32_t* height, uint32_t* generation,
                                                                         uint32_t* eyeHeight, uint32_t* hudWidth, uint32_t* hudHeight) {
  quest::StereoView* view = quest::g_stereoView;
  return view != nullptr && view->images(buffers, capacity, *count, *width, *height, *generation,
                                        *eyeHeight, *hudWidth, *hudHeight);
}

__attribute__((visibility("default"))) uint32_t PartyBoardQuest_StereoGeneration(void) {
  quest::StereoView* view = quest::g_stereoView;
  return view != nullptr ? view->generation() : 0;
}

__attribute__((visibility("default"))) void PartyBoardQuest_StereoSubmitted(uint32_t image, uint64_t tag, int syncFd, bool hasWorld,
                                                                            void*) {
  quest::StereoView* view = quest::g_stereoView;
  if (view != nullptr) {
    view->submitted(image, tag, syncFd, hasWorld);
  } else if (syncFd >= 0) {
    close(syncFd);
  }
}

} // extern "C"
