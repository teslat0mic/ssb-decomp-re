/**
 * netsnapshot.c — save and restore the simulation state of a VS battle (T3).
 *
 * Rollback netcode needs to rewind the simulation to an earlier tick and replay it with corrected
 * inputs. That requires capturing everything a tick can read or write, and nothing else.
 *
 * Where the state lives in this port:
 *
 *   1. The scene arena. `gSYTaskmanGeneralHeap` is a bump allocator over one 16 MiB block
 *      (`gPortSceneHeap`), zeroed per scene, holding every GObj, fighter, item, weapon and the
 *      battle state itself. Live bytes are exactly [start, ptr) — a bump allocator never frees, so
 *      there are no holes to chase and one memcpy captures all of it.
 *   2. A small number of globals outside the arena: list heads, manager counters, the RNG seed.
 *      Those are registered explicitly below.
 *
 * Anything NOT captured shows up as a SyncTest failure (SSB64_SYNCTEST=1): save, simulate a tick,
 * hash it, restore, simulate the same tick again, hash again. Two different hashes mean the second
 * run read something the snapshot did not restore. The registration table below was grown by
 * running exactly that test, not by reading the source and hoping.
 *
 * Deliberately NOT captured:
 *   - The graphics heap and display lists: rebuilt from scratch each frame and never read by the
 *     simulation (the draw pass is already known to be droppable — see docs/state_trace.md).
 *   - Audio: the simulation stores sound handles but never reads back mixer state.
 *   - Anything in the port layer (window, sockets, coroutine stacks): a rollback re-runs the tick
 *     function from the same call site on the same coroutine, so those never move.
 */

#include <sys/netsnapshot.h>

#include <ft/fighter.h>
#include <it/item.h>
#include <wp/weapon.h>
#include <gm/gmdef.h>
#include <gm/gmcamera.h>
#include <gr/ground.h>
#include <mp/map.h>
#include <sc/sctypes.h>
#include <sc/scmanager.h>
#include <sys/netsync.h>
#include <sys/objdef.h>
#include <sys/objman.h>
#include <sys/taskman.h>
#include <sys/utils.h>

#ifdef PORT
#include <stdlib.h>
#include <string.h>

extern void port_log(const char *fmt, ...);

/* The arena the scene allocates from (sys/taskman.c). */
extern SYMallocRegion gSYTaskmanGeneralHeap;

/* Simulation globals that live outside the arena. */
extern GObj *gGCCommonLinks[];
extern GObj *gGCCommonDLLinks[];
extern s32 *sSYUtilsRandomSeedPtr;

SYNetSnapshotBlock sSYNetSnapshotBlocks[SYNETSNAPSHOT_BLOCKS_MAX];
s32 sSYNetSnapshotBlockCount;
sb32 sSYNetSnapshotIsRegistered;
u32 sSYNetSnapshotBlockBytes;

void syNetSnapshotRegister(void *addr, u32 size, const char *name)
{
	if ((addr == NULL) || (size == 0))
	{
		return;
	}
	if (sSYNetSnapshotBlockCount >= SYNETSNAPSHOT_BLOCKS_MAX)
	{
		port_log("SSB64 Snapshot: block table full, '%s' not registered\n", name);
		return;
	}
	sSYNetSnapshotBlocks[sSYNetSnapshotBlockCount].addr = addr;
	sSYNetSnapshotBlocks[sSYNetSnapshotBlockCount].size = size;
	sSYNetSnapshotBlocks[sSYNetSnapshotBlockCount].name = name;
	sSYNetSnapshotBlockCount++;
	sSYNetSnapshotBlockBytes += size;
}

/* Registered once per session. Every entry here is state the simulation reads and writes that does
 * not live in the scene arena; see the file header for how the list is validated. */
