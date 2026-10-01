if(NOT DEFINED DAWN_SOURCE_DIR)
    message(FATAL_ERROR "DAWN_SOURCE_DIR is required")
endif()

# This patch is deliberately applied only to FetchContent's build-tree copy of
# Dawn. The pinned upstream source and PartyBoard's Aurora submodule stay clean.

set(_platform "${DAWN_SOURCE_DIR}/src/utils/platform.h")
if(NOT EXISTS "${_platform}")
    message(FATAL_ERROR "Dawn platform header not found: ${_platform}")
endif()

file(READ "${_platform}" _platform_content)
if(NOT _platform_content MATCHES "DAWN_PLATFORM_IS_SWITCH")
    set(_anchor_os "#elif defined(__Fuchsia__)\n#define DAWN_PLATFORM_IS_FUCHSIA 1\n#define DAWN_PLATFORM_IS_POSIX 1")
    set(_switch_os "#elif defined(__SWITCH__)\n#define DAWN_PLATFORM_IS_SWITCH 1\n#define DAWN_PLATFORM_IS_POSIX 1\n\n#elif defined(__Fuchsia__)\n#define DAWN_PLATFORM_IS_FUCHSIA 1\n#define DAWN_PLATFORM_IS_POSIX 1")
    string(REPLACE "${_anchor_os}" "${_switch_os}" _platform_content "${_platform_content}")

    set(_anchor_default "#if !defined(DAWN_PLATFORM_IS_FUCHSIA)\n#define DAWN_PLATFORM_IS_FUCHSIA 0\n#endif")
    set(_switch_default "#if !defined(DAWN_PLATFORM_IS_SWITCH)\n#define DAWN_PLATFORM_IS_SWITCH 0\n#endif\n\n#if !defined(DAWN_PLATFORM_IS_FUCHSIA)\n#define DAWN_PLATFORM_IS_FUCHSIA 0\n#endif")
    string(REPLACE "${_anchor_default}" "${_switch_default}" _platform_content "${_platform_content}")

    if(NOT _platform_content MATCHES "defined\\(__SWITCH__\\)")
        message(FATAL_ERROR "Failed to patch Dawn OS detection for __SWITCH__")
    endif()

    file(WRITE "${_platform}" "${_platform_content}")
    message(STATUS "Patched Dawn platform detection for libnx")
endif()

# The OpenGLES adapter is supplied an existing EGLDisplay + eglGetProcAddress,
# so Dawn never needs to dynamically load libEGL on Switch. libnx has no
# dlfcn.h; make DynamicLib an explicit unsupported/no-op service on this target
# instead of pretending the console is Linux.
set(_dynamic "${DAWN_SOURCE_DIR}/src/dawn/common/DynamicLib.cpp")
if(NOT EXISTS "${_dynamic}")
    message(FATAL_ERROR "Dawn DynamicLib source not found: ${_dynamic}")
endif()

