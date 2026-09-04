#include "msm/msmmus.h"
#include "msm/msmmem.h"
#include "musyx/seq.h"

#ifdef BYTESWAPPING
#include "port/byteswap.h"

static void msmMusSwapData(MSM_MUS *musData, u32 count)
{
    u32 i;

    for (i = 0; i < count; i++) {
        byteswap_u16(&musData[i].sgid);
        byteswap_u16(&musData[i].sid);
        byteswap_s32(&musData[i].songOfs);
        byteswap_s32(&musData[i].songSize);
    }
}

static BOOL msmMusSongOffsetValid(u32 offset, u32 size, u32 needed)
{
    return offset <= size && needed <= size - offset;
}

static void msmMusSwapSongData(void *songData, u32 songSize)
{
    ARR *arr;
    TENTRY *entry;
    SEQ_PATTERN *pattern;
    NOTE_DATA *note;
    MTRACK_DATA *tempo;
    u32 *trackTable;
    u32 *patternTable;
    u32 maxPattern;
    u32 patternId;
    u32 trackId;
    u32 entryOffset;
    u32 entryEnd;
    u32 patternOffset;
    u32 patternEnd;
    u32 nextOffset;
    u8 *base;

    if (songData == NULL || songSize < sizeof(*arr)) {
        return;
    }
    arr = songData;
    /* A native offset is small; a big-endian on-disk offset is not on x86. */
    if (arr->tTab != 0 && arr->tTab < songSize) {
        return;
    }

    byteswap_u32(&arr->tTab);
    byteswap_u32(&arr->pTab);
    byteswap_u32(&arr->tmTab);
    byteswap_u32(&arr->mTrack);
    byteswap_u32(&arr->info);
    if ((arr->info & 0x80000000) != 0) {
        for (trackId = 0; trackId < 16; trackId++) {
            byteswap_u32(&arr->loopPoint[trackId]);
        }
        byteswap_u32(&arr->tsTab);
    } else {
        byteswap_u32(&arr->loopPoint[0]);
    }

    base = songData;
    if (!msmMusSongOffsetValid(arr->tTab, songSize, 64 * sizeof(*trackTable)) ||
        !msmMusSongOffsetValid(arr->pTab, songSize, sizeof(*patternTable))) {
        return;
    }

    trackTable = (u32 *)(base + arr->tTab);
    maxPattern = 0;
    for (trackId = 0; trackId < 64; trackId++) {
        byteswap_u32(&trackTable[trackId]);
    }

    for (trackId = 0; trackId < 64; trackId++) {
        entryOffset = trackTable[trackId];
        if (entryOffset == 0 || entryOffset >= arr->pTab ||
            !msmMusSongOffsetValid(entryOffset, songSize, sizeof(*entry))) {
            continue;
        }
        entryEnd = arr->pTab;
        for (nextOffset = 0; nextOffset < 64; nextOffset++) {
            u32 candidate = trackTable[nextOffset];
            if (candidate > entryOffset && candidate < entryEnd) {
                entryEnd = candidate;
            }
        }
        entry = (TENTRY *)(base + entryOffset);
        while ((u8 *)entry + sizeof(*entry) <= base + entryEnd) {
            byteswap_u32(&entry->time);
            byteswap_u16(&entry->pattern);
            if (entry->pattern == 0xfffe) {
                byteswap_u16((u16 *)&entry->transpose);
                break;
            }
            if (entry->pattern == 0xffff) {
                break;
            }
            if (entry->pattern < 0xfffe && entry->pattern > maxPattern) {
                maxPattern = entry->pattern;
            }
            ++entry;
        }
    }

    if (maxPattern > 2048 ||
        !msmMusSongOffsetValid(arr->pTab, songSize, (maxPattern + 1) * sizeof(*patternTable))) {
        return;
    }
    patternTable = (u32 *)(base + arr->pTab);
    for (patternId = 0; patternId <= maxPattern; patternId++) {
        byteswap_u32(&patternTable[patternId]);
    }

    for (patternId = 0; patternId <= maxPattern; patternId++) {
        patternOffset = patternTable[patternId];
        if (patternOffset == 0 || !msmMusSongOffsetValid(patternOffset, songSize, sizeof(*pattern))) {
            continue;
        }
        patternEnd = songSize;
        for (nextOffset = 0; nextOffset <= maxPattern; nextOffset++) {
            u32 candidate = patternTable[nextOffset];
            if (candidate > patternOffset && candidate < patternEnd) {
                patternEnd = candidate;
            }
        }
        pattern = (SEQ_PATTERN *)(base + patternOffset);
        byteswap_u32(&pattern->headerLen);
        byteswap_u32(&pattern->pitchBend);
        byteswap_u32(&pattern->modulation);
        if (pattern->pitchBend > patternOffset && pattern->pitchBend < patternEnd) {
            patternEnd = pattern->pitchBend;
        }
        if (pattern->modulation > patternOffset && pattern->modulation < patternEnd) {
            patternEnd = pattern->modulation;
        }

        note = (NOTE_DATA *)&pattern->noteData;
        while ((u8 *)note + 4 <= base + patternEnd) {
            byteswap_u16(&note->time);
            if (note->key == 0xff && note->velocity == 0xff) {
                break;
            }
            if ((note->key & 0x80) != 0 || (note->key | note->velocity) == 0) {
                note = (NOTE_DATA *)((u8 *)note + 4);
            } else {
                if ((u8 *)note + sizeof(*note) > base + patternEnd) {
                    break;
                }
                byteswap_u16(&note->length);
                ++note;
            }
        }
    }

    if (arr->mTrack != 0 && msmMusSongOffsetValid(arr->mTrack, songSize, sizeof(*tempo))) {
        tempo = (MTRACK_DATA *)(base + arr->mTrack);
        while ((u8 *)tempo + sizeof(*tempo) <= base + songSize) {
            byteswap_u32((u32 *)&tempo->time);
            byteswap_u32(&tempo->bpm);
            if (tempo->time == -1) {
                break;
            }
            ++tempo;
        }
    }
}
#endif

