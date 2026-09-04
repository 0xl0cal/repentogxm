"""Fail-closed ownership proof for ``gen_all.py --outdir``."""

import inspect
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import build_contract as BC  # noqa: E402
import gen_all as G  # noqa: E402


def expect_failure(callback, fragment):
    try:
        callback()
    except RuntimeError as exc:
        assert fragment in str(exc), str(exc)
    else:
        raise AssertionError("expected failure containing %r" % fragment)


def configure(outdir):
    G.OUTDIR = os.path.abspath(outdir)
    G.MANIFEST = os.path.join(G.OUTDIR, "manifest.json")


def main():
    original = {
        "OUTDIR": G.OUTDIR,
        "MANIFEST": G.MANIFEST,
        "generated_output_paths": G.generated_output_paths,
    }
    try:
        repository_root = os.path.dirname(HERE)
        expect_failure(
            lambda: G._validate_generation_outdir_path(repository_root),
            "inside the source repository",
        )

        with tempfile.TemporaryDirectory(prefix="gen-outdir-owner-") as root:
            lf_output = os.path.join(root, "generated-lf.c")
            with G._open_generated_text(lf_output) as stream:
                stream.write("first\nsecond\n")
            with open(lf_output, "rb") as stream:
                assert stream.read() == b"first\nsecond\n"

            main_source = inspect.getsource(G._main)
            assert main_source.count("with _open_generated_text(") == 4
            for writer_call in (
                "_open_generated_text(path)",
                '_open_generated_text(os.path.join(OUTDIR, "guest_stubs.c"))',
                '_open_generated_text(os.path.join(OUTDIR, "guest_funcs.h"))',
                '_open_generated_text(os.path.join(OUTDIR, "guest_table.c"))',
            ):
                assert main_source.count(writer_call) == 1

            foreign = os.path.join(root, "foreign")
            os.mkdir(foreign)
            victim = os.path.join(foreign, "victim.c")
            with open(victim, "w", encoding="ascii") as stream:
                stream.write("must survive\n")
            configure(foreign)
            G.generated_output_paths = lambda: {}
            expect_failure(
                G._prepare_generation_outdir,
                "without its exact owner marker",
            )
            assert open(victim, encoding="ascii").read() == "must survive\n"
            assert not os.path.exists(G._generation_owner_marker(foreign))

            empty = os.path.join(root, "empty")
            configure(empty)
            G._prepare_generation_outdir()
            assert os.path.isdir(empty)
            assert G._generation_owner_matches(empty)
            assert os.listdir(empty) == []

            legacy = os.path.join(root, "legacy")
            os.mkdir(legacy)
            generated = os.path.join(legacy, "guest_0000.c")
            with open(generated, "w", encoding="ascii") as stream:
                stream.write("generated fixture\n")
            configure(legacy)
            G.generated_output_paths = lambda: {
                "guest_0000.c": generated,
            }
            recipe_id, recipe = BC.recipe(
                "generate", {}, params={"fixture": 1})
            BC.write_manifest_atomic(
                G.MANIFEST,
                BC.make_manifest(
                    "generate", recipe_id, recipe,
                    BC.snapshot(legacy, ["guest_0000.c"])),
            )
            G._prepare_generation_outdir()
            assert G._generation_owner_matches(legacy)

            note = os.path.join(legacy, "do-not-delete.txt")
            with open(note, "w", encoding="ascii") as stream:
                stream.write("preserve me\n")
            expect_failure(
                G._remove_owned_generation_outputs,
                "unowned entries",
            )
            assert os.path.isfile(generated)
            assert os.path.isfile(G.MANIFEST)
            assert open(note, encoding="ascii").read() == "preserve me\n"

            os.remove(note)
            G._remove_owned_generation_outputs()
            assert not os.path.exists(generated)
            assert not os.path.exists(G.MANIFEST)
            assert G._generation_owner_matches(legacy)

        print(
            "gen_all outdir ownership/LF writers: "
            "source/foreign/legacy/unknown-entry/4-writer contracts PASS")
        return 0
    finally:
        G.OUTDIR = original["OUTDIR"]
        G.MANIFEST = original["MANIFEST"]
        G.generated_output_paths = original["generated_output_paths"]


if __name__ == "__main__":
    raise SystemExit(main())
