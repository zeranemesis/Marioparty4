#include "port/retroachievements.h"

#include "port/config.hpp"
#include "port/http.hpp"
#include "port/netplay_runtime.h"
#include "port/retroachievements_memory.hpp"
#include "port/settings.h"
#include "ui/ui.hpp"

#include "partyboard_version.h"

#include <aurora/lib/logging.hpp>
#include <nod.h>
#include <rc_client.h>
#include <rc_hash.h>
#include <SDL3/SDL_platform.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace partyboard::ra {

namespace {

aurora::Module Log("partyboard::ra");

// Work finished on a worker thread, to be completed on the main thread. rcheevos
// callbacks and the toasts they raise then all run where the UI lives.
struct Completions {
    std::mutex mutex;
    std::deque<std::function<void()>> pending;

    void push(std::function<void()> fn) {
        std::lock_guard lock(mutex);
        pending.push_back(std::move(fn));
    }
    void drain() {
        std::deque<std::function<void()>> ready;
        {
            std::lock_guard lock(mutex);
            ready.swap(pending);
        }
        for (auto& fn : ready) {
            fn();
        }
    }
};

rc_client_t* s_client = nullptr;
// Shared with detached workers, so one that finishes after shutdown still has
// somewhere to put its result.
std::shared_ptr<Completions> s_completions;
// Bumped by shutdown and logout: a result that belongs to an older session is
// dropped instead of reaching an rc_client that no longer expects it.
uint64_t s_generation = 0;
State s_state = State::Unavailable;
std::string s_status;
std::string s_userAgent;
// Whether a game tick evaluated the set since the last frame pump. rcheevos
// wants rc_client_idle on every frame that has no rc_client_do_frame (a pause,
// a loading screen), so it can keep the session alive.
bool s_tickedThisFrame = false;
uint32_t s_revision = 0;

void set_state(State state, std::string status) {
    s_state = state;
    s_status = std::move(status);
    Log.info("{}", s_status);
}

void toast(const std::string& title, const std::string& content) {
    if (!getSettings().game.enableAchievementToasts) {
        return;
    }
    ui::push_toast({
        .type = "achievement",
        .title = title,
        .content = content,
        .duration = std::chrono::seconds(5),
    });
}

std::string build_user_agent() {
    // The server only honours clients it can identify: <product>/<numeric
    // version> (<system>) followed by rcheevos' own clause.
    std::string agent = "PartyBoard/" PARTY_BOARD_VERSION_STRING " (";
    agent += SDL_GetPlatform();
#ifdef PARTY_BOARD_ARCH
    agent += " " PARTY_BOARD_ARCH;
#endif
    agent += ")";
    char clause[64] = {};
    rc_client_get_user_agent_clause(s_client, clause, sizeof(clause));
    if (clause[0] != '\0') {
        agent += " ";
        agent += clause;
    }
    return agent;
}

// rc_client_server_call_t: run the request on a worker thread, hand the answer
// back on the main thread.
void RC_CCONV server_call(const rc_api_request_t* request, rc_client_server_callback_t callback,
    void* callbackData, rc_client_t*) {
    std::string url = request->url;
    std::string body = request->post_data != nullptr ? request->post_data : "";
    std::string contentType = request->content_type != nullptr ? request->content_type
                                                                : "application/x-www-form-urlencoded";
    std::weak_ptr<Completions> completions = s_completions;
    const uint64_t generation = s_generation;
    std::string agent = s_userAgent;

    std::thread([=] {
        http::Response response = http::post(url, body, contentType, agent);
        auto sink = completions.lock();
        if (!sink) {
            return;
        }
        sink->push([=] {
            if (generation != s_generation || s_client == nullptr) {
                return;
            }
            rc_api_server_response_t result{};
            result.body = response.body.c_str();
            result.body_length = response.body.size();
            // No answer at all (network, TLS, timeout) is worth retrying;
            // rcheevos queues unlocks and resends them.
            result.http_status_code =
                response.status != 0 ? response.status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
            if (response.status == 0) {
                Log.warn("Request failed: {}", response.error);
            }
            // PARTYBOARD_RA_DUMP_SET=<file> keeps the set definitions the server
            // sent, to see which GameCube addresses remain to be translated.
            // The response carries no credentials.
            if (const char* dump = std::getenv("PARTYBOARD_RA_DUMP_SET");
                dump != nullptr && *dump != '\0' && response.status == 200
                && (body.find("r=achievementsets") != std::string::npos || body.find("r=patch") != std::string::npos)) {
                if (FILE* file = std::fopen(dump, "wb")) {
                    std::fwrite(response.body.data(), 1, response.body.size(), file);
                    std::fclose(file);
                    Log.info("Set definitions written to {}", dump);
                }
            }
            callback(&result, callbackData);
        });
    }).detach();
}

uint32_t RC_CCONV read_memory(uint32_t address, uint8_t* buffer, uint32_t numBytes, rc_client_t*) {
    return readMemory(address, buffer, numBytes);
}

void RC_CCONV log_message(const char* message, const rc_client_t*) { Log.info("rcheevos: {}", message); }

void RC_CCONV handle_event(const rc_client_event_t* event, rc_client_t*) {
    switch (event->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
        ++s_revision;
        toast("Achievement Unlocked!", event->achievement->title);
        Log.info("Unlocked \"{}\" ({} points)", event->achievement->title, event->achievement->points);
        break;
    case RC_CLIENT_EVENT_GAME_COMPLETED:
        toast("RetroAchievements", "Every achievement unlocked!");
        break;
    case RC_CLIENT_EVENT_SERVER_ERROR:
        Log.warn("Server error: {}", event->server_error->error_message);
        break;
    case RC_CLIENT_EVENT_DISCONNECTED:
        toast("RetroAchievements", "Connection lost; unlocks will be sent later");
        break;
    case RC_CLIENT_EVENT_RECONNECTED:
        toast("RetroAchievements", "Reconnected");
        break;
    default:
        break;
    }
}

// rc_hash reads the disc through these, so the hash is computed exactly as an
// emulator would, from the same bytes, whatever the image format nod decodes.
void* RC_CCONV hash_open(const char* path) {
    NodHandle* handle = nullptr;
    if (nod_disc_open(path, nullptr, &handle) != NOD_RESULT_OK) {
        return nullptr;
    }
    return handle;
}
void RC_CCONV hash_seek(void* handle, int64_t offset, int origin) {
    nod_seek(static_cast<NodHandle*>(handle), offset, origin);
}
int64_t RC_CCONV hash_tell(void* handle) { return nod_seek(static_cast<NodHandle*>(handle), 0, 1); }
size_t RC_CCONV hash_read(void* handle, void* buffer, size_t requested) {
    auto* out = static_cast<uint8_t*>(buffer);
    size_t total = 0;
    while (total < requested) {
        const int64_t got = nod_read(static_cast<NodHandle*>(handle), out + total, requested - total);
        if (got <= 0) {
            break;
        }
        total += static_cast<size_t>(got);
    }
    return total;
}
void RC_CCONV hash_close(void* handle) { nod_free(static_cast<NodHandle*>(handle)); }

std::string disc_hash(const std::string& path) {
    rc_hash_iterator_t iterator;
    rc_hash_initialize_iterator(&iterator, path.c_str(), nullptr, 0);
    iterator.callbacks.filereader.open = hash_open;
    iterator.callbacks.filereader.seek = hash_seek;
    iterator.callbacks.filereader.tell = hash_tell;
    iterator.callbacks.filereader.read = hash_read;
    iterator.callbacks.filereader.close = hash_close;
    char hash[33] = {};
    const int ok = rc_hash_generate(hash, RC_CONSOLE_GAMECUBE, &iterator);
    rc_hash_destroy_iterator(&iterator);
    return ok ? std::string(hash) : std::string();
}

void load_game() {
    const std::string path = getSettings().backend.isoPath;
    if (path.empty()) {
        set_state(State::LoggedIn, "Logged in; no disc selected");
        return;
    }
    set_state(State::LoadingGame, "Identifying the disc...");
    std::weak_ptr<Completions> completions = s_completions;
    const uint64_t generation = s_generation;
    // Reading the disc takes a moment; keep it off the game thread.
    std::thread([=] {
        std::string hash = disc_hash(path);
        auto sink = completions.lock();
        if (!sink) {
            return;
        }
        sink->push([=] {
            if (generation != s_generation || s_client == nullptr) {
                return;
            }
            if (hash.empty()) {
                set_state(State::NoSet, "Could not read the disc to identify it");
                return;
            }
            Log.info("Disc hash {}", hash);
            rc_client_begin_load_game(
                s_client, hash.c_str(),
                [](int result, const char* error, rc_client_t* client, void*) {
                    if (result == RC_OK) {
                        const rc_client_game_t* game = rc_client_get_game_info(client);
                        rc_client_user_game_summary_t summary{};
                        rc_client_get_user_game_summary(client, &summary);
                        set_state(State::Playing,
                            std::string(game->title) + ": " + std::to_string(summary.num_unlocked_achievements)
                                + "/" + std::to_string(summary.num_core_achievements) + " unlocked");
                        logUnmappedAddresses();
                        toast("RetroAchievements", game->title);
                    } else if (result == RC_NO_GAME_LOADED) {
                        set_state(State::NoSet, "This disc has no achievement set");
                    } else {
                        set_state(State::NoSet,
                            std::string("Could not load the achievements: ") + (error ? error : "unknown error"));
                    }
                },
                nullptr);
        });
    }).detach();
}

void on_logged_in() {
    const rc_client_user_t* user = rc_client_get_user_info(s_client);
    auto& settings = getSettings().retroAchievements;
    settings.username.setValue(user->username);
    settings.token.setValue(user->token);
    config::Save();
    set_state(State::LoggedIn, std::string("Logged in as ") + user->display_name);
    load_game();
}

void RC_CCONV login_callback(int result, const char* error, rc_client_t*, void*) {
    if (result == RC_OK) {
        on_logged_in();
        return;
    }
    // A rejected token is stale, not merely unusable right now: forget it so
    // the settings screen asks for the password again.
    if (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN) {
        getSettings().retroAchievements.token.setValue("");
        config::Save();
    }
    set_state(State::LoggedOut, std::string("Login failed: ") + (error ? error : "unknown error"));
}

} // namespace

