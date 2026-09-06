#ifndef PARTYBOARD_ROLLBACK_SCENE_H
#define PARTYBOARD_ROLLBACK_SCENE_H
#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif
#ifdef __cplusplus
extern "C" {
#endif
#ifdef TARGET_PC
/* Same-process, same-build logical scene globals; game thread only.
 * Model/sprite tables are included, but their pointed-to resources, object
 * pools, presentation buffers, audio and process stacks are not
 * included. Their ownership must be restored by a complete scene coordinator.
 * Save/Load acquire the directory-I/O gate; do not call while holding it. */
size_t PartyBoard_RollbackSceneSize(void);
bool PartyBoard_RollbackSceneSave(void *destination, size_t capacity);
bool PartyBoard_RollbackSceneLoad(const void *source, size_t size);
bool PartyBoard_RollbackSceneSelfTest(void);
/* Adds the complete native SYSTEM and DATA heap arenas, including allocator
 * metadata. Still excludes other heaps, stacks, module data and external
 * device state. Requires initialized heaps; game thread at a safe boundary. */
size_t PartyBoard_RollbackResourcesSize(void);
bool PartyBoard_RollbackResourcesSave(void *destination, size_t capacity);
bool PartyBoard_RollbackResourcesLoad(const void *source, size_t size);
bool PartyBoard_RollbackResourcesSelfTest(void);
/* Coordinated same-process checkpoint: resources + PAD/game/sequence/clock,
 * suspended coroutine stacks and current Windows overlay .data. Requires a
 * loaded overlay and the scheduler's thread between HuPrcCall invocations.
 * This is an experimental building block, NOT complete gameplay coverage:
 * other heaps/managers, presentation and external effects remain excluded.
 * Every region and lifetime is validated before the first live write. */
size_t PartyBoard_RollbackCheckpointSize(void);
bool PartyBoard_RollbackCheckpointSave(void *destination, size_t capacity);
bool PartyBoard_RollbackCheckpointLoad(const void *source, size_t size);
bool PartyBoard_RollbackCheckpointSelfTest(void);
/* Internal enumeration for composing a capture under an existing I/O lease.
 * This only registers static regions and never copies or restores state. */
typedef bool (*PartyBoardRollbackRegionSink)(void *context, void *address, size_t size);
bool PartyBoard_RollbackObjectRegions(PartyBoardRollbackRegionSink sink, void *context);
bool PartyBoard_RollbackModelRegions(PartyBoardRollbackRegionSink sink, void *context);
bool PartyBoard_RollbackMotionRegions(PartyBoardRollbackRegionSink sink, void *context);
bool PartyBoard_RollbackSpriteRegions(PartyBoardRollbackRegionSink sink, void *context);
bool PartyBoard_RollbackTextureRegions(PartyBoardRollbackRegionSink sink, void *context);
bool PartyBoard_RollbackWipeRegions(PartyBoardRollbackRegionSink sink, void *context);
bool PartyBoard_RollbackWipeCanReplayWithoutDraw(void);
bool PartyBoard_RollbackWipeSafetySelfTest(void);
bool PartyBoard_RollbackTextRegions(PartyBoardRollbackRegionSink sink, void *context);
bool PartyBoard_RollbackPresentationRegions(PartyBoardRollbackRegionSink sink, void *context);
bool PartyBoard_RollbackSceneRegions(PartyBoardRollbackRegionSink sink, void *context);
/* Rendering callbacks and active wipes can change game state while issuing
 * GPU commands. Until each has a deterministic update path, fail closed. */
bool PartyBoard_RollbackRenderCanReplayWithoutDraw(void);
bool PartyBoard_RollbackRenderSafetySelfTest(void);
bool PartyBoard_RollbackPadRegions(PartyBoardRollbackRegionSink sink, void *context);
bool PartyBoard_RollbackGameRegions(PartyBoardRollbackRegionSink sink, void *context);
bool PartyBoard_RollbackFlagRegions(PartyBoardRollbackRegionSink sink, void *context);
bool PartyBoard_RollbackSequenceRegions(PartyBoardRollbackRegionSink sink, void *context);
bool PartyBoard_RollbackClockRegions(PartyBoardRollbackRegionSink sink, void *context);
bool PartyBoard_RollbackRandomRegions(PartyBoardRollbackRegionSink sink, void *context);
bool PartyBoard_RollbackProcessRegions(PartyBoardRollbackRegionSink sink, void *context);
bool PartyBoard_RollbackModuleRegions(PartyBoardRollbackRegionSink sink, void *context);
uint64_t PartyBoard_RollbackModuleGeneration(void);
#endif
#ifdef __cplusplus
}
#endif
#endif
