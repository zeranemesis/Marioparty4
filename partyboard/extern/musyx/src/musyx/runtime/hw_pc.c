#include "musyx/platform.h"

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_timer.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "musyx/adsr.h"
#include "musyx/assert.h"
#include "musyx/hardware.h"
#include "musyx/musyx.h"
#include "musyx/sal.h"
#include "musyx/synth.h"

/* The PC THP player shares this MusyX/SDL buffer so transition audio uses the
 * same clock as the rest of the game. */
extern void HuTHPPCM16Mix(s16* destination, u32 frames);

// Audio parameters
#define SAL_SAMPLES_PER_FRAME 160
#define SAL_SUBFRAMES 5
#define SAL_SAMPLES_PER_SUBFRAME (SAL_SAMPLES_PER_FRAME / SAL_SUBFRAMES)
#define SAL_STEREO_SAMPLES (SAL_SAMPLES_PER_FRAME * 2)
#define SAL_BUFFER_BYTES (SAL_STEREO_SAMPLES * sizeof(s16))
#define SAL_PREFILL_FRAMES 8
#define SAL_MAX_QUEUED_FRAMES 12
/* MusyX voice gains already target signed 16-bit output; do not attenuate twice. */
#define SAL_OUTPUT_SHIFT 0
#define SRC_STAGING_SIZE 256
#define POLYPHASE_FILTERS 3
#define POLYPHASE_PHASES 128
#define POLYPHASE_TAPS 4
#define SINC_PHASES 512
#define SINC_TAPS 8
#define SINC_HALF_TAPS (SINC_TAPS / 2)

// Mode 0: GC polyphase, mode 1: windowed sinc
#ifndef SAL_RESAMPLE_MODE
#define SAL_RESAMPLE_MODE 0
#endif

#if SAL_RESAMPLE_MODE != 0 && SAL_RESAMPLE_MODE != 1
#error "SAL_RESAMPLE_MODE must be 0 (GC polyphase) or 1 (windowed sinc)"
#endif

// Double buffer for output
static s16 salOutputBuffers[2][SAL_STEREO_SAMPLES];
static u8 salOutputIndex = 0;

// SDL3 audio
static SDL_AudioStream* salAudioStream = NULL;
static SDL_Thread* salAudioThread = NULL;
static SDL_AtomicInt salAudioThreadRunning;
static SDL_Mutex* globalMutex;
static FILE* salAudioDump = NULL;
static FILE* salAudioTimingDiagnostic = NULL;
static FILE* salVoiceTrace = NULL;
u32 salAudioDumpFrameCount = 0;
static u32 salVoiceTraceFrameLimit = 2000;
static int salVoiceTraceVoice = -1;
static FILE* salRenderDiagnostic = NULL;

/* A marker file lets automated UI tests enable diagnostics even when the
 * launcher cannot inherit custom environment variables.  It is checked once
 * during audio startup and is inert in normal builds when the file is absent. */
static const char* salDiagnosticUserPath(const char* filename, char* path, size_t pathSize) {
  const char* appData = getenv("APPDATA");
  if (appData == NULL || appData[0] == '\0')
    return filename;
  snprintf(path, pathSize, "%s\\MarioPartyRD\\Party Board\\%s", appData, filename);
  return path;
}

static bool salDiagnosticsMarkerEnabled(void) {
  char markerPath[1024];
  FILE* marker = fopen(salDiagnosticUserPath("audio_diagnostics.enable", markerPath, sizeof(markerPath)), "rb");
  if (marker == NULL) {
    marker = fopen("audio_diagnostics.enable", "rb");
    if (marker == NULL)
      return false;
  }
  fclose(marker);
  return true;
}

static uint64_t salFrameDecodedAbs = 0;
extern u32 missingSampleDiagnosticCount;
extern u32 startedSampleDiagnosticCount;
extern uint64_t startedSampleLagSum;
extern u32 startedSampleLagMax;
static u32 salDiagStartupBreak;
static u32 salDiagSetupDone;
static u32 salDiagSampleDone;
static u32 salDiagAdsrDone;
static u32 salDiagPostBreak;
static u32 salDiagVoiceStarts;
static u32 salDiagKeyOffs;
static u32 salDiagAdsrReleaseDone;
static u32 salDiagAdsrNaturalDone;
static uint64_t salDiagAdsrAttackSum;
static uint64_t salDiagAdsrDecaySum;
static uint64_t salDiagAdsrSustainSum;
static uint64_t salDiagAdsrReleaseSum;
uint64_t salDiagPitchSum;
uint64_t salDiagPitchHash = 1469598103934665603ULL;
u32 salDiagPitchMin = 0xffffffff;
u32 salDiagPitchMax;
static uint64_t salDiagMidiDecodedAbs[16];
static uint64_t salDiagMidiDryAbs[16];
static uint64_t salDiagMidiAuxAAbs[16];
static uint64_t salDiagMidiAuxBAbs[16];
static uint64_t salDiagMidiSegments[16];
static uint64_t salDiagTrackDecodedAbs[256];
static uint64_t salDiagTrackDryAbs[256];
static uint64_t salDiagTrackAuxAAbs[256];
static uint64_t salDiagTrackAuxBAbs[256];
static uint64_t salDiagTrackSegments[256];

static SND_SOME_CALLBACK userCallback = NULL;

// ADPCM decode state per voice
static s16 adpcmYn1[SYNTH_MAX_VOICES];
static s16 adpcmYn2[SYNTH_MAX_VOICES];
// Cached decoded ADPCM block per voice
static s32 adpcmBlockCache[SYNTH_MAX_VOICES][14];
static u32 adpcmCachedBlock[SYNTH_MAX_VOICES]; // block index currently cached, ~0u = invalid

typedef struct VoiceResamplerState {
  s32 srcBuf[SRC_STAGING_SIZE];
  u32 srcCount;
  u32 srcConsumed;
  u32 curPos;
  u32 srcPosHi;
  s16 lastSamples[POLYPHASE_TAPS];
  u8 ended;
} VoiceResamplerState;

static VoiceResamplerState voiceResampler[SYNTH_MAX_VOICES];
static s16 polyphaseTable[POLYPHASE_FILTERS][POLYPHASE_PHASES][POLYPHASE_TAPS];
static s16 sincTable[SINC_PHASES][SINC_TAPS];
static u8 resampleTablesInitialized = 0;
static u8 dspPolyphaseAvailable = 0;

typedef enum SalAudioBackend {
  SAL_AUDIO_BACKEND_DOLPHIN_AX,
  SAL_AUDIO_BACKEND_LEGACY
} SalAudioBackend;

/* The Dolphin AX path preserves the two 16-bit saturation stages used by the
 * GameCube DSP.  Keep the old combined multiply available for controlled A/B
 * captures through PARTYBOARD_AUDIO_BACKEND=legacy. */
static SalAudioBackend salAudioBackend = SAL_AUDIO_BACKEND_DOLPHIN_AX;

enum {
  MIX_BUS_L,
  MIX_BUS_R,
  MIX_BUS_S,
  MIX_BUS_AUX_AL,
  MIX_BUS_AUX_AR,
  MIX_BUS_AUX_AS,
  MIX_BUS_AUX_BL,
  MIX_BUS_AUX_BR,
  MIX_BUS_AUX_BS,
  MIX_BUS_COUNT
};

typedef struct VolumeRamp {
  s32 start;
  s32 delta;
  s32 end;
  u16 stopSample;
} VolumeRamp;

static s32 voiceLastMix[SYNTH_MAX_VOICES][MIX_BUS_COUNT];
static u32 salTargetVoiceStartDiagnosticCount;
static u32 salTargetVoiceKeyOffDiagnosticCount;
/* Index 160 carries a stop at the exact end of the current frame into the
 * next one.  Events are accumulated here and rendered once, chronologically. */
static s32 depopEvents[SAL_MAX_STUDIONUM][MIX_BUS_COUNT][SAL_SAMPLES_PER_FRAME + 1];
static int depopRenderOffset = -1;

// Mix accumulation buffers
static s32 mixBufferL[SAL_SAMPLES_PER_FRAME];
static s32 mixBufferR[SAL_SAMPLES_PER_FRAME];
static s32 mixBufferS[SAL_SAMPLES_PER_FRAME];
static s32 delayedSurround[SAL_SAMPLES_PER_FRAME];
static s16 dolphinOutputHistory[2];
static u8 salOutputColorationEnabled = 0;
#if MUSY_VERSION >= MUSY_VERSION_CHECK(2, 0, 1)
static s32 filterState[SYNTH_MAX_VOICES];
#endif

#pragma pack(push, 1)
typedef struct SalVoiceTraceHeader {
  char magic[8];
  u32 version;
  u32 recordSize;
  u32 sampleRate;
  u32 samplesPerSegment;
} SalVoiceTraceHeader;

typedef struct SalVoiceTraceRecord {
  u32 frame;
  u16 voice;
  u8 studio;
  u8 subframe;
  u8 compType;
  u8 srcType;
  u8 coefSelect;
  u8 filterEnabled;
  u8 midi;
  u8 track;
  u16 reserved;
  u32 pitch;
  u32 sourcePositionBefore;
  u32 sourcePositionAfter;
  u32 sourceFractionBefore;
  u32 sourceFractionAfter;
  u16 adsrStart;
  s16 adsrDelta;
  u16 volumes[MIX_BUS_COUNT];
  s16 resampled[SAL_SAMPLES_PER_SUBFRAME];
  s16 filtered[SAL_SAMPLES_PER_SUBFRAME];
  s16 enveloped[SAL_SAMPLES_PER_SUBFRAME];
} SalVoiceTraceRecord;
#pragma pack(pop)

static inline s16 clamp16(s32 v) {
  if (v > 32767)
    return 32767;
  if (v < -32768)
    return -32768;
  return (s16)v;
}

static inline s16 clampCoeff(s32 v) {
  if (v > 32767)
    return 32767;
  if (v < -32768)
    return -32768;
  return (s16)v;
}

static void applyOptionalOutputColoration(s16* samples) {
  /* This measured coloration is useful for A/B tests against a captured
   * host-output stream, but it is not part of Dolphin's AX ucode emulation.
   * Keep it opt-in so the default Dolphin AX path remains bit-faithful at the
   * mixer boundary. */
  for (u32 frame = 0; frame < SAL_SAMPLES_PER_FRAME; ++frame) {
    for (u32 channel = 0; channel < 2; ++channel) {
      const s32 input = samples[frame * 2 + channel];
      const s32 smoothed = (input * 4 + dolphinOutputHistory[channel]) / 5;
      const s32 scaled = smoothed * 30802;
      const s32 rounded = scaled >= 0 ? scaled + 16384 : scaled - 16384;
      dolphinOutputHistory[channel] = (s16)input;
      samples[frame * 2 + channel] = clamp16(rounded / 32768);
    }
  }
}