typedef struct MusPlayer_s {
    /* 0x00 */ s16 musId;
    /* 0x02 */ s8 baseVol;
    /* 0x03 */ s8 vol;
    /* 0x04 */ s8 status;
    /* 0x05 */ s8 busyF;
    /* 0x08 */ SND_SEQID seqId;
    /* 0x0C */ void* songBuf;
    /* 0x10 */ void* arrfile;
    /* 0x14 */ volatile s32 fadeMaxTime;
    /* 0x18 */ s32 fadeTime;
    /* 0x1C */ s8 fadeVol;
    /* 0x20 */ volatile s32 pauseOnMaxTime;
    /* 0x24 */ s32 pauseOnTime;
    /* 0x28 */ s8 pauseOnVol;
    /* 0x2C */ volatile s32 pauseOffMaxTime;
    /* 0x30 */ s32 pauseOffTime;
    /* 0x28 */ s8 pauseOffVol;
    /* 0x35 */ volatile s8 startVol;
    /* 0x36 */ s8 targetVol;
    /* 0x38 */ SND_PLAYPARA playPara;
} MUS_PLAYER; // Size 0x58

static struct {
    /* 0x000 */ s16 musMax;
    /* 0x002 */ s8 musChanMax;
    /* 0x003 */ s8 baseGrpNumPlay;
    /* 0x004 */ s8 numPlay;
    /* 0x008 */ s32 msmEntryNum;
    /* 0x00C */ MSM_MUS *musData;
    /* 0x010 */ s32 dummyMusOfs;
    /* 0x014 */ void* musBuf;
    /* 0x018 */ MUS_PLAYER player[4];
} mus;

static void msmMusPauseSub(MUS_PLAYER* player, BOOL pause, s32 speed) {
    s32 time;

    time = speed / 15;
    if (pause) {
        if (player->status == MSM_MUS_PLAY) {
            if (time != 0) {
                player->pauseOnTime = time;
                player->pauseOnMaxTime = time;
                player->pauseOnVol = 127;
            } else {
                sndSeqPause(player->seqId);
                player->pauseOnVol = 0;
            }
            player->status = MSM_MUS_PAUSE;
        }
    } else {
        if (player->status == MSM_MUS_PAUSE) {
            if (time != 0) {
                if (player->pauseOnMaxTime != 0) {
                    player->pauseOnMaxTime = -time;
                } else {
                    sndSeqContinue(player->seqId);
                    player->pauseOnMaxTime = -time;
                    player->pauseOnTime = 0;
                    player->pauseOnVol = 0;
                }
            } else {
                sndSeqContinue(player->seqId);
                player->pauseOnVol = 127;
                sndSeqVolume(player->baseVol * player->vol * player->fadeVol * player->pauseOnVol / (127*127*127), 0, player->seqId, 0);
            }
            player->status = MSM_MUS_PLAY;
        }
    }
}

