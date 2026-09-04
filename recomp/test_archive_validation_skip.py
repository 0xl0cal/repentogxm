#!/usr/bin/env python3
"""Frozen-PE, corpus, and generated-delta gate for the Vita archive skip."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath


HERE = Path(__file__).resolve().parent
PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
RECEIPT_CORPUS_SHA256 = (
    "8eaf383602b9e58ad42589f1436a911bae6feb1845b42fb6e33398383490f0f6"
)
DEFAULT_IMAGE_BASE = 0x30000000
VITA_IMAGE_BASE = 0x98000000
ROOT_RVA = 0x00563880
ROOT_END_RVA = 0x00563D74
SITE_RVA = 0x00563B59
RESUME_RVA = 0x00563C8C
DESCRIPTOR_TABLE_RVA = 0x007AAB40
ARCHIVE_HEADER = struct.Struct("<7sBIH")
ARCHIVE_ENTRY = struct.Struct("<IIIII")
FEATURE_GUARD = (
    "#if defined(__vita__) && "
    "defined(ISAAC_VITA_ARCHIVE_VALIDATION_SKIP)"
)
EXPECTED_CORPUS = {
    "archives": 18,
    "entries": 21_283,
    "data_bytes": 1_034_148_878,
    "skipped_entries": 18_635,
    "skipped_data_bytes": 888_222_052,
    "original_validation_entries": 2_648,
}
RAW_WINDOWS = (
    (ROOT_RVA, ROOT_END_RVA,
     "9ebb2c46ff8f11ac03fd54c19d8060c6be26079462240509abf07c300a0e3c61"),
    (0x00563B30, 0x00563B5E,
     "6c2acdecc2be3c891c3c9b3ebc3c6ab6b89f3e8fb00f51d6da3d42f8d3308c98"),
    (SITE_RVA, RESUME_RVA,
     "3a1ccc945662bbd62fd1a868cd2f83d9ae3735664df2223b4cb123aa4062b188"),
    (0x00563C7A, 0x00563C99,
     "109a70ad2c8a665486012367fe0d81aaedddad47227441464ca8208302981d11"),
)
PRECALL_HEX = (
    "8b8db0f1ffff51c7857cf1ffff000000008b790488411881e7ff7f00008d8d"
    "bcf1ffff89bd80f1ffffe842840300"
)
RESUME_HEX = (
    "8d8dbcf1ffffc745fcffffffffe8248403008b04bd687cbc0085c074718b95"
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


@dataclass(frozen=True)
class Section:
    rva: int
    span: int
    raw_offset: int


class PeImage:
    """Small independent PE32 reader for the release gate."""

    def __init__(self, path: Path) -> None:
        self.data = path.read_bytes()
        if len(self.data) != PE_SIZE or hashlib.sha256(self.data).hexdigest() != PE_SHA256:
            raise AssertionError("archive-skip test received a different PE")
        if self.data[:2] != b"MZ":
            raise AssertionError("frozen input has no MZ header")
        pe_offset = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[pe_offset:pe_offset + 4] != b"PE\0\0":
            raise AssertionError("frozen input has no PE signature")
        section_count = struct.unpack_from("<H", self.data, pe_offset + 6)[0]
        optional_size = struct.unpack_from("<H", self.data, pe_offset + 20)[0]
        optional = pe_offset + 24
        if struct.unpack_from("<H", self.data, optional)[0] != 0x10B:
            raise AssertionError("frozen input is not PE32")
        self.image_base = struct.unpack_from("<I", self.data, optional + 28)[0]
        section_table = optional + optional_size
        sections = []
        for index in range(section_count):
            offset = section_table + index * 40
            virtual_size, rva, raw_size, raw_offset = struct.unpack_from(
                "<IIII", self.data, offset + 8
            )
            sections.append(Section(rva, max(virtual_size, raw_size), raw_offset))
        self.sections = tuple(sections)

    def bytes_at(self, rva: int, size: int) -> bytes:
        for section in self.sections:
            if section.rva <= rva < section.rva + section.span:
                offset = section.raw_offset + rva - section.rva
                result = self.data[offset:offset + size]
                if len(result) == size:
                    return result
                break
        raise AssertionError("short/unmapped PE read at RVA 0x%08x" % rva)

    def bytes_at_va(self, va: int, size: int) -> bytes:
        return self.bytes_at(va - self.image_base, size)

    def cstring_va(self, va: int) -> str:
        rva = va - self.image_base
        for size in range(1, 1025):
            data = self.bytes_at(rva, size)
            if data[-1] == 0:
                return data[:-1].decode("ascii")
        raise AssertionError("unterminated PE string at VA 0x%08x" % va)


@dataclass(frozen=True)
class Descriptor:
    name: str
    expected_checksums: tuple[int, ...]


def load_descriptors(image: PeImage) -> tuple[Descriptor, ...]:
    result = []
    for index in range(32):
        descriptor_va = struct.unpack(
            "<I", image.bytes_at(DESCRIPTOR_TABLE_RVA + index * 4, 4)
        )[0]
        if descriptor_va == 0:
            break
        name_va, expected_va, expected_count = struct.unpack(
            "<III", image.bytes_at_va(descriptor_va, 12)
        )
        source_name = image.cstring_va(name_va)
        expected = struct.unpack(
            "<%dI" % expected_count,
            image.bytes_at_va(expected_va, expected_count * 4),
        ) if expected_count else ()
        result.append(Descriptor(PurePosixPath(source_name).name, tuple(expected)))
    if len(result) != EXPECTED_CORPUS["archives"]:
        raise AssertionError("frozen archive descriptor census changed")
    return tuple(result)


def verify_machine_code(image: PeImage) -> None:
    for start, end, expected in RAW_WINDOWS:
        actual = hashlib.sha256(image.bytes_at(start, end - start)).hexdigest()
        if actual != expected:
            raise AssertionError("machine-code window drift at 0x%08x" % start)
    if image.bytes_at(0x00563B30, 0x2E).hex() != PRECALL_HEX:
        raise AssertionError("archive registration/pre-call seam changed")
    if image.bytes_at(0x00563C7A, 0x1F).hex() != RESUME_HEX:
        raise AssertionError("archive hash-insertion resume seam changed")
    call = image.bytes_at(SITE_RVA, 5)
    target = (SITE_RVA + 5 + struct.unpack_from("<i", call, 1)[0]) & 0xFFFFFFFF
    if call[0] != 0xE8 or target != 0x0059BFA0:
        raise AssertionError("temporary ArchivedFile constructor edge changed")
    if image.bytes_at(0x0059C07A, 3) != b"\xc2\x04\x00":
        raise AssertionError("temporary ArchivedFile constructor no longer ret 4")


def verify_manifest_shape(manifest_path: Path, image: PeImage) -> tuple[dict, tuple[Descriptor, ...]]:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    canonical = json.dumps(
        manifest, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("utf-8")
    if hashlib.sha256(canonical).hexdigest() != RECEIPT_CORPUS_SHA256:
        raise AssertionError("archive receipt corpus identity changed")
    if manifest.get("schema") != "isaac-vita-archive-validation-corpus-v1":
        raise AssertionError("archive corpus schema changed")
    if manifest.get("pe_sha256") != PE_SHA256:
        raise AssertionError("archive corpus is not tied to the frozen PE")
    if manifest.get("descriptor_table_rva") != DESCRIPTOR_TABLE_RVA:
        raise AssertionError("archive descriptor-table receipt changed")
    if manifest.get("expected") != EXPECTED_CORPUS:
        raise AssertionError("archive skip aggregate receipt changed")
    rows = manifest.get("archives")
    descriptors = load_descriptors(image)
    if not isinstance(rows, list) or len(rows) != len(descriptors):
        raise AssertionError("archive corpus row census changed")
    if tuple(row.get("name") for row in rows) != tuple(item.name for item in descriptors):
        raise AssertionError("archive corpus order differs from the PE descriptor order")
    entries = sum(int(row["entries"]) for row in rows)
    data_bytes = sum(int(row["index_offset"]) - ARCHIVE_HEADER.size for row in rows)
    if entries != EXPECTED_CORPUS["entries"] or data_bytes != EXPECTED_CORPUS["data_bytes"]:
        raise AssertionError("archive corpus row aggregates changed")
    return manifest, descriptors


def verify_archive_corpus(manifest: dict, descriptors: tuple[Descriptor, ...], packed: Path) -> None:
    matched_entries = 0
    matched_data_bytes = 0
    total_entries = 0
    total_data_bytes = 0
    for row, descriptor in zip(manifest["archives"], descriptors):
        path = packed / descriptor.name
        if not path.is_file():
            raise AssertionError("missing archive from frozen descriptor set: %s" % path)
        if path.stat().st_size != row["size"]:
            raise AssertionError("archive size drift: %s" % path.name)
        with path.open("rb") as stream:
            magic, mode, index_offset, entry_count = ARCHIVE_HEADER.unpack(
                stream.read(ARCHIVE_HEADER.size)
            )
            if magic != b"ARCH000" or mode != row["mode"]:
                raise AssertionError("archive header drift: %s" % path.name)
            index_size = entry_count * ARCHIVE_ENTRY.size
            if index_offset != row["index_offset"] or entry_count != row["entries"]:
                raise AssertionError("archive index header drift: %s" % path.name)
            if index_offset + index_size != row["size"]:
                raise AssertionError("archive index no longer closes at EOF: %s" % path.name)
            stream.seek(index_offset)
            index = stream.read(index_size)
        if len(index) != index_size:
            raise AssertionError("short archive index: %s" % path.name)
        if hashlib.sha256(index).hexdigest() != row["index_sha256"]:
            raise AssertionError("archive index hash drift: %s" % path.name)
        if sha256_file(path) != row["archive_sha256"]:
            raise AssertionError("archive full hash drift: %s" % path.name)
        entries = [
            ARCHIVE_ENTRY.unpack_from(index, offset)
            for offset in range(0, len(index), ARCHIVE_ENTRY.size)
        ]
        offsets = tuple(entry[2] for entry in entries)
        if not offsets or offsets[0] != ARCHIVE_HEADER.size or any(
                left >= right for left, right in zip(offsets, offsets[1:])):
            raise AssertionError("archive entry offsets drift: %s" % path.name)
        for index_number, entry in enumerate(entries):
            end = offsets[index_number + 1] if index_number + 1 < len(entries) else index_offset
            if index_number < len(descriptor.expected_checksums) and \
                    descriptor.expected_checksums[index_number] == entry[4]:
                matched_entries += 1
                matched_data_bytes += end - entry[2]
        total_entries += entry_count
        total_data_bytes += index_offset - ARCHIVE_HEADER.size
    actual = {
        "archives": len(descriptors),
        "entries": total_entries,
        "data_bytes": total_data_bytes,
        "skipped_entries": matched_entries,
        "skipped_data_bytes": matched_data_bytes,
        "original_validation_entries": total_entries - matched_entries,
    }
    if actual != EXPECTED_CORPUS:
        raise AssertionError("archive checksum gate coverage drifted: %r" % actual)


def feature_blocks(text: str) -> tuple[str, ...]:
    lines = text.splitlines(keepends=True)
    blocks = []
    index = 0
    while index < len(lines):
        if lines[index].rstrip("\r\n") != FEATURE_GUARD:
            index += 1
            continue
        start = index
        index += 1
        depth = 1
        while index < len(lines) and depth:
            stripped = lines[index].strip()
            if stripped.startswith("#if"):
                depth += 1
            elif stripped == "#endif":
                depth -= 1
            index += 1
        if depth:
            raise AssertionError("unterminated archive-skip feature guard")
        blocks.append("".join(lines[start:index]))
    return tuple(blocks)


def strip_feature_blocks(text: str) -> str:
    result = text
    for block in feature_blocks(text):
        result = result.replace(block, "", 1)
    return result


def verify_rendered(text: str) -> None:
    blocks = feature_blocks(text)
    # The persistent-receipt extension adds begin/success/failure hooks under
    # the same outer opt-in guard.  Stripping all seven blocks must still
    # reproduce the pre-feature owner byte-for-byte.
    if len(blocks) != 7:
        raise AssertionError("archive-skip guarded-block census changed")
    joined = "".join(blocks)
    required = (
        "ld32((uint32_t)(c->ebp + (uint32_t)(int32_t)(-0xe50)))",
        "ld8((uint32_t)(_archive_entry + 0x18U)) != 0U",
        "if (!guest_stack_adjust_generated(c, 4U)) return;",
        "goto L_00563c8c;",
        "L_00563c8c:",
        "[archive-validation-skip] build=%s archive=%u skipped=%u original=%u\\n",
        "ISAAC_VITA_ARCHIVE_VALIDATION_SKIP_BUILD_ID",
        "isaac_vita_archive_receipt_begin(_archive_name)",
        "(_guest_vita_archive_receipt_token & 0x100U) != 0U",
        "++_guest_vita_archive_validation_receipt;",
        "++_guest_vita_archive_validation_success;",
        "++_guest_vita_archive_validation_failure;",
        "isaac_vita_archive_receipt_finish(",
        "[archive-validation-receipt] build=%s archive=%u baked=%u receipt=%u original=%u success=%u failure=%u published=%u token=%u\\n",
    )
    if any(item not in joined for item in required):
        raise AssertionError("archive-skip generated block changed")
    before = text.index("/* 00563b53  mov dword ptr [ebp - 0xe80], edi */")
    shortcut = text.index("Vita opt-in: skip only", before)
    constructor = text.index("/* 00563b59  call 0x59bfa0 */", shortcut)
    resume = text.index("/* 00563c8c  mov eax", constructor)
    if not before < shortcut < constructor < resume:
        raise AssertionError("archive-skip seam moved outside registration/insert edge")
    if text.count("sub_0059bfa0(c);") != 1 or text.count("sub_0059c0b0(c);") != 2:
        raise AssertionError("original validation constructor/destructor fallback changed")
    receipt_begin = text.index("isaac_vita_archive_receipt_begin(_archive_name)")
    opened_label = text.index("L_005638e3:")
    opened_body = text.index("/* 005638e3  mov edi", opened_label)
    receipt_shortcut = text.index(
        "(_guest_vita_archive_receipt_token & 0x100U) != 0U", shortcut
    )
    original_increment = text.index(
        "++_guest_vita_archive_validation_original;", receipt_shortcut
    )
    failure_increment = text.index(
        "++_guest_vita_archive_validation_failure;", original_increment
    )
    failure_body = text.index("/* 00563c30  push", failure_increment)
    success_label = text.index("L_00563c7a:", failure_body)
    success_increment = text.index(
        "++_guest_vita_archive_validation_success;", success_label
    )
    success_body = text.index("/* 00563c7a  lea", success_increment)
    receipt_finish = text.index("isaac_vita_archive_receipt_finish(", success_body)
    loop_exit = text.index("/* 00563d3b  mov eax", receipt_finish)
    if not (opened_label < receipt_begin < opened_body and
            shortcut < receipt_shortcut < original_increment < constructor and
            original_increment < failure_increment < failure_body and
            success_label < success_increment < success_body and
            success_body < receipt_finish < loop_exit):
        raise AssertionError("archive receipt hooks moved outside proven seams")
    for marker, expected_count in (
        ("++_guest_vita_archive_validation_receipt;", 1),
        ("++_guest_vita_archive_validation_success;", 1),
        ("++_guest_vita_archive_validation_failure;", 1),
        ("isaac_vita_archive_receipt_finish(", 2),
    ):
        if text.count(marker) != expected_count:
            raise AssertionError("archive receipt counter/call census changed: " + marker)


def exercise_generator(pe: Path) -> None:
    os.environ["REPENTOGXM_PE"] = str(pe)
    sys.path.insert(0, str(HERE))
    import gen_all as generator  # noqa: E402
    from image import Image  # noqa: E402

    generator.EXE = str(pe)
    pin = Image(str(pe), DEFAULT_IMAGE_BASE)
    rendered = []
    for base in (DEFAULT_IMAGE_BASE, VITA_IMAGE_BASE):
        emit = Image(str(pe), base)
        result = generator._translate_function(
            emit, {"rva": ROOT_RVA}, None, {}, pin_img=pin
        )
        if result.get("stub") is not None or not result.get("vita_archive_validation_skip"):
            raise AssertionError("archive-validation owner did not render cleanly")
        verify_rendered(result["text"])
        rendered.append(result["text"])
    if feature_blocks(rendered[0]) != feature_blocks(rendered[1]):
        raise AssertionError("archive shortcut differs by guest image base")

    mutated = copy.copy(pin)
    memory = bytearray(pin.mem)
    memory[SITE_RVA + 1] ^= 1
    mutated.mem = bytes(memory)
    try:
        generator._translate_function(
            Image(str(pe), VITA_IMAGE_BASE),
            {"rva": ROOT_RVA}, None, {}, pin_img=mutated,
        )
    except RuntimeError as exc:
        if "archive-validation-skip" not in str(exc):
            raise
    else:
        raise AssertionError("mutated archive-validation seam was accepted")


def extract_owner(text: str) -> str:
    marker = "/* sub_00563880  RVA 00563880"
    start = text.find(marker)
    if start < 0:
        raise AssertionError("generated archive-validation owner is absent")
    end = text.find("\n/* sub_", start + len(marker))
    return text[start:] if end < 0 else text[start:end]


def verify_generated_delta(baseline: Path, candidate: Path) -> str:
    pattern = re.compile(r"guest_\d{4}\.c$")
    base_files = {path.name: path for path in baseline.iterdir() if pattern.fullmatch(path.name)}
    candidate_files = {path.name: path for path in candidate.iterdir() if pattern.fullmatch(path.name)}
    if base_files.keys() != candidate_files.keys():
        raise AssertionError("generated numbered-unit sets differ")
    changed = [
        name for name in sorted(base_files)
        if sha256_file(base_files[name]) != sha256_file(candidate_files[name])
    ]
    if len(changed) != 1:
        raise AssertionError("archive skip changed %d numbered units: %r" % (len(changed), changed))
    name = changed[0]
    base_owner = extract_owner(base_files[name].read_text(encoding="utf-8"))
    candidate_owner = extract_owner(candidate_files[name].read_text(encoding="utf-8"))
    verify_rendered(candidate_owner)
    if strip_feature_blocks(candidate_owner) != base_owner:
        raise AssertionError("feature OFF does not reproduce the baseline owner byte-for-byte")
    owner_count = 0
    for path in candidate_files.values():
        owner_count += path.read_text(encoding="utf-8").count(
            "void sub_00563880(CPU *__restrict c)"
        )
    if owner_count != 1:
        raise AssertionError("generated archive-validation owner census changed")
    return name


def verify_build_switches() -> None:
    cmake = (HERE / "vita/CMakeLists.txt").read_text(encoding="utf-8")
    option = re.search(
        r"option\(ISAAC_VITA_ARCHIVE_VALIDATION_SKIP\s+.*?\s+(ON|OFF)\)",
        cmake, flags=re.S,
    )
    if option is None or option.group(1) != "OFF":
        raise AssertionError("archive-validation skip is not default OFF")
    if cmake.count("ISAAC_VITA_ARCHIVE_VALIDATION_SKIP=1") != 1 or \
            "ISAAC_VITA_ARCHIVE_VALIDATION_SKIP_BUILD_ID=" not in cmake:
        raise AssertionError("archive-validation owner stamp/definition changed")
    receipt = re.search(
        r"option\(ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT\s+.*?\s+(ON|OFF)\)",
        cmake, flags=re.S,
    )
    if receipt is None or receipt.group(1) != "OFF":
        raise AssertionError("archive-validation receipt is not default OFF")
    if cmake.count("host_vita_archive_receipt.c") != 1 or \
            cmake.count("ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT=1") != 1 or \
            "ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT requires " not in cmake:
        raise AssertionError("archive-validation receipt build closure changed")
    receipt_source = (
        HERE / "runtime/host_vita_archive_receipt.c"
    ).read_text(encoding="utf-8")
    corpus_match = re.search(
        r"s_corpus_sha256\[32\]\s*=\s*\{(.*?)\};",
        receipt_source, flags=re.S,
    )
    if corpus_match is None:
        raise AssertionError("archive receipt corpus constant is absent")
    corpus_bytes = bytes(
        int(value, 16)
        for value in re.findall(r"0x([0-9a-fA-F]{2})", corpus_match.group(1))
    )
    if corpus_bytes.hex() != RECEIPT_CORPUS_SHA256:
        raise AssertionError("archive receipt C corpus constant differs from JSON")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path, required=True)
    parser.add_argument(
        "--manifest", type=Path,
        default=HERE / "vita/archive_validation_corpus.v1.json",
    )
    parser.add_argument("--packed-dir", type=Path)
    parser.add_argument("--exercise-generator", action="store_true")
    parser.add_argument("--baseline-generated", type=Path)
    parser.add_argument("--candidate-generated", type=Path)
    args = parser.parse_args()
    if (args.baseline_generated is None) != (args.candidate_generated is None):
        parser.error("--baseline-generated and --candidate-generated must be paired")

    pe = args.pe.resolve()
    image = PeImage(pe)
    verify_machine_code(image)
    manifest, descriptors = verify_manifest_shape(args.manifest.resolve(), image)
    corpus_status = "receipt"
    if args.packed_dir:
        verify_archive_corpus(manifest, descriptors, args.packed_dir.resolve())
        corpus_status = "18 full hashes/index/checksum coverage"
    if args.exercise_generator:
        exercise_generator(pe)
    changed = "SKIP"
    if args.baseline_generated:
        changed = verify_generated_delta(
            args.baseline_generated.resolve(), args.candidate_generated.resolve()
        )
    verify_build_switches()
    print(
        "archive validation skip: PASS; PE/windows=4; corpus=%s; "
        "skip=18635/21283 entries, 888222052/1034148878 bytes; "
        "generator=%s; generated-delta=%s; default-OFF=PASS"
        % (corpus_status, "2 bases+mutation" if args.exercise_generator else "SKIP", changed)
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
