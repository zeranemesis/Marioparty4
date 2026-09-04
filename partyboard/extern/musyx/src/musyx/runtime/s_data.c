
#include "musyx/assert.h"
#include "musyx/hardware.h"
#include "musyx/s3d.h"
#include "musyx/seq.h"
#include "musyx/synth.h"
#include "musyx/synthdata.h"

#include <stdlib.h>
#include <string.h>

#if MUSY_VERSION <= MUSY_VERSION_CHECK(2, 0, 0)
static GSTACK gs[128];
#define GS_CURRENT gs
#define GS_GSI gs
static s16 sp;
#define SP_CURRENT sp
#define SP_GSI sp
#else
static GSTACK_INST gsDefault;
#define GS_CURRENT gsCurrent->gs
#define GS_GSI gsi->gs
#define SP_CURRENT gsCurrent->sp
#define SP_GSI gsi->sp
#endif

#if MUSY_VERSION >= MUSY_VERSION_CHECK(2, 0, 1)
static unsigned long gsNextID;
static GSTACK_INST* gsCurrent;
static GSTACK_INST* gsRoot;

static void dataInitStackInstance(GSTACK_INST* inst, unsigned long id, unsigned long aramBase,
                                  unsigned long aramSize) {
  inst->id = id;
  inst->sp = 0;
  inst->aramInfo.aramBase = aramBase;
  inst->aramInfo.aramWrite = aramBase;
  inst->aramInfo.aramTop = aramBase + aramSize;
  inst->next = gsRoot;
  gsRoot = inst;
}
#endif

void dataInitStack(
#if MUSY_VERSION >= MUSY_VERSION_CHECK(2, 0, 1)
    unsigned long aramBase, unsigned long aramSize
#endif
) {
#if MUSY_VERSION <= MUSY_VERSION_CHECK(2, 0, 0)
  sp = 0;
#else
  gsRoot = NULL;
  dataInitStackInstance(&gsDefault, -2, aramBase, aramSize);
  gsNextID = 0;
  gsCurrent = &gsDefault;
#endif
}

#if MUSY_VERSION >= MUSY_VERSION_CHECK(2, 0, 1)
ARAMInfo* dataARAMGetInfo() { return &gsCurrent->aramInfo; }

ARAMInfo* dataARAMDefaultGetInfo() { return &gsDefault.aramInfo; }
#endif

static MEM_DATA* GetPoolAddr(u16 id, MEM_DATA* m) {
  while (m->nextOff != 0xFFFFFFFF) {
    if (m->id == id) {
      return m;
    }

    m = (MEM_DATA*)((u8*)m + m->nextOff);
  }
  return NULL;
}

static MEM_DATA* GetMacroAddr(u16 id, POOL_DATA* pool) {
  return pool == NULL ? NULL : GetPoolAddr(id, (MEM_DATA*)((u8*)pool + pool->macroOff));
}

static MEM_DATA* GetCurveAddr(u16 id, POOL_DATA* pool) {
  return pool == NULL ? NULL : GetPoolAddr(id, (MEM_DATA*)((u8*)pool + pool->curveOff));
}
static MEM_DATA* GetKeymapAddr(u16 id, POOL_DATA* pool) {
  return pool == NULL ? NULL : GetPoolAddr(id, (MEM_DATA*)((u8*)pool + pool->keymapOff));
}
static MEM_DATA* GetLayerAddr(u16 id, POOL_DATA* pool) {
  return pool == NULL ? NULL : GetPoolAddr(id, (MEM_DATA*)((u8*)pool + pool->layerOff));
}

