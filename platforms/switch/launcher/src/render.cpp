#include "render.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>

#include "gl.hpp"

namespace partyboard::launcher {

namespace {

constexpr size_t kMaxQuads = 4096; // 1 MiB of vertices; a busy frame uses ~1500 quads
constexpr float kHuge = 1.0e5f;

constexpr const char* kVertex2D = R"(
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec2 aLocal;
layout(location = 4) in vec2 aHalf;
layout(location = 5) in vec4 aParams;
uniform vec2 uViewport;
out vec2 vUV;
out vec4 vColor;
out vec2 vLocal;
out vec2 vHalf;
out vec4 vParams;
void main() {
    vec2 ndc = vec2(aPos.x / uViewport.x * 2.0 - 1.0, 1.0 - aPos.y / uViewport.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = aUV;
    vColor = aColor;
    vLocal = aLocal;
    vHalf = aHalf;
    vParams = aParams;
}
)";

constexpr const char* kFragment2D = R"(
in vec2 vUV;
in vec4 vColor;
in vec2 vLocal;
in vec2 vHalf;
in vec4 vParams;
uniform sampler2D uTex;
uniform float uTime;
layout(location = 0) out vec4 fragColor;

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// "Tumbling blocks": a hexagon lattice whose cells are split into the three
// visible faces of an isometric cube, drifting slowly behind the shelf.
vec3 cubeLattice(vec2 uv) {
    const vec2 s = vec2(1.0, 1.7320508);
    vec2 p = uv * vec2(1280.0, 720.0) / 92.0 + vec2(uTime * 0.03, -uTime * 0.018);
    vec4 hc = floor(vec4(p, p - vec2(0.5, 1.0)) / s.xyxy) + 0.5;
    vec4 h = vec4(p - hc.xy * s, p - (hc.zw + 0.5) * s);
    vec2 q = dot(h.xy, h.xy) < dot(h.zw, h.zw) ? h.xy : h.zw;
    q.y = -q.y;
    float a = atan(q.y, q.x);
    float face = (a > 0.5236 && a < 2.618) ? 0.055 : ((a > -1.5708 && a <= 0.5236) ? -0.045 : 0.0);
    float edge = max(abs(q.x), max(abs(dot(q, vec2(0.5, 0.8660254))), abs(dot(q, vec2(-0.5, 0.8660254))))) - 0.5;
    float line = 1.0 - smoothstep(0.0, 0.035, abs(edge));
    float mask = mix(1.0, 0.25, uv.y);
    return (vec3(face) + vec3(0.55, 0.5, 1.0) * line * 0.05) * mask;
}

void main() {
    int kind = int(vParams.z + 0.5);
    vec4 color = vColor;
    float d = sdRoundBox(vLocal, vHalf, vParams.x);
    float aa = max(fwidth(d), 0.0001);
    float coverage;
    if (kind == 3) {
        float t = vParams.y;
        float od = abs(d + t * 0.5) - t * 0.5;
        coverage = clamp(0.5 - od / aa, 0.0, 1.0);
    } else if (vParams.w > 0.0) {
        coverage = 1.0 - smoothstep(-vParams.w, vParams.w, d);
    } else {
        coverage = clamp(0.5 - d / aa, 0.0, 1.0);
    }
    if (kind == 1) {
        color *= texture(uTex, vUV, vParams.y);
    } else if (kind == 2) {
        color.a *= texture(uTex, vUV).r;
    } else if (kind == 4) {
        float glow = 1.0 - smoothstep(0.0, 0.85, length((vUV - vec2(0.5, 0.18)) * vec2(1.0, 1.3)));
        color.rgb += vec3(0.07, 0.05, 0.15) * glow;
        color.rgb += cubeLattice(vUV) * vParams.y;
    }
    fragColor = vec4(color.rgb, color.a * coverage);
}
)";

constexpr const char* kVertex3D = R"(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vNormal;
out vec3 vWorld;
out vec3 vLocal;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vNormal = mat3(uModel) * aNormal;
    vWorld = (uModel * vec4(aPos, 1.0)).xyz;
    vLocal = aPos;
}
)";

