# PS Vita trophies (`ISAAC_VITA_TROPHIES`)

Status: implemented behind default-OFF knobs; ON in the v0.2.0-alpha recipe
(`release/v0.2.0-alpha.cmake`; `release/v0.1.1-alpha.cmake` pins both knobs
OFF). The trophy packs carry game-derived text and art: they are built locally
and never committed.

Reworked 2026-10-09 after the first device runs (FW 3.65, NoTrpDrm 1.1 under
`*main`): at most **two** trophy contexts can be live in one process (the third
`sceNpTrophyCreateContext` returns `0x80551606`, `sceNpTrophyDestroyContext`
frees the slot), every new context needs **its own setup dialog** (otherwise
`0x80551612` SETUP_REQUIRED), and SceShell refuses the comm id `ISAACR001_0N`
(error C0-11133-9) while `ISAC00000_0N` works. The original design (all six
contexts open at once) is superseded.

## What it does

The game unlocks achievements 1..637 through
`PersistentGameData::TryUnlock(id)` (`sub_00484c80`). With the knob ON every
fresh unlock is mirrored as a PS Vita trophy in six local trophy sets
`ISAC00000_00`..`ISAC00000_05` (NoTrpDrm packs, no PSN), and the manager can
ask for a one-shot import of a save slot's existing achievements.

* **Seam.** `recomp/gen_all.py` emits a knob-fenced observer at the entry of
  TryUnlock in both translated copies: the owner `sub_00484c80` and the
  tail-jump copy inlined into `sub_00485ab0` (challenge completion, 45 ids).
  The observer
  (`isaac_vita_trophies_try_unlock_observe`, `recomp/runtime/host_vita_trophies.c`)
  only reads guest memory: it checks that `this` is the live
  PersistentGameData (manager pointer at RVA `0x7fd680` + `0x14`), that the id
  is 1..637, not already unlocked (`[pgd+0x38+id]`) and that the PGD is not
  ReadOnly (`[pgd+1]`), i.e. exactly the body's own guards, and then sets one
  bit in a 640-bit pending set and signals a semaphore. The translated body
  always runs afterwards, so the game behaves the same with the knob on or off.
* **Two live sets, the rest deferred.** During play only the *gameplay pair*
  is live: the two lowest sets that still have a locked trophy. An unlock for
  another set is written to the pending store `trophy-pending.bin` and
  delivered at the next game start. No setup dialog is ever shown during
  gameplay (owner decision 2026-10-09).
* **Worker.** One thread (priority 191, 64 KiB stack, CPU 2) owns every
  handle / unlock / state call: live unlocks first, then the store's ids, the
  diagnostic test request and the import (live events are re-drained between
  import ids). It maps the id to a set and trophy, skips excluded ids (325,
  336, 354) and trophies the system already reports as unlocked, calls
  `sceNpTrophyUnlockTrophy` and pauses 100 ms after each call. It alone
  writes the pending store.
* **Mapping.** Sets: `_00` 1..89, `_01` 90..178, `_02` 179..276, `_03`
  277..403, `_04` 404..520, `_05` 521..637; the three Daily Challenge ids 325,
  336, 354 are in no set. Trophy = id - first + 1 - (excluded ids in
  [first, id)); trophy 000 is each set's platinum and is never unlocked
  directly. A trophy pack must number its trophies by the same rule;
  `recomp/test_vita_trophies.py` pins the SHA-256 of the canonical mapping
  dump, `f3b12afbc0acdbcb0fc930fee8ddd69d7bf9d96b588d04c092e16f94e07e7233`.
  The packs themselves are user-supplied: no pack and no pack generator is
  part of this repository (see `ISAAC_VITA_TROPHY_PACK_DIR` below for what
  configure accepts).

## Knobs