static void InsertData(u16 id, void* data, u8 dataType, u32 remove) {
  MEM_DATA* m; // r30

  switch (dataType) {
  case 0:
    if (!remove) {
      if ((m = GetMacroAddr(id, data)) != NULL) {
        dataInsertMacro(id, &m->data.cmd);

      } else {
        dataInsertMacro(id, NULL);
      }
    } else {
      dataRemoveMacro(id);
    }
    break;
  case 2: {
    id |= 0x4000;
    if (!remove) {
      if ((m = GetKeymapAddr(id, data)) != NULL) {
        dataInsertKeymap(id, &m->data.map);
      } else {
        dataInsertKeymap(id, NULL);
      }
    } else {
      dataRemoveKeymap(id);
    }
  } break;
  case 3: {
    id |= 0x8000;
    if (!remove) {
      if ((m = GetLayerAddr(id, data)) != NULL) {
        dataInsertLayer(id, &m->data.layer.entry, m->data.layer.num);
      } else {
        dataInsertLayer(id, NULL, 0);
      }
    } else {
      dataRemoveLayer(id);
    }
  } break;
  case 4:
    if (!remove) {
      if ((m = GetCurveAddr(id, data)) != NULL) {
        dataInsertCurve(id, &m->data.tab);
      } else {
        dataInsertCurve(id, NULL);
      }
    } else {
      dataRemoveCurve(id);
    }
    break;
  case 1:
    if (!remove) {
#if MUSY_VERSION <= MUSY_VERSION_CHECK(2, 0, 0)
      dataAddSampleReference(id);
#else
      dataAddSampleReference(id, &gsCurrent->aramInfo);
#endif
    } else {
#if MUSY_VERSION <= MUSY_VERSION_CHECK(2, 0, 0)
      dataRemoveSampleReference(id);
#else
      dataRemoveSampleReference(id, &gsCurrent->aramInfo);
#endif
    }
    break;
  }
}

static void ScanIDList(u16* ref, void* data, u8 dataType, u32 remove) {
  u16 id; // r30

  while (*ref != 0xFFFF) {
    if ((*ref & 0x8000)) {
      id = *ref & 0x3fff;
      while (id <= ref[1]) {
        InsertData(id, data, dataType, remove);
        ++id;
      }
      ref += 2;

    } else {
      InsertData(*ref++, data, dataType, remove);
    }
  }
}

static void ScanIDListReverse(u16* refBase, void* data, u8 dataType, u32 remove) {
  s16 id;
  u16* ref;

  if (*refBase != 0xffff) {
    ref = refBase;
    while (*ref != 0xffff) {
      ref++;
    }
    ref--;

    while (ref >= refBase) {
      if (ref != refBase) {
        if ((ref[-1] & 0x8000) != 0) {
          id = *ref;
          while (id >= (s16)(ref[-1] & 0x3fff)) {
            InsertData(id, data, dataType, remove);
            id--;
          }
          ref -= 2;
        } else {
          InsertData(*ref, data, dataType, remove);
          ref--;
        }
      } else {
        InsertData(*ref, data, dataType, remove);
        ref--;
      }
    }
  }
}

static void InsertMacros(unsigned short* ref, void* pool) { ScanIDList(ref, pool, 0, 0); }

static void InsertCurves(unsigned short* ref, void* pool) { ScanIDList(ref, pool, 4, 0); }

static void InsertKeymaps(unsigned short* ref, void* pool) { ScanIDList(ref, pool, 2, 0); }

static void InsertLayers(unsigned short* ref, void* pool) { ScanIDList(ref, pool, 3, 0); }

static void RemoveMacros(unsigned short* ref) { ScanIDList(ref, NULL, 0, 1); }

static void RemoveCurves(unsigned short* ref) { ScanIDList(ref, NULL, 4, 1); }

static void RemoveKeymaps(unsigned short* ref) { ScanIDList(ref, NULL, 2, 1); }

static void RemoveLayers(unsigned short* ref) { ScanIDList(ref, NULL, 3, 1); }

static bool InsertSamples(u16* ref, void* samples, void* sdir, bool ownsSdir,
                          bool ownsSampleData) {
  samples = hwTransAddr(samples);
  if ((ownsSampleData ? dataInsertOwnedSDirAndSamples((SDIR_DATA*)sdir, samples)
                      : ownsSdir ? dataInsertOwnedSDir((SDIR_DATA*)sdir, samples)
                                 : dataInsertSDir((SDIR_DATA*)sdir, samples))) {
    ScanIDList(ref, sdir, 1, 0);
    return TRUE;
  }
  return FALSE;
}

static void RemoveSamples(unsigned short* ref, void* sdir) {
  ScanIDListReverse(ref, NULL, 1, 1);
  dataRemoveSDir(sdir);
}