constexpr const char* kFragment3D = R"(
in vec3 vNormal;
in vec3 vWorld;
in vec3 vLocal;
uniform vec4 uColor;
uniform float uEmissive;
uniform float uGloss;
uniform vec3 uEye;
layout(location = 0) out vec4 fragColor;
void main() {
    vec3 n = normalize(vNormal);
    vec3 l = normalize(vec3(-0.45, 0.9, 0.55));
    vec3 v = normalize(uEye - vWorld);
    float diffuse = max(dot(n, l), 0.0);
    float spec = pow(max(dot(n, normalize(l + v)), 0.0), 40.0) * uGloss;
    float rim = pow(1.0 - max(dot(n, v), 0.0), 3.0);
    // Bevel highlight: the second-largest local coordinate approaches the
    // half-extent only near a cube edge.
    vec3 a = abs(vLocal) * 2.0;
    float second = a.x + a.y + a.z - max(a.x, max(a.y, a.z)) - min(a.x, min(a.y, a.z));
    float bevel = smoothstep(0.84, 0.97, second);
    vec3 base = uColor.rgb * (0.34 + 0.76 * diffuse) + vec3(spec) + uColor.rgb * rim * 0.45;
    base = mix(base, base * 1.25 + vec3(0.10, 0.09, 0.16), bevel * 0.7);
    base += uColor.rgb * uEmissive;
    fragColor = vec4(base, uColor.a);
}
)";

unsigned compile(unsigned type, const char* header, const char* body) {
    const unsigned shader = glCreateShader(type);
    const char* sources[2] = {header, body};
    glShaderSource(shader, 2, sources, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[2048] = {};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::printf("launcher: shader compile failed: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

unsigned link(const char* header, const char* vs, const char* fs) {
    const unsigned v = compile(GL_VERTEX_SHADER, header, vs);
    const unsigned f = compile(GL_FRAGMENT_SHADER, header, fs);
    if (!v || !f) {
        if (v)
            glDeleteShader(v);
        if (f)
            glDeleteShader(f);
        return 0;
    }
    const unsigned program = glCreateProgram();
    glAttachShader(program, v);
    glAttachShader(program, f);
    glLinkProgram(program);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[2048] = {};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::printf("launcher: program link failed: %s\n", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

// 36 vertices: position + outward normal, unit cube centred on the origin.
std::vector<float> cubeMesh() {
    struct FaceDef {
        Vec3 normal, u, v;
    };
    const FaceDef faces[6] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    };
    std::vector<float> data;
    data.reserve(36 * 6);
    for (const FaceDef& f : faces) {
        const Vec3 c = f.normal * 0.5f;
        const Vec3 corners[4] = {c - f.u * 0.5f - f.v * 0.5f, c + f.u * 0.5f - f.v * 0.5f,
                                 c + f.u * 0.5f + f.v * 0.5f, c - f.u * 0.5f + f.v * 0.5f};
        const int order[6] = {0, 1, 2, 0, 2, 3};
        for (int i : order) {
            data.insert(data.end(), {corners[i].x, corners[i].y, corners[i].z, f.normal.x, f.normal.y, f.normal.z});
        }
    }
    return data;
}

} // namespace

bool Renderer::init(const char* shaderHeader, const FontBlob& regular, const FontBlob& bold, const FontBlob& display) {
    m_program2D = link(shaderHeader, kVertex2D, kFragment2D);
    m_program3D = link(shaderHeader, kVertex3D, kFragment3D);
    if (!m_program2D || !m_program3D)
        return false;

    m_loc2DViewport = glGetUniformLocation(m_program2D, "uViewport");
    m_loc2DTime = glGetUniformLocation(m_program2D, "uTime");
    m_loc2DTex = glGetUniformLocation(m_program2D, "uTex");
    m_loc3DMVP = glGetUniformLocation(m_program3D, "uMVP");
    m_loc3DModel = glGetUniformLocation(m_program3D, "uModel");
    m_loc3DColor = glGetUniformLocation(m_program3D, "uColor");
    m_loc3DEmissive = glGetUniformLocation(m_program3D, "uEmissive");
    m_loc3DEye = glGetUniformLocation(m_program3D, "uEye");
    m_loc3DGloss = glGetUniformLocation(m_program3D, "uGloss");

    // 2D batch: dynamic vertices, static quad indices.
    glGenVertexArrays(1, &m_vao2D);
    glGenBuffers(1, &m_vbo2D);
    glGenBuffers(1, &m_ibo2D);
    glBindVertexArray(m_vao2D);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo2D);
    glBufferData(GL_ARRAY_BUFFER, kMaxQuads * 4 * sizeof(Vertex), nullptr, GL_DYNAMIC_DRAW);
    std::vector<uint16_t> indices(kMaxQuads * 6);
    for (size_t q = 0; q < kMaxQuads; ++q) {
        const auto base = static_cast<uint16_t>(q * 4);
        const uint16_t quad[6] = {base, uint16_t(base + 1), uint16_t(base + 2), base, uint16_t(base + 2),
                                  uint16_t(base + 3)};
        std::copy(quad, quad + 6, indices.begin() + q * 6);
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo2D);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint16_t), indices.data(), GL_STATIC_DRAW);
    const GLsizei stride = sizeof(Vertex);
    const struct {
        int size;
        size_t offset;
    } attribs[6] = {{2, offsetof(Vertex, x)},  {2, offsetof(Vertex, u)},  {4, offsetof(Vertex, r)},
                    {2, offsetof(Vertex, lx)}, {2, offsetof(Vertex, hw)}, {4, offsetof(Vertex, radius)}};
    for (GLuint i = 0; i < 6; ++i) {
        glEnableVertexAttribArray(i);
        glVertexAttribPointer(i, attribs[i].size, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<const void*>(attribs[i].offset));
    }

    glGenVertexArrays(1, &m_vao3D);
    glGenBuffers(1, &m_vbo3D);
    glBindVertexArray(m_vao3D);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo3D);
    const std::vector<float> mesh = cubeMesh();
    glBufferData(GL_ARRAY_BUFFER, mesh.size() * sizeof(float), mesh.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<const void*>(3 * sizeof(float)));
    glBindVertexArray(0);

    const uint8_t white[4] = {255, 255, 255, 255};
    glGenTextures(1, &m_whiteTexture);
    glBindTexture(GL_TEXTURE_2D, m_whiteTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    m_boundTexture = m_whiteTexture;

    if (!m_fonts.init(regular, bold, display, [this] { flush(); }))
        return false;

    m_vertices.reserve(kMaxQuads * 4);
    return true;
}

