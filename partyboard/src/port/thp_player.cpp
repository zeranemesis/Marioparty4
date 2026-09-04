#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

extern "C" {
#include "dolphin/dvd.h"
#include "dolphin/gx.h"
#include "game/hu3d.h"
#include "game/sprite.h"
}

#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace {

constexpr uint32_t kThpHeaderSize = 0x30;
constexpr int kTextureSlots = 3;
constexpr int kOutputRate = 32000;

uint32_t read_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

int16_t read_be16s(const uint8_t* p) {
    return static_cast<int16_t>((uint16_t(p[0]) << 8) | uint16_t(p[1]));
}

float read_be_float(const uint8_t* p) {
    uint32_t bits = read_be32(p);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

/* Nintendo THP stores the JPEG entropy stream already unescaped.  A regular
 * JPEG decoder instead treats every 0xff byte in that stream as the start of
 * a marker unless it is followed by a stuffed 0x00 byte.  Recreate that byte
 * stuffing while leaving the JPEG header and final EOI marker untouched. */
bool make_standard_jpeg(const uint8_t* source, size_t size, std::vector<uint8_t>& destination) {
    if (source == nullptr || size < 4 || source[0] != 0xff || source[1] != 0xd8) {
        return false;
    }

    size_t offset = 2;
    size_t entropyOffset = 0;
    while (offset + 1 < size) {
        if (source[offset] != 0xff) {
            return false;
        }
        while (offset < size && source[offset] == 0xff) ++offset;
        if (offset >= size) return false;
        const uint8_t marker = source[offset++];
        if (marker == 0xda) {
            if (offset + 2 > size) return false;
            const size_t segmentSize = (size_t(source[offset]) << 8) | source[offset + 1];
            if (segmentSize < 2 || offset + segmentSize > size) return false;
            entropyOffset = offset + segmentSize;
            break;
        }
        if (marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7)) {
            continue;
        }
        if (marker == 0xd9 || offset + 2 > size) return false;
        const size_t segmentSize = (size_t(source[offset]) << 8) | source[offset + 1];
        if (segmentSize < 2 || offset + segmentSize > size) return false;
        offset += segmentSize;
    }
    if (entropyOffset == 0 || entropyOffset >= size) return false;

    size_t eoiOffset = size;
    for (size_t i = size - 1; i > entropyOffset; --i) {
        if (source[i - 1] == 0xff && source[i] == 0xd9) {
            eoiOffset = i - 1;
            break;
        }
    }
    if (eoiOffset == size) return false;

    destination.clear();
    destination.reserve(size + (eoiOffset - entropyOffset) / 32);
    destination.insert(destination.end(), source, source + entropyOffset);
    for (size_t i = entropyOffset; i < eoiOffset; ++i) {
        destination.push_back(source[i]);
        if (source[i] == 0xff) destination.push_back(0x00);
    }
    destination.push_back(0xff);
    destination.push_back(0xd9);
    return true;
}

int clamp16(int value) {
    return std::clamp(value, -32768, 32767);
}

int gain_from_volume(float volume) {
    const float normalized = std::clamp(volume, 0.0f, 127.0f) / 127.0f;
    return static_cast<int>(std::lround(normalized * normalized * 32768.0f));
}

struct Frame {
    uint32_t videoOffset = 0;
    uint32_t videoSize = 0;
    uint32_t audioOffset = 0;
    uint32_t audioSize = 0;
};

class ThpMovie {
public:
    bool open(const char* path, bool loop, float volume) {
        DVDFileInfo file{};
        if (!DVDOpen(path, &file) || file.length < kThpHeaderSize) {
            if (file.cb.addr != nullptr) {
                DVDClose(&file);
            }
            return false;
        }
        bytes.resize(file.length);
        if (DVDReadPrio(&file, bytes.data(), static_cast<s32>(bytes.size()), 0, 2) < 0) {
            DVDClose(&file);
            bytes.clear();
            return false;
        }
        DVDClose(&file);
        if (!parse()) {
            bytes.clear();
            return false;
        }
        looped = loop;
        gain.store(gain_from_volume(volume));
        targetGain.store(gain_from_volume(volume));
        cursor.store(0);
        stopped.store(false);
        return true;
    }

    ~ThpMovie() {
        // Graphics resources are released by close_graphics() on the render thread.
    }

    void close_graphics() {
        if (graphicsClosed.exchange(true)) {
            return;
        }
        for (int i = 0; i < kTextureSlots; ++i) {
            if (textureInitialized[i]) {
                GXDestroyTexObj(&textures[i]);
                textureInitialized[i] = false;
            }
            textureData[i].clear();
            textureData[i].shrink_to_fit();
        }
    }