static void InsertFXTab(unsigned short gid, FX_DATA* fd) { dataInsertFX(gid, fd->fx, fd->num); }

static void RemoveFXTab(unsigned short gid) { dataRemoveFX(gid); }

#if MUSY_TARGET == MUSY_TARGET_PC
static u16 swapU16(u16 value) {
  return (u16)((value << 8) | (value >> 8));
}

static u32 swapU32(u32 value) {
  return ((value & 0x000000ffU) << 24) | ((value & 0x0000ff00U) << 8) |
         ((value & 0x00ff0000U) >> 8) | ((value & 0xff000000U) >> 24);
}

static void swapIdList(u16* ref) {
  u16 raw;

  do {
    raw = *ref;
    *ref++ = swapU16(raw);
  } while (raw != 0xffff);
}

static void swapAdpcmInfo(DSPADPCMplusInfo* info, u8 compType) {
  u32 i;

  info->numCoef = swapU16(info->numCoef);
  info->loopY0 = (s16)swapU16((u16)info->loopY0);
  info->loopY1 = (s16)swapU16((u16)info->loopY1);
  for (i = 0; i < 8; ++i) {
    info->coefTab[i][0] = (s16)swapU16((u16)info->coefTab[i][0]);
    info->coefTab[i][1] = (s16)swapU16((u16)info->coefTab[i][1]);
  }

  /* Standard ADPCM metadata ends immediately after coefTab (0x28 bytes).
   * Only ADPCM-plus directories carry the checkpoint block which follows it.
   * Touching blk[0] for a standard sample corrupts the first four bytes of the
   * next sample's metadata (including its initial/loop predictor-scale bytes). */
  if (compType == 1) {
    info->blk[0].Y0 = (s16)swapU16((u16)info->blk[0].Y0);
    info->blk[0].Y1 = (s16)swapU16((u16)info->blk[0].Y1);
  }
}

static void swapSdirData(SDIR_DATA_INTER* sdir) {
  u16 rawId;
  u8* base = (u8*)sdir;
  u8 compType;
  SDIR_DATA_INTER* first = sdir;
  SDIR_DATA_INTER* previous;
  bool extraDataSwapped;

  do {
    rawId = sdir->id;
    sdir->id = swapU16(sdir->id);
    sdir->ref_cnt = swapU16(sdir->ref_cnt);

    /* The on-disk terminator contains only id/ref_cnt.  The ADPCM metadata
     * area starts four bytes later, so swapping the rest of a fictitious
     * terminator entry would overwrite the first sample's decoder state. */
    if (rawId == 0xffff) {
      break;
    }

    sdir->offset = swapU32(sdir->offset);
    sdir->addr = swapU32(sdir->addr);
    sdir->header.info = swapU32(sdir->header.info);
    sdir->header.length = swapU32(sdir->header.length);
    sdir->header.loopOffset = swapU32(sdir->header.loopOffset);
    sdir->header.loopLength = swapU32(sdir->header.loopLength);
    sdir->extraData = swapU32(sdir->extraData);
    compType = (u8)(sdir->header.length >> 24);
    extraDataSwapped = FALSE;
    for (previous = first; previous < sdir; ++previous) {
      if (previous->extraData == sdir->extraData) {
        extraDataSwapped = TRUE;
        break;
      }
    }
    if (!extraDataSwapped && sdir->extraData != 0 &&
        (compType == 0 || compType == 1 || compType == 4 || compType == 5)) {
      swapAdpcmInfo((DSPADPCMplusInfo*)(base + sdir->extraData), compType);
    }
    ++sdir;
  } while (rawId != 0xffff);
}

static u32 getSampleDataSize(const SDIR_DATA_INTER* sdir) {
  u32 size = 0;

  for (; sdir->id != 0xffff; ++sdir) {
    u32 length = sdir->header.length & 0x00ffffff;
    u32 encodedLength;
    u32 end;

    switch (sdir->header.length >> 24) {
      case 0:
      case 1:
      case 4:
      case 5:
        encodedLength = ((length + 13) / 14) * 8;
        break;
      case 2:
      case 6:
        encodedLength = length * 2;
        break;
      case 3:
        encodedLength = length;
        break;
      default:
        encodedLength = 0;
        break;
    }
    end = sdir->offset + encodedLength;
    if (end > size) {
      size = end;
    }
  }
  return size;
}