static void msmMusPauseFade(MUS_PLAYER *player) {
    if (player->pauseOnMaxTime > 0) {
        if (--player->pauseOnTime == 0) {
            player->pauseOnMaxTime = 0;
            player->pauseOnVol = 0;
            sndSeqPause(player->seqId);
        } else {
            player->pauseOnVol = player->pauseOnTime * 127 / player->pauseOnMaxTime;
            sndSeqVolume(player->baseVol * player->vol * player->fadeVol * player->pauseOnVol / (127*127*127), 0, player->seqId, 0);
        }
    } else if (player->pauseOnMaxTime < 0) {
        if (++player->pauseOnTime >= -player->pauseOnMaxTime) {
            player->pauseOnMaxTime = 0;
            player->pauseOnVol = 127;
        } else {
            player->pauseOnVol = player->pauseOnTime * 127 / -player->pauseOnMaxTime;
        }
        sndSeqVolume(player->baseVol * player->vol * player->fadeVol * player->pauseOnVol / (127*127*127), 0, player->seqId, 0);
    }
}

static void msmMusFade(MUS_PLAYER *player) {
    if (player->status != MSM_MUS_PLAY) {
        return;
    }
    if (player->fadeMaxTime > 0) {
        if (--player->fadeTime == 0) {
            if (player->status != 0) {
                player->fadeMaxTime = 0;
                if (player->fadeMaxTime != 0) {
                    player->fadeTime = player->fadeMaxTime;
                    player->fadeVol = 127;
                } else {
                    sndSeqStop(player->seqId);
                    player->status = 1;
                }
            }
        } else {
            player->fadeVol = player->fadeTime * 127 / player->fadeMaxTime;
            sndSeqVolume(player->baseVol * player->vol * player->fadeVol * player->pauseOnVol / (127*127*127), 0, player->seqId, 0);
        }
    } else if (player->fadeMaxTime < 0) {
        if (++player->fadeTime >= -player->fadeMaxTime) {
            player->fadeMaxTime = 0;
            player->fadeVol = 127;
        } else {
            player->fadeVol = player->fadeTime * 127 / -player->fadeMaxTime;
        }
        sndSeqVolume(player->baseVol * player->vol * player->fadeVol * player->pauseOnVol / (127*127*127), 0, player->seqId, 0);
    }
}

void msmMusFdoutEnd(void) {
}

void msmMusPeriodicProc(void) {
    s32 i;
    MUS_PLAYER* player;

    mus.numPlay = mus.baseGrpNumPlay = 0;
    for (i = 0; i < mus.musChanMax; i++) {
        player = &mus.player[i];
        if (player->busyF != FALSE) {
            continue;
        }
        switch (player->status) {
            case MSM_MUS_STOP:
            case MSM_MUS_PLAY:
                if (sndSeqGetValid(player->seqId) == FALSE) {
                    player->status = 0;
                }
                break;
        }
        if (player->status == MSM_MUS_DONE) {
            continue;
        }
        if (msmSysCheckBaseGroup(mus.musData[player->musId].sgid) != 0) {
            mus.numPlay++;
        } else {
            mus.baseGrpNumPlay++;
        }
        msmMusPauseFade(player);
        if (player->status == MSM_MUS_PLAY && player->pauseOffMaxTime != 0) {
            if (++player->pauseOffTime >= player->pauseOffMaxTime) {
                player->pauseOffMaxTime = 0;
                player->baseVol = player->targetVol;
            } else {
                player->baseVol = player->startVol + player->pauseOffTime * (player->targetVol - player->startVol) / player->pauseOffMaxTime;
            }
            sndSeqVolume(player->baseVol * player->vol * player->fadeVol * player->pauseOnVol / (127*127*127), 0, player->seqId, 0);
        }
        msmMusFade(player);
    }
}

s32 msmMusGetMidiCtrl(int musNo, s32 channel, s32 ctrl) {
    MUS_PLAYER *player;

    if (musNo < 0 || musNo >= mus.musChanMax) {
        return MSM_ERR_OUTOFMUS;
    }
    player = &mus.player[musNo];
    if (sndSeqGetValid(player->seqId) == TRUE) {
        return sndSeqGetMidiCtrl(player->seqId, channel & 0xF, ctrl);
    }
    return 0;
}

