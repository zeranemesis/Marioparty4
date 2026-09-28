#include "cubeshelf.hpp"

#include "../file_select.hpp"
#include "localization.hpp"
#include "modal.hpp"
#include "online.hpp"
#include "pane.hpp"
#include "port/android_bridge.hpp"
#include "port/main.h"
#include "port/online/cubeshelf_friends.hpp"
#include "string_button.hpp"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_stdinc.h>
#include <aurora/lib/window.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

namespace partyboard::ui::cubeshelf {
namespace {

    namespace fs = std::filesystem;

    // A state file older than this was left by a CubeShelf that is no longer running. Offering to
    // host then would close the game and leave nobody to open the lobby.
    constexpr long long kFreshSeconds = 15;
    // How long the game waits for CubeShelf to answer a request before giving up.
    constexpr auto kReplyTimeout = std::chrono::seconds(12);
    constexpr std::uintmax_t kMaximumStateBytes = 256 * 1024;
    constexpr std::size_t kMaximumFriends = 500;
    constexpr std::size_t kMaximumString = 400;

    // CUBESHELF_INGAME_DIR is UTF-8. A path built from a plain std::string on Windows is read in
    // the ANSI code page, which breaks the moment the account name has an accent in it.
    fs::path utf8_path(const std::string &value)
    {
        return fs::path(std::u8string(value.begin(), value.end()));
    }

    const std::optional<fs::path> &directory() noexcept
    {
        static const std::optional<fs::path> dir = []() -> std::optional<fs::path> {
            const char *value = SDL_getenv("CUBESHELF_INGAME_DIR");
            if (value == nullptr || *value == '\0') {
                return std::nullopt;
            }
            std::error_code ec;
            fs::path path = utf8_path(value);
            if (!fs::is_directory(path, ec)) {
                return std::nullopt;
            }
            return path;
        }();
        return dir;
    }

