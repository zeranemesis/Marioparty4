#include "stereo_view.hpp"

#include <GLES2/gl2ext.h>
#include <unistd.h>
#include <poll.h>
#include <linux/sync_file.h>
#include <sys/ioctl.h>
#include <sys/system_properties.h>
#include <cstdlib>

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

int64_t steady_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// When a signaled sync file's fences signaled (CLOCK_MONOTONIC, the steady
// clock's), the latest of them; 0 when unknown.
int64_t fence_signal_time_ns(int fd) {
  if (fd < 0) return 0;
  sync_fence_info fences[8]{};
  sync_file_info info{};
  info.num_fences = 8;
  info.sync_fence_info = reinterpret_cast<uintptr_t>(fences);
  if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) != 0 || info.status != 1) return 0;
  int64_t latest = 0;
  for (uint32_t i = 0; i < std::min(info.num_fences, 8u); ++i) {
    latest = std::max(latest, static_cast<int64_t>(fences[i].timestamp_ns));
  }
  return latest;
}

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

  char ringProperty[PROP_VALUE_MAX]{};
  __system_property_get("debug.partyboard.ring", ringProperty);
  if (const int ring = std::atoi(ringProperty); ring >= 2 && ring <= 6) mSlotCount = static_cast<size_t>(ring);
  LOGI("Stereo: ring of %zu images (debug.partyboard.ring=%s)", mSlotCount, ringProperty[0] != '\0' ? ringProperty : "unset");
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
  create_hud_screen();

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

