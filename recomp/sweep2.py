"""Whole-binary sweep, with boundary filtering and real flag pairing.

Two defects in `sweep.py` are fixed here, and both were mine:

1. **It called `emit(ins, True, None)`** -- always-live flags, never paired --
   so it reported `conditions answered direct: 0` and 307,462 flag stores.
   That contradicted the six-function measurement, where 53 of 56 conditions
   needed no flag context at all. It was a statement about the sweep's loop,
   not about the binary. This driver runs the same producer/consumer pairing
   `build_one.py` uses, so the numbers mean what their names say.

2. **It counted data as untranslated code.** `bounds2.py` established that
   candidates whose recursive descent immediately decodes an instruction MSVC
   cannot emit (`aas`, `aam`, `das`, `into`, port IO) are orphans -- nothing
   calls them, nothing points at them, and they exist only because the bytes
   before them happened to be `CC` padding. They are jump tables and constant
   pools. Counting them as emitter work overstates what is left.

   The discriminator is sharp: of the candidates flagged, **every** direct call
   target has its impossible instruction deep inside the body (median offset
   1,040 bytes) and never at the start, while the orphans have it at offset 0.
   Real functions with embedded tables versus tables mistaken for functions.

Both filtered and unfiltered figures are printed, because a filter that
improves a number is exactly the kind of thing that should be shown alongside
what it removed.
"""
import collections
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import build_one as B                                        # noqa: E402
import emit as E                                             # noqa: E402
import trans as T                                            # noqa: E402
from image import Image                                      # noqa: E402

_DEFAULT_WORKDIR = os.path.abspath(os.environ.get(
    "REPENTOGXM_WORKDIR",
    os.path.join(os.path.dirname(__file__), "..", "build", "recompiler"),
))
EXE = os.path.abspath(os.environ.get(
    "REPENTOGXM_PE",
    os.path.join(_DEFAULT_WORKDIR, "isaac-ng.exe.unpacked.exe"),
))
FUNCS = os.path.join(_DEFAULT_WORKDIR, "functions.json")
BOUNDS = os.path.join(_DEFAULT_WORKDIR, "bounds.json")
OUT = os.path.join(_DEFAULT_WORKDIR, "sweep2.json")

IMPOSSIBLE = {
    "aaa", "aas", "aam", "aad", "daa", "das",
    "into", "bound", "salc", "xlatb", "xlat",
    "lds", "les", "arpl", "sysenter", "sysexit", "hlt",
    "in", "out", "insb", "insd", "outsb", "outsd",
    "lgdt", "lidt", "lldt", "ltr", "clts", "invd",
    "loopne", "loope", "iret", "iretd", "retf", "sysret",
}


def classify(img, fn, called, ptrs, jumped):
    """Is this candidate real code, or data that padding invented?

    Judged on the bytes the *translator* would actually decode, following
    control flow -- not on a linear walk of the extent, which marches into
    embedded jump tables and produced the contradiction that exposed the
    first probe.
    """
    rva = fn["rva"]
    if rva in called or rva in ptrs or rva in jumped:
        return "code", None
    try:
        insns, order, _ = T.decode(img, rva)
    except Exception:                                       # noqa: BLE001
        return "data", "undecodable orphan"
    if not order:
        return "data", "empty orphan"
    # An orphan whose own control flow decodes something no compiler emits is
    # not a function. Look only near the entry: deep hits are embedded tables
    # in code that merely happens to have no caller we found.
    for r in order[:40]:
        if insns[r].mnemonic in IMPOSSIBLE:
            return "data", "orphan decoding %s" % insns[r].mnemonic
    return "code", None