static u32 parsePositiveEnvironmentValue(const char* name, u32 fallback) {
  const char* value = getenv(name);
  if (value == NULL || value[0] == '\0')
    return fallback;

  char* end = NULL;
  const unsigned long parsed = strtoul(value, &end, 10);
  if (end == value || *end != '\0' || parsed == 0 || parsed > UINT32_MAX)
    return fallback;
  return (u32)parsed;
}

static void writeVoiceTraceRecord(const DSPvoice* vp, u32 voiceIdx, int frameOffset,
                                  int frameSamples, u32 pitch, u32 sourcePositionBefore,
                                  u32 sourceFractionBefore, u16 adsrStart, s16 adsrDelta,
                                  const VolumeRamp ramps[MIX_BUS_COUNT], const s16* resampled,
                                  const s16* enveloped, const s16* filtered) {
  if (salVoiceTrace == NULL || frameSamples != SAL_SAMPLES_PER_SUBFRAME ||
      salAudioDumpFrameCount >= salVoiceTraceFrameLimit ||
      (salVoiceTraceVoice >= 0 && voiceIdx != (u32)salVoiceTraceVoice)) {
    return;
  }

  SalVoiceTraceRecord record;
  memset(&record, 0, sizeof(record));
  record.frame = salAudioDumpFrameCount;
  record.voice = (u16)voiceIdx;
  record.studio = vp->studio;
  record.subframe = (u8)(frameOffset / SAL_SAMPLES_PER_SUBFRAME);
  record.compType = vp->smp_info.compType;
  record.srcType = (u8)vp->srcTypeSelect;
  record.coefSelect = vp->srcCoefSelect;
#if MUSY_VERSION >= MUSY_VERSION_CHECK(2, 0, 1)
  record.filterEnabled = vp->filter.on != 0;
#else
  record.filterEnabled = 0;
#endif
  record.reserved = vp->streamLoopPS;
  if (synthVoice != NULL && voiceIdx < salNumVoices) {
    record.midi = synthVoice[voiceIdx].midi;
    record.track = synthVoice[voiceIdx].track;
  } else {
    record.midi = 0xFF;
    record.track = 0xFF;
  }
  record.pitch = pitch;
  record.sourcePositionBefore = sourcePositionBefore;
  record.sourcePositionAfter = voiceResampler[voiceIdx].srcPosHi;
  record.sourceFractionBefore = sourceFractionBefore;
  record.sourceFractionAfter = voiceResampler[voiceIdx].curPos;
  record.adsrStart = adsrStart;
  record.adsrDelta = adsrDelta;

  for (u32 bus = 0; bus < MIX_BUS_COUNT; ++bus)
    record.volumes[bus] = (u16)ramps[bus].start;

  for (int i = 0; i < frameSamples; ++i) {
    record.resampled[i] = resampled[i];
    record.filtered[i] = filtered[i];
    record.enveloped[i] = enveloped[i];
  }

  fwrite(&record, sizeof(record), 1, salVoiceTrace);
}

static inline double sincUnit(double x) {
  if (SDL_fabs(x) < 1e-12)
    return 1.0;

  const double pix = x * SDL_PI_D;
  return SDL_sin(pix) / pix;
}

static double besselI0(double x) {
  const double halfSquared = (x * x) * 0.25;
  double sum = 1.0;
  double term = 1.0;

  for (u32 k = 1; k < 32; ++k) {
    term *= halfSquared / ((double)k * (double)k);
    sum += term;
    if (term <= sum * 1e-15)
      break;
  }

  return sum;
}

static u8 loadDspPolyphaseTable(void) {
  enum {
    COEFFICIENT_COUNT = POLYPHASE_FILTERS * POLYPHASE_PHASES * POLYPHASE_TAPS,
    DSP_COEFFICIENT_COUNT = 0x800
  };
  u8 raw[DSP_COEFFICIENT_COUNT * sizeof(s16)];
  FILE* file = fopen("dsp_coef.bin", "rb");

  if (file == NULL)
    return FALSE;

  const size_t bytesRead = fread(raw, 1, sizeof(raw), file);
  const int hasTrailingByte = fgetc(file) != EOF;
  fclose(file);
  if (bytesRead != sizeof(raw) || hasTrailingByte)
    return FALSE;

  for (u32 filter = 0; filter < POLYPHASE_FILTERS; ++filter) {
    for (u32 phase = 0; phase < POLYPHASE_PHASES; ++phase) {
      for (u32 tap = 0; tap < POLYPHASE_TAPS; ++tap) {
        const u32 index = (filter * POLYPHASE_PHASES * POLYPHASE_TAPS) +
                          (phase * POLYPHASE_TAPS) + tap;
        const u16 value = ((u16)raw[index * 2] << 8) | raw[index * 2 + 1];
        polyphaseTable[filter][phase][tap] = (s16)value;
      }
    }
  }

  fprintf(stderr, "[MusyX] Loaded GameCube DSP resampling coefficients.\n");
  return TRUE;
}

static void initResampleTables(void) {
  if (resampleTablesInitialized)
    return;

  dspPolyphaseAvailable = loadDspPolyphaseTable();
  if (!dspPolyphaseAvailable) {
    /* Dolphin deliberately falls back from AX polyphase to AX linear when the
     * copyrighted DSP DROM table is unavailable.  Fabricated FIR coefficients
     * sound different from the console and were the source of audible ringing. */
    fprintf(stderr,
            "[MusyX] dsp_coef.bin missing or invalid; using Dolphin AX linear fallback.\n");
  }

  for (u32 phase = 0; phase < SINC_PHASES; ++phase) {
    const double frac = (double)phase / (double)SINC_PHASES;
    double taps[SINC_TAPS];
    double sum = 0.0;

    for (u32 tap = 0; tap < SINC_TAPS; ++tap) {
      const double x = (double)((s32)tap - (SINC_HALF_TAPS - 1)) - frac;
      double value = 0.0;

      if (SDL_fabs(x) < (double)SINC_HALF_TAPS)
        value = sincUnit(x) * sincUnit(x / (double)SINC_HALF_TAPS);

      taps[tap] = value;
      sum += value;
    }

    if (SDL_fabs(sum) < 1e-12) {
      memset(sincTable[phase], 0, sizeof(sincTable[phase]));
      sincTable[phase][SINC_HALF_TAPS - 1] = 0x7FFF;
      continue;
    }

    const double scale = 32767.0 / sum;
    s32 accum = 0;
    for (u32 tap = 0; tap < SINC_TAPS - 1; ++tap) {
      const s16 q = clampCoeff((s32)SDL_lround(taps[tap] * scale));
      sincTable[phase][tap] = q;
      accum += q;
    }

    sincTable[phase][SINC_TAPS - 1] = clampCoeff(32767 - accum);
  }

  resampleTablesInitialized = 1;
}

static VolumeRamp prepareVolumeRamp(u16 lastVol, u16 targetVol) {
  VolumeRamp ramp;
  const s32 start = lastVol;
  const s32 difference = (s16)targetVol - (s16)lastVol;

  ramp.start = start;
  ramp.delta = difference / SAL_SAMPLES_PER_FRAME;
  ramp.stopSample = SAL_SAMPLES_PER_FRAME;
  ramp.end = start + ramp.delta * SAL_SAMPLES_PER_FRAME;

  /* Match the DSP's small-ramp handling.  A plain integer division leaves
   * |difference| < 160 stuck forever, most audibly just above zero. */
  if (difference >= SAL_SAMPLES_PER_SUBFRAME && difference < SAL_SAMPLES_PER_FRAME) {
    ramp.delta = 1;
    ramp.stopSample = (u16)((difference >> 5) * SAL_SAMPLES_PER_SUBFRAME);
    ramp.end = start + ramp.stopSample;
  } else if (difference <= -SAL_SAMPLES_PER_SUBFRAME &&
             difference > -SAL_SAMPLES_PER_FRAME) {
    ramp.delta = -1;
    ramp.stopSample = (u16)(((-difference) >> 5) * SAL_SAMPLES_PER_SUBFRAME);
    ramp.end = start - ramp.stopSample;
  } else if (targetVol == 0 && difference < 0 && difference > -SAL_SAMPLES_PER_SUBFRAME) {
    ramp.start = 0;
    ramp.delta = 0;
    ramp.stopSample = 0;
    ramp.end = 0;
  }

  return ramp;
}

static inline void mixRampChannel(s32* dest, const s32* src, int nSamples,
                                  const VolumeRamp* ramp, int frameOffset,
                                  s32* lastContribution,
                                  uint64_t* absoluteContribution) {
  *lastContribution = 0;
  if (dest == NULL || (ramp->start == 0 && ramp->delta == 0))
    return;

  for (int i = 0; i < nSamples; ++i) {
    const u16 absoluteSample = (u16)(frameOffset + i);
    const u16 rampSample = absoluteSample < ramp->stopSample ? absoluteSample : ramp->stopSample;
    const u16 vol = (u16)(ramp->start + ramp->delta * rampSample);
    s32 contribution;

    if (salAudioBackend == SAL_AUDIO_BACKEND_DOLPHIN_AX) {
      /* The signed envelope and optional filter have already been applied in
       * AXVoice::ProcessVoice order. MixAdd now applies only the unsigned bus
       * volume and saturates before adding to the 32-bit studio accumulator. */
      contribution = clamp16((s32)(((int64_t)src[i] * vol) >> 15));
    } else {
      contribution = (src[i] * (s32)vol) >> 15;
    }
    dest[i] += contribution;
    *lastContribution = contribution;
    if (absoluteContribution != NULL)
      *absoluteContribution +=
          contribution < 0 ? (uint64_t)-(int64_t)contribution : (uint64_t)contribution;
  }
}

static void applyVoiceEnvelope(s32* samples, int count, u16 start, s16 delta) {
  u16 current = start;
  for (int i = 0; i < count; ++i) {
    /* AX GC treats the envelope accumulator as a wrapping u16 register, then
     * interprets that register as signed for the multiply.  Clamping the
     * mathematically expanded value changes attacks/releases that cross a
     * register boundary and can leave quiet voices effectively inaudible. */
    const s32 volume = (s16)current;
    samples[i] = clamp16((s32)(((int64_t)samples[i] * volume) >> 15));
    current = (u16)(current + (u16)delta);
  }
}

static s32* getStudioBusBuffer(DSPstudioinfo* stp, u32 bus) {
  if (bus < 3) {
    s32* main = stp->main[salFrame];
    return main != NULL ? main + bus * SAL_SAMPLES_PER_FRAME : NULL;
  }
  if (bus < 6) {
    s32* auxA = stp->auxA[salAuxFrame];
    return auxA != NULL ? auxA + (bus - 3) * SAL_SAMPLES_PER_FRAME : NULL;
  }

  s32* auxB = stp->auxB[salAuxFrame];
  return auxB != NULL ? auxB + (bus - 6) * SAL_SAMPLES_PER_FRAME : NULL;
}

