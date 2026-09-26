// Meta Quest 3 (and 2): Party Board in mixed reality, on the player's table,
// played with the Touch controllers (QuestVr.java).
//
// The game does not change. OpenXR's XR_KHR_android_surface_swapchain gives an
// android.view.Surface that the headset composites as a quad in the room; SDL
// is handed that Surface instead of the SurfaceView's (SDLSurface "external
// surface"), so Aurora's Vulkan swapchain presents straight into the headset.
// The session needs a graphics binding even though nothing is drawn here,
// hence the 1x1 OpenGL ES pbuffer context.
//
// Layers, back to front: the room (XR_FB_passthrough), the game's screen
// standing on the table (a spatial anchor, table_anchor.cpp), and the
// placement help while the player moves it.
//
// Everything OpenXR runs on one thread, started by nativeStart: set-up, the
// frame loop and tear-down. The controllers go to Java (QuestVr.onControllers),
// which presents them to SDL as a gamepad.

#include "stereo_view.hpp"
#include "table_anchor.hpp"
#include "xr_util.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace quest;

namespace {

// The screen stands this far above the anchor (the table top).
constexpr float kScreenLiftMeters = 0.02f;
// Before any placement: in front of the player, a little below the eyes.
constexpr float kDefaultDistanceMeters = 1.1f;
constexpr float kDefaultDropMeters = 0.35f;
// With the model on the table, the flat screen (text, menus, split-screen
// minigames) stands behind it.
constexpr float kScreenBehindModelMeters = 0.45f;
// The model's scale: meters per game unit (a board is some 4000 units wide).
constexpr float kMinModelScale = 0.00003f;
constexpr float kMaxModelScale = 0.002f;
constexpr float kMinScreenWidth = 0.3f;
constexpr float kMaxScreenWidth = 4.0f;

// The placement help, drawn by QuestVr.drawHelp, floats below the line of sight.
constexpr int kHelpWidthPixels = 1024;
constexpr int kHelpHeightPixels = 640;
constexpr float kHelpWidthMeters = 0.56f;
constexpr XrVector3f kHelpPosition{0.0f, -0.22f, -0.9f};

constexpr std::array<int, 3> kResolutions{1080, 1440, 2160};

// Controller bits sent to QuestVr.onControllers; QuestControllers.java has the same list.
enum ControllerBit : int {
  kButtonA = 1 << 0,
  kButtonB = 1 << 1,
  kButtonX = 1 << 2,
  kButtonY = 1 << 3,
  kButtonMenu = 1 << 4,
  kButtonLeftStick = 1 << 5,
  kButtonRightStick = 1 << 6,
};

struct ControllerState {
  int buttons = 0;
  float leftX = 0, leftY = 0, rightX = 0, rightY = 0;
  float leftTrigger = 0, rightTrigger = 0, leftGrip = 0, rightGrip = 0;
};

struct Java {
  JavaVM* vm = nullptr;
  jobject activity = nullptr;
  jclass questVr = nullptr;
  jmethodID onSurface = nullptr;
  jmethodID onSurfaceReplaced = nullptr;
  jmethodID onHelpSurface = nullptr;
  jmethodID onPlacement = nullptr;
  jmethodID onSessionState = nullptr;
  jmethodID onControllers = nullptr;
  jmethodID onExit = nullptr;
};

struct Actions {
  XrActionSet set = XR_NULL_HANDLE;
  XrAction a = XR_NULL_HANDLE, b = XR_NULL_HANDLE, x = XR_NULL_HANDLE, y = XR_NULL_HANDLE;
  XrAction menu = XR_NULL_HANDLE, leftStickClick = XR_NULL_HANDLE, rightStickClick = XR_NULL_HANDLE;
  XrAction leftStick = XR_NULL_HANDLE, rightStick = XR_NULL_HANDLE;
  XrAction leftTrigger = XR_NULL_HANDLE, rightTrigger = XR_NULL_HANDLE;
  XrAction leftGrip = XR_NULL_HANDLE, rightGrip = XR_NULL_HANDLE;
  XrAction aim = XR_NULL_HANDLE;
  XrAction haptic = XR_NULL_HANDLE;
  XrPath leftHand = XR_NULL_PATH, rightHand = XR_NULL_PATH;
  XrSpace rightAim = XR_NULL_HANDLE;
};

struct Egl {
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLConfig config = nullptr;
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface pbuffer = EGL_NO_SURFACE;
};

// An Android surface the headset shows as a quad.
struct SurfaceQuad {
  XrSwapchain swapchain = XR_NULL_HANDLE;
  jobject surface = nullptr; // global reference
  int width = 0;
  int height = 0;
};

struct Passthrough {
  XrPassthroughFB passthrough = XR_NULL_HANDLE;
  XrPassthroughLayerFB layer = XR_NULL_HANDLE;
  PFN_xrPassthroughStartFB start = nullptr;
  PFN_xrPassthroughPauseFB pause = nullptr;
};

struct Extensions {
  bool refreshRate = false;
  bool passthrough = false;
  bool layerSettings = false;
  bool colorScale = false;
  bool performance = false;
  bool imageLayout = false;
  bool anchors = false;
};

struct App {
  XrInstance instance = XR_NULL_HANDLE;
  XrSystemId system = XR_NULL_SYSTEM_ID;
  XrSession session = XR_NULL_HANDLE;
  XrSessionState state = XR_SESSION_STATE_UNKNOWN;
  bool running = false;
  Extensions extensions;
  Egl egl;
  Actions actions;
  XrSpace stage = XR_NULL_HANDLE; // the room's floor; LOCAL without a play area
  XrSpace view = XR_NULL_HANDLE;  // the head
  Passthrough passthrough;
  PFN_xrRequestDisplayRefreshRateFB requestRefreshRate = nullptr;

  SurfaceQuad screen;   // the game
  SurfaceQuad next;     // the game at the resolution being switched to
  SurfaceQuad retiring; // the previous one, kept until Aurora has let go of it
  std::chrono::steady_clock::time_point retireAt;
  SurfaceQuad help;

  std::string statePath;
  TableSettings table;
  TableAnchor anchor;
  StereoView stereo; // the game's world as a model on the table
  XrPosef pose = identity_pose(); // the anchor point, in STAGE space
  bool poseKnown = false;