| knob | default | effect |
|---|---|---|
| `ISAAC_VITA_TROPHIES` | OFF | the seam in the two TryUnlock owner units, `host_vita_trophies.c`, both boot hooks, the NP stubs. Needs `ISAAC_VITA_KAGE=ON`, refused for scaffold builds. |
| `ISAAC_VITA_TROPHY_TEST_REQUEST` | OFF | diagnostic: honour `trophy-test.req` (below). Needs `ISAAC_VITA_TROPHIES=ON`. |
| `ISAAC_VITA_TROPHY_PACK_DIR` | "" | absolute directory holding `ISAC00000_00/TROPHY.TRP` .. `ISAC00000_05/TROPHY.TRP` (the NP comm id base `ISAC00000`, not the title id: SceShell refused `ISAACR001_0N` with C0-11133-9 on the device, 2026-10-09). Checked at configure time by `recomp/vita/trophy_pack_check.py` (comm id shape 4 letters + 5 digits, container, SHA-1, npcommid == directory, trophy counts 90/90/99/125/118/118, one platinum 000) and packed as `sce_sys/trophy/ISAC00000_0N/TROPHY.TRP`. Empty with the knob ON: a STATUS warning and no trophy members (the failure-policy build). Non-empty with the knob OFF: FATAL. |

With every knob OFF the generated units, `entry_vita.c` and
`kage_vita_backend.c` preprocess to today's text, the runtime unit is not
compiled, and the VPK member list and `param.sfo` are unchanged.

## Building

Trophy builds are configured with `cmake -C <recipe>` and built with Ninja
(VitaSDK required). Overrides on top of the release recipe:

```
# trophies ON with packs (user-supplied, kept outside the repository)
-DISAAC_VITA_TROPHIES=ON -DISAAC_VITA_TROPHY_PACK_DIR=/abs/path/to/packs

# diagnostic build: ON + the test request
-DISAAC_VITA_TROPHIES=ON -DISAAC_VITA_TROPHY_TEST_REQUEST=ON -DISAAC_VITA_TROPHY_PACK_DIR=/abs/path/to/packs

# failure-policy build: ON without packs
-DISAAC_VITA_TROPHIES=ON
```

`tools/build_vita.py` is unchanged: its `verify_package` rejects trophy
members, so a trophy build through it fails closed. Configure prints
`Isaac Vita trophies: enabled in owners <guest_XXXX.c>; test request <ON|OFF>;
<packs DIR|no packs>` and, with packs, one `trophy pack OK: ISAC00000_0N ...`
line per set. Host gates: `python recomp/test_vita_trophies.py`
(oracle with clang and gcc, mapping hash, save checksum, CMake legs; add
`--generated-dir <corpus>` for the census on a real corpus and
`--real-saves FILE...` for real saves), `python
recomp/test_vita_trophy_unlock_seam.py --pe <PE> [--generated-dir <corpus>]`,
`python recomp/vita/test_trophy_pack_check.py`.

## Where it runs, and why there

* **Stage A** `isaac_vita_trophies_boot()` in `entry_vita.c`, right before
  `guest_register_all()`, after the other pre-vitaGL reserves: sysmodule
  `SCE_SYSMODULE_NP_TROPHY`, `sceNpTrophyInit`, the semaphore and the worker
  thread, then the pending store, the gameplay pair (from the saves) and the
  import and test requests. **No context** is created here. Before vitaGL so
  the NP module and the worker stack come out of vitaGL's pool, not out of the
  tight post-init window; before the guest so reading the saves cannot race a
  game write.
* **Stage B** `isaac_vita_trophies_setup(present)` in
  `kage_vita_backend_initialize`, cold path, right after the `GL_VERSION`
  check. The work list is (a) every set outside the pair that has queued ids
  (pending store, import, test request), in set order, then (b) the pair. For
  a set of (a): create its context (comm id `ISAC00000` + set number, the
  NoTrpDrm signature), show its setup dialog, hand it to the worker, wait
  until the worker has delivered its ids (bounded: 5 s without progress),
  destroy the context, go on. The pair is opened the same way and handed to
  the worker when Stage B ends; it stays live for the whole session and
  delivers its own queued ids in the background. So at most two contexts
  exist, no `UnlockTrophy` runs while a dialog is shown, and the end state is
  exactly the pair (or fewer after failures).
