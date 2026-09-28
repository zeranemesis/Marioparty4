#pragma once

#include <cstdint>
#include <vector>

// The screen's refresh rate, where the platform decides it for the app (Android keeps games at
// 60 Hz unless they ask). Elsewhere the monitor runs at its own rate and these do nothing.
namespace partyboard::display {

// Refresh rates the screen offers at its current resolution, rounded; empty when unknown.
std::vector<int> supported_refresh_rates();

// Actual XR display rate, or zero outside an active headset session.
int headset_frame_rate();

// On the headset, while it shows the game's world: when the game's next frame
// should start (steady-clock nanoseconds), on the display's schedule, so every
// image is shown for the same number of display frames. Zero: keep the game's
// own clock.
int64_t headset_frame_start(int64_t nowNs, int targetFps);

// Ask the screen for at least this rate (60 or less: let the system choose).
void request_frame_rate(int fps);

} // namespace partyboard::display