    void mix(int16_t* destination, uint32_t frames) {
        if (stopped.load(std::memory_order_relaxed) || audio.empty()) {
            return;
        }
        const uint32_t available = static_cast<uint32_t>(audio.size() / 2);
        uint64_t position = cursor.load(std::memory_order_relaxed);
        int current = gain.load(std::memory_order_relaxed);
        int target = targetGain.load(std::memory_order_relaxed);
        int remaining = rampRemaining.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < frames; ++i) {
            if (position >= available) {
                if (looped) {
                    position = 0;
                } else {
                    stopped.store(true, std::memory_order_relaxed);
                    break;
                }
            }
            if (remaining > 0) {
                current += (target > current) - (target < current);
                --remaining;
            } else {
                current = target;
            }
            destination[i * 2] = static_cast<int16_t>(clamp16(destination[i * 2] + ((current * audio[position * 2]) >> 15)));
            destination[i * 2 + 1] = static_cast<int16_t>(clamp16(destination[i * 2 + 1] + ((current * audio[position * 2 + 1]) >> 15)));
            ++position;
        }
        cursor.store(position, std::memory_order_relaxed);
        gain.store(current, std::memory_order_relaxed);
        rampRemaining.store(remaining, std::memory_order_relaxed);
    }

    bool ended() const {
        if (looped) {
            return false;
        }
        const uint64_t sample = cursor.load(std::memory_order_relaxed);
        const uint64_t frame = sample * static_cast<uint64_t>(fps * 1000.0f) / (uint64_t(rate) * 1000);
        return stopped.load(std::memory_order_relaxed) || frame >= frames.size();
    }

    int current_frame() const {
        const uint64_t sample = cursor.load(std::memory_order_relaxed);
        if (frames.empty()) {
            return 0;
        }
        const uint64_t frame = sample * static_cast<uint64_t>(fps * 100000.0f) / (uint64_t(rate) * 100000);
        return static_cast<int>(std::min<uint64_t>(frame, frames.size() - 1));
    }

    int total_frames() const { return static_cast<int>(frames.size()); }

    void stop() { stopped.store(true, std::memory_order_relaxed); }

    void restart() {
        cursor.store(0, std::memory_order_relaxed);
        stopped.store(false, std::memory_order_relaxed);
    }

    void set_volume(int volume, int rampMs) {
        const int newGain = gain_from_volume(static_cast<float>(volume));
        targetGain.store(newGain, std::memory_order_relaxed);
        // THPSimpleSetVolume uses `right << 5` decoded samples for its ramp.
        // Keep that timing so HuTHPSetVolume has the same behaviour as the
        // original GameCube implementation (the argument is not milliseconds).
        rampRemaining.store(std::clamp(rampMs, 0, 60000) << 5, std::memory_order_relaxed);
    }

    void draw(const HUSPRITE* sprite) {
        if (graphicsClosed.load(std::memory_order_relaxed) || frames.empty()) {
            return;
        }
        const int frameNo = current_frame();
        if (!upload_frame(frameNo)) {
            return;
        }

        Mtx modelview;
        Vec axis = {0.0f, 0.0f, 1.0f};
        if (sprite->zRot != 0.0f) {
            Mtx rotation;
            MTXRotAxisDeg(rotation, &axis, sprite->zRot);
            MTXScale(modelview, sprite->scale.x, sprite->scale.y, 1.0f);
            MTXConcat(rotation, modelview, modelview);
        } else {
            MTXScale(modelview, sprite->scale.x, sprite->scale.y, 1.0f);
        }
        mtxTransCat(modelview, sprite->pos.x, sprite->pos.y, 0.0f);
        MTXConcat(*sprite->groupMtx, modelview, modelview);

        // The decoded frame is already an opaque RGBA image.  Do not let the
        // sprite channel colour or a previous TEV state tint it: that produces
        // a translucent/coloured veil over PC THP transitions.
        GXSetColorUpdate(GX_TRUE);
        GXSetAlphaUpdate(GX_FALSE);
        GXSetCullMode(GX_CULL_NONE);
        GXSetNumChans(0);
        GXSetNumTexGens(1);
        GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
        GXSetNumTevStages(1);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
        GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
        GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
        GXSetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
        GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
        GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        GXSetCurrentMtx(GX_PNMTX0);
        GXLoadPosMtxImm(modelview, GX_PNMTX0);
        GXLoadTexObj(&textures[textureSlot], GX_TEXMAP0);

        const float left = -static_cast<float>(width) * 0.5f;
        const float top = -static_cast<float>(height) * 0.5f;
        const float right = left + static_cast<float>(width);
        const float bottom = top + static_cast<float>(height);
        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        GXPosition3f32(left, top, 0.0f);
        GXTexCoord2f32(0.0f, 0.0f);
        GXPosition3f32(right, top, 0.0f);
        GXTexCoord2f32(1.0f, 0.0f);
        GXPosition3f32(right, bottom, 0.0f);
        GXTexCoord2f32(1.0f, 1.0f);
        GXPosition3f32(left, bottom, 0.0f);
        GXTexCoord2f32(0.0f, 1.0f);
        GXEnd();
    }