* **Dialogs.** Each pumped by `vglSwapBuffers(GL_TRUE)` for at most 600
  frames (10 s); a dialog still running then gets
  `sceNpTrophySetupDialogAbort` and at most 300 more frames. After the first
  timeout no further dialog is shown in that boot (the remaining sets keep
  their ids pending), so one broken dialog costs at most 15 s, not 15 s per
  set. Only here does the thread own GXM with no scene open, before the
  loading overlay callback, the scheduler, the watchdog and the present
  counters exist. The warm re-initialize path returns earlier; the call is
  idempotent anyway.
* **Destroying safely.** A context is destroyed only after the worker has
  been seen outside it: the worker publishes the set it is about to use and
  then re-reads the set's slot (both seq_cst), the main thread marks the slot
  closed and then waits (at most 2 s) for the worker to leave. A worker stuck
  inside a call keeps its context (logged `destroy ... reason=busy`).

Measured costs (device run 2026-10-09): a setup dialog takes ~0.92 s
(55-58 frames) for an installed set and ~4.0 s (241-245 frames) on first
install; `UnlockTrophy` ~0.48 s, plus the 100 ms pacing. The host oracle
replays those costs (`oracle model ...` lines of the host gate):

| boot | Stage B |
|---|---|
| normal, nothing pending, pair installed | 1.9 s (2 dialogs) |
| first boot, installing the pair | 8.2 s (2 dialogs) |
| import of 100 ids in 3 sets outside the pair (+30 in the pair), installed | 62.7 s (5 dialogs, 100 unlocks at boot) |
| the same, first install | 78.4 s |

Rule of thumb: Stage B = 0.93 s per installed set opened (4.1 s on first
install) + 0.58 s per trophy delivered outside the pair. Trophies of the pair
are delivered during play and do not lengthen the boot.

## Pending store

`ux0:/data/isaacr001/trophy-pending.bin`, 200 bytes, little endian: magic
`ISAACTRP`, version 1, id max 637, a 640-bit set of pending achievement ids,
a mask of sets whose state is known and each set's last
`GetTrophyUnlockState` flags, then the game's save checksum over the first 196
bytes. Written only by the worker, atomically (temp created with O_EXCL,
synced, the old file moved aside, the temp moved into place, the aside copy
removed; the same protocol as `options.ini` in `host_vita_render_scale.c`). A
live unlock whose set is not live is added (`pending id=...`); a bit is
cleared only when `UnlockTrophy` returned 0 or `0x8055160f` (already
unlocked), or the set's state read says the trophy is unlocked. A missing file
is "nothing pending"; a file with a wrong size, magic, version, id range,
checksum or impossible bits is treated as empty and replaced by the next
publish; a file left aside by an interrupted publish is moved back.

## Gameplay pair

The two lowest sets that still have a locked trophy. A trophy counts as
unlocked when its id is set in any of
`persistentgamedata1..3.dat` (read with the import parser, a bad save is
ignored) or in the set's last known Vita state from the pending store. No
readable save: sets 0 and 1. One set left: only that one. Nothing left: no
live set (any later event goes to the store). The choice is logged (`pair`).

## Failure policy

Every failure turns off only what it touches, logs it, and never asserts,
blocks without a bound or changes guest behaviour.

