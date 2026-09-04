# Pinned vitaGL overlay

This archive is the fallback renderer: CMake links it only when
`ISAAC_VITA_VITAGL_STOCK_REFERENCE=OFF`. The perf builds recorded in
`../../STATUS.md` use the stock-reference profile in
[`../vitagl-stock-reference/`](../vitagl-stock-reference/README.md) instead.

The game's renderer build (`ISAAC_VITA_KAGE`) does not link the mutable
`libvitaGL.a` installed in VitaSDK. This recipe pins upstream vitaGL commit
`f4b23b61c84e8ba59de542832f8e507b3660f994`, verifies the source and local
patch hashes, and builds a private archive/header pair with:

```text
SOFTFP_ABI=1
NO_DEBUG=1
NO_SPLASHSCREEN=1
SINGLE_THREADED_GC=1
SHARED_RENDERTARGETS=1
```

`SINGLE_THREADED_GC=1` removes vitaGL's garbage-collector thread (this also
avoided a render-target race seen in the Vita3K emulator).
`SHARED_RENDERTARGETS=1` reuses small FBO render targets;
`SHARED_RENDERTARGETS=2` (aggressive recycling) is rejected by the build
contract.

The pinned commit is a descendant of `a136dd9ff73534a53bb45dcbeceb7c5ffafdb038`.
The recipe and test both require that commit's packed-VBO `first`/range pointer
adjustments; losing them regresses later `glDrawArrays`/`glMultiDrawArrays`
ranges even when the rest of the GL state is correct.

The recipe invokes only upstream's `libvitaGL.a` target and copies its output
to the requested directory.  It never invokes `make install` and refuses an
output directory inside `VITASDK`.

Build and run the static/link/configuration check on the Vita build host:

```sh
export VITASDK=/opt/vitasdk-softfp
bash recomp/vita/vitagl-overlay/build.sh /tmp/isaac-vitagl-overlay
bash recomp/vita/vitagl-overlay/test.sh /tmp/isaac-vitagl-overlay
```

The test does not execute the ARM ELF.  It verifies EABI5 softfp attributes,
the absence of GC thread/semaphore imports and the splash entry point, the
presence of shared-render-target symbols, every GL entry consumed by the
production adapter (also compared with the installed SDK baseline when
available), the `a136dd9` source fix, a complete GLSL/FBO link probe, and
CMake's selection of the exact private archive and matching header.
