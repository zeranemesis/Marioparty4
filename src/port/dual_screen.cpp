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
#include "game/gamework_data.h"
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
