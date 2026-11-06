# repentogxm -- release notes

The VPK of each release is attached to the GitHub Releases page together with
its SHA-256 and the eboot SHA-256; the game's resources are not included and
come from your own PC copy. What runs on real hardware, how fast, and what is
broken is recorded in [../STATUS.md](../STATUS.md); the priorities are in
[../PLAN.md](../PLAN.md).

## Unreleased

- Manager pages other than HOME showed only the lower part of the screen
  on the device (issue #1 photos: no header, panel from about row 156,
  text from row 258, footer intact) while pngshot screenshots looked
  complete. The manager drew straight into the one CDRAM buffer the
  display was scanning and redrew those pages every frame, phase-locked
  to the vblank, so the beam raced the CPU redraw: rows scanned before
  the header fill landed showed the cleared background. Every draw now
  targets a RAM canvas; `manager_present` copies a finished frame into
  the off-screen half of a two-buffer CDRAM block, flips with
  `SCE_DISPLAY_SETBUF_NEXTFRAME` and waits for the flip; sub-pages redraw
  only on a button edge. Device confirmation pending (the LCD is the only
  witness; screenshots never showed the glitch).
- At the Mom's Heart death the main loop stalled 128 ms and 317 ms
  (2026-09-10, nt2p); 45.5 ms and 28.6 ms of that were the game's own
  `log.txt` writes, one synchronous `sceIoWrite` of 15-25 ms per record on the
  game thread, because the guest logger flushes after every record and the
  Vita CRT hands the flush straight to newlib. New knob
  `ISAAC_VITA_GAME_LOG_ASYNC` (default OFF; needs
  `ISAAC_VITA_ARCHIVE_FILE_CACHE` and `ISAAC_VITA_LOG_ASYNC`) keeps the log
  bytes in a host ring appended by one worker thread; INFO flushes return
  without storage I/O, WARN/ERROR/ASSERT flushes append synchronously before
  control returns, every controlled exit drains, and any read, seek, stat or
  reopen of `log.txt` first drains, so the file stays byte-identical to the
  synchronous result. No generated object changes; the OFF build links no
  wrapper. Pinned by `recomp/test_vita_game_log_async.py`. The first device
  build of the knob (int2, 2026-09-10) stopped before the first present with
  the game's `Failed to obtain writable save data directory!` box: the two
  guest string formatters left the lane flag of their output record unset,
  so stack garbage routed every `sprintf` byte away from the guest buffer
  and the save-path probe saw an empty string. Both formatters now clear the
  flag; the gate pins every formatter output to a cleared flag. Device
  measurement pending.
- Every cutscene with a video part (all endings; `cutscenes.xml` ids 3 and
  up) stopped v0.1.1-alpha with `guest stop: addr=0x005e0050
  fault=untranslated: popfd` and a controlled exit to the LiveArea, no
  psp2core dump. theoraplayer opens the `.ogv` with libtheora 1.2.0alpha,
  whose CPU-feature probe toggles EFLAGS.ID through `pushfd`/`popfd`, and
  `popfd` was the one instruction of that function the translator did not
  emit. The translator now emits `popfd` (CF PF ZF SF OF as an explicit flag
  snapshot, DF and ID into their CPU fields) and `pushfd` reports the ID bit,
  so the probe succeeds and libtheora reads the port's conservative CPUID
  profile (no MMX: the MMX decoder paths stay untranslated and unreachable).
  Reproduced and verified with the console command `cutscene 4` on the
  2026-09-09 test device.
- With `popfd` in place the same cutscene stopped with `bad_alloc`:
  theoraplayer's frame queue allocates 8-16 RGBA frames of 816x480 (1.5 MiB
  each) and after a run with a mod loaded the guest heap has about 1 MiB
  left; the 8 MiB overflow pool took four frames and the fifth failed. New
  knob `ISAAC_VITA_THEORA_PRECACHE_FRAMES` (default 0 = untouched; the
  release build sets 3) wraps `TheoraFrameQueue::setSize` with
  `--wrap=sub_005a4c60` and caps the requested count on the guest stack
  before the untouched translated body runs, so no generated object changes
  and the OFF build links no wrapper. A shorter queue only shortens the
  decoder's read-ahead. Pinned by `recomp/test_theora_precache_sites.py`.