s32 msmMusGetNumPlay(BOOL baseGrp) {
    switch (baseGrp) {
        case FALSE:
            return mus.baseGrpNumPlay + mus.numPlay;
        default:
            return mus.baseGrpNumPlay;
    }
}

s32 msmMusGetStatus(int musNo) {
    if (musNo < 0 || musNo >= mus.musChanMax) {
        return MSM_ERR_OUTOFMUS;
    }
    return mus.player[musNo].status;
}

void msmMusSetMasterVolume(s32 vol) {
    sndMasterVolume(vol & 127, 0, 1, 0);
}

s32 msmMusSetParam(s32 musNo, MSM_MUSPARAM *param) {
    MUS_PLAYER* player;

    if (musNo < 0 || musNo >= mus.musChanMax) {
        return MSM_ERR_OUTOFMUS;
    }
    player = &mus.player[musNo];
    if (param->flag & MSM_MUSPARAM_VOL) {
        player->pauseOffMaxTime = param->fadeSpeed / 15;
        if (player->pauseOffMaxTime != 0) {
            player->busyF = 1;
            player->pauseOffTime = 0;
            player->startVol = player->baseVol;
            player->targetVol = param->vol & 127;
            player->busyF = 0;
        } else {
            player->baseVol = param->vol & 127;
            sndSeqVolume(player->baseVol * player->vol * player->fadeVol * player->pauseOnVol / (127*127*127), 0, player->seqId, 0);
        }
    }
    if (param->flag & MSM_MUSPARAM_SPEED) {
        sndSeqSpeed(player->seqId, (param->speed << 8) / 100);
    }
    return 0;
}

void msmMusPauseAll(BOOL pause, s32 speed) {
    s32 var_r27;
    MUS_PLAYER* player;

    for (var_r27 = 0; var_r27 < mus.musChanMax; var_r27++) {
        player = &mus.player[var_r27];
        if (player->status != MSM_MUS_DONE) {
            player->busyF = TRUE;
            msmMusPauseSub(player, pause, speed);
            player->busyF =  FALSE;
        }
    }
}

s32 msmMusPause(int musNo, BOOL pause, s32 speed) {
    MUS_PLAYER *player;

    if (musNo < 0 || musNo >= mus.musChanMax) {
        return MSM_ERR_OUTOFMUS;
    }
    player = &mus.player[musNo];
    player->busyF = TRUE;
    msmMusPauseSub(player, pause, speed);
    player->busyF = FALSE;
    return 0;
}

void msmMusStopAll(BOOL checkGrp, s32 speed) {
    s32 i;
    MUS_PLAYER *player;

    for (i = 0; i < mus.musChanMax; i++) {
        player = &mus.player[i];
        if (player->status == 0) {
            continue;
        }
        player->busyF = TRUE;
        switch (checkGrp) {
            case FALSE:
                if (player->status != 0) {
                    player->fadeMaxTime = speed / 15;
                    if (player->fadeMaxTime != 0) {
                        player->fadeTime = player->fadeMaxTime;
                        player->fadeVol = 127;
                    } else {
                        sndSeqStop(player->seqId);
                        player->status = 1;
                    }
                }
                break;
            default:
                if (msmSysCheckBaseGroup(mus.musData[player->musId].sgid) == 0 && player->status != 0) {
                    player->fadeMaxTime = speed / 15;
                    if (player->fadeMaxTime != 0) {
                        player->fadeTime = player->fadeMaxTime;
                        player->fadeVol = 127;
                    } else {
                        sndSeqStop(player->seqId);
                        player->status = 1;
                    }
                }
                break;
        }
        player->busyF = FALSE;
    }
}

s32 msmMusStop(int musNo, s32 speed) {
    MUS_PLAYER* player;

    if (musNo < 0 || musNo >= mus.musChanMax) {
        return MSM_ERR_OUTOFMUS;
    }
    player = &mus.player[musNo];
    player->busyF = TRUE;
    if (player->status != 0) {
        player->fadeMaxTime = speed / 15;
        if (player->fadeMaxTime != 0) {
            player->fadeTime = player->fadeMaxTime;
            player->fadeVol = 127;
        } else {
            sndSeqStop(player->seqId);
            player->status = MSM_MUS_STOP;
        }
    }
    player->busyF = FALSE;
    return 0;
}

