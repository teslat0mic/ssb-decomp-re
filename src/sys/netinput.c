#include <sys/netinput.h>

#include <sys/netpeer.h>
#include <sys/taskman.h>

typedef struct SYNetInputSlot
{
	SYNetInputSource source;
	SYNetInputFrame last_confirmed;
	SYNetInputFrame last_published;

} SYNetInputSlot;

SYNetInputSlot sSYNetInputSlots[MAXCONTROLLERS];
SYNetInputFrame sSYNetInputHistory[MAXCONTROLLERS][SYNETINPUT_HISTORY_LENGTH];
SYNetInputFrame sSYNetInputRemoteHistory[MAXCONTROLLERS][SYNETINPUT_HISTORY_LENGTH];
SYNetInputFrame sSYNetInputSavedHistory[MAXCONTROLLERS][SYNETINPUT_HISTORY_LENGTH];
#ifdef PORT
/* Local controller samples scheduled `input delay` ticks into the future, so this machine applies
 * them on the same tick the peer does; also what the peer is sent. */
SYNetInputFrame sSYNetInputLocalScheduled[MAXCONTROLLERS][SYNETINPUT_HISTORY_LENGTH];

/* Set when the last syNetInputFuncRead() declined to advance because the remote input for the
 * next tick had not arrived; the VS update reads it and skips the simulation for that VI. */
sb32 sSYNetInputIsStalled;

/* VIs spent waiting for remote input. The battle clock subtracts these so it advances once per
 * simulated tick rather than once per VI (see ifCommonTimerFuncRun). */
u32 sSYNetInputStalledVICount;
#endif
SYNetInputFrame sSYNetInputReplayFrames[MAXCONTROLLERS][SYNETINPUT_REPLAY_MAX_FRAMES];
SYNetInputReplayMetadata sSYNetInputReplayMetadata;
u32 sSYNetInputTick;
u32 sSYNetInputRecordedFrameCount;
sb32 sSYNetInputIsRecording;
sb32 sSYNetInputIsReplayMetadataValid;
/* Source-independent checksum over every frame published for a tick that
 * was actually advanced by syNetInputFuncRead(). Accumulated at the same
 * point sSYNetInputTick is incremented, so a tick re-published while the
 * P2P start barrier holds is counted once, like the recorder. When a limit
 * is set the value at exactly that many ticks is frozen for the caller.
 * sSYNetInputPublishedTickCount counts advances and today moves in lockstep
 * with sSYNetInputTick; anything that rewinds the tick (a future rollback
 * via syNetInputSetTick) must save and restore these four values with it. */
u32 sSYNetInputPublishedChecksum;
u32 sSYNetInputPublishedTickCount;
u32 sSYNetInputPublishedChecksumLimit;
u32 sSYNetInputPublishedChecksumAtLimit;

u32 syNetInputGetTick(void)
{
	return sSYNetInputTick;
}

void syNetInputSetTick(u32 tick)
{
	sSYNetInputTick = tick;
}

sb32 syNetInputCheckPlayer(s32 player)
{
	return ((player >= 0) && (player < MAXCONTROLLERS)) ? TRUE : FALSE;
}

void syNetInputClearFrame(SYNetInputFrame *frame)
{
	frame->tick = 0;
	frame->buttons = 0;
	frame->stick_x = 0;
	frame->stick_y = 0;
	frame->source = nSYNetInputSourceLocal;
	frame->is_predicted = FALSE;
	frame->is_valid = FALSE;
}

void syNetInputMakeFrame(SYNetInputFrame *frame, u32 tick, u16 buttons, s8 stick_x, s8 stick_y, SYNetInputSource source, sb32 is_predicted)
{
	frame->tick = tick;
	frame->buttons = buttons;
	frame->stick_x = stick_x;
	frame->stick_y = stick_y;
	frame->source = source;
	frame->is_predicted = is_predicted;
	frame->is_valid = TRUE;
}

