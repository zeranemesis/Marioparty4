#include "port/online/cubeshelf_friends.hpp"

#include <filesystem>

#include "port/http.hpp"
#include "port/main.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

namespace partyboard::online::cubeshelf {
namespace {

    namespace fs = std::filesystem;

    // games.json: the id CubeShelf puts on a Mario Party 4 invitation.
    constexpr std::string_view kGameId = "GMPE01_00";
    constexpr const char *kUserAgent = "PartyBoard-CubeShelf/1";
    // PresencePolicy: PollInterval, MaximumBackoff, MaximumDocumentBytes.
    constexpr std::int64_t kPollSeconds = 2 * 60;
    constexpr std::int64_t kMaximumBackoffSeconds = 60 * 60;
    constexpr std::size_t kMaximumDocumentBytes = 256 * 1024;
    constexpr std::size_t kMaximumProfileBytes = 1024 * 1024;

    std::int64_t unix_now()
    {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    fs::path profile_path()
    {
        return PartyBoard_ConfigPath / "cubeshelf-profile.json";
    }

    struct FriendState {
        std::optional<Snapshot> snapshot;
        int failures = 0;
        std::int64_t nextAttempt = 0;
    };

    struct Service {
        std::mutex mutex;
        std::condition_variable wake;
        bool loaded = false;
        bool threadStarted = false;
        bool refreshRequested = false;
        std::optional<Profile> profile;
        std::map<std::string, FriendState> friends;
        long long revision = 1;
    };

    // Never destroyed: the reader thread may still be inside an HTTP request at exit.
    Service &service()
    {
        static auto *instance = new Service;
        return *instance;
    }

    bool save(const Profile &profile)
    {
        std::error_code ec;
        const fs::path path = profile_path();
        fs::create_directories(path.parent_path(), ec);
        fs::path temporary = path;
        temporary += ".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out) {
                return false;
            }
            out << serialize_profile(profile);
            if (!out) {
                return false;
            }
        }
        fs::rename(temporary, path, ec);
        if (ec) {
            fs::remove(temporary, ec);
            return false;
        }
        // The file holds the identity key, like CubeShelf's identity.key: owner only.
        fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
        return true;
    }