static s32* getStudioDepopSum(DSPstudioinfo* stp, u32 bus) {
  switch (bus) {
  case MIX_BUS_L:
    return &stp->hostDPopSum.l;
  case MIX_BUS_R:
    return &stp->hostDPopSum.r;
  case MIX_BUS_S:
    return &stp->hostDPopSum.s;
  case MIX_BUS_AUX_AL:
    return &stp->hostDPopSum.lA;
  case MIX_BUS_AUX_AR:
    return &stp->hostDPopSum.rA;
  case MIX_BUS_AUX_AS:
    return &stp->hostDPopSum.sA;
  case MIX_BUS_AUX_BL:
    return &stp->hostDPopSum.lB;
  case MIX_BUS_AUX_BR:
    return &stp->hostDPopSum.rB;
  default:
    return &stp->hostDPopSum.sB;
  }
}

static s32 clampDepopSum(s32 value) {
  if (value > 0x7fffff)
    return 0x7fffff;
  if (value < -0x7fffff)
    return -0x7fffff;
  return value;
}

static void scheduleStudioDepop(DSPstudioinfo* stp, u32 bus, int offset, s32 contribution) {
  if (contribution == 0)
    return;

  const u32 studio = (u32)(stp - dspStudio);
  offset = CLAMP(offset, 0, SAL_SAMPLES_PER_FRAME);
  depopEvents[studio][bus][offset] =
      clampDepopSum(depopEvents[studio][bus][offset] + contribution);
}

static s32 calculateDepopDelta(s32 value) {
  /* Fade every non-zero residue all the way to silence.  Truncating the
   * division left values in ]-160, 160[ with a zero delta forever, producing
   * a DC-like buzz after voices stopped.  A ceiling division also guarantees
   * that the tail reaches (or crosses) zero within one 5 ms DSP frame. */
  if (value < 0) {
    const s32 magnitude = -value;
    return magnitude >= 3200 ? 20 : (magnitude + SAL_SAMPLES_PER_FRAME - 1) /
                                             SAL_SAMPLES_PER_FRAME;
  }
  if (value > 0)
    return value >= 3200 ? -20 : -(value + SAL_SAMPLES_PER_FRAME - 1) /
                                             SAL_SAMPLES_PER_FRAME;
  return 0;
}

static void renderStudioDepop(DSPstudioinfo* stp) {
  const u32 studio = (u32)(stp - dspStudio);

  for (u32 bus = 0; bus < MIX_BUS_COUNT; ++bus) {
    s32* dest = getStudioBusBuffer(stp, bus);
    s32* sum = getStudioDepopSum(stp, bus);
    s32 current = *sum;
    s32 delta = calculateDepopDelta(current);

    for (int sample = 0; sample < SAL_SAMPLES_PER_FRAME; ++sample) {
      const s32 event = depopEvents[studio][bus][sample];
      if (event != 0) {
        current = clampDepopSum(current + event);
        delta = calculateDepopDelta(current);
        depopEvents[studio][bus][sample] = 0;
      }
      if (dest != NULL)
        dest[sample] += current;

      current += delta;
      if (current == 0 || (delta < 0 && current < 0) || (delta > 0 && current > 0)) {
        current = 0;
        delta = 0;
      }
    }

    /* A stop at sample 160 begins at sample zero of the next frame. */
    current = clampDepopSum(current + depopEvents[studio][bus][SAL_SAMPLES_PER_FRAME]);
    depopEvents[studio][bus][SAL_SAMPLES_PER_FRAME] = 0;
    *sum = current;
  }
}

void salPCDepopVoice(DSPvoice* vp) {
  if (vp == NULL || dspVoice == NULL || vp < dspVoice || vp >= dspVoice + salNumVoices ||
      vp->studio >= salMaxStudioNum) {
    return;
  }

  const u32 voiceIdx = (u32)(vp - dspVoice);
  DSPstudioinfo* stp = &dspStudio[vp->studio];
  const int offset = depopRenderOffset >= 0 ? depopRenderOffset : 0;
  for (u32 bus = 0; bus < MIX_BUS_COUNT; ++bus) {
    scheduleStudioDepop(stp, bus, offset, voiceLastMix[voiceIdx][bus]);
    voiceLastMix[voiceIdx][bus] = 0;
  }
}

static void deactivateVoiceAtOffset(DSPvoice* vp, int offset) {
  depopRenderOffset = offset;
  salDeactivateVoice(vp);
  depopRenderOffset = -1;
}

static inline void addThreeChannelBuffer(s32* dst, const s32* src, u16 vol) {
  if (dst == NULL || src == NULL || vol == 0)
    return;

  for (int i = 0; i < SAL_SAMPLES_PER_FRAME * 3; ++i)
    dst[i] += (src[i] * vol) >> 15;
}

static void foldStereoToOutput(const s32* left, const s32* right) {
  for (int i = 0; i < SAL_SAMPLES_PER_FRAME; ++i) {
    mixBufferL[i] += left[i];
    mixBufferR[i] += right[i];
  }
}

/* Add one L/R/S bus to the DSP master buffers. SET_OPPOSITE_LR initializes
 * L/R from the previous frame's saved S channel; OUTPUT saves this frame's S
 * for the next command list. The one-frame delay and signs are part of the
 * original Dolby Surround matrix. */
static void mixLrsToOutput(const s32* left, const s32* right, const s32* surround) {
  for (int i = 0; i < SAL_SAMPLES_PER_FRAME; ++i) {
    mixBufferL[i] += left[i];
    mixBufferR[i] += right[i];
    if (surround != NULL)
      mixBufferS[i] += surround[i];
  }
}

/*
 * Decode an ADPCM block (8 bytes -> 14 samples) and update history. A loop may
 * resume in the middle of a block, in which case the DSP starts at firstSample
 * with the loop predictor/scale and histories instead of replaying the block's
 * preceding nibbles.
 */
static void decodeADPCMBlock(const u8* blockData, const s16 coefTab[8][2], s16* yn1, s16* yn2,
                             s32* out, u32 firstSample, u8 ps) {
  int pred = (ps >> 4) & 0x7;
  int scale = 1 << (ps & 0xF);
  s16 c1 = coefTab[pred][0];
  s16 c2 = coefTab[pred][1];
  s16 y1 = *yn1, y2 = *yn2;

  memset(out, 0, 14 * sizeof(*out));
  for (u32 s = firstSample; s < 14; s++) {
    int nibble;
    if (s % 2 == 0) {
      nibble = (blockData[1 + s / 2] >> 4) & 0xF;
    } else {
      nibble = blockData[1 + s / 2] & 0xF;
    }
    if (nibble >= 8)
      nibble -= 16;
    /* The GameCube DSP rounds the Q11 prediction before shifting. */
    s32 decoded = (s32)(((int64_t)nibble * scale * 2048 + (int64_t)c1 * y1 +
                         (int64_t)c2 * y2 + 1024) >>
                        11);
    decoded = clamp16(decoded);
    y2 = y1;
    y1 = (s16)decoded;
    out[s] = decoded;
  }
  *yn1 = y1;
  *yn2 = y2;
}

static void ensureADPCMBlockDecoded(SAMPLE_INFO* smp, u32 voiceIdx, u32 blockIdx,
                                    const s16 coefTab[8][2]) {
  if (adpcmCachedBlock[voiceIdx] == blockIdx)
    return;

  u32 startBlock = blockIdx;
  if (adpcmCachedBlock[voiceIdx] != ~0u && adpcmCachedBlock[voiceIdx] < blockIdx)
    startBlock = adpcmCachedBlock[voiceIdx] + 1;

  for (u32 b = startBlock; b <= blockIdx; ++b) {
    const u8* blockData = (const u8*)smp->addr + b * 8;
    decodeADPCMBlock(blockData, coefTab, &adpcmYn1[voiceIdx], &adpcmYn2[voiceIdx],
                     adpcmBlockCache[voiceIdx], 0, blockData[0]);
  }

  adpcmCachedBlock[voiceIdx] = blockIdx;
}

static const s16 zeroCoefTab[8][2] = {{0}};

static inline int isVoiceADPCM(u8 compType) {
  return compType == 0 || compType == 1 || compType == 4 || compType == 5;
}

static const s16 (*getVoiceCoefTab(SAMPLE_INFO* smp))[2] {
  if (!isVoiceADPCM(smp->compType))
    return NULL;

  DSPADPCMplusInfo* adpcmInfo = smp->extraData;
  return adpcmInfo ? adpcmInfo->coefTab : zeroCoefTab;
}

static void resetVoiceLoopState(DSPvoice* vp, u32 voiceIdx, const s16 coefTab[8][2]) {
  SAMPLE_INFO* smp = &vp->smp_info;

  if (!isVoiceADPCM(smp->compType))
    return;

  DSPADPCMplusInfo* adpcmInfo = smp->extraData;
  if (smp->compType == 0 || smp->compType == 1) {
    if (adpcmInfo != NULL) {
      adpcmYn1[voiceIdx] = adpcmInfo->loopY1;
      adpcmYn2[voiceIdx] = adpcmInfo->loopY0;
    } else {
      adpcmYn1[voiceIdx] = 0;
      adpcmYn2[voiceIdx] = 0;
    }
  }

  /*
   * A DSP loop address is expressed in samples and can point inside a 14-sample
   * ADPCM block. Prime exactly the remainder of that block from the saved loop
   * history. Decoding from nibble zero here corrupts every loop transition.
   */
  const u32 blockIdx = smp->loop / 14;
  const u32 firstSample = smp->loop % 14;
  const u8* blockData = (const u8*)smp->addr + blockIdx * 8;
  const u8 loopPS = (smp->compType == 4 || smp->compType == 5)
                        ? vp->streamLoopPS
                        : adpcmInfo != NULL ? adpcmInfo->loopPS : blockData[0];
  decodeADPCMBlock(blockData, coefTab, &adpcmYn1[voiceIdx], &adpcmYn2[voiceIdx],
                   adpcmBlockCache[voiceIdx], firstSample, loopPS);
  adpcmCachedBlock[voiceIdx] = blockIdx;
}