void Renderer::shutdown() {
    m_fonts.shutdown();
    destroyTargets();
    if (m_whiteTexture)
        glDeleteTextures(1, &m_whiteTexture);
    if (m_vbo2D)
        glDeleteBuffers(1, &m_vbo2D);
    if (m_ibo2D)
        glDeleteBuffers(1, &m_ibo2D);
    if (m_vbo3D)
        glDeleteBuffers(1, &m_vbo3D);
    if (m_vao2D)
        glDeleteVertexArrays(1, &m_vao2D);
    if (m_vao3D)
        glDeleteVertexArrays(1, &m_vao3D);
    if (m_program2D)
        glDeleteProgram(m_program2D);
    if (m_program3D)
        glDeleteProgram(m_program3D);
    m_whiteTexture = m_vbo2D = m_ibo2D = m_vbo3D = m_vao2D = m_vao3D = m_program2D = m_program3D = 0;
}

bool Renderer::createTargets(int width, int height) {
    destroyTargets();
    GLint maxSamples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    const GLsizei samples = std::min<GLint>(4, maxSamples);
    if (samples < 2)
        return false;

    glGenFramebuffers(1, &m_msaaFbo);
    glGenRenderbuffers(1, &m_msaaColor);
    glGenRenderbuffers(1, &m_msaaDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, m_msaaColor);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, m_msaaDepth);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, m_msaaFbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_msaaColor);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_msaaDepth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::printf("launcher: MSAA target unavailable, rendering without it\n");
        destroyTargets();
        return false;
    }
    m_targetWidth = width;
    m_targetHeight = height;
    return true;
}

void Renderer::destroyTargets() {
    if (m_msaaFbo)
        glDeleteFramebuffers(1, &m_msaaFbo);
    if (m_msaaColor)
        glDeleteRenderbuffers(1, &m_msaaColor);
    if (m_msaaDepth)
        glDeleteRenderbuffers(1, &m_msaaDepth);
    m_msaaFbo = m_msaaColor = m_msaaDepth = 0;
    m_targetWidth = m_targetHeight = 0;
}