void syNetSnapshotRegisterAll(void)
{
	if (sSYNetSnapshotIsRegistered != FALSE)
	{
		return;
	}
	sSYNetSnapshotBlockCount = 0;
	sSYNetSnapshotBlockBytes = 0;

	/* Object system: the per-kind linked-list heads every gcRunAll walk starts from. */
	syNetSnapshotRegister(gGCCommonLinks, sizeof(GObj *) * GC_COMMON_MAX_LINKS, "gGCCommonLinks");

	/* Random number generator. The seed itself is addressed through a pointer that the game can
	 * repoint (syUtilsSetRandomSeedPtr), so save what it points at, not the default global. */
	syNetSnapshotRegister(sSYUtilsRandomSeedPtr, sizeof(s32), "rng seed");

	/* Stage and collision state: the first SyncTest run named `stage` as the first column that
	 * differed after a rollback, which is exactly this set. The pointer-valued entries are the
	 * pointers themselves, not their targets - those live in the arena and travel with it. */
	syNetSnapshotRegister(&gGRCommonStruct, sizeof(gGRCommonStruct), "gGRCommonStruct");
	syNetSnapshotRegister(&gMPCollisionBounds, sizeof(gMPCollisionBounds), "gMPCollisionBounds");
	syNetSnapshotRegister(&gMPCollisionLinesNum, sizeof(gMPCollisionLinesNum), "gMPCollisionLinesNum");
	syNetSnapshotRegister(&gMPCollisionYakumonosNum, sizeof(gMPCollisionYakumonosNum), "gMPCollisionYakumonosNum");
	syNetSnapshotRegister(&gMPCollisionUpdateTic, sizeof(gMPCollisionUpdateTic), "gMPCollisionUpdateTic");
	syNetSnapshotRegister(&gMPCollisionSpeeds, sizeof(gMPCollisionSpeeds), "gMPCollisionSpeeds ptr");
	syNetSnapshotRegister(&gMPCollisionYakumonoDObjs, sizeof(gMPCollisionYakumonoDObjs), "gMPCollisionYakumonoDObjs ptr");
	syNetSnapshotRegister(&gMPCollisionBGMCurrent, sizeof(gMPCollisionBGMCurrent), "gMPCollisionBGMCurrent");
	syNetSnapshotRegister(&gMPCollisionBGMDefault, sizeof(gMPCollisionBGMDefault), "gMPCollisionBGMDefault");

	/* Camera: named by SyncTest after the stage set was fixed. */
	syNetSnapshotRegister(&gGMCameraStruct, sizeof(gGMCameraStruct), "gGMCameraStruct");
	syNetSnapshotRegister(&gGMCameraPauseCameraEyeX, sizeof(gGMCameraPauseCameraEyeX), "gGMCameraPauseEyeX");
	syNetSnapshotRegister(&gGMCameraPauseCameraEyeY, sizeof(gGMCameraPauseCameraEyeY), "gGMCameraPauseEyeY");

	/* Item manager: spawn scheduling and the Poke Ball monster table. */
	syNetSnapshotRegister(&gITManagerRandomWeights, sizeof(gITManagerRandomWeights), "gITManagerRandomWeights");
	syNetSnapshotRegister(&gITManagerMonsterData, sizeof(gITManagerMonsterData), "gITManagerMonsterData");
	syNetSnapshotRegister(&gITManagerAppearActor, sizeof(gITManagerAppearActor), "gITManagerAppearActor");

	/* Fighter manager counters that feed hit/stat bookkeeping. */
	syNetSnapshotRegister(&gFTManagerPlayersNum, sizeof(gFTManagerPlayersNum), "gFTManagerPlayersNum");
	syNetSnapshotRegister(&gFTManagerMotionCount, sizeof(gFTManagerMotionCount), "gFTManagerMotionCount");
	syNetSnapshotRegister(&gFTManagerStatUpdateCount, sizeof(gFTManagerStatUpdateCount), "gFTManagerStatUpdateCount");

	/* Display-list link heads: the object manager threads objects onto these as well. */
	syNetSnapshotRegister(gGCCommonDLLinks, sizeof(GObj *) * GC_COMMON_MAX_DLLINKS, "gGCCommonDLLinks");

	/* The battle-state pointer itself (its target lives in the arena). */
	syNetSnapshotRegister(&gSCManagerBattleState, sizeof(gSCManagerBattleState), "gSCManagerBattleState ptr");

	port_log("SSB64 Snapshot: registered %d global block(s), %u bytes\n",
	         sSYNetSnapshotBlockCount, sSYNetSnapshotBlockBytes);
	sSYNetSnapshotIsRegistered = TRUE;
}

