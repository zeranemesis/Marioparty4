/* Private macro wait implementation, shared with the focused regression test. */
static u32 mcmdWait(SYNTH_VOICE* svoice, MSTEP* cstep) {
  u32 w;  // r1+0x10
  u32 ms; // r29

  if ((ms = (u16)(cstep->para[1] >> 0x10))) {
    if (((u8)(cstep->para[0] >> 8) & 1)) {
      if (svoice->cFlags & 8) {
        if (!(svoice->cFlags & 0x10000000000)) {
          return 0;
        }
        svoice->cFlags |= 0x40000000000;
      }
      svoice->cFlags |= 4;
    } else {
      svoice->cFlags &= ~4;
    }

    if (((u8)(cstep->para[0] >> 0x18) & 1)) {
      if (!(svoice->cFlags & 0x20) && !hwIsActive(svoice->id & 0xFF)) {
        return 0;
      }
      svoice->cFlags |= 0x40000;
    } else {
      svoice->cFlags &= ~0x40000;
    }

    if (((u8)(cstep->para[0] >> 0x10)) & 1) {
      ms = sndRand() % ms;
    }

    if (ms != 0xFFFF) {
      if ((w = ((u8)(cstep->para[1] >> 0x8) & 1) != 0)) {
        sndConvertMs(&ms);
      } else {
        sndConvertTicks(&ms, svoice);
      }

      if (w != 0) {
        if ((u8)cstep->para[1] & 1) {
          svoice->wait = svoice->macStartTime + ms;
        } else {
          svoice->wait = macRealTime + ms;
        }
      } else {
        if ((u8)cstep->para[1] & 1) {
          svoice->wait = ms;
        } else {
          svoice->wait = svoice->waitTime + ms;
        }
      }

      if (!(svoice->wait > macRealTime)) {
        svoice->waitTime = svoice->wait;
        svoice->wait = 0;
      }
    } else {
      svoice->wait = -1;
    }

    if (svoice->wait != 0) {
      if (svoice->wait != -1) {
        TimeQueueAdd(svoice);
      }
      macMakeInactive(svoice, 1);
      return 1;
    }
  }

  return 0;
}

static u32 mcmdWaitMs(SYNTH_VOICE* svoice, MSTEP* cstep) {
  /* The original PPC byte write at offset 6 selects milliseconds (bits
   * 8..15 of para[1]). On little-endian hosts that byte is part of the
   * duration instead: 333 becomes 257, and 0xffff loses its infinite-wait
   * meaning. An absolute wait then uses the tick/global-time path and
   * eventually expires immediately for every newly started instrument.
   * Select the field by value, preserving duration and absolute mode. */
  cstep->para[1] = (cstep->para[1] & ~0x0000ff00u) | 0x00000100u;
  return mcmdWait(svoice, cstep);
}