State state() { return s_state; }

uint32_t revision() { return s_revision; }

std::vector<AchievementInfo> achievements() {
    std::vector<AchievementInfo> result;
    if (s_client == nullptr || s_state != State::Playing) {
        return result;
    }
    rc_client_achievement_list_t* list = rc_client_create_achievement_list(
        s_client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    if (list == nullptr) {
        return result;
    }
    for (uint32_t b = 0; b < list->num_buckets; ++b) {
        const rc_client_achievement_bucket_t& bucket = list->buckets[b];
        for (uint32_t i = 0; i < bucket.num_achievements; ++i) {
            const rc_client_achievement_t* a = bucket.achievements[i];
            // The server injects warnings as fake achievements with ids from
            // 101000000; they are not part of the game's set.
            if (a->id >= 101000000u) {
                continue;
            }
            result.push_back({
                .id = a->id,
                .title = a->title,
                .description = a->description,
                .points = a->points,
                .unlocked = (a->unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_SOFTCORE) != 0,
                .progress = a->measured_progress,
                .progressPercent = a->measured_percent,
            });
        }
    }
    rc_client_destroy_achievement_list(list);
    return result;
}
std::string statusText() { return s_status; }

std::string username() {
    if (s_client != nullptr) {
        if (const rc_client_user_t* user = rc_client_get_user_info(s_client)) {
            return user->display_name;
        }
    }
    return getSettings().retroAchievements.username;
}

void loginWithPassword(const std::string& user, const std::string& password) {
    if (s_client == nullptr || user.empty() || password.empty()) {
        return;
    }
    set_state(State::LoggingIn, "Logging in...");
    rc_client_begin_login_with_password(s_client, user.c_str(), password.c_str(), login_callback, nullptr);
}

void logout() {
    if (s_client == nullptr) {
        return;
    }
    ++s_generation;
    rc_client_logout(s_client);
    getSettings().retroAchievements.token.setValue("");
    config::Save();
    set_state(State::LoggedOut, "Logged out");
}

} // namespace partyboard::ra