void syNetInputStoreFrame(SYNetInputFrame history[][SYNETINPUT_HISTORY_LENGTH], s32 player, SYNetInputFrame *frame)
{
	history[player][frame->tick % SYNETINPUT_HISTORY_LENGTH] = *frame;
}

sb32 syNetInputGetStoredFrame(SYNetInputFrame history[][SYNETINPUT_HISTORY_LENGTH], s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	SYNetInputFrame *frame;

	if (syNetInputCheckPlayer(player) == FALSE)
	{
		return FALSE;
	}
	frame = &history[player][tick % SYNETINPUT_HISTORY_LENGTH];

	if ((frame->is_valid == FALSE) || (frame->tick != tick))
	{
		return FALSE;
	}
	if (out_frame != NULL)
	{
		*out_frame = *frame;
	}
	return TRUE;
}

void syNetInputReset(void)
{
	s32 player;
	s32 i;

	sSYNetInputTick = 0;
#ifdef PORT
	sSYNetInputIsStalled = FALSE;
	sSYNetInputStalledVICount = 0;
#endif
	sSYNetInputRecordedFrameCount = 0;
	sSYNetInputIsRecording = FALSE;
	sSYNetInputIsReplayMetadataValid = FALSE;
	sSYNetInputPublishedChecksum = 2166136261U;
	sSYNetInputPublishedTickCount = 0;
	sSYNetInputPublishedChecksumLimit = 0;
	sSYNetInputPublishedChecksumAtLimit = 0;

	sSYNetInputReplayMetadata.magic = SYNETINPUT_REPLAY_MAGIC;
	sSYNetInputReplayMetadata.version = SYNETINPUT_REPLAY_VERSION;
	sSYNetInputReplayMetadata.scene_kind = 0;
	sSYNetInputReplayMetadata.player_count = 0;
	sSYNetInputReplayMetadata.stage_kind = 0;
	sSYNetInputReplayMetadata.stocks = 0;
	sSYNetInputReplayMetadata.time_limit = 0;
	sSYNetInputReplayMetadata.item_switch = 0;
	sSYNetInputReplayMetadata.item_toggles = 0;
	sSYNetInputReplayMetadata.rng_seed = 0;
	sSYNetInputReplayMetadata.game_type = 0;
	sSYNetInputReplayMetadata.game_rules = 0;
	sSYNetInputReplayMetadata.is_team_battle = FALSE;
	sSYNetInputReplayMetadata.handicap = 0;
	sSYNetInputReplayMetadata.is_team_attack = FALSE;
	sSYNetInputReplayMetadata.is_stage_select = FALSE;
	sSYNetInputReplayMetadata.damage_ratio = 0;
	sSYNetInputReplayMetadata.item_appearance_rate = 0;
	sSYNetInputReplayMetadata.is_not_teamshadows = FALSE;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		sSYNetInputSlots[player].source = nSYNetInputSourceLocal;
		syNetInputClearFrame(&sSYNetInputSlots[player].last_confirmed);
		syNetInputClearFrame(&sSYNetInputSlots[player].last_published);
		sSYNetInputReplayMetadata.player_kinds[player] = 0;
		sSYNetInputReplayMetadata.fighter_kinds[player] = 0;
		sSYNetInputReplayMetadata.costumes[player] = 0;
		sSYNetInputReplayMetadata.teams[player] = 0;
		sSYNetInputReplayMetadata.handicaps[player] = 0;
		sSYNetInputReplayMetadata.levels[player] = 0;
		sSYNetInputReplayMetadata.shades[player] = 0;

		for (i = 0; i < SYNETINPUT_HISTORY_LENGTH; i++)
		{
			syNetInputClearFrame(&sSYNetInputHistory[player][i]);
			syNetInputClearFrame(&sSYNetInputRemoteHistory[player][i]);
#ifdef PORT
			syNetInputClearFrame(&sSYNetInputLocalScheduled[player][i]);
#endif
			syNetInputClearFrame(&sSYNetInputSavedHistory[player][i]);
		}
		for (i = 0; i < SYNETINPUT_REPLAY_MAX_FRAMES; i++)
		{
			syNetInputClearFrame(&sSYNetInputReplayFrames[player][i]);
		}
	}
}