u32 syNetSnapshotGetArenaSize(void)
{
	u8 *start = (u8 *)gSYTaskmanGeneralHeap.start;
	u8 *ptr = (u8 *)gSYTaskmanGeneralHeap.ptr;

	if ((start == NULL) || (ptr == NULL) || (ptr < start))
	{
		return 0;
	}
	return (u32)(ptr - start);
}

sb32 syNetSnapshotSave(SYNetSnapshot *snap)
{
	u32 arena_size;
	s32 i;
	u8 *cursor;

	if (snap == NULL)
	{
		return FALSE;
	}
	syNetSnapshotRegisterAll();
	arena_size = syNetSnapshotGetArenaSize();

	if (arena_size == 0)
	{
		return FALSE;
	}
	/* Grow the buffers on demand; the arena high-water mark only rises within a scene, so this
	 * settles after the first few ticks and never reallocates mid-match. */
	if (snap->arena_capacity < arena_size)
	{
		u8 *grown = (u8 *)realloc(snap->arena_copy, arena_size);

		if (grown == NULL)
		{
			port_log("SSB64 Snapshot: out of memory for %u arena bytes\n", arena_size);
			return FALSE;
		}
		snap->arena_copy = grown;
		snap->arena_capacity = arena_size;
	}
	if (snap->block_capacity < sSYNetSnapshotBlockBytes)
	{
		u8 *grown = (u8 *)realloc(snap->block_copy, sSYNetSnapshotBlockBytes);

		if (grown == NULL)
		{
			port_log("SSB64 Snapshot: out of memory for %u global bytes\n", sSYNetSnapshotBlockBytes);
			return FALSE;
		}
		snap->block_copy = grown;
		snap->block_capacity = sSYNetSnapshotBlockBytes;
	}
	memcpy(snap->arena_copy, gSYTaskmanGeneralHeap.start, arena_size);
	snap->arena_size = arena_size;
	snap->arena_ptr = gSYTaskmanGeneralHeap.ptr;

	cursor = snap->block_copy;

	for (i = 0; i < sSYNetSnapshotBlockCount; i++)
	{
		memcpy(cursor, sSYNetSnapshotBlocks[i].addr, sSYNetSnapshotBlocks[i].size);
		cursor += sSYNetSnapshotBlocks[i].size;
	}
	snap->block_size = sSYNetSnapshotBlockBytes;
	snap->is_valid = TRUE;

	return TRUE;
}

sb32 syNetSnapshotRestore(SYNetSnapshot *snap)
{
	s32 i;
	const u8 *cursor;

	if ((snap == NULL) || (snap->is_valid == FALSE))
	{
		return FALSE;
	}
	if (gSYTaskmanGeneralHeap.start == NULL)
	{
		return FALSE;
	}
	memcpy(gSYTaskmanGeneralHeap.start, snap->arena_copy, snap->arena_size);

	/* Rewind the bump pointer too: objects allocated during the rolled-back ticks must be
	 * forgotten, or the arena would grow without bound as ticks are replayed. */
	gSYTaskmanGeneralHeap.ptr = snap->arena_ptr;

	cursor = snap->block_copy;

	for (i = 0; i < sSYNetSnapshotBlockCount; i++)
	{
		memcpy(sSYNetSnapshotBlocks[i].addr, cursor, sSYNetSnapshotBlocks[i].size);
		cursor += sSYNetSnapshotBlocks[i].size;
	}
	return TRUE;
}

