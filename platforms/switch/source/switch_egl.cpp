#include "partyboard_switch/egl.hpp"

#include <cstdio>
#include <cstring>

namespace {

EGLDisplay g_display = EGL_NO_DISPLAY;
EGLContext g_context = EGL_NO_CONTEXT;
EGLSurface g_surface = EGL_NO_SURFACE;
NWindow* g_window = nullptr;
uint32_t g_width = 1280;
uint32_t g_height = 720;

} // namespace

bool PartyBoardSwitch_EglInitializeDisplay() {
    if (g_display != EGL_NO_DISPLAY)
        return true;

    g_window = nwindowGetDefault();
    nwindowGetDimensions(g_window, &g_width, &g_height);

    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_display == EGL_NO_DISPLAY) {
        std::printf("eglGetDisplay failed: 0x%x\n", eglGetError());
        return false;
    }

    if (eglInitialize(g_display, nullptr, nullptr) == EGL_FALSE) {
        std::printf("eglInitialize failed: 0x%x\n", eglGetError());
        g_display = EGL_NO_DISPLAY;
        g_window = nullptr;
        return false;
    }

    return true;
}

bool PartyBoardSwitch_EglInitialize() {
    if (g_context != EGL_NO_CONTEXT && g_surface != EGL_NO_SURFACE)
        return true;

    if (!PartyBoardSwitch_EglInitializeDisplay())
        return false;

    if (eglBindAPI(EGL_OPENGL_API) == EGL_FALSE) {
        std::printf("eglBindAPI failed: 0x%x\n", eglGetError());
        PartyBoardSwitch_EglShutdown();
        return false;
    }

    const EGLint configAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE,
    };

    EGLConfig config = nullptr;
    EGLint configCount = 0;
    if (eglChooseConfig(g_display, configAttribs, &config, 1, &configCount) == EGL_FALSE ||
        configCount == 0) {
        std::printf("eglChooseConfig failed: 0x%x\n", eglGetError());
        PartyBoardSwitch_EglShutdown();
        return false;
    }

    g_surface = eglCreateWindowSurface(g_display, config, g_window, nullptr);
    if (g_surface == EGL_NO_SURFACE) {
        std::printf("eglCreateWindowSurface failed: 0x%x\n", eglGetError());
        PartyBoardSwitch_EglShutdown();
        return false;
    }

    const EGLint contextAttribs[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
        EGL_CONTEXT_MAJOR_VERSION_KHR, 4,
        EGL_CONTEXT_MINOR_VERSION_KHR, 3,
        EGL_NONE,
    };

    g_context = eglCreateContext(g_display, config, EGL_NO_CONTEXT, contextAttribs);
    if (g_context == EGL_NO_CONTEXT) {
        std::printf("eglCreateContext failed: 0x%x\n", eglGetError());
        PartyBoardSwitch_EglShutdown();
        return false;
    }

    if (eglMakeCurrent(g_display, g_surface, g_surface, g_context) == EGL_FALSE) {
        std::printf("eglMakeCurrent failed: 0x%x\n", eglGetError());
        PartyBoardSwitch_EglShutdown();
        return false;
    }

    return true;
}

void PartyBoardSwitch_EglShutdown() {
    if (g_display == EGL_NO_DISPLAY)
        return;

    eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (g_context != EGL_NO_CONTEXT) {
        eglDestroyContext(g_display, g_context);
        g_context = EGL_NO_CONTEXT;
    }

    if (g_surface != EGL_NO_SURFACE) {
        eglDestroySurface(g_display, g_surface);
        g_surface = EGL_NO_SURFACE;
    }

    eglTerminate(g_display);
    g_display = EGL_NO_DISPLAY;
    g_window = nullptr;
}

EGLDisplay PartyBoardSwitch_EglDisplay() {
    return g_display;
}

EGLContext PartyBoardSwitch_EglContext() {
    return g_context;
}

EGLSurface PartyBoardSwitch_EglSurface() {
    return g_surface;
}

NWindow* PartyBoardSwitch_NativeWindow() {
    return g_window;
}

uint32_t PartyBoardSwitch_FramebufferWidth() {
    return g_width;
}

uint32_t PartyBoardSwitch_FramebufferHeight() {
    return g_height;
}

bool PartyBoardSwitch_SwapBuffers() {
    return g_display != EGL_NO_DISPLAY &&
           g_surface != EGL_NO_SURFACE &&
           eglSwapBuffers(g_display, g_surface) == EGL_TRUE;
}

namespace {

bool hasExtension(const char* extensions, const char* name) {
    if (!extensions || !name || !*name || std::strchr(name, ' '))
        return false;

    const size_t nameLen = std::strlen(name);
    const char* cursor = extensions;
    while ((cursor = std::strstr(cursor, name)) != nullptr) {
        const bool startsAtBoundary = cursor == extensions || cursor[-1] == ' ';
        const char after = cursor[nameLen];
        const bool endsAtBoundary = after == '\0' || after == ' ';
        if (startsAtBoundary && endsAtBoundary)
            return true;
        cursor += nameLen;
    }
    return false;
}

} // namespace

bool PartyBoardSwitch_EglHasDawnRequirements() {
    if (g_display == EGL_NO_DISPLAY)
        return false;

    const char* extensions = eglQueryString(g_display, EGL_EXTENSIONS);
    const bool robust =
        hasExtension(extensions, "EGL_EXT_create_context_robustness");
    const bool sync =
        hasExtension(extensions, "EGL_KHR_fence_sync") ||
        hasExtension(extensions, "EGL_KHR_reusable_sync");

    std::printf("Dawn EGL requirements: robustness=%s sync=%s\n",
                robust ? "yes" : "no",
                sync ? "yes" : "no");
    return robust && sync;
}