private:
    bool parse() {
        if (bytes.size() < kThpHeaderSize || std::memcmp(bytes.data(), "THP\0", 4) != 0) {
            return false;
        }
        const uint32_t version = read_be32(bytes.data() + 4);
        if (version != 0x10000 && version != 0x11000) {
            return false;
        }
        fps = read_be_float(bytes.data() + 0x10);
        const uint32_t frameCount = read_be32(bytes.data() + 0x14);
        const uint32_t firstFrameSize = read_be32(bytes.data() + 0x18);
        const uint32_t compOffset = read_be32(bytes.data() + 0x20);
        const uint32_t movieOffset = read_be32(bytes.data() + 0x28);
        if (fps <= 0.0f || frameCount == 0 || firstFrameSize < 16 || compOffset + 20 > bytes.size() || movieOffset >= bytes.size()) {
            return false;
        }
        const uint32_t componentCount = read_be32(bytes.data() + compOffset);
        if (componentCount == 0 || componentCount > 16 || compOffset + 20 + componentCount * 16 > bytes.size()) {
            return false;
        }
        const uint8_t* componentIds = bytes.data() + compOffset + 4;
        uint32_t infoOffset = compOffset + 20;
        bool haveVideo = false;
        bool haveAudio = false;
        for (uint32_t i = 0; i < componentCount; ++i) {
            if (componentIds[i] == 0) {
                if (infoOffset + 8 > bytes.size()) return false;
                width = read_be32(bytes.data() + infoOffset);
                height = read_be32(bytes.data() + infoOffset + 4);
                /* THP 1.1 adds one unknown field to VideoInfo. */
                infoOffset += (version == 0x11000) ? 12 : 8;
                haveVideo = true;
            } else if (componentIds[i] == 1) {
                if (infoOffset + 12 > bytes.size()) return false;
                channels = read_be32(bytes.data() + infoOffset);
                rate = read_be32(bytes.data() + infoOffset + 4);
                audioSamples = read_be32(bytes.data() + infoOffset + 8);
                /* THP 1.1 adds numData after the common audio fields. */
                infoOffset += (version == 0x11000) ? 16 : 12;
                haveAudio = true;
            } else {
                return false;
            }
        }
        if (!haveVideo || width == 0 || height == 0 || width > 2048 || height > 2048) {
            return false;
        }
        if (haveAudio && (channels == 0 || channels > 2 || rate == 0)) {
            return false;
        }
        frames.clear();
        frames.reserve(frameCount);
        uint32_t frameOffset = movieOffset;
        uint32_t frameSize = firstFrameSize;
        for (uint32_t frameNo = 0; frameNo < frameCount; ++frameNo) {
            if (frameOffset + 8 + componentCount * 4 > bytes.size() || frameSize < 8 + componentCount * 4 || frameOffset + frameSize > bytes.size()) {
                return false;
            }
            Frame frame;
            uint32_t payloadOffset = frameOffset + 8 + componentCount * 4;
            for (uint32_t component = 0; component < componentCount; ++component) {
                const uint32_t componentSize = read_be32(bytes.data() + frameOffset + 8 + component * 4);
                if (payloadOffset + componentSize > frameOffset + frameSize) return false;
                if (componentIds[component] == 0) {
                    frame.videoOffset = payloadOffset;
                    frame.videoSize = componentSize;
                } else {
                    frame.audioOffset = payloadOffset;
                    frame.audioSize = componentSize;
                }
                payloadOffset += componentSize;
            }
            if (frame.videoSize == 0) return false;
            frames.push_back(frame);
            const uint32_t nextSize = read_be32(bytes.data() + frameOffset);
            frameOffset += frameSize;
            frameSize = nextSize;
        }
        if (haveAudio) {
            audio.reserve(audioSamples * 2);
            for (const Frame& frame : frames) {
                if (frame.audioSize == 0 || frame.audioOffset + frame.audioSize > bytes.size() || !decode_audio_frame(frame)) {
                    return false;
                }
            }
            if (audio.empty()) return false;
        }
        return true;
    }

    bool decode_audio_frame(const Frame& frame) {
        const uint32_t audioHeaderSize = 8 + channels * 36;
        if (frame.audioSize < audioHeaderSize) return false;
        const uint8_t* data = bytes.data() + frame.audioOffset;
        const uint32_t offsetNext = read_be32(data);
        const uint32_t samples = read_be32(data + 4);
        if (samples == 0 || offsetNext == 0 || audioHeaderSize + offsetNext > frame.audioSize) return false;
        const uint8_t* channelData = data + audioHeaderSize;
        std::vector<int16_t> left(samples);
        std::vector<int16_t> right(samples);
        const uint8_t* histories = data + 8 + channels * 32;
        if (!decode_channel(channelData, offsetNext, data + 8, histories, samples, left)) return false;
        if (channels == 2) {
            if (audioHeaderSize + offsetNext * 2 > frame.audioSize) return false;
            if (!decode_channel(channelData + offsetNext, offsetNext, data + 40, histories + 4, samples, right)) return false;
        } else {
            right = left;
        }
        for (uint32_t i = 0; i < samples; ++i) {
            audio.push_back(left[i]);
            audio.push_back(right[i]);
        }
        return true;
    }

    bool decode_channel(const uint8_t* encoded, uint32_t encodedSize, const uint8_t* coeffData,
                        const uint8_t* historyData, uint32_t samples, std::vector<int16_t>& output) {
        if (encodedSize == 0) return false;
        int16_t coefficients[8][2];
        for (int i = 0; i < 16; ++i) {
            coefficients[i / 2][i % 2] = read_be16s(coeffData + i * 2);
        }
        int16_t yn1 = read_be16s(historyData);
        int16_t yn2 = read_be16s(historyData + 2);
        uint32_t byteIndex = 0;
        uint32_t outputIndex = 0;
        while (outputIndex < samples) {
            if (byteIndex >= encodedSize) return false;
            const uint8_t header = encoded[byteIndex++];
            const uint8_t predictor = (header >> 4) & 7;
            const uint8_t scale = header & 0xf;

            /* One Nintendo DSP ADPCM packet contains one header byte followed
             * by seven data bytes (fourteen signed nibbles/samples). */
            for (uint32_t nibble = 0; nibble < 14 && outputIndex < samples; ++nibble) {
                if (byteIndex >= encodedSize) return false;
                int sample;
                if ((nibble & 1) == 0) {
                    sample = static_cast<int8_t>(encoded[byteIndex] & 0xf0) >> 4;
                } else {
                    sample = static_cast<int8_t>(encoded[byteIndex] << 4) >> 4;
                    ++byteIndex;
                }
                int64_t value = int64_t(coefficients[predictor][0]) * yn1 +
                                int64_t(coefficients[predictor][1]) * yn2;
                value = (value >> 11) + int64_t(sample) * (int64_t(1) << scale);
                const int16_t decoded = static_cast<int16_t>(clamp16(static_cast<int>(value)));
                output[outputIndex++] = decoded;
                yn2 = yn1;
                yn1 = decoded;
            }
        }
        return true;
    }

    bool upload_frame(int frameNo) {
        if (frameNo == uploadedFrame && textureInitialized[textureSlot]) return true;
        const Frame& frame = frames[frameNo];
        int decodedWidth = 0;
        int decodedHeight = 0;
        int decodedChannels = 0;
        if (!make_standard_jpeg(bytes.data() + frame.videoOffset, frame.videoSize, jpegData)) {
            return false;
        }
        stbi_uc* rgba = stbi_load_from_memory(jpegData.data(), static_cast<int>(jpegData.size()), &decodedWidth, &decodedHeight, &decodedChannels, 4);
        if (rgba == nullptr || decodedWidth != static_cast<int>(width) || decodedHeight != static_cast<int>(height)) {
            if (rgba) stbi_image_free(rgba);
            return false;
        }
        textureSlot = (textureSlot + 1) % kTextureSlots;
        textureData[textureSlot].resize(static_cast<size_t>(width) * height * 4);
        /* Aurora exposes a native linear RGBA8 texture format for PC.  The
         * previous manual GameCube 4x4 swizzle used a pixel offset as a tile
         * offset, causing adjacent tiles to overlap and leaving most of the
         * transition frame transparent. */
        std::memcpy(textureData[textureSlot].data(), rgba, textureData[textureSlot].size());
        stbi_image_free(rgba);
        if (!textureInitialized[textureSlot]) {
            GXInitTexObj(&textures[textureSlot], textureData[textureSlot].data(), static_cast<u16>(width), static_cast<u16>(height), GX_TF_RGBA8_PC, GX_CLAMP, GX_CLAMP, GX_FALSE);
            GXInitTexObjLOD(&textures[textureSlot], GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
            textureInitialized[textureSlot] = true;
        } else {
            GXInitTexObjData(&textures[textureSlot], textureData[textureSlot].data());
        }
        uploadedFrame = frameNo;
        return true;
    }

    std::vector<uint8_t> bytes;
    std::vector<Frame> frames;
    std::vector<int16_t> audio;
    std::vector<uint8_t> jpegData;
    std::vector<uint8_t> textureData[kTextureSlots];
    GXTexObj textures[kTextureSlots]{};
    bool textureInitialized[kTextureSlots]{};
    std::atomic<bool> graphicsClosed{false};
    std::atomic<bool> stopped{true};
    std::atomic<uint64_t> cursor{0};
    std::atomic<int> gain{0};
    std::atomic<int> targetGain{0};
    std::atomic<int> rampRemaining{0};
    bool looped = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channels = 0;
    uint32_t rate = kOutputRate;
    uint32_t audioSamples = 0;
    float fps = 0.0f;
    int textureSlot = 0;
    int uploadedFrame = -1;
};

