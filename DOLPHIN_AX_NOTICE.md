# Dolphin AX HLE reference notice

The experimental Windows audio backend in
`extern/musyx/src/musyx/runtime/hw_pc.c` was validated against Dolphin's AX
HLE implementation, in particular its GameCube resampling fallback and its
two-stage signed-envelope/unsigned-mixer saturation behavior.

Reference revision: Dolphin `1fd7f3521895f285aa9382af8e7e464991437225`.

Upstream source: <https://github.com/dolphin-emu/dolphin>

Dolphin is licensed under GPL-2.0-or-later. This experimental integration is
currently intended for local testing only and must receive a complete license
review before any binary or source distribution.
