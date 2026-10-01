#pragma once

// FreeType glyph cache packed into one GL_R8 atlas. On the console the faces
// come from the system's shared font (pl service), so the launcher reads like
// the rest of the Switch UI without shipping a font file.

#include <cstdint>
#include <functional>
#include <unordered_map>

#include "platform.hpp"

namespace partyboard::launcher {

enum class FontWeight : unsigned char { Regular, Bold };

struct Glyph {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    float width = 0, height = 0;     // bitmap size, physical pixels
    float bearingX = 0, bearingY = 0; // physical pixels
    float advance = 0;                // physical pixels
    bool hasBitmap = false;
};

struct FontMetrics {
    float ascender = 0;
    float descender = 0;
    float lineHeight = 0;
};

class FontCache {
public:
    static constexpr int kAtlasSize = 2048;

    FontCache() = default;
    ~FontCache();
    FontCache(const FontCache&) = delete;
    FontCache& operator=(const FontCache&) = delete;

    // `beforeReset` runs before the atlas is wiped when it fills up, so the
    // renderer can flush quads that still point at the old glyphs.
    bool init(const FontBlob& regular, const FontBlob& bold, std::function<void()> beforeReset);
    void shutdown();

    const Glyph& glyph(FontWeight weight, int pixelSize, uint32_t codepoint);
    FontMetrics metrics(FontWeight weight, int pixelSize);
    float kerning(FontWeight weight, int pixelSize, uint32_t left, uint32_t right);

    unsigned texture() const { return m_texture; }

private:
    struct Face;

    Face* face(FontWeight weight);
    bool setSize(Face& face, int pixelSize);
    void reset();

    void* m_library = nullptr; // FT_Library, kept opaque to spare includes
    Face* m_faces[2] = {};
    unsigned m_texture = 0;
    int m_penX = 1;
    int m_penY = 1;
    int m_rowHeight = 0;
    std::unordered_map<uint64_t, Glyph> m_glyphs;
    std::function<void()> m_beforeReset;
};

// Decodes one UTF-8 sequence and advances `i`. Invalid bytes yield U+FFFD.
uint32_t decodeUtf8(const char* text, size_t length, size_t& i);

} // namespace partyboard::launcher
