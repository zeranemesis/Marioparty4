// Bottom-screen companion for dual-screen Android handhelds (AYN Thor and the like).
// DualScreen.java shows a live board map and a few settings on the second display;
// this side publishes the board each frame and applies the settings it changes.
// Both run on the game thread (PartyBoard_DualScreenFrame); Java only copies under the lock.
#include "port/dual_screen.h"

#include "port/config.hpp"
#include "port/display_rate.hpp"
#include "port/game_activity.hpp"
#include "port/settings.h"

#include <gx/GXAurora.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "game/board/model.h"
#include "game/board/player.h"
#include "game/board/space.h"
#include "game/animdata.h"
#include "game/gamework_data.h"

// Game code, PC only: images the board already holds (src/game/board/space.c, ui.c).
void *BoardSpaceIconAtlasGet(s32 *size, s32 *format);
ANIMDATA *BoardStatusSpriteDataGet(s32 player, s32 member);
}

namespace {

// Layout shared with DualScreen.java (BoardMapView): keep both in step.
constexpr int kVersion = 1;
constexpr int kHeader = 7;      // version, onBoard, turn, maxTurn, playerCurr, starSpace, spaceCount
constexpr int kPlayerStride = 6; // x, z, character, coins, stars, com
constexpr int kSpaceStride = 3 + BOARD_SPACE_LINKMAX; // x, z, type, links (1-based, 0 = none)
constexpr int kMaxSpaces = 256;
constexpr int kMaxFloats = kHeader + 4 * kPlayerStride + kMaxSpaces * kSpaceStride;

std::mutex gLock;
std::array<float, kMaxFloats> gSnapshot {};
int gSnapshotSize = 0;
std::vector<std::pair<std::string, int>> gPending;

// The map draws the game's own space icons and portraits, decoded from what the board
// loaded from the player's disc. Image 0 is the 4x4 space atlas, 1 + character a portrait,
// 9 the coin icon and 10 the star icon of the status bars.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> argb;
};
constexpr int kImageCount = 11;
std::array<Image, kImageCount> gImages;
int gImageGeneration = 0;
const void *gAtlasSource = nullptr;
std::array<const void *, kImageCount> gSpriteSource {};

uint32_t argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b)
{
    return a << 24 | r << 16 | g << 8 | b;
}

uint16_t be16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] << 8 | p[1]);
}

uint32_t rgb5a3(uint16_t v)
{
    if (v & 0x8000) {
        const uint32_t r = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F, b = v & 0x1F;
        return argb(255, r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2);
    }
    const uint32_t a = (v >> 12) & 7, r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
    return argb(a << 5 | a << 2 | a >> 1, r << 4 | r, g << 4 | g, b << 4 | b);
}

uint32_t rgb565(uint16_t v)
{
    const uint32_t r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
    return argb(255, r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2);
}