static void updateCurrentAddr(DSPvoice* vp, u32 srcPosHi) {
  SAMPLE_INFO* smp = &vp->smp_info;

  switch (smp->compType) {
  case 0:
  case 1:
  case 4:
  case 5:
    vp->currentAddr = (u32)((uintptr_t)smp->addr * 2 + (srcPosHi / 14) * 16 + 2 + (srcPosHi % 14));
    break;
  case 2:
#if MUSY_VERSION >= MUSY_VERSION_CHECK(2, 0, 2)
  case 6:
#endif
    vp->currentAddr = (u32)((uintptr_t)smp->addr / 2 + srcPosHi);
    break;
  case 3:
    vp->currentAddr = (u32)((uintptr_t)smp->addr + srcPosHi);
    break;
  default:
    break;
  }
}

static s32 sampleAtPos(SAMPLE_INFO* smp, u32 voiceIdx, u32 posHi, const s16 coefTab[8][2]) {
  switch (smp->compType) {
  case 0:
  case 1:
  case 4:
  case 5: {
    u32 blockIdx = posHi / 14;
    ensureADPCMBlockDecoded(smp, voiceIdx, blockIdx, coefTab);
    return adpcmBlockCache[voiceIdx][posHi % 14];
  }
  case 2:
#if MUSY_VERSION >= MUSY_VERSION_CHECK(2, 0, 2)
  case 6:
#endif
    return ((s16*)smp->addr)[posHi];
  case 3:
    return ((s32)((u8*)smp->addr)[posHi] - 128) << 8;
  default:
    return 0;
  }
}

/*
 * Decode sequential source-rate samples into a staging buffer.
 */
static int decodeSourceSamples(DSPvoice* vp, u32 voiceIdx, s32* out, int maxSamples, int* hitEnd) {
  SAMPLE_INFO* smp = &vp->smp_info;
  VoiceResamplerState* state = &voiceResampler[voiceIdx];
  const s16(*coefTab)[2] = getVoiceCoefTab(smp);
  int count = 0;

  *hitEnd = 0;

  for (; count < maxSamples; ++count) {
    u32 playbackEnd = smp->length;
    if (smp->loopLength > 0 && smp->loop < smp->length &&
        smp->loopLength <= smp->length - smp->loop) {
      playbackEnd = smp->loop + smp->loopLength;
    }

    if (state->srcPosHi >= playbackEnd) {
      if (smp->loopLength > 0 && playbackEnd > smp->loop) {
        state->srcPosHi = smp->loop + ((state->srcPosHi - playbackEnd) % smp->loopLength);
        resetVoiceLoopState(vp, voiceIdx, coefTab);
      } else {
        *hitEnd = 1;
        state->ended = 1;
        break;
      }
    }

    out[count] = sampleAtPos(smp, voiceIdx, state->srcPosHi, coefTab);
    ++state->srcPosHi;
  }

  for (int i = count; i < maxSamples; ++i)
    out[i] = 0;

  return count;
}

static int fillSourceBuffer(DSPvoice* vp, u32 voiceIdx, int outputSamples, u32 pitch,
                            u16 srcType) {
  VoiceResamplerState* state = &voiceResampler[voiceIdx];
  const u32 history = state->srcConsumed < SINC_HALF_TAPS ? state->srcConsumed : SINC_HALF_TAPS;
  const u32 available = state->srcCount - state->srcConsumed;
  u32 needed;

  /* AX SRC type 2 bypasses rate conversion and consumes exactly one source
   * sample per output sample.  Sizing this buffer from pitch starves those
   * voices whenever pitch is below 1.0, producing short repeated fragments. */
  if (srcType == 2) {
    needed = (u32)outputSamples;
  } else {
    needed = (u32)(((u64)outputSamples * pitch + 0xFFFFu) >> 16);
  }

  /* Dolphin's AX ResampleAudio pulls source samples only when curr_pos crosses
   * 1.0.  The GC polyphase and linear paths therefore need no look-ahead.
   * Decoding five samples early advanced hwGetPos ahead of audible playback,
   * which could make the stream producer recycle a ring-buffer region before
   * the corresponding AX boundary.  Only the optional windowed-sinc build
   * needs future taps in its staging buffer. */
#if SAL_RESAMPLE_MODE == 1
  if (srcType != 2)
    needed += SINC_HALF_TAPS + 1;
#endif
  if (available >= needed || state->ended)
    return state->ended;

  if (available + history > 0) {
    memmove(state->srcBuf, state->srcBuf + state->srcConsumed - history,
            (available + history) * sizeof(state->srcBuf[0]));
  }

  state->srcCount = available + history;
  state->srcConsumed = history;

  u32 targetCount = history + needed;
  if (targetCount > SRC_STAGING_SIZE)
    targetCount = SRC_STAGING_SIZE;

  if (targetCount > state->srcCount) {
    int hitEnd = 0;
    state->srcCount += (u32)decodeSourceSamples(vp, voiceIdx, state->srcBuf + state->srcCount,
                                                (int)(targetCount - state->srcCount), &hitEnd);
  }

  updateCurrentAddr(vp, state->srcPosHi);
  return state->ended;
}

static inline s32 readSourceBufferSample(const VoiceResamplerState* state, s32 idx) {
  if (idx < 0 || (u32)idx >= state->srcCount)
    return 0;

  return state->srcBuf[idx];
}

static int resampleVoice(u32 voiceIdx, s32* out, int numSamples, u32 pitch, u16 srcType) {
  VoiceResamplerState* state = &voiceResampler[voiceIdx];

  if (srcType > 2)
    srcType = 0;

#if SAL_RESAMPLE_MODE == 0
  if (srcType == 0 && dspPolyphaseAvailable) {
  s16 temp[POLYPHASE_TAPS];
  u32 idx = 0;
  const u32 filter = dspVoice[voiceIdx].srcCoefSelect < POLYPHASE_FILTERS
                         ? dspVoice[voiceIdx].srcCoefSelect
                         : 1;

  temp[idx++ & 3] = state->lastSamples[0];
  temp[idx++ & 3] = state->lastSamples[1];
  temp[idx++ & 3] = state->lastSamples[2];
  temp[idx++ & 3] = state->lastSamples[3];

  for (int i = 0; i < numSamples; ++i) {
    state->curPos += pitch;
    while (state->curPos >= 0x10000) {
      s32 sample = 0;
      if (state->srcConsumed < state->srcCount)
        sample = state->srcBuf[state->srcConsumed++];

      temp[idx++ & 3] = clamp16(sample);
      state->curPos -= 0x10000;
    }

    const u32 phase = (state->curPos & 0xFFFF) >> 9;
    const s16* c = polyphaseTable[filter][phase];
    const long long t0 = temp[idx++ & 3];
    const long long t1 = temp[idx++ & 3];
    const long long t2 = temp[idx++ & 3];
    const long long t3 = temp[idx++ & 3];
    const long long sample = (t0 * c[0] + t1 * c[1] + t2 * c[2] + t3 * c[3]) >> 15;

    out[i] = clamp16((s32)sample);
  }

  state->lastSamples[3] = temp[--idx & 3];
  state->lastSamples[2] = temp[--idx & 3];
  state->lastSamples[1] = temp[--idx & 3];
  state->lastSamples[0] = temp[--idx & 3];
  } else if (srcType == 0 || srcType == 1) {
    s16 temp[POLYPHASE_TAPS];
    u32 idx = 0;

    temp[idx++ & 3] = state->lastSamples[0];
    temp[idx++ & 3] = state->lastSamples[1];
    temp[idx++ & 3] = state->lastSamples[2];
    temp[idx++ & 3] = state->lastSamples[3];

    for (int i = 0; i < numSamples; ++i) {
      state->curPos += pitch;
      while (state->curPos >= 0x10000) {
        s32 sample = 0;
        if (state->srcConsumed < state->srcCount)
          sample = state->srcBuf[state->srcConsumed++];

        temp[idx++ & 3] = clamp16(sample);
        state->curPos -= 0x10000;
      }

      const u16 fraction = (u16)state->curPos;
      if (fraction != 0) {
        const u16 inverseFraction = (u16)-fraction;
        const s32 sample0 = temp[idx++ & 3];
        const s32 sample1 = temp[idx++ & 3];
        out[i] = (s16)((sample0 * inverseFraction + sample1 * fraction) >> 16);
        idx += 2;
      } else {
        out[i] = temp[idx++ & 3];
        idx += 3;
      }
    }

    state->lastSamples[3] = temp[--idx & 3];
    state->lastSamples[2] = temp[--idx & 3];
    state->lastSamples[1] = temp[--idx & 3];
    state->lastSamples[0] = temp[--idx & 3];
  } else {
    for (int i = 0; i < numSamples; ++i) {
      s32 sample = 0;
      if (state->srcConsumed < state->srcCount)
        sample = state->srcBuf[state->srcConsumed++];
      out[i] = clamp16(sample);
    }

    if (numSamples >= POLYPHASE_TAPS) {
      for (u32 i = 0; i < POLYPHASE_TAPS; ++i)
        state->lastSamples[i] = (s16)out[numSamples - POLYPHASE_TAPS + i];
    }
  }
#else
  if (srcType == 2) {
    for (int i = 0; i < numSamples; ++i) {
      s32 sample = 0;
      if (state->srcConsumed < state->srcCount)
        sample = state->srcBuf[state->srcConsumed++];
      out[i] = clamp16(sample);
    }
  } else {
    for (int i = 0; i < numSamples; ++i) {
      const u32 phase = (state->curPos & 0xFFFF) >> 7;
      const s16* c = sincTable[phase];
      long long sample = 0;

      for (u32 tap = 0; tap < SINC_TAPS; ++tap) {
        const s32 srcIdx = (s32)state->srcConsumed + (s32)tap - (SINC_HALF_TAPS - 1);
        sample += (long long)readSourceBufferSample(state, srcIdx) * c[tap];
      }

      out[i] = clamp16((s32)(sample >> 15));

      const u32 step = state->curPos + pitch;
      state->srcConsumed += step >> 16;
      state->curPos = step & 0xFFFF;
    }
  }
#endif

  return numSamples;
}

/*
 * Software voice rendering for one frame (SAL_SAMPLES_PER_FRAME samples).
 * Decodes samples, then mixes into main and AUX buffers with per-channel volumes.
 */
static s32 voiceDecodeBuf[SAL_SAMPLES_PER_FRAME];

