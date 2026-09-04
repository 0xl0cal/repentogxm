# Release checklist

A VPK is not a release merely because it boots. Work through this list before
attaching one to the GitHub Releases page.

## 1. Source and legal preflight

- [ ] [THIRD_PARTY.md](THIRD_PARTY.md) and [NOTICE.md](NOTICE.md) list every
  bundled dependency and asset; the project licence is GPL-2.0-or-later
  ([LICENSE](LICENSE)).
- [ ] Confirm the repository contains no game PE, resources, saves, keys, Sony
  runtime/module, Steam Workshop archive, eboot, VPK, or local crash dump.
- [ ] Run `python tools/release_audit.py --strict --include-untracked` from the
  repository root.
- [ ] Review every untracked and ignored path before staging; never use a blind
  `git add -A` in the development workspace.
- [ ] Confirm the build and install process requires users to supply their own
  legally obtained game data.

## 2. Reproducible source tree

- [ ] Classify the current dirty tree into source, generated interfaces,
  evidence, local-only tooling, and disposable output.
- [ ] Keep generated guest translation units and proprietary inputs out of Git.
- [ ] Replace developer-machine paths with parameters or clearly labelled
  examples.
- [ ] Provide one documented build entry point that fails immediately on a
  generation, compile, link, SELF, VPK, or hash error.
- [ ] Pin the translator recipe, generated output-set hash, supported PE hash,
  VitaSDK/toolchain identity, and required library revisions.
- [ ] Regenerate into a fresh directory and prove a second build is byte-exact
  or document every nondeterministic member.
- [ ] Make coherent local commits.

## 3. Static and component gates

- [ ] Run all host behavior/ABI oracles from clean outputs.
- [ ] Run all ARMv7 Thumb-2 softfp compile/link/readelf gates with warnings as
  errors and no unexpected undefined symbols.
- [ ] Regenerate test consumers before trusting tests of generated code.
- [ ] Verify the complete import/manual/GL/XInput/audio coverage manifests and
  fail-closed unknown paths.
- [ ] Run repository diff checks and the strict release audit.

## 4. Vita3K acceptance

- [ ] Install by exact artifact hash and record the loaded eboot identity.
- [ ] Show the loading display immediately, finish cleanly, and leave no border
  or callback residue on the first real frame.
- [ ] Complete a 30-minute multi-room soak with pause/resume, death/restart,
  save/continue, language change, and controlled shutdown.
- [ ] Attribute every unsupported import, guest stop, frozen-present watchdog,
  and emulator exception; silence is not success.
- [ ] Verify the retail Vita control layout, analog ranges, menu context, Drop
  hold behavior, and front-touch zones.
- [ ] Verify music, effects, pause/resume, shutdown, underrun recovery, latency,
  and audio/video drift.
- [ ] Preserve save backups and a PID-bound diagnostic package for every failed
  run.

## 5. Real-hardware alpha

- [ ] Test the same hashed package on a real Vita after prerequisites are
  installed according to [INSTALL.md](INSTALL.md).
- [ ] Measure boot/loading wall time, frame-time distribution, CPU cost, native
  and guest heap high-water, and fixed-image headroom.
- [ ] Verify controls, touch, audio, suspend/resume, PS-button lifecycle, safe
  quit, save persistence, and at least a 30-minute multi-room run.
- [ ] Confirm the loading callback/cache behavior on hardware; Vita3K cannot
  prove scanout and cache coherency.
- [ ] Test update, rollback, save restore, and uninstall instructions from a
  clean user perspective.

## 6. Package audit

- [ ] Verify the VPK has only the expected safe members and CRC-clean contents.
- [ ] Confirm `eboot.bin` is byte-identical to the audited sidecar and is ARM
  EABI5 softfp/NOASLR with the intended heap and stack values.
- [ ] Confirm the embedded PE exactly matches the supported input hash and that
  no resources or saves are packaged.
- [ ] Record SHA-256 and sizes for source revision, recipe, manifest/output set,
  ELF, VELF, eboot, VPK, tests, and runtime prerequisites.
- [ ] Build an installable package only from the clean audited tree, not from a
  dirty development directory.

## 7. Publication gate

- [ ] README status and limitations match the accepted artifact rather than an
  older successful run.
- [ ] Installation, troubleshooting, recovery, attribution, and licence links
  render correctly from a fresh clone.
- [ ] A second person follows the documented build/install/update/recovery flow.
- [ ] Attach the VPK and its SHA-256 to the GitHub Release. Never attach game
  resources, saves or keys.