void syNetInputStartVSSession(void)
{
	syNetInputReset();
}

void syNetInputSetSlotSource(s32 player, SYNetInputSource source)
{
	if (syNetInputCheckPlayer(player) != FALSE)
	{
		sSYNetInputSlots[player].source = source;
	}
}

SYNetInputSource syNetInputGetSlotSource(s32 player)
{
	if (syNetInputCheckPlayer(player) == FALSE)
	{
		return nSYNetInputSourceLocal;
	}
	return sSYNetInputSlots[player].source;
}

void syNetInputSetRemoteInput(s32 player, u32 tick, u16 buttons, s8 stick_x, s8 stick_y)
{
	SYNetInputFrame frame;

	if (syNetInputCheckPlayer(player) != FALSE)
	{
		syNetInputMakeFrame(&frame, tick, buttons, stick_x, stick_y, nSYNetInputSourceRemoteConfirmed, FALSE);
		syNetInputStoreFrame(sSYNetInputRemoteHistory, player, &frame);
	}
}

void syNetInputSetSavedInput(s32 player, u32 tick, u16 buttons, s8 stick_x, s8 stick_y)
{
	SYNetInputFrame frame;

	if (syNetInputCheckPlayer(player) != FALSE)
	{
		syNetInputMakeFrame(&frame, tick, buttons, stick_x, stick_y, nSYNetInputSourceSaved, FALSE);
		syNetInputStoreFrame(sSYNetInputSavedHistory, player, &frame);
	}
}

#ifdef PORT
#include <stdlib.h>

extern void port_log(const char *fmt, ...);

/* SSB64_NETPLAY_FAKE_INPUT=<seed>: deterministic stand-in for the local controller.
 *
 * Netplay predicts a missing remote input by repeating the last one, so a peer whose controller
 * never moves is predicted perfectly and an automated test reports a sync that real play does not
 * have. This generates a reproducible, input-rich stream from (seed, player, tick) so those tests
 * exercise the same paths a player does. Debug aid only: it is off unless the env var is set. */
u32 sSYNetInputFakeSeed;
sb32 sSYNetInputIsFakeInput;

void syNetInputInitFakeInputEnv(void)
{
	char *env = getenv("SSB64_NETPLAY_FAKE_INPUT");

	sSYNetInputIsFakeInput = FALSE;
	sSYNetInputFakeSeed = 0;

	if (env != NULL)
	{
		u32 seed = (u32)strtoul(env, NULL, 10);

		if (seed != 0)
		{
			sSYNetInputFakeSeed = seed;
			sSYNetInputIsFakeInput = TRUE;
			port_log("SSB64 NetInput: SSB64_NETPLAY_FAKE_INPUT seed=%u (local input is synthetic)\n", seed);
		}
	}
}

u32 syNetInputFakeHash(s32 player, u32 tick, u32 salt)
{
	u32 h = 2166136261U;

	h = (h ^ sSYNetInputFakeSeed) * 16777619U;
	h = (h ^ (u32)player) * 16777619U;
	h = (h ^ tick) * 16777619U;
	h = (h ^ salt) * 16777619U;
	h ^= h >> 15;

	return h;
}

void syNetInputMakeFakeFrame(s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	/* Hold each decision for a few ticks: single-frame noise would exercise the input plumbing
	 * but never produce a real action (a jump, a smash, a grab). */
	u32 slot = tick / 7;
	u32 h = syNetInputFakeHash(player, slot, 0);
	u16 buttons = 0;
	s8 stick_x = 0;
	s8 stick_y = 0;

	if ((h & 0x00000003U) == 0)
	{
		buttons |= CONT_A;
	}
	if ((h & 0x0000001CU) == 0)
	{
		buttons |= CONT_B;
	}
	if ((h & 0x000000E0U) == 0)
	{
		buttons |= CONT_G;   /* Z */
	}
	if ((h & 0x00000700U) == 0)
	{
		buttons |= CONT_R;
	}
	if ((h & 0x00003800U) == 0)
	{
		buttons |= CONT_UP;
	}
	stick_x = (s8)((s32)((h >> 16) & 0xFFU) - 128);
	stick_y = (s8)((s32)((h >> 24) & 0xFFU) - 128);

	syNetInputMakeFrame(out_frame, tick, buttons, stick_x, stick_y, nSYNetInputSourceLocal, FALSE);
}
#endif

