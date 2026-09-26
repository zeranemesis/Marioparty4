#ifndef PARTYBOARD_PORT_TEST_INPUT_H
#define PARTYBOARD_PORT_TEST_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

// Applies scripted pad input when PARTYBOARD_TEST_INPUT is set (src/port/test_input.cpp).
// Called once per frame; does nothing otherwise.
void PartyBoard_TestInputFrame(void);

#ifdef __cplusplus
}
#endif

#endif