| failure | effect | log |
|---|---|---|
| sysmodule load rc<0 | feature off: no thread, observer inert, Stage B no-op, import `unavailable` (request kept), store and saves not read | `boot module=0x... init=- ...`, `disabled stage=module` |
| `sceNpTrophyInit` rc<0 | same | `disabled stage=init` |
| semaphore / thread create (191, then 0x10000100) / start rc<0 | same | `worker rc=...`, `disabled stage=thread` |
| `CreateContext(n)` rc<0 | set n not live this boot; its ids stay queued (live ids go to the store) | `ctx set=n ... rc=...` |
| setup dialog init rc<0 / GetResult rc<0 or result not OK | context destroyed at once, set n not live this boot | `setup set=n status=init-failed|result-failed`, `destroy set=n ... reason=setup` |
| setup dialog timeout (600 frames + Abort) | as above, and no further dialog this boot (later sets `skipped`) | `setup set=n status=timeout`, `destroy ... reason=setup`, `setup set=m status=skipped` |
| a delivery set makes no progress for 5 s | its remaining ids stay queued for the next boot; the set is destroyed | `deliver set=n status=stalled left=k`, `destroy ... reason=stalled` |
| the worker is still inside a call on a set to destroy (2 s) | the context is kept, never destroyed under the worker; the pool then has one slot less this boot | `destroy set=n rc=0x00000000 reason=busy` |
| no set live after Stage B | the observer stays on: live unlocks go to the store; a pending import resolves as `noset` and writes `partial` | `setup done live=0/...` |
| `UnlockTrophy` rc 0x80551612 (setup required) | set not live from then on; the id goes to the store (live) or counts as noset | `unlock ... rc=0x80551612`, `set-dead set=n` |
| `UnlockTrophy` rc 0x8055160f (already unlocked) | counted as already, cached, store bit cleared | `unlock ... rc=0x8055160f` |
| any other rc<0 (also `CreateHandle`) | counted as failed; a live id goes to the store and is retried at the next start, an import or test id by its request | `unlock ... rc=...` (+ `pending id=...` for live) |
| store publish fails | the previous file stays in place; retried on the worker's next pass | `pending save ... status=write-error|rename-error` |

## Manager contract: import request

Files live in `ISAAC_VITA_NATIVE_DATA_ROOT` = `ux0:/data/isaacr001`.

| file | writer | content |
|---|---|---|
| `ux0:/data/isaacr001/trophy-import.req` | the manager (or the user by FTP) | one ASCII digit `1`, `2` or `3` (the save slot) with optional space, tab, CR or LF around it; at most 16 bytes |
| `ux0:/data/isaacr001/trophy-import.res` | the game runtime | one line, overwritten each time (format below) |
| `ux0:/data/isaacr001/Documents/My Games/Binding of Isaac Repentance/persistentgamedata<N>.dat` | the game | the save that is read (no `rep_` prefix on the Vita) |

Processing, once per boot (the manager writes the request, then launches the
game):

1. Stage A reads the request. If the trophy feature is unavailable at that
   point (sysmodule, init or the thread failed): result `unavailable`, slot =
   the requested digit if valid else 0, the request is **kept** and the save
   is not read.
2. Content other than a valid slot (including an unreadable request, an empty
   file or more than 16 bytes): `bad-request`, `slot=0`, request deleted.
3. The save is read once, streamed through the game's checksum with a 4 KiB
   buffer: cannot open -> `no-save`; size or read failure -> `read-error`;
   more than 1 MiB -> `too-large`; magic not `ISAACNGSAVE09R  ` ->
   `bad-magic`; checksum over `[0x10, size-4)` not equal to the LE u32 at
   `size-4` -> `bad-checksum`; chunk at 0x14 not id 1, or the file shorter
   than `0x20 + min(count, 0x27e) + 4` -> `bad-chunk`. Each of these is final:
   the result is written and the request deleted.
4. A valid save: ids 1..637 whose byte, read as **signed**, is > 0 are
   `saved`; minus 325/336/354 they are `mapped` and queued. Ids of sets
   outside the gameplay pair are delivered at Stage B (their sets are opened
   one at a time, see above); ids of the pair right after Stage B, in the
   background, with live unlocks keeping priority. So an import completes in
   one boot. Then the result: `done` when `failed=0` and `noset=0` (request
   deleted), otherwise `partial` (request kept; the next boot retries, and
   trophies already unlocked come back as `already`). `noset` counts ids
   whose set could not be opened this boot.