void syNetInputMakeLocalFrame(s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	SYController *controller = &gSYControllerDevices[player];

#ifdef PORT
	{
		extern u32 syNetPeerGetActiveInputDelay(void);
		extern sb32 syNetPeerCheckInputSchedulingActive(void);

		if (syNetPeerCheckInputSchedulingActive() != FALSE)
		{
			u32 delay = syNetPeerGetActiveInputDelay();
			SYNetInputFrame sample;

			/* Schedule this VI's sample for tick + delay, the tick the peer is also told to apply
			 * it on, then apply whatever was scheduled for the current tick. The first `delay`
			 * ticks have nothing scheduled and run neutral, mirroring the remote side. */
			if (sSYNetInputIsFakeInput != FALSE)
			{
				syNetInputMakeFakeFrame(player, tick + delay, &sample);
			}
			else
			{
				syNetInputMakeFrame(&sample, tick + delay, controller->button_hold,
				                    controller->stick_range.x, controller->stick_range.y,
				                    nSYNetInputSourceLocal, FALSE);
			}
			syNetInputStoreFrame(sSYNetInputLocalScheduled, player, &sample);

			if (syNetInputGetStoredFrame(sSYNetInputLocalScheduled, player, tick, out_frame) == FALSE)
			{
				syNetInputMakeFrame(out_frame, tick, 0, 0, 0, nSYNetInputSourceLocal, FALSE);
			}
			return;
		}
	}
	if (sSYNetInputIsFakeInput != FALSE)
	{
		syNetInputMakeFakeFrame(player, tick, out_frame);
		return;
	}
#endif
	syNetInputMakeFrame
	(
		out_frame,
		tick,
		controller->button_hold,
		controller->stick_range.x,
		controller->stick_range.y,
		nSYNetInputSourceLocal,
		FALSE
	);
}

void syNetInputMakePredictedFrame(s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	SYNetInputFrame *last_confirmed = &sSYNetInputSlots[player].last_confirmed;

	if (last_confirmed->is_valid != FALSE)
	{
		syNetInputMakeFrame
		(
			out_frame,
			tick,
			last_confirmed->buttons,
			last_confirmed->stick_x,
			last_confirmed->stick_y,
			nSYNetInputSourceRemotePredicted,
			TRUE
		);
	}
	else syNetInputMakeFrame(out_frame, tick, 0, 0, 0, nSYNetInputSourceRemotePredicted, TRUE);
}

void syNetInputResolveFrame(s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	switch (sSYNetInputSlots[player].source)
	{
	case nSYNetInputSourceRemoteConfirmed:
	case nSYNetInputSourceRemotePredicted:
		if (syNetInputGetStoredFrame(sSYNetInputRemoteHistory, player, tick, out_frame) != FALSE)
		{
			sSYNetInputSlots[player].last_confirmed = *out_frame;
		}
		else syNetInputMakePredictedFrame(player, tick, out_frame);
		break;

	case nSYNetInputSourceSaved:
		if (syNetInputGetReplayFrame(player, tick, out_frame) != FALSE)
		{
			out_frame->source = nSYNetInputSourceSaved;
			out_frame->is_predicted = FALSE;
		}
		else if (syNetInputGetStoredFrame(sSYNetInputSavedHistory, player, tick, out_frame) == FALSE)
		{
			syNetInputMakeFrame(out_frame, tick, 0, 0, 0, nSYNetInputSourceSaved, FALSE);
		}
		break;

	case nSYNetInputSourceLocal:
	default:
		syNetInputMakeLocalFrame(player, tick, out_frame);
		break;
	}
}