// The ring's images at the size the game draws: the eyes side by side, and
// the interface in an image of its own. The GPU clears and writes whole
// images out at the end of its pass, so a larger image costs bandwidth for
// pixels nobody drew: with the interface below the eyes, 41% of the eyes'
// multisampled pass was the interface or unused (3360x2960 for 3360x1760 of
// eyes, 2026-09-29). A smaller drawn size uses a part of them for a while
// (ImageSizePolicy).
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
  // A shared buffer, its EGL image and the GL texture the copies read.
  const auto make = [this](uint32_t width, uint32_t height, AHardwareBuffer*& buffer, EGLImageKHR& eglImage,
                           GLuint& texture) {
    AHardwareBuffer_Desc desc{};
    desc.width = width;
    desc.height = height;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    if (AHardwareBuffer_allocate(&desc, &buffer) != 0) {
      LOGW("Stereo: image allocation failed (%ux%u)", width, height);
      return false;
    }
    const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    eglImage = g_gl.createImage(mDisplay, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                g_gl.getNativeClientBuffer(buffer), attributes);
    if (eglImage == EGL_NO_IMAGE_KHR) {
      return false;
    }
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    g_gl.imageTargetTexture(GL_TEXTURE_2D, static_cast<GLeglImageOES>(eglImage));
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
  };
  std::vector<Slot> replacement(mSlotCount);
  for (auto& slot : replacement) {
    if (!make(eyeWidth * 2, eyeHeight, slot.buffer, slot.eglImage, slot.texture) ||
        !make(hudWidth, hudWidth * 3 / 4, slot.hudBuffer, slot.hudEglImage, slot.hudTexture)) {
      free_images(replacement);
      if (hudReplacement != XR_NULL_HANDLE) xrDestroySwapchain(hudReplacement);
      return false;
    }
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

void StereoView::free_images(std::vector<Slot>& slots) {
  const auto release = [this](AHardwareBuffer*& buffer, EGLImageKHR& eglImage, GLuint& texture) {
    if (texture != 0) {
      glDeleteTextures(1, &texture);
      texture = 0;
    }
    if (eglImage != EGL_NO_IMAGE_KHR) {
      g_gl.destroyImage(mDisplay, eglImage);
      eglImage = EGL_NO_IMAGE_KHR;
    }
    if (buffer != nullptr) {
      AHardwareBuffer_release(buffer); // Dawn keeps its own reference while it uses it
      buffer = nullptr;
    }
  };
  for (auto& slot : slots) {
    release(slot.buffer, slot.eglImage, slot.texture);
    release(slot.hudBuffer, slot.hudEglImage, slot.hudTexture);
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
  if (mHudScreenSwapchain != XR_NULL_HANDLE) {
    xrDestroySwapchain(mHudScreenSwapchain);
    mHudScreenSwapchain = XR_NULL_HANDLE;
  }
  mHudShown = false;
  mShown = false;
  mPresentedTag = 0;
}

void StereoView::load_hidden_area(bool available) {
  if (!available) {
    LOGI("Hidden area: XR_KHR_visibility_mask unavailable, every pixel is drawn");
    return;
  }
  PFN_xrGetVisibilityMaskKHR getMask = nullptr;
  if (XR_FAILED(xrGetInstanceProcAddr(mInstance, "xrGetVisibilityMaskKHR",
                                      reinterpret_cast<PFN_xrVoidFunction*>(&getMask))) ||
      getMask == nullptr) {
    LOGW("Hidden area: xrGetVisibilityMaskKHR missing");
    return;
  }
  // One mask of an eye, its points in index order (empty: none).
  const auto read = [&](uint32_t eye, XrVisibilityMaskTypeKHR type, XrResult& result) {
    std::vector<XrVector2f> points;
    XrVisibilityMaskKHR mask{XR_TYPE_VISIBILITY_MASK_KHR};
    result = getMask(mSession, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, type, &mask);
    if (XR_FAILED(result) || mask.vertexCountOutput == 0 || mask.indexCountOutput == 0) return points;
    std::vector<XrVector2f> vertices(mask.vertexCountOutput);
    std::vector<uint32_t> indices(mask.indexCountOutput);
    mask.vertexCapacityInput = mask.vertexCountOutput;
    mask.vertices = vertices.data();
    mask.indexCapacityInput = mask.indexCountOutput;
    mask.indices = indices.data();
    result = getMask(mSession, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, type, &mask);
    if (XR_FAILED(result)) return points;
    for (const uint32_t index : indices) {
      if (index < vertices.size()) points.push_back(vertices[index]);
    }
    return points;
  };
  // The visible outline (whatever is outside it is hidden), else the hidden
  // triangles: the runtime may give either.
  std::vector<XrVector2f> outline[2], triangles[2];
  for (uint32_t eye = 0; eye < 2; ++eye) {
    XrResult loopResult = XR_SUCCESS, meshResult = XR_SUCCESS;
    outline[eye] = read(eye, XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR, loopResult);
    if (outline[eye].size() < 3) {
      outline[eye].clear();
      triangles[eye] = read(eye, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, meshResult);
      triangles[eye].resize(triangles[eye].size() / 3 * 3);
    }
    LOGI("Hidden area, eye %u: outline %zu points (result %d), hidden mesh %zu triangles (result %d)", eye,
         outline[eye].size(), loopResult, triangles[eye].size() / 3, meshResult);
    if (outline[eye].empty() && triangles[eye].empty()) {
      LOGW("Hidden area: none given for eye %u, every pixel is drawn", eye);
      return;
    }
  }
  std::lock_guard lock{mMutex};
  for (int eye = 0; eye < 2; ++eye) {
    mVisibleOutline[eye] = std::move(outline[eye]);
    mHiddenTriangles[eye] = std::move(triangles[eye]);
  }
  mHiddenPending = true;
}

namespace {

// Rectangles of an eye's image (0..1, from its top-left) the lenses never
// show, on a grid. With the visible outline, a cell counts when no edge of
// the outline crosses it and its center is outside; with the hidden
// triangles, when one triangle holds its four corners. Each row's runs of
// such cells are one rectangle, and a run equal to the one above extends it.
std::vector<float> hidden_rects(const std::vector<XrVector2f>& outline, const std::vector<XrVector2f>& triangles,
                                const XrFovf& fov, float& hiddenPart) {
  constexpr int kGrid = 48;
  const float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight);
  const float d = std::tan(fov.angleDown), u = std::tan(fov.angleUp);
  const auto to_image = [&](const XrVector2f& v) { return std::array<float, 2>{(v.x - l) / (r - l), (u - v.y) / (u - d)}; };
  std::vector<std::array<float, 2>> points, corners;
  points.reserve(outline.size());
  for (const auto& v : outline) points.push_back(to_image(v));
  corners.reserve(triangles.size());
  for (const auto& v : triangles) corners.push_back(to_image(v));
  // Whether (x, y) is inside triangle t, edges included.
  const auto in_triangle = [&](size_t t, float x, float y) {
    const auto& a = corners[t];
    const auto& b = corners[t + 1];
    const auto& c = corners[t + 2];
    const float d1 = (x - b[0]) * (a[1] - b[1]) - (a[0] - b[0]) * (y - b[1]);
    const float d2 = (x - c[0]) * (b[1] - c[1]) - (b[0] - c[0]) * (y - c[1]);
    const float d3 = (x - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (y - a[1]);
    return !((d1 < 0.f || d2 < 0.f || d3 < 0.f) && (d1 > 0.f || d2 > 0.f || d3 > 0.f));
  };
  const auto inside = [&](float x, float y) {
    bool in = false;
    for (size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
      const auto& a = points[i];
      const auto& b = points[j];
      if ((a[1] > y) != (b[1] > y) && x < (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]) + a[0]) in = !in;
    }
    return in;
  };
  // Liang-Barsky: whether segment a-b reaches the box.
  const auto crosses = [](const std::array<float, 2>& a, const std::array<float, 2>& b, float x0, float y0, float x1,
                          float y1) {
    float t0 = 0.f, t1 = 1.f;
    const float dx = b[0] - a[0], dy = b[1] - a[1];
    const float p[4] = {-dx, dx, -dy, dy};
    const float q[4] = {a[0] - x0, x1 - a[0], a[1] - y0, y1 - a[1]};
    for (int k = 0; k < 4; ++k) {
      if (p[k] == 0.f) {
        if (q[k] < 0.f) return false;
      } else {
        const float t = q[k] / p[k];
        if (p[k] < 0.f) t0 = std::max(t0, t);
        else t1 = std::min(t1, t);
        if (t0 > t1) return false;
      }
    }
    return true;
  };
  std::vector<float> rects;
  std::vector<std::array<int, 2>> above;
  std::vector<size_t> aboveRect;
  int hiddenCells = 0;
  for (int row = 0; row < kGrid; ++row) {
    const float y0 = static_cast<float>(row) / kGrid, y1 = static_cast<float>(row + 1) / kGrid;
    std::vector<std::array<int, 2>> runs;
    int start = -1;
    for (int col = 0; col <= kGrid; ++col) {
      bool hidden = false;
      if (col < kGrid) {
        const float x0 = static_cast<float>(col) / kGrid, x1 = static_cast<float>(col + 1) / kGrid;
        if (!points.empty()) {
          hidden = !inside((x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
          for (size_t i = 0, j = points.size() - 1; hidden && i < points.size(); j = i++) {
            hidden = !crosses(points[j], points[i], x0, y0, x1, y1);
          }
        } else {
          for (size_t t = 0; !hidden && t + 2 < corners.size(); t += 3) {
            hidden = in_triangle(t, x0, y0) && in_triangle(t, x1, y0) && in_triangle(t, x0, y1) && in_triangle(t, x1, y1);
          }
        }
        hiddenCells += hidden;
      }
      if (hidden && start < 0) start = col;
      if (!hidden && start >= 0) {
        runs.push_back({start, col});
        start = -1;
      }
    }
    std::vector<size_t> runRect;
    for (const auto& run : runs) {
      const auto same = std::ranges::find(above, run);
      if (same != above.end()) {
        const size_t index = aboveRect[same - above.begin()];
        rects[index + 3] = y1;
        runRect.push_back(index);
      } else {
        runRect.push_back(rects.size());
        rects.insert(rects.end(), {static_cast<float>(run[0]) / kGrid, y0, static_cast<float>(run[1]) / kGrid, y1});
      }
    }
    above = std::move(runs);
    aboveRect = std::move(runRect);
  }
  hiddenPart = static_cast<float>(hiddenCells) / (kGrid * kGrid);
  return rects;
}

} // namespace

uint32_t StereoView::hidden_area(uint32_t eye, float* rects, uint32_t capacity, uint32_t& version) const {
  std::lock_guard lock{mMutex};
  version = mHiddenVersion;
  if (eye > 1) return 0;
  const auto& all = mHiddenRects[eye];
  const uint32_t count = static_cast<uint32_t>(all.size() / 4);
  if (rects != nullptr) std::copy_n(all.begin(), std::min(count, capacity) * 4, rects);
  return count;
}

void StereoView::update(const XrView (&views)[2], const XrPosef& world, float scale, bool enabled, float hudWidth, float hudHeight) {
  // Headset measurements (debug.partyboard.freeze 1, with the game's frozen
  // simulation): the eyes keep the views they had, so every image of an A/B
  // is the very same work whatever the head does (the compositor still
  // places each image with the poses it was drawn with).
  static bool frozen = false;
  static auto frozenReadAt = std::chrono::steady_clock::time_point{};
  if (const auto now = std::chrono::steady_clock::now(); now - frozenReadAt >= std::chrono::seconds(1)) {
    frozenReadAt = now;
    char value[PROP_VALUE_MAX]{};
    __system_property_get("debug.partyboard.freeze", value);
    const bool wanted = value[0] == '1';
    if (wanted != frozen) LOGI("Stereo: eyes' views %s (debug.partyboard.freeze)", wanted ? "frozen" : "following the head");
    frozen = wanted;
  }
  std::lock_guard lock{mMutex};
  if (!frozen || !(mViews[0].fov.angleRight > mViews[0].fov.angleLeft)) {
    mViews[0] = views[0];
    mViews[1] = views[1];
  }
  if (mHiddenPending && views[0].fov.angleRight > views[0].fov.angleLeft &&
      views[1].fov.angleRight > views[1].fov.angleLeft) {
    mHiddenPending = false;
    float part[2]{};
    for (int eye = 0; eye < 2; ++eye) {
      mHiddenRects[eye] = hidden_rects(mVisibleOutline[eye], mHiddenTriangles[eye], views[eye].fov, part[eye]);
    }
    ++mHiddenVersion;
    LOGI("Hidden area: left %zu rectangles (%.1f%% of the eye), right %zu (%.1f%%)", mHiddenRects[0].size() / 4,
         part[0] * 100.f, mHiddenRects[1].size() / 4, part[1] * 100.f);
  }
  pose_matrix(world, scale, mWorld);
  mEnabled = enabled && mSwapchain != XR_NULL_HANDLE;
  mHudWidth = hudWidth;
  mHudHeight = hudHeight;
}

bool StereoView::game_frame(StereoFrame& out) {
  std::lock_guard lock{mMutex};
  if (!mEnabled || mScreenRequired || mResizePending) {
    mLastRequestAt = {}; // not a hitch of the board: the ring drains, or no world
    return false; // resizing: the ring drains, then new images come (layer())
  }
  // The time between the game's requests: its hitches, for adapt_resolution().
  const auto requestAt = std::chrono::steady_clock::now();
  if (mLastRequestAt != std::chrono::steady_clock::time_point{}) {
    const float ms = std::chrono::duration<float, std::milli>(requestAt - mLastRequestAt).count();
    if (ms < 250.0f) { // longer: loading or paused
      mRequestIntervals.push_back(ms);
    }
  }
  mLastRequestAt = requestAt;
  // Never reclaim Drawing by age: a queued GPU frame can still own it.
  // Cameras without a view cancel explicitly; empty eye passes clear and submit.
  const auto free = std::ranges::find_if(mSlots, [](const Slot& slot) { return slot.state == State::Free; });
  if (free == mSlots.end()) {
    ++mRingFullCount;
    ++mAdaptRingFull;
    for (const auto& slot : mSlots) {
      mFullDrawing += slot.state == State::Drawing;
      mFullReady += slot.state == State::Ready;
      mFullCopying += slot.state == State::Copying;
    }
    return false; // the headset is behind: this frame goes to the flat screen only
  }
  ++mLeaseCount;
  ++mAdaptLeases;
  // The controller's size, within the images (larger: a resize is pending).
  const auto [drawWidth, drawHeight] = eye_size(mRenderScale);
  free->renderWidth = std::min(drawWidth, mImageEyeWidth);
  free->renderHeight = std::min(drawHeight, mImageEyeHeight);
  free->leaseTime = mFrameTime;
  // The paced start this frame began at (next_frame_start()), for the pacer.
  free->startNs = mStartGiven ? mGivenStartNs : 0;
  free->dueNs = mStartGiven ? mGivenDueNs : 0;
  mStartGiven = false;
  // The interface, every other image: it is copied at 30 Hz, and its pass
  // (1600x1200 at 4x MSAA) was a fifth of the pixels the GPU cleared and wrote.
  free->hud = mHudEveryImage || !mHudLastLease;
  mHudLastLease = free->hud;
  out.drawHud = free->hud ? 1 : 0;
  out.generation = mGeneration;
  free->state = State::Drawing;
  free->leaseNs = steady_ns();
  free->submitNs = 0;
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

void StereoView::set_frame_time(XrTime time, XrDuration period) {
  std::lock_guard lock{mMutex};
  mFrameTime = time;
  if (period > 0) mPeriodNs = period;
}

int64_t StereoView::next_frame_start(int64_t nowNs, float targetHz) {
  std::lock_guard lock{mMutex};
  mStartGiven = false;
  if (!mPacing || !mEnabled || mScreenRequired) return 0;
  const int64_t start = mPacer.next_start(nowNs, targetHz, &mGivenDueNs);
  mStartGiven = start != 0;
  mGivenStartNs = start;
  return start;
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
  // A lower ceiling: drawn in a part of the images until ImageSizePolicy
  // shrinks them (adapt_resolution()).
  const auto [width, height] = eye_size(mRenderScale);
  if (width > mImageEyeWidth || height > mImageEyeHeight || mDesiredHudWidth != mHudPixelsWidth) mResizePending = true;
}

// One decision per fresh one-second metric window; do not lower resolution
// merely because the CPU or the image bridge is late while the GPU has headroom.
void StereoView::adapt_resolution() {
  const auto now = std::chrono::steady_clock::now();
  if (now - mAdaptAt < std::chrono::seconds(1)) {
    return;
  }
  mAdaptAt = now;
  mPacer.update(); // the start's place, from the images finished this second
  const float before = mRenderScale;
  const uint32_t wanted = mAdaptLeases + mAdaptRingFull;
  // A hitch: an image that came more than 1.5 times the usual interval (the
  // window's quarter-lowest, whatever the render rate) and 4 ms after it.
  uint32_t slow = 0;
  if (mRequestIntervals.size() >= 10) {
    auto sorted = mRequestIntervals;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 4, sorted.end());
    const float usual = sorted[sorted.size() / 4];
    for (const float ms : mRequestIntervals) {
      slow += ms > 1.5f * usual && ms > usual + 4.0f;
    }
  }
  mRequestIntervals.clear();
  mLastSlow = slow;
  const float measuredScale = mQuality.update(mQualitySample, wanted, mAdaptRingFull, slow);
  mRenderScale = std::min(1.0f, measuredScale / mMaxScale);
  // debug.partyboard.eye_scale <percent of recommended, 50 to 125>: a fixed
  // resolution instead of the adaptive one, read every second, so an A/B
  // capture (tools/quest_campaign.ps1) compares the same pixels.
  char pinned[PROP_VALUE_MAX] = {};
  __system_property_get("debug.partyboard.eye_scale", pinned);
  if (const float percent = std::strtof(pinned, nullptr); percent >= 50.0f && percent <= 125.0f) {
    mRenderScale = std::min(1.0f, percent / 100.0f / mMaxScale);
  }
  mAdaptLeases = mAdaptRingFull = 0;
  // New images only when they must grow, or have been far too large for a
  // while; layer() reallocates once the ring is empty.
  const auto [drawWidth, drawHeight] = eye_size(mRenderScale);
  if (mResizePending || now < mResizeRetryAt) {
    mImageSize.reset(); // pending, or failed a moment ago: the ring would empty every second
  } else if (mImageSize.update(drawWidth, drawHeight, mImageEyeWidth, mImageEyeHeight, mRenderScale != before)) {
    mResizePending = true;
    ++mResizeCount;
  }
  if (mRenderScale != before) {
    const auto decision = mQuality.last_decision();
    LOGI("Stereo: resolution %.0f%% of recommended (%ux%u per eye)%s, %u hitches in the last second",
         mRenderScale * mMaxScale * 100.0f, static_cast<unsigned>(mEyeWidth * mRenderScale),
         static_cast<unsigned>(mEyeHeight * mRenderScale),
         decision == AdaptiveQuality::Decision::Restored ? ", restored: fewer pixels brought no more images or hitches"
                                                          : "",
         mLastSlow);
  }
}

bool StereoView::images(void** eyeBuffers, void** hudBuffers, uint32_t capacity, uint32_t& count, uint32_t& width,
                        uint32_t& height, uint32_t& generation, uint32_t& hudWidth, uint32_t& hudHeight) {
  std::lock_guard lock{mMutex};
  if (mSlots.empty() || capacity < mSlots.size() || mSlots[0].buffer == nullptr || mSlots[0].hudBuffer == nullptr) {
    return false;
  }
  // Each buffer gets a reference for the caller, who imports it outside this
  // lock: the XR thread may replace the images meanwhile (a resize, or the
  // interface's size set at session start) and release its own references.
  // A buffer freed under the import aborted the game in Dawn ("Unsupported
  // VkFormat 0", Quest 3, 2026-09-28). The caller hands them back with
  // PartyBoardQuest_StereoReleaseImages once imported.
  for (size_t i = 0; i < mSlots.size(); ++i) {
    eyeBuffers[i] = mSlots[i].buffer;
    AHardwareBuffer_acquire(mSlots[i].buffer);
    hudBuffers[i] = mSlots[i].hudBuffer;
    AHardwareBuffer_acquire(mSlots[i].hudBuffer);
  }
  count = static_cast<uint32_t>(mSlots.size());
  width = mImageEyeWidth * 2;
  height = mImageEyeHeight;
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

bool StereoView::minigame_mode() const {
  std::lock_guard lock{mMutex};
  return !mScreenRequired && !mBoardMode;
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
  mSlots[image].submitNs = steady_ns();
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

StereoView::Slot* StereoView::newest_completed(int64_t lookNs) {
  Slot* newest = nullptr;
  for (auto& slot : mSlots) {
    if (slot.state != State::Ready || slot.tag <= mPresentedTag) continue;
    if (!mPacer.is_due(slot.dueNs, lookNs)) continue; // finished early: its look comes
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
    // This look's time: the grid the game's frames start on.
    const auto lookAt = std::chrono::steady_clock::now();
    const int64_t lookNs = steady_ns();
    mPacer.look(lookNs, mPeriodNs);
    if (lookAt - mLastLayerAt > std::chrono::milliseconds(100)) {
      mHoldCounted = false; // the layer was away: no hold to count
    }
    mLastLayerAt = lookAt;
    ++mFramesSinceNew;
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
    newest = newest_completed(lookNs);
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
  // When the GPU finished it, before EGL takes the fence.
  const int64_t doneNs = newest != nullptr ? fence_signal_time_ns(fence) : 0;
  const int64_t finishedNs = newest != nullptr && newest->startNs != 0 ? doneNs : 0;

  // Dynamic resolution changed: new images at the new size, once every image
  // is back (game_frame() gives no lease meanwhile).
  if (newest == nullptr && resize_ready()) {
    const auto [width, height] = eye_size(mRenderScale);
    const uint32_t previousWidth = mImageEyeWidth, previousHeight = mImageEyeHeight;
    const bool resized = allocate_images(width, height, mDesiredHudWidth);
    if (resized) {
      LOGI("Stereo: images now %ux%u per eye (were %ux%u), HUD %ux%u, %u resizes since start", width, height,
           previousWidth, previousHeight, mHudPixelsWidth, mHudPixelsHeight, mResizeCount);
    } else {
      LOGW("Stereo: resize failed; retaining previous eye images, next try in 10 s");
    }
    std::lock_guard lock{mMutex}; // game_frame() reads the scale
    if (!resized) {
      // Retain the previous valid allocation instead of leaving a dead ring.
      mRenderScale = static_cast<float>(mImageEyeWidth) / mEyeWidth;
      mResizeRetryAt = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    }
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
    const auto acquireImage = [&](XrSwapchain swapchain, uint32_t& image, size_t destination) {
      const auto start = std::chrono::steady_clock::now();
      const bool acquired = XR_SUCCEEDED(xrAcquireSwapchainImage(swapchain, &acquire, &image));
      const auto waitStart = std::chrono::steady_clock::now();
      const bool ready = acquired && XR_SUCCEEDED(xrWaitSwapchainImage(swapchain, &wait));
      const auto end = std::chrono::steady_clock::now();
      mDestinationAcquireMaxMs[destination] = std::max(mDestinationAcquireMaxMs[destination],
          std::chrono::duration<double, std::milli>(waitStart - start).count());
      if (acquired) {
        mDestinationWaitMaxMs[destination] = std::max(mDestinationWaitMaxMs[destination],
            std::chrono::duration<double, std::milli>(end - waitStart).count());
      }
      mAcquireMaxMs = std::max(mAcquireMaxMs,
          std::chrono::duration<double, std::milli>(end - start).count());
      return ready;
    };
    if (keepBoard || acquireImage(mSwapchain, index, 0)) {
      uint32_t hudIndex = 0;
      const auto hudNow = std::chrono::steady_clock::now();
      // Only from an image that drew it; at every image's rate, 30 Hz at most.
      const bool copyHud = newest->hud && (!mHudShown || !mHudEveryImage ||
                                           hudNow - mHudCopiedAt >= std::chrono::milliseconds(33));
      const bool hudReady = copyHud && acquireImage(mHudSwapchain, hudIndex, 1);
      // Acquire both destinations before queuing copies, then flush once.
      if (copyTimer) g_gl.queryCounter(copyTimer->queries[0], GL_TIMESTAMP_EXT);

      if (!keepBoard) {
        // Only the drawn part of each eye's half (dynamic resolution): at the
        // left, centered vertically (Aurora's common.cpp draws it there, a
        // half being the image's width / 2). The images are replaced only
        // when every slot is free, so this slot has the current images' size.
        const auto y = static_cast<GLint>((mEyeHeight - newest->renderHeight) / 2);
        const auto srcY = static_cast<GLint>((mImageEyeHeight - newest->renderHeight) / 2);
        const uint32_t srcStride = mImageEyeWidth;
        for (uint32_t eye = 0; eye < 2; ++eye) {
          g_gl.copyImage(newest->texture, GL_TEXTURE_2D, 0, static_cast<GLint>(eye * srcStride), srcY, 0,
                         mSwapchainImages[index].image, GL_TEXTURE_2D, 0, static_cast<GLint>(eye * mEyeWidth), y, 0,
                         static_cast<GLsizei>(newest->renderWidth), static_cast<GLsizei>(newest->renderHeight), 1);
        }
      }
      if (hudReady) {
        mHudCopiedAt = hudNow;
        g_gl.copyImage(newest->hudTexture, GL_TEXTURE_2D, 0, 0, 0, 0,
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
        if (mHoldCounted) ++mHolds[std::min<uint32_t>(mFramesSinceNew, 4) - 1];
        mHoldCounted = true;
        mFramesSinceNew = 0;
      }
      // How long images take from the game's lease to the display.
      if (newest->leaseTime > 0 && mFrameTime > newest->leaseTime) {
        const double sample = static_cast<double>(mFrameTime - newest->leaseTime);
        mLatencyNs = mLatencyNs == 0.0 ? sample : mLatencyNs * 0.9 + sample * 0.1;
        mLatencyMinMs = mLatencyMinMs == 0.0 ? sample / 1e6 : std::min(mLatencyMinMs, sample / 1e6);
        mLatencyMaxMs = std::max(mLatencyMaxMs, sample / 1e6);
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
    if (finishedNs > newest->startNs) mPacer.finished(finishedNs - newest->startNs);
    {
      const int64_t copyNs = steady_ns();
      if (newest->submitNs > newest->leaseNs && newest->leaseNs != 0) mStageRecord.add(newest->submitNs - newest->leaseNs);
      if (doneNs > newest->submitNs && newest->submitNs != 0) {
        mStageGpu.add(doneNs - newest->submitNs);
        mStageWait.add(copyNs - doneNs);
      }
      if (newest->leaseNs != 0) mStageTotal.add(copyNs - newest->leaseNs);
    }
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
           "res=%.0f%% latency=%.1fms screenHidden=%d acquireCpuMax=%.2fms worldNew=%.1fHz hudNew=%.1fHz copyGpuMax=%.3fms gpuSamples=%u eye=%ux%u "
           "worldAcquireMax=%.3fms worldWaitMax=%.3fms hudAcquireMax=%.3fms hudWaitMax=%.3fms "
           "holds=%u/%u/%u/%u latencyMin=%.1fms latencyMax=%.1fms paced=%d paceWork=%.1fms paceLooks=%lld pacePhase=%.1fms",
          mLeaseCount / elapsed, mPresentedCount / elapsed, mRingFullCount, mCopyMaxMs,
          drawing, ready, copying, mShownHasWorld, mRenderScale * mMaxScale * 100.0f, mLatencyNs / 1e6, mScreenHidden, mAcquireMaxMs, mNewWorldCount / elapsed, mNewHudCount / elapsed, mCopyGpuMaxMs, mCopyGpuSamples,
          mShownWidth, mShownHeight, mDestinationAcquireMaxMs[0], mDestinationWaitMaxMs[0],
          mDestinationAcquireMaxMs[1], mDestinationWaitMaxMs[1], mHolds[0], mHolds[1], mHolds[2], mHolds[3],
          mLatencyMinMs, mLatencyMaxMs, mPacing, mPacer.work_ns() / 1e6, static_cast<long long>(mPacer.looks()),
          mPacer.phase_ns() / 1e6);
      LOGI("Stereo pipeline: images=%u record=%.1f/%.1fms gpu=%.1f/%.1fms wait=%.1f/%.1fms total=%.1f/%.1fms "
           "(avg/max) ring=%zu full=%u (at full, images drawing/ready/copying: %u/%u/%u)",
           mStageTotal.count, mStageRecord.avg(), mStageRecord.max, mStageGpu.avg(), mStageGpu.max, mStageWait.avg(),
           mStageWait.max, mStageTotal.avg(), mStageTotal.max, mSlots.size(), mRingFullCount, mFullDrawing, mFullReady,
           mFullCopying);
      mStageRecord = mStageGpu = mStageWait = mStageTotal = Stage{};
      mFullDrawing = mFullReady = mFullCopying = 0;
      mHolds.fill(0);
      mLatencyMinMs = mLatencyMaxMs = 0;
      // A/B switch, read every 2 s: `adb shell setprop debug.partyboard.xr_pacing 0`.
      char pacing[PROP_VALUE_MAX] = {};
      __system_property_get("debug.partyboard.xr_pacing", pacing);
      mPacing = std::strcmp(pacing, "0") != 0;
      char hudRate[PROP_VALUE_MAX] = {};
      __system_property_get("debug.partyboard.hud_rate", hudRate);
      const bool everyImage = std::strcmp(hudRate, "full") == 0;
      if (everyImage != mHudEveryImage) {
        LOGI("Stereo: interface drawn %s (debug.partyboard.hud_rate=%s)", everyImage ? "in every image" : "every other image",
             hudRate[0] != '\0' ? hudRate : "unset");
        mHudEveryImage = everyImage;
      }
      mWorldRate = static_cast<float>(mNewWorldCount / elapsed);
      mStatsAt = now;
      mLeaseCount = mRingFullCount = mPresentedCount = 0;
      mCopyMaxMs = mAcquireMaxMs = 0;
      mDestinationAcquireMaxMs.fill(0);
      mDestinationWaitMaxMs.fill(0);
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

const XrCompositionLayerBaseHeader* StereoView::hud_layer(XrSpace space, const XrPosef& pose, XrExtent2Df size,
                                                          const void* next) {
  if (!world_visible() || !mHudShown) return nullptr;
  mHudLayer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
  mHudLayer.next = next;
  mHudLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  mHudLayer.space = space;
  mHudLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  mHudLayer.subImage.swapchain = mHudSwapchain;
  mHudLayer.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(mHudPixelsWidth), static_cast<int32_t>(mHudPixelsHeight)}};
  mHudLayer.pose = pose;
  mHudLayer.size = size;
  return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&mHudLayer);
}

// The interface's screen: a static image, a bezel around a dark face, drawn
// once (GL context current). Without it the interface shows on its own.
void StereoView::create_hud_screen() {
  constexpr int kWidth = 256, kHeight = 192; // the interface's 4:3
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
  info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  info.format = mSwapchainFormat;
  info.sampleCount = 1;
  info.width = kWidth;
  info.height = kHeight;
  info.faceCount = 1;
  info.arraySize = 1;
  info.mipCount = 1;
  if (!check(mInstance, xrCreateSwapchain(mSession, &info, &mHudScreenSwapchain), "xrCreateSwapchain (HUD screen)")) {
    mHudScreenSwapchain = XR_NULL_HANDLE;
    return;
  }
  uint32_t count = 0;
  xrEnumerateSwapchainImages(mHudScreenSwapchain, 0, &count, nullptr);
  std::vector<XrSwapchainImageOpenGLESKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
  xrEnumerateSwapchainImages(mHudScreenSwapchain, count, &count,
                             reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
  uint32_t index = 0;
  XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait.timeout = XR_INFINITE_DURATION;
  if (count == 0 || XR_FAILED(xrAcquireSwapchainImage(mHudScreenSwapchain, &acquire, &index)) ||
      XR_FAILED(xrWaitSwapchainImage(mHudScreenSwapchain, &wait))) {
    xrDestroySwapchain(mHudScreenSwapchain);
    mHudScreenSwapchain = XR_NULL_HANDLE;
    return;
  }
  // The bezel's width in pixels: kHudBezel of the interface's width on each
  // side of a quad that is the interface plus its bezel.
  const auto bezel = static_cast<GLint>(std::lround(kWidth * kHudBezel / (1.0f + 2.0f * kHudBezel)));
  GLuint framebuffer = 0;
  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, images[index].image, 0);
  const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  if (complete) {
    // Linear colors (the image is sRGB), alpha premultiplied: a dark grey
    // bezel, and a nearly black face that lets a little of the room through.
    glViewport(0, 0, kWidth, kHeight);
    glClearColor(0.035f, 0.035f, 0.045f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    constexpr float kFaceAlpha = 0.88f;
    glEnable(GL_SCISSOR_TEST);
    glScissor(bezel, bezel, kWidth - 2 * bezel, kHeight - 2 * bezel);
    glClearColor(0.003f * kFaceAlpha, 0.004f * kFaceAlpha, 0.008f * kFaceAlpha, kFaceAlpha);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteFramebuffers(1, &framebuffer);
  glFinish();
  XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  xrReleaseSwapchainImage(mHudScreenSwapchain, &release);
  if (!complete) {
    LOGW("Stereo: HUD screen unavailable (framebuffer incomplete)");
    xrDestroySwapchain(mHudScreenSwapchain);
    mHudScreenSwapchain = XR_NULL_HANDLE;
  }
}

const XrCompositionLayerBaseHeader* StereoView::hud_screen_layer(XrSpace space, const XrPosef& pose, XrExtent2Df size) {
  if (!world_visible() || !mHudShown || mHudScreenSwapchain == XR_NULL_HANDLE) return nullptr;
  mHudScreenLayer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
  mHudScreenLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  mHudScreenLayer.space = space;
  mHudScreenLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  mHudScreenLayer.subImage.swapchain = mHudScreenSwapchain;
  mHudScreenLayer.subImage.imageRect = {{0, 0}, {256, 192}};
  mHudScreenLayer.pose = pose;
  // The interface's size plus the bezel on each side (the same in both
  // directions, as on a real screen).
  const float bezel = size.width * kHudBezel;
  mHudScreenLayer.size = {size.width + 2.0f * bezel, size.height + 2.0f * bezel};
  return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&mHudScreenLayer);
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

// Game thread: `capacity` eye buffers and as many HUD buffers, each with a
// reference the caller gives back (PartyBoardQuest_StereoReleaseImages).
__attribute__((visibility("default"))) bool PartyBoardQuest_StereoImages(void** eyeBuffers, void** hudBuffers,
                                                                         uint32_t capacity, uint32_t* count,
                                                                         uint32_t* width, uint32_t* height,
                                                                         uint32_t* generation, uint32_t* hudWidth,
                                                                         uint32_t* hudHeight) {
  quest::StereoView* view = quest::g_stereoView;
  return view != nullptr && view->images(eyeBuffers, hudBuffers, capacity, *count, *width, *height, *generation,
                                        *hudWidth, *hudHeight);
}

// Game thread: the references PartyBoardQuest_StereoImages gave, once imported.
__attribute__((visibility("default"))) void PartyBoardQuest_StereoReleaseImages(void** buffers, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    if (buffers[i] != nullptr) {
      AHardwareBuffer_release(static_cast<AHardwareBuffer*>(buffers[i]));
    }
  }
}

// Game thread, its frame done: when its next frame starts (steady-clock
// nanoseconds), on the display's schedule; 0: the game keeps its own clock.
__attribute__((visibility("default"))) int64_t PartyBoardQuest_NextFrameStart(int64_t nowNs, float targetHz) {
  quest::StereoView* view = quest::g_stereoView;
  return view != nullptr ? view->next_frame_start(nowNs, targetHz) : 0;
}

__attribute__((visibility("default"))) uint32_t PartyBoardQuest_StereoHiddenArea(uint32_t eye, float* rects,
                                                                                uint32_t capacity, uint32_t* version) {
  quest::StereoView* view = quest::g_stereoView;
  uint32_t current = 0;
  const uint32_t count = view != nullptr ? view->hidden_area(eye, rects, capacity, current) : 0;
  if (version != nullptr) *version = current;
  return count;
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