void Renderer::beginFrame(int framebufferWidth, int framebufferHeight, unsigned presentFramebuffer, float time) {
    m_fbWidth = framebufferWidth;
    m_fbHeight = framebufferHeight;
    m_presentFbo = presentFramebuffer;
    m_scale = static_cast<float>(framebufferHeight) / kHeight;
    m_time = time;
    m_clips.clear();

    if (!m_msaaUnavailable && (m_targetWidth != framebufferWidth || m_targetHeight != framebufferHeight))
        m_msaaUnavailable = !createTargets(framebufferWidth, framebufferHeight);

    glBindFramebuffer(GL_FRAMEBUFFER, m_msaaFbo ? m_msaaFbo : m_presentFbo);
    glViewport(0, 0, m_fbWidth, m_fbHeight);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void Renderer::endFrame() {
    flush();
    glDisable(GL_SCISSOR_TEST);
    if (m_msaaFbo) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_msaaFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_presentFbo);
        glBlitFramebuffer(0, 0, m_fbWidth, m_fbHeight, 0, 0, m_fbWidth, m_fbHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        const GLenum discard[2] = {GL_COLOR_ATTACHMENT0, GL_DEPTH_STENCIL_ATTACHMENT};
        glBindFramebuffer(GL_FRAMEBUFFER, m_msaaFbo);
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, discard);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_presentFbo);
}

void Renderer::bindTexture(unsigned texture) {
    if (texture != m_boundTexture) {
        flush();
        m_boundTexture = texture;
    }
}

void Renderer::flush() {
    if (m_vertices.empty())
        return;
    glUseProgram(m_program2D);
    glUniform2f(m_loc2DViewport, kWidth, kHeight);
    glUniform1f(m_loc2DTime, m_time);
    glUniform1i(m_loc2DTex, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_boundTexture);
    glBindVertexArray(m_vao2D);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo2D);
    // Orphan the store first: a frame flushes many times (texture switches),
    // and rewriting a buffer the GPU may still read would stall on it.
    glBufferData(GL_ARRAY_BUFFER, kMaxQuads * 4 * sizeof(Vertex), nullptr, GL_DYNAMIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, m_vertices.size() * sizeof(Vertex), m_vertices.data());
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(m_vertices.size() / 4 * 6), GL_UNSIGNED_SHORT, nullptr);
    glBindVertexArray(0);
    m_vertices.clear();
}

void Renderer::pushQuad(const float pos[8], const float uv[8], const Color colors[4], const float local[8],
                        const ShapeParams& shape) {
    if (m_vertices.size() + 4 > kMaxQuads * 4)
        flush();
    for (int i = 0; i < 4; ++i) {
        m_vertices.push_back(Vertex{pos[i * 2], pos[i * 2 + 1], uv[i * 2], uv[i * 2 + 1], colors[i].r, colors[i].g,
                                    colors[i].b, colors[i].a, local[i * 2], local[i * 2 + 1], shape.hw, shape.hh,
                                    shape.radius, shape.thickness, static_cast<float>(shape.kind), shape.softness});
    }
}

void Renderer::boxQuad(float x, float y, float w, float h, Color top, Color bottom, float radius, int kind,
                       float thickness, float softness, float u0, float v0, float u1, float v1) {
    if (w <= 0.0f || h <= 0.0f)
        return;
    const float hw = w * 0.5f;
    const float hh = h * 0.5f;
    // Shapes get a margin so anti-aliasing and soft falloff are not clipped
    // by the quad itself; textured quads map their UVs to the exact box.
    const float m = kind == kImage ? 0.0f : softness + 2.0f / m_scale;
    const float pos[8] = {x - m, y - m, x + w + m, y - m, x + w + m, y + h + m, x - m, y + h + m};
    const float uv[8] = {u0, v0, u1, v0, u1, v1, u0, v1};
    const float local[8] = {-hw - m, -hh - m, hw + m, -hh - m, hw + m, hh + m, -hw - m, hh + m};
    const Color colors[4] = {top, top, bottom, bottom};
    const ShapeParams shape{hw, hh, std::min(radius, std::min(hw, hh)), thickness, softness, kind};
    pushQuad(pos, uv, colors, local, shape);
}

void Renderer::rect(float x, float y, float w, float h, Color c) { boxQuad(x, y, w, h, c, c, 0.0f, kShape, 0.0f, 0.0f); }