- Behind that, the first decoded frame stopped the game with `fault=getenv
  received an unmeasured variable name return=0x005d652a`: theoraplayer
  converts frames through libyuv, whose CPU-dispatch init asks `getenv` for
  twelve `LIBYUV_DISABLE_*` switches, the third and last getenv reference in
  the image. The CRT shim now answers NULL for all twelve (they are unset on
  a PC), so libyuv dispatches on the port's CPUID profile (X86 + SSE2) and
  runs its C row functions; the SSSE3/SSE4/AVX variants remain untranslated
  stubs and are never selected. Pinned by `recomp/test_crt_getenv_names.py`.
- With those three fixed the clip loads and the screen stays black at 60 FPS:
  theoraplayer decodes on a worker thread (`KERNEL32!CreateThread`, unresolved
  on the Vita) and the generator's worker bypass leaves the manager with zero
  workers, so no frame is ever decoded. New knob
  `ISAAC_VITA_THEORA_WORKER_PUMP` (default 0 = untouched; the release build
  sets 1) keeps a host-resident `TheoraWorkerThread` and runs the untouched
  worker loop as one bounded slice per game frame on the main thread, entered
  from the game's own per-frame `TheoraVideoManager::update`
  (`--wrap=sub_00598d60`, the first call of every main-loop iteration);
  `--wrap=sub_005aa6d0` caps the decodes per slice and the Sleep shim ends a
  slice instead of delaying.
  No generated object changes; the OFF build links no wrapper. Pinned by
  `recomp/test_theora_worker_pump_sites.py`.
  Device (2026-09-10, popfd5/popfd6 builds): the first pump build froze the
  device after three slices because its Sleep intercept compared the return
  address on the guest stack against `GUEST_IMAGE_BASE + RVA`; translated call
  sites push the bare RVA, so a full frame queue slept the real `Sleep(100)`
  forever. Fixed (bare-RVA comparison, `no_work` counter); the ending video
  then plays to the end, the credits follow and the run continues.