file(READ "${_dynamic}" _dynamic_content)
if(NOT _dynamic_content MATCHES "Dynamic loading is unavailable on libnx")
    string(REPLACE
        "#elif DAWN_PLATFORM_IS(POSIX)\n#include <dlfcn.h>"
        "#elif DAWN_PLATFORM_IS(SWITCH)\n// Dynamic loading is unavailable on libnx. The Switch OpenGLES path supplies\n// eglGetProcAddress and EGLDisplay explicitly, so no shared library is needed.\n#elif DAWN_PLATFORM_IS(POSIX)\n#include <dlfcn.h>"
        _dynamic_content "${_dynamic_content}")

    string(REPLACE
        "#elif DAWN_PLATFORM_IS(POSIX)\n    mHandle = dlopen(filename.c_str(), RTLD_NOW);"
        "#elif DAWN_PLATFORM_IS(SWITCH)\n    mHandle = nullptr;\n    if (error != nullptr) {\n        *error = \"Dynamic loading is unavailable on libnx\";\n    }\n#elif DAWN_PLATFORM_IS(POSIX)\n    mHandle = dlopen(filename.c_str(), RTLD_NOW);"
        _dynamic_content "${_dynamic_content}")

    string(REPLACE
        "#elif DAWN_PLATFORM_IS(POSIX)\n    mHandle = dlopen(filename.c_str(), RTLD_NOW | RTLD_NOLOAD);"
        "#elif DAWN_PLATFORM_IS(SWITCH)\n    mHandle = nullptr;\n    if (error != nullptr) {\n        *error = \"Dynamic loading is unavailable on libnx\";\n    }\n#elif DAWN_PLATFORM_IS(POSIX)\n    mHandle = dlopen(filename.c_str(), RTLD_NOW | RTLD_NOLOAD);"
        _dynamic_content "${_dynamic_content}")

    string(REPLACE
        "#elif DAWN_PLATFORM_IS(POSIX)\n        dlclose(mHandle);"
        "#elif DAWN_PLATFORM_IS(SWITCH)\n        // No dynamic libraries are opened on libnx.\n#elif DAWN_PLATFORM_IS(POSIX)\n        dlclose(mHandle);"
        _dynamic_content "${_dynamic_content}")

    string(REPLACE
        "#elif DAWN_PLATFORM_IS(POSIX)\n    proc = reinterpret_cast<void*>(dlsym(mHandle, procName.c_str()));"
        "#elif DAWN_PLATFORM_IS(SWITCH)\n    if (error != nullptr) {\n        *error = \"Dynamic loading is unavailable on libnx\";\n    }\n#elif DAWN_PLATFORM_IS(POSIX)\n    proc = reinterpret_cast<void*>(dlsym(mHandle, procName.c_str()));"
        _dynamic_content "${_dynamic_content}")

    if(NOT _dynamic_content MATCHES "DAWN_PLATFORM_IS\\(SWITCH\\)")
        message(FATAL_ERROR "Failed to patch Dawn DynamicLib for libnx")
    endif()

    file(WRITE "${_dynamic}" "${_dynamic_content}")
    message(STATUS "Patched Dawn DynamicLib for external EGL/libnx")
endif()


# Add an opaque EGL-native-window surface path. This avoids adding a new
# generated WebGPU sType: PartyBoard creates the surface through Dawn's native
# OpenGL extension, and the existing SwapChainEGL owns presentation from there.
set(_surface_h "${DAWN_SOURCE_DIR}/src/dawn/native/Surface.h")
set(_surface_cpp "${DAWN_SOURCE_DIR}/src/dawn/native/Surface.cpp")
set(_opengl_h "${DAWN_SOURCE_DIR}/include/dawn/native/OpenGLBackend.h")
set(_opengl_cpp "${DAWN_SOURCE_DIR}/src/dawn/native/opengl/OpenGLBackend.cpp")
set(_swapchain_cpp "${DAWN_SOURCE_DIR}/src/dawn/native/opengl/SwapChainEGL.cpp")

foreach(_required IN ITEMS "${_surface_h}" "${_surface_cpp}" "${_opengl_h}" "${_opengl_cpp}" "${_swapchain_cpp}")
    if(NOT EXISTS "${_required}")
        message(FATAL_ERROR "Required Dawn surface source not found: ${_required}")
    endif()
endforeach()