void Renderer::roundRect(float x, float y, float w, float h, float radius, Color c) {
    boxQuad(x, y, w, h, c, c, radius, kShape, 0.0f, 0.0f);
}

void Renderer::roundRectGradient(float x, float y, float w, float h, float radius, Color top, Color bottom) {
    boxQuad(x, y, w, h, top, bottom, radius, kShape, 0.0f, 0.0f);
}

void Renderer::roundRectOutline(float x, float y, float w, float h, float radius, float thickness, Color c) {
    boxQuad(x, y, w, h, c, c, radius, kOutline, thickness, 0.0f);
}

void Renderer::softRect(float x, float y, float w, float h, float radius, float blur, Color c) {
    boxQuad(x, y, w, h, c, c, radius, kShape, 0.0f, std::max(blur, 0.01f));
}

void Renderer::circle(float cx, float cy, float radius, Color c) {
    boxQuad(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, c, c, radius, kShape, 0.0f, 0.0f);
}

void Renderer::ring(float cx, float cy, float radius, float thickness, Color c) {
    boxQuad(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, c, c, radius, kOutline, thickness, 0.0f);
}

void Renderer::rotatedRoundRect(float cx, float cy, float w, float h, float radius, float angle, Color c) {
    const float m = 2.0f / m_scale;
    const float hw = w * 0.5f + m;
    const float hh = h * 0.5f + m;
    const float cs = std::cos(angle);
    const float sn = std::sin(angle);
    const float local[8] = {-hw, -hh, hw, -hh, hw, hh, -hw, hh};
    float pos[8];
    for (int i = 0; i < 4; ++i) {
        pos[i * 2] = cx + local[i * 2] * cs - local[i * 2 + 1] * sn;
        pos[i * 2 + 1] = cy + local[i * 2] * sn + local[i * 2 + 1] * cs;
    }
    const float uv[8] = {};
    const Color colors[4] = {c, c, c, c};
    const ShapeParams shape{w * 0.5f, h * 0.5f, std::min(radius, std::min(w, h) * 0.5f), 0.0f, 0.0f, kShape};
    pushQuad(pos, uv, colors, local, shape);
}

void Renderer::quad(const float xy[8], Color c) {
    const float uv[8] = {};
    const float local[8] = {};
    const Color colors[4] = {c, c, c, c};
    pushQuad(xy, uv, colors, local, ShapeParams{kHuge, kHuge, 0.0f, 0.0f, 0.0f, kShape});
}

void Renderer::triangle(float x0, float y0, float x1, float y1, float x2, float y2, Color c) {
    const float xy[8] = {x0, y0, x1, y1, x2, y2, x2, y2};
    quad(xy, c);
}

void Renderer::image(const Texture& texture, float x, float y, float w, float h, Color tint, float radius, float u0,
                     float v0, float u1, float v1, float blur) {
    if (!texture)
        return;
    bindTexture(texture.id);
    boxQuad(x, y, w, h, tint, tint, radius, kImage, blur, 0.0f, u0, v0, u1, v1);
}

void Renderer::imageCover(const Texture& texture, float x, float y, float w, float h, float focusX, float focusY,
                          Color tint, float radius, float blur) {
    if (!texture || w <= 0.0f || h <= 0.0f)
        return;
    const float textureAspect = static_cast<float>(texture.width) / static_cast<float>(texture.height);
    const float boxAspect = w / h;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (textureAspect > boxAspect) {
        const float keep = boxAspect / textureAspect;
        u0 = std::clamp(focusX - keep * 0.5f, 0.0f, 1.0f - keep);
        u1 = u0 + keep;
    } else {
        const float keep = textureAspect / boxAspect;
        v0 = std::clamp(focusY - keep * 0.5f, 0.0f, 1.0f - keep);
        v1 = v0 + keep;
    }
    image(texture, x, y, w, h, tint, radius, u0, v0, u1, v1, blur);
}

void Renderer::gradientRect(float x, float y, float w, float h, Color topLeft, Color topRight, Color bottomRight,
                            Color bottomLeft) {
    const float pos[8] = {x, y, x + w, y, x + w, y + h, x, y + h};
    const float uv[8] = {};
    const float local[8] = {};
    const Color colors[4] = {topLeft, topRight, bottomRight, bottomLeft};
    pushQuad(pos, uv, colors, local, ShapeParams{kHuge, kHuge, 0.0f, 0.0f, 0.0f, kShape});
}

