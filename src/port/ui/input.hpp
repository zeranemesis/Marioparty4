// Credits: TwilitRealm

#pragma once

union SDL_Event;

namespace partyboard::ui::input {

void handle_event(const SDL_Event &event) noexcept;
void update_input() noexcept;
void reset_input_state() noexcept;
void sync_input_block() noexcept;
void release_input_block() noexcept;
// Opens the Party Board menu, as F1, the Back button or a three-finger tap does.
void open_menu() noexcept;

} // namespace partyboard::ui