file(READ "${_surface_h}" _surface_h_content)
if(NOT _surface_h_content MATCHES "EGLNativeWindow")
    string(REPLACE
        "    static Ref<Surface> MakeError(InstanceBase* instance);"
        "    static Ref<Surface> MakeError(InstanceBase* instance);\n    static Ref<Surface> CreateEGLNativeWindow(InstanceBase* instance, void* window);"
        _surface_h_content "${_surface_h_content}")

    string(REPLACE
        "        AndroidWindow,\n        MetalLayer,"
        "        AndroidWindow,\n        EGLNativeWindow,\n        MetalLayer,"
        _surface_h_content "${_surface_h_content}")

    string(REPLACE
        "    void* GetAndroidNativeWindow() const;"
        "    void* GetAndroidNativeWindow() const;\n\n    // Valid to call if the type is EGLNativeWindow\n    void* GetEGLNativeWindow() const;"
        _surface_h_content "${_surface_h_content}")

    string(REPLACE
        "    Surface(InstanceBase* instance, ErrorMonad::ErrorTag tag);"
        "    Surface(InstanceBase* instance, ErrorMonad::ErrorTag tag);\n    Surface(InstanceBase* instance, void* eglNativeWindow);"
        _surface_h_content "${_surface_h_content}")

    string(REPLACE
        "    // ANativeWindow\n    raw_ptr<void> mAndroidNativeWindow = nullptr;"
        "    // ANativeWindow\n    raw_ptr<void> mAndroidNativeWindow = nullptr;\n\n    // Opaque EGLNativeWindowType supplied by PartyBoard/libnx.\n    raw_ptr<void> mEGLNativeWindow = nullptr;"
        _surface_h_content "${_surface_h_content}")

    if(NOT _surface_h_content MATCHES "CreateEGLNativeWindow")
        message(FATAL_ERROR "Failed to patch Dawn Surface.h for EGL native window")
    endif()
    file(WRITE "${_surface_h}" "${_surface_h_content}")
endif()

file(READ "${_surface_cpp}" _surface_cpp_content)
if(NOT _surface_cpp_content MATCHES "CreateEGLNativeWindow")
    string(REPLACE
        "        case Surface::Type::AndroidWindow:\n            s->Append(\"AndroidWindow\");\n            break;"
        "        case Surface::Type::AndroidWindow:\n            s->Append(\"AndroidWindow\");\n            break;\n        case Surface::Type::EGLNativeWindow:\n            s->Append(\"EGLNativeWindow\");\n            break;"
        _surface_cpp_content "${_surface_cpp_content}")

    string(REPLACE
        "Ref<Surface> Surface::MakeError(InstanceBase* instance) {\n    return AcquireRef(new Surface(instance, ErrorMonad::kError));\n}"
        "Ref<Surface> Surface::MakeError(InstanceBase* instance) {\n    return AcquireRef(new Surface(instance, ErrorMonad::kError));\n}\n\n// static\nRef<Surface> Surface::CreateEGLNativeWindow(InstanceBase* instance, void* window) {\n    DAWN_CHECK(window != nullptr);\n    return AcquireRef(new Surface(instance, window));\n}"
        _surface_cpp_content "${_surface_cpp_content}")

    string(REPLACE
        "Surface::Surface(InstanceBase* instance, ErrorTag tag) : ErrorMonad(tag), mInstance(instance) {}"
        "Surface::Surface(InstanceBase* instance, ErrorTag tag) : ErrorMonad(tag), mInstance(instance) {}\n\nSurface::Surface(InstanceBase* instance, void* eglNativeWindow)\n    : ErrorMonad(),\n      mInstance(instance),\n      mCapabilityCache(std::make_unique<AdapterSurfaceCapCache>()),\n      mType(Type::EGLNativeWindow),\n      mEGLNativeWindow(eglNativeWindow) {}"
        _surface_cpp_content "${_surface_cpp_content}")

    string(REPLACE
        "void* Surface::GetAndroidNativeWindow() const {\n    DAWN_CHECK(!IsError());\n    DAWN_CHECK(mType == Type::AndroidWindow);\n    return mAndroidNativeWindow;\n}"
        "void* Surface::GetAndroidNativeWindow() const {\n    DAWN_CHECK(!IsError());\n    DAWN_CHECK(mType == Type::AndroidWindow);\n    return mAndroidNativeWindow;\n}\n\nvoid* Surface::GetEGLNativeWindow() const {\n    DAWN_CHECK(!IsError());\n    DAWN_CHECK(mType == Type::EGLNativeWindow);\n    return mEGLNativeWindow;\n}"
        _surface_cpp_content "${_surface_cpp_content}")

    if(NOT _surface_cpp_content MATCHES "Surface::CreateEGLNativeWindow")
        message(FATAL_ERROR "Failed to patch Dawn Surface.cpp for EGL native window")
    endif()
    file(WRITE "${_surface_cpp}" "${_surface_cpp_content}")
