#include "font.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H

#include "gl.hpp"

namespace partyboard::launcher {

struct FontCache::Face {
    FT_Face face = nullptr;
    bool synthesizeBold = false;
    int pixelSize = 0;
};

FontCache::~FontCache() { shutdown(); }

bool FontCache::init(const FontBlob& regular, const FontBlob& bold, const FontBlob& display,
                     std::function<void()> beforeReset) {
    m_beforeReset = std::move(beforeReset);

    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) != 0) {
        std::printf("launcher: FT_Init_FreeType failed\n");
        return false;
    }
    m_library = library;
    if (!regular.data)
        return false;

    // Missing faces fall back: Bold to an emboldened Regular, Display to Bold.
    const FontBlob* blobs[kFaces] = {&regular, bold.data ? &bold : &regular,
                                     display.data ? &display : (bold.data ? &bold : &regular)};
    const bool synthesize[kFaces] = {regular.synthesizeBold, bold.data ? bold.synthesizeBold : true,
                                     display.data ? display.synthesizeBold : !bold.data};
    for (int i = 0; i < kFaces; ++i) {
        auto* face = new Face();
        if (FT_New_Memory_Face(library, blobs[i]->data, static_cast<FT_Long>(blobs[i]->size), 0, &face->face) != 0) {
            std::printf("launcher: FT_New_Memory_Face failed for face %d\n", i);
            delete face;
            return false;
        }
        face->synthesizeBold = synthesize[i];
        m_faces[i] = face;
    }

    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    std::vector<uint8_t> zero(size_t(kAtlasSize) * kAtlasSize, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kAtlasSize, kAtlasSize, 0, GL_RED, GL_UNSIGNED_BYTE, zero.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}

void FontCache::shutdown() {
    for (Face*& face : m_faces) {
        if (face) {
            FT_Done_Face(face->face);
            delete face;
            face = nullptr;
        }
    }
    if (m_library) {
        FT_Done_FreeType(static_cast<FT_Library>(m_library));
        m_library = nullptr;
    }
    if (m_texture) {
        glDeleteTextures(1, &m_texture);
        m_texture = 0;
    }
    m_glyphs.clear();
}

FontCache::Face* FontCache::face(FontWeight weight) { return m_faces[static_cast<int>(weight)]; }

FontCache::Face* FontCache::faceFor(FontWeight weight, uint32_t codepoint, unsigned& index) {
    Face* preferred = face(weight);
    index = preferred ? FT_Get_Char_Index(preferred->face, codepoint) : 0;
    if (index != 0 || !preferred)
        return preferred;
    // e.g. a symbol the display face lacks: borrow it from Bold, then Regular.
    for (int i = kFaces - 2; i >= 0; --i) {
        if (m_faces[i] && m_faces[i] != preferred) {
            if (const unsigned other = FT_Get_Char_Index(m_faces[i]->face, codepoint)) {
                index = other;
                return m_faces[i];
            }
        }
    }
    return preferred;
}

bool FontCache::setSize(Face& face, int pixelSize) {
    if (face.pixelSize == pixelSize)
        return true;
    if (FT_Set_Pixel_Sizes(face.face, 0, static_cast<FT_UInt>(pixelSize)) != 0)
        return false;
    face.pixelSize = pixelSize;
    return true;
}

void FontCache::reset() {
    if (m_beforeReset)
        m_beforeReset();
    std::vector<uint8_t> zero(size_t(kAtlasSize) * kAtlasSize, 0);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kAtlasSize, kAtlasSize, GL_RED, GL_UNSIGNED_BYTE, zero.data());
    m_glyphs.clear();
    m_penX = 1;
    m_penY = 1;
    m_rowHeight = 0;
}