- The video plays at 1-2 frames per second with bursty audio: the translated
  libtheora needs about 25 ms of the main thread per 816x480 packet, the clip
  timer runs ahead, theoraplayer pre-drops all but every sixteenth frame and
  `decodeNextFrame` feeds the audio only once per kept frame (popfd6 receipts:
  46 kept frames in the first 40 s, end of file at ~245 s, 4306 idle slices
  after it). New knob `ISAAC_VITA_NATIVE_THEORA` (default OFF) decodes with
  the VitaSDK `libtheoradec` compiled natively: thirteen `--wrap` seams on the
  cross-unit `th_*` calls from `TheoraVideoClip_Theora` read the cdecl
  arguments from the guest stack and call the native function on the same
  memory (`th_info`, `ogg_packet` and `th_ycbcr_buffer` have identical
  layouts on x86-32 and ARM32; guest address == host address), returning in
  `eax`, `edx:eax` or `ST(0)` as the translated caller expects; a timed
  pass-through on `TheoraVideoFrame::decode` reports the remaining YUV->RGB
  cost. The pump receipt now carries the slice time. No generated object
  changes; the OFF build links neither wrapper nor libtheoradec. Pinned by
  `recomp/test_native_theora_sites.py`. Device 2026-09-10 (nt1, a full run
  to Mom's Heart): the ending video's headers parsed (960x544, 30 fps) and
  `th_decode_alloc` answered NULL: libtheoradec allocates from the raw newlib
  arena, which is the guest heap and had no 2.5 MiB left at the end of the
  run (the overflow pool that saves the guest's own frames is not reachable
  from a native `malloc`); theoraplayer stored the NULL decoder unchecked and
  the native `th_granule_frame` read it (C2-12828-1, data abort at +0x38).
  Fixed: the archive is linked as a copy whose four allocator imports are
  renamed by objcopy to the seam's pool allocator (a private 8 MiB, else
  6 MiB, memblock mspace created at the first allocation of a video and
  released with its last free; guest-heap fallback), and every seam that
  receives the decoder pointer answers a NULL one with `TH_EFAULT` / -1, so a
  failed allocation ends the video instead of the process. Device 2026-09-10
  (nt2, the same ending video after a full run): the 8 MiB memblock was
  refused (about 5 MiB of user memory free at that point) and the 6 MiB one
  created, the decoder used 4.7 MiB of it, no guest-heap fallback, no crash,
  the video played to the end and the pool was released with its last free.
  Speed: the native `th_decode_packetin` takes about 18 ms per 960x544 packet
  (mean), the translated libyuv YUV->RGB conversion 86 ms per kept frame, and
  a `decodeNextFrame` slice still ran 390-800 ms early in the clip, so 59 of
  712 frames were kept and the audio stayed bursty. New knob
  `ISAAC_VITA_NATIVE_THEORA_RGB` (default OFF, requires the native decoder)
  runs the conversion natively: one `--wrap` seam on libyuv's
  `I420ToRGB24Matrix` (two direct callers, no address-taken reference)
  reproduces the C reference row math in NEON with the coefficients read from
  the table the caller passes, honours the negative-height flip and answers
  0 / -1 like the original; the theora seam also times the clip's Vorbis
  audio decode (`vorbis_synthesis` pass-through) to attribute the rest of a
  slice. The Vita cpuid seam reports no SSSE3, so the translated build ran
  libyuv's C row and the native frame is bit-identical to it. Device
  2026-09-10 (nt3): the conversion fell from 86 to 18.6 ms per frame, the
  packet decode took 16.3 ms and the Vorbis decode 1.5 ms per packet; the
  video ran at real time with about 4-5 frames per second shown. The row now
  runs in int16 lanes, sixteen pixels per step, when the loaded constants pass
  a range guard (both frozen tables do; exhaustive proof in
  `recomp/test_native_yuv_s16.py`), else in 32-bit lanes. Pinned by
  `recomp/test_native_theora_sites.py`.
- The size of the second-tier guest-heap pool (the USER_RW SceClibMspace
  served after newlib exhaustion, `ISAAC_VITA_HEAP_OVERFLOW_MSPACE`) is now
  the knob `ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES`. Default unchanged at
  `0x007d5000` (8,212,480 bytes), so the released eboot and
  `release/v0.1.1-alpha.cmake` reproduce byte-identically; the retained
  span, the combined ledger+pool budgets and the RoomConfig slab remainder
  derive from the one value. Why: both recorded heap deaths were second-tier
  exhaustion, not the 81 MiB arena alone -- the 307,200-byte music-stream
  request after 27 minutes found 295,178 bytes free in the 8 MiB pool, and
  the fifth 1.5 MiB theora frame after a run failed the same way. Proposed
  pair for the next release: `ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES=0x017fd000`
  (24 MiB class, +16.9 MB second tier) with `ISAAC_VITA_LUA_ARENA_MB=33`
  (vitaGL RAM pool net -1.2 MB); expected boot receipt `guest heap overflow
  PASS: ledger_capacity=524288 pool_bytes=25153536`. Device verification
  pending.
- New knob `ISAAC_VITA_GUEST_STACK_MEMBLOCK` (default OFF, not set by the
  release configuration): the 4 MiB guest stack comes from a dedicated
  USER_RW memblock `isaac_guest_stack` (request 0x401000, so the low set bit
  stays 4 KiB) instead of newlib `memalign`, which frees 4,194,304 bytes of
  the 81 MiB guest heap and costs the vitaGL RAM pool 4 MiB + 4 KiB. One
  `guest stack reserve: status=ready path=memblock uid= base=` receipt at
  boot; a failed reserve stops the boot with the existing `guest setup
  FAILED: 4 MiB guest stack allocation` line (no silent fallback). With the
  generated stack guard OFF, an esp excursion below the stack now faults
  visibly instead of corrupting heap chunks. Diagnostic builds that read the
  guest stack from another core (`ISAAC_VITA_GUEST_SAMPLER`,
  `ISAAC_VITA_LUA_RECEIPT`; both OFF in the release configuration) stop
  those readers before the stack is released (`KAGE VITA GUEST SAMPLER:
  stopped` and `[isaac-lua] beat stopped` receipts) and `guest_stack_free`
  publishes the unbinding before the unmap. OFF keeps `guest.c` and
  `entry_vita.c` byte-identical. Pinned by
  `recomp/test_guest_stack_memblock.py` and
  `recomp/vita/test_guest_stack_memblock.sh`. Device measurement pending.