5. Idempotent: the same request again gives `unlocked=0 already=<mapped>`.

`request=deleted` is written only when the remove actually succeeded.
Result line (log) and file (`trophy-import.res`, the same text without the
leading `KAGE VITA ` and with a final `\n`); the format and the meaning of
every field are unchanged by the 2026-10-09 rework:

```
KAGE VITA TROPHY IMPORT: slot=%u status=%s saved=%u mapped=%u unlocked=%u already=%u failed=%u noset=%u request=%s
TROPHY IMPORT: slot=1 status=done saved=28 mapped=28 unlocked=27 already=1 failed=0 noset=0 request=deleted
```

`status` is one of `done`, `partial`, `bad-request`, `no-save`, `read-error`,
`too-large`, `bad-magic`, `bad-checksum`, `bad-chunk`, `unavailable`;
`request` is `deleted` or `kept`. No request file means no result is written.
No import line is logged per id; only actual `UnlockTrophy` calls log an
`unlock src=import` line. A large import lengthens that one boot by ~0.58 s
per trophy outside the pair (see the table above).

## Diagnostic test request (`ISAAC_VITA_TROPHY_TEST_REQUEST=ON` only)

`ux0:/data/isaacr001/trophy-test.req`: at most 8 decimal achievement ids
1..637 separated by whitespace, at most 64 bytes. Read at Stage A (only when
the feature is up; otherwise the file is not touched), queued as `src=test`
(`TEST: queued ids=<distinct ids>`) and delivered like import ids (sets
outside the pair at Stage B), deleted after the worker processed them
(`TEST: done ...`). Anything else queues nothing, logs `TEST: queued ids=0`
and deletes the file. Without the knob the path is never opened. Use an id the
player has already earned so the test trophy is honest.

## Log lines

All through `isaac_vita_log`, one call per line:

```
KAGE VITA TROPHY: boot module=0x%08x init=0x%08x title=ISAACR001 sets=6
KAGE VITA TROPHY: boot module=0x%08x init=- title=ISAACR001 sets=6
KAGE VITA TROPHY: ctx set=%u comm=ISAC00000_%02u rc=0x%08x ctx=%d
KAGE VITA TROPHY: worker rc=0x%08x prio=%u stack=65536 affinity=0x%08x
KAGE VITA TROPHY: disabled stage=%s rc=0x%08x
KAGE VITA TROPHY: setup set=%u status=%s init=0x%08x result=%d frames=%u us=%u
KAGE VITA TROPHY: setup done live=%u/%u dialogs=%u timeouts=%u us=%u
KAGE VITA TROPHY: destroy set=%u rc=0x%08x reason=%s
KAGE VITA TROPHY: deliver set=%u status=%s left=%u us=%u
KAGE VITA TROPHY: pending id=%u set=%u trophy=%03u
KAGE VITA TROPHY: state set=%u rc=0x%08x count=%d bits=%08x%08x%08x%08x
KAGE VITA TROPHY: event id=%u ra=0x%08x
KAGE VITA TROPHY: readonly id=%u ra=0x%08x
KAGE VITA TROPHY: pgd-mismatch pgd=0x%08x mgr=0x%08x id=%u ra=0x%08x n=%u
KAGE VITA TROPHY: id-range id=%u ra=0x%08x
KAGE VITA TROPHY: unlock src=%s id=%u set=%u trophy=%03u rc=0x%08x plat=%d us=%u
KAGE VITA TROPHY: skip src=%s id=%u reason=%s
KAGE VITA TROPHY: set-dead set=%u rc=0x%08x
KAGE VITA TROPHY: platinum set=%u trophy=%d
KAGE VITA TROPHY IMPORT: slot=%u status=%s saved=%u mapped=%u unlocked=%u already=%u failed=%u noset=%u request=%s
KAGE VITA TROPHY TEST: queued ids=%u
KAGE VITA TROPHY TEST: done ids=%u unlocked=%u already=%u failed=%u
KAGE VITA TROPHY: pending load status=%s ids=%u known=0x%02x
KAGE VITA TROPHY: pending save ids=%u status=%s rc=0x%08x
KAGE VITA TROPHY: pair a=%d b=%d rule=%s saves=%u known=0x%02x locked=%u,%u,%u,%u,%u,%u
```

