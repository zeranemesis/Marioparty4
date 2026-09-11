#ifndef _PORT_AUDIO_LIFETIME_H
#define _PORT_AUDIO_LIFETIME_H

/* Lifetime instrumentation for MusyX sample banks and the voices that read them.
 *
 * Why this exists. D3 is an invalid read inside the ADPCM decoder, on the MusyX
 * audio thread, always on the exact frame an overlay is unloaded. The suspicion
 * is a bank freed by the game thread while a voice still points into it. This
 * module exists to turn that suspicion into a dated, named observation, or to
 * refute it.
 *
 * It answers exactly one question:
 *
 *     at the moment a sample bank is freed, does any live DSP voice still hold
 *     a sample address inside that bank?
 *
 * Two detectors, because they fail in different places and one alone is not
 * enough:
 *
 *   1. At the free. Every bank is registered with its address range, a bank id
 *      and a generation. Just before free(), every DSP voice is walked and any
 *      voice whose smp_info.addr lands inside the range being freed is reported.
 *      This fires on the game thread, synchronously, BEFORE the memory goes
 *      away, so it produces proof without needing the crash to happen.
 *
 *   2. At the read. Freed ranges are kept in a retired table with their bank id
 *      and generation. The audio thread checks the address it is about to
 *      dereference against that table, so a read that would otherwise be a bare
 *      access violation at a random address becomes a named event: which bank,
 *      which generation, which voice, which simulation frame.
 *
 * Detector 1 is the important one. A crash is a probabilistic consequence of the
 * defect; a live voice pointing into a bank being freed IS the defect.
 *
 * Nothing here changes what the game does. Every hook is a call that returns
 * immediately when the module is off. Off by default:
 * PARTYBOARD_AUDIO_DIAGNOSTICS=1 turns it on.
 */

/* Self-contained on purpose: this header is included both by the port, which
 * has the Dolphin types, and by MusyX, which has its own. Fixed-width types
 * belong to neither and are understood by both. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Master switch, read once from the environment. */
bool PartyBoard_AudioLifetimeEnabled(void);

/* ---- bank events -------------------------------------------------------- */

/* A group is being pushed. Opens a naming context: every sample stored until
 * PartyBoard_AudioBankVisible closes it belongs to this bank and generation.
 * `base`/`size` describe the staging blob the samples are copied FROM; the
 * addresses voices actually read are the per-sample copies reported by
 * PartyBoard_AudioSampleStore, which is why the two are tracked separately. */
void PartyBoard_AudioBankLoad(const void *sdir, const void *base, size_t size, int32_t groupId);

/* The group has been inserted: from here the synthesiser can start voices on it,
 * and no further sample belongs to this push. */
void PartyBoard_AudioBankVisible(const void *sdir);

/* ---- sample events ------------------------------------------------------ */

/* One sample has been copied into the emulated ARAM. `addr` is the exact
 * pointer a voice will later carry in smp_info.addr, so it is the only address
 * that can be matched against a voice. Called from hwSaveSample, where the
 * decoded length is already known. */
void PartyBoard_AudioSampleStore(const void *addr, size_t length);

/* The sample id behind an address, learned one call later than the address
 * itself because hwSaveSample does not see it. */
void PartyBoard_AudioSampleIdentify(const void *addr, uint32_t sampleId);

/* A sample copy is about to be freed. This is detector 1: the voices are walked
 * here, while the memory is still mapped, and any voice still carrying this
 * address is reported. Called from hwRemoveSample, immediately before free(). */
void PartyBoard_AudioSampleFree(const void *addr);

/* sndPopGroup has been entered for this group. The voices are about to be asked
 * to stop; the memory is still valid at this point. */
void PartyBoard_AudioBankReleaseRequest(const void *sdir, int32_t groupId);

/* HuAudSndGrpWait is about to drain the audio state before replacing banks. */
void PartyBoard_AudioDrainBegin(int32_t steps);
void PartyBoard_AudioDrainEnd(void);

/* The last moment at which the bank is still mapped. Walks the voices and
 * reports every one that still references this range. This is detector 1. */
void PartyBoard_AudioBankFree(const void *sdir, const void *base);

/* ---- voice events ------------------------------------------------------- */

void PartyBoard_AudioVoiceStart(uint32_t voice, const void *addr, uint32_t smpId,
    uint8_t compType, uint32_t length);
void PartyBoard_AudioVoiceStopRequest(uint32_t voice);
void PartyBoard_AudioVoiceStopped(uint32_t voice, const char *why);

/* ---- audio thread ------------------------------------------------------- */

/* One audio interrupt has begun. Supplies the callback counter that every event
 * is stamped with, so a game-thread event and an audio-thread event can be
 * ordered against each other. */
void PartyBoard_AudioIrqTick(void);

/* Called on the audio thread immediately BEFORE dereferencing `addr` for
 * `voice`. Detector 2. Cheap on the hot path: a cached compare per voice. */
void PartyBoard_AudioNoteSampleRead(uint32_t voice, const void *addr);

/* ---- voice enumeration, supplied by the audio backend ------------------- */

/* Implemented in hw_pc.c, where dspVoice and salNumVoices are visible, so this
 * module never needs its own copy of the voice layout. Mirrors the arrangement
 * already used for the HuMem heap walk. */
typedef void (*PartyBoardAudioVoiceSink)(void *context, uint32_t voice, uint32_t state,
    const void *addr, uint32_t smpId, uint8_t compType, uint32_t length, uint32_t posHi);
void PartyBoard_AudioWalkVoices(PartyBoardAudioVoiceSink sink, void *context);

/* Same shape, so the self-test can substitute a set of voices it chose itself. */
typedef void (*PartyBoardAudioVoiceWalk)(PartyBoardAudioVoiceSink sink, void *context);

/* ---- results ------------------------------------------------------------ */

/* True once a live voice has been seen referencing a bank at its free, or once
 * the audio thread has been caught reading a retired bank. */
bool PartyBoard_AudioLifetimeViolationDetected(void);

void PartyBoard_AudioLifetimeStats(uint32_t *banksLoaded, uint32_t *banksFreed,
    uint32_t *staleAtFree, uint32_t *staleReads, uint32_t *voicesStarted);

/* Suppresses report files while the detector is being tested against known
 * stale references, so a self-test never litters a session directory. */
void PartyBoard_AudioLifetimeSetQuiet(bool quiet);

bool PartyBoard_AudioLifetimeRunSelfTest(void);

#ifdef __cplusplus
}
#endif

#endif
