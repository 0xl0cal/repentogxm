# Contributing

This is an AI-assisted reverse-engineering project. AI-written, human-written
and mixed contributions are all acceptable. What matters is whether the change
is understandable, attributable, scoped to the actual goal and backed by the
mechanism it claims to preserve.

Provenance is recorded in [THIRD_PARTY.md](THIRD_PARTY.md) and checked by
`tools/release_audit.py`; discuss licensing before bringing in third-party code.

## Never commit

- game executables, resources, saves, Workshop archives or generated
  `guest_####.c` translation units;
- console keys, firmware, Sony modules, Vita3K profiles or proprietary SDK
  material;
- ELF, VELF, SELF, `eboot.bin`, VPK, SFO, static build outputs, crash dumps or
  local diagnostic captures;
- credentials or local absolute paths.

Do not bypass the ignore rules with a blind `git add -f` or `git add -A`.

## Code changes

1. Start from a clean branch and inspect `git status --short --ignored`.
2. Search the existing source, tests and upstream prior art before inventing
   a replacement.
3. Keep a frozen native fast path guarded and preserve the original translated
   fallback for every unproved input.
4. Run the focused oracle that actually exercises the changed mechanism.
5. If generated code is consumed, regenerate it first; stale generated files
   are not a regression test.
6. Run `python tools/build_vita.py --self-test` and the release audit before a
   release candidate, not as a substitute for focused development checks.
7. Record copied algorithms, upstream revisions and licences in
   [THIRD_PARTY.md](THIRD_PARTY.md).
8. Separate hardware measurements from static estimates and emulator results.

Performance patches must name the measured denominator: frames, game updates,
calls, bytes or wall time. Do not trade image quality or game speed for a larger
FPS number without making that mode explicit.

## Releases

Source lives on GitHub; the VPK of each release is attached to the Releases
page by the maintainer together with its SHA-256. The game's resources, saves
and keys are never published. Release notes are text only, under `release/`.

## Licence

Contributions are accepted under the project licence, GPL-2.0-or-later
([LICENSE](LICENSE)). Third-party code you bring in needs an entry in
[THIRD_PARTY.md](THIRD_PARTY.md).