* `disabled stage`: `module`, `init`, `thread` (a context or dialog failure
  no longer disables the feature).
* `ctx`: one per context created at Stage B; `ctx` is the handle (-1 on
  failure).
* `setup status`: `ok`, `init-failed`, `timeout`, `result-failed`, `skipped`
  (not attempted: an earlier dialog timed out this boot, or the pool was
  full). `result` is the dialog result, or the GetResult error when that call
  failed. `frames` includes the Abort tail.
* `setup done`: `live` = pair sets live / sets in the pair; `dialogs` shown;
  `timeouts`; `us` = all of Stage B.
* `destroy reason`: `setup` (its dialog failed), `delivered`, `stalled`, or
  `busy` (not destroyed: the worker was still inside a call; rc is 0).
* `deliver status`: `drained` or `stalled`; `left` = ids of the set still
  queued.
* `pending id`: a live unlock kept for the next start (its set is not live,
  or the unlock failed).
* `pending load status`: `ok`, `recovered` (an interrupted publish left the
  file aside; moved back), `absent`, `read-error`, `garbage` (treated as
  empty, replaced by the next publish). `pending save status`: `ok`,
  `write-error`, `rename-error` (the previous file is back in place).
* `pair`: the gameplay pair (`-1` = none). `rule`: `locked`, `fallback` (no
  readable save: sets 0 and 1), `complete` (nothing left to unlock); `saves`
  = readable `persistentgamedata1..3`, `known` = sets whose last Vita state is
  in the pending store, `locked` = locked trophies per set.
* `state bits`: the four 32-bit words of the set's unlock flags, word 0
  (trophies 0..31) first.
* `event` / `readonly` / `pgd-mismatch` / `id-range` come from the game thread;
  `ra` is the bare return RVA of the TryUnlock call. `readonly` is logged once
  per id; `pgd-mismatch` and `id-range` only the first four times (`n` counts
  all).
* `unlock src` / `skip src`: `live`, `test`, `import`, `pending` (an id from
  the store; skip lines for every source but import); `skip reason`:
  `excluded`, `noset`, `already`. `plat` is the platinum id the system
  reports (-1 none); `us` covers handle + unlock + destroy, not the 100 ms
  pause.

## Mods and the debug console: a game rule, not a port bug

With any mod or the debug console enabled, the game marks every run ReadOnly
unless the save already has achievement 4 (`sub_004b3eb0`). On such a save
nothing unlocks: no achievement and therefore no trophy (the observer logs
`readonly id=... ra=...` once per id). This is the game's own behaviour and is
kept as is (owner decision 2026-10-05). Achievements that already exist in a
save reach the trophy sets only through the import request above.

## Device test plan

Prerequisites: firmware supported by NoTrpDrm (3.60/3.65/3.67/3.68), NoTrpDrm
under `*main` in `ur0:tai/config.txt` and a reboot since. Back up
`persistentgamedata{1,2,3}.dat` by FTP first. Decide the system language before
the first launch. Read `ux0:/data/isaacr001/first-arm-fault.log` filtered by
the build stamp. Remove any `trophy-pending.bin` left by an older build.

The test slot must contain achievement 4 (the mod rule above; repro_console is
a mod): copy File 1 over slot 2 by FTP (`persistentgamedata1.dat` ->
`persistentgamedata2.dat`; the checksum covers content only).

