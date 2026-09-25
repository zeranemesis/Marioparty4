#include "online.hpp"

#include "../file_select.hpp"
#include "localization.hpp"
#include "pane.hpp"
#include "port/config.hpp"
#include "port/online/invitation.hpp"
#include "prelaunch.hpp"
#include "string_button.hpp"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_stdinc.h>
#include <fmt/format.h>

#include <ctime>

namespace partyboard::ui {
namespace {

    constexpr int kNicknameMaxLength = 24;
    // Same bound as the companion's invitation box.
    constexpr int kInvitationMaxLength = 220;

    // Mirrors PlayerInfo.CleanName: 1 to 24 characters, no control character.
    bool valid_nickname(const std::string &name)
    {
        if (name.empty()) {
            return false;
        }
        std::size_t characters = 0;
        for (const unsigned char c : name) {
            if (c < 0x20 || c == 0x7f) {
                return false;
            }
            if ((c & 0xC0) != 0x80) {
                ++characters;
            }
        }
        return characters <= kNicknameMaxLength;
    }

    std::string trimmed(std::string text)
    {
        const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        while (!text.empty() && isSpace(text.back())) {
            text.pop_back();
        }
        std::size_t start = 0;
        while (start < text.size() && isSpace(text[start])) {
            ++start;
        }
        return text.substr(start);
    }

    std::string nickname()
    {
        const auto &value = getSettings().online.nickname.getValue();
        return value.empty() ? ui_translate("Player") : value;
    }

    std::string disc_label()
    {
        const auto &state = prelaunch_state();
        if (state.configuredDiscPath.empty()) {
            return "(none)";
        }
        auto display = display_name_for_path(state.configuredDiscPath);
        return display.empty() ? state.configuredDiscPath : display;
    }