std::mutex g_movieMutex;
std::shared_ptr<ThpMovie> g_movie;
s16 g_sprite = HUSPR_NONE;

void movie_sprite_draw(HUSPRITE* sprite) {
    std::shared_ptr<ThpMovie> movie;
    {
        std::lock_guard<std::mutex> lock(g_movieMutex);
        movie = g_movie;
    }
    if (movie) movie->draw(sprite);
}

} // namespace

extern "C" s16 HuTHPSprCreateVol(char* path, s16 loop, s16 prio, float volume) {
    auto movie = std::make_shared<ThpMovie>();
    if (!movie->open(path, loop != 0, volume)) {
        return HUSPR_NONE;
    }
    const s16 sprite = HuSprFuncCreate(movie_sprite_draw, prio);
    if (sprite == HUSPR_NONE) {
        return HUSPR_NONE;
    }
    {
        std::lock_guard<std::mutex> lock(g_movieMutex);
        g_movie = std::move(movie);
        g_sprite = sprite;
    }
    return sprite;
}

extern "C" s16 HuTHPSprCreate(char* path, s16 loop, s16 prio) {
    return HuTHPSprCreateVol(path, loop, prio, 110.0f);
}

extern "C" s16 HuTHP3DCreateVol(char* path, s16 loop, float volume) {
    (void)path;
    (void)loop;
    (void)volume;
    return HUSPR_NONE;
}

