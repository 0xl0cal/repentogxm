#!/usr/bin/env python3
"""Configure-time check of the six trophy packs (ISAAC_VITA_TROPHY_PACK_DIR).

usage: trophy_pack_check.py --pack-dir DIR --comm-base ISAC00000 --sets 6

The comm id base must be 4 upper-case letters + 5 digits (the NPWR#####
shape): device run 2026-10-09 (FW 3.65, NoTrpDrm 1.1) showed SceShell
refusing ISAACR001_0N at the setup dialog (C0-11133-9) while ISAC00000_0N
installs.  It is not the title id (ISAACR001 stays the title id).

For each <DIR>/<BASE>_0N/TROPHY.TRP (N = 0 .. sets-1):
  * container (TRP v2, decoded from a reference pack and goo_vita's pack;
    see recomp/vita/TROPHIES.md): big-endian header
    '>IIQIII' = magic 0xDCA24D00, version 2, u64 file size, entry count,
    entry size 0x40, dev flag 0; SHA-1 at 0x1C..0x30 == sha1(file with those
    20 bytes zeroed); 0x30..0x3F zero; entries {32-byte NUL-padded name,
    u64 offset, u64 size, 16 zero bytes}, each block 16-aligned after the
    previous one, inside the file, names unique;
  * TROPCONF.SFM and TROP.SFM present;
  * TROPCONF.SFM: <npcommid> == the directory name; trophy count == the
    set's mapped achievements + 1 (90, 90, 99, 125, 118, 118) and <= 128;
    exactly one ttype="P", which is id 000 with pid -1.
Prints one OK line per set; exits 1 with the reason on the first failure.
The packs are game-derived data and never enter git; this script only
reads them.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import struct
import sys
import xml.etree.ElementTree as ET


TRP_MAGIC = 0xDCA24D00
TRP_VERSION = 2
TRP_HEADER_SIZE = 0x40
TRP_ENTRY_SIZE = 0x40
TRP_NAME_SIZE = 32
TRP_SHA1_OFFSET = 0x1C
TRP_SHA1_END = 0x30
MAX_TROPHIES = 128
# Mapped achievements per set + the platinum (set sizes
# 89, 89, 98, 124, 117, 117).
EXPECTED_TROPHIES = (90, 90, 99, 125, 118, 118)
COMM_BASE_SHAPE = re.compile(r"[A-Z]{4}[0-9]{5}")


class PackCheckError(Exception):
    pass


def align16(value: int) -> int:
    return (value + 15) & ~15


def parse_container(data: bytes, where: str) -> dict[str, bytes]:
    if len(data) < TRP_HEADER_SIZE:
        raise PackCheckError("%s: %d bytes, shorter than the header" %
                             (where, len(data)))
    magic, version, size_field, count, entry_size, dev_flag = \
        struct.unpack_from(">IIQIII", data, 0)
    if magic != TRP_MAGIC:
        raise PackCheckError("%s: magic 0x%08x" % (where, magic))
    if version != TRP_VERSION:
        raise PackCheckError("%s: version %d" % (where, version))
    if size_field != len(data):
        raise PackCheckError("%s: size field %d != file size %d" %
                             (where, size_field, len(data)))
    if entry_size != TRP_ENTRY_SIZE:
        raise PackCheckError("%s: entry size 0x%x" % (where, entry_size))
    if dev_flag != 0:
        raise PackCheckError("%s: dev flag %d" % (where, dev_flag))
    zeroed = bytearray(data)
    zeroed[TRP_SHA1_OFFSET:TRP_SHA1_END] = bytes(TRP_SHA1_END -
                                                 TRP_SHA1_OFFSET)
    if data[TRP_SHA1_OFFSET:TRP_SHA1_END] != hashlib.sha1(zeroed).digest():
        raise PackCheckError("%s: SHA-1 field mismatch" % where)
    if any(data[TRP_SHA1_END:TRP_HEADER_SIZE]):
        raise PackCheckError("%s: header bytes 0x30..0x3f not zero" % where)
    if count == 0:
        raise PackCheckError("%s: no entries" % where)
    table_end = TRP_HEADER_SIZE + count * TRP_ENTRY_SIZE
    if table_end > len(data):
        raise PackCheckError("%s: entry table runs past the file end" % where)
    files: dict[str, bytes] = {}
    previous_end = table_end
    for index in range(count):
        base = TRP_HEADER_SIZE + index * TRP_ENTRY_SIZE
        raw = data[base:base + TRP_NAME_SIZE]
        name_bytes, _, padding = raw.partition(b"\0")
        if not name_bytes or any(padding):
            raise PackCheckError("%s: entry %d name field %r" %
                                 (where, index, raw))
        try:
            name = name_bytes.decode("ascii")
        except UnicodeDecodeError:
            raise PackCheckError("%s: entry %d name is not ASCII" %
                                 (where, index)) from None
        offset, size = struct.unpack_from(">QQ", data, base + TRP_NAME_SIZE)
        if any(data[base + TRP_NAME_SIZE + 16:base + TRP_ENTRY_SIZE]):
            raise PackCheckError("%s: entry %s reserved bytes not zero" %
                                 (where, name))
        if offset % 16 != 0 or offset < previous_end:
            raise PackCheckError("%s: entry %s at 0x%x is not 16-aligned "
                                 "after the previous block (0x%x)" %
                                 (where, name, offset, previous_end))
        if offset + size > len(data):
            raise PackCheckError("%s: entry %s runs past the file end" %
                                 (where, name))
        if name in files:
            raise PackCheckError("%s: duplicate entry %s" % (where, name))
        files[name] = data[offset:offset + size]
        previous_end = offset + size
    return files


def check_tropconf(text: bytes, comm_id: str, expected: int,
                   where: str) -> int:
    try:
        root = ET.fromstring(text.decode("utf-8"))
    except (UnicodeDecodeError, ET.ParseError) as exc:
        raise PackCheckError("%s: TROPCONF.SFM does not parse: %s" %
                             (where, exc)) from None
    if root.tag != "trophyconf":
        raise PackCheckError("%s: TROPCONF.SFM root <%s>" % (where, root.tag))
    npcommid = root.findtext("npcommid")
    if npcommid != comm_id:
        raise PackCheckError("%s: npcommid %r != directory %s" %
                             (where, npcommid, comm_id))
    trophies = root.findall("trophy")
    if len(trophies) != expected or len(trophies) > MAX_TROPHIES:
        raise PackCheckError("%s: %d trophies, expected %d (max %d)" %
                             (where, len(trophies), expected, MAX_TROPHIES))
    platinum = [t for t in trophies if t.get("ttype") == "P"]
    if (len(platinum) != 1 or platinum[0].get("id") != "000" or
            platinum[0].get("pid") != "-1"):
        raise PackCheckError("%s: platinum must be exactly id 000 pid -1, got "
                             "%s" % (where, [(t.get("id"), t.get("pid"))
                                             for t in platinum]))
    return len(trophies)


def check_pack_dir(pack_dir: Path, comm_base: str, sets: int) -> list[str]:
    if sets < 1 or sets > len(EXPECTED_TROPHIES):
        raise PackCheckError("--sets %d outside 1..%d" %
                             (sets, len(EXPECTED_TROPHIES)))
    if COMM_BASE_SHAPE.fullmatch(comm_base) is None:
        raise PackCheckError("comm id base %r is not 4 letters + 5 digits "
                             "(SceShell refuses other shapes, C0-11133-9)" %
                             comm_base)
    lines = []
    for number in range(sets):
        comm_id = "%s_%02d" % (comm_base, number)
        path = pack_dir / comm_id / "TROPHY.TRP"
        if not path.is_file():
            raise PackCheckError("%s: missing" % path)
        data = path.read_bytes()
        files = parse_container(data, str(path))
        for required in ("TROPCONF.SFM", "TROP.SFM"):
            if required not in files:
                raise PackCheckError("%s: no %s" % (path, required))
        count = check_tropconf(files["TROPCONF.SFM"], comm_id,
                               EXPECTED_TROPHIES[number], str(path))
        lines.append("trophy pack OK: %s entries=%d trophies=%d bytes=%d "
                     "sha256=%s" % (comm_id, len(files), count, len(data),
                                    hashlib.sha256(data).hexdigest()))
    return lines


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pack-dir", type=Path, required=True)
    parser.add_argument("--comm-base", required=True)
    parser.add_argument("--sets", type=int, required=True)
    args = parser.parse_args(argv)
    try:
        for line in check_pack_dir(args.pack_dir, args.comm_base, args.sets):
            print(line)
    except PackCheckError as exc:
        print("trophy pack check FAILED: %s" % exc)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