## v0.1.1-alpha (2026-09-07)

Built from the source snapshot of 2026-09-07 and configured from
`v0.1.1-alpha.cmake` in this directory: every profiling and observer option
off, the native fast paths on, PNG large-image reuse off. VPK members are
identical to v0.1.0-alpha except `eboot.bin`.

Changed since v0.1.0-alpha:

- host-side replay of the sprite quad builder `Image::PushQuad` for every
  quad (`ISAAC_VITA_KAGE_QUAD_FASTPATH`): room render median down from
  11.7-13.1 ms to 9.9-10.3 ms, rooms with enemies from 48-52 to 55-59 FPS;
- the neutral ColorOffset fragment path
  (`ISAAC_VITA_COLOROFFSET_NEUTRAL_FASTPATH`) and omission of the sampled/ring
  laser shadow pass (`ISAAC_VITA_LASER_RING_SHADOW_SKIP`);
- a one-slot emergency allocation for the music stream queue
  (`ISAAC_VITA_OGG_QUEUE_EMERGENCY`), a lock-free read of the allocator's
  terminal state (`ISAAC_VITA_HEAP_TERMINAL_FASTPATH`), a render-skip observer
  ordering fix in the 60 Hz scheduler, the KAGE reference-count seam
  (`ISAAC_VITA_KAGE_REFCOUNT_SEAM`);
- exact native PNG row delivery batching and bounded row reuse, a strict PNG
  inflate fast path with the original fallback, NEON premultiply tables,
  faster room grid initialization;
- the vitaGL scissor and float-output selector fixes (stock-reference patches
  0028-0031);
- recovery of file handles the Vita kernel invalidates across an app suspend
  (`ISAAC_VITA_CRT_DESCRIPTOR_RECOVER`): pressing the PS button or letting
  the console lock during play used to stop the game with `ArchivedFile block
  header is invalid` on the next music-stream read; the handle is now
  reopened at the same position and the read redone.

Hardware sessions: about 2.5 minutes of play on the candidate without the
handle recovery and 4.5 minutes on the one before it (quad fast path for
batched quads only), including a save in a dense room; no fault record, every
native seam at fallbacks=0, picture correct. The release build itself: 4.9
minutes of a Utero run (Continue, four rooms, saves written), no fault,
every native seam at fallbacks=0; and at the main menu with music, PS
button to the home screen and back, both music-stream handles recovered,
no fault. Menus 60 FPS; rooms with enemies 55-59 FPS;
dense rooms 33-34 FPS; door transitions with resource loads 0.9-1.1 s;
Continue from the main menu about 7 s; process start to the menu about 4 s.

Known problems: long sessions may exhaust the
game heap (one crash on "Exit game" after 27 minutes); the on-device Save /
Mod Manager and the PC sync tool are untested on hardware; PS TV and other
languages are untested.

## v0.1.0-alpha (source snapshot of 2026-09-04)

The game boots, plays, saves and loads on a PS Vita at the native 960x544
with the External Item Descriptions mod loaded. Menus and ordinary rooms run
at 60 FPS with hitches; dense rooms at 35-37 FPS; rooms with large laser
effects at 30 FPS or worse; room entry stalls about one second; a long session
can exhaust the game heap.

## Distribution

The build consumes a user-supplied PC executable and resources. See
[../DISTRIBUTION.md](../DISTRIBUTION.md), [../THIRD_PARTY.md](../THIRD_PARTY.md)
and the release steps in [README.md](README.md) before publishing any package.
