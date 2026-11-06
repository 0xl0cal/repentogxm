#!/usr/bin/env python3
"""Synthetic-pack tests of recomp/vita/trophy_pack_check.py.

Every pack is built here from our own placeholder text (no game data): six
good packs pass with one OK line each through the CLI, and each container
or TROPCONF rule, broken once, fails with its own reason.
"""

from __future__ import annotations

import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import trophy_pack_check as C  # noqa: E402

BASE = "ISAC00000"
SIGNATURE = "<!--Sce-Np-Trophy-Signature: " + "x" * 320 + "-->\n"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def tropconf(comm_id: str, count: int, platinum=(("000", "-1"),),
             extra_ttype: str = "B") -> bytes:
    lines = [SIGNATURE.rstrip("\n"),
             '<trophyconf version="1.1" platform="psp2" policy="large">',
             " <npcommid>%s</npcommid>" % comm_id,
             " <trophyset-version>01.00</trophyset-version>",
             ' <parental-level license-area="default">0</parental-level>']
    used = 0
    for number, pid in platinum:
        lines.append(' <trophy id="%s" hidden="no" ttype="P" pid="%s"/>' %
                     (number, pid))
        used += 1
    for number in range(used, count):
        lines.append(' <trophy id="%03d" hidden="no" ttype="%s" pid="000"/>' %
                     (number, extra_ttype))
    lines.append("</trophyconf>")
    return ("\n".join(lines) + "\n").encode("utf-8")


def container(files: list[tuple[str, bytes]], align: bool = True) -> bytearray:
    count = len(files)
    cursor = C.TRP_HEADER_SIZE + count * C.TRP_ENTRY_SIZE
    layout = []
    for name, blob in files:
        offset = C.align16(cursor) if align else cursor
        layout.append((name.encode("ascii"), offset, blob))
        cursor = offset + len(blob)
    out = bytearray(C.align16(cursor))
    struct.pack_into(">IIQIII", out, 0, C.TRP_MAGIC, C.TRP_VERSION, len(out),
                     count, C.TRP_ENTRY_SIZE, 0)
    for index, (raw, offset, blob) in enumerate(layout):
        base = C.TRP_HEADER_SIZE + index * C.TRP_ENTRY_SIZE
        out[base:base + C.TRP_NAME_SIZE] = raw.ljust(C.TRP_NAME_SIZE, b"\0")
        struct.pack_into(">QQ", out, base + C.TRP_NAME_SIZE, offset, len(blob))
        out[offset:offset + len(blob)] = blob
    seal(out)
    return out


def seal(out: bytearray) -> None:
    out[C.TRP_SHA1_OFFSET:C.TRP_SHA1_END] = bytes(20)
    out[C.TRP_SHA1_OFFSET:C.TRP_SHA1_END] = hashlib.sha1(bytes(out)).digest()


def good_files(number: int, **conf) -> list[tuple[str, bytes]]:
    comm_id = "%s_%02d" % (BASE, number)
    count = conf.pop("count", C.EXPECTED_TROPHIES[number])
    return [("TROPCONF.SFM", tropconf(comm_id, count, **conf)),
            ("TROP.SFM", b"<trophyconf/>\n"),
            ("ICON0.PNG", b"\x89PNG placeholder"),
            ("TROP000.PNG", b"\x89PNG placeholder 0")]


def write_packs(root: Path, override=None) -> None:
    for number in range(6):
        directory = root / ("%s_%02d" % (BASE, number))
        directory.mkdir(parents=True, exist_ok=True)
        data = container(good_files(number))
        if override is not None and override[0] == number:
            data = override[1]
        (directory / "TROPHY.TRP").write_bytes(bytes(data))


def expect_failure(root: Path, needle: str, override) -> None:
    write_packs(root, override)
    try:
        C.check_pack_dir(root, BASE, 6)
    except C.PackCheckError as exc:
        require(needle in str(exc), "expected %r, got %r" % (needle, str(exc)))
        return
    raise AssertionError("accepted a broken pack (%s)" % needle)