static void swapPCM16SampleData(void* samples, u32 sampleDataSize,
                                const SDIR_DATA_INTER* sdir) {
  u8* sampleBytes = samples;

  if (samples == NULL) {
    return;
  }

  for (; sdir->id != 0xffff; ++sdir) {
    const u8 compType = (u8)(sdir->header.length >> 24);
    const u32 length = sdir->header.length & 0x00ffffff;
    const u32 offset = sdir->offset;

    /* Static PCM16 samples are stored big-endian in the GameCube bank. */
    if (compType != 2 && compType != 6) {
      continue;
    }
    if (offset > sampleDataSize || length > (sampleDataSize - offset) / sizeof(u16)) {
      continue;
    }
    for (u32 i = 0; i < length; ++i) {
      u8* sample = sampleBytes + offset + i * sizeof(u16);
      const u8 high = sample[0];
      sample[0] = sample[1];
      sample[1] = high;
    }
  }
}

static void swapPageData(PAGE* page) {
  do {
    page->macro = swapU16(page->macro);
    ++page;
  } while (page[-1].index != 0xff);
}

static void swapMidiSetups(MIDISETUP* setup) {
  u16 rawSongId;

  do {
    rawSongId = setup->songId;
    setup->songId = swapU16(setup->songId);
    setup->reserved = swapU16(setup->reserved);
    ++setup;
  } while (rawSongId != 0xffff);
}

static bool swapMarkOffset(u32* offsets, u32* count, u32 capacity, u32 offset) {
  u32 i;

  for (i = 0; i < *count; ++i) {
    if (offsets[i] == offset) {
      return FALSE;
    }
  }
  if (*count < capacity) {
    offsets[(*count)++] = offset;
  }
  return TRUE;
}

static void swapMemList(MEM_DATA* data, u32 type) {
  u32 payloadSize;
  u32 i;
  u32 count;

  while (data->nextOff != 0xffffffff) {
    data->nextOff = swapU32(data->nextOff);
    data->id = swapU16(data->id);

    if (data->nextOff > 0x100000) {
      return;
    }
    if (data->nextOff < 8) {
      return;
    }
    payloadSize = data->nextOff - 8;

    switch (type) {
      case 0:  // Macro commands are 32-bit words.
        for (i = 0; i < payloadSize / sizeof(u32); ++i) {
          ((u32*)&data->data)[i] = swapU32(((u32*)&data->data)[i]);
        }
        break;
      case 2:  // Keymaps
        for (i = 0; i < payloadSize / sizeof(data->data.map[0]); ++i) {
          data->data.map[i].id = swapU16(data->data.map[i].id);
          data->data.map[i].prioOffset = (s16)swapU16((u16)data->data.map[i].prioOffset);
        }
        break;
      case 3:  // Layers
        if (payloadSize < sizeof(data->data.layer.num)) {
          return;
        }
        data->data.layer.num = swapU32(data->data.layer.num);
        count = data->data.layer.num;
        if (count > (payloadSize - sizeof(data->data.layer.num)) / sizeof(LAYER)) {
          count = (payloadSize - sizeof(data->data.layer.num)) / sizeof(LAYER);
        }
        for (i = 0; i < count; ++i) {
          data->data.layer.entry[i].id = swapU16(data->data.layer.entry[i].id);
          data->data.layer.entry[i].prioOffset =
              (s16)swapU16((u16)data->data.layer.entry[i].prioOffset);
        }
        break;
    }

    data = (MEM_DATA*)((u8*)data + data->nextOff);
  }
}