extern "C" s16 HuTHP3DCreate(char* path, s16 loop) {
    return HuTHP3DCreateVol(path, loop, 110.0f);
}

extern "C" void HuTHPStop(void) {
    std::lock_guard<std::mutex> lock(g_movieMutex);
    if (g_movie) g_movie->stop();
}

extern "C" void HuTHPClose(void) {
    std::shared_ptr<ThpMovie> movie;
    s16 sprite = HUSPR_NONE;
    {
        std::lock_guard<std::mutex> lock(g_movieMutex);
        movie = std::move(g_movie);
        sprite = g_sprite;
        g_sprite = HUSPR_NONE;
    }
    if (movie) {
        movie->stop();
        movie->close_graphics();
    }
    if (sprite != HUSPR_NONE) HuSprKill(sprite);
}

extern "C" void HuTHPRestart(void) {
    std::lock_guard<std::mutex> lock(g_movieMutex);
    if (g_movie) g_movie->restart();
}

extern "C" BOOL HuTHPEndCheck(void) {
    std::lock_guard<std::mutex> lock(g_movieMutex);
    return g_movie ? g_movie->ended() : TRUE;
}

extern "C" s32 HuTHPFrameGet(void) {
    std::lock_guard<std::mutex> lock(g_movieMutex);
    return g_movie ? g_movie->current_frame() : 0;
}

extern "C" s32 HuTHPTotalFrameGet(void) {
    std::lock_guard<std::mutex> lock(g_movieMutex);
    return g_movie ? g_movie->total_frames() : 0;
}

extern "C" void HuTHPSetVolume(s32 left, s32 right) {
    std::lock_guard<std::mutex> lock(g_movieMutex);
    if (g_movie) g_movie->set_volume(left, right);
}

extern "C" void HuTHPPCM16Mix(int16_t* destination, uint32_t frames) {
    std::shared_ptr<ThpMovie> movie;
    {
        std::lock_guard<std::mutex> lock(g_movieMutex);
        movie = g_movie;
    }
    if (movie) movie->mix(destination, frames);
}