void Renderer::background(Color top, Color bottom, float pattern) {
    const float pos[8] = {0, 0, kWidth, 0, kWidth, kHeight, 0, kHeight};
    const float uv[8] = {0, 0, 1, 0, 1, 1, 0, 1};
    const float local[8] = {};
    const Color colors[4] = {top, top, bottom, bottom};
    pushQuad(pos, uv, colors, local, ShapeParams{kHuge, kHuge, 0.0f, pattern, 0.0f, kBackground});
}

float Renderer::lineHeight(FontWeight weight, float size) {
    const int px = std::max(1, static_cast<int>(std::lround(size * m_scale)));
    return m_fonts.metrics(weight, px).lineHeight / m_scale;
}

float Renderer::measure(FontWeight weight, float size, std::string_view s, float tracking) {
    const int px = std::max(1, static_cast<int>(std::lround(size * m_scale)));
    float width = 0.0f;
    uint32_t previous = 0;
    size_t count = 0;
    for (size_t i = 0; i < s.size();) {
        const uint32_t cp = decodeUtf8(s.data(), s.size(), i);
        if (previous)
            width += m_fonts.kerning(weight, px, previous, cp);
        width += m_fonts.glyph(weight, px, cp).advance;
        previous = cp;
        ++count;
    }
    return width / m_scale + (count > 1 ? tracking * static_cast<float>(count - 1) : 0.0f);
}

float Renderer::text(FontWeight weight, float size, float x, float y, std::string_view s, Color c, Align align,
                     float tracking) {
    const float width = measure(weight, size, s, tracking);
    if (align == Align::Center)
        x -= width * 0.5f;
    else if (align == Align::Right)
        x -= width;

    const int px = std::max(1, static_cast<int>(std::lround(size * m_scale)));
    const FontMetrics metrics = m_fonts.metrics(weight, px);
    const float baseline = std::round(y * m_scale + metrics.ascender);
    float pen = x * m_scale;
    uint32_t previous = 0;
    for (size_t i = 0; i < s.size();) {
        const uint32_t cp = decodeUtf8(s.data(), s.size(), i);
        if (previous)
            pen += m_fonts.kerning(weight, px, previous, cp);
        // glyph() may upload into (or reset) the atlas: bind afterwards.
        const Glyph& g = m_fonts.glyph(weight, px, cp);
        if (g.hasBitmap) {
            bindTexture(m_fonts.texture());
            const float gx = std::round(pen + g.bearingX) / m_scale;
            const float gy = (baseline - g.bearingY) / m_scale;
            const float gw = g.width / m_scale;
            const float gh = g.height / m_scale;
            const float pos[8] = {gx, gy, gx + gw, gy, gx + gw, gy + gh, gx, gy + gh};
            const float uv[8] = {g.u0, g.v0, g.u1, g.v0, g.u1, g.v1, g.u0, g.v1};
            const float local[8] = {};
            const Color colors[4] = {c, c, c, c};
            pushQuad(pos, uv, colors, local, ShapeParams{kHuge, kHuge, 0.0f, 0.0f, 0.0f, kText});
        }
        pen += g.advance + tracking * m_scale;
        previous = cp;
    }
    return width;
}

float Renderer::textMiddle(FontWeight weight, float size, float x, float centerY, std::string_view s, Color c,
                           Align align, float tracking) {
    const int px = std::max(1, static_cast<int>(std::lround(size * m_scale)));
    const float ascender = m_fonts.metrics(weight, px).ascender / m_scale;
    const float capHeight = m_fonts.glyph(weight, px, 'H').bearingY / m_scale;
    return text(weight, size, x, centerY + capHeight * 0.5f - ascender, s, c, align, tracking);
}

std::vector<std::string> Renderer::wrap(FontWeight weight, float size, std::string_view s, float maxWidth) {
    std::vector<std::string> lines;
    std::string line;
    std::string word;
    auto flushWord = [&] {
        if (word.empty())
            return;
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && measure(weight, size, candidate) > maxWidth) {
            lines.push_back(line);
            line = word;
        } else {
            line = candidate;
        }
        word.clear();
    };
    for (char ch : s) {
        if (ch == ' ') {
            flushWord();
        } else if (ch == '\n') {
            flushWord();
            lines.push_back(line);
            line.clear();
        } else {
            word.push_back(ch);
        }
    }
    flushWord();
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