int msmMusPlay(int musId, MSM_MUSPARAM* musParam) {
    s32 var_r30;
    int var_r29;
    s32 temp_r3_3;
    MSM_GRP_HEAD* temp_r3_2;
    MSM_MUS* temp_r28;
    MUS_PLAYER* temp_r27;
    DVDFileInfo sp10;

    if (musId < 0 || musId >= mus.musMax) {
#ifdef TARGET_PC
        OSReport("[AUDIO] msmMusPlay id=%d failed=%d (valid range 0..%d)\n",
            musId, MSM_ERR_INVALIDID, mus.musMax - 1);
#endif
        return MSM_ERR_INVALIDID;
    }
    temp_r28 = &mus.musData[musId];
    if (temp_r28->sgid == 0xFFFF) {
#ifdef TARGET_PC
        OSReport("[AUDIO] msmMusPlay id=%d failed=%d (removed)\n",
            musId, MSM_ERR_REMOVEDID);
#endif
        return MSM_ERR_REMOVEDID;
    }
    if (msmSysCheckLoadGroupID(temp_r28->sgid) == 0) {
#ifdef TARGET_PC
        OSReport("[AUDIO] msmMusPlay id=%d failed=%d sgid=%u sid=%u (group not loaded)\n",
            musId, MSM_ERR_GRP_NOTLOADED, temp_r28->sgid, temp_r28->sid);
#endif
        return MSM_ERR_GRP_NOTLOADED;
    }
    var_r30 = (musParam != NULL) ? musParam->flag : 0;
    var_r29 = (var_r30 & MSM_MUSPARAM_CHAN) ? musParam->chan : 0;
    if (var_r29 < 0 || var_r29 >= mus.musChanMax) {
#ifdef TARGET_PC
        OSReport("[AUDIO] msmMusPlay id=%d failed=%d channel=%d max=%d\n",
            musId, MSM_ERR_OUTOFMUS, var_r29, mus.musChanMax);
#endif
        return MSM_ERR_OUTOFMUS;
    }
    temp_r27 = &mus.player[var_r29];
    /* Menu code can request the same background music more than once while
     * overlays hand control to each other. Reusing the active default play
     * avoids an audible stop/restart at the beginning of the same sequence. */
    if (temp_r27->status == MSM_MUS_PLAY && temp_r27->musId == musId && musParam == NULL) {
#ifdef TARGET_PC
        OSReport("[AUDIO] msmMusPlay id=%d channel=%d reused seq=%u\n",
            musId, var_r29, temp_r27->seqId);
#endif
        return var_r29;
    }
    if (temp_r27->status != 0) {
        sndSeqStop(temp_r27->seqId);
    }
    if (temp_r28->songGrp < 0) {
        if (temp_r27->musId != musId) {
            if (msmFioOpen(mus.msmEntryNum, &sp10) != 1) {
#ifdef TARGET_PC
                OSReport("[AUDIO] msmMusPlay id=%d failed=%d (song file open)\n",
                    musId, MSM_ERR_OPENFAIL);
#endif
                return MSM_ERR_OPENFAIL;
            }
            if (msmFioRead(&sp10, temp_r27->songBuf, temp_r28->songSize, temp_r28->songOfs + mus.dummyMusOfs) < 0) {
                msmFioClose(&sp10);
#ifdef TARGET_PC
                OSReport("[AUDIO] msmMusPlay id=%d failed=%d (song read)\n",
                    musId, MSM_ERR_READFAIL);
#endif
                return MSM_ERR_READFAIL;
            }
            msmFioClose(&sp10);
            temp_r27->arrfile = temp_r27->songBuf;
        }
    } else {
        temp_r3_2 = msmSysGetGroupDataPtr(temp_r28->songGrp);
        if (temp_r3_2 == NULL) {
#ifdef TARGET_PC
            OSReport("[AUDIO] msmMusPlay id=%d failed=%d songGrp=%d sgid=%u sid=%u\n",
                musId, MSM_ERR_MUSGRP_NOTLOADED, temp_r28->songGrp,
                temp_r28->sgid, temp_r28->sid);
#endif
            return MSM_ERR_MUSGRP_NOTLOADED;
        }
        temp_r27->arrfile = (u8 *)temp_r3_2 + temp_r3_2->sngOfs + temp_r28->songOfs;
    }
#ifdef BYTESWAPPING
    msmMusSwapSongData(temp_r27->arrfile, temp_r28->songSize);
#endif
    temp_r27->busyF = 1;
    temp_r27->vol = temp_r28->vol;
    temp_r27->pauseOffMaxTime = 0;
    temp_r27->pauseOnMaxTime = 0;
    temp_r27->fadeMaxTime = 0;
    temp_r27->pauseOnVol = 127;
    temp_r27->fadeVol = 127;
    temp_r27->playPara.flags = 4;
    temp_r27->playPara.volume.time = 0;
    temp_r27->baseVol = (var_r30 & MSM_MUSPARAM_VOL) ? musParam->vol : 127;
    temp_r27->playPara.volume.target = temp_r27->vol * temp_r27->fadeVol * temp_r27->baseVol / 16129;
    if (var_r30 & MSM_MUSPARAM_PAUSE) {
        temp_r27->playPara.flags |= 0x10;
    }
    if (var_r30 & MSM_MUSPARAM_SPEED) {
        temp_r27->playPara.flags |= 2;
        temp_r27->playPara.speed = musParam->speed * 256 / 100;
    }
    if (var_r30 & MSM_MUSPARAM_FADESPEED) {
        temp_r27->fadeMaxTime = -(musParam->fadeSpeed / 15);
        if (temp_r27->fadeMaxTime != 0) {
            temp_r27->fadeTime = 0;
            temp_r27->fadeVol = 0;
            temp_r27->playPara.volume.target = 0;
        }
    }
    temp_r3_3 = sndSeqPlay(temp_r28->sgid, temp_r28->sid, temp_r27->arrfile, &temp_r27->playPara);
    if (temp_r3_3 == SND_ID_ERROR) {
        temp_r27->busyF = 0;
#ifdef TARGET_PC
        OSReport("[AUDIO] msmMusPlay id=%d failed=%d sgid=%u sid=%u songGrp=%d (sndSeqPlay)\n",
            musId, MSM_ERR_PLAYFAIL, temp_r28->sgid, temp_r28->sid,
            temp_r28->songGrp);
#endif
        return MSM_ERR_PLAYFAIL;
    }
    temp_r27->seqId = temp_r3_3;
    temp_r27->musId = musId;
    temp_r27->status = (temp_r27->playPara.flags & 0x10) ? 3 : 2;
    temp_r27->busyF = 0;
#ifdef TARGET_PC
    OSReport("[AUDIO] msmMusPlay id=%d channel=%d seq=%u sgid=%u sid=%u songGrp=%d vol=%d\n",
        musId, var_r29, temp_r27->seqId, temp_r28->sgid, temp_r28->sid,
        temp_r28->songGrp, temp_r27->playPara.volume.target);
#endif
    return var_r29;
}

