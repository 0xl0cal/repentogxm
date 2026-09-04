"""Load the guest image, laid out by RVA and rebased to a chosen address.

Why rebasing rather than "map it at its own ImageBase":

  * 0x400000 is not available in a 32-bit Windows process. ntdll creates the
    default process heap there during process initialisation, before the entry
    point and before any TLS callback, so there is no hook early enough to
    reserve it. Measured, not assumed -- VirtualAlloc returns 487 and a region
    walk shows a heap segment sitting at 0x400000.
  * It would not be available on the Vita either, whose user addresses live
    around 0x8xxxxxxx. A fixed 0x400000 was an assumption that happened to
    look free, and finding that out now costs one afternoon rather than a
    regeneration of 14k functions.

So the image is relocated to a base chosen at build time, using the PE's own
`.reloc` section (324 KB of it). The translator disassembles the *relocated*
bytes, so every absolute displacement it emits is already correct for the
runtime base, and the runtime performs the identical relocation when it maps
the image. Data pointers inside .rdata/.data are fixed by the same pass.
"""
import os
import struct

import pefile

# Chosen for the PC milestone: our own executable is linked at 0x20000000,
# the CRT heap sits low, and the system DLLs sit high, so this is free.
# The Vita build will pick its own and re-run the translator.
DEFAULT_BASE = 0x30000000


class Image:
    def __init__(self, path, newbase=DEFAULT_BASE):
        pe = pefile.PE(path, fast_load=True)
        pe.parse_data_directories(directories=[
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]])
        self.path = path
        self.orig_base = pe.OPTIONAL_HEADER.ImageBase
        self.base = newbase
        self.size = pe.OPTIONAL_HEADER.SizeOfImage
        self.entry = pe.OPTIONAL_HEADER.AddressOfEntryPoint

        buf = bytearray(self.size)
        hdr = pe.OPTIONAL_HEADER.SizeOfHeaders
        buf[:hdr] = pe.__data__[:hdr]
        self.sections = []
        for s in pe.sections:
            nm = s.Name.rstrip(b"\x00").decode("latin1")
            d = s.get_data()
            n = min(len(d), s.Misc_VirtualSize) if s.Misc_VirtualSize else len(d)
            buf[s.VirtualAddress:s.VirtualAddress + n] = d[:n]
            self.sections.append({
                "name": nm, "rva": s.VirtualAddress,
                "vsize": s.Misc_VirtualSize,
                "exec": bool(s.Characteristics & 0x20000000),
            })

        delta = (newbase - self.orig_base) & 0xFFFFFFFF
        self.applied = 0
        self.skipped = 0
        self.reloc_rvas = set()
        for reloc in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
            for e in reloc.entries:
                if e.type == 0:                         # ABSOLUTE, padding
                    self.skipped += 1
                    continue
                if e.type != 3:                         # HIGHLOW is all a
                    raise RuntimeError(                  # 32-bit PE should have
                        "unexpected relocation type %d at rva %x"
                        % (e.type, e.rva))
                off = e.rva
                self.reloc_rvas.add(off)
                if delta:
                    v = struct.unpack_from("<I", buf, off)[0]
                    struct.pack_into("<I", buf, off, (v + delta) & 0xFFFFFFFF)
                    self.applied += 1
        self.mem = bytes(buf)

        text = next(s for s in self.sections if s["name"] == ".text")
        self.text_rva = text["rva"]
        self.text_end = text["rva"] + text["vsize"]

    # -- accessors used by the translator ---------------------------------
    def code_at(self, rva, n):
        return self.mem[rva:rva + n]

    def is_code(self, rva):
        return self.text_rva <= rva < self.text_end

    def va(self, rva):
        return self.base + rva

    def rva_of(self, va):
        return va - self.base


if __name__ == "__main__":
    import sys
    default_workdir = os.path.abspath(os.environ.get(
        "REPENTOGXM_WORKDIR",
        os.path.join(os.path.dirname(__file__), "..", "build", "recompiler"),
    ))
    p = sys.argv[1] if len(sys.argv) > 1 else os.path.abspath(os.environ.get(
        "REPENTOGXM_PE",
        os.path.join(default_workdir, "isaac-ng.exe.unpacked.exe"),
    ))
    img = Image(p)
    print("original base   : 0x%08x" % img.orig_base)
    print("rebased to      : 0x%08x  (delta 0x%08x)"
          % (img.base, (img.base - img.orig_base) & 0xFFFFFFFF))
    print("size            : 0x%x" % img.size)
    print("relocations     : %d applied, %d padding entries skipped"
          % (img.applied, img.skipped))
    print(".text           : rva %06x..%06x" % (img.text_rva, img.text_end))
    # A spot check that costs nothing and would catch a broken relocation:
    # the RNG shift table must still read 1, 3, 10.
    t = struct.unpack_from("<3I", img.mem, 0x608aa0)
    print("RNG::s_Shifts[0..2] = %d, %d, %d   (expect 1, 3, 10)" % t)