  bool placing = false;
  bool grabbing = false;
  bool triggerHeld = false;
  XrPosef grabOffset = identity_pose();
  int previousButtons = 0;
  int suppressedButtons = 0; // held when placement ended: the game waits for their release
};

Java g_java;
std::thread g_thread;
std::atomic_bool g_stop = false;
std::atomic_bool g_surfaceSwitched = false;

std::mutex g_ratesMutex;
std::vector<float> g_refreshRates;
std::atomic<float> g_activeRefreshRate{0.0f};
std::atomic<float> g_requestedRefreshRate = 0.0f;

// Rumble from SDL (QuestVr.rumble), applied by the frame loop.
std::atomic<float> g_rumbleAmplitude = 0.0f;
std::atomic_int g_rumbleDurationMs = 0;
std::atomic_uint g_rumbleSerial = 0;

void clear_exception(JNIEnv* env) {
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
}

bool has_extension(const std::vector<XrExtensionProperties>& extensions, const char* name) {
  return std::ranges::any_of(extensions, [name](const XrExtensionProperties& e) {
    return std::strcmp(e.extensionName, name) == 0;
  });
}

int screen_width_pixels(int height) { return height * 16 / 9; }

// --- Set-up ---

bool init_loader() {
  auto initializeLoader = proc<PFN_xrInitializeLoaderKHR>(XR_NULL_HANDLE, "xrInitializeLoaderKHR");
  if (initializeLoader == nullptr) {
    LOGW("xrInitializeLoaderKHR missing");
    return false;
  }
  XrLoaderInitInfoAndroidKHR info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
  info.applicationVM = g_java.vm;
  info.applicationContext = g_java.activity;
  return check(XR_NULL_HANDLE, initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&info)),
               "xrInitializeLoaderKHR");
}

