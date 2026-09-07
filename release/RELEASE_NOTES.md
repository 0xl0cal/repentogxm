# repentogxm -- release notes

The VPK of each release is attached to the GitHub Releases page together with
its SHA-256 and the eboot SHA-256; the game's resources are not included and
come from your own PC copy. What runs on real hardware, how fast, and what is
broken is recorded in [../STATUS.md](../STATUS.md); the priorities are in
[../PLAN.md](../PLAN.md).

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