void syNetInputPublishFrame(s32 player, SYNetInputFrame *frame)
{
	SYNetInputFrame *last_published = &sSYNetInputSlots[player].last_published;
	u16 prev_buttons = (last_published->is_valid != FALSE) ? last_published->buttons : 0;
	u16 pressed = (frame->buttons ^ prev_buttons) & frame->buttons;
	u16 released = (frame->buttons ^ prev_buttons) & prev_buttons;

	gSYControllerDevices[player].button_hold = frame->buttons;
	gSYControllerDevices[player].button_tap = pressed;
	gSYControllerDevices[player].button_release = released;
	gSYControllerDevices[player].button_update = pressed;
	gSYControllerDevices[player].stick_range.x = frame->stick_x;
	gSYControllerDevices[player].stick_range.y = frame->stick_y;

	sSYNetInputSlots[player].last_published = *frame;
	syNetInputStoreFrame(sSYNetInputHistory, player, frame);
}

void syNetInputPublishMainController(void)
{
	gSYControllerMain.button_hold = gSYControllerDevices[0].button_hold;
	gSYControllerMain.button_tap = gSYControllerDevices[0].button_tap;
	gSYControllerMain.button_update = gSYControllerDevices[0].button_update;
	gSYControllerMain.button_release = gSYControllerDevices[0].button_release;
	gSYControllerMain.stick_range.x = gSYControllerDevices[0].stick_range.x;
	gSYControllerMain.stick_range.y = gSYControllerDevices[0].stick_range.y;
}

#ifdef PORT
sb32 syNetInputCheckStalled(void)
{
	return sSYNetInputIsStalled;
}

u32 syNetInputGetStalledVICount(void)
{
	return sSYNetInputStalledVICount;
}

/* Called when the battle clock is (re)started, at which point the game also zeroes the VI counter
 * (sySchedulerSetTicCount(0)); the waited-VI count has to restart from the same origin. */
void syNetInputResetStalledVICount(void)
{
	sSYNetInputStalledVICount = 0;
}

/* TRUE when a confirmed remote frame for `tick` is already stored. */
sb32 syNetInputCheckRemoteFrameReady(s32 player, u32 tick)
{
	return syNetInputGetStoredFrame(sSYNetInputRemoteHistory, player, tick, NULL);
}

/* A local sample already scheduled for `tick` (netplay input delay). */
sb32 syNetInputGetScheduledFrame(s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	return syNetInputGetStoredFrame(sSYNetInputLocalScheduled, player, tick, out_frame);
}
#endif

sb32 syNetInputGetHistoryFrame(s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	return syNetInputGetStoredFrame(sSYNetInputHistory, player, tick, out_frame);
}

sb32 syNetInputGetPublishedFrame(s32 player, SYNetInputFrame *out_frame)
{
	if (syNetInputCheckPlayer(player) == FALSE)
	{
		return FALSE;
	}
	if (sSYNetInputSlots[player].last_published.is_valid == FALSE)
	{
		return FALSE;
	}
	if (out_frame != NULL)
	{
		*out_frame = sSYNetInputSlots[player].last_published;
	}
	return TRUE;
}

u32 syNetInputGetHistoryChecksum(s32 player, u32 tick_begin, u32 frame_count)
{
	SYNetInputFrame frame;
	u32 checksum = 2166136261U;
	u32 i;

	for (i = 0; i < frame_count; i++)
	{
		if (syNetInputGetHistoryFrame(player, tick_begin + i, &frame) != FALSE)
		{
			checksum ^= frame.tick;
			checksum *= 16777619U;
			checksum ^= frame.buttons;
			checksum *= 16777619U;
			checksum ^= (u8)frame.stick_x;
			checksum *= 16777619U;
			checksum ^= (u8)frame.stick_y;
			checksum *= 16777619U;
			checksum ^= frame.source;
			checksum *= 16777619U;
			checksum ^= frame.is_predicted;
			checksum *= 16777619U;
		}
	}
	return checksum;
}