static void swapPoolData(POOL_DATA* pool) {
  pool->macroOff = swapU32(pool->macroOff);
  pool->curveOff = swapU32(pool->curveOff);
  pool->keymapOff = swapU32(pool->keymapOff);
  pool->layerOff = swapU32(pool->layerOff);
  if (pool->macroOff != 0) {
    swapMemList((MEM_DATA*)((u8*)pool + pool->macroOff), 0);
  }
  if (pool->curveOff != 0) {
    swapMemList((MEM_DATA*)((u8*)pool + pool->curveOff), 1);
  }
  if (pool->keymapOff != 0) {
    swapMemList((MEM_DATA*)((u8*)pool + pool->keymapOff), 2);
  }
  if (pool->layerOff != 0) {
    swapMemList((MEM_DATA*)((u8*)pool + pool->layerOff), 3);
  }
}

static void swapFxData(FX_DATA* fxData) {
  u32 i;

  fxData->num = swapU16(fxData->num);
  for (i = 0; i < fxData->num; ++i) {
    fxData->fx[i].id = swapU16(fxData->fx[i].id);
    fxData->fx[i].macro = swapU16(fxData->fx[i].macro);
  }
}

static void swapProjectData(void* prjData, void* sdirData, void* poolData) {
  GROUP_DATA* group = prjData;
  u32 listOffsets[640];
  u32 pageOffsets[256];
  u32 midiOffsets[128];
  u32 fxOffsets[128];
  u32 listCount = 0;
  u32 pageCount = 0;
  u32 midiCount = 0;
  u32 fxCount = 0;

  swapSdirData((SDIR_DATA_INTER*)sdirData);
  swapPoolData((POOL_DATA*)poolData);

  while (group->nextOff != 0xffffffff) {
    group->nextOff = swapU32(group->nextOff);
    group->id = swapU16(group->id);
    group->type = swapU16(group->type);
    group->macroOff = swapU32(group->macroOff);
    group->sampleOff = swapU32(group->sampleOff);
    group->curveOff = swapU32(group->curveOff);
    group->keymapOff = swapU32(group->keymapOff);
    group->layerOff = swapU32(group->layerOff);
    group->data.song.normpageOff = swapU32(group->data.song.normpageOff);
    group->data.song.drumpageOff = swapU32(group->data.song.drumpageOff);
    group->data.song.midiSetupOff = swapU32(group->data.song.midiSetupOff);

    if (group->macroOff != 0 &&
        swapMarkOffset(listOffsets, &listCount, 640, group->macroOff)) {
      swapIdList((u16*)((u8*)prjData + group->macroOff));
    }
    if (group->sampleOff != 0 &&
        swapMarkOffset(listOffsets, &listCount, 640, group->sampleOff)) {
      swapIdList((u16*)((u8*)prjData + group->sampleOff));
    }
    if (group->curveOff != 0 &&
        swapMarkOffset(listOffsets, &listCount, 640, group->curveOff)) {
      swapIdList((u16*)((u8*)prjData + group->curveOff));
    }
    if (group->keymapOff != 0 &&
        swapMarkOffset(listOffsets, &listCount, 640, group->keymapOff)) {
      swapIdList((u16*)((u8*)prjData + group->keymapOff));
    }
    if (group->layerOff != 0 &&
        swapMarkOffset(listOffsets, &listCount, 640, group->layerOff)) {
      swapIdList((u16*)((u8*)prjData + group->layerOff));
    }
    if (group->type == 1 && group->data.song.normpageOff != 0 &&
        swapMarkOffset(fxOffsets, &fxCount, 128, group->data.song.normpageOff)) {
      swapFxData((FX_DATA*)((u8*)prjData + group->data.song.normpageOff));
    }
    if (group->type == 0) {
      if (group->data.song.normpageOff != 0 &&
          swapMarkOffset(pageOffsets, &pageCount, 256, group->data.song.normpageOff)) {
        swapPageData((PAGE*)((u8*)prjData + group->data.song.normpageOff));
      }
      if (group->data.song.drumpageOff != 0 &&
          swapMarkOffset(pageOffsets, &pageCount, 256, group->data.song.drumpageOff)) {
        swapPageData((PAGE*)((u8*)prjData + group->data.song.drumpageOff));
      }
      if (group->data.song.midiSetupOff != 0 &&
          swapMarkOffset(midiOffsets, &midiCount, 128, group->data.song.midiSetupOff)) {
        swapMidiSetups((MIDISETUP*)((u8*)prjData + group->data.song.midiSetupOff));
      }
    }

    group = (GROUP_DATA*)((u8*)prjData + group->nextOff);
  }
}
#endif

