#pragma once

#include <cstdint>

namespace partyboard::ra {

// The rcheevos memory callback: copies numBytes of emulated GameCube RAM at
// address (an offset from 0x80000000) into buffer and returns how many bytes it
// could provide. Fewer than numBytes means the address is not translated.
uint32_t readMemory(uint32_t address, uint8_t* buffer, uint32_t numBytes);

// Marks the start of a game tick, so the console images are rebuilt from the
// variables' current values on their next read.
void beginTick();

// Logs every address the set asked for that could not be translated.
void logUnmappedAddresses();

} // namespace partyboard::ra
