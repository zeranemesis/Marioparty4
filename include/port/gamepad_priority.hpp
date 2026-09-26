#pragma once

// Phones and tablets: a real gamepad takes player 1 as soon as it connects, ahead of the screen
// controller, and devices that only look like gamepads (keyboards, remotes) take no port at all.
namespace partyboard::input {

// Reorders the connected gamepads. Called at start and whenever one connects or disconnects;
// does nothing on desktop, or once the player has assigned ports in the controller settings.
void apply_gamepad_priority() noexcept;

} // namespace partyboard::input