void sndSetSampleDataUploadCallback(void* (*callback)(u32, u32), u32 chunckSize) {
  hwSetSaveSampleCallback(callback, chunckSize);
}

bool sndRegisterSampleDirectory(void* samples, void* sampdir) {
#if MUSY_TARGET == MUSY_TARGET_PC
  SDIR_DATA* converted;
  u32 sampleDataSize;

  if (samples == NULL || sampdir == NULL) {
    return FALSE;
  }

  swapSdirData((SDIR_DATA_INTER*)sampdir);
  sampleDataSize = getSampleDataSize((const SDIR_DATA_INTER*)sampdir);
  swapPCM16SampleData(samples, sampleDataSize, (const SDIR_DATA_INTER*)sampdir);
  converted = sndConvert32BitSDIRTo64BitSDIR(sampdir);
  if (converted == NULL) {
    return FALSE;
  }
  if (!dataInsertOwnedSDirAndSamples(converted, hwTransAddr(samples))) {
    free(converted);
    return FALSE;
  }
  return TRUE;
#else
  return dataInsertSDir((SDIR_DATA*)sampdir, hwTransAddr(samples));
#endif
}

bool sndPushGroup(void* prj_data, u16 gid, void* samples, void* sdir, void* pool) {
  GROUP_DATA* g; // r31
#if MUSY_TARGET == MUSY_TARGET_PC
  void* sampleCopy;
  u32 sampleDataSize;
#endif
  MUSY_ASSERT_MSG(prj_data != NULL, "Project data pointer is NULL");
  MUSY_ASSERT_MSG(sdir != NULL, "Sample directory pointer is NULL");

  if (sndActive && SP_CURRENT < 128) {
    g = prj_data;

#if MUSY_TARGET == MUSY_TARGET_PC
    swapProjectData(prj_data, sdir, pool);
#endif

    while (g->nextOff != 0xFFFFFFFF) {
      if (g->id == gid) {
#if MUSY_TARGET == MUSY_TARGET_PC
        sampleDataSize = getSampleDataSize((const SDIR_DATA_INTER*)sdir);
        sampleCopy = NULL;
        if (sampleDataSize != 0) {
          if (samples == NULL || (sampleCopy = malloc(sampleDataSize)) == NULL) {
            return FALSE;
          }
          memcpy(sampleCopy, samples, sampleDataSize);
          swapPCM16SampleData(sampleCopy, sampleDataSize, (const SDIR_DATA_INTER*)sdir);
          samples = sampleCopy;
        }
        sdir = sndConvert32BitSDIRTo64BitSDIR(sdir);
        if (sdir == NULL) {
          free(sampleCopy);
          return FALSE;
        }
#endif
        GS_CURRENT[SP_CURRENT].gAddr = g;
        GS_CURRENT[SP_CURRENT].prjAddr = prj_data;
        GS_CURRENT[SP_CURRENT].sdirAddr = sdir;
        if (!InsertSamples((u16*)((u8*)prj_data + g->sampleOff), samples, sdir,
#if MUSY_TARGET == MUSY_TARGET_PC
                           TRUE, sampleCopy != NULL)) {
#else
                           FALSE, FALSE)) {
#endif
#if MUSY_TARGET == MUSY_TARGET_PC
          free(sdir);
          free(sampleCopy);
#endif
          return FALSE;
        }
        InsertMacros((u16*)((u8*)prj_data + g->macroOff), pool);
        InsertCurves((u16*)((u8*)prj_data + g->curveOff), pool);
        InsertKeymaps((u16*)((u8*)prj_data + g->keymapOff), pool);
        InsertLayers((u16*)((u8*)prj_data + g->layerOff), pool);
        if (g->type == 1) {
          InsertFXTab(gid, (FX_DATA*)((u8*)prj_data + g->data.song.normpageOff));
        }
        hwSyncSampleMem();
        ++SP_CURRENT;
        return TRUE;
      }

      g = (GROUP_DATA*)((u8*)prj_data + g->nextOff);
    }
  }

  MUSY_DEBUG("Group ID=%d could not be pushed.\n", gid);
  return FALSE;
}