u32 syNetInputAccumulateInputChecksum(u32 checksum, s32 player, SYNetInputFrame *frame)
{
	checksum ^= (u32)player;
	checksum *= 16777619U;
	checksum ^= frame->tick;
	checksum *= 16777619U;
	checksum ^= frame->buttons;
	checksum *= 16777619U;
	checksum ^= (u8)frame->stick_x;
	checksum *= 16777619U;
	checksum ^= (u8)frame->stick_y;
	checksum *= 16777619U;

	return checksum;
}

u32 syNetInputGetHistoryInputValueChecksumForPlayer(s32 player, u32 tick_begin, u32 frame_count)
{
	SYNetInputFrame frame;
	u32 checksum = 2166136261U;
	u32 i;

	for (i = 0; i < frame_count; i++)
	{
		if (syNetInputGetHistoryFrame(player, tick_begin + i, &frame) != FALSE)
		{
			checksum = syNetInputAccumulateInputChecksum(checksum, player, &frame);
		}
	}
	return checksum;
}

void syNetInputGetHistoryInputValueChecksumWindow(u32 tick_begin, u32 frame_count, u32 *out_checksums,
                                                  u32 *out_combined_checksum)
{
	SYNetInputFrame frame;
	u32 checksum = 2166136261U;
	u32 tick_limit;
	u32 tick;
	s32 player;

	tick_limit = tick_begin + frame_count;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		u32 player_checksum = 2166136261U;

		for (tick = tick_begin; tick < tick_limit; tick++)
		{
			if (syNetInputGetHistoryFrame(player, tick, &frame) != FALSE)
			{
				player_checksum = syNetInputAccumulateInputChecksum(player_checksum, player, &frame);
			}
		}
		checksum ^= player_checksum;
		checksum *= 16777619U;

		if (out_checksums != NULL)
		{
			out_checksums[player] = player_checksum;
		}
	}
	if (out_combined_checksum != NULL)
	{
		*out_combined_checksum = checksum;
	}
}

void syNetInputSetRecordingEnabled(sb32 is_enabled)
{
	sSYNetInputIsRecording = is_enabled;

	if (is_enabled != FALSE)
	{
		sSYNetInputRecordedFrameCount = 0;
	}
}

sb32 syNetInputGetRecordingEnabled(void)
{
	return sSYNetInputIsRecording;
}

u32 syNetInputGetRecordedFrameCount(void)
{
	return sSYNetInputRecordedFrameCount;
}

void syNetInputClearReplayFrames(void)
{
	s32 player;
	s32 i;

	sSYNetInputRecordedFrameCount = 0;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		for (i = 0; i < SYNETINPUT_REPLAY_MAX_FRAMES; i++)
		{
			syNetInputClearFrame(&sSYNetInputReplayFrames[player][i]);
		}
	}
}

sb32 syNetInputSetReplayFrame(s32 player, u32 tick, const SYNetInputFrame *frame)
{
	if ((syNetInputCheckPlayer(player) == FALSE) || (frame == NULL) || (tick >= SYNETINPUT_REPLAY_MAX_FRAMES))
	{
		return FALSE;
	}
	sSYNetInputReplayFrames[player][tick] = *frame;
	sSYNetInputReplayFrames[player][tick].tick = tick;
	sSYNetInputReplayFrames[player][tick].is_valid = TRUE;

	if (sSYNetInputRecordedFrameCount < (tick + 1))
	{
		sSYNetInputRecordedFrameCount = tick + 1;
	}
	return TRUE;
}

sb32 syNetInputGetReplayFrame(s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	SYNetInputFrame *frame;

	if ((syNetInputCheckPlayer(player) == FALSE) || (tick >= SYNETINPUT_REPLAY_MAX_FRAMES))
	{
		return FALSE;
	}
	frame = &sSYNetInputReplayFrames[player][tick];

	if ((frame->is_valid == FALSE) || (frame->tick != tick))
	{
		return FALSE;
	}
	if (out_frame != NULL)
	{
		*out_frame = *frame;
	}
	return TRUE;
}

