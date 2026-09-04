/* Exercise the actual wait interpreter with isolated clock/hardware boundaries. */
#include "musyx/hardware.h"
#include "musyx/macros.h"
#include "musyx/snd.h"
#include <stdio.h>

static u64 macRealTime;
static SYNTH_VOICE* macActiveMacroRoot;
static SYNTH_VOICE* macTimeQueueRoot;
static void TimeQueueAdd(SYNTH_VOICE* voice) { macTimeQueueRoot = voice; }
void macMakeInactive(SYNTH_VOICE* voice, MAC_STATE state) { voice->macState = state; }
#include "../src/musyx/runtime/synth_wait.h"

static unsigned tickConversions;
u32 hwIsActive(u32 voice) { (void)voice; return 1; }
u16 sndRand(void) { return 123; }
void synthForceLowPrecisionUpdate(SYNTH_VOICE* voice) { (void)voice; }
void sndConvertMs(u32* value) { *value *= 256; }
void sndConvertTicks(u32* value, SYNTH_VOICE* voice) {
  (void)voice;
  ++tickConversions;
  /* synthSetBpm(90): ((90 << 3) * 1536) / 240 == 4608. */
  *value = (((*value << 16) / 4608) * 1000) / 32;
}

static int check(const char* name, u64 now, u32 first, u32 second,
                 u64 expectedWait, u32 expectedYield) {
  SYNTH_VOICE voice = {0};
  MSTEP command = {{first, second}};
  macActiveMacroRoot = NULL;
  macTimeQueueRoot = NULL;
  macRealTime = now;
  voice.macStartTime = now;
  voice.waitTime = now;
  voice.addr = &command;
  voice.macState = MAC_STATE_RUNNABLE;
  tickConversions = 0;
  u32 yielded = mcmdWaitMs(&voice, &command);
  int ok = voice.wait == expectedWait && yielded == expectedYield &&
           tickConversions == 0 && (command.para[1] >> 16) == (second >> 16);
  printf("%s %s: wait=%llu expected=%llu yield=%u ticks=%u duration=%u\n",
         ok ? "PASS" : "FAIL", name, (unsigned long long)voice.wait,
         (unsigned long long)expectedWait, yielded, tickConversions,
         command.para[1] >> 16);
  return !ok;
}

static u32 legacyWaitMs(SYNTH_VOICE* voice, MSTEP* command) {
  /* Exact former implementation; retained only to reproduce the failure. */
  *((u8*)command->para + 6) = 1;
  return mcmdWait(voice, command);
}

static int checkMenuChain(void) {
  int stable[2] = {0};
  for (int fixed = 0; fixed < 2; ++fixed) {
    SYNTH_VOICE voice = {0};
    MSTEP startWait = {{7, 0x014d0001}};
    MSTEP noteWait = {{0x107, 0xffff0000}};
    u32 (*execute)(SYNTH_VOICE*, MSTEP*) = fixed ? mcmdWaitMs : legacyWaitMs;
    macRealTime = 120000ULL * 256;
    voice.macStartTime = macRealTime;
    voice.waitTime = macRealTime;
    macTimeQueueRoot = NULL;
    if (execute(&voice, &startWait)) {
      /* Resume the macro when its initial timed wait expires. */
      macRealTime = voice.wait;
      voice.waitTime = voice.wait;
      voice.wait = 0;
    }
    stable[fixed] = execute(&voice, &noteWait) && voice.wait == (u64)-1;
  }
  printf("Menu macro after 120s: former code %s; fixed code %s\n",
         stable[0] ? "waits" : "falls through to KeyOff immediately",
         stable[1] ? "waits for the note's KeyOff" : "FAIL");
  return stable[0] || !stable[1];
}

int main(void) {
  int failures = 0;
  /* 0x014d0001 is present in the original menu's instrument macros. */
  failures += check("333ms absolute at startup", 0, 7, 0x014d0001,
                    333ULL * 256, 1);
  failures += check("333ms absolute after two minutes", 120000ULL * 256,
                    7, 0x014d0001, 120333ULL * 256, 1);
  failures += check("333ms absolute after one hour", 3600000ULL * 256,
                    7, 0x014d0001, 3600333ULL * 256, 1);
  failures += check("indefinite until keyoff", 120000ULL * 256,
                    0x107, 0xffff0000, (u64)-1, 1);
  failures += check("indefinite until sample end", 120000ULL * 256,
                    0x01000007, 0xffff0000, (u64)-1, 1);
  failures += check("3000ms absolute sample-end timeout", 120000ULL * 256,
                    0x01000007, 0x0bb80001, 123000ULL * 256, 1);
  failures += check("100ms relative", 120000ULL * 256,
                    7, 0x00640000, 120100ULL * 256, 1);
  failures += check("zero duration", 120000ULL * 256, 7, 0, 0, 0);
  failures += checkMenuChain();
  /* Every encoded duration, in absolute and relative mode, at startup and
   * after an hour. This includes the 0xffff indefinite sentinel. */
  unsigned cases = 0;
  for (unsigned late = 0; late < 2; ++late) {
    for (unsigned absolute = 0; absolute < 2; ++absolute) {
      for (unsigned duration = 0; duration <= 0xffff; ++duration) {
        SYNTH_VOICE voice = {0};
        MSTEP command = {{7, (duration << 16) | absolute}};
        macRealTime = late ? 3600000ULL * 256 : 0;
        macTimeQueueRoot = NULL;
        voice.macStartTime = macRealTime;
        voice.waitTime = macRealTime;
        tickConversions = 0;
        u64 expected = duration == 0xffff ? (u64)-1 :
                       duration == 0 ? 0 : macRealTime + duration * 256ULL;
        u32 yielded = mcmdWaitMs(&voice, &command);
        if (voice.wait != expected || yielded != (duration != 0) ||
            tickConversions != 0 || command.para[1] >> 16 != duration) {
          ++failures;
        }
        ++cases;
      }
    }
  }
  printf("%u exhaustive duration/clock cases executed\n", cases);
  printf("%d regression failures\n", failures);
  return failures ? 1 : 0;
}