const Glyph& FontCache::glyph(FontWeight weight, int pixelSize, uint32_t codepoint) {
    const uint64_t key = (uint64_t(weight) << 56) | (uint64_t(pixelSize) << 32) | codepoint;
    if (auto it = m_glyphs.find(key); it != m_glyphs.end())
        return it->second;

    Glyph result;
    unsigned index = 0;
    Face* f = faceFor(weight, codepoint, index);
    if (f && setSize(*f, pixelSize)) {
        if (FT_Load_Glyph(f->face, index, FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP) == 0) {
            FT_GlyphSlot slot = f->face->glyph;
            if (f->synthesizeBold)
                FT_GlyphSlot_Embolden(slot);
            if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) == 0) {
                const FT_Bitmap& bitmap = slot->bitmap;
                result.advance = static_cast<float>(slot->advance.x) / 64.0f;
                result.bearingX = static_cast<float>(slot->bitmap_left);
                result.bearingY = static_cast<float>(slot->bitmap_top);
                result.width = static_cast<float>(bitmap.width);
                result.height = static_cast<float>(bitmap.rows);

                const int w = static_cast<int>(bitmap.width);
                const int h = static_cast<int>(bitmap.rows);
                if (w > 0 && h > 0 && w + 2 < kAtlasSize && h + 2 < kAtlasSize) {
                    if (m_penX + w + 1 >= kAtlasSize) {
                        m_penX = 1;
                        m_penY += m_rowHeight + 1;
                        m_rowHeight = 0;
                    }
                    if (m_penY + h + 1 >= kAtlasSize) {
                        reset();
                        // The slot still holds this glyph's bitmap.
                    }
                    std::vector<uint8_t> pixels(size_t(w) * h);
                    for (int row = 0; row < h; ++row) {
                        const uint8_t* src = bitmap.buffer + (bitmap.pitch >= 0 ? row * bitmap.pitch
                                                                                : (h - 1 - row) * -bitmap.pitch);
                        std::copy(src, src + w, pixels.begin() + size_t(row) * w);
                    }
                    glBindTexture(GL_TEXTURE_2D, m_texture);
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, m_penX, m_penY, w, h, GL_RED, GL_UNSIGNED_BYTE, pixels.data());
                    const float inv = 1.0f / kAtlasSize;
                    result.u0 = m_penX * inv;
                    result.v0 = m_penY * inv;
                    result.u1 = (m_penX + w) * inv;
                    result.v1 = (m_penY + h) * inv;
                    result.hasBitmap = true;
                    m_penX += w + 1;
                    m_rowHeight = std::max(m_rowHeight, h);
                }
            }
        }
    }
    return m_glyphs.emplace(key, result).first->second;
}

FontMetrics FontCache::metrics(FontWeight weight, int pixelSize) {
    FontMetrics m;
    Face* f = face(weight);
    if (!f || !setSize(*f, pixelSize))
        return m;
    const FT_Size_Metrics& sm = f->face->size->metrics;
    m.ascender = static_cast<float>(sm.ascender) / 64.0f;
    m.descender = static_cast<float>(sm.descender) / 64.0f;
    m.lineHeight = static_cast<float>(sm.height) / 64.0f;
    return m;
}

float FontCache::kerning(FontWeight weight, int pixelSize, uint32_t left, uint32_t right) {
    Face* f = face(weight);
    if (!f || !FT_HAS_KERNING(f->face) || !setSize(*f, pixelSize))
        return 0.0f;
    FT_Vector delta{};
    const FT_UInt a = FT_Get_Char_Index(f->face, left);
    const FT_UInt b = FT_Get_Char_Index(f->face, right);
    if (a == 0 || b == 0)
        return 0.0f;
    if (FT_Get_Kerning(f->face, a, b, FT_KERNING_DEFAULT, &delta) != 0)
        return 0.0f;
    return static_cast<float>(delta.x) / 64.0f;
}

uint32_t decodeUtf8(const char* text, size_t length, size_t& i) {
    const auto c = static_cast<uint8_t>(text[i]);
    auto continuation = [&](size_t count, uint32_t initial) -> uint32_t {
        if (i + count >= length) {
            ++i;
            return 0xFFFD;
        }
        uint32_t cp = initial;
        for (size_t k = 1; k <= count; ++k) {
            const auto next = static_cast<uint8_t>(text[i + k]);
            if ((next & 0xC0) != 0x80) {
                ++i;
                return 0xFFFD;
            }
            cp = (cp << 6) | (next & 0x3F);
        }
        i += count + 1;
        return cp;
    };
    if (c < 0x80) {
        ++i;
        return c;
    }
    if ((c & 0xE0) == 0xC0)
        return continuation(1, c & 0x1F);
    if ((c & 0xF0) == 0xE0)
        return continuation(2, c & 0x0F);
    if ((c & 0xF8) == 0xF0)
        return continuation(3, c & 0x07);
    ++i;
    return 0xFFFD;
}

} // namespace partyboard::launcher
