#include "game/board/space.h"
#include "game/board/model.h"
#include "game/board/main.h"
#include "game/board/star.h"
#include "game/board/shop.h"

#include "game/data.h"

static s16 bgMdl = -1;
static s16 fgMdl = -1;
static s16 hostMdl = -1;

static s32 hostMotTbl[] = {
	DATA_MAKE_NUM(DATADIR_BGUEST, 42),
	-1
};

static void LightSetHook(void);
static void LightResetHook(void);
static s32 WalkEvent(void);
static s32 WalkMiniEvent(void);
static s32 LandEvent(void);

void BoardCreate(void)
{
	BoardSpaceInit(DATA_MAKE_NUM(DATADIR_W21, 0));
	bgMdl = BoardModelCreate(DATA_MAKE_NUM(DATADIR_W21, 1), NULL, 0);
	fn_8006DDE8(bgMdl, -1.0f);
	BoardModelMotionStart(bgMdl, 0, 0x40000001);
	fgMdl = BoardModelCreate(DATA_MAKE_NUM(DATADIR_W21, 2), NULL, 0);
	fn_8006DDE8(fgMdl, -1.0f);
	BoardModelPosSet(fgMdl, 0.0f, 0.0f, 0.0f);
	BoardModelMotionStart(fgMdl, 0, 0x40000001);
	hostMdl = BoardModelCreate(DATA_MAKE_NUM(DATADIR_BGUEST, 41), hostMotTbl, 0);
	BoardModelPosSet(hostMdl, 0.0f, 0.0f, 0.0f);
	BoardModelMotionStart(hostMdl, 1, 0x40000001);
	BoardLightHookSet(LightSetHook, LightResetHook);
	BoardSpaceWalkEventFuncSet(WalkEvent);
	BoardSpaceWalkMiniEventFuncSet(WalkMiniEvent);
	BoardSpaceLandEventFuncSet(LandEvent);
	BoardStarHostSet(hostMdl);
	BoardShopHostSet(hostMdl);
}

void BoardDestroy(void)
{
	if(hostMdl != -1) {
		BoardModelKill(hostMdl);
		hostMdl = -1;
	}
	if(bgMdl != -1) {
		BoardModelKill(bgMdl);
		bgMdl = -1;
	}
	if(fgMdl != -1) {
		BoardModelKill(fgMdl);
		fgMdl = -1;
	}
}

static void LightSetHook(void)
{
	s16 id = BoardModelIDGet(bgMdl);
	Hu3DModelLightInfoSet(id, 1);
}

static void LightResetHook(void)
{
	Hu3DFogClear();
}

static s32 WalkEvent(void)
{
	return 0;
}

static s32 WalkMiniEvent(void)
{
	
}

static s32 LandEvent(void)
{
	
}