// GameCube textures are stored in tiles (blockW x blockH texels, big-endian).
// Returns an empty image for a format the map does not need.
Image decode_gx(int format, int width, int height, const uint8_t *data, const uint8_t *palette)
{
    Image image;
    int blockW = 4, blockH = 4, bits = 16;
    switch (format) {
    case ANIM_BMP_RGBA8: bits = 32; break;
    case ANIM_BMP_RGB5A3:
    case ANIM_BMP_RGB5A3_DUPE:
    case ANIM_BMP_IA8: break;
    case ANIM_BMP_C8:
    case ANIM_BMP_IA4:
    case ANIM_BMP_I8:
    case ANIM_BMP_A8: blockW = 8; bits = 8; break;
    case ANIM_BMP_C4:
    case ANIM_BMP_I4:
    case ANIM_BMP_CMPR: blockW = 8; blockH = 8; bits = 4; break;
    default: return image;
    }
    if (width <= 0 || height <= 0 || width > 1024 || height > 1024 || data == nullptr
        || ((format == ANIM_BMP_C8 || format == ANIM_BMP_C4) && palette == nullptr)) {
        return image;
    }
    image.width = width;
    image.height = height;
    image.argb.assign(static_cast<size_t>(width) * height, 0);
    const auto put = [&](int x, int y, uint32_t c) {
        if (x < width && y < height) {
            image.argb[static_cast<size_t>(y) * width + x] = c;
        }
    };
    const uint8_t *p = data;
    for (int by = 0; by < height; by += blockH) {
        for (int bx = 0; bx < width; bx += blockW) {
            if (format == ANIM_BMP_CMPR) {
                // Four DXT1 sub-blocks of 4x4.
                for (int sub = 0; sub < 4; ++sub, p += 8) {
                    const uint16_t c0 = be16(p), c1 = be16(p + 2);
                    uint32_t colors[4] = {rgb565(c0), rgb565(c1), 0, 0};
                    const auto mix = [&](int wa, int wb, int d) {
                        uint32_t out = 0xFF000000u;
                        for (int shift = 0; shift < 24; shift += 8) {
                            out |= (((colors[0] >> shift & 0xFF) * wa + (colors[1] >> shift & 0xFF) * wb) / d) << shift;
                        }
                        return out;
                    };
                    if (c0 > c1) {
                        colors[2] = mix(2, 1, 3);
                        colors[3] = mix(1, 2, 3);
                    } else {
                        colors[2] = mix(1, 1, 2);
                        colors[3] = 0;
                    }
                    const int ox = bx + (sub & 1) * 4, oy = by + (sub >> 1) * 4;
                    for (int y = 0; y < 4; ++y) {
                        for (int x = 0; x < 4; ++x) {
                            put(ox + x, oy + y, colors[(p[4 + y] >> (6 - 2 * x)) & 3]);
                        }
                    }
                }
                continue;
            }
            if (format == ANIM_BMP_RGBA8) {
                // AR pairs for the 16 texels, then GB pairs.
                for (int i = 0; i < 16; ++i) {
                    put(bx + i % 4, by + i / 4, argb(p[2 * i], p[2 * i + 1], p[32 + 2 * i], p[32 + 2 * i + 1]));
                }
                p += 64;
                continue;
            }
            for (int y = 0; y < blockH; ++y) {
                for (int x = 0; x < blockW; ++x) {
                    const int i = y * blockW + x;
                    uint32_t c = 0;
                    if (bits == 16) {
                        const uint16_t v = be16(p + 2 * i);
                        c = format == ANIM_BMP_IA8 ? argb(v >> 8, v & 0xFF, v & 0xFF, v & 0xFF) : rgb5a3(v);
                    } else if (bits == 8) {
                        const uint8_t v = p[i];
                        switch (format) {
                        case ANIM_BMP_C8: c = rgb5a3(be16(palette + 2 * v)); break;
                        case ANIM_BMP_IA4: c = argb((v >> 4) * 17, (v & 0xF) * 17, (v & 0xF) * 17, (v & 0xF) * 17); break;
                        case ANIM_BMP_I8: c = argb(255, v, v, v); break;
                        default: c = argb(v, 255, 255, 255); break;
                        }
                    } else {
                        const uint8_t v = (p[i / 2] >> (i % 2 ? 0 : 4)) & 0xF;
                        c = format == ANIM_BMP_C4 ? rgb5a3(be16(palette + 2 * v)) : argb(255, v * 17, v * 17, v * 17);
                    }
                    put(bx + x, by + y, c);
                }
            }
            p += blockW * blockH * bits / 8;
        }
    }
    return image;
}

// The first frame of a sprite as the game shows it: its first layer's part of the bitmap.
Image decode_sprite(const ANIMDATA *anim)
{
    if (anim == nullptr || anim->bmpNum <= 0 || anim->bmp == nullptr) {
        return {};
    }
    const ANIMLAYER *layer = nullptr;
    if (anim->bankNum > 0 && anim->bank != nullptr && anim->bank[0].timeNum > 0 && anim->bank[0].frame != nullptr
        && anim->pat != nullptr) {
        const int pat = anim->bank[0].frame[0].pat;
        if (pat >= 0 && pat < anim->patNum && anim->pat[pat].layerNum > 0 && anim->pat[pat].layer != nullptr) {
            layer = &anim->pat[pat].layer[0];
        }
    }
    const int bmpNo = layer != nullptr && layer->bmpNo >= 0 && layer->bmpNo < anim->bmpNum ? layer->bmpNo : 0;
    const ANIMBMP &bmp = anim->bmp[bmpNo];
    Image full = decode_gx(bmp.dataFmt & ANIM_BMP_FMTMASK, bmp.sizeX, bmp.sizeY,
        static_cast<const uint8_t *>(bmp.data), static_cast<const uint8_t *>(bmp.palData));
    if (layer == nullptr || full.argb.empty() || layer->sizeX <= 0 || layer->sizeY <= 0
        || layer->startX < 0 || layer->startY < 0 || layer->startX + layer->sizeX > full.width
        || layer->startY + layer->sizeY > full.height) {
        return full;
    }
    Image part;
    part.width = layer->sizeX;
    part.height = layer->sizeY;
    part.argb.resize(static_cast<size_t>(part.width) * part.height);
    for (int y = 0; y < part.height; ++y) {
        std::copy_n(&full.argb[static_cast<size_t>(layer->startY + y) * full.width + layer->startX], part.width,
            &part.argb[static_cast<size_t>(y) * part.width]);
    }
    return part;
}