// Returns 1 if voice finished playing (end of non-looping sample), 0 otherwise.
static int renderVoiceSegment(DSPvoice* vp, s32* mainL, s32* mainR, s32* mainS, s32* auxAL,
                              s32* auxAR, s32* auxAS, s32* auxBL, s32* auxBR, s32* auxBS,
                              u32 voiceIdx, int frameOffset, int frameSamples, u16 adsrStart,
                              s16 adsrDelta, const VolumeRamp ramps[MIX_BUS_COUNT]) {
  if (vp->state == 0)
    return 0;

  const SAMPLE_INFO* smp = &vp->smp_info;
  if (smp->addr == NULL)
    return 0;

  u32 pitch = vp->playInfo.pitch;
  if (pitch == 0)
    pitch = vp->pitch[0];
  if (pitch == 0)
    return 0;

  /* Decode samples into temp buffer */
  VoiceResamplerState* state = &voiceResampler[voiceIdx];
  const u32 sourcePositionBefore = state->srcPosHi;
  const u32 sourceFractionBefore = state->curPos;
  const int traceThisSegment =
      salVoiceTrace != NULL && frameSamples == SAL_SAMPLES_PER_SUBFRAME &&
      salAudioDumpFrameCount < salVoiceTraceFrameLimit &&
      (salVoiceTraceVoice < 0 || voiceIdx == (u32)salVoiceTraceVoice);
  s16 resampledTrace[SAL_SAMPLES_PER_SUBFRAME];
  s16 envelopedTrace[SAL_SAMPLES_PER_SUBFRAME];
  s16 filteredTrace[SAL_SAMPLES_PER_SUBFRAME];
  fillSourceBuffer(vp, voiceIdx, frameSamples, pitch, vp->srcTypeSelect);
  int nSamples =
      resampleVoice(voiceIdx, voiceDecodeBuf, frameSamples, pitch, vp->srcTypeSelect);
  if (traceThisSegment) {
    for (int i = 0; i < nSamples; ++i)
      resampledTrace[i] = clamp16(voiceDecodeBuf[i]);
  }
  uint64_t decodedAbs = 0;
  if (salRenderDiagnostic != NULL) {
    for (int i = 0; i < nSamples; ++i) {
      const uint64_t sampleAbs = voiceDecodeBuf[i] < 0 ? (uint64_t)-(int64_t)voiceDecodeBuf[i]
                                                      : (uint64_t)voiceDecodeBuf[i];
      decodedAbs += sampleAbs;
      salFrameDecodedAbs += sampleAbs;
    }
  }
  vp->playInfo.posHi = state->srcPosHi;
  vp->playInfo.posLo = state->curPos;
  int voiceDone = state->ended && state->srcConsumed >= state->srcCount;

  /* AX GC applies the signed volume envelope before its optional low-pass
   * filter.  Filtering the full-volume waveform and applying ADSR afterwards
   * gives the filter the wrong history, which progressively changes or loses
   * filtered instruments as notes attack and release. */
  applyVoiceEnvelope(voiceDecodeBuf, nSamples, adsrStart, adsrDelta);
  if (traceThisSegment) {
    for (int i = 0; i < nSamples; ++i)
      envelopedTrace[i] = clamp16(voiceDecodeBuf[i]);
  }

#if MUSY_VERSION >= MUSY_VERSION_CHECK(2, 0, 1)
  if (vp->filter.on) {
    s32 yn = filterState[voiceIdx];
    for (int i = 0; i < nSamples; ++i) {
      /* Dolphin's AX LowPassFilter saturates every result before feeding it
       * back as history.  Keeping an unclamped 32-bit history can run away
       * and produce harsh or missing filtered instruments. */
      yn = clamp16(((s32)vp->filter.coefA * voiceDecodeBuf[i] +
                    (s32)vp->filter.coefB * yn) >>
                   15);
      voiceDecodeBuf[i] = yn;
    }
    filterState[voiceIdx] = yn;
  }
#endif

  if (traceThisSegment) {
    for (int i = 0; i < nSamples; ++i)
      filteredTrace[i] = clamp16(voiceDecodeBuf[i]);
    writeVoiceTraceRecord(vp, voiceIdx, frameOffset, frameSamples, pitch,
                          sourcePositionBefore, sourceFractionBefore, adsrStart, adsrDelta,
                          ramps, resampledTrace, envelopedTrace, filteredTrace);
  }

  s32* lastMix = voiceLastMix[voiceIdx];
  uint64_t dryAbs = 0;
  uint64_t auxAAbs = 0;
  uint64_t auxBAbs = 0;
  uint64_t* dryDiagnostic = salRenderDiagnostic != NULL ? &dryAbs : NULL;
  uint64_t* auxADiagnostic = salRenderDiagnostic != NULL ? &auxAAbs : NULL;
  uint64_t* auxBDiagnostic = salRenderDiagnostic != NULL ? &auxBAbs : NULL;
  mixRampChannel(mainL ? mainL + frameOffset : NULL, voiceDecodeBuf, nSamples,
                 &ramps[MIX_BUS_L], frameOffset,
                 &lastMix[MIX_BUS_L], dryDiagnostic);
  mixRampChannel(mainR ? mainR + frameOffset : NULL, voiceDecodeBuf, nSamples,
                 &ramps[MIX_BUS_R], frameOffset,
                 &lastMix[MIX_BUS_R], dryDiagnostic);
  mixRampChannel(mainS ? mainS + frameOffset : NULL, voiceDecodeBuf, nSamples,
                 &ramps[MIX_BUS_S], frameOffset,
                 &lastMix[MIX_BUS_S], dryDiagnostic);
  mixRampChannel(auxAL ? auxAL + frameOffset : NULL, voiceDecodeBuf, nSamples,
                 &ramps[MIX_BUS_AUX_AL], frameOffset,
                 &lastMix[MIX_BUS_AUX_AL], auxADiagnostic);
  mixRampChannel(auxAR ? auxAR + frameOffset : NULL, voiceDecodeBuf, nSamples,
                 &ramps[MIX_BUS_AUX_AR], frameOffset,
                 &lastMix[MIX_BUS_AUX_AR], auxADiagnostic);
  mixRampChannel(auxAS ? auxAS + frameOffset : NULL, voiceDecodeBuf, nSamples,
                 &ramps[MIX_BUS_AUX_AS], frameOffset,
                 &lastMix[MIX_BUS_AUX_AS], auxADiagnostic);
  mixRampChannel(auxBL ? auxBL + frameOffset : NULL, voiceDecodeBuf, nSamples,
                 &ramps[MIX_BUS_AUX_BL], frameOffset,
                 &lastMix[MIX_BUS_AUX_BL], auxBDiagnostic);
  mixRampChannel(auxBR ? auxBR + frameOffset : NULL, voiceDecodeBuf, nSamples,
                 &ramps[MIX_BUS_AUX_BR], frameOffset,
                 &lastMix[MIX_BUS_AUX_BR], auxBDiagnostic);
  mixRampChannel(auxBS ? auxBS + frameOffset : NULL, voiceDecodeBuf, nSamples,
                 &ramps[MIX_BUS_AUX_BS], frameOffset,
                 &lastMix[MIX_BUS_AUX_BS], auxBDiagnostic);

  if (salRenderDiagnostic != NULL && synthVoice != NULL && voiceIdx < salNumVoices) {
    const SYNTH_VOICE* svoice = &synthVoice[voiceIdx];
    const u8 midi = svoice->midi;
    const u8 track = svoice->track;
    if (midi < 16) {
      salDiagMidiDecodedAbs[midi] += decodedAbs;
      salDiagMidiDryAbs[midi] += dryAbs;
      salDiagMidiAuxAAbs[midi] += auxAAbs;
      salDiagMidiAuxBAbs[midi] += auxBAbs;
      ++salDiagMidiSegments[midi];
    }
    salDiagTrackDecodedAbs[track] += decodedAbs;
    salDiagTrackDryAbs[track] += dryAbs;
    salDiagTrackAuxAAbs[track] += auxAAbs;
    salDiagTrackAuxBAbs[track] += auxBAbs;
    ++salDiagTrackSegments[track];
  }

  return voiceDone;
}

