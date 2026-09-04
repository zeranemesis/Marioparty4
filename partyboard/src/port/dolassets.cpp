#include <cstdint>
#include <cstring>
#include <port/port_version.h>

extern "C" {
#include "game/dvd.h"
}
#include "port/dolassets.h"

struct DOLIncludeEntry {
    uint32_t addresses[6];
    uint32_t size;
};

struct RELIncludeEntry {
    char path[32];
    uint32_t addresses[6];
    uint32_t size;
};

static constexpr uint32_t DOL_SHIFT = 0x80003000u;

// TODO JP exclusive variables
static constexpr DOLIncludeEntry s_dol_includes[DOL_INCLUDE_COUNT] = {
    /* INCLUDE_ANK8X8_4B     */ { { 0x8011FE00, 0x8011FE00, 0x8011FC20, 0x8011FC20, 0x8011FC20, 0x8011F9E0 }, 0x2000 },
    /* INCLUDE_ASCII8X8_1BPP */ { { 0x8012DCD7, 0x8012DCD7, 0x8012DAF7, 0x8012DAF7, 0x8012DAF7, 0x8012D8B7 }, 0x0800 },

    /* INCLUDE_HILITEDATA    */ { { 0x8012C360, 0x8012C360, 0x8012C180, 0x8012C180, 0x8012C180, 0x8012BF40 }, 0x0480 },
    /* INCLUDE_HILITEDATA2   */ { { 0x8012C7E0, 0x8012C7E0, 0x8012C600, 0x8012C600, 0x8012C600, 0x8012C3C0 }, 0x0480 },
    /* INCLUDE_HILITEDATA3   */ { { 0x8012CC60, 0x8012CC60, 0x8012CA80, 0x8012CA80, 0x8012CA80, 0x8012C840 }, 0x0480 },
    /* INCLUDE_HILITEDATA4   */ { { 0x8012D0E0, 0x8012D0E0, 0x8012CF00, 0x8012CF00, 0x8012CF00, 0x8012CCC0 }, 0x0480 },

    /* INCLUDE_REFMAPDATA0   */ { { 0x801225A0, 0x801225A0, 0x801223C0, 0x801223C0, 0x801223C0, 0x80122180 }, 0x1240 },
    /* INCLUDE_REFMAPDATA1   */ { { 0x801237E0, 0x801237E0, 0x80123600, 0x80123600, 0x80123600, 0x801233C0 }, 0x1100 },
    /* INCLUDE_REFMAPDATA2   */ { { 0x801248E0, 0x801248E0, 0x80124700, 0x80124700, 0x80124700, 0x801244C0 }, 0x2080 },
    /* INCLUDE_REFMAPDATA3   */ { { 0x80126960, 0x80126960, 0x80126780, 0x80126780, 0x80126780, 0x80126540 }, 0x2080 },
    /* INCLUDE_REFMAPDATA4   */ { { 0x801289E0, 0x801289E0, 0x80128800, 0x80128800, 0x80128800, 0x801285C0 }, 0x2080 },

    /* INCLUDE_TOONMAPDATA   */ { { 0x8012AA60, 0x8012AA60, 0x8012A880, 0x8012A880, 0x8012A880, 0x8012A640 }, 0x0880 },
    /* INCLUDE_TOONMAPDATA2  */ { { 0x8012B2E0, 0x8012B2E0, 0x8012B100, 0x8012B100, 0x8012B100, 0x8012AEC0 }, 0x1080 },

    /* INCLUDE_COVEROPEN_EN  */ { { 0x80132208, 0x80132208, 0x801320D0, 0x801320D0, 0x801320D0, 0x00000000 }, 0x1384 },
    /* INCLUDE_FATALERROR_EN */ { { 0x8013358C, 0x8013358C, 0x80133454, 0x80133454, 0x80133454, 0x00000000 }, 0x1384 },
    /* INCLUDE_LOADING_EN    */ { { 0x80134910, 0x80134910, 0x801347D8, 0x801347D8, 0x801347D8, 0x00000000 }, 0x1384 },
    /* INCLUDE_NODISC_EN     */ { { 0x80135C94, 0x80135C94, 0x80135B5C, 0x80135B5C, 0x80135B5C, 0x00000000 }, 0x1384 },
    /* INCLUDE_RETRYERROR_EN */ { { 0x80137018, 0x80137018, 0x80136EE0, 0x80136EE0, 0x80136EE0, 0x00000000 }, 0x1384 },
    /* INCLUDE_WRONGDISC_EN  */ { { 0x8013839C, 0x8013839C, 0x80138264, 0x80138264, 0x80138264, 0x00000000 }, 0x1384 },

    /* INCLUDE_COVEROPEN_FR  */ { { 0x00000000, 0x00000000, 0x801395E8, 0x801395E8, 0x801395E8, 0x00000000 }, 0x1384 },
    /* INCLUDE_FATALERROR_FR */ { { 0x00000000, 0x00000000, 0x8013A96C, 0x8013A96C, 0x8013A96C, 0x00000000 }, 0x1384 },
    /* INCLUDE_LOADING_FR    */ { { 0x00000000, 0x00000000, 0x8013BCF0, 0x8013BCF0, 0x8013BCF0, 0x00000000 }, 0x1384 },
    /* INCLUDE_NODISC_FR     */ { { 0x00000000, 0x00000000, 0x8013D074, 0x8013D074, 0x8013D074, 0x00000000 }, 0x1384 },
    /* INCLUDE_RETRYERROR_FR */ { { 0x00000000, 0x00000000, 0x8013E3F8, 0x8013E3F8, 0x8013E3F8, 0x00000000 }, 0x1384 },
    /* INCLUDE_WRONGDISC_FR  */ { { 0x00000000, 0x00000000, 0x8013F77C, 0x8013F77C, 0x8013F77C, 0x00000000 }, 0x1384 },

    /* INCLUDE_COVEROPEN_GE  */ { { 0x00000000, 0x00000000, 0x80140B00, 0x80140B00, 0x80140B00, 0x00000000 }, 0x1384 },
    /* INCLUDE_FATALERROR_GE */ { { 0x00000000, 0x00000000, 0x80141E84, 0x80141E84, 0x80141E84, 0x00000000 }, 0x1384 },
    /* INCLUDE_LOADING_GE    */ { { 0x00000000, 0x00000000, 0x80143208, 0x80143208, 0x80143208, 0x00000000 }, 0x1384 },
    /* INCLUDE_NODISC_GE     */ { { 0x00000000, 0x00000000, 0x8014458C, 0x8014458C, 0x8014458C, 0x00000000 }, 0x1384 },
    /* INCLUDE_RETRYERROR_GE */ { { 0x00000000, 0x00000000, 0x80145910, 0x80145910, 0x80145910, 0x00000000 }, 0x1384 },
    /* INCLUDE_WRONGDISC_GE  */ { { 0x00000000, 0x00000000, 0x80146C94, 0x80146C94, 0x80146C94, 0x00000000 }, 0x1384 },

    /* INCLUDE_COVEROPEN_IT  */ { { 0x00000000, 0x00000000, 0x80148018, 0x80148018, 0x80148018, 0x00000000 }, 0x1384 },
    /* INCLUDE_FATALERROR_IT */ { { 0x00000000, 0x00000000, 0x8014939C, 0x8014939C, 0x8014939C, 0x00000000 }, 0x1384 },
    /* INCLUDE_LOADING_IT    */ { { 0x00000000, 0x00000000, 0x8014A720, 0x8014A720, 0x8014A720, 0x00000000 }, 0x1384 },
    /* INCLUDE_NODISC_IT     */ { { 0x00000000, 0x00000000, 0x8014BAA4, 0x8014BAA4, 0x8014BAA4, 0x00000000 }, 0x1384 },
    /* INCLUDE_RETRYERROR_IT */ { { 0x00000000, 0x00000000, 0x8014CE28, 0x8014CE28, 0x8014CE28, 0x00000000 }, 0x1384 },
    /* INCLUDE_WRONGDISC_IT  */ { { 0x00000000, 0x00000000, 0x8014E1AC, 0x8014E1AC, 0x8014E1AC, 0x00000000 }, 0x1384 },

    /* INCLUDE_COVEROPEN_SP  */ { { 0x00000000, 0x00000000, 0x8014F530, 0x8014F530, 0x8014F530, 0x00000000 }, 0x1384 },
    /* INCLUDE_FATALERROR_SP */ { { 0x00000000, 0x00000000, 0x801508B4, 0x801508B4, 0x801508B4, 0x00000000 }, 0x1384 },
    /* INCLUDE_LOADING_SP    */ { { 0x00000000, 0x00000000, 0x80151C38, 0x80151C38, 0x80151C38, 0x00000000 }, 0x1384 },
    /* INCLUDE_NODISC_SP     */ { { 0x00000000, 0x00000000, 0x80152FBC, 0x80152FBC, 0x80152FBC, 0x00000000 }, 0x1384 },
    /* INCLUDE_RETRYERROR_SP */ { { 0x00000000, 0x00000000, 0x80154340, 0x80154340, 0x80154340, 0x00000000 }, 0x1384 },
    /* INCLUDE_WRONGDISC_SP  */ { { 0x00000000, 0x00000000, 0x801556C4, 0x801556C4, 0x801556C4, 0x00000000 }, 0x1384 },
};