endif()

file(READ "${_opengl_h}" _opengl_h_content)
if(NOT _opengl_h_content MATCHES "CreateSurfaceFromEGLNativeWindow")
    string(REPLACE
        "struct DAWN_NATIVE_EXPORT ExternalImageDescriptorEGLImage : ExternalImageDescriptor {"
        "// Creates a Dawn surface around an EGLNativeWindowType supplied by the host.\n// Intended for platforms such as libnx that are not represented by a generated WebGPU surface sType.\nDAWN_NATIVE_EXPORT WGPUSurface\nCreateSurfaceFromEGLNativeWindow(WGPUInstance instance, void* window);\n\nstruct DAWN_NATIVE_EXPORT ExternalImageDescriptorEGLImage : ExternalImageDescriptor {"
        _opengl_h_content "${_opengl_h_content}")

    if(NOT _opengl_h_content MATCHES "CreateSurfaceFromEGLNativeWindow")
        message(FATAL_ERROR "Failed to patch Dawn OpenGLBackend.h surface helper")
    endif()
    file(WRITE "${_opengl_h}" "${_opengl_h_content}")
endif()

file(READ "${_opengl_cpp}" _opengl_cpp_content)
if(NOT _opengl_cpp_content MATCHES "CreateSurfaceFromEGLNativeWindow")
    string(REPLACE
        "#include \"src/dawn/native/opengl/DeviceGL.h\""
        "#include \"src/dawn/native/opengl/DeviceGL.h\"\n#include \"src/dawn/native/Instance.h\"\n#include \"src/dawn/native/Surface.h\""
        _opengl_cpp_content "${_opengl_cpp_content}")

    string(REPLACE
        "ExternalImageDescriptorEGLImage::ExternalImageDescriptorEGLImage()"
        "WGPUSurface CreateSurfaceFromEGLNativeWindow(WGPUInstance instance, void* window) {\n    if (instance == nullptr || window == nullptr) {\n        return nullptr;\n    }\n    Ref<Surface> surface = Surface::CreateEGLNativeWindow(FromAPI(instance), window);\n    return ToAPI(ReturnToAPI(std::move(surface)));\n}\n\nExternalImageDescriptorEGLImage::ExternalImageDescriptorEGLImage()"
        _opengl_cpp_content "${_opengl_cpp_content}")

    if(NOT _opengl_cpp_content MATCHES "WGPUSurface CreateSurfaceFromEGLNativeWindow")
        message(FATAL_ERROR "Failed to patch Dawn OpenGLBackend.cpp surface helper")
    endif()
    file(WRITE "${_opengl_cpp}" "${_opengl_cpp_content}")
endif()

file(READ "${_swapchain_cpp}" _swapchain_content)
if(NOT _swapchain_content MATCHES "GetEGLNativeWindow")
    string(REPLACE
        "        switch (surface->GetType()) {"
        "        switch (surface->GetType()) {\n#if DAWN_PLATFORM_IS(SWITCH)\n            case Surface::Type::EGLNativeWindow:\n                mEGLSurface = egl.CreateWindowSurface(\n                    eglDisplay, config,\n                    static_cast<EGLNativeWindowType>(surface->GetEGLNativeWindow()),\n                    attribs.data());\n                return {};\n#endif  // DAWN_PLATFORM_IS(SWITCH)"
        _swapchain_content "${_swapchain_content}")

    if(NOT _swapchain_content MATCHES "GetEGLNativeWindow")
        message(FATAL_ERROR "Failed to patch Dawn SwapChainEGL for libnx")
    endif()
    file(WRITE "${_swapchain_cpp}" "${_swapchain_content}")
endif()

message(STATUS "Patched Dawn native EGL-window surface for libnx")
