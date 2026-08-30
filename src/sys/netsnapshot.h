#ifndef _SYS_NETSNAPSHOT_H_
#define _SYS_NETSNAPSHOT_H_

#include <PR/ultratypes.h>
#include <ssb_types.h>

#define SYNETSNAPSHOT_BLOCKS_MAX 32

typedef struct SYNetSnapshotBlock
{
	void *addr;
	u32 size;
	const char *name;

} SYNetSnapshotBlock;

/* One captured simulation state. Buffers are host-heap allocations owned by the snapshot and are
 * reused between saves, so a rollback ring costs its allocations once. */
typedef struct SYNetSnapshot
{
	u8 *arena_copy;
	u32 arena_capacity;
	u32 arena_size;
	void *arena_ptr;    /* bump-allocator high-water mark to restore */
	u8 *block_copy;
	u32 block_capacity;
	u32 block_size;
	u32 tick;
	sb32 is_valid;

} SYNetSnapshot;

#ifdef PORT
extern void syNetSnapshotRegister(void *addr, u32 size, const char *name);
extern void syNetSnapshotRegisterAll(void);
extern u32 syNetSnapshotGetArenaSize(void);
extern sb32 syNetSnapshotSave(SYNetSnapshot *snap);
extern sb32 syNetSnapshotRestore(SYNetSnapshot *snap);
extern void syNetSnapshotFree(SYNetSnapshot *snap);

/* SSB64_SYNCTEST=1: after each simulated tick, restore the state captured before it, simulate the
 * same tick again and compare the state hashes. A mismatch means the snapshot is missing state.
 * Implemented in sc/sccommon/scvsbattle.c, where the tick function is called. */
extern sb32 syNetSnapshotCheckSyncTest(void);
extern void syNetSnapshotInitDebugEnv(void);
extern void syNetSnapshotReportSyncTest(u32 tick, sb32 matched, s32 column);
extern u32 syNetSnapshotGetSyncTestMismatches(void);
extern u32 syNetSnapshotGetSyncTestStartTick(void);
extern sb32 syNetSnapshotCheckSyncTestWindow(void);
extern void syNetSnapshotNoteSyncTestTick(void);
extern u32 syNetSnapshotGetSyncTestCheckedTicks(void);
#endif

#endif /* _SYS_NETSNAPSHOT_H_ */