std::string Renderer::ellipsize(FontWeight weight, float size, std::string_view s, float maxWidth) {
    if (measure(weight, size, s) <= maxWidth)
        return std::string(s);
    static constexpr const char* kEllipsis = "\xE2\x80\xA6";
    std::string out(s);
    while (!out.empty()) {
        // Drop one UTF-8 code point from the end.
        size_t cut = out.size() - 1;
        while (cut > 0 && (static_cast<uint8_t>(out[cut]) & 0xC0) == 0x80)
            --cut;
        out.erase(cut);
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        if (measure(weight, size, out + kEllipsis) <= maxWidth)
            break;
    }
    return out + kEllipsis;
}

void Renderer::pushClip(float x, float y, float w, float h) {
    flush();
    if (!m_clips.empty()) {
        const ClipRect& outer = m_clips.back();
        const float x1 = std::max(x, outer.x);
        const float y1 = std::max(y, outer.y);
        const float x2 = std::min(x + w, outer.x + outer.w);
        const float y2 = std::min(y + h, outer.y + outer.h);
        x = x1;
        y = y1;
        w = std::max(0.0f, x2 - x1);
        h = std::max(0.0f, y2 - y1);
    }
    m_clips.push_back({x, y, w, h});
    applyClip();
}

void Renderer::popClip() {
    flush();
    if (!m_clips.empty())
        m_clips.pop_back();
    applyClip();
}

void Renderer::applyClip() {
    if (m_clips.empty()) {
        glDisable(GL_SCISSOR_TEST);
        return;
    }
    const ClipRect& c = m_clips.back();
    glEnable(GL_SCISSOR_TEST);
    glScissor(static_cast<GLint>(std::floor(c.x * m_scale)),
              static_cast<GLint>(std::floor(m_fbHeight - (c.y + c.h) * m_scale)),
              static_cast<GLsizei>(std::ceil(c.w * m_scale)), static_cast<GLsizei>(std::ceil(c.h * m_scale)));
}

void Renderer::begin3D(float x, float y, float w, float h) {
    flush();
    const GLint vx = static_cast<GLint>(std::lround(x * m_scale));
    const GLint vy = static_cast<GLint>(std::lround(m_fbHeight - (y + h) * m_scale));
    const GLsizei vw = static_cast<GLsizei>(std::lround(w * m_scale));
    const GLsizei vh = static_cast<GLsizei>(std::lround(h * m_scale));
    glViewport(vx, vy, vw, vh);
    glEnable(GL_SCISSOR_TEST);
    glScissor(vx, vy, vw, vh);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glUseProgram(m_program3D);
    glBindVertexArray(m_vao3D);
}

void Renderer::cube(const Mat4& viewProjection, const Mat4& model, Vec3 eye, Color color, float emissive,
                    float gloss) {
    const Mat4 mvp = viewProjection * model;
    glUniformMatrix4fv(m_loc3DMVP, 1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(m_loc3DModel, 1, GL_FALSE, model.m);
    glUniform4f(m_loc3DColor, color.r, color.g, color.b, color.a);
    glUniform1f(m_loc3DEmissive, emissive);
    glUniform1f(m_loc3DGloss, gloss);
    glUniform3f(m_loc3DEye, eye.x, eye.y, eye.z);
    glDrawArrays(GL_TRIANGLES, 0, 36);
}

void Renderer::end3D() {
    glBindVertexArray(0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glViewport(0, 0, m_fbWidth, m_fbHeight);
    applyClip();
}

Texture Renderer::createTexture(int width, int height, const uint8_t* rgba, bool mipmaps) {
    Texture texture;
    texture.width = width;
    texture.height = height;
    glGenTextures(1, &texture.id);
    glBindTexture(GL_TEXTURE_2D, texture.id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    if (mipmaps)
        glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

void Renderer::destroyTexture(Texture& texture) {
    if (texture.id) {
        if (m_boundTexture == texture.id) {
            flush();
            m_boundTexture = m_whiteTexture;
        }
        glDeleteTextures(1, &texture.id);
    }
    texture = {};
}

} // namespace partyboard::launcher