    std::optional<Profile> load()
    {
        std::ifstream in(profile_path(), std::ios::binary);
        if (!in) {
            return std::nullopt;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return deserialize_profile(buffer.str());
    }

    std::string short_id(const std::string &payload)
    {
        unsigned char digest[32];
        mbedtls_sha256(reinterpret_cast<const unsigned char *>(payload.data()), payload.size(), digest, 0);
        static constexpr char hex[] = "0123456789abcdef";
        std::string out;
        for (int i = 0; i < 6; ++i) {
            out += hex[digest[i] >> 4];
            out += hex[digest[i] & 15];
        }
        return out;
    }

    std::int64_t backoff(int failures)
    {
        std::int64_t delay = kPollSeconds << std::min(failures, 10);
        return std::min(delay, kMaximumBackoffSeconds);
    }

    // One pass over every friend, like PresenceFetcher.FetchAllAsync but one at a time: a phone
    // has few friends and a pass every two minutes.
    void read_friends_once()
    {
        auto &s = service();
        Profile profile;
        {
            std::lock_guard lock(s.mutex);
            if (!s.profile) {
                return;
            }
            profile = *s.profile;
        }

        bool changed = false;
        bool sequencesAdvanced = false;
        for (auto &f : profile.friends) {
            PublicKey key;
            if (f.paused || !decode_public_key(f.publicKey, key)) {
                continue;
            }
            const std::int64_t now = unix_now();
            bool hadSnapshot = false;
            {
                std::lock_guard lock(s.mutex);
                const auto &state = s.friends[f.publicKey];
                if (state.nextAttempt > now) {
                    continue;
                }
                hadSnapshot = state.snapshot.has_value();
            }

            const auto response = http::get(f.presenceUrl, kUserAgent);
            std::optional<Snapshot> opened;
            bool reachable = response.status >= 200 && response.status < 300 && response.body.size() <= kMaximumDocumentBytes;
            if (reachable) {
                opened = open_presence(profile, key, response.body);
            }

            std::lock_guard lock(s.mutex);
            auto &state = s.friends[f.publicKey];
            if (!reachable) {
                state.failures = std::min(state.failures + 1, 16);
                state.nextAttempt = now + backoff(state.failures);
                continue;
            }
            state.failures = 0;
            state.nextAttempt = 0;
            // Replays are refused (FriendStore.TryAcceptSequence). The one exception is the very
            // first read: the PC handed over the last sequence it saw, and the document may not
            // have moved since -- it is still the right thing to show.
            if (opened && (opened->sequence > f.lastSequence || (!hadSnapshot && opened->sequence == f.lastSequence))) {
                if (opened->sequence > f.lastSequence) {
                    f.lastSequence = opened->sequence;
                    sequencesAdvanced = true;
                }
                state.snapshot = std::move(opened);
                changed = true;
            }
        }

        std::lock_guard lock(s.mutex);
        if (sequencesAdvanced && s.profile && s.profile->publicKey == profile.publicKey) {
            // Only the sequences move; the list itself may have been replaced meanwhile.
            for (auto &stored : s.profile->friends) {
                for (const auto &seen : profile.friends) {
                    if (stored.publicKey == seen.publicKey) {
                        stored.lastSequence = std::max(stored.lastSequence, seen.lastSequence);
                    }
                }
            }
            save(*s.profile);
        }
        if (changed) {
            ++s.revision;
        }
    }

    void reader_thread()
    {
        auto &s = service();
        for (;;) {
            read_friends_once();
            std::unique_lock lock(s.mutex);
            s.wake.wait_for(lock, std::chrono::seconds(kPollSeconds), [&] { return s.refreshRequested; });
            s.refreshRequested = false;
        }
    }

    std::string lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    ImportError adopt(std::optional<Profile> profile, ImportError error)
    {
        if (!profile) {
            return error;
        }
        auto &s = service();
        std::lock_guard lock(s.mutex);
        if (!save(*profile)) {
            return ImportError::Unreadable;
        }
        s.profile = std::move(profile);
        s.loaded = true;
        s.friends.clear();
        s.refreshRequested = true;
        ++s.revision;
        s.wake.notify_all();
        return ImportError::None;
    }

} // namespace

void start() noexcept
{
    auto &s = service();
    std::lock_guard lock(s.mutex);
    if (!s.loaded) {
        s.loaded = true;
        try {
            s.profile = load();
        }
        catch (...) {
            s.profile.reset();
        }
    }
    if (!s.threadStarted && http::available()) {
        try {
            std::thread(reader_thread).detach();
            s.threadStarted = true;
        }
        catch (...) {
        }
    }
}

NativeView view(bool french)
{
    const auto t = [french](const char *fr, const char *en) { return std::string(french ? fr : en); };
    auto &s = service();
    std::lock_guard lock(s.mutex);

    NativeView out;
    out.revision = s.revision;
    if (!s.profile) {
        return out;
    }
    const Profile &me = *s.profile;
    out.hasProfile = true;
    out.me = handle(me.name, me.publicKey);
    const std::int64_t now = unix_now();

    // Two friends who would read the same -- or one who reads like us -- both get six digits.
    std::map<std::string, int> shortCount;
    std::vector<PublicKey> keys(me.friends.size());
    std::vector<bool> valid(me.friends.size());
    for (std::size_t i = 0; i < me.friends.size(); ++i) {
        valid[i] = decode_public_key(me.friends[i].publicKey, keys[i]);
        if (valid[i]) {
            ++shortCount[lower(handle(me.friends[i].name, keys[i]))];
        }
    }
    const std::string ownLower = lower(out.me);

    for (std::size_t i = 0; i < me.friends.size(); ++i) {
        if (!valid[i]) {
            continue;
        }
        const auto &f = me.friends[i];
        const std::string shortHandle = handle(f.name, keys[i]);
        const bool clash = shortCount[lower(shortHandle)] > 1 || lower(shortHandle) == ownLower;

        FriendView v;
        v.key = f.publicKey;
        v.handle = clash ? handle(f.name, keys[i], true) : shortHandle;

        const auto it = s.friends.find(f.publicKey);
        const Snapshot *known = it != s.friends.end() && it->second.snapshot ? &*it->second.snapshot : nullptr;
        const Status effective = known != nullptr ? effective_status(*known, now) : Status::Offline;
        v.status = f.paused ? "paused" : effective == Status::InGame ? "ingame" : effective == Status::Online ? "online" : "offline";
        const std::string game = known != nullptr && !known->currentGameTitle.empty() ? known->currentGameTitle : t("un jeu", "a game");
        v.label = v.status == "paused" ? t("En pause", "Paused")
            : v.status == "ingame"     ? (french ? "En jeu : " + game : "Playing " + game)
            : v.status == "online"     ? t("En ligne", "Online")
                                       : t("Hors ligne", "Offline");
        v.invitesYou = !f.paused && known != nullptr && invites(*known, me, kGameId, now);
        if (v.invitesYou) {
            v.inviteId = short_id(known->invite->joinPayload);
        }
        out.friends.push_back(std::move(v));
    }

    // Whoever is inviting you first, then who could play right now, then everyone else.
    const auto rank = [](const FriendView &v) {
        return v.invitesYou ? 0 : v.status == "ingame" ? 1 : v.status == "online" ? 2 : v.status == "offline" ? 3 : 4;
    };
    std::stable_sort(out.friends.begin(), out.friends.end(), [&](const FriendView &a, const FriendView &b) {
        if (rank(a) != rank(b)) {
            return rank(a) < rank(b);
        }
        return lower(a.handle) < lower(b.handle);
    });
    return out;
}

ImportError import_from_text(const std::string &text, const std::string &passphrase)
{
    ImportError error = ImportError::None;
    auto profile = import_profile(text, passphrase, error);
    return adopt(std::move(profile), error);
}

ImportError import_from_path(const std::string &path, const std::string &passphrase)
{
    // SDL reads Android content:// URIs as well as plain paths.
    std::size_t size = 0;
    void *data = SDL_LoadFile(path.c_str(), &size);
    if (data == nullptr) {
        return ImportError::Unreadable;
    }
    std::string text;
    if (size <= kMaximumProfileBytes) {
        text.assign(static_cast<const char *>(data), size);
    }
    SDL_free(data);
    if (text.empty()) {
        return ImportError::NotAProfile;
    }
    return import_from_text(text, passphrase);
}

void forget_profile() noexcept
{
    auto &s = service();
    std::lock_guard lock(s.mutex);
    s.profile.reset();
    s.friends.clear();
    ++s.revision;
    std::error_code ec;
    fs::remove(profile_path(), ec);
}

void refresh_now() noexcept
{
    auto &s = service();
    std::lock_guard lock(s.mutex);
    for (auto &[key, state] : s.friends) {
        state.nextAttempt = 0;
    }
    s.refreshRequested = true;
    s.wake.notify_all();
}

std::optional<std::string> join_payload(const std::string &friendKey)
{
    auto &s = service();
    std::lock_guard lock(s.mutex);
    if (!s.profile) {
        return std::nullopt;
    }
    const auto friendIt = std::find_if(s.profile->friends.begin(), s.profile->friends.end(), [&](const Friend &f) { return f.publicKey == friendKey; });
    const auto it = s.friends.find(friendKey);
    if (friendIt == s.profile->friends.end() || friendIt->paused || it == s.friends.end() || !it->second.snapshot
        || !invites(*it->second.snapshot, *s.profile, kGameId, unix_now())) {
        return std::nullopt;
    }
    return it->second.snapshot->invite->joinPayload;
}

} // namespace partyboard::online::cubeshelf
