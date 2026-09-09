# Netplay phase A — audit and implementation (2026-09-08/09)

Base: audio-local, 3ec61a3c (0.15.6). Source checkout: existing partyboard-audio-local.
Existing edits: extern/aurora and extern/musyx dirty; preserved. Videos/bon plan is the installed binary directory.

## Confirmed causes
- src/port/netplay_transport.cpp encode/decode: v5, 52 bytes, stateChecksum at byte 40 is used by runtime sendInput/receivePendingPackets as RTX1, not game state. Packet FNV validates transmission, not determinism.
- netplay_runtime.cpp PartyBoard_NetplayTick increments frame BEFORE HuPadRead and PartyBoard_RunGameLogicTick (src/game/main.c). Its checkpoint log is a pre-logic sample labelled with the following frame. Initial RNG agreement is not ongoing state verification.
- receivePendingPackets silently counts conflicting input/context as rejected. Retained old inputs are discarded before checking contradictions. Loss repair exists, but current/future inputs are sent separately, not an input history bundle.
- Full-game context capture allows 120 disagreeing input frames. objdll loading and renderer callbacks are outside the synchronized PAD tick. This is not evidence that the gameplay paths remain identical.
- main.c publishes GlobalCounter after rendering, or before a subsequent simulation tick in a batch. A canonical capture must specify exactly where it samples this counter.
- GameStart.OnlineArguments fixes delay at 3 and does not request rollback. Preserve that default in A. Bridge measures UDP heartbeat RTT but has no initial jitter/delay agreement.
- SessionProgress measures committed progress, so duplicate packets cannot extend its terminal 120-second deadline. Separate network health states are absent.

## Canonical state design
Read numeric fields explicitly, including each bitfield by name; encode each value as unsigned 32-bit big-endian, signed values modulo 2^32. Use versioned FNV-1a-64 over the resulting stream. Capture after accepted lockstep logic in CommitTick; frame F means AFTER logic/input F, BEFORE render/counter publication. Include context, both current RNG states, GlobalCounter, player configuration and gameplay state, system rules/turn/board/dice/minigame/results/flags and board_data bytes, effective minigame availability, persistent settings that affect gameplay, deterministic sequence counters. Keep a bounded immutable history and compare in frame order, independent of UDP arrival order. Never compare a speculative rollback snapshot.

Do not hash pointers, padding, OS handles, allocation addresses, audio/graphics resource IDs, save creation time or purely visual transforms. GlobalCounter is included as sampled, not silently normalized. Matching this projection is necessary, not proof of full-game equality: per-overlay private object state and coroutine locals require dedicated canonical exporters in later audits.

## Checkpoint / rollback audit
- rollback_scene.cpp Builder/signature and Header: raw region layout identity (address/size), pointerSize, process/module generations, I/O epoch; local resource and scene snapshots.
- process.c HuPrcSnapshotSave/Load: native coroutine stacks and thread addresses; exact topology identity required.
- objdll.c overlay snapshots: loaded module memory and local lifetime identity.
- gamework.c: memcpy of GameStat/SystemState/PlayerState plus globals; includes padding/create_time. Suitable for local restore validation, unsuitable for peer hashes.
- minigame_seq_snapshot.inc: function/data/pauseProcess pointers and sprite handles; canonical exporter must select logical numeric state.
- rollback_animation_test.cpp / rollback_scene.cpp: renderer model/sprite/texture clocks and presentation; tests use fixture assets.
- rollback_audio.cpp: confirmed external commands and virtual handles; native self-test uses mock backend. Speaker/device and callbacks are not portable state.
- rollback_io.cpp: capture lease rejects active asynchronous I/O; cannot replay arbitrary file/worker effects.
- rollback_clock.h + main.c: RNG/GlobalCounter restored locally. Rendering paths and waits remain audit items.
- rollback checkpoint currently gates active wipes and render callbacks; no cross-process relocation or host snapshot transfer exists. Do not activate resync restore or expand the six-frame live window.

## Further divergence candidates
Online skip boot is already forced off; minigame availability ignores local saves/cheat online. Other settings (Bowser unlock, language, message speed, records, pause/save flags), resource completion ordering, renderer callbacks, floating-point paths and private minigame state remain to verify. A canonical difference must be reported, not corrected by modifying offline preferences.

## Phase gate
A implements detection + protocol versioning + deliberate desync tests. B–G remain later logical changes.
Baseline native --netplay-self-test: PASS. Baseline launcher: 108 checks under the normal Windows profile (sandbox certificate private-key storage failed before TLS tests). Final launcher: 116 checks, including authenticated native traffic, UDP outage recovery, loss of TLS after commit and mandatory desync report export.
Full board → minigame → results → board validation is still required; subsystem probes are not a substitute.

## Implemented in A

Two independent wall-time RNG paths are fixed only when netplay is enabled: BoardRandInit now derives the board RNG from the synchronized frand state without consuming it; frand's zero-seed fallback uses a fixed salt with rand8 instead of OSGetTime. Offline retains both original time-based paths. The third RNG, boardRandSeed, is part of the canonical projection.

The projection also covers logical PAD values and sequence/dice state via read-only C exporters. Sequence floats that gate completion are serialized as IEEE-754 words, normalizing signed zero and NaNs. Format version 1 must change whenever the ordered projection changes. Field names are diagnostic metadata; repeated arrays are distinguished by field ordinal and explicit player/sequence indices.