    long long unix_now() noexcept
    {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    std::string capped(std::string value)
    {
        if (value.size() > kMaximumString) {
            value.resize(kMaximumString);
        }
        return value;
    }

    std::string text_of(const nlohmann::json &object, const char *key)
    {
        const auto it = object.find(key);
        return it != object.end() && it->is_string() ? capped(it->get<std::string>()) : std::string();
    }

    bool flag_of(const nlohmann::json &object, const char *key)
    {
        const auto it = object.find(key);
        return it != object.end() && it->is_boolean() && it->get<bool>();
    }

    std::optional<std::string> read_file(const fs::path &path, std::uintmax_t limit) noexcept
    {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        if (ec || size > limit) {
            return std::nullopt;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return std::nullopt;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // Written beside its final name and renamed into place, so the other side never reads half a
    // file.
    bool write_atomically(const fs::path &path, const std::string &content) noexcept
    {
        std::error_code ec;
        fs::path temporary = path;
        temporary += ".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out) {
                return false;
            }
            out << content;
            if (!out) {
                return false;
            }
        }
        fs::rename(temporary, path, ec);
        if (ec) {
            fs::remove(temporary, ec);
            return false;
        }
        return true;
    }

    std::string new_request_id()
    {
        std::random_device device;
        std::mt19937_64 engine(device());
        char id[17];
        std::snprintf(id, sizeof id, "%016llx", static_cast<unsigned long long>(engine()));
        return id;
    }

    // The toast renders a title or a message that starts with '<' as markup rather than text.
    // Every string here is shown as text, and a friend's pseudo is the one part CubeShelf did not
    // write itself; a leading space makes the toast escape it like anything else.
    std::string as_toast_text(std::string value)
    {
        if (!value.empty() && value.front() == '<') {
            value.insert(value.begin(), ' ');
        }
        return value;
    }

    clock::time_point sLastTick {};
    std::set<std::string> sAnnounced;

    // Where CubeShelf cannot run (Android, or the game started without it), the game reads the
    // friends itself from a profile exported by CubeShelf on the PC. Windows keeps going through
    // CubeShelf and its companion.
    bool native_mode() noexcept
    {
#if defined(_WIN32)
        return false;
#else
        return !directory().has_value();
#endif
    }

    bool french() noexcept
    {
        return getSettings().game.language.getValue() == GameLanguage::French;
    }

    // MainWindow.InGame.cs InGameText, for a device that reads its friends but never hosts.
    std::map<std::string, std::string> native_text(bool fr)
    {
        const auto t = [fr](const char *f, const char *e) { return std::string(fr ? f : e); };
        return {
            { "tab", t("Amis", "Friends") },
            { "title", t("Amis", "Friends") },
            { "friendsSection", t("Amis", "Friends") },
            { "noFriends", t("Pas encore d’amis. Ajoute-les dans CubeShelf sur ton PC, puis exporte ton profil de nouveau.",
                               "No friends yet. Add them in CubeShelf on your PC, then export your profile again.") },
            { "join", t("Rejoindre", "Join") },
            { "inviteTitle", t("Invitation", "Invitation") },
            { "inviteToast", t("t’invite à jouer. Menu, onglet Amis, pour rejoindre.", "invites you to play. Menu, Friends tab, to join.") },
            { "notice", t("Cet appareil lit tes amis et leurs invitations. C’est ton PC qui publie ta présence et crée les salons.",
                            "This device reads your friends and their invitations. Your PC publishes your presence and creates lobbies.") },
        };
    }

    std::optional<State> native_state()
    {
        online::cubeshelf::start();
        const bool fr = french();
        const auto view = online::cubeshelf::view(fr);
        State state;
        state.fresh = true;
        state.revision = view.revision;
        state.ready = view.hasProfile;
        state.me = view.me;
        state.text = native_text(fr);
        state.notice = view.hasProfile ? state.text["notice"] : std::string();
        for (const auto &v : view.friends) {
            state.friends.push_back(Friend {
                .key = v.key,
                .handle = v.handle,
                .status = v.status,
                .label = v.label,
                .inviteId = v.inviteId,
                .invitesYou = v.invitesYou,
            });
        }
        return state;
    }

    // The file picker answers on its own schedule, possibly from another thread; the window
    // collects the path on its next update.
    std::mutex sPickedMutex;
    std::optional<std::string> sPickedProfile;

    void profile_picked(void *, const char *path, const char *error)
    {
        if (path == nullptr || error != nullptr) {
            return;
        }
        std::lock_guard lock(sPickedMutex);
        sPickedProfile = path;
    }

    constexpr SDL_DialogFileFilter kProfileFilters[] = {
        { "CubeShelf profile", "cubeshelf-profile" },
        { "All Files", "*" },
    };

} // namespace

std::optional<State> read_state_unchecked();

std::string State::t(const char *key, const char *fallback) const
{
    const auto it = text.find(key);
    return it != text.end() && !it->second.empty() ? it->second : std::string(fallback);
}

bool available() noexcept
{
    return directory().has_value() || native_mode();
}

std::optional<State> read_state() noexcept
{
    // nlohmann's value() and get<>() throw on a field of the wrong type, and this function is
    // noexcept: a throw would be std::terminate, a crash in the middle of a game. A state file
    // that cannot be read is simply no state.
    try {
        if (native_mode()) {
            return native_state();
        }
        return read_state_unchecked();
    }
    catch (...) {
        return std::nullopt;
    }
}

std::optional<State> read_state_unchecked()
{
    const auto &dir = directory();
    if (!dir) {
        return std::nullopt;
    }
    const auto raw = read_file(*dir / "state.json", kMaximumStateBytes);
    if (!raw) {
        return std::nullopt;
    }
    const auto json = nlohmann::json::parse(*raw, nullptr, false);
    if (json.is_discarded() || !json.is_object() || json.value("schema", 0) != 1) {
        return std::nullopt;
    }

    State state;
    const long long updated = json.value("updatedAt", 0LL);
    const long long age = unix_now() - updated;
    // A little slack either way for two clocks read a moment apart.
    state.fresh = age >= -60 && age <= kFreshSeconds;
    state.revision = json.value("revision", 0LL);
    state.me = text_of(json, "me");
    state.notice = text_of(json, "notice");
    state.ready = flag_of(json, "ready");
    state.canHost = flag_of(json, "canHost");
    state.hosting = flag_of(json, "hosting");

    if (const auto text = json.find("text"); text != json.end() && text->is_object()) {
        for (auto it = text->begin(); it != text->end(); ++it) {
            if (it.value().is_string()) {
                state.text[it.key()] = capped(it.value().get<std::string>());
            }
        }
    }
    if (const auto friends = json.find("friends"); friends != json.end() && friends->is_array()) {
        for (const auto &entry : *friends) {
            if (!entry.is_object() || state.friends.size() >= kMaximumFriends) {
                continue;
            }
            Friend f;
            f.key = text_of(entry, "key");
            f.handle = text_of(entry, "handle");
            f.status = text_of(entry, "status");
            f.label = text_of(entry, "label");
            f.inviteId = text_of(entry, "inviteId");
            f.invitesYou = flag_of(entry, "invitesYou");
            if (!f.key.empty() && !f.handle.empty()) {
                state.friends.push_back(std::move(f));
            }
        }
    }
    return state;
}

std::string tab_title() noexcept
{
    const auto state = read_state();
    return state ? state->t("tab", "Friends") : std::string("Friends");
}

void tick() noexcept
{
    if (!available()) {
        return;
    }
    const auto now = clock::now();
    if (now - sLastTick < std::chrono::seconds(2)) {
        return;
    }
    sLastTick = now;

    const auto state = read_state();
    if (!state || !state->fresh) {
        return;
    }
    for (const auto &f : state->friends) {
        if (!f.invitesYou || f.inviteId.empty()) {
            continue;
        }
        // Once per lobby, not once per read: the file is rewritten every few seconds with the
        // invitation unchanged, and a toast every two seconds would be worse than none.
        if (!sAnnounced.insert(f.key + "|" + f.inviteId).second) {
            continue;
        }
        if (sAnnounced.size() > 256) {
            sAnnounced.clear();
            sAnnounced.insert(f.key + "|" + f.inviteId);
        }
        push_toast({
            .type = "",
            .title = as_toast_text(state->t("inviteTitle", "Invitation")),
            .content = as_toast_text(f.handle + " " + state->t("inviteToast", "invites you. Open F1, Friends tab, to join.")),
            .duration = std::chrono::seconds(8),
        });
    }
}

FriendsWindow::FriendsWindow()
{
    add_tab(tab_title(), [this](Rml::Element *content) { build(content); });
}

void FriendsWindow::build(Rml::Element *content)
{
    const auto state = read_state();
    mRevision = state ? state->revision : -1;
    mFresh = state && state->fresh;

    if (native_mode() && (!state || !state->ready)) {
        build_import(content);
        return;
    }

    auto &pane = add_child<Pane>(content, Pane::Type::Controlled);

    if (!state || !state->fresh) {
        pane.add_section(state ? state->t("title", "Friends") : std::string("Friends"));
        pane.add_text(state ? state->t("cubeshelfClosed", "CubeShelf is not running. Friends and invitations go through it: open it and come back.")
                            : std::string("CubeShelf is not running. Friends and invitations go through it: open it and come back."));
        pane.finalize();
        return;
    }

    pane.add_section(state->me.empty() ? state->t("title", "Friends") : state->me);
    if (!state->notice.empty()) {
        pane.add_text(state->notice);
    }

    if (!mPendingId.empty()) {
        pane.add_text(state->t("pending", "Waiting for CubeShelf..."));
    }
    else {
        if (state->canHost) {
            pane.add_button(state->t("host", "Create a lobby and invite everyone"))
                .on_pressed([this] { act("host", "", true); });
        }
        if (state->hosting) {
            pane.add_button(state->t("cancel", "Withdraw the invitation"))
                .on_pressed([this] { act("cancel", "", false); });
        }
    }

    pane.add_section(state->t("friendsSection", "Friends"));
    if (state->friends.empty()) {
        pane.add_text(state->t("noFriends", "No friends yet. Add them from the Friends page in CubeShelf."));
    }

    const std::string separator = "  -  ";
    for (const auto &f : state->friends) {
        const std::string line = f.handle + separator + f.label;
        const bool reachable = f.status == "online" || f.status == "ingame";
        if (mPendingId.empty() && f.invitesYou) {
            pane.add_button(line + separator + state->t("join", "Join"))
                .on_pressed([this, key = f.key] { act("join", key, true); });
        }
        else if (mPendingId.empty() && state->canHost && reachable) {
            pane.add_button(line + separator + state->t("invite", "Invite"))
                .on_pressed([this, key = f.key] { act("invite", key, true); });
        }
        else {
            pane.add_text(line);
        }
    }

    if (native_mode()) {
        add_link_section(pane);
        pane.add_section("CubeShelf profile");
        pane.add_button("Refresh now").on_pressed([] { online::cubeshelf::refresh_now(); });
        pane.add_button("Forget this profile").on_pressed([this] {
            push(std::make_unique<Modal>(Modal::Props {
                .title = "Forget this profile",
                .bodyRml = "This device will no longer see your friends. Your PC and your friends are not affected; you can import the profile again at any time.",
                .actions = {
                    ModalAction {
                        .label = "Cancel",
                        .onPressed = [](Modal &modal) { modal.pop(); },
                    },
                    ModalAction {
                        .label = "Forget",
                        .onPressed = [this](Modal &modal) {
                            modal.pop();
                            online::cubeshelf::forget_profile();
                            refresh_active_tab();
                        },
                    },
                },
            }));
        });
    }

    pane.finalize();
}

void FriendsWindow::build_import(Rml::Element *content)
{
    auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
    auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

#ifdef __ANDROID__
    add_link_section(leftPane);
#endif
    leftPane.add_section("Import your CubeShelf profile");
    leftPane.register_control(leftPane.add_child<StringButton>(StringButton::Props {
                                  .key = "Passphrase",
                                  .getValue = [this] { return mPassphrase; },
                                  .setValue = [this](Rml::String value) { mPassphrase = std::move(value); },
                                  .maxLength = 256,
                                  .type = "password",
                                  .secret = true,
                              }),
        rightPane, [](Pane &pane) { pane.add_text("The passphrase you chose in CubeShelf when exporting the profile."); });
    leftPane.register_control(leftPane.add_button("Import from a file").on_pressed([] {
        partyboard::ShowFileSelect(&profile_picked, nullptr, aurora::window::get_sdl_window(), kProfileFilters,
            static_cast<int>(std::size(kProfileFilters)), nullptr, false);
    }),
        rightPane, [](Pane &pane) { pane.add_text("Choose the .cubeshelf-profile file exported by CubeShelf and copied to this device."); });
    leftPane.register_control(leftPane.add_button("Import from the clipboard").on_pressed([this] {
        char *text = SDL_HasClipboardText() ? SDL_GetClipboardText() : nullptr;
        std::string value = text != nullptr ? text : "";
        SDL_free(text);
        start_import(std::move(value), false);
    }),
        rightPane, [](Pane &pane) { pane.add_text("If you sent yourself the content of the file (it starts with CSP1-), copy it, then import it from here."); });

    leftPane.add_text("On your PC, open CubeShelf, then My profile, Play on a phone, and export your profile. This device will then see your friends and their invitations; your PC keeps publishing your presence.");
    mImportStatus = leftPane.add_text(mImportMessage);
    leftPane.finalize();
}

void FriendsWindow::add_link_section([[maybe_unused]] Pane &pane)
{
#ifdef __ANDROID__
    pane.add_section("Account and saves (QR code)");
    pane.add_text("In CubeShelf on your PC: My profile, Play on a phone, Show the QR code. Both devices must be on the same Wi-Fi.");
    pane.add_button("Scan: get my account and my saves").on_pressed([this] { start_link(0); });
    pane.add_button("Scan: send my saves to the PC").on_pressed([this] { start_link(1); });
    pane.add_button("Paste the code shown under the QR code").on_pressed([this] {
        char *text = SDL_HasClipboardText() ? SDL_GetClipboardText() : nullptr;
        std::string value = text != nullptr ? text : "";
        SDL_free(text);
        if (value.find("CSL1:") == std::string::npos) {
            set_link_message("The clipboard holds no CubeShelf code (it starts with CSL1:).");
            return;
        }
        mLinkCode = value;
        set_link_message("Code pasted. Choose what to do: get my account and my saves, or send my saves to the PC.");
    });
    mLinkStatus = pane.add_text(mLinkMessage);
#endif
}

void FriendsWindow::set_link_message(std::string message)
{
    mLinkMessage = std::move(message);
    if (mLinkStatus != nullptr) {
        mLinkStatus->SetInnerRML(escape(ui_translate(mLinkMessage)));
    }
}

void FriendsWindow::start_link([[maybe_unused]] int mode)
{
#ifdef __ANDROID__
    if (mLinkActive) {
        return;
    }
    bool started = false;
    const bool fr = french();
    const std::string code = std::exchange(mLinkCode, {});
    android::with_activity("startCubeShelfLink", "(ILjava/lang/String;Z)Z", [&](JNIEnv *env, jobject activity, jmethodID method) {
        jstring text = env->NewStringUTF(code.c_str());
        started = env->CallBooleanMethod(activity, method, static_cast<jint>(mode), text, fr ? JNI_TRUE : JNI_FALSE) == JNI_TRUE;
        env->DeleteLocalRef(text);
    });
    if (!started) {
        set_link_message("The QR code transfer could not start.");
        return;
    }
    mLinkActive = true;
    set_link_message(mode == 0 ? "Getting your account and your saves from the PC..." : "Sending your saves to the PC...");
#endif
}

void FriendsWindow::poll_link()
{
#ifdef __ANDROID__
    if (!mLinkActive) {
        return;
    }
    std::string raw;
    android::with_activity("pollCubeShelfLink", "()Ljava/lang/String;", [&](JNIEnv *env, jobject activity, jmethodID method) {
        auto value = static_cast<jstring>(env->CallObjectMethod(activity, method));
        raw = android::to_string(env, value);
        if (value != nullptr) {
            env->DeleteLocalRef(value);
        }
    });
    const auto result = nlohmann::json::parse(raw, nullptr, false);
    if (result.is_discarded() || !result.is_object()) {
        return;
    }
    const std::string state = result.value("state", "");
    if (state == "scanning" || state == "working" || state == "idle") {
        return;
    }
    mLinkActive = false;
    const std::string message = result.value("message", "");
    if (state == "error") {
        set_link_message(message.empty() ? "The transfer failed." : message);
        return;
    }
    if (result.value("mode", 0) == 1) {
        set_link_message(message.empty() ? "Saves sent to the PC." : message);
        return;
    }
    // Mode 0: the account comes in now; the saves at the next start.
    std::string imported = "Your account is on this device.";
    if (result.contains("profile") && result["profile"].is_string()) {
        const auto error = online::cubeshelf::import_from_document(result["profile"].get<std::string>());
        if (error != online::cubeshelf::ImportError::None) {
            imported = online::cubeshelf::import_error_message(error);
        }
    }
    const int saves = result.value("saves", 0);
    if (saves <= 0) {
        set_link_message(imported + " " + ui_translate("The PC had no Mario Party 4 save to send."));
        refresh_active_tab();
        return;
    }
    push(std::make_unique<Modal>(Modal::Props {
        .title = "CubeShelf",
        .bodyRml = escape(ui_translate(imported) + " "
            + fmt::format(fmt::runtime(ui_translate("{} save file(s) received from the PC. They replace this device's at the next start (the old ones are kept in save-backups).")), saves)),
        .actions = {
            ModalAction {
                .label = "Later",
                .onPressed = [this](Modal &modal) {
                    modal.pop();
                    refresh_active_tab();
                },
            },
            ModalAction {
                .label = "Restart now",
                .onPressed = [](Modal &modal) {
                    modal.pop();
                    android::with_activity("restartGame", "()V", [](JNIEnv *env, jobject activity, jmethodID method) {
                        env->CallVoidMethod(activity, method);
                    });
                    PartyBoard_IsRunning = false;
                },
            },
        },
    }));
#endif
}

struct FriendsWindow::ImportJob {
    std::atomic<bool> done = false;
    online::cubeshelf::ImportError error = online::cubeshelf::ImportError::None;
};

void FriendsWindow::set_import_message(std::string message)
{
    mImportMessage = std::move(message);
    if (mImportStatus != nullptr) {
        mImportStatus->SetInnerRML(escape(ui_translate(mImportMessage)));
    }
}

void FriendsWindow::start_import(std::string source, bool fromFile)
{
    if (mImport) {
        return;
    }
    if (source.empty()) {
        set_import_message(fromFile ? "No file was chosen." : "The clipboard is empty.");
        return;
    }
    if (mPassphrase.empty()) {
        set_import_message("Type the passphrase first.");
        return;
    }
    set_import_message("Importing...");
    // PBKDF2 takes about a second on a phone: never on the frame.
    auto job = std::make_shared<ImportJob>();
    mImport = job;
    std::thread([job, source = std::move(source), passphrase = mPassphrase, fromFile] {
        job->error = fromFile ? online::cubeshelf::import_from_path(source, passphrase) : online::cubeshelf::import_from_text(source, passphrase);
        job->done = true;
    }).detach();
}

void FriendsWindow::act(const std::string &action, const std::string &key, bool closesGame)
{
    if (native_mode()) {
        // Nothing to close here: the lobby opens inside the game, on the invitation.
        if (action == "join") {
            if (const auto payload = online::cubeshelf::join_payload(key)) {
#ifdef __ANDROID__
                // The phone's lobby is its own screen, which restarts the game for the session.
                if (open_android_lobby(*payload)) {
                    PartyBoard_IsRunning = false;
                    return;
                }
#endif
                push(std::make_unique<OnlineWindow>(*payload));
            }
            else {
                push_toast({
                    .type = "error",
                    .title = "Invitation",
                    .content = "That invitation is no longer valid: the lobby was closed or has expired.",
                    .duration = std::chrono::seconds(6),
                });
                refresh_active_tab();
            }
        }
        return;
    }

    if (!closesGame) {
        send(action, key, false);
        return;
    }

    // Online play starts both games together from the companion, so hosting or joining ends this
    // one. That is said before it happens, never discovered after.
    const auto state = read_state();
    const auto t = [&](const char *k, const char *fallback) { return state ? state->t(k, fallback) : std::string(fallback); };
    push(std::make_unique<Modal>(Modal::Props {
        .title = escape(t("closeTitle", "Mario Party 4 will close")),
        .bodyRml = escape(t("closeBody", "Online play starts Mario Party 4 again on both PCs, from the lobby. Unsaved progress in this game will be lost.")),
        .actions = {
            ModalAction {
                .label = t("cancelButton", "Cancel"),
                .onPressed = [](Modal &modal) { modal.pop(); },
            },
            ModalAction {
                .label = t("continueButton", "Continue"),
                .onPressed = [this, action, key](Modal &modal) {
                    modal.pop();
                    send(action, key, true);
                },
            },
        },
    }));
}

void FriendsWindow::send(const std::string &action, const std::string &key, bool closesGame)
{
    const auto &dir = directory();
    if (!dir) {
        return;
    }

    std::error_code ec;
    const fs::path requests = *dir / "requests";
    fs::create_directories(requests, ec);

    const std::string id = new_request_id();
    const nlohmann::json request = { { "schema", 1 }, { "id", id }, { "action", action }, { "key", key } };
    if (!write_atomically(requests / (id + ".json"), request.dump())) {
        const auto state = read_state();
        push_toast({
            .type = "error",
            .title = as_toast_text(state ? state->t("inviteTitle", "Invitation") : std::string("Invitation")),
            .content = as_toast_text(state ? state->t("writeFailed", "The request could not be written for CubeShelf.") : std::string("The request could not be written for CubeShelf.")),
            .duration = std::chrono::seconds(5),
        });
        return;
    }

    mPendingId = id;
    mPendingCloses = closesGame;
    mPendingSince = clock::now();
    refresh_active_tab();
}

void FriendsWindow::update()
{
    const auto now = clock::now();

    if (native_mode()) {
        poll_link();
        std::optional<std::string> picked;
        {
            std::lock_guard lock(sPickedMutex);
            picked.swap(sPickedProfile);
        }
        if (picked) {
            start_import(std::move(*picked), true);
        }
        if (mImport && mImport->done) {
            const auto error = mImport->error;
            mImport.reset();
            if (error == online::cubeshelf::ImportError::None) {
                mPassphrase.clear();
                mImportMessage.clear();
                mImportStatus = nullptr;
                refresh_active_tab();
            }
            else {
                set_import_message(online::cubeshelf::import_error_message(error));
            }
        }
    }

    if (!mPendingId.empty()) {
        const auto &dir = directory();
        const fs::path reply = dir ? *dir / "requests" / (mPendingId + ".done") : fs::path();
        std::error_code ec;
        if (dir && fs::exists(reply, ec)) {
            bool ok = false;
            std::string message;
            if (const auto raw = read_file(reply, 16 * 1024)) {
                try {
                    const auto json = nlohmann::json::parse(*raw, nullptr, false);
                    if (!json.is_discarded() && json.is_object()) {
                        ok = flag_of(json, "ok");
                        message = text_of(json, "message");
                    }
                }
                catch (...) {
                    ok = false;
                }
            }
            fs::remove(reply, ec);

            const bool closes = mPendingCloses;
            mPendingId.clear();
            if (!ok || !message.empty()) {
                const auto state = read_state();
                push_toast({
                    .type = ok ? "" : "error",
                    .title = as_toast_text(state ? state->t("inviteTitle", "Invitation") : std::string("Invitation")),
                    .content = as_toast_text(message.empty() ? std::string("CubeShelf could not do it.") : message),
                    .duration = std::chrono::seconds(6),
                });
            }
            if (ok && closes) {
                // The companion is opening the lobby; this game gives way to the one it will start,
                // exactly as the "Play Online" tab already does.
                PartyBoard_IsRunning = false;
                return;
            }
            refresh_active_tab();
        }
        else if (now - mPendingSince > kReplyTimeout) {
            mPendingId.clear();
            const auto state = read_state();
            push_toast({
                .type = "error",
                .title = as_toast_text(state ? state->t("inviteTitle", "Invitation") : std::string("Invitation")),
                .content = as_toast_text(state ? state->t("noAnswer", "CubeShelf did not answer. Is it still open?") : std::string("CubeShelf did not answer. Is it still open?")),
                .duration = std::chrono::seconds(6),
            });
            refresh_active_tab();
        }
    }
    else if (now - mLastRead > std::chrono::seconds(2)) {
        mLastRead = now;
        const auto state = read_state();
        const long long revision = state ? state->revision : -1;
        const bool fresh = state && state->fresh;
        // Rebuilt only when something a player would see has changed. The file is rewritten every
        // few seconds as a heartbeat, and rebuilding on each one would steal the focus from
        // whoever is choosing a friend.
        if (revision != mRevision || fresh != mFresh) {
            refresh_active_tab();
        }
    }

    Window::update();
}

} // namespace partyboard::ui::cubeshelf
