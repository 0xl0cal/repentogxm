#!/usr/bin/env python3
"""Fresh actual owner bodies for the attribute coalescing backend fixture."""
from pathlib import Path
import argparse


def body(text, signature):
    start = text.index(signature)
    opening = text.index("{", start)
    end, depth = opening + 1, 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end] + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    runtime = Path(__file__).resolve().parents[1] / "runtime"
    header = "/* Regenerated from the current production sources; do not freeze. */\n"
    for filename, signature, name in (
        ("kage_vita_backend.c", "void kage_vita_backend_deactivate(void)", "kage_vita_backend_deactivate"),
        ("kage_vita_loading.c", "static void kage_loading_swap(", "kage_loading_swap"),
        ("kage_vita_continue_profile.c", "static int continue_overlay_begin(", "continue_overlay_begin"),
        ("kage_vita_continue_profile.c", "static void continue_overlay_stage(", "continue_overlay_stage"),
    ):
        text = (runtime / filename).read_text(encoding="utf-8")
        actual = body(text, signature)
        assert actual.count("gl_vita_backend_attrib_sync();") == 1, name
        if "vglSwapBuffers(" in actual:
            assert actual.index("gl_vita_backend_attrib_sync();") < actual.index("vglSwapBuffers(")
        header += actual.replace(name + "(", "attrib_owner_" + name + "(", 1)
    # The long game owner has an early unready return; all live-context native
    # consumers are after the sync, independent of optional GL/FBO profiling.
    present = body((runtime / "kage_vita_backend.c").read_text(), "int kage_vita_backend_present(void)")
    sync = present.index("gl_vita_backend_attrib_sync();")
    for native in ("kage_vita_loading_finish();", "gl_vita_backend_first_frame_before_present(", "vglSwapBuffers("):
        assert sync < present.index(native), native
    assert "glBindVertexArray" not in (runtime / "gl_surface_generated.h").read_text()
    assert "glGetVertexAttrib" not in (runtime / "gl_surface_generated.h").read_text()
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "gl_vita_attrib_boundaries_generated.inc").write_text(header, encoding="utf-8")


if __name__ == "__main__":
    main()
