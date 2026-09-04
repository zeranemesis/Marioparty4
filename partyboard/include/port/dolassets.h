#ifndef PORT_DOLASSETS_H_
#define PORT_DOLASSETS_H_

#ifdef __cplusplus
extern "C" {
#endif

enum DolIncludes {
    DOL_INCLUDE_ANK8X8_4B,
    DOL_INCLUDE_ASCII8X8_1BPP,

    DOL_INCLUDE_HILITEDATA,
    DOL_INCLUDE_HILITEDATA2,
    DOL_INCLUDE_HILITEDATA3,
    DOL_INCLUDE_HILITEDATA4,

    DOL_INCLUDE_REFMAPDATA0,
    DOL_INCLUDE_REFMAPDATA1,
    DOL_INCLUDE_REFMAPDATA2,
    DOL_INCLUDE_REFMAPDATA3,
    DOL_INCLUDE_REFMAPDATA4,

    DOL_INCLUDE_TOONMAPDATA,
    DOL_INCLUDE_TOONMAPDATA2,

    DOL_INCLUDE_COVEROPEN_EN,
    DOL_INCLUDE_FATALERROR_EN,
    DOL_INCLUDE_LOADING_EN,
    DOL_INCLUDE_NODISC_EN,
    DOL_INCLUDE_RETRYERROR_EN,
    DOL_INCLUDE_WRONGDISC_EN,

    DOL_INCLUDE_COVEROPEN_FR,
    DOL_INCLUDE_FATALERROR_FR,
    DOL_INCLUDE_LOADING_FR,
    DOL_INCLUDE_NODISC_FR,
    DOL_INCLUDE_RETRYERROR_FR,
    DOL_INCLUDE_WRONGDISC_FR,

    DOL_INCLUDE_COVEROPEN_gr,
    DOL_INCLUDE_FATALERROR_gr,
    DOL_INCLUDE_LOADING_gr,
    DOL_INCLUDE_NODISC_gr,
    DOL_INCLUDE_RETRYERROR_gr,
    DOL_INCLUDE_WRONGDISC_gr,

    DOL_INCLUDE_COVEROPEN_IT,
    DOL_INCLUDE_FATALERROR_IT,
    DOL_INCLUDE_LOADING_IT,
    DOL_INCLUDE_NODISC_IT,
    DOL_INCLUDE_RETRYERROR_IT,
    DOL_INCLUDE_WRONGDISC_IT,

    DOL_INCLUDE_COVEROPEN_SP,
    DOL_INCLUDE_FATALERROR_SP,
    DOL_INCLUDE_LOADING_SP,
    DOL_INCLUDE_NODISC_SP,
    DOL_INCLUDE_RETRYERROR_SP,
    DOL_INCLUDE_WRONGDISC_SP,

    DOL_INCLUDE_COUNT,
};

enum RelIncludes {
    REL_INCLUDE_NINTENDODATA,
    REL_INCLUDE_COUNT,
};

#define ank8x8_4b GetDolIncludeData(DOL_INCLUDE_ANK8X8_4B)
#define Ascii8x8_1bpp GetDolIncludeData(DOL_INCLUDE_ASCII8X8_1BPP)

#define hiliteData GetDolIncludeData(DOL_INCLUDE_HILITEDATA)
#define hiliteData2 GetDolIncludeData(DOL_INCLUDE_HILITEDATA2)
#define hiliteData3 GetDolIncludeData(DOL_INCLUDE_HILITEDATA3)
#define hiliteData4 GetDolIncludeData(DOL_INCLUDE_HILITEDATA4)

#define refMapData0 GetDolIncludeData(DOL_INCLUDE_REFMAPDATA0)
#define refMapData1 GetDolIncludeData(DOL_INCLUDE_REFMAPDATA1)
#define refMapData2 GetDolIncludeData(DOL_INCLUDE_REFMAPDATA2)
#define refMapData3 GetDolIncludeData(DOL_INCLUDE_REFMAPDATA3)
#define refMapData4 GetDolIncludeData(DOL_INCLUDE_REFMAPDATA4)

#define toonMapData GetDolIncludeData(DOL_INCLUDE_TOONMAPDATA)
#define toonMapData2 GetDolIncludeData(DOL_INCLUDE_TOONMAPDATA2)

#define coveropen_en GetDolIncludeData(DOL_INCLUDE_COVEROPEN_EN)
#define fatalerror_en GetDolIncludeData(DOL_INCLUDE_FATALERROR_EN)
#define loading_en GetDolIncludeData(DOL_INCLUDE_LOADING_EN)
#define nodisc_en GetDolIncludeData(DOL_INCLUDE_NODISC_EN)
#define retryerror_en GetDolIncludeData(DOL_INCLUDE_RETRYERROR_FR)

#define wrongdisc_fr GetDolIncludeData(DOL_INCLUDE_WRONGDISC_FR)
#define coveropen_fr GetDolIncludeData(DOL_INCLUDE_COVEROPFR_FR)
#define fatalerror_fr GetDolIncludeData(DOL_INCLUDE_FATALERROR_FR)
#define loading_fr GetDolIncludeData(DOL_INCLUDE_LOADING_FR)
#define nodisc_fr GetDolIncludeData(DOL_INCLUDE_NODISC_FR)
#define retryerror_fr GetDolIncludeData(DOL_INCLUDE_RETRYERROR_FR)
#define wrongdisc_fr GetDolIncludeData(DOL_INCLUDE_WRONGDISC_FR)

#define coveropen_gr GetDolIncludeData(DOL_INCLUDE_COVEROPEN_GR)
#define fatalerror_gr GetDolIncludeData(DOL_INCLUDE_FATALERROR_GR)
#define loading_gr GetDolIncludeData(DOL_INCLUDE_LOADING_GR)
#define nodisc_gr GetDolIncludeData(DOL_INCLUDE_NODISC_GR)
#define retryerror_gr GetDolIncludeData(DOL_INCLUDE_RETRYERROR_GR)
#define wrongdisc_gr GetDolIncludeData(DOL_INCLUDE_WRONGDISC_GR)

#define coveropen_it GetDolIncludeData(DOL_INCLUDE_COVEROPEN_IT)
#define fatalerror_it GetDolIncludeData(DOL_INCLUDE_FATALERROR_IT)
#define loading_it GetDolIncludeData(DOL_INCLUDE_LOADING_IT)
#define nodisc_it GetDolIncludeData(DOL_INCLUDE_NODISC_IT)
#define retryerror_it GetDolIncludeData(DOL_INCLUDE_RETRYERROR_IT)
#define wrongdisc_it GetDolIncludeData(DOL_INCLUDE_WRONGDISC_IT)

#define coveropen_sp GetDolIncludeData(DOL_INCLUDE_COVEROPEN_SP)
#define fatalerror_sp GetDolIncludeData(DOL_INCLUDE_FATALERROR_SP)
#define loading_sp GetDolIncludeData(DOL_INCLUDE_LOADING_SP)
#define nodisc_sp GetDolIncludeData(DOL_INCLUDE_NODISC_SP)
#define retryerror_sp GetDolIncludeData(DOL_INCLUDE_RETRYERROR_SP)
#define wrongdisc_sp GetDolIncludeData(DOL_INCLUDE_WRONGDISC_SP)

#define nintendoData GetRelIncludeData(REL_INCLUDE_NINTENDODATA)

void InitializeDol();
void *GetDolIncludeData(int index);
void *GetRelIncludeData(int index);

#ifdef __cplusplus
}
#endif

#endif