Native protocol v6 uses 88 bytes: magic[0..3], version[4..5], type[6] (Input=1, Retransmit=2, State=3), player[7], session[8..11], sequence[12..15], input frame[16..19], input[20..27], config[28..31], initial seeds[32..39], hashAckNext[40..43], input context[44..47], hash format[48..51], hash frame[52..55], hash context[56..59], current frand/rand8[60..67], GlobalCounter[68..71], state hash high/low[72..79], zero reserved[80..83], packet FNV32[84..87]. Numeric words are big-endian. UINT32_MAX marks an absent hash frame; a standalone State packet requires a hash. The existing authenticated envelope is now 118 bytes (14 + 88 + 16); HMAC, replay window and control TLS remain intact. Native v5 peers are incompatible and both binaries/launchers must be updated together.

Each committed lockstep tick streams its latest digest; ordinary input/repair traffic also carries the oldest peer-unacknowledged digest. A 100 ms repair continues during stalls and after a detected desync. hashAckNext is the exclusive count of equal canonical states; it is neither an input ACK nor a portable checkpoint. Local and received histories retain 256 entries, and progress stops at a lead of 128 over either verification frontier. Comparisons run in frame order. Contradictory retained hashes or inputs are fatal protocol errors. Once stopped on DESYNC, the runtime resends the failed digest and cycles earlier retained hashes so a peer missing a prefix can still reach the same failure.

DESYNC logs the first divergent hash frame, both hashes/contexts/current RNGs/counters, last committed input and last equal state count. There is no confirmed portable checkpoint: diagnostics deliberately say last_checkpoint=none. The historical local projection is dumped by ordinal/name/value to stderr and a separate .desync file. The launcher exports this sidecar separately from its capped routine log. The two local dumps allow exact field comparison without sending full state over UDP.

## Validation and limits

- Build and native --netplay-self-test pass, including the actual canonical exporter, online RNG repeatability, offline board RNG path and the existing rollback suites.
- tools/test_netplay_state.ps1 passes 741033 assertions across 76 deterministic hash-stream profiles: RTT 0/50/100/150/250 ms, per-direction jitter +/-0/10/30 ms, loss 0/0.5/1/3/5%, duplicate/reorder and a burst/pause case. This tests StateHistory, not game inputs or a full board replay.
- tools/test_netplay_pad.ps1 passes the existing PAD regression (1161 checks), 2400-frame native pairs at delay 0/3/8, context desync at frame 100, disconnect without offline fallback, and injected coin desync at frame 87 at delay 0/3/8. The test-only disconnect deadline now accommodates the unchanged 120-second runtime watchdog. SDL_DelayPrecise fixes coarse Windows Sleep timing only inside the probe.
- Final authenticated launcher test passes 116 checks: 405 datagrams dropped, 153 reordered, a 6.5-second UDP outage recovered after frame 2364, and TLS loss at frame 1200 with both peers finishing and comparing all 2400 states.
- Actual dual-process boot with a local disc passes for 20 seconds and compares 10 common periodic checkpoints. Profiles are isolated with an explicitly supplied, online-only PARTYBOARD_NETPLAY_TEST_PROFILE. The user's offline config and memory card remain byte-identical (SHA-256). This is boot/title coverage, not board/minigame validation.
- Standalone existing regressions pass: lobby 269, rollback network 97361, snapshot restore 43, rollback effects 18313 and direct-connection self-test 15 checks.

The full PAD suite preceded final additions to canonical PAD/sequence fields and diagnostic sidecar export; final native self-test, authenticated 2400-frame pair, focused frame-87 field comparison and real boot were repeated on the final binary. Exact focused mismatch: ordinal 180, p.coins, host 0x2f4 vs guest 0x2f5; RNG/context/counter match.

Rollback remains disabled by default, with the experimental six-frame window unchanged. Canonical hashes are intentionally not captured when rollback is explicitly requested, including its lockstep fallback, because a confirmed-only historical projection is not yet wired to that path. No cross-process restore is enabled. Fixed delay 3, input RTX and the 120-frame context grace remain; strict canonical comparison can now expose a transition divergence before the legacy grace expires. This must be validated before production distribution. Routine received/rejected counters still mix useful traffic and harmless stale duplicates, so they are not a packet-loss estimate.

## Safe resync design constraints for E/F

Keep three distinct concepts: committed input frame, contiguous equal canonical state count, and a restorable checkpoint acknowledged by both peers. A confirmed hash alone cannot satisfy the third. A future host-coordinated checkpoint protocol needs a monotonic transition/checkpoint ID, expected context, build/disc/mapping identity, canonical schema version, bounded size and content digest. Both peers must stop at an explicit barrier and finish local loads, I/O, callbacks and side effects before reporting READY. Only a portable, validated state exporter may be transferred; current raw snapshots fail that requirement because of pointers, native stacks and lifetime identities.

On desync: stop simulation, select an actually acknowledged safe checkpoint, transfer its bounded host snapshot (never the current divergent state), validate it before mutation, restore both peers to that same checkpoint, compare freshly exported hashes, then commit the same agreed resume frame on both peers. Any version/topology/hash/timeout failure stays stopped. All unresolved input and hash history must be reset only as part of that coordinated commit. Transition IDs and idempotent acknowledgements must prevent delayed packets from a prior boundary from changing the new state. Until those prerequisites are implemented and the real board cycle passes, restart the session rather than attempting restore.
