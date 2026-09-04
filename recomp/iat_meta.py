"""Extract exact PE import metadata for the generated guest runtime.

The translated program does not call host functions directly.  At load time
the runtime replaces every IAT entry with a stable token (the relocated
address of the slot itself); an indirect guest call can then map that token
back to the exact ``DLL!symbol`` recorded here and fault by name.

This module deliberately owns only extraction and C emission.  It does not
classify imports or guess at host implementations.
"""
import json

import pefile


def read_imports(exe_path):
    """Return sorted ``(slot_rva, "DLL!symbol")`` entries from a PE.

    Import names in PE files are ASCII.  Ordinal imports use ``#<ordinal>`` so
    the displayed value remains exact and unambiguous.
    """
    pe = pefile.PE(exe_path)
    base = pe.OPTIONAL_HEADER.ImageBase
    by_slot = {}
    for descriptor in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        dll = descriptor.dll.decode("ascii")
        for imported in descriptor.imports:
            symbol = (imported.name.decode("ascii") if imported.name is not None
                      else "#%d" % imported.ordinal)
            slot_rva = imported.address - base
            display = "%s!%s" % (dll, symbol)
            old = by_slot.setdefault(slot_rva, display)
            if old != display:
                raise ValueError("IAT slot 0x%08x has two names: %s / %s"
                                 % (slot_rva, old, display))
    return sorted(by_slot.items())


def emit_c_table(out, entries):
    """Append a ``guest_import`` table and registration wrapper to C output."""
    out.write("\n/* GENERATED from the PE import directory.  Each slot RVA is\n"
              " * patched to its own relocated VA by guest_image_load(), so\n"
              " * register-loaded and direct-IAT calls retain exact identity. */\n")
    out.write("static const guest_import s_imports[] = {\n")
    for slot_rva, display in entries:
        # json.dumps gives a quoted, escaped ASCII string accepted by C as-is.
        out.write("    { 0x%08xU, %s },\n"
                  % (slot_rva, json.dumps(display, ensure_ascii=True)))
    out.write("};\n\n")
    out.write("const uint32_t guest_import_table_len = %dU;\n\n" % len(entries))
    out.write("void guest_register_all_imports(void)\n{\n"
              "    guest_register_imports(s_imports, %dU);\n}\n" % len(entries))
