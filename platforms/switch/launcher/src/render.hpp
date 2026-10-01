#pragma once

// Batched 2D renderer (signed-distance rounded shapes, textured quads, text)
// plus a lit cube pass for the 3D logo, all drawn into a multisampled
// offscreen target and resolved once per frame.
//
// 2D coordinates are a virtual 1280x720 canvas, top-left origin; the
// framebuffer may be 720p (handheld) or 1080p (docked).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "font.hpp"
#include "math.hpp"

namespace partyboard::launcher {

struct Color {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

constexpr Color rgb(uint32_t hex, float alpha = 1.0f) {
    return {static_cast<float>((hex >> 16) & 0xFF) / 255.0f, static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
            static_cast<float>(hex & 0xFF) / 255.0f, alpha};
}

inline Color withAlpha(Color c, float alpha) { return {c.r, c.g, c.b, c.a * alpha}; }

inline Color mixColor(Color a, Color b, float t) {
    return {lerp(a.r, b.r, t), lerp(a.g, b.g, t), lerp(a.b, b.b, t), lerp(a.a, b.a, t)};
}

struct Texture {
    unsigned id = 0;
    int width = 0;
    int height = 0;

    explicit operator bool() const { return id != 0; }
};

enum class Align : unsigned char { Left, Center, Right };

class Renderer {
public:
    static constexpr float kWidth = 1280.0f;
    static constexpr float kHeight = 720.0f;

    bool init(const char* shaderHeader, const FontBlob& regular, const FontBlob& bold, const FontBlob& display);
    void shutdown();

    void beginFrame(int framebufferWidth, int framebufferHeight, unsigned presentFramebuffer, float time);
    void endFrame();

    // Physical pixels per virtual pixel.
    float pixelScale() const { return m_scale; }

    // ---- 2D shapes -------------------------------------------------------
    void rect(float x, float y, float w, float h, Color c);
    void roundRect(float x, float y, float w, float h, float radius, Color c);
    void roundRectGradient(float x, float y, float w, float h, float radius, Color top, Color bottom);
    void roundRectOutline(float x, float y, float w, float h, float radius, float thickness, Color c);
    // Soft-edged shape for drop shadows and glows; `blur` is the falloff width.
    void softRect(float x, float y, float w, float h, float radius, float blur, Color c);
    void circle(float cx, float cy, float radius, Color c);
    void ring(float cx, float cy, float radius, float thickness, Color c);
    void rotatedRoundRect(float cx, float cy, float w, float h, float radius, float angle, Color c);
    // Arbitrary convex quad (clockwise), e.g. the faces of a 2D isometric cube.
    void quad(const float xy[8], Color c);
    void triangle(float x0, float y0, float x1, float y1, float x2, float y2, Color c);
    // `blur` biases the mip level: 0 is sharp, ~3 is a soft backdrop
    // (the texture needs mipmaps).
    void image(const Texture& texture, float x, float y, float w, float h, Color tint = {}, float radius = 0.0f,
               float u0 = 0.0f, float v0 = 0.0f, float u1 = 1.0f, float v1 = 1.0f, float blur = 0.0f);
    // Draws `texture` so it covers the box, cropping the overflow around
    // (focusX, focusY) in texture space [0, 1].
    void imageCover(const Texture& texture, float x, float y, float w, float h, float focusX, float focusY,
                    Color tint = {}, float radius = 0.0f, float blur = 0.0f);
    // Axis-aligned rectangle with one colour per corner (clockwise from top-left).
    void gradientRect(float x, float y, float w, float h, Color topLeft, Color topRight, Color bottomRight,
                      Color bottomLeft);
    // Full-screen gradient with the drifting cube lattice of the shelf.
    void background(Color top, Color bottom, float pattern);

    // ---- Text --------------------------------------------------------------
    // `y` is the top of the line box. Returns the advance width.
    float text(FontWeight weight, float size, float x, float y, std::string_view s, Color c,
               Align align = Align::Left, float tracking = 0.0f);
    // Same as text(), but centres capital letters on `centerY` whatever the
    // face's line gap (Rodin and N64 Party have very different metrics).
    float textMiddle(FontWeight weight, float size, float x, float centerY, std::string_view s, Color c,
                     Align align = Align::Left, float tracking = 0.0f);
    float measure(FontWeight weight, float size, std::string_view s, float tracking = 0.0f);
    float lineHeight(FontWeight weight, float size);
    std::vector<std::string> wrap(FontWeight weight, float size, std::string_view s, float maxWidth);
    // Shortens `s` with an ellipsis so it fits in maxWidth.
    std::string ellipsize(FontWeight weight, float size, std::string_view s, float maxWidth);

    // ---- Clipping (virtual coordinates, nests by intersection) -------------
    void pushClip(float x, float y, float w, float h);
    void popClip();

    // ---- 3D ----------------------------------------------------------------
    // Opens a 3D pass in a virtual-space viewport; depth is cleared there.
    void begin3D(float x, float y, float w, float h);
    void cube(const Mat4& viewProjection, const Mat4& model, Vec3 eye, Color color, float emissive,
              float gloss = 1.0f);
    void end3D();

    Texture createTexture(int width, int height, const uint8_t* rgba, bool mipmaps);
    void destroyTexture(Texture& texture);

private:
    struct Vertex {
        float x, y;
        float u, v;
        float r, g, b, a;
        float lx, ly;
        float hw, hh;
        float radius, thickness, kind, softness;
    };

    enum Kind : int { kShape = 0, kImage = 1, kText = 2, kOutline = 3, kBackground = 4 };

    struct ShapeParams {
        float hw, hh, radius, thickness, softness;
        int kind;
    };

    void pushQuad(const float pos[8], const float uv[8], const Color colors[4], const float local[8],
                  const ShapeParams& shape);
    void boxQuad(float x, float y, float w, float h, Color top, Color bottom, float radius, int kind,
                 float thickness, float softness, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1);
    void bindTexture(unsigned texture);
    void flush();
    void applyClip();
    bool createTargets(int width, int height);
    void destroyTargets();

    FontCache m_fonts;
    unsigned m_program2D = 0;
    unsigned m_program3D = 0;
    unsigned m_vao2D = 0, m_vbo2D = 0, m_ibo2D = 0;
    unsigned m_vao3D = 0, m_vbo3D = 0;
    unsigned m_whiteTexture = 0;
    unsigned m_boundTexture = 0;

    int m_loc2DViewport = -1, m_loc2DTime = -1, m_loc2DTex = -1;
    int m_loc3DMVP = -1, m_loc3DModel = -1, m_loc3DColor = -1, m_loc3DEmissive = -1, m_loc3DEye = -1,
        m_loc3DGloss = -1;

    unsigned m_msaaFbo = 0, m_msaaColor = 0, m_msaaDepth = 0;
    int m_targetWidth = 0, m_targetHeight = 0;
    bool m_msaaUnavailable = false;
    unsigned m_presentFbo = 0;

    int m_fbWidth = 1280, m_fbHeight = 720;
    float m_scale = 1.0f;
    float m_time = 0.0f;

    struct ClipRect {
        float x, y, w, h;
    };
    std::vector<ClipRect> m_clips;
    std::vector<Vertex> m_vertices;
};

} // namespace partyboard::launcher