s32 msmMusInit(MSM_SYS* arg0, DVDFileInfo* arg1) {
    s32 temp_r4;
    s32 var_r8;

    mus.musMax = 0;
    mus.musChanMax = 0;
    mus.numPlay = 0;
    mus.baseGrpNumPlay = 0;
    if (arg0->info->musMax == 0) {
        return 0;
    }
    temp_r4 = arg0->header->musSize;
    if (temp_r4 == 0) {
        return 0;
    }
    if ((mus.musData = msmMemAlloc(temp_r4)) == NULL) {
        return MSM_ERR_OUTOFMEM;
    }
    if (msmFioRead(arg1, mus.musData, arg0->header->musSize, arg0->header->musOfs) < 0) {
        return MSM_ERR_READFAIL;
    }
#ifdef BYTESWAPPING
    msmMusSwapData(mus.musData, arg0->header->musSize / sizeof(*mus.musData));
#endif
    temp_r4 = arg0->info->dummyMusSize;
    if (temp_r4 != 0) {
        if ((mus.musBuf = msmMemAlloc(temp_r4 * arg0->info->musChanMax)) == NULL) {
            return MSM_ERR_OUTOFMEM;
        }
    } else {
        mus.musBuf = NULL;
    }
    mus.musMax = arg0->info->musMax;
    mus.musChanMax = arg0->info->musChanMax;
    mus.dummyMusOfs = arg0->header->dummyMusOfs;
    mus.msmEntryNum = arg0->msmEntryNum;
    for (var_r8 = 0; var_r8 < mus.musChanMax; var_r8++) {
        mus.player[var_r8].songBuf = (u8 *)mus.musBuf + arg0->info->dummyMusSize * var_r8;
        mus.player[var_r8].musId = -1;
        mus.player[var_r8].busyF = 0;
    }
    return 0;
}