// TODO JP
static constexpr RELIncludeEntry s_rel_includes[REL_INCLUDE_COUNT] = {
    /* INCLUDE_NINTENDODATA  */ { "dll/bootDll.rel", {0x2B80 + 0xA0, 0x2B80 + 0xA0, 0x4000 + 0xA0, 0x4000 + 0xA0, 0x4000 + 0xA0, 0x2B80 + 0xA0}, 0x307D },
};

static u8 *dolPtr;

extern "C" {

void InitializeDol()
{
    s32 dolSize;
    const u8 *dol = DVDGetDOLLocation(&dolSize);
    if (!dol)
        return;
    dolPtr = new u8[dolSize];
    memcpy(dolPtr, dol, dolSize);
}

void *GetDolIncludeData(int index)
{
    if (index < 0 || index >= DOL_INCLUDE_COUNT)
        return nullptr;

    const auto &entry = s_dol_includes[index];
    uint32_t address = entry.addresses[static_cast<int>(partyboard::version::getGameVersion())];
    if (address == 0) {
        OSReport("Requested non-existing data\n");
        return nullptr;
    }
    void *ret = &dolPtr[address - DOL_SHIFT];
    return ret;
}

void *GetRelIncludeData(int index)
{
    if (index < 0 || index >= REL_INCLUDE_COUNT)
        return nullptr;

    const auto &entry = s_rel_includes[index];
    uint32_t address = entry.addresses[static_cast<int>(partyboard::version::getGameVersion())];
    if (address == 0) {
        OSReport("Requested non-existing data\n");
        return nullptr;
    }
    DVDFileInfo fileInfo;
    if (!DVDOpen(entry.path, &fileInfo)) {
        OSReport("Failed to open REL\n");
        return nullptr;
    }
    void *buffer = HuMemDirectMalloc(HEAP_DVD, entry.size);
    s32 result = DVDReadPrio(&fileInfo, buffer, static_cast<s32>(entry.size), static_cast<s32>(address), 2);
    if (result < 0) {
        OSReport("Reading REL failed\n");
    }

    DVDClose(&fileInfo);

    return buffer;
}
};