/*










*/
bool sndPopGroup() {
  GROUP_DATA* g;
  SDIR_DATA* sdir;
  void* prj;
  FX_DATA* fd;

  // TODO workaround for shutdown crash
  // MUSY_ASSERT_MSG(sndActive != FALSE, "Sound system is not initialized.");
  // MUSY_ASSERT_MSG(SP_CURRENT != 0, "Soundstack is empty.");
  if (!sndActive || SP_CURRENT == 0) return FALSE;
  g = GS_CURRENT[--SP_CURRENT].gAddr;
  prj = GS_CURRENT[SP_CURRENT].prjAddr;
  sdir = GS_CURRENT[SP_CURRENT].sdirAddr;
  hwDisableIrq();

  if (g->type == 1) {
    fd = (FX_DATA*)((u8*)prj + g->data.song.normpageOff);
    s3dKillEmitterByFXID(fd->fx, fd->num);
  } else {
    seqKillInstancesByGroupID(g->id);
  }

  synthKillVoicesByMacroReferences((u16*)((u8*)prj + g->macroOff));
#if MUSY_VERSION >= MUSY_VERSION_CHECK(2, 0, 1)
  synthKillVoicesBySampleReferences((u16*)((u8*)prj + g->sampleOff));
#endif
  hwEnableIrq();
  RemoveSamples((u16*)((u8*)prj + g->sampleOff), sdir);
  RemoveMacros((u16*)((u8*)prj + g->macroOff));
  RemoveCurves((u16*)((u8*)prj + g->curveOff));
  RemoveKeymaps((u16*)((u8*)prj + g->keymapOff));
  RemoveLayers((u16*)((u8*)prj + g->layerOff));
  if (g->type == 1) {
    RemoveFXTab(g->id);
  }
  return TRUE;
}

/*












*/

#if MUSY_VERSION >= MUSY_VERSION_CHECK(2, 0, 2)
u32 sndStackGetSize() { return 0x618; }

u32 sndStackAdd(void* stackWorkMem, u32 aramBase, u32 aramSize) {
  GSTACK_INST* gs; // r31
  u32 id;          // r30

  MUSY_ASSERT_MSG(sndActive != FALSE, "Sound system is not initialized.");
  MUSY_ASSERT_MSG((aramBase & 31) == 0, "ARAM area base must be 32-byte aligned.");
  MUSY_ASSERT_MSG((aramSize & 31) == 0, "ARAM area size must be a multiple of 32 bytes.");
  do {
    for (;;) {
      id = gsNextID;
      gsNextID = id + 1;
      if (id == -2) {
        continue;
      }
      if (id == -1) {
        continue;
      }
      break;
    }
    for (gs = gsRoot; gs != NULL; gs = gs->next) {
      if (gs->id == id) {
        break;
      }
    }
  } while (gs != NULL);
  dataInitStackInstance(stackWorkMem, id, aramBase, aramSize);
  return id;
}

u32 sndStackRemove(u32 id) {
  GSTACK_INST* gs;  // r31
  GSTACK_INST* lgs; // r29

  MUSY_ASSERT_MSG(sndActive != FALSE, "Sound system is not initialized.");
  MUSY_ASSERT_MSG(id != -2, "Default sound stack cannot be removed.");

  for (lgs = NULL, gs = gsRoot; gs != NULL; lgs = gs, gs = gs->next) {
    if (gs->id == id) {
      MUSY_ASSERT_MSG(gs->sp == 0, "Sound stack is not empty.");
      if (lgs == NULL) {
        gsRoot = gs->next;
      } else {
        lgs->next = gs->next;
      }
      return 1;
    }
  }
  return 0;
}

u32 sndStackSetCurrent(u32 id) {
  GSTACK_INST* gs; // r31

  MUSY_ASSERT_MSG(sndActive != FALSE, "Sound system is not initialized.");
  for (gs = gsRoot; gs != NULL; gs = gs->next) {
    if (gs->id == id) {
      gsCurrent = gs;
      return 1;
    }
  }
  return 0;
}