void salCtrlDsp(s16* dest) {
  u8 st;
  DSPstudioinfo* stp;

  salFrameDecodedAbs = 0;

  for (int i = 0; i < SAL_SAMPLES_PER_FRAME; ++i) {
    mixBufferL[i] = -delayedSurround[i];
    mixBufferR[i] = delayedSurround[i];
    mixBufferS[i] = 0;
  }

  for (st = 0, stp = dspStudio; st < salMaxStudioNum; ++st, ++stp) {
    if (stp->state != 1)
      continue;

    /* Clear the current frame buffers only. Previous-frame data feeds studio inputs. */
    if (stp->main[salFrame])
      memset(stp->main[salFrame], 0, SAL_SAMPLES_PER_FRAME * 3 * sizeof(s32));

    /* Clear AUX buffers for the current aux frame */
    if (stp->auxA[salAuxFrame])
      memset(stp->auxA[salAuxFrame], 0, SAL_SAMPLES_PER_FRAME * 3 * sizeof(s32));
    if (stp->auxB[salAuxFrame])
      memset(stp->auxB[salAuxFrame], 0, SAL_SAMPLES_PER_FRAME * 3 * sizeof(s32));

    /* Render all voices in this studio */
    DSPvoice* vp = stp->voiceRoot;
    while (vp != NULL) {
      DSPvoice* nextVp = vp->next; /* save in case voice is deactivated */
      if (vp->state != 0) {
        u32 voiceIdx = (u32)(vp - dspVoice);
        u8 mixStart = 0;
        u8 newVoice = 0;

        /* New voice initialization (mirrors salBuildCommandList state==1 path) */
        if (vp->state == 1) {
          ++salDiagVoiceStarts;
          salDiagAdsrAttackSum += vp->adsr.data.dls.aTime;
          salDiagAdsrDecaySum += vp->adsr.data.dls.dTime;
          salDiagAdsrSustainSum += vp->adsr.data.dls.sLevel;
          salDiagAdsrReleaseSum += vp->adsr.data.dls.rTime;
          if (vp->startupBreak != 0 && (vp->changed[0] & 0x20) != 0) {
            ++salDiagStartupBreak;
            vp->startupBreak = 0;
            deactivateVoiceAtOffset(vp, 0);
            vp = nextVp;
            continue;
          }
          if (adsrSetup(&vp->adsr) != 0) {
            ++salDiagSetupDone;
            salSynthSendMessage(vp, 0);
            deactivateVoiceAtOffset(vp, 0);
            vp = nextVp;
            continue;
          }

          if (salRenderDiagnostic != NULL && synthVoice != NULL &&
              voiceIdx < salNumVoices &&
              (synthVoice[voiceIdx].track == 14 || synthVoice[voiceIdx].track == 15 ||
               synthVoice[voiceIdx].track == 23) &&
              salTargetVoiceStartDiagnosticCount < 8192) {
            fprintf(salRenderDiagnostic,
                    "TARGET_VOICE_START frame=%u voice=%u track=%u midi=%u sample=%u "
                    "mixStart=%u pitch=%u flags=%llx "
                    "length=%u loop=%u loopLength=%u offset=%u comp=%u "
                    "adsrMode=%u adsrState=%u attack=%u decay=%u sustain=%u release=%u "
                    "vol=%u/%u/%u auxA=%u/%u/%u auxB=%u/%u/%u\n",
                    salAudioDumpFrameCount, voiceIdx, synthVoice[voiceIdx].track,
                    synthVoice[voiceIdx].midi, vp->smp_id, vp->singleOffset,
                    vp->pitch[vp->singleOffset],
                    (unsigned long long)synthVoice[voiceIdx].cFlags, vp->smp_info.length,
                    vp->smp_info.loop, vp->smp_info.loopLength, vp->smp_info.offset,
                    vp->smp_info.compType, vp->adsr.mode, vp->adsr.state,
                    vp->adsr.data.dls.aTime, vp->adsr.data.dls.dTime,
                    vp->adsr.data.dls.sLevel, vp->adsr.data.dls.rTime, vp->volL,
                    vp->volR, vp->volS, vp->volLa, vp->volRa, vp->volSa, vp->volLb,
                    vp->volRb, vp->volSb);
            ++salTargetVoiceStartDiagnosticCount;
          }

          vp->lastVolL = vp->volL;
          vp->lastVolR = vp->volR;
          vp->lastVolS = vp->volS;
          vp->lastVolLa = vp->volLa;
          vp->lastVolRa = vp->volRa;
          vp->lastVolSa = vp->volSa;
          vp->lastVolLb = vp->volLb;
          vp->lastVolRb = vp->volRb;
          vp->lastVolSb = vp->volSb;

          /* Initialize playback position based on compression type */
          switch (vp->smp_info.compType) {
          case 0:
          case 4:
          case 5:
            vp->playInfo.posHi = 0;
            vp->playInfo.posLo = 0;
            break;
          case 1: {
            u32 offset = (vp->smp_info.offset + 0xD) / 14;
            vp->playInfo.posHi = offset * 0xE;
            vp->playInfo.posLo = 0;
          } break;
          case 2:
#if MUSY_VERSION >= MUSY_VERSION_CHECK(2, 0, 2)
          case 6:
#endif
          case 3:
            vp->playInfo.posHi = vp->smp_info.offset;
            vp->playInfo.posLo = 0;
            break;
          }

          /* Reset ADPCM decode state for this voice */
          adpcmYn1[voiceIdx] = 0;
          adpcmYn2[voiceIdx] = 0;
          adpcmCachedBlock[voiceIdx] = (u32)~0u;
#if MUSY_VERSION >= MUSY_VERSION_CHECK(2, 0, 1)
          filterState[voiceIdx] = 0;
#endif

          mixStart = vp->singleOffset;
          newVoice = 1;
          /* A recycled voice always needs the pitch prepared for its exact
           * start subframe.  The generic changed flag can be consumed while
           * replacing the previous occupant in the same audio frame. */
          vp->playInfo.pitch = vp->pitch[mixStart];

          /* salActivateVoice preserves bit 0x20 so the GameCube can depop the
           * old occupant of a recycled slot.  That old contribution has
           * already been deposited above; it must not release the new voice. */
          vp->changed[0] &= ~0x20;

          if (vp->smp_info.compType == 1) {
            /* SDIR stores a variable-sized table of ADPCM seek checkpoints
             * after the coefficient header.  Only its first entry has a
             * fixed-size declaration, so blindly reading blk[offset] on the
             * little-endian host also reads unswapped histories.  Rebuild the
             * predictor state from the sample itself; this is the same
             * deterministic seek used by Amuse and avoids a burst of corrupt
             * audio whenever a macro starts inside a sample. */
            const u32 offset = (vp->smp_info.offset + 0xD) / 14;
            const s16(*coefTab)[2] = getVoiceCoefTab(&vp->smp_info);
            for (u32 block = 0; block < offset; ++block) {
              const u8* blockData = (const u8*)vp->smp_info.addr + block * 8;
              decodeADPCMBlock(blockData, coefTab, &adpcmYn1[voiceIdx], &adpcmYn2[voiceIdx],
                               adpcmBlockCache[voiceIdx], 0, blockData[0]);
            }
            if (offset != 0)
              adpcmCachedBlock[voiceIdx] = offset - 1;
          }

          voiceResampler[voiceIdx].srcCount = 0;
          voiceResampler[voiceIdx].srcConsumed = 0;
          voiceResampler[voiceIdx].curPos = 0;
          voiceResampler[voiceIdx].srcPosHi = vp->playInfo.posHi;
          memset(voiceResampler[voiceIdx].lastSamples, 0,
                 sizeof(voiceResampler[voiceIdx].lastSamples));
          voiceResampler[voiceIdx].ended = 0;
          updateCurrentAddr(vp, voiceResampler[voiceIdx].srcPosHi);

          vp->state = 2;
        }

        /* Get studio main buffer pointers */
        s32* studioMain = stp->main[salFrame];
        s32* studioL = studioMain;
        s32* studioR = studioMain ? studioMain + SAL_SAMPLES_PER_FRAME : NULL;
        s32* studioS = studioMain ? studioMain + SAL_SAMPLES_PER_FRAME * 2 : NULL;

        /* Get AUX buffer pointers */
        s32* auxACur = stp->auxA[salAuxFrame];
        s32* auxAL = auxACur;
        s32* auxAR = auxACur ? auxACur + SAL_SAMPLES_PER_FRAME : NULL;
        s32* auxAS = auxACur ? auxACur + SAL_SAMPLES_PER_FRAME * 2 : NULL;
        s32* auxBCur = stp->auxB[salAuxFrame];
        s32* auxBL = auxBCur;
        s32* auxBR = auxBCur ? auxBCur + SAL_SAMPLES_PER_FRAME : NULL;
        s32* auxBS = auxBCur ? auxBCur + SAL_SAMPLES_PER_FRAME * 2 : NULL;

        if (studioMain != NULL) {
          VolumeRamp ramps[MIX_BUS_COUNT] = {
              prepareVolumeRamp(vp->lastVolL, vp->volL),
              prepareVolumeRamp(vp->lastVolR, vp->volR),
              prepareVolumeRamp(vp->lastVolS, vp->volS),
              prepareVolumeRamp(vp->lastVolLa, vp->volLa),
              prepareVolumeRamp(vp->lastVolRa, vp->volRa),
              prepareVolumeRamp(vp->lastVolSa, vp->volSa),
              prepareVolumeRamp(vp->lastVolLb, vp->volLb),
              prepareVolumeRamp(vp->lastVolRb, vp->volRb),
              prepareVolumeRamp(vp->lastVolSb, vp->volSb),
          };
          int finished = 0;

          for (u8 subframe = mixStart; subframe < SAL_SUBFRAMES; ++subframe) {
            if ((vp->changed[subframe] & 0x20) != 0 &&
                !(newVoice && subframe == mixStart)) {
              ++salDiagPostBreak;
              adsrStartRelease(&vp->adsr, 10);
              vp->postBreak = 1;
            } else if (vp->postBreak == 0) {
              if ((vp->changed[subframe] & 0x40) != 0) {
                ++salDiagKeyOffs;
                if (salRenderDiagnostic != NULL && synthVoice != NULL &&
                    voiceIdx < salNumVoices &&
                    (synthVoice[voiceIdx].track == 14 || synthVoice[voiceIdx].track == 15 ||
                     synthVoice[voiceIdx].track == 23) &&
                    salTargetVoiceKeyOffDiagnosticCount < 8192) {
                  fprintf(salRenderDiagnostic,
                          "TARGET_VOICE_KEYOFF frame=%u voice=%u track=%u midi=%u "
                          "subframe=%u mixStart=%u newVoice=%u changed=%08x flags=%llx "
                          "adsrState=%u adsrCount=%u currentVolume=%d currentIndex=%d\n",
                          salAudioDumpFrameCount, voiceIdx, synthVoice[voiceIdx].track,
                          synthVoice[voiceIdx].midi, subframe, mixStart, newVoice,
                          vp->changed[subframe],
                          (unsigned long long)synthVoice[voiceIdx].cFlags, vp->adsr.state,
                          vp->adsr.cnt, vp->adsr.currentVolume, vp->adsr.currentIndex);
                  ++salTargetVoiceKeyOffDiagnosticCount;
                }
                adsrRelease(&vp->adsr);
              }
              if ((vp->changed[subframe] & 8) != 0)
                vp->playInfo.pitch = vp->pitch[subframe];
            }

            if ((vp->changed[subframe] & 0x10) != 0) {
              if (adsrSetup(&vp->adsr) != 0) {
                ++salDiagSetupDone;
                salSynthSendMessage(vp, 0);
                deactivateVoiceAtOffset(vp, subframe * SAL_SAMPLES_PER_SUBFRAME);
                finished = 1;
                break;
              }
            }

            if (newVoice && subframe == mixStart) {
              const u32 diagnosticPitch = vp->playInfo.pitch;
              salDiagPitchSum += diagnosticPitch;
              salDiagPitchHash = (salDiagPitchHash ^ diagnosticPitch) * 1099511628211ULL;
              if (diagnosticPitch < salDiagPitchMin)
                salDiagPitchMin = diagnosticPitch;
              if (diagnosticPitch > salDiagPitchMax)
                salDiagPitchMax = diagnosticPitch;
            }

            u16 adsrStart = 0;
            u16 adsrDelta = 0;
            u32 adsrDone = adsrHandle(&vp->adsr, &adsrStart, &adsrDelta);
            int sampleOffset = subframe * SAL_SAMPLES_PER_SUBFRAME;

            if (renderVoiceSegment(vp, studioL, studioR, studioS, auxAL, auxAR, auxAS, auxBL, auxBR,
                                   auxBS, voiceIdx, sampleOffset, SAL_SAMPLES_PER_SUBFRAME,
                                   adsrStart, (s16)adsrDelta, ramps)) {
              ++salDiagSampleDone;
              salSynthSendMessage(vp, 0);
              deactivateVoiceAtOffset(vp, sampleOffset + SAL_SAMPLES_PER_SUBFRAME);
              finished = 1;
              break;
            }

            if (adsrDone) {
              ++salDiagAdsrDone;
              if (vp->adsr.state == 4)
                ++salDiagAdsrReleaseDone;
              else
                ++salDiagAdsrNaturalDone;
              salSynthSendMessage(vp, 0);
              deactivateVoiceAtOffset(vp, sampleOffset + SAL_SAMPLES_PER_SUBFRAME);
              finished = 1;
              break;
            }
          }

          vp->lastVolL = (u16)ramps[MIX_BUS_L].end;
          vp->lastVolR = (u16)ramps[MIX_BUS_R].end;
          vp->lastVolS = (u16)ramps[MIX_BUS_S].end;
          vp->lastVolLa = (u16)ramps[MIX_BUS_AUX_AL].end;
          vp->lastVolRa = (u16)ramps[MIX_BUS_AUX_AR].end;
          vp->lastVolSa = (u16)ramps[MIX_BUS_AUX_AS].end;
          vp->lastVolLb = (u16)ramps[MIX_BUS_AUX_BL].end;
          vp->lastVolRb = (u16)ramps[MIX_BUS_AUX_BR].end;
          vp->lastVolSb = (u16)ramps[MIX_BUS_AUX_BS].end;

          if (finished) {
            vp = nextVp;
            continue;
          }
        }
      }
      vp = nextVp;
    }

    /* Mix each stopped voice's last contribution once, in chronological
     * order, then fade the accumulated tail like the GameCube DSP. */
    renderStudioDepop(stp);

    if (stp->main[salFrame] != NULL) {
      s32* studioMain = stp->main[salFrame];
      s32* auxACur = stp->auxA[salAuxFrame];
      s32* auxBCur = stp->auxB[salAuxFrame];
      for (u8 inputIdx = 0; inputIdx < stp->numInputs; ++inputIdx) {
        DSPinput* input = &stp->in[inputIdx];
        DSPstudioinfo* srcStudio = &dspStudio[input->studio];
        s32* srcMain = srcStudio->main[salFrame ^ 1];
        if (srcMain == NULL)
          continue;

        addThreeChannelBuffer(studioMain, srcMain, input->vol);
        addThreeChannelBuffer(auxACur, srcMain, input->volA);
        addThreeChannelBuffer(auxBCur, srcMain, input->volB);
      }
    }

    {
      s32* auxAWork = stp->auxA[(salAuxFrame + 2) % 3];
      if (auxAWork != NULL && stp->auxAHandler != NULL) {
        SND_AUX_INFO info;
        info.data.bufferUpdate.left = auxAWork;
        info.data.bufferUpdate.right = auxAWork + SAL_SAMPLES_PER_FRAME;
        info.data.bufferUpdate.surround = auxAWork + SAL_SAMPLES_PER_FRAME * 2;
        stp->auxAHandler(SND_AUX_REASON_BUFFERUPDATE, &info, stp->auxAUser);
      }

      if (stp->type == SND_STUDIO_TYPE_STD) {
        s32* auxBWork = stp->auxB[(salAuxFrame + 2) % 3];
        if (auxBWork != NULL && stp->auxBHandler != NULL) {
          SND_AUX_INFO info;
          info.data.bufferUpdate.left = auxBWork;
          info.data.bufferUpdate.right = auxBWork + SAL_SAMPLES_PER_FRAME;
          info.data.bufferUpdate.surround = auxBWork + SAL_SAMPLES_PER_FRAME * 2;
          stp->auxBHandler(SND_AUX_REASON_BUFFERUPDATE, &info, stp->auxBUser);
        }
      }
    }

    /* Accumulate master studio into final mix */
    if (stp->isMaster) {
      s32* studioMain = stp->main[salFrame];
      if (studioMain != NULL) {
        s32* studioL = studioMain;
        s32* studioR = studioMain + SAL_SAMPLES_PER_FRAME;
        s32* studioS = studioMain + SAL_SAMPLES_PER_FRAME * 2;
        mixLrsToOutput(studioL, studioR, studioS);
      }

      /* Add processed AUX A (reverb output) to mix */
      /* The callback writes frame +2 while the mixer consumes the previously
       * processed frame +1, matching the GameCube's three-buffer AUX pipeline. */
      s32* auxProcessed = stp->auxA[(salAuxFrame + 1) % 3];
      if (auxProcessed && stp->auxAHandler) {
        s32* auxL = auxProcessed;
        s32* auxR = auxProcessed + SAL_SAMPLES_PER_FRAME;
        s32* auxS = auxProcessed + SAL_SAMPLES_PER_FRAME * 2;
        mixLrsToOutput(auxL, auxR, auxS);
      }

      if (stp->type == SND_STUDIO_TYPE_DPL2) {
        s32* dpl2Rear = stp->auxB[salAuxFrame];
        if (dpl2Rear != NULL)
          foldStereoToOutput(dpl2Rear, dpl2Rear + SAL_SAMPLES_PER_FRAME);
      } else {
        /* Add processed AUX B to mix for standard studios only. */
        s32* auxBProcessed = stp->auxB[(salAuxFrame + 1) % 3];
        if (auxBProcessed && stp->auxBHandler) {
          s32* auxBL2 = auxBProcessed;
          s32* auxBR2 = auxBProcessed + SAL_SAMPLES_PER_FRAME;
          s32* auxBS2 = auxBProcessed + SAL_SAMPLES_PER_FRAME * 2;
          mixLrsToOutput(auxBL2, auxBR2, auxBS2);
        }
      }
    }
  }

  /* OUTPUT preserves the accumulated surround channel for the following
   * SET_OPPOSITE_LR command list. */
  memcpy(delayedSurround, mixBufferS, sizeof(delayedSurround));

  /* Write interleaved stereo s16 to dest */
  if (dest) {
    for (int i = 0; i < SAL_SAMPLES_PER_FRAME; i++) {
      dest[i * 2 + 0] = clamp16(mixBufferL[i] >> SAL_OUTPUT_SHIFT);
      dest[i * 2 + 1] = clamp16(mixBufferR[i] >> SAL_OUTPUT_SHIFT);
    }
    HuTHPPCM16Mix(dest, SAL_SAMPLES_PER_FRAME);
    if (salAudioBackend == SAL_AUDIO_BACKEND_DOLPHIN_AX && salOutputColorationEnabled)
      applyOptionalOutputColoration(dest);
    if (salAudioDump != NULL) {
      fwrite(dest, sizeof(s16), SAL_STEREO_SAMPLES, salAudioDump);
    }
    ++salAudioDumpFrameCount;
    if (salAudioDump != NULL && salAudioDumpFrameCount % 20 == 0) {
      fflush(salAudioDump);
    }
    if (salVoiceTrace != NULL && salAudioDumpFrameCount % 200 == 0)
      fflush(salVoiceTrace);
    if (salRenderDiagnostic != NULL && salAudioDumpFrameCount % 200 == 0) {
      uint64_t outputAbs = 0;
      u32 active = 0;
      u32 ended = 0;
      uint64_t volume = 0;
      for (int i = 0; i < SAL_STEREO_SAMPLES; ++i)
        outputAbs += dest[i] < 0 ? -(int64_t)dest[i] : dest[i];
      for (u32 i = 0; i < salNumVoices; ++i) {
        if (dspVoice[i].state != 0) {
          ++active;
          ended += voiceResampler[i].ended != 0;
          volume += (u16)dspVoice[i].volL + (u16)dspVoice[i].volR;
        }
      }
      fprintf(salRenderDiagnostic,
              "frame=%u decodedAbs=%llu outputAbs=%llu active=%u ended=%u volume=%llu samples=%u sampleLag=%llu lagMax=%u missing=%u stopStartup=%u stopSetup=%u stopSample=%u stopAdsr=%u postBreak=%u voiceStarts=%u keyOffs=%u adsrReleaseDone=%u adsrNaturalDone=%u adsrAttack=%llu adsrDecay=%llu adsrSustain=%llu adsrRelease=%llu pitchSum=%llu pitchMin=%u pitchMax=%u\n",
              salAudioDumpFrameCount, (unsigned long long)salFrameDecodedAbs,
              (unsigned long long)outputAbs, active, ended, (unsigned long long)volume,
              startedSampleDiagnosticCount, (unsigned long long)startedSampleLagSum,
              startedSampleLagMax, missingSampleDiagnosticCount, salDiagStartupBreak,
              salDiagSetupDone, salDiagSampleDone, salDiagAdsrDone, salDiagPostBreak,
              salDiagVoiceStarts, salDiagKeyOffs, salDiagAdsrReleaseDone,
              salDiagAdsrNaturalDone, (unsigned long long)salDiagAdsrAttackSum,
              (unsigned long long)salDiagAdsrDecaySum,
              (unsigned long long)salDiagAdsrSustainSum,
               (unsigned long long)salDiagAdsrReleaseSum,
               (unsigned long long)salDiagPitchSum, salDiagPitchMin, salDiagPitchMax);
      fprintf(salRenderDiagnostic, "MIDI_ENERGY frame=%u", salAudioDumpFrameCount);
      for (u32 midi = 0; midi < 16; ++midi) {
        if (salDiagMidiSegments[midi] != 0) {
          fprintf(salRenderDiagnostic, " %u:%llu/%llu/%llu/%llu/%llu", midi,
                  (unsigned long long)salDiagMidiDecodedAbs[midi],
                  (unsigned long long)salDiagMidiDryAbs[midi],
                  (unsigned long long)salDiagMidiAuxAAbs[midi],
                  (unsigned long long)salDiagMidiAuxBAbs[midi],
                  (unsigned long long)salDiagMidiSegments[midi]);
        }
      }
      fputc('\n', salRenderDiagnostic);
      fprintf(salRenderDiagnostic, "TRACK_ENERGY frame=%u", salAudioDumpFrameCount);
      for (u32 track = 0; track < 256; ++track) {
        if (salDiagTrackSegments[track] != 0) {
          fprintf(salRenderDiagnostic, " %u:%llu/%llu/%llu/%llu/%llu", track,
                  (unsigned long long)salDiagTrackDecodedAbs[track],
                  (unsigned long long)salDiagTrackDryAbs[track],
                  (unsigned long long)salDiagTrackAuxAAbs[track],
                  (unsigned long long)salDiagTrackAuxBAbs[track],
                  (unsigned long long)salDiagTrackSegments[track]);
        }
      }
      fputc('\n', salRenderDiagnostic);
      fflush(salRenderDiagnostic);
    }
  }
}