using namespace partyboard;
using namespace partyboard::ra;

void PartyBoard_RAInit(void) {
    if (s_client != nullptr) {
        return;
    }
    if (!http::available()) {
        set_state(State::Unavailable, "RetroAchievements is not available in this build (no HTTPS backend)");
        return;
    }
    const auto& settings = getSettings().retroAchievements;
    if (!settings.enabled.getValue()) {
        set_state(State::Disabled, "RetroAchievements is switched off");
        return;
    }

    s_completions = std::make_shared<Completions>();
    s_client = rc_client_create(read_memory, server_call);
    rc_client_enable_logging(s_client, RC_CLIENT_LOG_LEVEL_INFO, log_message);
    rc_client_set_event_handler(s_client, handle_event);
    // Hardcore needs a client the RetroAchievements team has validated.
    rc_client_set_hardcore_enabled(s_client, 0);
    // PARTYBOARD_RA_SPECTATOR=1 evaluates the set and raises the notifications
    // but submits nothing to the server: for checking the address translation
    // without unlocking anything on the account.
    if (const char* spectator = std::getenv("PARTYBOARD_RA_SPECTATOR"); spectator != nullptr && *spectator == '1') {
        rc_client_set_spectator_mode_enabled(s_client, 1);
        Log.warn("Spectator mode: nothing will be submitted to the server");
    }
    s_userAgent = build_user_agent();
    Log.info("User agent: {}", s_userAgent);

    const std::string user = settings.username.getValue();
    const std::string token = settings.token.getValue();
    if (!user.empty() && !token.empty()) {
        set_state(State::LoggingIn, "Logging in...");
        rc_client_begin_login_with_token(s_client, user.c_str(), token.c_str(), login_callback, nullptr);
    } else {
        set_state(State::LoggedOut, "Not logged in");
    }
}