    bool disc_ready()
    {
        const auto &state = prelaunch_state();
        return !state.configuredDiscPath.empty() && state.configuredDiscCanLaunch;
    }

} // namespace

OnlineWindow::OnlineWindow()
{
    ensure_initialized();
    mStatus = disc_ready() ? "Disc ready. Create a lobby or paste your friend's invitation." : "Choose your disc to get started.";
    add_tab("Online Lobby", [this](Rml::Element *content) { build(content); });
}

void OnlineWindow::build(Rml::Element *content)
{
    auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
    auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

    leftPane.add_section("Your Party Board lobby");
    leftPane.register_control(leftPane.add_child<StringButton>(StringButton::Props {
                                  .key = "Nickname",
                                  .getValue = [] { return nickname(); },
                                  .setValue =
                                      [this](Rml::String value) {
                                          value = trimmed(std::move(value));
                                          if (!valid_nickname(value)) {
                                              set_status("Choose a nickname of 1 to 24 characters, on a single line.");
                                              return;
                                          }
                                          getSettings().online.nickname.setValue(value);
                                          config::Save();
                                      },
                                  .maxLength = kNicknameMaxLength,
                              }),
        rightPane, [](Pane &pane) { pane.add_text("The name the other players see in the lobby."); });

    leftPane.register_control(leftPane
                                  .add_select_button({
                                      .key = "Disc Image",
                                      .getValue = [] { return disc_label(); },
                                  })
                                  .on_pressed([] { open_iso_picker(); }),
        rightPane, [](Pane &pane) {
            pane.add_text("Every player needs the same disc image: Mario Party 4 USA. The whole file is checked before joining.");
        });

    leftPane.register_control(leftPane.add_select_button({
                                  .key = "Players",
                                  .getValue = [this] { return std::to_string(mPlayers); },
                              }),
        rightPane, [this](Pane &pane) {
            for (int count = 2; count <= online::kMaxSeats; ++count) {
                pane.add_button({
                                    .text = std::to_string(count),
                                    .isSelected = [this, count] { return mPlayers == count; },
                                })
                    .on_pressed([this, count] { mPlayers = count; });
            }
            pane.add_text("How many players the lobby waits for, you included. Only the host chooses.");
        });

    leftPane.register_control(leftPane.add_button("Create a lobby").on_pressed([this] { create_lobby(); }), rightPane,
        [](Pane &pane) { pane.add_text("Create the lobby, then send the invitation to your friends. Only you can start the game."); });

    leftPane.add_section("Join a lobby");
    leftPane.register_control(leftPane.add_child<StringButton>(StringButton::Props {
                                  .key = "Invitation",
                                  .getValue = [this] { return mInvitation; },
                                  .setValue = [this](Rml::String value) { mInvitation = trimmed(std::move(value)); },
                                  .maxLength = kInvitationMaxLength,
                              }),
        rightPane, [](Pane &pane) { pane.add_text("The invitation starts with PB4. The host copies it from their lobby, on a PC or a phone."); });
    leftPane.register_control(leftPane.add_button("Paste invitation").on_pressed([this] { paste_invitation(); }), rightPane,
        [](Pane &pane) { pane.add_text("Paste the invitation your friend sent you."); });
    leftPane.register_control(leftPane.add_button("Join").on_pressed([this] { join_lobby(); }), rightPane,
        [](Pane &pane) { pane.add_text("Join your friend's lobby. The host starts the game for everyone."); });

    leftPane.add_section("Lobby");
    leftPane.register_control(leftPane.add_button("Leave lobby").on_pressed([this] { leave_lobby(); }), rightPane,
        [](Pane &pane) { pane.add_text("Leave the lobby and clear the invitation."); });
    mStatusElem = leftPane.add_text(mStatus);

    leftPane.finalize();
}

void OnlineWindow::set_status(std::string text)
{
    mStatus = std::move(text);
    if (mStatusElem != nullptr) {
        mStatusElem->SetInnerRML(escape(ui_translate(mStatus)));
    }
}

void OnlineWindow::create_lobby()
{
    if (!disc_ready()) {
        set_status("Choose your disc to get started.");
        return;
    }
    // The lobby itself (TLS control channel, router opening, relay) is being
    // ported from tools/online; until it lands this window says so rather than
    // pretending a lobby exists.
    set_status("Hosting from this device is not available yet: the online connection is still being ported. Create the lobby on a PC for now.");
}

void OnlineWindow::join_lobby()
{
    if (mInvitation.empty()) {
        set_status("Paste your friend's invitation first.");
        return;
    }
    const auto decoded = online::decode_invitation(mInvitation, static_cast<std::uint32_t>(std::time(nullptr)));
    if (!decoded.invitation) {
        set_status(online::invitation_error_message(decoded.error));
        return;
    }
    if (!disc_ready()) {
        set_status("Choose your disc to get started.");
        return;
    }
    const auto &invite = *decoded.invitation;
    mStatus = fmt::format(fmt::runtime(ui_translate("Valid invitation for a lobby of {} players.")), invite.maxPlayers) + " "
        + ui_translate("Joining from this device is not available yet: the online connection is still being ported.");
    if (mStatusElem != nullptr) {
        mStatusElem->SetInnerRML(escape(mStatus));
    }
}

void OnlineWindow::paste_invitation()
{
    if (!SDL_HasClipboardText()) {
        set_status("The clipboard is empty.");
        return;
    }
    char *text = SDL_GetClipboardText();
    if (text == nullptr) {
        set_status("The clipboard is empty.");
        return;
    }
    std::string value = trimmed(text);
    SDL_free(text);
    if (value.size() > static_cast<std::size_t>(kInvitationMaxLength)) {
        value.resize(kInvitationMaxLength);
    }
    mInvitation = std::move(value);
    set_status(mInvitation.empty() ? "The clipboard is empty." : "Invitation pasted. Press Join.");
}

void OnlineWindow::leave_lobby()
{
    mInvitation.clear();
    set_status(disc_ready() ? "Disc ready. Create a lobby or paste your friend's invitation." : "Choose your disc to get started.");
}

} // namespace partyboard::ui