u32 sndStackGetARAMAddressRange(u32 id, u32* start, u32* end) {
  GSTACK_INST* gs; // r31

  MUSY_ASSERT_MSG(sndActive != FALSE, "Sound system is not initialized.");
  for (gs = gsRoot; gs != NULL; gs = gs->next) {
    if (gs->id == id) {
      *start = gs->aramInfo.aramBase;
      *end = gs->aramInfo.aramTop;
      return 1;
    }
  }
  return 0;
}

u32 sndStackGetAvailableSampleMemory(unsigned long id) {
  GSTACK_INST* gs; // r31

  MUSY_ASSERT_MSG(sndActive != FALSE, "Sound system is not initialized.");
  for (gs = gsRoot; gs != NULL; gs = gs->next) {
    if (gs->id == id) {
      return hwGetAvailableSampleMemory(&gs->aramInfo);
    }
  }
  return 0;
}
#endif

u32 seqPlaySong(u16 sgid, u16 sid, void* arrfile, SND_PLAYPARA* para, u8 irq_call, u8 studio) {
  int i;
  GROUP_DATA* g;
  PAGE* norm;
  PAGE* drum;
  MIDISETUP* midiSetup;
  u32 seqId;
  void* prj;
#if MUSY_VERSION >= MUSY_VERSION_CHECK(2, 0, 1)
  GSTACK_INST* gsi;
#endif
  MUSY_ASSERT_MSG(sndActive != FALSE, "Sound system is not initialized.");

#if MUSY_VERSION >= MUSY_VERSION_CHECK(2, 0, 1)
  for (gsi = gsRoot; gsi != NULL; gsi = gsi->next) {
#endif
    for (i = 0; i < SP_GSI; ++i) {
      if (GS_GSI[i].gAddr->id != sgid) {
        continue;
      }

      if (GS_GSI[i].gAddr->type == 0) {
        g = GS_GSI[i].gAddr;
        prj = GS_GSI[i].prjAddr;
        norm = (PAGE*)((size_t)prj + g->data.song.normpageOff);
        drum = (PAGE*)((size_t)prj + g->data.song.drumpageOff);
        midiSetup = (MIDISETUP*)((size_t)prj + g->data.song.midiSetupOff);
        while (midiSetup->songId != 0xFFFF) {
          if (midiSetup->songId == sid) {
            if (irq_call != 0) {
              seqId = seqStartPlay(norm, drum, midiSetup, arrfile, para, studio, sgid);
            } else {
              hwDisableIrq();
              seqId = seqStartPlay(norm, drum, midiSetup, arrfile, para, studio, sgid);
              hwEnableIrq();
            }
            return seqId;
          }

          ++midiSetup;
        }

#if MUSY_VERSION <= MUSY_VERSION_CHECK(2, 0, 1)
        MUSY_DEBUG("Song ID=%d is not in group ID=%d.", sid, sgid);
#else
      MUSY_DEBUG("Song ID=%d is not in group ID=%d.\n", sid, sgid);
#endif
        return 0xffffffff;
      } else {
#if MUSY_VERSION <= MUSY_VERSION_CHECK(2, 0, 1)
        MUSY_DEBUG("Group ID=%d is no songgroup.", sgid);
#else
      MUSY_DEBUG("Group ID=%d is no songgroup.\n", sgid);
#endif
        return 0xffffffff;
      }
    }
#if MUSY_VERSION >= MUSY_VERSION_CHECK(2, 0, 1)
  }
#endif

#if MUSY_VERSION <= MUSY_VERSION_CHECK(2, 0, 0)
  MUSY_DEBUG("Group ID=%d is not on soundstack.", sgid);
#else
  MUSY_DEBUG("Group ID=%d is not on any soundstack.\n", sgid);
#endif
  return 0xffffffff;
}

u32 sndSeqPlayEx(u16 sgid, u16 sid, void* arrfile, SND_PLAYPARA* para, u8 studio) {
  return seqPlaySong(sgid, sid, arrfile, para, 0, studio);
}