static int salAudioThreadFunc(void* data) {
  (void)data;
  u32 diagnosticFrames = 0;
  u32 diagnosticLowQueue = 0;
  u32 diagnosticPutFailures = 0;
  u32 diagnosticQueueErrors = 0;
  u64 diagnosticRenderNsMax = 0;
  int diagnosticQueueMin = INT_MAX;
  int diagnosticQueueMax = 0;

  SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);
  while (SDL_GetAtomicInt(&salAudioThreadRunning)) {
    const int queuedBytes = salAudioStream != NULL ? SDL_GetAudioStreamQueued(salAudioStream) : 0;
    if (queuedBytes < 0) {
      ++diagnosticQueueErrors;
      SDL_Delay(1);
      continue;
    }
    if (salAudioStream != NULL && queuedBytes > SAL_BUFFER_BYTES * SAL_MAX_QUEUED_FRAMES) {
      SDL_Delay(1);
      continue;
    }

    if (queuedBytes < diagnosticQueueMin)
      diagnosticQueueMin = queuedBytes;
    if (queuedBytes > diagnosticQueueMax)
      diagnosticQueueMax = queuedBytes;
    if (queuedBytes < SAL_BUFFER_BYTES)
      ++diagnosticLowQueue;

    const u64 renderStart = SDL_GetTicksNS();
    if (userCallback) {
      userCallback();
    }
    const u64 renderNs = SDL_GetTicksNS() - renderStart;
    if (renderNs > diagnosticRenderNsMax)
      diagnosticRenderNsMax = renderNs;

    /* Push rendered buffer to SDL audio stream */
    s16* buf = salOutputBuffers[salOutputIndex];
    if (salAudioStream) {
      if (!SDL_PutAudioStreamData(salAudioStream, buf, SAL_BUFFER_BYTES))
        ++diagnosticPutFailures;
    }

    if (salAudioTimingDiagnostic != NULL && ++diagnosticFrames == 200) {
      fprintf(salAudioTimingDiagnostic,
              "frames=%u queueMin=%d queueMax=%d lowQueue=%u renderMaxUs=%llu "
              "queueErrors=%u putFailures=%u\n",
              salAudioDumpFrameCount, diagnosticQueueMin, diagnosticQueueMax,
              diagnosticLowQueue, (unsigned long long)(diagnosticRenderNsMax / 1000),
              diagnosticQueueErrors, diagnosticPutFailures);
      fflush(salAudioTimingDiagnostic);
      diagnosticFrames = 0;
      diagnosticLowQueue = 0;
      diagnosticPutFailures = 0;
      diagnosticQueueErrors = 0;
      diagnosticRenderNsMax = 0;
      diagnosticQueueMin = INT_MAX;
      diagnosticQueueMax = 0;
    }

    if (salAudioStream == NULL)
      SDL_Delay(1);
  }
  return 0;
}

