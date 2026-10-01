#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cmath>

#include <switch.h>
#include <glad/glad.h>
#include <dolphin/pad.h>
#include <dolphin/mtx.h>
#include "partyboard_switch/egl.hpp"

namespace {

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

bool auroraMtxSelfTest() {
    Mtx transform{};
    MTXTrans(transform, 4.0f, 5.0f, 6.0f);

    const Vec source{1.0f, 2.0f, 3.0f};
    Vec result{};
    MTXMultVec(transform, &source, &result);

    constexpr float epsilon = 0.0001f;
    return std::fabs(result.x - 5.0f) < epsilon &&
           std::fabs(result.y - 7.0f) < epsilon &&
           std::fabs(result.z - 9.0f) < epsilon;
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
    if (!auroraMtxSelfTest()) {
        std::printf("Aurora MTX ARM64 self-test failed\n");
        return EXIT_FAILURE;
    }

    if (!PartyBoardSwitch_EglInitialize())
        return EXIT_FAILURE;

    const u32 width = PartyBoardSwitch_FramebufferWidth();
    const u32 height = PartyBoardSwitch_FramebufferHeight();

    if (!gladLoadGL()) {
        std::printf("gladLoadGL failed\n");
        PartyBoardSwitch_EglShutdown();
        return EXIT_FAILURE;
    }

    // Report the exact EGL requirements Dawn's OpenGL backend checks during
    // adapter discovery. The normal render probe still runs if they are absent.
    PartyBoardSwitch_EglHasDawnRequirements();

    if (!initScene()) {
        PartyBoardSwitch_EglShutdown();
        return EXIT_FAILURE;
    }

    PADInit();
    PADStatus pads[PAD_CHANMAX] = {};
    unsigned frame = 0;

    while (appletMainLoop()) {
        PADRead(pads);

        bool exitRequested = false;
        bool aHeld = false;
        for (unsigned i = 0; i < PAD_CHANMAX; ++i) {
            if (pads[i].err != PAD_ERR_NONE)
                continue;
            exitRequested |= (pads[i].button & PAD_BUTTON_START) != 0;
            aHeld |= (pads[i].button & PAD_BUTTON_A) != 0;
        }
        if (exitRequested)
            break;

        // Holding the GameCube A mapping brightens the background. This makes
        // the bootstrap validate the exact PAD API consumed by Mario Party 4,
        // not a second libnx-only input path.
        const float pulse = aHeld ? 1.0f : static_cast<float>((frame / 90u) & 1u);
        renderFrame(pulse, width, height);
        PartyBoardSwitch_SwapBuffers();
        ++frame;
    }

    shutdownScene();
    PartyBoardSwitch_EglShutdown();
    return EXIT_SUCCESS;
}