void syNetSnapshotFree(SYNetSnapshot *snap)
{
	if (snap == NULL)
	{
		return;
	}
	if (snap->arena_copy != NULL)
	{
		free(snap->arena_copy);
	}
	if (snap->block_copy != NULL)
	{
		free(snap->block_copy);
	}
	memset(snap, 0, sizeof(*snap));
}

/* ------------------------------------------------------------------------- */
/*  SyncTest: prove the snapshot captures everything a tick can touch          */
/* ------------------------------------------------------------------------- */

sb32 sSYNetSnapshotIsSyncTest;
u32 sSYNetSnapshotSyncTestStartTick;
u32 sSYNetSnapshotSyncTestCount;
u32 sSYNetSnapshotSyncTestChecked;
sb32 sSYNetSnapshotIsSyncTestChecked;
u32 sSYNetSnapshotSyncTestMismatches;

void syNetSnapshotInitDebugEnv(void)
{
	char *env = getenv("SSB64_SYNCTEST");

	char *start_env = getenv("SSB64_SYNCTEST_START");

	sSYNetSnapshotIsSyncTest = ((env != NULL) && (strtoul(env, NULL, 10) != 0)) ? TRUE : FALSE;
	{
		char *count_env = getenv("SSB64_SYNCTEST_COUNT");

		sSYNetSnapshotSyncTestStartTick = (start_env != NULL) ? (u32)strtoul(start_env, NULL, 10) : 0;
		sSYNetSnapshotSyncTestCount = (count_env != NULL) ? (u32)strtoul(count_env, NULL, 10) : 0;
		sSYNetSnapshotSyncTestChecked = 0;
	}
	sSYNetSnapshotIsSyncTestChecked = TRUE;
	sSYNetSnapshotSyncTestMismatches = 0;

	if (sSYNetSnapshotIsSyncTest != FALSE)
	{
		port_log("SSB64 Snapshot: SSB64_SYNCTEST=1 - every tick is simulated twice and compared\n");
	}
}

sb32 syNetSnapshotCheckSyncTest(void)
{
	if (sSYNetSnapshotIsSyncTestChecked == FALSE)
	{
		syNetSnapshotInitDebugEnv();
	}
	return sSYNetSnapshotIsSyncTest;
}

/* The check runs from this tick on; before it, interface coroutines (countdown, announcer) are
 * still being created and destroyed and their fiber handles cannot survive a rollback. */
u32 syNetSnapshotGetSyncTestStartTick(void)
{
	return sSYNetSnapshotSyncTestStartTick;
}

/* TRUE while the bounded check window is still open (an unset count means "no limit"). */
sb32 syNetSnapshotCheckSyncTestWindow(void)
{
	if (sSYNetSnapshotSyncTestCount == 0)
	{
		return TRUE;
	}
	return (sSYNetSnapshotSyncTestChecked < sSYNetSnapshotSyncTestCount) ? TRUE : FALSE;
}

void syNetSnapshotNoteSyncTestTick(void)
{
	sSYNetSnapshotSyncTestChecked++;
}

u32 syNetSnapshotGetSyncTestCheckedTicks(void)
{
	return sSYNetSnapshotSyncTestChecked;
}

u32 syNetSnapshotGetSyncTestMismatches(void)
{
	return sSYNetSnapshotSyncTestMismatches;
}

void syNetSnapshotReportSyncTest(u32 tick, sb32 matched, s32 column)
{
	if (matched != FALSE)
	{
		return;
	}
	sSYNetSnapshotSyncTestMismatches++;

	/* Only the first few are interesting: once one tick differs, every later tick starts from
	 * already-wrong state. */
	if (sSYNetSnapshotSyncTestMismatches <= 5)
	{
		port_log("SSB64 Snapshot: SYNCTEST MISMATCH tick=%u column=%s (state not captured by the snapshot)\n",
		         tick, syNetSyncGetColumnName(column));
	}
}

#endif /* PORT */