void PartyBoard_RAShutdown(void) {
    if (s_client == nullptr) {
        return;
    }
    ++s_generation;
    rc_client_destroy(s_client);
    s_client = nullptr;
    s_completions.reset();
}

void PartyBoard_RAGameTick(void) {
    if (s_client == nullptr || s_state != State::Playing) {
        return;
    }
    // An online session rolls back and replays ticks, and would evaluate the
    // same moment several times. Keep the session alive without evaluating.
    if (PartyBoard_NetplayEnabled()) {
        return;
    }
    beginTick();
    rc_client_do_frame(s_client);
    s_tickedThisFrame = true;
}

void PartyBoard_RAFramePump(void) {
    if (s_client == nullptr) {
        return;
    }
    s_completions->drain();
    // Log the rich presence whenever it changes. It describes the game's state
    // from the same translated addresses as the achievements, so it doubles as
    // a readable check of the translation ("In the title screen", the board,
    // the characters...).
    if (s_state == State::Playing) {
        static uint32_t frames = 0;
        static std::string lastPresence;
        if (++frames % 60 == 0) {
            char presence[256] = {};
            rc_client_get_rich_presence_message(s_client, presence, sizeof(presence));
            if (lastPresence != presence) {
                lastPresence = presence;
                Log.info("Rich presence: {}", lastPresence);
            }
        }
    }
    if (!s_tickedThisFrame) {
        rc_client_idle(s_client);
    }
    s_tickedThisFrame = false;
}