def mutated(number: int, edit) -> bytearray:
    data = container(good_files(number))
    edit(data)
    return data


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="isaac-trophy-pack-") as value:
        root = Path(value) / "packs"
        write_packs(root)
        out = subprocess.run([sys.executable, str(HERE / "trophy_pack_check.py"),
                              "--pack-dir", str(root), "--comm-base", BASE,
                              "--sets", "6"], capture_output=True, text=True)
        lines = out.stdout.splitlines()
        require(out.returncode == 0 and len(lines) == 6 and all(
            line.startswith("trophy pack OK: %s_%02d entries=4 trophies=%d " %
                            (BASE, n, C.EXPECTED_TROPHIES[n]))
            for n, line in enumerate(lines)), "good packs: %r %r" %
            (out.stdout, out.stderr))

        def header(offset, fmt, value):
            def edit(data):
                struct.pack_into(fmt, data, offset, value)
                seal(data)
            return edit

        def entry(index, delta_offset=0, delta_size=0, name=None):
            def edit(data):
                base = C.TRP_HEADER_SIZE + index * C.TRP_ENTRY_SIZE
                offset, size = struct.unpack_from(">QQ", data,
                                                  base + C.TRP_NAME_SIZE)
                struct.pack_into(">QQ", data, base + C.TRP_NAME_SIZE,
                                 offset + delta_offset, size + delta_size)
                if name is not None:
                    data[base:base + C.TRP_NAME_SIZE] = name.ljust(
                        C.TRP_NAME_SIZE, b"\0")
                seal(data)
            return edit

        def raw(offset, value):
            def edit(data):
                data[offset] = value
                seal(data)
            return edit

        cases = [
            ("magic 0x", 0, header(0, ">I", 0xDCA24D01)),
            ("version 3", 1, header(4, ">I", 3)),
            ("size field", 2, header(8, ">Q", 12345)),
            ("entry size 0x41", 3, header(0x14, ">I", 0x41)),
            ("dev flag 1", 4, header(0x18, ">I", 1)),
            ("SHA-1 field mismatch", 5,
             lambda d: d.__setitem__(C.TRP_SHA1_OFFSET, d[C.TRP_SHA1_OFFSET] ^ 1)),
            ("header bytes 0x30..0x3f", 0, raw(0x30, 1)),
            ("not 16-aligned", 1, entry(1, delta_offset=4)),
            ("runs past the file end", 2, entry(3, delta_size=0x10000)),
            ("duplicate entry TROPCONF.SFM", 3,
             entry(1, name=b"TROPCONF.SFM")),
            ("reserved bytes not zero", 4,
             raw(C.TRP_HEADER_SIZE + C.TRP_NAME_SIZE + 16, 1)),
            ("no entries", 5, header(0x10, ">I", 0)),
        ]
        for needle, number, edit in cases:
            expect_failure(root, needle, (number, mutated(number, edit)))
        conf_cases = [
            ("no TROP.SFM", 0,
             container([f for f in good_files(0) if f[0] != "TROP.SFM"])),
            ("npcommid 'ISAC00000_01' != directory ISAC00000_02", 2,
             container([("TROPCONF.SFM", tropconf("ISAC00000_01", 99)),
                        ("TROP.SFM", b"x")])),
            ("89 trophies, expected 90", 1, container(good_files(1, count=89))),
            ("platinum must be exactly id 000 pid -1", 3,
             container(good_files(3, platinum=(("000", "-1"), ("001", "-1"))))),
            ("platinum must be exactly id 000 pid -1", 4,
             container(good_files(4, platinum=(("000", "000"),)))),
            ("platinum must be exactly id 000 pid -1", 5,
             container(good_files(5, platinum=()))),
            ("does not parse", 0,
             container([("TROPCONF.SFM", b"<trophyconf>"),
                        ("TROP.SFM", b"x")])),
        ]
        for needle, number, data in conf_cases:
            expect_failure(root, needle, (number, data))
        write_packs(root)
        (root / "ISAC00000_04" / "TROPHY.TRP").unlink()
        try:
            C.check_pack_dir(root, BASE, 6)
        except C.PackCheckError as exc:
            require("ISAC00000_04" in str(exc) and "missing" in str(exc),
                    str(exc))
        else:
            raise AssertionError("missing pack accepted")
        out = subprocess.run([sys.executable, str(HERE / "trophy_pack_check.py"),
                              "--pack-dir", str(root), "--comm-base", BASE,
                              "--sets", "6"], capture_output=True, text=True)
        require(out.returncode == 1 and "trophy pack check FAILED" in
                out.stdout, "CLI failure exit: %r" % out.stdout)
        require(C.MAX_TROPHIES == 128 and
                C.EXPECTED_TROPHIES == (90, 90, 99, 125, 118, 118),
                "pinned counts changed")
        # Comm id base shape (device run 2026-10-09: ISAACR001_0N refused,
        # ISAC00000_0N accepted).  Packs named after a refused base fail on
        # the base, before any file is opened.
        bad_bases = ("ISAACR001", "isac00000", "ISAC0000", "ISAC000000",
                     "ISA000000", "ISAC0000A")
        for bad in bad_bases:
            legacy = Path(value) / ("packs-%s" % bad)
            for number in range(6):
                directory = legacy / ("%s_%02d" % (bad, number))
                directory.mkdir(parents=True, exist_ok=True)
                (directory / "TROPHY.TRP").write_bytes(bytes(
                    container(good_files(number))))
            try:
                C.check_pack_dir(legacy, bad, 6)
            except C.PackCheckError as exc:
                require("is not 4 letters + 5 digits" in str(exc), str(exc))
            else:
                raise AssertionError("comm base %r accepted" % bad)
        write_packs(root)
        (root / "ISAC00000_02").rename(root / "ISAACR001_02")
        try:
            C.check_pack_dir(root, BASE, 6)
        except C.PackCheckError as exc:
            require("ISAC00000_02" in str(exc) and "missing" in str(exc),
                    str(exc))
        else:
            raise AssertionError("a title-id directory name was accepted")
        total = len(cases) + len(conf_cases) + 1 + len(bad_bases) + 1
    print("trophy pack check: PASS (6 synthetic packs OK via the CLI; %d "
          "broken packs rejected with their reason)" % total)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
