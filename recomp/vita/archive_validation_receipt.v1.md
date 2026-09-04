# Archive validation receipt v1

This is an opt-in cold-start cache for the two frozen archives which cannot use
the PE-baked checksum shortcut: `resources/packed/animations.a` and
`resources/packed/afterbirth.a`.  It never changes archive contents or the
original failure path.

The runtime creates a row only after the generated seam reports every entry on
the original validation path with no failure (1/1 or 2647/2647), then performs
an independent full SHA-256 pass over the current archive.  A future PC/Vita
transfer tool may create the same row only after verifying the complete file
hash before upload and reading the final Vita-side metadata after upload.

## Frozen identities

The complete corpus key is SHA-256
`8eaf383602b9e58ad42589f1436a911bae6feb1845b42fb6e33398383490f0f6`.
It is calculated from `archive_validation_corpus.v1.json` encoded as UTF-8 JSON
with keys sorted and separators `(',', ':')` (4851 bytes).  A receipt for a
different corpus is invalid even if its two rows happen to look compatible.

| Archive | Bytes | Mode | Index | Entries | Full SHA-256 |
|---|---:|---:|---:|---:|---|
| `animations.a` | 660301 | 1 | 660281 | 1 | `182e071934fc2d4600506bb326bb8d3946861bfa7ce3c4404d524d68ada8abf5` |
| `afterbirth.a` | 145319513 | 2 | 145266573 | 2647 | `ee400c960d9b6854f612f15baededbb8221920795961ba422253893557d64d8c` |

The index SHA-256 values remain the ones in the JSON corpus and are copied into
every valid row.

## On-disk encoding

All integers are unsigned little-endian.  Each slot is exactly 512 bytes; any
short, long, malformed, unknown-version, or bad-digest file is ignored.

| Offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 16 | `ISAACAVRCPTV1\0\0\0` |
| 16 | 4 | version, exactly 1 |
| 20 | 4 | length, exactly 512 |
| 24 | 8 | generation, non-zero |
| 32 | 32 | complete-corpus SHA-256 |
| 64 | 4 | valid-row mask, subset of 3 and non-zero |
| 68 | 4 | row count, exactly 2 |
| 72 | 8 | zero |
| 80 | 160 | `animations.a` row |
| 240 | 160 | `afterbirth.a` row |
| 400 | 80 | zero |
| 480 | 32 | SHA-256 of bytes 0..479 |

A missing row is 160 zero bytes.  A valid row is:

| Row offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 4 | kind: 1 animations, 2 afterbirth |
| 4 | 4 | provenance: 1 runtime, 2 PC/Vita transfer tool |
| 8 | 8 | exact file size |
| 16 | 8 | exact index offset |
| 24 | 4 | exact entry count |
| 28 | 4 | exact archive mode |
| 32 | 16 | Vita ctime (`u16` Y/M/D/h/m/s, `u32` microseconds) |
| 48 | 16 | Vita mtime in the same encoding |
| 64 | 32 | frozen full-file SHA-256 |
| 96 | 32 | frozen full-index SHA-256 |
| 128 | 32 | selected-data SHA-256; zero for animations |

Zero is admitted for a completely unavailable Vita timestamp.  Otherwise the
date must be structurally valid.  A PC utility must serialize the timestamps
returned by the Vita after the final archive rename, not host timestamps from
before transfer.

## Cheap identity and selected-data digest

Every reuse opens the canonical archive and compares path and descriptor stats
before and after all reads.  Size, regular-file type, ctime, mtime, `ARCH000`
header, and the full index SHA-256 must match.  `animations.a` is only 660301
bytes and is fully SHA-256 hashed on every reuse.

For `afterbirth.a`, hash exactly:

1. ASCII `isaac-vita-sample-v1` without a terminator.
2. LE `u32 kind`, `u64 file_size`, `u64 index_offset`, `u32 64`, `u32 4096`.
3. For sample `i = 0..63`, let
   `offset = floor((index_offset - 4096) * i / 63)`.  Hash LE `u64 offset`,
   LE `u32 4096`, then those exact 4096 file bytes.

This reads 262144 selected data bytes plus the 52940-byte index instead of the
145319513-byte archive.  The digest stored by the publisher is computed from
the same file descriptor and stable pre/post metadata as its mandatory full
hash.

This is an integrity cache for a locally managed, process-lifetime immutable
asset directory, matching the existing packed-archive descriptor-cache
contract.  It is not an authentication mechanism: an attacker able to rewrite
an unsampled byte while preserving Vita ctime/mtime, size, index, and all
selected blocks can evade the cheap identity.  Supporting hostile concurrent
writers would require a full-file hash on every boot and would remove the fast
path.

## Atomic publication

Slots are:

- `ux0:/data/isaacr001/archive-validation-receipt.v1.0.bin`
- `ux0:/data/isaacr001/archive-validation-receipt.v1.1.bin`

Load both independently and select the valid greatest generation.  Equal
generations have no unique winner and invalidate the cache for that process.
Files ending in `.tmp` are never loaded.

To publish, preserve the selected slot, write the other slot's `.tmp` with
exclusive creation and Vita mode `0666`, write all 512 bytes, sync the
descriptor, close it, reopen and parse/compare all bytes, remove the inactive
destination, rename the temp, then sync `ux0:`.  The system read/write bits in
that mode are mandatory because the application SELF is UNSAFE.  Only after
all steps succeed may in-memory state select the new generation.  Failure at
any step leaves the original validation path and the previously selected slot
available.  Generation `UINT64_MAX` is readable but cannot be advanced.

## Bounded finish diagnostic

Each target `finish` call retains one fixed-size diagnostic record and emits
one Vita log line.  It performs no additional filesystem I/O.  The line starts
with `[archive-validation-receipt-stage]` and reports `kind`, `published`, a
stable numeric stage plus its name, the native negative I/O result when one
exists, and three stage-specific 32-bit values.

The post-validation stages deliberately distinguish full-file read (21), full
digest mismatch (22), descriptor/path stability and close (23--27), every temp
write/reopen check (40--61), byte/semantic comparison (62--63), destination
removal (64), rename (65), and device sync (66).  For stat mismatches, `detail`
is a bit mask: size `0x01`, regular-file type `0x02`, ctime `0x04`, and mtime
`0x08`.  For temp decode failures, the low byte identifies the format check;
`6` and `7` are invalid ctime and mtime, with the invalid-field mask in bits
16--22.  Stage 100 is a completed publication.