u32 syNetInputGetReplayInputChecksum(void)
{
	SYNetInputFrame frame;
	u32 checksum = 2166136261U;
	u32 tick;
	s32 player;

	for (tick = 0; tick < sSYNetInputRecordedFrameCount; tick++)
	{
		for (player = 0; player < MAXCONTROLLERS; player++)
		{
			if (syNetInputGetReplayFrame(player, tick, &frame) != FALSE)
			{
				checksum = syNetInputAccumulateInputChecksum(checksum, player, &frame);
			}
		}
	}
	return checksum;
}

void syNetInputSetReplayMetadata(const SYNetInputReplayMetadata *metadata)
{
	if (metadata != NULL)
	{
		sSYNetInputReplayMetadata = *metadata;
		sSYNetInputReplayMetadata.magic = SYNETINPUT_REPLAY_MAGIC;
		sSYNetInputReplayMetadata.version = SYNETINPUT_REPLAY_VERSION;
		sSYNetInputIsReplayMetadataValid = TRUE;
	}
}

sb32 syNetInputGetReplayMetadata(SYNetInputReplayMetadata *out_metadata)
{
	if (sSYNetInputIsReplayMetadataValid == FALSE)
	{
		return FALSE;
	}
	if (out_metadata != NULL)
	{
		*out_metadata = sSYNetInputReplayMetadata;
	}
	return TRUE;
}

void syNetInputFuncRead(void)
{
	SYNetInputFrame frame;
	u32 tick;
	s32 player;

	syControllerFuncRead();
	tick = syNetInputGetTick();

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		syNetInputResolveFrame(player, tick, &frame);
		syNetInputPublishFrame(player, &frame);

		if (sSYNetInputIsRecording != FALSE)
		{
			syNetInputSetReplayFrame(player, tick, &frame);
		}
	}
	syNetInputPublishMainController();

	if (syNetPeerCheckStartBarrierReleased() == FALSE)
	{
		return;
	}
#ifdef PORT
	{
		extern sb32 syNetPeerCheckSimulationInputReady(u32 tick);

		/* Flow control: without the remote input for this tick, simulating would mean guessing,
		 * and nothing here can undo a wrong guess. Hold the tick instead - the peer that is
		 * behind keeps running and will supply it shortly. */
		if (syNetPeerCheckSimulationInputReady(tick) == FALSE)
		{
			sSYNetInputIsStalled = TRUE;
			sSYNetInputStalledVICount++;
			return;
		}
		sSYNetInputIsStalled = FALSE;
	}
#endif
	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		sSYNetInputPublishedChecksum = syNetInputAccumulateInputChecksum(
			sSYNetInputPublishedChecksum, player, &sSYNetInputSlots[player].last_published);
	}
	sSYNetInputPublishedTickCount++;

	if ((sSYNetInputPublishedChecksumLimit != 0) && (sSYNetInputPublishedTickCount == sSYNetInputPublishedChecksumLimit))
	{
		sSYNetInputPublishedChecksumAtLimit = sSYNetInputPublishedChecksum;
	}
	sSYNetInputTick++;
}

void syNetInputSetPublishedChecksumLimit(u32 tick_limit)
{
	sSYNetInputPublishedChecksumLimit = tick_limit;
	sSYNetInputPublishedChecksumAtLimit = sSYNetInputPublishedChecksum;
}

u32 syNetInputGetPublishedTickCount(void)
{
	return sSYNetInputPublishedTickCount;
}

u32 syNetInputGetPublishedInputChecksum(void)
{
	if ((sSYNetInputPublishedChecksumLimit != 0) && (sSYNetInputPublishedTickCount >= sSYNetInputPublishedChecksumLimit))
	{
		return sSYNetInputPublishedChecksumAtLimit;
	}
	return sSYNetInputPublishedChecksum;
}