bool salInitAi(SND_SOME_CALLBACK callback, u32 flags, u32* outFreq) {
  (void)flags;
  memset(salOutputBuffers, 0, sizeof(salOutputBuffers));
  salOutputIndex = 0;
  userCallback = callback;

  const char* backendName = getenv("PARTYBOARD_AUDIO_BACKEND");
  salAudioBackend = backendName != NULL && strcmp(backendName, "legacy") == 0
                        ? SAL_AUDIO_BACKEND_LEGACY
                        : SAL_AUDIO_BACKEND_DOLPHIN_AX;
  fprintf(stderr, "[MusyX] Audio backend: %s.\n",
          salAudioBackend == SAL_AUDIO_BACKEND_DOLPHIN_AX ? "Dolphin AX" : "legacy");

  const char* outputColoration = getenv("PARTYBOARD_AUDIO_OUTPUT_COLORATION");
  salOutputColorationEnabled =
      outputColoration != NULL &&
      (strcmp(outputColoration, "1") == 0 || strcmp(outputColoration, "on") == 0 ||
       strcmp(outputColoration, "true") == 0);
  if (salOutputColorationEnabled)
    fprintf(stderr, "[MusyX] Optional host-output coloration enabled.\n");

  const bool diagnosticsMarkerEnabled = salDiagnosticsMarkerEnabled();
  const char* audioDumpPath = getenv("PARTYBOARD_AUDIO_DUMP");
  if (audioDumpPath != NULL && audioDumpPath[0] != '\0') {
    salAudioDump = fopen(audioDumpPath, "wb");
    salAudioDumpFrameCount = 0;
  }
  const char* timingDiagnosticPath = getenv("PARTYBOARD_AUDIO_TIMING_DIAGNOSTICS");
  if (timingDiagnosticPath != NULL && timingDiagnosticPath[0] != '\0')
    salAudioTimingDiagnostic = fopen(timingDiagnosticPath, "w");
  else if (diagnosticsMarkerEnabled) {
    char diagnosticPath[1024];
    salAudioTimingDiagnostic = fopen(
        salDiagnosticUserPath("audio_timing_diagnostic.log", diagnosticPath, sizeof(diagnosticPath)), "w");
  }
  const char* voiceTracePath = getenv("PARTYBOARD_AUDIO_VOICE_TRACE");
  if (voiceTracePath != NULL && voiceTracePath[0] != '\0') {
    salVoiceTrace = fopen(voiceTracePath, "wb");
    if (salVoiceTrace != NULL) {
      const SalVoiceTraceHeader header = {
          .magic = {'P', 'B', 'A', 'T', 'R', 'C', 'E', '\0'},
          .version = 1,
          .recordSize = sizeof(SalVoiceTraceRecord),
          .sampleRate = *outFreq,
          .samplesPerSegment = SAL_SAMPLES_PER_SUBFRAME,
      };
      fwrite(&header, sizeof(header), 1, salVoiceTrace);
      salVoiceTraceFrameLimit =
          parsePositiveEnvironmentValue("PARTYBOARD_AUDIO_TRACE_FRAMES", 2000);
      const char* voiceFilter = getenv("PARTYBOARD_AUDIO_TRACE_VOICE");
      salVoiceTraceVoice = -1;
      if (voiceFilter != NULL && voiceFilter[0] != '\0') {
        char* end = NULL;
        const long parsed = strtol(voiceFilter, &end, 10);
        if (end != voiceFilter && *end == '\0' && parsed >= 0 && parsed < SYNTH_MAX_VOICES)
          salVoiceTraceVoice = (int)parsed;
      }
      fprintf(stderr, "[MusyX] Voice trace enabled for %u frames%s.\n",
              salVoiceTraceFrameLimit,
              salVoiceTraceVoice >= 0 ? " (single voice)" : "");
    }
  }
  if (getenv("PARTYBOARD_RENDER_DIAGNOSTICS") != NULL) {
    salRenderDiagnostic = fopen("audio_render_diagnostic.log", "w");
  } else if (diagnosticsMarkerEnabled) {
    char diagnosticPath[1024];
    salRenderDiagnostic = fopen(
        salDiagnosticUserPath("audio_render_diagnostic.log", diagnosticPath, sizeof(diagnosticPath)), "w");
  }

  memset(adpcmYn1, 0, sizeof(adpcmYn1));
  memset(adpcmYn2, 0, sizeof(adpcmYn2));
  memset(adpcmBlockCache, 0, sizeof(adpcmBlockCache));
  memset(adpcmCachedBlock, 0xFF, sizeof(adpcmCachedBlock)); /* ~0u = invalid */
  memset(voiceResampler, 0, sizeof(voiceResampler));
  memset(voiceLastMix, 0, sizeof(voiceLastMix));
  memset(mixBufferS, 0, sizeof(mixBufferS));
  memset(delayedSurround, 0, sizeof(delayedSurround));
  memset(dolphinOutputHistory, 0, sizeof(dolphinOutputHistory));
  memset(salDiagMidiDecodedAbs, 0, sizeof(salDiagMidiDecodedAbs));
  memset(salDiagMidiDryAbs, 0, sizeof(salDiagMidiDryAbs));
  memset(salDiagMidiAuxAAbs, 0, sizeof(salDiagMidiAuxAAbs));
  memset(salDiagMidiAuxBAbs, 0, sizeof(salDiagMidiAuxBAbs));
  memset(salDiagMidiSegments, 0, sizeof(salDiagMidiSegments));
  memset(salDiagTrackDecodedAbs, 0, sizeof(salDiagTrackDecodedAbs));
  memset(salDiagTrackDryAbs, 0, sizeof(salDiagTrackDryAbs));
  memset(salDiagTrackAuxAAbs, 0, sizeof(salDiagTrackAuxAAbs));
  memset(salDiagTrackAuxBAbs, 0, sizeof(salDiagTrackAuxBAbs));
  memset(salDiagTrackSegments, 0, sizeof(salDiagTrackSegments));
  memset(depopEvents, 0, sizeof(depopEvents));
  depopRenderOffset = -1;
  initResampleTables();

  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    return false;
  }

  SDL_AudioSpec spec = {
      .format = SDL_AUDIO_S16,
      .channels = 2,
      .freq = *outFreq,
  };
  salAudioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
  if (!salAudioStream || !SDL_GetAudioStreamFormat(salAudioStream, &spec, NULL)) {
    return false;
  }

  synthInfo.numSamples = 0x20;
  *outFreq = spec.freq;
  return true;
}

bool salStartAi() {
  if (!salAudioStream)
    return false;

  for (u32 i = 0; i < SAL_PREFILL_FRAMES; ++i) {
    if (userCallback)
      userCallback();
    SDL_PutAudioStreamData(salAudioStream, salOutputBuffers[salOutputIndex], SAL_BUFFER_BYTES);
  }

  SDL_SetAtomicInt(&salAudioThreadRunning, 1);
  salAudioThread = SDL_CreateThread(salAudioThreadFunc, "MusyX Audio", NULL);
  if (!salAudioThread) {
    return false;
  }
  SDL_ResumeAudioStreamDevice(salAudioStream);
  return true;
}

bool salExitAi() {
  if (salAudioThread) {
    SDL_SetAtomicInt(&salAudioThreadRunning, 0);
    SDL_WaitThread(salAudioThread, NULL);
    salAudioThread = NULL;
  }
  if (salAudioStream) {
    SDL_DestroyAudioStream(salAudioStream);
    salAudioStream = NULL;
  }
  if (salAudioDump != NULL) {
    fclose(salAudioDump);
    salAudioDump = NULL;
  }
  if (salAudioTimingDiagnostic != NULL) {
    fclose(salAudioTimingDiagnostic);
    salAudioTimingDiagnostic = NULL;
  }
  if (salVoiceTrace != NULL) {
    fclose(salVoiceTrace);
    salVoiceTrace = NULL;
  }
  if (salRenderDiagnostic != NULL) {
    fclose(salRenderDiagnostic);
    salRenderDiagnostic = NULL;
  }
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
  return true;
}

void* salAiGetDest() {
  salOutputIndex ^= 1;
  return salOutputBuffers[salOutputIndex];
}

bool salInitDsp(u32 flags) {
  (void)flags;
  return true;
}

bool salExitDsp() { return true; }

void salStartDsp(u16* cmdList) {
  (void)cmdList;
  /* no-op */
}

void hwInitIrq() {
  globalMutex = SDL_CreateMutex();
  /* Start with IRQs disabled (locked), matching hwIrqLevel=1 on Dolphin.
   * hwEnableIrq() in hwInit() will unlock. */
  SDL_LockMutex(globalMutex);
}

void hwExitIrq() {
  if (globalMutex != NULL) {
    SDL_DestroyMutex(globalMutex);
    globalMutex = NULL;
  }
}

void hwEnableIrq() { SDL_UnlockMutex(globalMutex); }

void hwDisableIrq() { SDL_LockMutex(globalMutex); }

void hwIRQEnterCritical() { SDL_LockMutex(globalMutex); }

void hwIRQLeaveCritical() { SDL_UnlockMutex(globalMutex); }