int atlas_anim_format(s32 gxFormat)
{
    switch (gxFormat) {
    case GX_TF_RGBA8: return ANIM_BMP_RGBA8;
    case GX_TF_RGB5A3: return ANIM_BMP_RGB5A3;
    case GX_TF_CMPR: return ANIM_BMP_CMPR;
    default: return -1;
    }
}

// Decodes once per board (the atlas) and once per character met (the portraits).
void publish_images()
{
    bool changed = false;
    s32 size = 0, format = 0;
    const void *atlas = BoardSpaceIconAtlasGet(&size, &format);
    if (atlas != nullptr && atlas != gAtlasSource) {
        Image image = decode_gx(atlas_anim_format(format), size, size, static_cast<const uint8_t *>(atlas), nullptr);
        std::lock_guard lock(gLock);
        gImages[0] = std::move(image);
        gAtlasSource = atlas;
        changed = true;
    }
    const auto take = [&](int id, const ANIMDATA *anim) {
        if (anim == nullptr || anim == gSpriteSource[id]) {
            return;
        }
        Image image = decode_sprite(anim);
        std::lock_guard lock(gLock);
        gImages[id] = std::move(image);
        gSpriteSource[id] = anim;
        changed = true;
    };
    for (int player = 0; player < 4; ++player) {
        const int character = GWPlayer[player].character;
        if (character >= 0 && character < 8) {
            take(1 + character, BoardStatusSpriteDataGet(player, 0xB));
        }
    }
    take(9, BoardStatusSpriteDataGet(0, 6));
    take(10, BoardStatusSpriteDataGet(0, 10));
    if (changed) {
        std::lock_guard lock(gLock);
        ++gImageGeneration;
    }
}

void publish_board()
{
    std::array<float, kMaxFloats> next {};
    next[0] = kVersion;
    const auto activity = partyboard::current_game_activity();
    const int spaceCount = std::min<int>(BoardSpaceCountGet(0), kMaxSpaces);
    const bool onBoard = activity.scene == partyboard::GameActivity::Scene::Board && spaceCount > 0;
    int size = kHeader;
    if (onBoard) {
        next[1] = 1;
        next[2] = GWSystem.turn;
        next[3] = GWSystem.max_turn;
        next[4] = GWSystem.player_curr;
        next[5] = BoardSpaceStarGetCurr();
        next[6] = spaceCount;
        for (int i = 0; i < 4; ++i) {
            // The model, to follow a walk between spaces. On the way out of the board
            // the models go back to the origin first: then the player's space.
            Vec pos {};
            if (BoardModelPosGet(BoardPlayerModelGet(i), &pos) != 0 || (pos.x == 0.0f && pos.z == 0.0f)) {
                BoardSpacePosGet(0, GWPlayer[i].space_curr, &pos);
            }
            float *p = &next[size];
            p[0] = pos.x;
            p[1] = pos.z;
            p[2] = GWPlayer[i].character;
            p[3] = GWPlayer[i].coins;
            p[4] = GWPlayer[i].stars;
            p[5] = GWPlayer[i].com;
            size += kPlayerStride;
        }
        for (int i = 1; i <= spaceCount; ++i) {
            const BoardSpace *space = BoardSpaceGet(0, i);
            float *s = &next[size];
            s[0] = space->pos.x;
            s[1] = space->pos.z;
            s[2] = space->type;
            for (int l = 0; l < BOARD_SPACE_LINKMAX; ++l) {
                s[3 + l] = l < space->link_cnt ? space->link[l] : 0;
            }
            size += kSpaceStride;
        }
    }
    std::lock_guard lock(gLock);
    gSnapshot = next;
    gSnapshotSize = size;
}

// Same side effects as the in-game settings menu (src/port/ui/settings.cpp).
void apply_setting(const std::string &key, int value)
{
    using namespace partyboard;
    auto &video = getSettings().video;
    if (key == "video.enableAdaptiveWidescreen") {
        video.enableAdaptiveWidescreen.setValue(value != 0);
        if (value != 0) {
            video.lockAspectRatio.setValue(false);
        }
        AuroraSetViewportPolicy(value != 0 || !video.lockAspectRatio.getValue() ? AURORA_VIEWPORT_STRETCH : AURORA_VIEWPORT_FIT);
    } else if (key == "video.lockAspectRatio") {
        video.lockAspectRatio.setValue(value != 0);
        if (value != 0) {
            video.enableAdaptiveWidescreen.setValue(false);
        }
        AuroraSetViewportPolicy(value != 0 ? AURORA_VIEWPORT_FIT : AURORA_VIEWPORT_STRETCH);
    } else if (auto *var = config::GetConfigVar(key)) {
        if (auto *b = dynamic_cast<config::ConfigVar<bool> *>(var)) {
            b->setValue(value != 0);
        } else if (auto *n = dynamic_cast<config::ConfigVar<int> *>(var)) {
            n->setValue(value);
        } else {
            return;
        }
    } else {
        return;
    }
    config::Save();
}

} // namespace

