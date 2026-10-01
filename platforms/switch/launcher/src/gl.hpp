#pragma once

// The launcher only uses the OpenGL ES 3.0 subset, which is also core in the
// desktop GL 4.3 context the Switch EGL layer creates. Shaders get their
// #version line from Platform::shaderHeader().

#if defined(__SWITCH__)
#include <glad/glad.h>
#else
#include <GLES3/gl3.h>
#endif
