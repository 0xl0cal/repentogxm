# Vita OpenAL overlay

`build.sh` builds OpenAL Soft 1.19.1 the way the VitaSDK package does: it
downloads the upstream `openal-soft-1.19.1` source and isage's Vita backend
patch, both pinned by SHA-256, without modifying `$VITASDK`. It then applies
the two project patches in this directory:

- `0001-limit-initial-voices.patch`: upstream reserves 256 mixer voices in
  `alcCreateContext` even when `[general] sources` is lower, which costs
  8.13 MiB on the Vita. The patch bounds the initial reservation by
  `device->SourcesMax`. The packaged `alsoft.conf` sets `sources = 80` (the
  game's 64 manager sources plus 16 for music streams, the jingle and the
  Theora video player), so the reservation drops to about 2.5 MiB. The game's
  own source and buffer counts are unchanged.
- `0002-route-direct-allocators.patch` with `openal_pool_redirect.h`: routes
  every direct `malloc`/`calloc`/`realloc`/`free`/`aligned_alloc`/`strdup` in
  the library to the project's OpenAL pool. It is controlled by
  `ISAAC_VITA_OPENAL_POOL` (default ON; `build_vita.py --openal-pool`), and
  the recipe fails if the archive still imports a raw allocator.

The main CMake build installs `alsoft.conf` as `app0:/alsoft.conf`.

From a VitaSDK shell:

```sh
export VITASDK=/opt/vitasdk-softfp
bash recomp/vita/openal-overlay/build.sh /tmp/isaac-openal-overlay
bash recomp/vita/openal-overlay/test.sh /tmp/isaac-openal-overlay
```

The main Vita CMake build performs the same build in its binary directory and
links the resulting project-local archive. It never overwrites the SDK copy.
The archive is built reproducibly (GNU binutils deterministic `ar`/`ranlib`
mode) and `verify_deterministic_archive.py` checks the result after every build
(53 regular members, zero member timestamps); see its `--help` for details.

The focused regression builds the overlay twice in different roots and
requires byte-identical archives:

```sh
python3 -B recomp/vita/openal-overlay/test_deterministic_archive.py \
  --vitasdk "$VITASDK"
```
