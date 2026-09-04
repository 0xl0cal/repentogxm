import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent
RUNTIME = ROOT / "runtime"
VITA = ROOT / "vita"
BUILD_WRAPPER = ROOT.parent / "tools" / "build_vita.py"


class VitaFiosCacheContractTests(unittest.TestCase):
    def test_cmake_switch_is_default_off_and_source_scoped(self):
        cmake = (VITA / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertRegex(
            cmake,
            r"option\(ISAAC_VITA_FIOS_CACHE\s+"
            r'"[^"]+" OFF\)',
        )
        block = re.search(
            r"if\(ISAAC_VITA_FIOS_CACHE\)(.*?)\n\s*else\(\)",
            cmake,
            re.S,
        )
        self.assertIsNotNone(block)
        self.assertIn('"${ISAAC_RUNTIME}/host_vita_fios_cache.c"', block.group(1))
        self.assertIn('"${ISAAC_RUNTIME}/entry_vita.c"', block.group(1))
        self.assertIn("ISAAC_VITA_FIOS_CACHE=1", block.group(1))
        self.assertNotIn("${ISAAC_GENERATED_C}", block.group(1))
        self.assertRegex(
            re.sub(r"\s+", " ", cmake),
            r"if\(NOT ISAAC_VITA_SCAFFOLD_ONLY AND ISAAC_VITA_FIOS_CACHE\)"
            r".*?SceFios2_stub.*?endif\(\)",
        )

        wrapper = BUILD_WRAPPER.read_text(encoding="utf-8")
        self.assertIn('"--fios-cache"', wrapper)
        self.assertIn("default=False", wrapper)
        self.assertIn('f"-DISAAC_VITA_FIOS_CACHE={on_off(fios_cache)}"', wrapper)

    def test_entry_lifecycle_surrounds_guest_archive_work(self):
        entry = (RUNTIME / "entry_vita.c").read_text(encoding="utf-8")
        image = entry.index("guest_image_load(pe_path)")
        stack = entry.index("guest_stack_init(&cpu)")
        fios = entry.index("isaac_vita_fios_cache_initialize()")
        guest_entry = entry.index("guest_run_until_stop(&cpu, entry)")
        archive_close = entry.index("isaac_vita_crt_archive_cache_shutdown()")
        fios_close = entry.index("isaac_vita_fios_cache_shutdown()")
        stack_close = entry.index("guest_stack_free(&cpu)")
        image_close = entry.index("guest_image_free()")
        self.assertLess(image, stack)
        self.assertLess(stack, fios)
        self.assertLess(fios, guest_entry)
        self.assertLess(archive_close, fios_close)
        self.assertLess(fios_close, stack_close)
        self.assertLess(stack_close, image_close)

    def test_owned_memory_is_bounded_and_not_newlib(self):
        header = (RUNTIME / "host_vita_fios_cache.h").read_text(encoding="utf-8")
        source = (RUNTIME / "host_vita_fios_cache.c").read_text(encoding="utf-8")
        self.assertIn("ISAAC_VITA_FIOS_MEMBLOCK_BYTES      0x9e000U", header)
        self.assertIn("ISAAC_VITA_FIOS_CACHE_BYTES         0x80000U", header)
        self.assertIn("SCE_KERNEL_MEMBLOCK_TYPE_USER_RW", source)
        self.assertIn("ISAAC_FIOS_LAYOUT_BYTES == 644024U", source)
        self.assertEqual(source.count("sceKernelAllocMemBlock("), 1)
        self.assertEqual(source.count("sceKernelFreeMemBlock("), 1)
        for allocator in (
            "malloc", "calloc", "realloc", "free", "memalign",
            "aligned_alloc", "strdup",
        ):
            self.assertNotRegex(source, rf"\b{allocator}\s*\(")

    def test_filter_scope_and_failure_cleanup_are_explicit(self):
        source = (RUNTIME / "host_vita_fios_cache.c").read_text(encoding="utf-8")
        platform = (VITA / "platform.h").read_text(encoding="utf-8")
        self.assertIn("s_fios.cache_context.pPath = ISAAC_VITA_DATA_ROOT", source)
        self.assertIn('ISAAC_VITA_GUEST_DATA_PARENT  "ux0:data"', platform)
        self.assertIn('ISAAC_VITA_GUEST_DATA_PARENT "/isaacr001"', platform)
        self.assertIn("sceFiosTerminate();", source)
        self.assertIn("sceKernelFreeMemBlock", source)
        self.assertNotIn("sceFiosIOFilterRemove", source)

    def test_raw_allocator_source_closure_knows_the_optional_owner(self):
        gate = (VITA / "vita_raw_allocator_gate.py").read_text(encoding="utf-8")
        self.assertIn(
            '("ISAAC_VITA_FIOS_CACHE", "host_vita_fios_cache.c")', gate
        )

    def test_fault_oracle_covers_every_lifecycle_edge(self):
        oracle = (RUNTIME / "host_vita_fios_cache_oracle.c").read_text(
            encoding="utf-8"
        )
        for scenario in (
            "cold-shutdown",
            "alloc-fail",
            "getbase-fail",
            "getbase-null",
            "layout-fail",
            "initialize-fail",
            "filter-fail",
            "filter-free-fail",
            "shutdown-free-fail",
        ):
            self.assertIn(f'"{scenario}"', oracle)


if __name__ == "__main__":
    unittest.main()
