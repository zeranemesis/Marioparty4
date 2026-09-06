#ifdef TARGET_PC
#include "port/rollback_scene.h"
bool PartyBoard_RollbackObjectRegions(PartyBoardRollbackRegionSink sink, void *context)
{
    if (!sink) return false;
#define OBJECT_REGION(value) if (!sink(context, &(value), sizeof(value))) return false;
    OBJECT_REGION(omDBGSysKeyObj)
    OBJECT_REGION(omwatchproc)
    OBJECT_REGION(omnextovl)
    OBJECT_REGION(omcurovl)
    OBJECT_REGION(omcurdll)
    OBJECT_REGION(omovlhisidx)
    OBJECT_REGION(omovlevtno)
    OBJECT_REGION(omnextovlevtno)
    OBJECT_REGION(omovlstat)
    OBJECT_REGION(omnextovlstat)
    OBJECT_REGION(omUPauseFlag)
    OBJECT_REGION(omSysExitReq)
    OBJECT_REGION(omdispinfo)
    OBJECT_REGION(pcOvlToKill)
    OBJECT_REGION(pcOvlKillArg)
    OBJECT_REGION(omovlhis)
    OBJECT_REGION(omSysPauseEnableFlag)
    OBJECT_REGION(omprevovl)
#undef OBJECT_REGION
    return true;
}
#endif