| run | setup | proves | log lines that must appear |
|---|---|---|---|
| A boot + pair | ON+TEST build with packs (ISAC00000_0N); `trophy-test.req` = an earned id of the pair (e.g. `1`) | module, pair, dialogs, worker, one real trophy | `boot module=0x00000000 init=0x00000000`; `worker rc=0x00000000`; `pending load status=absent`; `pair a=0 b=1 ...` (or the sets the saves imply); `TEST: queued ids=1`; `ctx set=<a> comm=ISAC00000_0<a> rc=0x00000000`, `setup set=<a> status=ok` (frames ~245 first install, ~56 later), the same for `<b>`; `setup done live=2/2 dialogs=2 timeouts=0`; `state set=<a>` and `set=<b>` `rc=0x00000000`; `unlock src=test id=1 ... rc=0x00000000` (or `0x8055160f`); `TEST: done ids=1`; no `0x80551606`, no `0x80551612` |
| A' delivery set | the same build; `trophy-test.req` = an earned id of a set outside the pair | open / deliver / destroy / reopen of the pool | before the pair: `ctx set=<n> ... rc=0x00000000`, `setup set=<n> status=ok`, `state set=<n> rc=0x00000000`, `unlock src=test id=<id> set=<n> ...`, `deliver set=<n> status=drained left=0`, `destroy set=<n> rc=0x00000000 reason=delivered`; then the pair as in A; `setup done live=2/2 dialogs=3` |
| B seam | repro_console script `once` / `giveitem c133` / `giveitem c134`; New Run on File 2 (no seed, no challenge) | the TryUnlock seam end to end (achievement 31, set 0) | game log `unlock steam achievement '31'`; `event id=31 ra=0x0032b4cd`; `unlock src=live id=31 set=0 trophy=031 rc=0x00000000` if set 0 is in the pair, else `pending id=31 set=0 trophy=031` and `pending save ids=1 status=ok`; no `readonly id=31` |
| C import | `trophy-import.req` = `1` | the manager contract | `deliver` / `destroy ... reason=delivered` lines for the sets outside the pair that hold ids; `TROPHY IMPORT: slot=1 status=done saved=<k> mapped=<k> unlocked=<u> already=<a> failed=0 noset=0 request=deleted`; `trophy-import.res` holds the same line; the request is gone; note `setup done ... us=` (boot cost) |
| C' idempotency | the same request again | idempotent | `status=done ... unlocked=0 already=<k>` |
| D failure (optional) | ON build without packs, or without NoTrpDrm | silent off, nothing lost | `ctx` / `setup` lines with rc<0 or `status=timeout` (at most one timeout, then `skipped`), `setup done live=0/2`; live unlocks log `pending id=...`; the game boots and plays; no FAULT |
| D' delivery after D | the build with packs again, boot once | the store reaches the Vita | `pending load status=ok ids=<n>`; `unlock src=pending ...`; `pending save ids=0 status=ok` (or fewer ids) |
| E ReadOnly (optional) | run B's script on a slot without achievement 4 | the mod rule is visible | `readonly id=31 ra=0x0032b4cd`, no unlock line |

In every run also check: no `FAULT`, no `guest stop`, the boot reaches the
menu, and the `setup done ... us=` value (compare with the table above).
Expected extra events (not bugs): opening the save-select screen loads slots
1..3 and runs the game's retroactive checks for each, so an `event` with `ra`
inside RVA 0x484a90..0x484c35 is a genuine unlock; composite achievements
arrive as nested events with `ra` inside `sub_00485650`.

Open questions for the device run: whether `UnlockTrophy` stays at ~0.48 s
during a long boot delivery (the `us=` values), whether an aborted error dialog
leaves SceShell in a state where the next boot's dialogs work, whether the
Russian `TROP_08.SFM` works under NoTrpDrm, and whether the 100 ms pacing needs
tuning.
