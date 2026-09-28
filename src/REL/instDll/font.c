#include <version.h>
#include "game/sprite.h"

#include "REL/instDll.h"
#include "port/version_runtime.h"

#define FONT_CHAR_MAX 100

#ifdef TARGET_PC
// The PC port keeps both tables: the inst archive of a PAL disc has extra letters,
// so its file numbers differ. FontCreate picks the one matching the loaded disc.
#define FONT_CHAR_FILE_PAL 0
static s32 FontCharFile[0xC6] = {
#include "REL/instDll/font_tbl.inc"
};
#undef FONT_CHAR_FILE_PAL
#define FONT_CHAR_FILE_PAL 1
static s32 FontCharFilePal[0x130] = {
#include "REL/instDll/font_tbl.inc"
};
#undef FONT_CHAR_FILE_PAL
#else
#define FONT_CHAR_FILE_PAL VERSION_PAL
#if VERSION_PAL
static s32 FontCharFile[0x130] = {
#else
static s32 FontCharFile[0xC6] = {
#endif
#include "REL/instDll/font_tbl.inc"
};
#endif

s32 FontCreate(SeqWork *work, char *mess, s16 jp_flag)
{
#ifdef TARGET_PC
    u8 *str; // unsigned as on the GameCube: letters above 0x7F (PAL accents) index the table
#else
    char *str;
#endif
    s16 len;
    s16 *posY;
    s16 charNum;
    s32 *fileTbl;
    s16 i;
    s16 grpNo;
    s16 *posX;
    ANIMDATA **animP;
    s16 gid;
    s16 sprid;
    s32 file;

    fileTbl = FontCharFile;
#ifdef TARGET_PC
    if (VERSION_RT_PAL) {
        fileTbl = FontCharFilePal;
    }
#endif
    for (grpNo = 0; grpNo < 0x10; grpNo++) {
        if (work->spr_grp[grpNo] == -1) {
            break;
        }
    }
    if (grpNo == 0x10) {
        return -1;
    }
    animP = HuMemDirectMalloc(HEAP_SYSTEM, (FONT_CHAR_MAX * sizeof(ANIMDATA*)));
    posX = HuMemDirectMalloc(HEAP_SYSTEM, FONT_CHAR_MAX * sizeof(*posX));
    posY = HuMemDirectMalloc(HEAP_SYSTEM, FONT_CHAR_MAX * sizeof(*posY));

#ifdef TARGET_PC
    for (str = (u8 *)mess, len = 0, charNum = 0; str[0] != 0; str++) {
#else
    for (str = mess, len = 0, charNum = 0; str[0] != 0; str++) {
#endif
        if (str[0] == 0x20 || str[0] == 0x10) {
            len += 0xE;
        }
        else if (str[0] < 0x30) {
        }
#ifdef TARGET_PC
        // NTSC messages mark voiced kana with a 0x80/0x81 suffix, PAL messages have no suffix
        else if (VERSION_RT_NTSC && str[0] == 0x80) {
        }
        else if (VERSION_RT_NTSC && str[0] == 0x81) {
        }
        else {
            if (VERSION_RT_NTSC && str[1] == 0x80) {
                if ((str[0] >= 0x96) && (str[0] <= 0xA4)) {
                    file = fileTbl[str[0] + 0x6A];
                }
                else if ((str[0] >= 0xAA) && (str[0] <= 0xAE)) {
                    file = fileTbl[str[0] + 0x65];
                }
                else if ((str[0] >= 0xD6) && (str[0] <= 0xE4)) {
                    file = fileTbl[str[0] + 0x43];
                }
                else if ((str[0] >= 0xEA) && (str[0] <= 0xEE)) {
                    file = fileTbl[str[0] + 0x3E];
                }
            }
            else if (VERSION_RT_NTSC && str[1] == 0x81) {
                if ((str[0] >= 0xAA) && (str[0] <= 0xAE)) {
                    file = fileTbl[str[0] + 0x6A];
                }
                else if ((str[0] >= 0xEA) && (str[0] <= 0xEE)) {
                    file = fileTbl[str[0] + 0x43];
                }
            }
            else {
                file = fileTbl[str[0]];
            }
#elif VERSION_NTSC
        else if (str[0] == 0x80) {
        }
        else if (str[0] == 0x81) {
        }
        else {
            if (str[1] == 0x80) {
                if ((str[0] >= 0x96) && (str[0] <= 0xA4)) {
                    file = fileTbl[str[0] + 0x6A];
                }
                else if ((str[0] >= 0xAA) && (str[0] <= 0xAE)) {
                    file = fileTbl[str[0] + 0x65];
                }
                else if ((str[0] >= 0xD6) && (str[0] <= 0xE4)) {
                    file = fileTbl[str[0] + 0x43];
                }
                else if ((str[0] >= 0xEA) && (str[0] <= 0xEE)) {
                    file = fileTbl[str[0] + 0x3E];
                }
            }
            else if (str[1] == 0x81) {
                if ((str[0] >= 0xAA) && (str[0] <= 0xAE)) {
                    file = fileTbl[str[0] + 0x6A];
                }
                else if ((str[0] >= 0xEA) && (str[0] <= 0xEE)) {
                    file = fileTbl[str[0] + 0x43];
                }
            }
            else {
                file = fileTbl[str[0]];
            }
#else
        else {
            file = fileTbl[str[0]];
#endif
            animP[charNum] = HuSprAnimReadFile(file);
            posX[charNum] = len;
            if ((str[0] >= 0x61) && (str[0] <= 0x7A)) {
                posY[charNum] = 2;
                len += 0x12;
            }
#ifdef TARGET_PC
            else if ((str[0] == 0xC2) || (str[0] == 0xC3) || (VERSION_RT_PAL && str[0] == 0xC7)) {
#elif VERSION_PAL
            else if ((str[0] == 0xC2) || (str[0] == 0xC3) || (str[0] == 0xC7)) {
#else
            else if ((str[0] == 0xC2) || (str[0] == 0xC3)) {
#endif
                posY[charNum] = 0;
                len += 0x12;
            }
#ifdef TARGET_PC
            // Letter widths of the loaded disc's font
            else if (VERSION_RT_PAL && str[0] == 0xC6) {
                posY[charNum] = 0;
                len += 0x18;
            }
            else if (VERSION_RT_PAL && str[0] == 0x5C) {
                posY[charNum] = 0;
                len += 8;
            }
            else if (VERSION_RT_PAL && ((str[0] == 0x85) || (str[0] == 0x82))) {
                posY[charNum] = 0;
                len += 0xC;
            }
            else if (VERSION_RT_PAL && str[0] == 0x94) {
                posY[charNum] = -2;
                len += 0x18;
            }
            else if (VERSION_RT_PAL && ((str[0] >= 0x90) && (str[0] <= 0x9F))) {
                posY[charNum] = -2;
                len += 0x18;
            }
            else if (VERSION_RT_PAL && ((str[0] >= 0xD0) && (str[0] <= 0xEF))) {
                posY[charNum] = -2;
                len += 0x12;
            }
            else if (VERSION_RT_NTSC && (str[0] == 0x5C || str[0] == 0x85)) {
                posY[charNum] = 0;
                len += 8;
            }
            else if (VERSION_RT_NTSC && ((str[0] >= 0x87) && (str[0] <= 0x8F))) {
                posY[charNum] = 4;
                len += 0x18;
            }
            else if (VERSION_RT_NTSC && ((str[0] >= 0xC7) && (str[0] <= 0xCF))) {
                posY[charNum] = 4;
                len += 0x18;
            }
#elif VERSION_PAL
            else if (str[0] == 0xC6) {
                posY[charNum] = 0;
                len += 0x18;
            }
            else if (str[0] == 0x5C) {
                posY[charNum] = 0;
                len += 8;
            }
            else if ((str[0] == 0x85) || (str[0] == 0x82)) {
                posY[charNum] = 0;
                len += 0xC;
            }
            else if (str[0] == 0x94) {
                posY[charNum] = -2;
                len += 0x18;
            }
            else if ((str[0] >= 0x90) && (str[0] <= 0x9F)) {
                posY[charNum] = -2;
                len += 0x18;
            }
            else if ((str[0] >= 0xD0) && (str[0] <= 0xEF)) {
                posY[charNum] = -2;
                len += 0x12;
            }
#else
            else if (str[0] == 0x5C || str[0] == 0x85) {
                posY[charNum] = 0;
                len += 8;
            }
            else if ((str[0] >= 0x87) && (str[0] <= 0x8F)) {
                posY[charNum] = 4;
                len += 0x18;
            }
            else if ((str[0] >= 0xC7) && (str[0] <= 0xCF)) {
                posY[charNum] = 4;
                len += 0x18;
            }
#endif
            else if ((str[0] == 0x3D) || (str[0] == 0x84)) {
                posY[charNum] = 0;
                len += 0x14;
            }
            else {
                posY[charNum] = 0;
                len += 0x1C;
            }
            charNum++;
        }
    }
    gid = HuSprGrpCreate(charNum);
    work->spr_grp[grpNo] = gid;
    work->alt_word_len = len;
    len = (len / 2) - 0xE;
    for (i = 0; i < charNum; i++) {
        sprid = HuSprCreate(animP[i], 0, 0);
        HuSprGrpMemberSet(gid, i, sprid);
        HuSprPosSet(gid, i, posX[i] - len, posY[i]);
    }
    work->word_len = charNum;
    HuMemDirectFree(animP);
    HuMemDirectFree(posX);
    HuMemDirectFree(posY);
    return grpNo;
}