extern "C" void PartyBoard_DualScreenFrame(void)
{
    std::vector<std::pair<std::string, int>> pending;
    {
        std::lock_guard lock(gLock);
        pending.swap(gPending);
    }
    for (const auto &[key, value] : pending) {
        apply_setting(key, value);
    }
    publish_board();
    if (partyboard::current_game_activity().scene == partyboard::GameActivity::Scene::Board) {
        publish_images();
    }
}

#ifdef __ANDROID__
#include <jni.h>

extern "C" JNIEXPORT jint JNICALL Java_com_mariopartyrd_partyboard_DualScreen_nativeBoardSnapshot(JNIEnv *env, jclass, jfloatArray out)
{
    std::lock_guard lock(gLock);
    const jsize size = std::min<jsize>(gSnapshotSize, env->GetArrayLength(out));
    env->SetFloatArrayRegion(out, 0, size, gSnapshot.data());
    return size;
}

// Reads straight from the setting: a stale value for one frame is harmless here.
extern "C" JNIEXPORT jint JNICALL Java_com_mariopartyrd_partyboard_DualScreen_nativeGetSetting(JNIEnv *env, jclass, jstring jkey)
{
    const char *chars = env->GetStringUTFChars(jkey, nullptr);
    const std::string key = chars;
    env->ReleaseStringUTFChars(jkey, chars);
    {
        std::lock_guard lock(gLock);
        for (auto it = gPending.rbegin(); it != gPending.rend(); ++it) {
            if (it->first == key) {
                return it->second;
            }
        }
    }
    auto *var = partyboard::config::GetConfigVar(key);
    if (auto *b = dynamic_cast<partyboard::config::ConfigVar<bool> *>(var)) {
        return b->getValue() ? 1 : 0;
    }
    if (auto *n = dynamic_cast<partyboard::config::ConfigVar<int> *>(var)) {
        return n->getValue();
    }
    return -1;
}

extern "C" JNIEXPORT void JNICALL Java_com_mariopartyrd_partyboard_DualScreen_nativeSetSetting(JNIEnv *env, jclass, jstring jkey, jint value)
{
    const char *chars = env->GetStringUTFChars(jkey, nullptr);
    std::lock_guard lock(gLock);
    gPending.emplace_back(chars, value);
    env->ReleaseStringUTFChars(jkey, chars);
}

extern "C" JNIEXPORT jint JNICALL Java_com_mariopartyrd_partyboard_DualScreen_nativeImageGeneration(JNIEnv *, jclass)
{
    std::lock_guard lock(gLock);
    return gImageGeneration;
}

// Pixels of image `id` as ARGB, width and height in size[0..1]; null when not loaded yet.
extern "C" JNIEXPORT jintArray JNICALL Java_com_mariopartyrd_partyboard_DualScreen_nativeImage(JNIEnv *env, jclass, jint id, jintArray size)
{
    std::lock_guard lock(gLock);
    if (id < 0 || id >= kImageCount || gImages[id].argb.empty()) {
        return nullptr;
    }
    const Image &image = gImages[id];
    const jint dims[2] = {image.width, image.height};
    env->SetIntArrayRegion(size, 0, 2, dims);
    jintArray pixels = env->NewIntArray(static_cast<jsize>(image.argb.size()));
    env->SetIntArrayRegion(pixels, 0, static_cast<jsize>(image.argb.size()), reinterpret_cast<const jint *>(image.argb.data()));
    return pixels;
}

// Frame rates the in-game menu would offer on this screen (the ones it shows evenly).
extern "C" JNIEXPORT jintArray JNICALL Java_com_mariopartyrd_partyboard_DualScreen_nativeFrameRates(JNIEnv *env, jclass)
{
    static constexpr int kRates[] = {60, 90, 120, 144, 165};
    const auto screen = partyboard::display::supported_refresh_rates();
    std::vector<jint> rates;
    for (const int rate : kRates) {
        const bool even = std::ranges::any_of(screen, [rate](int refresh) {
            const float ratio = static_cast<float>(refresh) / static_cast<float>(rate);
            return ratio >= 0.99f && std::abs(ratio - std::round(ratio)) < 0.02f;
        });
        if (rate == kRates[0] || screen.empty() || even) {
            rates.push_back(rate);
        }
    }
    jintArray result = env->NewIntArray(static_cast<jsize>(rates.size()));
    env->SetIntArrayRegion(result, 0, static_cast<jsize>(rates.size()), rates.data());
    return result;
}
#endif
