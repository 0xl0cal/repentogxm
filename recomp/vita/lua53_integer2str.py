#!/usr/bin/env python3
"""Derive the integer2str lobject.c for ISAAC_VITA_LUA_INTEGER2STR.

Pristine Lua 5.3.3 turns an integer into a string in exactly one place:
lobject.c:376 `luaO_tostring` calls `lua_integer2str(buff, sizeof(buff),
ivalue(obj))`, which luaconf.h:514 defines as `l_sprintf((s), sz,
LUA_INTEGER_FMT, (n))` and luaconf.h:591 as `snprintf(s,sz,f,i)`, with
LUA_INTEGER_FMT = "%lld" (luaconf.h:513/550, lua_Integer = long long on this
target).  luaO_tostring is reached from string coercion (lvm.c:455 cvt2str:
`..`, string arguments), lua_tolstring (lapi.c:381), and
lua_pushfstring's %d/%I (lobject.c:433, so tostring()/luaL_tolstring too).
On the console newlib's snprintf("%lld") is a full vfprintf; with EID it runs
~53 times per rendered frame (measured on the device).

The derivation changes two pristine lines and appends one function after the
last pristine line, so every pristine line keeps its number:

* lobject.c:366 (a blank line after MAXNUMBER2STR) becomes the prototype of
  isaac_lua_integer2str;
* lobject.c:376 calls isaac_lua_integer2str instead of lua_integer2str;
* after lobject.c:521 the definition: a digit loop whose output is the
  "%lld" text byte for byte (an optional '-', then the decimal digits of
  |n| with no leading zeros, "0" for zero, NUL-terminated, length returned).
  |n| is taken as an unsigned 64-bit value, so LLONG_MIN works; values of
  2^32 and above are split by 10^9 so the hot path (|n| < 2^32) only uses
  32-bit division.

No other formatting changes: io.write (liolib.c:621 fprintf) and
string.format (lstrlib.c:962 l_sprintf) keep the C library.  The proof is
recomp/test_vita_lua_integer2str.py (derives, compiles pristine Lua with the
derived file, and compares against snprintf over every input class).
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import sys

# lua53_vanilla.cmake: "lobject.c|d12ac0f8...".  test_vita_lua_integer2str.py
# checks that this copy still matches the manifest record.
PRISTINE_LOBJECT_SHA256 = (
    "d12ac0f80a51bc29e5d3ca86360bf7480fdddbddaa61cc7d2712aa56ec3b2dd9")
PRISTINE_LINES = 521

_PROTO_OLD = (b"#define MAXNUMBER2STR\t50\n"
              b"\n"
              b"\n"
              b"/*\n"
              b"** Convert a number object to a string\n")
_PROTO_NEW = (b"#define MAXNUMBER2STR\t50\n"
              b"LUAI_FUNC int isaac_lua_integer2str (char *buff, size_t sz, "
              b"lua_Integer n);  /* ISAAC_VITA_LUA_INTEGER2STR */\n"
              b"\n"
              b"/*\n"
              b"** Convert a number object to a string\n")
_CALL_OLD = b"    len = lua_integer2str(buff, sizeof(buff), ivalue(obj));\n"
_CALL_NEW = (b"    len = isaac_lua_integer2str(buff, sizeof(buff), ivalue(obj));"
             b"  /* ISAAC_VITA_LUA_INTEGER2STR (pristine: lua_integer2str = "
             b"snprintf LUA_INTEGER_FMT) */\n")
_TAIL_OLD = (b"    memcpy(out, POS, (LL(POS) + 1) * sizeof(char));\n"
             b"  }\n"
             b"}\n"
             b"\n")
_DEFINITION = b"""
/*
** ISAAC_VITA_LUA_INTEGER2STR: the text snprintf(buff, sz, LUA_INTEGER_FMT, n)
** writes ("%lld": optional '-', the decimal digits of |n| without leading
** zeros, '\\0'), and its return value (the length without the '\\0').  At
** most 20 characters + '\\0' (LLONG_MIN), so any sz >= 21 holds it; the one
** caller passes MAXNUMBER2STR (50).  |n| is unsigned arithmetic so LLONG_MIN
** needs no special case; above 2^32 the value is cut into 9-digit groups so
** each 64-bit division happens at most twice and the rest is 32-bit.
*/
int isaac_lua_integer2str (char *buff, size_t sz, lua_Integer n) {
  char digits[24];
  int count = 0;
  int len = 0;
  lua_Unsigned u = (n < 0) ? (lua_Unsigned)0 - l_castS2U(n) : l_castS2U(n);
  unsigned int w;
  (void)sz;
  while (u > 0xffffffffu) {
    lua_Unsigned q = u / 1000000000u;
    unsigned int group = (unsigned int)(u - q * 1000000000u);
    int k;
    for (k = 0; k < 9; k++) {
      digits[count++] = (char)('0' + (int)(group % 10u));
      group /= 10u;
    }
    u = q;
  }
  w = (unsigned int)u;
  do {
    digits[count++] = (char)('0' + (int)(w % 10u));
    w /= 10u;
  } while (w != 0u);
  if (n < 0)
    buff[len++] = '-';
  while (count > 0)
    buff[len++] = digits[--count];
  buff[len] = '\\0';
  return len;
}
"""


class DeriveError(Exception):
    pass


def derive(pristine: bytes) -> bytes:
    """Return the integer2str lobject.c (bytes in, bytes out)."""
    actual = hashlib.sha256(pristine).hexdigest()
    if actual != PRISTINE_LOBJECT_SHA256:
        raise DeriveError(f"input is not pristine Lua 5.3.3 lobject.c: {actual}")
    text = pristine
    for old, new in ((_PROTO_OLD, _PROTO_NEW), (_CALL_OLD, _CALL_NEW)):
        if text.count(old) != 1:
            raise DeriveError(f"anchor not found exactly once: {old!r}")
        text = text.replace(old, new)
    if text.count(b"\n") != pristine.count(b"\n"):
        raise DeriveError("derivation changed the pristine line count")
    if not text.endswith(_TAIL_OLD):
        raise DeriveError("pristine lobject.c does not end where expected")
    return text + _DEFINITION


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        derived = derive(args.input.read_bytes())
    except (OSError, DeriveError) as exc:
        print(f"lua53_integer2str: {exc}", file=sys.stderr)
        return 1
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # Rewrite when the bytes differ, and also when the output is older than
    # the script or the pristine input, so the build rule that lists those as
    # dependencies converges after one run.
    stale = not args.output.is_file()
    if not stale:
        newest = max(os.stat(__file__).st_mtime, args.input.stat().st_mtime)
        stale = (args.output.read_bytes() != derived or
                 args.output.stat().st_mtime < newest)
    if stale:
        args.output.write_bytes(derived)
    print(f"lua53_integer2str: sha256={hashlib.sha256(derived).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
