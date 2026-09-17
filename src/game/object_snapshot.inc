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

#include "port/netplay_state.h"

Process *PartyBoard_PrcListHead(void);

/* Overlay/module identity and history. Every value is a game-defined index or
 * flag; module base addresses and handles are deliberately never exported. */
void PartyBoard_NetplayOverlayState(PartyBoardNetplayStateSink sink, void *context)
{
    s32 i;
#define WORD(value) sink(context, #value, (uint32_t)(value))
    WORD(omcurovl); WORD(omnextovl); WORD(omprevovl); WORD(omcurdll);
    WORD(omovlhisidx); WORD(omovlevtno); WORD(omnextovlevtno);
    WORD(omovlstat); WORD(omnextovlstat);
    WORD(omUPauseFlag); WORD(omSysExitReq); WORD(omdispinfo);
    WORD(omSysPauseEnableFlag); WORD(pcOvlToKill); WORD(pcOvlKillArg);
    WORD(omDBGSysKeyObj != NULL);
    for (i = 0; i <= omovlhisidx && i < OM_OVL_HIS_MAX; i++) {
        WORD(i);
        WORD(omovlhis[i].overlay); WORD(omovlhis[i].event); WORD(omovlhis[i].stat);
    }
#undef WORD
}

/* Object-manager population. Slot indices and link indices are canonical; the
 * descriptor addresses, callback pointers and work words are not exported. */
void PartyBoard_NetplayObjectState(PartyBoardNetplayStateSink sink, void *context)
{
    Process *process;
    s32 manager = 0;
    s32 i;
#define WORD(value) sink(context, #value, (uint32_t)(value))
    for (process = PartyBoard_PrcListHead(); process != NULL; process = process->next) {
        omObjMan *objman;
        if (process->dtor != omDestroyObjMan || process->user_data == NULL) {
            continue;
        }
        objman = process->user_data;
        WORD(manager);
        WORD(objman->max_objs); WORD(objman->num_objs); WORD(objman->next_idx);
        WORD(objman->obj_first); WORD(objman->obj_last);
        if (objman->obj == NULL) {
            ++manager;
            continue;
        }
        for (i = 0; i < objman->max_objs; i++) {
            const omObjData *object = &objman->obj[i];
            WORD(object->stat);
            if (object->stat & OM_STAT_DELETED) {
                continue; /* Free slots keep stale presentation data. */
            }
            WORD(i);
            WORD(object->prio); WORD(object->prev); WORD(object->next);
            WORD(object->next_idx); WORD(object->next_idx_alloc);
            WORD(object->group); WORD(object->group_idx);
            WORD(object->mdlcnt); WORD(object->mtncnt);
            WORD(PartyBoard_NetplayFloatWord(object->trans.x));
            WORD(PartyBoard_NetplayFloatWord(object->trans.y));
            WORD(PartyBoard_NetplayFloatWord(object->trans.z));
            WORD(PartyBoard_NetplayFloatWord(object->rot.x));
            WORD(PartyBoard_NetplayFloatWord(object->rot.y));
            WORD(PartyBoard_NetplayFloatWord(object->rot.z));
            WORD(PartyBoard_NetplayFloatWord(object->scale.x));
            WORD(PartyBoard_NetplayFloatWord(object->scale.y));
            WORD(PartyBoard_NetplayFloatWord(object->scale.z));
            WORD(object->func != NULL); WORD(object->data != NULL);
        }
        if (objman->group != NULL) {
            for (i = 0; i < OM_MAX_GROUPS; i++) {
                WORD(i);
                WORD(objman->group[i].max_objs);
                WORD(objman->group[i].num_objs);
                WORD(objman->group[i].next_idx);
            }
        }
        ++manager;
    }
    WORD(manager);
#undef WORD
}
#endif
