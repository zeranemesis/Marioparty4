#include <cstddef>
#include <cstdlib>
#include <cstdio>

#include <switch.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glad/glad.h>

namespace {

EGLDisplay g_display = EGL_NO_DISPLAY;
EGLContext g_context = EGL_NO_CONTEXT;
EGLSurface g_surface = EGL_NO_SURFACE;

GLuint g_program = 0;
GLuint g_vao = 0;
GLuint g_vbo = 0;

bool initEgl(NWindow* window) {
    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_display == EGL_NO_DISPLAY) {
        std::printf("eglGetDisplay failed: 0x%x\n", eglGetError());
        return false;
    }

    if (eglInitialize(g_display, nullptr, nullptr) == EGL_FALSE) {
        std::printf("eglInitialize failed: 0x%x\n", eglGetError());
        return false;
    }

    if (eglBindAPI(EGL_OPENGL_API) == EGL_FALSE) {
        std::printf("eglBindAPI failed: 0x%x\n", eglGetError());
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
        return false;
    }

    g_surface = eglCreateWindowSurface(g_display, config, window, nullptr);
    if (g_surface == EGL_NO_SURFACE) {
        std::printf("eglCreateWindowSurface failed: 0x%x\n", eglGetError());
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
        return false;
    }

    if (eglMakeCurrent(g_display, g_surface, g_surface, g_context) == EGL_FALSE) {
        std::printf("eglMakeCurrent failed: 0x%x\n", eglGetError());
        return false;
    }

    return true;
}

void shutdownEgl() {
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
}

GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE)
        return shader;

    char log[1024] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    std::printf("Shader compile failed: %s\n", log);
    glDeleteShader(shader);
    return 0;
}

bool initScene() {
    static constexpr const char* kVertexShader = R"(
        #version 330 core
        layout(location = 0) in vec3 aPosition;
        layout(location = 1) in vec3 aColor;
        out vec3 vColor;

        void main() {
            gl_Position = vec4(aPosition, 1.0);
            vColor = aColor;
        }
    )";

    static constexpr const char* kFragmentShader = R"(
        #version 330 core
        in vec3 vColor;
        out vec4 fragColor;

        void main() {
            fragColor = vec4(vColor, 1.0);
        }
    )";

    const GLuint vertexShader = compileShader(GL_VERTEX_SHADER, kVertexShader);
    const GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vertexShader || !fragmentShader)
        return false;

    g_program = glCreateProgram();
    glAttachShader(g_program, vertexShader);
    glAttachShader(g_program, fragmentShader);
    glLinkProgram(g_program);

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    GLint linked = GL_FALSE;
    glGetProgramiv(g_program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char log[1024] = {};
        glGetProgramInfoLog(g_program, sizeof(log), nullptr, log);
        std::printf("Program link failed: %s\n", log);
        return false;
    }

    struct Vertex {
        float position[3];
        float color[3];
    };

    static constexpr Vertex kVertices[] = {
        {{-0.62f, -0.55f, 0.0f}, {0.95f, 0.25f, 0.20f}},
        {{ 0.62f, -0.55f, 0.0f}, {0.20f, 0.85f, 0.35f}},
        {{ 0.00f,  0.62f, 0.0f}, {0.20f, 0.45f, 1.00f}},
    };

    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);

    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kVertices), kVertices, GL_STATIC_DRAW);

    glVertexAttribPointer(
        0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
        reinterpret_cast<const void*>(offsetof(Vertex, position)));
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(
        1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
        reinterpret_cast<const void*>(offsetof(Vertex, color)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
    return true;
}

void shutdownScene() {
    if (g_vbo)
        glDeleteBuffers(1, &g_vbo);
    if (g_vao)
        glDeleteVertexArrays(1, &g_vao);
    if (g_program)
        glDeleteProgram(g_program);

    g_vbo = 0;
    g_vao = 0;
    g_program = 0;
}

void renderFrame(float pulse, u32 width, u32 height) {
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));

    const float blue = 0.10f + pulse * 0.10f;
    glClearColor(0.035f, 0.045f, blue, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(g_program);
    glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

} // namespace

int main(int, char**) {
    NWindow* window = nwindowGetDefault();

    u32 width = 1280;
    u32 height = 720;
    nwindowGetDimensions(window, &width, &height);

    if (!initEgl(window))
        return EXIT_FAILURE;

    if (!gladLoadGL()) {
        std::printf("gladLoadGL failed\n");
        shutdownEgl();
        return EXIT_FAILURE;
    }

    if (!initScene()) {
        shutdownEgl();
        return EXIT_FAILURE;
    }

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    unsigned frame = 0;

    while (appletMainLoop()) {
        padUpdate(&pad);

        const u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_Plus)
            break;

        const float pulse = static_cast<float>((frame / 90u) & 1u);
        renderFrame(pulse, width, height);
        eglSwapBuffers(g_display, g_surface);
        ++frame;
    }

    shutdownScene();
    shutdownEgl();
    return EXIT_SUCCESS;
}