def main():
    img = Image(EXE)
    with open(FUNCS, encoding="utf-8") as f:
        funcs = [x for x in json.load(f)["functions"] if x["size"] > 0]
    with open(BOUNDS, encoding="utf-8") as f:
        bd = json.load(f)
    called = set(bd["called"])
    ptrs = set(bd["ptrs"])
    jumped = set(bd["jumped"])

    print("candidates from functions.json : %d" % len(funcs))

    # ---- boundary pass ----------------------------------------------------
    t0 = time.time()
    real, junk = [], []
    why = collections.Counter()
    for fn in funcs:
        kind, reason = classify(img, fn, called, ptrs, jumped)
        if kind == "code":
            real.append(fn)
        else:
            junk.append(fn)
            why[reason.split()[0] + " " + reason.split()[-1]] += 1
    junk_bytes = sum(f["size"] for f in junk)
    print("dropped as data, not code     : %d  (%d B, %.1f%% of .text)"
          % (len(junk), junk_bytes,
             100.0 * junk_bytes / sum(f["size"] for f in funcs)))
    for r, n in why.most_common(6):
        print("      %5d  %s" % (n, r))
    print("real functions                : %d   (boundary pass %.0f s)"
          % (len(real), time.time() - t0))

    # ---- translate --------------------------------------------------------
    ok = failed = crashed = 0
    ok_bytes = fail_bytes = 0
    insn_total = flag_stores = direct_pairs = unsafe = 0
    reasons = collections.Counter()
    mnemonics = collections.Counter()
    worst = []
    failing = []

    t0 = time.time()
    for i, fn in enumerate(real):
        if i and i % 3000 == 0:
            print("   ... %d/%d  (%.0f s)" % (i, len(real), time.time() - t0))
        rva = fn["rva"]
        try:
            em, insns, order, body, unsup, indirect, members = \
                B.translate(img, rva, "sub_%08x" % rva)
        except T.Unsupported as ex:
            crashed += 1
            fail_bytes += fn["size"]
            reasons["decode: " + ex.why.split()[0]] += 1
            continue
        except Exception as ex:                             # noqa: BLE001
            crashed += 1
            fail_bytes += fn["size"]
            reasons["decode-exception: %s" % type(ex).__name__] += 1
            continue

        insn_total += len(order)
        flag_stores += em.flag_stores
        direct_pairs += em.direct_pairs
        unsafe += getattr(em, "unsafe_pairs", 0)
        for ex in unsup:
            mnemonics[ex.ins.mnemonic if hasattr(ex, "ins") else "?"] += 1
        if not unsup:
            ok += 1
            ok_bytes += fn["size"]
        else:
            failed += 1
            fail_bytes += fn["size"]
            first = unsup[0]
            m = first.ins.mnemonic if hasattr(first, "ins") else "?"
            reasons["%s: %s" % (m, first.why)] += 1
            failing.append({"rva": rva, "size": fn["size"], "insn": m})
            if fn["size"] > 4096:
                worst.append((fn["size"], rva, m))

    dt = time.time() - t0
    total = ok_bytes + fail_bytes
    print("\n=== sweep complete in %.0f s ===" % dt)
    print("functions clean, by count     : %d of %d  (%.1f%%)"
          % (ok, len(real), 100.0 * ok / max(1, len(real))))
    print("functions clean, BY BYTES     : %.2f%%  (%d of %d)"
          % (100.0 * ok_bytes / max(1, total), ok_bytes, total))
    print("blocked by an unknown insn    : %d" % failed)
    print("failed to decode              : %d" % crashed)
    print("instructions translated       : %d" % insn_total)
    print("\nflag handling, measured with real pairing:")
    print("   flag-context stores        : %d" % flag_stores)
    print("   conditions answered direct : %d" % direct_pairs)
    if flag_stores + direct_pairs:
        print("   -> %.1f%% of conditions need no flag context at all"
              % (100.0 * direct_pairs / (flag_stores + direct_pairs)))
    print("   pairs refused as unsafe    : %d" % unsafe)

    print("\ntop first-failure reasons (by functions blocked):")
    for r, n in reasons.most_common(20):
        print("   %5d  %s" % (n, r))

    if worst:
        print("\nlargest blocked functions:")
        for sz, rva, m in sorted(worst, reverse=True)[:12]:
            print("   %08x  %8d B  %s" % (rva, sz, m))
        blocked_big = sum(s for s, _, _ in worst)
        print("   those %d alone are %d B = %.1f%% of .text"
              % (len(worst), blocked_big, 100.0 * blocked_big / max(1, total)))

    with open(OUT, "w", encoding="utf-8") as f:
        json.dump({"real": len(real), "junk": len(junk),
                   "junk_bytes": junk_bytes,
                   "ok": ok, "failed": failed, "crashed": crashed,
                   "ok_bytes": ok_bytes, "total_bytes": total,
                   "flag_stores": flag_stores, "direct_pairs": direct_pairs,
                   "reasons": reasons.most_common(),
                   "failing": failing}, f)
    print("\nwritten: %s" % OUT)


if __name__ == "__main__":
    sys.exit(main())