bool create_instance(App& app) {
  uint32_t count = 0;
  if (!check(XR_NULL_HANDLE, xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr),
             "xrEnumerateInstanceExtensionProperties")) {
    return false;
  }
  std::vector<XrExtensionProperties> available(count, {XR_TYPE_EXTENSION_PROPERTIES});
  xrEnumerateInstanceExtensionProperties(nullptr, count, &count, available.data());

  std::vector<const char*> enabled;
  for (const char* required : {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
                               XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME}) {
    if (!has_extension(available, required)) {
      LOGW("OpenXR runtime lacks %s", required);
      return false;
    }
    enabled.push_back(required);
  }
  auto optional = [&](const char* name) {
    if (!has_extension(available, name)) {
      LOGI("Optional extension %s unavailable", name);
      return false;
    }
    enabled.push_back(name);
    return true;
  };
  Extensions& ext = app.extensions;
  ext.refreshRate = optional(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
  ext.passthrough = optional(XR_FB_PASSTHROUGH_EXTENSION_NAME);
  ext.layerSettings = optional(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
  ext.colorScale = optional(XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME);
  ext.performance = optional(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
  ext.imageLayout = optional(XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME);
  ext.anchors = std::ranges::all_of(TableAnchor::kExtensions,
                                    [&](const char* name) { return has_extension(available, name); });
  if (ext.anchors) {
    enabled.insert(enabled.end(), std::begin(TableAnchor::kExtensions), std::end(TableAnchor::kExtensions));
  }

  XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
  android.applicationVM = g_java.vm;
  android.applicationActivity = g_java.activity;

  XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
  info.next = &android;
  std::strncpy(info.applicationInfo.applicationName, "Party Board", XR_MAX_APPLICATION_NAME_SIZE - 1);
  info.applicationInfo.applicationVersion = 1;
  std::strncpy(info.applicationInfo.engineName, "Aurora", XR_MAX_ENGINE_NAME_SIZE - 1);
  info.applicationInfo.engineVersion = 1;
  info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  info.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
  info.enabledExtensionNames = enabled.data();
  if (!check(XR_NULL_HANDLE, xrCreateInstance(&info, &app.instance), "xrCreateInstance")) {
    return false;
  }

  XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
  systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  return check(app.instance, xrGetSystem(app.instance, &systemInfo, &app.system), "xrGetSystem");
}

bool create_egl(Egl& egl) {
  egl.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (egl.display == EGL_NO_DISPLAY || !eglInitialize(egl.display, nullptr, nullptr)) {
    LOGW("EGL display unavailable");
    return false;
  }
  const EGLint configAttributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                     EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
                                     EGL_RED_SIZE,        8,
                                     EGL_GREEN_SIZE,      8,
                                     EGL_BLUE_SIZE,       8,
                                     EGL_ALPHA_SIZE,      8,
                                     EGL_NONE};
  EGLint configCount = 0;
  if (!eglChooseConfig(egl.display, configAttributes, &egl.config, 1, &configCount) || configCount == 0) {
    LOGW("No EGL config");
    return false;
  }
  const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  egl.context = eglCreateContext(egl.display, egl.config, EGL_NO_CONTEXT, contextAttributes);
  const EGLint pbufferAttributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  egl.pbuffer = eglCreatePbufferSurface(egl.display, egl.config, pbufferAttributes);
  if (egl.context == EGL_NO_CONTEXT || egl.pbuffer == EGL_NO_SURFACE ||
      !eglMakeCurrent(egl.display, egl.pbuffer, egl.pbuffer, egl.context)) {
    LOGW("EGL context unavailable");
    return false;
  }
  return true;
}

void destroy_egl(Egl& egl) {
  if (egl.display == EGL_NO_DISPLAY) {
    return;
  }
  eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (egl.pbuffer != EGL_NO_SURFACE) {
    eglDestroySurface(egl.display, egl.pbuffer);
  }
  if (egl.context != EGL_NO_CONTEXT) {
    eglDestroyContext(egl.display, egl.context);
  }
  eglTerminate(egl.display);
  egl = {};
}

bool create_reference_space(App& app, XrReferenceSpaceType type, XrSpace& space) {
  XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  info.referenceSpaceType = type;
  info.poseInReferenceSpace = identity_pose();
  return XR_SUCCEEDED(xrCreateReferenceSpace(app.session, &info, &space));
}

bool create_session(App& app) {
  auto requirements =
      proc<PFN_xrGetOpenGLESGraphicsRequirementsKHR>(app.instance, "xrGetOpenGLESGraphicsRequirementsKHR");
  if (requirements == nullptr) {
    return false;
  }
  XrGraphicsRequirementsOpenGLESKHR needed{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
  if (!check(app.instance, requirements(app.instance, app.system, &needed), "xrGetOpenGLESGraphicsRequirementsKHR")) {
    return false;
  }

  XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
  binding.display = app.egl.display;
  binding.config = app.egl.config;
  binding.context = app.egl.context;
  XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
  info.next = &binding;
  info.systemId = app.system;
  if (!check(app.instance, xrCreateSession(app.instance, &info, &app.session), "xrCreateSession")) {
    return false;
  }

  // STAGE is the room's floor: it stays put when the player recenters.
  if (!create_reference_space(app, XR_REFERENCE_SPACE_TYPE_STAGE, app.stage)) {
    LOGW("No STAGE space: placing relative to the start position");
    if (!create_reference_space(app, XR_REFERENCE_SPACE_TYPE_LOCAL, app.stage)) {
      return false;
    }
  }
  if (!create_reference_space(app, XR_REFERENCE_SPACE_TYPE_VIEW, app.view)) {
    LOGW("No VIEW space");
    return false;
  }
  return true;
}

XrPath path(const App& app, const char* string) {
  XrPath result = XR_NULL_PATH;
  xrStringToPath(app.instance, string, &result);
  return result;
}

XrAction create_action(const App& app, XrActionType type, const char* name, const char* localized,
                       const XrPath* subactions = nullptr, uint32_t subactionCount = 0) {
  XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
  info.actionType = type;
  std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
  std::strncpy(info.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
  info.countSubactionPaths = subactionCount;
  info.subactionPaths = subactions;
  XrAction action = XR_NULL_HANDLE;
  check(app.instance, xrCreateAction(app.actions.set, &info, &action), name);
  return action;
}

bool create_actions(App& app) {
  Actions& actions = app.actions;
  XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strncpy(setInfo.actionSetName, "gamecube_pad", XR_MAX_ACTION_SET_NAME_SIZE - 1);
  std::strncpy(setInfo.localizedActionSetName, "GameCube controller", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
  if (!check(app.instance, xrCreateActionSet(app.instance, &setInfo, &actions.set), "xrCreateActionSet")) {
    return false;
  }

  actions.leftHand = path(app, "/user/hand/left");
  actions.rightHand = path(app, "/user/hand/right");
  const XrPath hands[] = {actions.leftHand, actions.rightHand};

  actions.a = create_action(app, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_a", "A");
  actions.b = create_action(app, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_b", "B");
  actions.x = create_action(app, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_x", "X");
  actions.y = create_action(app, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_y", "Y");
  actions.menu = create_action(app, XR_ACTION_TYPE_BOOLEAN_INPUT, "start", "Start");
  actions.leftStickClick = create_action(app, XR_ACTION_TYPE_BOOLEAN_INPUT, "party_board_menu", "Menu");
  actions.rightStickClick = create_action(app, XR_ACTION_TYPE_BOOLEAN_INPUT, "placement", "Place the game");
  actions.leftStick = create_action(app, XR_ACTION_TYPE_VECTOR2F_INPUT, "main_stick", "Control stick");
  actions.rightStick = create_action(app, XR_ACTION_TYPE_VECTOR2F_INPUT, "c_stick", "C stick");
  actions.leftTrigger = create_action(app, XR_ACTION_TYPE_FLOAT_INPUT, "trigger_l", "L");
  actions.rightTrigger = create_action(app, XR_ACTION_TYPE_FLOAT_INPUT, "trigger_r", "R");
  actions.leftGrip = create_action(app, XR_ACTION_TYPE_FLOAT_INPUT, "dpad_modifier", "D-pad");
  actions.rightGrip = create_action(app, XR_ACTION_TYPE_FLOAT_INPUT, "trigger_z", "Z");
  actions.aim = create_action(app, XR_ACTION_TYPE_POSE_INPUT, "aim", "Pointer", hands, 2);
  actions.haptic = create_action(app, XR_ACTION_TYPE_VIBRATION_OUTPUT, "rumble", "Rumble", hands, 2);

  const XrActionSuggestedBinding bindings[] = {
      {actions.a, path(app, "/user/hand/right/input/a/click")},
      {actions.b, path(app, "/user/hand/right/input/b/click")},
      {actions.x, path(app, "/user/hand/left/input/x/click")},
      {actions.y, path(app, "/user/hand/left/input/y/click")},
      {actions.menu, path(app, "/user/hand/left/input/menu/click")},
      {actions.leftStickClick, path(app, "/user/hand/left/input/thumbstick/click")},
      {actions.rightStickClick, path(app, "/user/hand/right/input/thumbstick/click")},
      {actions.leftStick, path(app, "/user/hand/left/input/thumbstick")},
      {actions.rightStick, path(app, "/user/hand/right/input/thumbstick")},
      {actions.leftTrigger, path(app, "/user/hand/left/input/trigger/value")},
      {actions.rightTrigger, path(app, "/user/hand/right/input/trigger/value")},
      {actions.leftGrip, path(app, "/user/hand/left/input/squeeze/value")},
      {actions.rightGrip, path(app, "/user/hand/right/input/squeeze/value")},
      {actions.aim, path(app, "/user/hand/left/input/aim/pose")},
      {actions.aim, path(app, "/user/hand/right/input/aim/pose")},
      {actions.haptic, path(app, "/user/hand/left/output/haptic")},
      {actions.haptic, path(app, "/user/hand/right/output/haptic")},
  };
  // Quest 2, 3, 3S and Pro controllers all answer to the Touch profile.
  XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
  suggested.interactionProfile = path(app, "/interaction_profiles/oculus/touch_controller");
  suggested.suggestedBindings = bindings;
  suggested.countSuggestedBindings = static_cast<uint32_t>(std::size(bindings));
  if (!check(app.instance, xrSuggestInteractionProfileBindings(app.instance, &suggested),
             "xrSuggestInteractionProfileBindings")) {
    return false;
  }

  XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
  spaceInfo.action = actions.aim;
  spaceInfo.subactionPath = actions.rightHand;
  spaceInfo.poseInActionSpace = identity_pose();
  check(app.instance, xrCreateActionSpace(app.session, &spaceInfo, &actions.rightAim), "xrCreateActionSpace");

  XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  attach.countActionSets = 1;
  attach.actionSets = &actions.set;
  return check(app.instance, xrAttachSessionActionSets(app.session, &attach), "xrAttachSessionActionSets");
}

bool create_surface_quad(App& app, JNIEnv* env, int width, int height, SurfaceQuad& quad) {
  auto createSurface = proc<PFN_xrCreateSwapchainAndroidSurfaceKHR>(app.instance, "xrCreateSwapchainAndroidSurfaceKHR");
  if (createSurface == nullptr) {
    return false;
  }
  // Meta's runtime rejects a format, sample, face, array or mip count for an
  // Android surface (the surface's producer decides them): all zero. Another
  // runtime may want the usual values instead.
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  info.width = static_cast<uint32_t>(width);
  info.height = static_cast<uint32_t>(height);
  jobject surface = nullptr;
  XrResult result = createSurface(app.session, &info, &quad.swapchain, &surface);
  if (XR_FAILED(result)) {
    info.format = GL_RGBA8;
    info.sampleCount = 1;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    result = createSurface(app.session, &info, &quad.swapchain, &surface);
  }
  if (!check(app.instance, result, "xrCreateSwapchainAndroidSurfaceKHR") || surface == nullptr) {
    return false;
  }
  quad.surface = env->NewGlobalRef(surface);
  quad.width = width;
  quad.height = height;
  return true;
}

void destroy_surface_quad(JNIEnv* env, SurfaceQuad& quad) {
  if (quad.swapchain != XR_NULL_HANDLE) {
    xrDestroySwapchain(quad.swapchain);
  }
  if (quad.surface != nullptr) {
    env->DeleteGlobalRef(quad.surface);
  }
  quad = {};
}

// The room around the game. Paused (not destroyed) when the player turns it
// off, so the cameras stop costing power.
void create_passthrough(App& app) {
  if (!app.extensions.passthrough) {
    return;
  }
  auto create = proc<PFN_xrCreatePassthroughFB>(app.instance, "xrCreatePassthroughFB");
  auto createLayer = proc<PFN_xrCreatePassthroughLayerFB>(app.instance, "xrCreatePassthroughLayerFB");
  app.passthrough.start = proc<PFN_xrPassthroughStartFB>(app.instance, "xrPassthroughStartFB");
  app.passthrough.pause = proc<PFN_xrPassthroughPauseFB>(app.instance, "xrPassthroughPauseFB");
  if (create == nullptr || createLayer == nullptr || app.passthrough.start == nullptr ||
      app.passthrough.pause == nullptr) {
    return;
  }
  XrPassthroughCreateInfoFB info{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
  info.flags = app.table.passthrough ? XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB : 0;
  if (!check(app.instance, create(app.session, &info, &app.passthrough.passthrough), "xrCreatePassthroughFB")) {
    return;
  }
  XrPassthroughLayerCreateInfoFB layerInfo{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
  layerInfo.passthrough = app.passthrough.passthrough;
  layerInfo.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
  layerInfo.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
  check(app.instance, createLayer(app.session, &layerInfo, &app.passthrough.layer), "xrCreatePassthroughLayerFB");
}

void destroy_passthrough(App& app) {
  if (app.passthrough.layer != XR_NULL_HANDLE) {
    proc<PFN_xrDestroyPassthroughLayerFB>(app.instance, "xrDestroyPassthroughLayerFB")(app.passthrough.layer);
  }
  if (app.passthrough.passthrough != XR_NULL_HANDLE) {
    proc<PFN_xrDestroyPassthroughFB>(app.instance, "xrDestroyPassthroughFB")(app.passthrough.passthrough);
  }
  app.passthrough = {};
}

bool passthrough_available(const App& app) { return app.passthrough.layer != XR_NULL_HANDLE; }

void set_passthrough(App& app, bool on) {
  app.table.passthrough = on;
  if (!passthrough_available(app)) {
    return;
  }
  if (on) {
    check(app.instance, app.passthrough.start(app.passthrough.passthrough), "xrPassthroughStartFB");
  } else {
    check(app.instance, app.passthrough.pause(app.passthrough.passthrough), "xrPassthroughPauseFB");
  }
}

void query_refresh_rates(App& app) {
  if (!app.extensions.refreshRate) {
    return;
  }
  auto enumerate = proc<PFN_xrEnumerateDisplayRefreshRatesFB>(app.instance, "xrEnumerateDisplayRefreshRatesFB");
  app.requestRefreshRate = proc<PFN_xrRequestDisplayRefreshRateFB>(app.instance, "xrRequestDisplayRefreshRateFB");
  uint32_t count = 0;
  if (enumerate == nullptr || XR_FAILED(enumerate(app.session, 0, &count, nullptr)) || count == 0) {
    return;
  }
  std::vector<float> rates(count);
  enumerate(app.session, count, &count, rates.data());
  rates.resize(count);
  std::lock_guard lock{g_ratesMutex};
  g_refreshRates = std::move(rates);
  if (!g_refreshRates.empty()) {
    g_requestedRefreshRate.store(*std::max_element(g_refreshRates.begin(), g_refreshRates.end()));
  }
}

float current_refresh_rate(const App& app) {
  if (app.extensions.refreshRate) {
    auto get = proc<PFN_xrGetDisplayRefreshRateFB>(app.instance, "xrGetDisplayRefreshRateFB");
    float rate = 0.0f;
    if (get != nullptr && XR_SUCCEEDED(get(app.session, &rate)) && rate > 0.0f) {
      return rate;
    }
  }
  return 72.0f;
}

// The game emulates a console on the CPU and renders up to 4K: ask for the
// highest levels the headset sustains without throttling.
void request_performance(const App& app) {
  if (!app.extensions.performance) {
    return;
  }
  auto set = proc<PFN_xrPerfSettingsSetPerformanceLevelEXT>(app.instance, "xrPerfSettingsSetPerformanceLevelEXT");
  if (set != nullptr) {
    check(app.instance, set(app.session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT),
          "CPU performance level");
    check(app.instance, set(app.session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT),
          "GPU performance level");
  }
}

// --- Controllers ---

bool read_bool(const App& app, XrAction action) {
  XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
  info.action = action;
  XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
  return XR_SUCCEEDED(xrGetActionStateBoolean(app.session, &info, &state)) && state.isActive && state.currentState;
}

float read_float(const App& app, XrAction action) {
  XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
  info.action = action;
  XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
  if (XR_FAILED(xrGetActionStateFloat(app.session, &info, &state)) || !state.isActive) {
    return 0.0f;
  }
  return state.currentState;
}

XrVector2f read_vector(const App& app, XrAction action) {
  XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
  info.action = action;
  XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
  if (XR_FAILED(xrGetActionStateVector2f(app.session, &info, &state)) || !state.isActive) {
    return {0.0f, 0.0f};
  }
  return state.currentState;
}

ControllerState read_controllers(const App& app) {
  const Actions& actions = app.actions;
  const XrActiveActionSet active{actions.set, XR_NULL_PATH};
  XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
  sync.countActiveActionSets = 1;
  sync.activeActionSets = &active;
  if (XR_FAILED(xrSyncActions(app.session, &sync))) {
    return {};
  }

  ControllerState state;
  const std::pair<XrAction, int> buttons[] = {
      {actions.a, kButtonA},       {actions.b, kButtonB},
      {actions.x, kButtonX},       {actions.y, kButtonY},
      {actions.menu, kButtonMenu}, {actions.leftStickClick, kButtonLeftStick},
      {actions.rightStickClick, kButtonRightStick},
  };
  for (const auto& [action, bit] : buttons) {
    if (read_bool(app, action)) {
      state.buttons |= bit;
    }
  }
  const XrVector2f left = read_vector(app, actions.leftStick);
  const XrVector2f right = read_vector(app, actions.rightStick);
  state.leftX = left.x;
  state.leftY = left.y;
  state.rightX = right.x;
  state.rightY = right.y;
  state.leftTrigger = read_float(app, actions.leftTrigger);
  state.rightTrigger = read_float(app, actions.rightTrigger);
  state.leftGrip = read_float(app, actions.leftGrip);
  state.rightGrip = read_float(app, actions.rightGrip);
  return state;
}

bool locate(XrSpace space, XrSpace base, XrTime time, XrPosef& pose) {
  if (space == XR_NULL_HANDLE) {
    return false;
  }
  XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
  if (XR_FAILED(xrLocateSpace(space, base, time, &location))) {
    return false;
  }
  constexpr XrSpaceLocationFlags kValid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  if ((location.locationFlags & kValid) != kValid) {
    return false;
  }
  pose = location.pose;
  return true;
}

void vibrate(const App& app, XrPath hand, float amplitude, XrDuration duration) {
  XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
  info.action = app.actions.haptic;
  info.subactionPath = hand;
  if (amplitude <= 0.0f || duration <= 0) {
    xrStopHapticFeedback(app.session, &info);
    return;
  }
  XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
  vibration.amplitude = std::clamp(amplitude, 0.0f, 1.0f);
  vibration.duration = duration;
  vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
  xrApplyHapticFeedback(app.session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
}

// A short tick in the right hand: placement feedback.
void tick(const App& app) { vibrate(app, app.actions.rightHand, 0.4f, 25'000'000); }

void apply_rumble(const App& app, unsigned& appliedSerial) {
  const unsigned serial = g_rumbleSerial.load(std::memory_order_acquire);
  if (serial == appliedSerial) {
    return;
  }
  appliedSerial = serial;
  const float amplitude = g_rumbleAmplitude.load(std::memory_order_relaxed);
  const XrDuration duration = static_cast<XrDuration>(g_rumbleDurationMs.load(std::memory_order_relaxed)) * 1'000'000;
  vibrate(app, app.actions.leftHand, amplitude, duration);
  vibrate(app, app.actions.rightHand, amplitude, duration);
}

void send_controllers(JNIEnv* env, const ControllerState& s) {
  env->CallStaticVoidMethod(g_java.questVr, g_java.onControllers, s.buttons, s.leftX, s.leftY, s.rightX, s.rightY,
                            s.leftTrigger, s.rightTrigger, s.leftGrip, s.rightGrip);
  clear_exception(env);
}

// --- Placement ---

void notify_placement(const App& app, JNIEnv* env) {
  env->CallStaticVoidMethod(g_java.questVr, g_java.onPlacement, app.placing, app.table.resolution,
                            app.table.passthrough && passthrough_available(app), app.table.diorama);
  clear_exception(env);
}

void save_settings(App& app) { app.table.save(app.statePath); }

// Where the screen appears before the first placement: ahead, facing the player.
void default_pose(App& app, const XrPosef& head) {
  const XrVector3f ahead = rotate(upright(head).orientation, {0.0f, 0.0f, -kDefaultDistanceMeters});
  const float screenHeight = app.table.screenWidth * 9.0f / 16.0f;
  XrVector3f at = add(head.position, ahead);
  at.y = head.position.y - kDefaultDropMeters - screenHeight * 0.5f - kScreenLiftMeters;
  app.pose = facing(at, head.position);
  app.poseKnown = true;
}

void begin_placement(App& app, JNIEnv* env) {
  LOGI("Placement mode");
  app.placing = true;
  app.grabbing = false;
  app.triggerHeld = true; // a trigger already held does not place at once
  notify_placement(app, env);
}

// Keeps the placement: a new anchor where the screen stands, and the settings.
void end_placement(App& app, JNIEnv* env, XrTime time, int heldButtons) {
  LOGI("Placed: %.2f %.2f %.2f, screen %.2f m", app.pose.position.x, app.pose.position.y, app.pose.position.z,
       app.table.screenWidth);
  app.placing = false;
  app.grabbing = false;
  app.suppressedButtons = heldButtons;
  app.table.placed = true;
  app.table.pose = app.pose;
  app.table.anchorUuid.clear();
  if (app.extensions.anchors) {
    app.anchor.place(app.pose, time);
  }
  save_settings(app);
  notify_placement(app, env);
}

void switch_resolution(App& app, JNIEnv* env) {
  if (app.next.swapchain != XR_NULL_HANDLE) {
    return; // a switch is still under way
  }
  const auto current = std::ranges::find(kResolutions, app.table.resolution);
  const int resolution =
      current == kResolutions.end() || current + 1 == kResolutions.end() ? kResolutions.front() : *(current + 1);
  if (!create_surface_quad(app, env, screen_width_pixels(resolution), resolution, app.next)) {
    return;
  }
  app.table.resolution = resolution;
  g_surfaceSwitched.store(false);
  LOGI("Screen resolution %dx%d", app.next.width, app.next.height);
  env->CallStaticVoidMethod(g_java.questVr, g_java.onSurfaceReplaced, app.next.surface, app.next.width,
                            app.next.height);
  clear_exception(env);
  notify_placement(app, env);
}

// Once SDL draws into the new surface, show it; the old one goes a little
// later, after Aurora's swapchain has certainly let go of it.
void finish_resolution_switch(App& app, JNIEnv* env) {
  const auto now = std::chrono::steady_clock::now();
  if (app.next.swapchain != XR_NULL_HANDLE && g_surfaceSwitched.exchange(false)) {
    destroy_surface_quad(env, app.retiring);
    app.retiring = app.screen;
    app.retireAt = now + std::chrono::seconds(2);
    app.screen = app.next;
    app.next = {};
    save_settings(app);
  }
  if (app.retiring.swapchain != XR_NULL_HANDLE && now >= app.retireAt) {
    destroy_surface_quad(env, app.retiring);
  }
}

// The controls while placing (QuestVr.drawHelp shows them). The game sees a
// released pad meanwhile.
void place(App& app, JNIEnv* env, const ControllerState& input, int pressed, float dt, const XrPosef* head,
           const XrPosef* aim) {
  // Right trigger: the screen stands where the controller's tip touches the table.
  const bool trigger = input.rightTrigger > 0.6f;
  if (trigger && !app.triggerHeld && head != nullptr && aim != nullptr) {
    app.pose = facing(aim->position, head->position);
    app.poseKnown = true;
    LOGI("Screen set at %.2f %.2f %.2f", app.pose.position.x, app.pose.position.y, app.pose.position.z);
    tick(app);
  }
  app.triggerHeld = input.rightTrigger > (app.triggerHeld ? 0.3f : 0.6f);

  // Right grip held: carry it with the controller.
  const bool grip = input.rightGrip > (app.grabbing ? 0.4f : 0.6f);
  if (grip && aim != nullptr) {
    if (!app.grabbing) {
      app.grabOffset = compose(inverse(upright(*aim)), app.pose);
      tick(app);
    }
    app.pose = compose(upright(*aim), app.grabOffset);
  }
  app.grabbing = grip && aim != nullptr;

  // Right stick: size (up/down), turn (left/right). Left stick: height.
  if (std::abs(input.rightY) > 0.2f) {
    app.table.screenWidth = std::clamp(app.table.screenWidth * std::exp(input.rightY * dt * 0.8f), kMinScreenWidth,
                                       kMaxScreenWidth);
  }
  if (std::abs(input.rightX) > 0.2f) {
    app.pose.orientation = multiply(yaw_rotation(-input.rightX * dt * 1.5f), app.pose.orientation);
  }
  if (std::abs(input.leftY) > 0.2f) {
    app.pose.position.y += input.leftY * dt * 0.15f;
  }
  // Left stick left/right: the model's size. Left stick click: the model on or off.
  if (std::abs(input.leftX) > 0.2f) {
    app.table.modelScale =
        std::clamp(app.table.modelScale * std::exp(input.leftX * dt * 0.8f), kMinModelScale, kMaxModelScale);
  }
  if ((pressed & kButtonLeftStick) != 0) {
    app.table.diorama = !app.table.diorama;
    tick(app);
    notify_placement(app, env);
  }

  if ((pressed & kButtonX) != 0 && passthrough_available(app)) {
    set_passthrough(app, !app.table.passthrough);
    tick(app);
    notify_placement(app, env);
  }
  if ((pressed & kButtonY) != 0) {
    switch_resolution(app, env);
    tick(app);
  }
}

// --- Frame loop ---

// Returns false once the session is over for good (the player quit from the
// headset's menu, or the runtime lost the session).
bool poll_events(App& app, JNIEnv* env) {
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  while (xrPollEvent(app.instance, &event) == XR_SUCCESS) {
    if (app.anchor.handle_event(event)) {
      event = {XR_TYPE_EVENT_DATA_BUFFER};
      continue;
    }
    switch (event.type) {
    case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
      return false;
    case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
      const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
      app.state = changed.state;
      LOGI("Session state %d", static_cast<int>(app.state));
      if (app.state == XR_SESSION_STATE_READY) {
        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        app.running = check(app.instance, xrBeginSession(app.session, &begin), "xrBeginSession");
        if (app.running) {
          request_performance(app);
        }
      } else if (app.state == XR_SESSION_STATE_STOPPING) {
        xrEndSession(app.session);
        app.running = false;
      } else if (app.state == XR_SESSION_STATE_EXITING || app.state == XR_SESSION_STATE_LOSS_PENDING) {
        return false;
      }
      env->CallStaticVoidMethod(g_java.questVr, g_java.onSessionState, app.running,
                                app.state == XR_SESSION_STATE_FOCUSED);
      clear_exception(env);
      break;
    }
    default:
      break;
    }
    event = {XR_TYPE_EVENT_DATA_BUFFER};
  }
  return true;
}

void run_frame(App& app, JNIEnv* env, unsigned& rumbleSerial) {
  XrFrameState frame{XR_TYPE_FRAME_STATE};
  if (!check(app.instance, xrWaitFrame(app.session, nullptr, &frame), "xrWaitFrame")) {
    return;
  }
  xrBeginFrame(app.session, nullptr);
  const XrTime time = frame.predictedDisplayTime;
  const float dt = std::clamp(static_cast<float>(frame.predictedDisplayPeriod) * 1e-9f, 0.004f, 0.05f);
  const bool focused = app.state == XR_SESSION_STATE_FOCUSED;

  // Inputs only mean something while the game has focus: the system menu
  // takes them otherwise, and the pad reads released.
  const ControllerState input = focused ? read_controllers(app) : ControllerState{};
  XrPosef head, aim;
  const bool haveHead = locate(app.view, app.stage, time, head);
  const bool haveAim = focused && locate(app.actions.rightAim, app.stage, time, aim);

  // Where the game stands: the anchor, which follows the real table, unless
  // the player is moving it.
  XrPosef anchored;
  if (!app.placing && app.anchor.locate(time, anchored)) {
    app.pose = anchored;
    app.poseKnown = true;
  }
  if (!app.poseKnown && haveHead) {
    default_pose(app, head);
  }

  const int pressed = input.buttons & ~app.previousButtons;
  app.previousButtons = input.buttons;
  // Placement ends with A, B, the menu button or the right stick again: any of them, so
  // nobody stays stuck in it.
  constexpr int kEndPlacement = kButtonA | kButtonB | kButtonMenu | kButtonRightStick;
  if ((pressed & kButtonRightStick) != 0 || (app.placing && (pressed & kEndPlacement) != 0)) {
    if (app.placing) {
      end_placement(app, env, time, input.buttons);
    } else if (app.poseKnown) {
      begin_placement(app, env);
    }
    tick(app);
  } else if (app.placing) {
    place(app, env, input, pressed, dt, haveHead ? &head : nullptr, haveAim ? &aim : nullptr);
  }

  // Sent every frame (SDL drops repeated values) so the pad is right as soon
  // as SDL opens it.
  ControllerState forGame = app.placing ? ControllerState{} : input;
  app.suppressedButtons &= input.buttons;
  forGame.buttons &= ~app.suppressedButtons;
  send_controllers(env, forGame);
  if (focused) {
    apply_rumble(app, rumbleSerial);
  }
  g_activeRefreshRate.store(current_refresh_rate(app), std::memory_order_relaxed);
  const float requested = g_requestedRefreshRate.exchange(0.0f);
  if (requested > 0.0f && app.requestRefreshRate != nullptr) {
    check(app.instance, app.requestRefreshRate(app.session, requested), "xrRequestDisplayRefreshRateFB");
  }
  if (app.anchor.take_saved_uuid(app.table.anchorUuid)) {
    save_settings(app);
  }
  finish_resolution_switch(app, env);

  // The model on the table: the eyes where the game's next frame will be seen.
  XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
  locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  locateInfo.displayTime = time;
  locateInfo.space = app.stage;
  XrViewState viewState{XR_TYPE_VIEW_STATE};
  uint32_t viewCount = 0;
  constexpr XrViewStateFlags validViews = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
  const bool haveViews = XR_SUCCEEDED(xrLocateViews(app.session, &locateInfo, &viewState, 2, &viewCount, views)) &&
                         viewCount == 2 && (viewState.viewStateFlags & validViews) == validViews;
  const bool model = app.table.diorama && app.poseKnown && haveViews && frame.shouldRender && !app.stereo.screen_required();
  // Invalid tracking must disable new game leases, rather than leave the
  // previous valid eye poses enabled while the headset cannot render.
  XrPosef modelPose = app.pose;
  modelPose.position.y += 0.02f; // clearance above the lowest playable surface
  app.stereo.update(views, modelPose, app.table.modelScale, model, app.table.screenWidth, app.table.screenWidth * 0.75f);
  XrCompositionLayerImageLayoutFB modelFlip{XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB};
  modelFlip.flags = XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;
  XrCompositionLayerSettingsFB modelSettings{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
  modelSettings.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB;
  modelSettings.next = app.extensions.imageLayout ? &modelFlip : nullptr;
  const void* modelChain = app.extensions.layerSettings ? static_cast<const void*>(&modelSettings)
                                                       : modelSettings.next;
  const auto* modelLayer = model ? app.stereo.layer(app.stage, modelChain) : nullptr;

  std::array<const XrCompositionLayerBaseHeader*, 5> layers{};
  uint32_t layerCount = 0;

  XrCompositionLayerPassthroughFB room{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
  if (app.table.passthrough && passthrough_available(app)) {
    room.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    room.layerHandle = app.passthrough.layer;
    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&room);
  }

  // The game's screen, standing on the table. A 4K or 1440p picture is larger
  // than what the headset shows of it: the compositor's supersampling filters
  // it down cleanly; a 1080p one is sharpened instead.
  const float screenWidth = app.table.screenWidth;
  const float screenHeight = screenWidth * static_cast<float>(app.screen.height) / static_cast<float>(app.screen.width);
  XrCompositionLayerSettingsFB settings{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
  settings.layerFlags = app.screen.height > 1080 ? XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB
                                                 : XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB;
  // While placing, the screen is tinted blue: it is being moved, not played.
  XrCompositionLayerColorScaleBiasKHR tint{XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR};
  tint.colorScale = {0.7f, 0.85f, 1.0f, 1.0f};
  // Meta's compositor reads an Android surface bottom row first (OpenGL's
  // origin), which shows it upside down: flip it back.
  XrCompositionLayerImageLayoutFB screenFlip{XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB};
  screenFlip.flags = XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;
  const void* chain = nullptr;
  if (app.extensions.imageLayout) {
    chain = &screenFlip;
  }
  if (app.placing && app.extensions.colorScale) {
    tint.next = chain;
    chain = &tint;
  }
  if (app.extensions.layerSettings) {
    settings.next = chain;
    chain = &settings;
  }
  XrCompositionLayerQuad screen{XR_TYPE_COMPOSITION_LAYER_QUAD};
  screen.next = chain;
  screen.space = app.stage;
  screen.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  screen.subImage.swapchain = app.screen.swapchain;
  screen.subImage.imageRect = {{0, 0}, {app.screen.width, app.screen.height}};
  XrPosef onTable = identity_pose();
  onTable.position.y = kScreenLiftMeters + screenHeight * 0.5f;
  if (model) {
    onTable.position.z = -kScreenBehindModelMeters;
  }
  screen.pose = compose(app.pose, onTable);
  screen.size = {screenWidth, screenHeight};
  if (app.poseKnown && (!modelLayer || !app.stereo.world_visible() || app.placing)) {
    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&screen);
  }

  // The images come from Aurora like the screen's, so they need the same flip.
  if (modelLayer) {
    layers[layerCount++] = modelLayer;
    if (const auto* hud = app.stereo.hud_layer(app.view, app.extensions.imageLayout ? &modelFlip : nullptr)) {
      layers[layerCount++] = hud;
    }
  }

  XrCompositionLayerQuad help{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrCompositionLayerImageLayoutFB helpFlip{XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB};
  helpFlip.flags = XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;
  if (app.placing && app.help.swapchain != XR_NULL_HANDLE) {
    help.next = app.extensions.imageLayout ? &helpFlip : nullptr;
    help.space = app.view;
    help.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    help.subImage.swapchain = app.help.swapchain;
    help.subImage.imageRect = {{0, 0}, {app.help.width, app.help.height}};
    help.pose = identity_pose();
    help.pose.position = kHelpPosition;
    help.size = {kHelpWidthMeters, kHelpWidthMeters * app.help.height / app.help.width};
    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&help);
  }

  XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
  end.displayTime = time;
  end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  end.layerCount = frame.shouldRender ? layerCount : 0;
  end.layers = layers.data();
  check(app.instance, xrEndFrame(app.session, &end), "xrEndFrame");
}

bool set_up(App& app, JNIEnv* env) {
  app.table.load(app.statePath);
  if (app.table.placed) {
    app.pose = app.table.pose;
    app.poseKnown = true;
  }
  if (!(init_loader() && create_instance(app) && create_egl(app.egl) && create_session(app) && create_actions(app))) {
    return false;
  }
  const int resolution = app.table.resolution;
  if (!create_surface_quad(app, env, screen_width_pixels(resolution), resolution, app.screen)) {
    return false;
  }
  create_surface_quad(app, env, kHelpWidthPixels, kHelpHeightPixels, app.help);
  create_passthrough(app);
  // The eyes' images: 4K-class for the pair at the 4K setting.
  const float eyeScale = 0.5f; // balanced mobile MR profile
  if (!app.stereo.init(app.instance, app.session, app.system, app.egl.display, eyeScale)) {
    LOGW("No model on the table: the game stays on its screen");
  }
  query_refresh_rates(app);
  if (app.extensions.anchors) {
    app.anchor.init(app.instance, app.session, app.stage);
    if (!app.table.anchorUuid.empty()) {
      app.anchor.load(app.table.anchorUuid);
    }
  }
  return true;
}

void tear_down(App& app, JNIEnv* env) {
  app.stereo.destroy(); // GL objects: while the context is current
  app.anchor.destroy();
  destroy_passthrough(app);
  for (SurfaceQuad* quad : {&app.screen, &app.next, &app.retiring, &app.help}) {
    destroy_surface_quad(env, *quad);
  }
  if (app.actions.rightAim != XR_NULL_HANDLE) {
    xrDestroySpace(app.actions.rightAim);
  }
  if (app.actions.set != XR_NULL_HANDLE) {
    xrDestroyActionSet(app.actions.set);
  }
  for (XrSpace space : {app.stage, app.view}) {
    if (space != XR_NULL_HANDLE) {
      xrDestroySpace(space);
    }
  }
  if (app.session != XR_NULL_HANDLE) {
    xrDestroySession(app.session);
  }
  if (app.instance != XR_NULL_HANDLE) {
    xrDestroyInstance(app.instance);
  }
  destroy_egl(app.egl);
}

void xr_thread(std::string statePath) {
  JNIEnv* env = nullptr;
  g_java.vm->AttachCurrentThread(&env, nullptr);

  App app;
  app.statePath = std::move(statePath);
  const bool ok = set_up(app, env);

  if (app.help.surface != nullptr) {
    env->CallStaticVoidMethod(g_java.questVr, g_java.onHelpSurface, app.help.surface, app.help.width,
                              app.help.height);
    clear_exception(env);
  }
  env->CallStaticVoidMethod(g_java.questVr, g_java.onSurface, ok ? app.screen.surface : nullptr, app.screen.width,
                            app.screen.height, ok ? current_refresh_rate(app) : 0.0f);
  clear_exception(env);

  if (ok) {
    LOGI("Screen %dx%d, room %s, anchors %s", app.screen.width, app.screen.height,
         passthrough_available(app) ? "visible" : "unavailable", app.extensions.anchors ? "on" : "off");
    // The first time, the player starts by putting the game on the table.
    if (!app.table.placed) {
      app.placing = true;
      app.triggerHeld = true;
    }
    notify_placement(app, env);
    unsigned rumbleSerial = 0;
    while (!g_stop.load(std::memory_order_acquire)) {
      if (!poll_events(app, env)) {
        env->CallStaticVoidMethod(g_java.questVr, g_java.onExit);
        clear_exception(env);
        break;
      }
      if (!app.running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      run_frame(app, env, rumbleSerial);
    }
    if (app.running) {
      xrRequestExitSession(app.session);
      xrEndSession(app.session);
    }
  }

  tear_down(app, env);
  g_java.vm->DetachCurrentThread();
}

jmethodID static_method(JNIEnv* env, jclass clazz, const char* name, const char* signature) {
  jmethodID method = env->GetStaticMethodID(clazz, name, signature);
  if (method == nullptr) {
    env->ExceptionClear();
    LOGW("QuestVr.%s%s missing", name, signature);
  }
  return method;
}

} // namespace

extern "C" {

__attribute__((visibility("default"))) float PartyBoardQuest_ActiveRefreshRate() {
  return g_activeRefreshRate.load(std::memory_order_relaxed);
}

JNIEXPORT jboolean JNICALL Java_com_mariopartyrd_partyboard_quest_QuestVr_nativeStart(JNIEnv* env, jclass clazz,
                                                                                     jobject activity,
                                                                                     jstring statePath) {
  if (g_thread.joinable()) {
    return JNI_TRUE;
  }
  env->GetJavaVM(&g_java.vm);
  // The XR thread is attached from native code, so its FindClass would not see
  // the app's classes: look everything up here, on a Java thread.
  g_java.onSurface = static_method(env, clazz, "onSurface", "(Landroid/view/Surface;IIF)V");
  g_java.onSurfaceReplaced = static_method(env, clazz, "onSurfaceReplaced", "(Landroid/view/Surface;II)V");
  g_java.onHelpSurface = static_method(env, clazz, "onHelpSurface", "(Landroid/view/Surface;II)V");
  g_java.onPlacement = static_method(env, clazz, "onPlacement", "(ZIZZ)V");
  g_java.onSessionState = static_method(env, clazz, "onSessionState", "(ZZ)V");
  g_java.onControllers = static_method(env, clazz, "onControllers", "(IFFFFFFFF)V");
  g_java.onExit = static_method(env, clazz, "onExit", "()V");
  for (jmethodID method : {g_java.onSurface, g_java.onSurfaceReplaced, g_java.onHelpSurface, g_java.onPlacement,
                           g_java.onSessionState, g_java.onControllers, g_java.onExit}) {
    if (method == nullptr) {
      return JNI_FALSE;
    }
  }
  g_java.activity = env->NewGlobalRef(activity);
  g_java.questVr = static_cast<jclass>(env->NewGlobalRef(clazz));

  const char* chars = env->GetStringUTFChars(statePath, nullptr);
  std::string path{chars};
  env->ReleaseStringUTFChars(statePath, chars);

  g_stop.store(false);
  g_thread = std::thread{xr_thread, std::move(path)};
  return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_com_mariopartyrd_partyboard_quest_QuestVr_nativeStop(JNIEnv* env, jclass) {
  if (!g_thread.joinable()) {
    return;
  }
  g_stop.store(true, std::memory_order_release);
  g_thread.join();
  g_activeRefreshRate.store(0.0f);
  env->DeleteGlobalRef(g_java.activity);
  env->DeleteGlobalRef(g_java.questVr);
  g_java = {};
}

JNIEXPORT void JNICALL Java_com_mariopartyrd_partyboard_quest_QuestVr_nativeSurfaceSwitched(JNIEnv*, jclass) {
  g_surfaceSwitched.store(true);
}

JNIEXPORT void JNICALL Java_com_mariopartyrd_partyboard_quest_QuestVr_nativeRumble(JNIEnv*, jclass, jfloat amplitude,
                                                                                  jint durationMs) {
  g_rumbleAmplitude.store(amplitude, std::memory_order_relaxed);
  g_rumbleDurationMs.store(durationMs, std::memory_order_relaxed);
  g_rumbleSerial.fetch_add(1, std::memory_order_release);
}

JNIEXPORT jfloatArray JNICALL Java_com_mariopartyrd_partyboard_quest_QuestVr_nativeRefreshRates(JNIEnv* env, jclass) {
  std::lock_guard lock{g_ratesMutex};
  jfloatArray out = env->NewFloatArray(static_cast<jsize>(g_refreshRates.size()));
  env->SetFloatArrayRegion(out, 0, static_cast<jsize>(g_refreshRates.size()), g_refreshRates.data());
  return out;
}

JNIEXPORT void JNICALL Java_com_mariopartyrd_partyboard_quest_QuestVr_nativeRequestRefreshRate(JNIEnv*, jclass,
                                                                                              jfloat rate) {
  g_requestedRefreshRate.store(rate);
}

} // extern "C"
