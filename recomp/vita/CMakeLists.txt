cmake_minimum_required(VERSION 3.19)

include("${CMAKE_CURRENT_LIST_DIR}/literal_glob_path.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/deterministic_vpk.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/vita_text_layout.cmake")
foreach(ISAAC_STRICT_BUILD_PATH IN ITEMS
        CMAKE_CURRENT_SOURCE_DIR CMAKE_BINARY_DIR)
  isaac_validate_strict_absolute_path(
    "${${ISAAC_STRICT_BUILD_PATH}}" "${ISAAC_STRICT_BUILD_PATH}")
endforeach()
if(NOT DEFINED VITASDK OR "${VITASDK}" STREQUAL "")
  if(DEFINED ENV{VITASDK} AND NOT "$ENV{VITASDK}" STREQUAL "")
    set(VITASDK "$ENV{VITASDK}")
  else()
    message(FATAL_ERROR
      "Set VITASDK to the strict portable softfp SDK root")
  endif()
endif()
isaac_validate_strict_absolute_path("${VITASDK}" "VITASDK")

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${VITASDK}/share/vita.toolchain.cmake"
      CACHE FILEPATH "VitaSDK toolchain")
endif()
isaac_validate_strict_absolute_path(
  "${CMAKE_TOOLCHAIN_FILE}" "CMAKE_TOOLCHAIN_FILE")

set(ISAAC_GENERATED_DIR "" CACHE PATH
    "gen_all.py output regenerated for ISAAC_GUEST_BASE")
set(ISAAC_PE_PATH "" CACHE FILEPATH
    "Unpacked Repentance PE to package as app0:/isaac-ng.exe.unpacked.exe")
set(SOURCE_DATE_EPOCH "$ENV{SOURCE_DATE_EPOCH}" CACHE STRING
    "UTC commit epoch used for deterministic VPK member timestamps")
option(ISAAC_VITA_SCAFFOLD_ONLY
       "Build only the fixed-allocation/logging smoke VPK" OFF)
if(NOT ISAAC_VITA_SCAFFOLD_ONLY)
  isaac_validate_generated_absolute_path(
    "${ISAAC_GENERATED_DIR}" "ISAAC_GENERATED_DIR")
  isaac_validate_strict_absolute_path(
    "${ISAAC_PE_PATH}" "ISAAC_PE_PATH")
endif()

project(isaac_first_arm_fault LANGUAGES C)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

isaac_validate_strict_absolute_path("${VITASDK}" "VITASDK")
include("${VITASDK}/share/vita.cmake" REQUIRED)

# Real-hardware diagnostic: request the extended user-memory budget.
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

set(ISAAC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(ISAAC_RUNTIME "${ISAAC_ROOT}/recomp/runtime")
set(ISAAC_VITA_RAW_ALLOCATOR_POISON
    "${CMAKE_CURRENT_SOURCE_DIR}/isaac_vita_raw_allocator_poison.h")
set(ISAAC_VITA_RAW_ALLOCATOR_GATE
    "${CMAKE_CURRENT_SOURCE_DIR}/vita_raw_allocator_gate.py")
set(ISAAC_VITA_DETERMINISTIC_VPK_WRAPPER
    "${CMAKE_CURRENT_SOURCE_DIR}/deterministic_vpk.py")
set(ISAAC_VITA_LOADING_FALLBACK_INCLUDE
    "${ISAAC_RUNTIME}/kage_vita_loading_fallback.inc")
set(ISAAC_VITA_LOADING_SPECIALIST_INCLUDE
    "${ISAAC_RUNTIME}/kage_vita_loading_specialist.inc")
set(ISAAC_VITA_LOADING_SELECTION_MODULE
    "${CMAKE_CURRENT_SOURCE_DIR}/kage_vita_loading_selection.cmake")
set(ISAAC_VITA_STOCK_REFERENCE_SELECTION_MODULE
    "${CMAKE_CURRENT_SOURCE_DIR}/kage_vita_stock_reference_selection.cmake")
set(ISAAC_GUEST_BASE "0x98000000" CACHE STRING
    "Fixed Vita address of the relocated PE image")
set(ISAAC_VITA_HEAP_MB 81 CACHE STRING
    "newlib heap size below the fixed 0x98000000 guest image")
set(ISAAC_VITA_GUEST_LINK_ID "link:vita-cmake" CACHE STRING
    "Short executable identity printed by guest.c and diagnostic profiles")
set(ISAAC_VITA_GUEST_BUILD_OPT "O2" CACHE STRING
    "Short guest.c-only optimization identity printed at guest registration")
option(ISAAC_VITA_KAGE
       "Link the first honest KAGE/vitaGL graphics boundary" OFF)
option(ISAAC_VITA_LOADING_SPECIALIST
       "Show the Specialist Dance loading animation (needs ISAAC_VITA_KAGE)" OFF)
option(ISAAC_VITA_LOADING_PRESENTATION
       "Present the CPU-written loading overlay through vitaGL" ON)
if(ISAAC_VITA_AUDIO)
  set(ISAAC_VITA_VITAGL_RAM_RESERVE_DEFAULT 32)
else()
  set(ISAAC_VITA_VITAGL_RAM_RESERVE_DEFAULT 16)
endif()
set(ISAAC_VITA_VITAGL_RAM_RESERVE_MB ${ISAAC_VITA_VITAGL_RAM_RESERVE_DEFAULT} CACHE STRING
    "MiB of USER_RW vitaGL leaves outside its RAM pool (16 = vitaGL default; 32 when the OpenAL pool is reserved ahead of vitaGL)")
if(NOT ISAAC_VITA_VITAGL_RAM_RESERVE_MB MATCHES "^[0-9]+$" OR
   ISAAC_VITA_VITAGL_RAM_RESERVE_MB LESS 8 OR ISAAC_VITA_VITAGL_RAM_RESERVE_MB GREATER 96)
  message(FATAL_ERROR
    "ISAAC_VITA_VITAGL_RAM_RESERVE_MB must be an integer in 8..96: ${ISAAC_VITA_VITAGL_RAM_RESERVE_MB}")
endif()
math(EXPR ISAAC_VITA_VITAGL_RAM_RESERVE_BYTES "${ISAAC_VITA_VITAGL_RAM_RESERVE_MB} * 1048576")
option(ISAAC_VITA_AUDIO
       "Enable the OpenAL Soft KAGE audio boundary (A/B switch)" ON)
option(ISAAC_VITA_AUDIO_STREAM_RECEIPT
       "Log bounded OGG queue state around cooperative audio updates" OFF)
if(ISAAC_VITA_AUDIO_STREAM_RECEIPT AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE OR
    NOT ISAAC_VITA_AUDIO))
  message(FATAL_ERROR
    "ISAAC_VITA_AUDIO_STREAM_RECEIPT requires the translated KAGE runtime and ISAAC_VITA_AUDIO=ON")
endif()
option(ISAAC_VITA_AUDIO_WORKER
       "Pace guest stream refills to one translated decode per spacing window and pin the OpenAL Soft mixer thread to its own core (requires ISAAC_VITA_AUDIO=ON)" OFF)
option(ISAAC_VITA_AUDIO_REFILL_RECEIPT
       "Log bounded stream refill/decode receipts at the OpenAL import boundary" OFF)
set(ISAAC_VITA_AUDIO_REFILL_SPACING_US "20000" CACHE STRING
    "Minimum microseconds between two granted stream refills (ISAAC_VITA_AUDIO_WORKER)")
if(ISAAC_VITA_AUDIO_WORKER AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE OR
    NOT ISAAC_VITA_AUDIO))
  message(FATAL_ERROR
    "ISAAC_VITA_AUDIO_WORKER requires the translated KAGE runtime and ISAAC_VITA_AUDIO=ON")
endif()
if(ISAAC_VITA_AUDIO_REFILL_RECEIPT AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE OR
    NOT ISAAC_VITA_AUDIO))
  message(FATAL_ERROR
    "ISAAC_VITA_AUDIO_REFILL_RECEIPT requires the translated KAGE runtime and ISAAC_VITA_AUDIO=ON")
endif()
# No leading zero: the value is pasted as a C literal and 020000 would be octal.
if(NOT ISAAC_VITA_AUDIO_REFILL_SPACING_US MATCHES "^(0|[1-9][0-9]*)$" OR
   ISAAC_VITA_AUDIO_REFILL_SPACING_US GREATER 200000)
  message(FATAL_ERROR
    "ISAAC_VITA_AUDIO_REFILL_SPACING_US must be a whole number of microseconds (no leading zero) no larger than 200000")
endif()
option(ISAAC_VITA_NATIVE_VORBIS
       "Replace the translated stb_vorbis decoder (0x5bdf80) with the vendored v1.04 compiled natively; music streams are cloned into host mirrors and decoded ahead of time on a CPU 2 worker (requires ISAAC_VITA_AUDIO=ON)" OFF)
option(ISAAC_VITA_NATIVE_VORBIS_WORKER
       "Decode mirrored streams ahead of time on a SceKernel worker pinned to CPU 2; OFF decodes natively but synchronously on the game thread (model a)" ON)
option(ISAAC_VITA_NATIVE_VORBIS_RECEIPT
       "Log bounded native Vorbis receipts: header/clone facts per stream, frames and CRC32 per slot" OFF)
option(ISAAC_VITA_NATIVE_VORBIS_VERIFY
       "Also run the translated decoder for every slot, log PCM mismatches and ship the translated PCM (slow; proof build)" OFF)
if(ISAAC_VITA_NATIVE_VORBIS AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE OR
    NOT ISAAC_VITA_AUDIO))
  message(FATAL_ERROR
    "ISAAC_VITA_NATIVE_VORBIS requires the translated KAGE runtime and ISAAC_VITA_AUDIO=ON")
endif()
if((ISAAC_VITA_NATIVE_VORBIS_RECEIPT OR ISAAC_VITA_NATIVE_VORBIS_VERIFY) AND
   NOT ISAAC_VITA_NATIVE_VORBIS)
  message(FATAL_ERROR
    "ISAAC_VITA_NATIVE_VORBIS_RECEIPT/VERIFY require ISAAC_VITA_NATIVE_VORBIS=ON")
endif()
option(ISAAC_VITA_NATIVE_VORBIS_ASYNC
       "Native Vorbis stage 1: partial PCM slots (a wrapped call returns what the worker has ready once >= MIN_FRAMES, else decodes on the game thread only up to MIN_FRAMES inside SYNC_BUDGET_US), the mirror is cloned and its OGG ring primed already at stb_vorbis_open_file (--wrap=sub_005bd740) so the first Decode finds PCM ready, and the receipt CRCs leave the serving path; also counts stream replays (source ran dry) in the refill receipts.  Requires ISAAC_VITA_NATIVE_VORBIS=ON with its worker; excludes VERIFY.  OFF keeps every object byte-identical" OFF)
set(ISAAC_VITA_NATIVE_VORBIS_MIN_FRAMES "2048" CACHE STRING
    "ISAAC_VITA_NATIVE_VORBIS_ASYNC: frames a wrapped call returns as a partial slot without decoding on the game thread (2048 = 43 ms at 48 kHz)")
set(ISAAC_VITA_NATIVE_VORBIS_SYNC_BUDGET_US "3000" CACHE STRING
    "ISAAC_VITA_NATIVE_VORBIS_ASYNC: bound on the game thread's own decoding per wrapped call, microseconds (it still returns at least one frame)")
if(ISAAC_VITA_NATIVE_VORBIS_ASYNC AND
   (NOT ISAAC_VITA_NATIVE_VORBIS OR NOT ISAAC_VITA_NATIVE_VORBIS_WORKER))
  message(FATAL_ERROR
    "ISAAC_VITA_NATIVE_VORBIS_ASYNC requires ISAAC_VITA_NATIVE_VORBIS=ON and ISAAC_VITA_NATIVE_VORBIS_WORKER=ON")
endif()
if(ISAAC_VITA_NATIVE_VORBIS_ASYNC AND ISAAC_VITA_NATIVE_VORBIS_VERIFY)
  message(FATAL_ERROR
    "ISAAC_VITA_NATIVE_VORBIS_ASYNC serves partial slots; VERIFY compares per-call frame counts against the translated decoder -- run VERIFY with ASYNC=OFF")
endif()
# Pasted as C literals with a U suffix: whole numbers, no leading zero.
if(NOT ISAAC_VITA_NATIVE_VORBIS_MIN_FRAMES MATCHES "^[1-9][0-9]*$" OR
   ISAAC_VITA_NATIVE_VORBIS_MIN_FRAMES GREATER 16384)
  message(FATAL_ERROR
    "ISAAC_VITA_NATIVE_VORBIS_MIN_FRAMES must be a whole number of frames in 1..16384 (no leading zero)")
endif()
if(NOT ISAAC_VITA_NATIVE_VORBIS_SYNC_BUDGET_US MATCHES "^[1-9][0-9]*$" OR
   ISAAC_VITA_NATIVE_VORBIS_SYNC_BUDGET_US GREATER 200000)
  message(FATAL_ERROR
    "ISAAC_VITA_NATIVE_VORBIS_SYNC_BUDGET_US must be a whole number of microseconds in 1..200000 (no leading zero)")
endif()
option(ISAAC_VITA_NATIVE_PNG
       "Replace the translated libpng row decode (png_read_row 0x5b1500) of every 8-bit non-interlaced sprite sheet with a native whole-image inflate + unfilter into a host scratch; the guest's own libpng 1.0.2 still parses headers, palettes and the trailer (requires ISAAC_VITA_KAGE)" OFF)
option(ISAAC_VITA_NATIVE_PNG_RECEIPT
       "Log bounded native PNG receipts: per-image dimensions/colour type/chunks/filters/microseconds and periodic STATS lines" OFF)
option(ISAAC_VITA_NATIVE_PNG_VERIFY
       "Also run the translated png_read_row for every row, compare rows and the final png_struct/stream state, log mismatches and ship the translated bytes (slow; proof build)" OFF)
if(ISAAC_VITA_NATIVE_PNG AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))
  message(FATAL_ERROR
    "ISAAC_VITA_NATIVE_PNG requires the translated KAGE runtime")
endif()
if((ISAAC_VITA_NATIVE_PNG_RECEIPT OR ISAAC_VITA_NATIVE_PNG_VERIFY) AND
   NOT ISAAC_VITA_NATIVE_PNG)
  message(FATAL_ERROR
    "ISAAC_VITA_NATIVE_PNG_RECEIPT/VERIFY require ISAAC_VITA_NATIVE_PNG=ON")
endif()
option(ISAAC_VITA_NATIVE_INFLATE
       "Run the frozen zlib 1.1.4 inflate_codes leaf (sub_005d6d80, the hot inner decoder + inflate_fast + inflate_flush) through the identical vendored native 1.1.4 core in place on the guest inflate state; accelerates the translated-inflate share of room/stage transitions (the over-reserve PNG fallbacks and ancillary chunks the native PNG seam does not serve). Requires ISAAC_VITA_KAGE" OFF)
if(ISAAC_VITA_NATIVE_INFLATE AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))
  message(FATAL_ERROR
    "ISAAC_VITA_NATIVE_INFLATE requires the translated KAGE runtime")
endif()
set(ISAAC_VITA_NATIVE_PNG_RESERVE_MB 16 CACHE STRING
    "MiB of USER_RW the native PNG decoder reserves for its whole-image scratch before vitaGL init (+64 KiB of filter bytes; 16 holds 2048x2048 RGBA, 12 holds 1536x2048, 8 holds 1024x2048; 0 = per-image blocks only, which the vitaGL RAM pool starves after init)")
if(NOT ISAAC_VITA_NATIVE_PNG_RESERVE_MB MATCHES "^[0-9]+$" OR
   ISAAC_VITA_NATIVE_PNG_RESERVE_MB GREATER 31)
  message(FATAL_ERROR
    "ISAAC_VITA_NATIVE_PNG_RESERVE_MB must be an integer in 0..31: ${ISAAC_VITA_NATIVE_PNG_RESERVE_MB}")
endif()
option(ISAAC_VITA_LUA
       "Enable the experimental pinned Lua 5.3.3 runtime bridge" OFF)
set(ISAAC_LUA53_SOURCE_DIR "" CACHE PATH
    "Extracted pristine lua-5.3.3 source root (used only with ISAAC_VITA_LUA)")
if(ISAAC_VITA_LUA AND ISAAC_VITA_SCAFFOLD_ONLY)
  message(FATAL_ERROR "ISAAC_VITA_LUA requires the generated guest runtime")
endif()
option(ISAAC_VITA_LUA_OFF_CLOSE_NOOP
       "With ISAAC_VITA_LUA=OFF, let the game's shutdown lua_close return instead of faulting the process exit" OFF)
if(ISAAC_VITA_LUA_OFF_CLOSE_NOOP AND ISAAC_VITA_LUA)
  message(FATAL_ERROR
    "ISAAC_VITA_LUA_OFF_CLOSE_NOOP applies only to ISAAC_VITA_LUA=OFF builds")
endif()
if(ISAAC_VITA_LUA_OFF_CLOSE_NOOP AND ISAAC_VITA_SCAFFOLD_ONLY)
  message(FATAL_ERROR
    "ISAAC_VITA_LUA_OFF_CLOSE_NOOP requires the generated guest runtime")
endif()
option(ISAAC_VITA_LUA_RECEIPT
       "Log bounded [isaac-lua] receipts: every Lua chunk load, every failed pcall, every mod-tree _access probe" OFF)
option(ISAAC_VITA_LUA_SCOPE_FASTPATH
       "Lua bridge fixed-overhead trims (behaviour-identical): relaxed re-entrant scope_enter before the CAS, field-wise callback frame init, no redundant trampoline re-checks; OFF compiles the pre-knob code byte-identically (bisect escape hatch)" ON)
set(ISAAC_VITA_LUA_GCSTEP_CLAMP_KB 128 CACHE STRING
    "Cap the per-frame lua_gc(LUA_GCSTEP) step LuaEngine::Update requests (KB); 0 = game's adaptive step (up to 10,000 KB, a full heap mark per frame with a mod loaded)")
if(NOT ISAAC_VITA_LUA_GCSTEP_CLAMP_KB MATCHES "^[0-9]+$" OR
   ISAAC_VITA_LUA_GCSTEP_CLAMP_KB GREATER 10000)
  message(FATAL_ERROR
    "ISAAC_VITA_LUA_GCSTEP_CLAMP_KB must be an integer in 0..10000: "
    "${ISAAC_VITA_LUA_GCSTEP_CLAMP_KB}")
endif()
set(ISAAC_VITA_LUA_ARENA_MB 0 CACHE STRING
    "Serve the LuaEngine state from a private USER_RW mspace of this many MiB, reserved before vitaGL init (0 keeps the guest heap)")
if(ISAAC_VITA_LUA_RECEIPT AND NOT ISAAC_VITA_LUA)
  message(FATAL_ERROR "ISAAC_VITA_LUA_RECEIPT requires ISAAC_VITA_LUA=ON")
endif()
if(NOT ISAAC_VITA_LUA_ARENA_MB MATCHES "^[0-9]+$" OR
   ISAAC_VITA_LUA_ARENA_MB GREATER 96)
  message(FATAL_ERROR
    "ISAAC_VITA_LUA_ARENA_MB must be an integer 0..96 (got '${ISAAC_VITA_LUA_ARENA_MB}')")
endif()
option(ISAAC_VITA_LUA_NATIVE_INDEX
       "Replay LuaBridge's CFunc::indexMetaMethod (sub_003f95c0, the __index of every LuaBridge class/namespace) natively in the Lua callback trampoline with the translated body's exact C-API sequence; any deviation restores the entry stack and runs the translated body.  OFF keeps host_vita_lua.c byte-identical; enable after a NATIVE_INDEX_VERIFY device run logs mismatch=0" OFF)
option(ISAAC_VITA_LUA_NATIVE_INDEX_VERIFY
       "With ISAAC_VITA_LUA_NATIVE_INDEX: re-run the translated indexMetaMethod after the first 1000 native hits and every 4096th, compare the whole Lua stack residue, log '[isaac-lua] native-index verify mismatch' and disable the replay on the first mismatch" OFF)
if(ISAAC_VITA_LUA_NATIVE_INDEX AND NOT ISAAC_VITA_LUA)
  message(FATAL_ERROR "ISAAC_VITA_LUA_NATIVE_INDEX requires ISAAC_VITA_LUA=ON")
endif()
if(ISAAC_VITA_LUA_NATIVE_INDEX_VERIFY AND NOT ISAAC_VITA_LUA_NATIVE_INDEX)
  message(FATAL_ERROR
    "ISAAC_VITA_LUA_NATIVE_INDEX_VERIFY requires ISAAC_VITA_LUA_NATIVE_INDEX=ON")
endif()
if(ISAAC_VITA_LUA_ARENA_MB GREATER 0 AND NOT ISAAC_VITA_LUA)
  message(FATAL_ERROR "ISAAC_VITA_LUA_ARENA_MB requires ISAAC_VITA_LUA=ON")
endif()
# Typed direct endpoints for the thirteen hottest Lua C-API imports
# (lua_rawgetp/getmetatable/type/touserdata/pushvalue/rawget/rawgeti/settop/
# gettop/pushnil/pushinteger/pushnumber/pushstring; the LuaBridge getClass
# and callback-invoker family, ~2.0 ms/frame of host time with EID in the
# perf:wf-prof-v6 sampler).  One stack-range check, plain argument loads,
# plain owner word, one indirect call from guest_import_call; every other
# Lua import and every rejected stack keeps the generic handler.  Owners:
# host_vita_lua.c and host_vita_import_id.c only; generated units, guest.c
# and the corpus are byte-identical.  Default OFF; enable in perf builds
# (-DISAAC_VITA_LUA_IMPORT_FASTDISPATCH=ON) after the device A/B by relink.
option(ISAAC_VITA_LUA_IMPORT_FASTDISPATCH
       "Typed direct endpoints for the 13 hottest Lua C-API imports (needs ISAAC_VITA_LUA)" OFF)
if(ISAAC_VITA_LUA_IMPORT_FASTDISPATCH AND NOT ISAAC_VITA_LUA)
  message(FATAL_ERROR
    "ISAAC_VITA_LUA_IMPORT_FASTDISPATCH requires ISAAC_VITA_LUA=ON")
endif()
option(ISAAC_VITA_LUA_GCCOLLECT_CLAMP
       "Replace the Level-init family's per-floor lua_gc(LUA_GCCOLLECT) (return word 0x3b5870, shared by the seven sub_003b0e80 fragments; 130-286 ms of full mark/sweep/finalizers per transition with EID) with one bounded GCSTEP of ISAAC_VITA_LUA_GCCOLLECT_STEP_KB; RunScript's mod-load collection and every GCCOUNT query are untouched (same behaviour class as ISAAC_VITA_LUA_GCSTEP_CLAMP_KB)" OFF)
set(ISAAC_VITA_LUA_GCCOLLECT_STEP_KB 256 CACHE STRING
    "ISAAC_VITA_LUA_GCCOLLECT_CLAMP: KB of collector debt paid at each clamped floor-transition collection (0 = skip the collection entirely, the paced Update steps catch up)")
option(ISAAC_VITA_LUA_GC_PROFILE
       "Log one bounded [isaac-lua] gc window line per 120 LuaEngine::Update GC steps: step n/sum/p95/max us, __gc finalizers run and worst per step, GCCOUNT, arena live/peak/fallbacks, requested vs applied KB, GCCOLLECT clamp/full counts" OFF)
if(ISAAC_VITA_LUA_GCCOLLECT_CLAMP AND NOT ISAAC_VITA_LUA)
  message(FATAL_ERROR
    "ISAAC_VITA_LUA_GCCOLLECT_CLAMP requires ISAAC_VITA_LUA=ON")
endif()
if(ISAAC_VITA_LUA_GC_PROFILE AND NOT ISAAC_VITA_LUA)
  message(FATAL_ERROR "ISAAC_VITA_LUA_GC_PROFILE requires ISAAC_VITA_LUA=ON")
endif()
if(NOT ISAAC_VITA_LUA_GCCOLLECT_STEP_KB MATCHES "^[0-9]+$" OR
   ISAAC_VITA_LUA_GCCOLLECT_STEP_KB GREATER 10000)
  message(FATAL_ERROR
    "ISAAC_VITA_LUA_GCCOLLECT_STEP_KB must be an integer in 0..10000: "
    "${ISAAC_VITA_LUA_GCCOLLECT_STEP_KB}")
endif()
option(ISAAC_VITA_LUA_NATIVE_GETCLASS
       "Serve the frozen Userdata::getClass (sub_003f8e30; 917 call sites, ~14 Lua import crossings per call) and Userdata::getExact (sub_003f8c80, the __gc handler chain) natively: a link-time --wrap seam replays the happy path with direct Lua 5.3.3 API calls under the bridge owner and hands every error / const violation / odd metatable to the translated body before any side effect, so error texts are unchanged.  The elided import crossings are counted in the seam stats, not replayed into the ph120 import census.  Requires ISAAC_VITA_LUA; OFF keeps every object byte-identical" OFF)
option(ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY
       "Also run the translated getClass/getExact after the first 64 native hits and every 4096th, log eax/esp/stack-top mismatches and ship the translated result (proof build)" OFF)
if((ISAAC_VITA_LUA_NATIVE_GETCLASS OR ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY) AND
   NOT ISAAC_VITA_LUA)
  message(FATAL_ERROR
    "ISAAC_VITA_LUA_NATIVE_GETCLASS requires ISAAC_VITA_LUA=ON")
endif()
if(ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY AND NOT ISAAC_VITA_LUA_NATIVE_GETCLASS)
  message(FATAL_ERROR
    "ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY requires ISAAC_VITA_LUA_NATIVE_GETCLASS=ON")
endif()
option(ISAAC_VITA_OPENAL_POOL
       "Route direct OpenAL C allocations through a dedicated USER_RW mspace" ON)
option(ISAAC_VITA_IO_PROFILE
       "Profile native sceIoRead/sceIoLseek32/sceIoLseek during startup" OFF)
option(ISAAC_VITA_ARCHIVE_FILE_CACHE
       "Reuse one idle read-only packed-archive FILE below the guest" OFF)
option(ISAAC_VITA_ARCHIVE_VALIDATION_SKIP
       "Skip only PE-baked packed-entry checksum matches during startup" OFF)
option(ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT
       "Reuse a versioned exact-corpus receipt for animations/afterbirth validation" OFF)
if(ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT AND
   NOT ISAAC_VITA_ARCHIVE_VALIDATION_SKIP)
  message(FATAL_ERROR
    "ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT requires ISAAC_VITA_ARCHIVE_VALIDATION_SKIP=ON")
endif()
option(ISAAC_VITA_FIOS_CACHE
       "Use a bounded fail-open FIOS2 RAM cache for the Isaac data root" OFF)
if(ISAAC_VITA_FIOS_CACHE AND ISAAC_VITA_SCAFFOLD_ONLY)
  message(FATAL_ERROR
    "ISAAC_VITA_FIOS_CACHE requires the real Vita runtime")
endif()
option(ISAAC_VITA_ASYNC_SAVE_WRITE
       "Write binary Documents/*.dat saves from a worker thread as one image" OFF)
if(ISAAC_VITA_ASYNC_SAVE_WRITE AND NOT ISAAC_VITA_ARCHIVE_FILE_CACHE)
  message(FATAL_ERROR
    "ISAAC_VITA_ASYNC_SAVE_WRITE requires ISAAC_VITA_ARCHIVE_FILE_CACHE=ON")
endif()
if(ISAAC_VITA_ASYNC_SAVE_WRITE AND ISAAC_VITA_SCAFFOLD_ONLY)
  message(FATAL_ERROR
    "ISAAC_VITA_ASYNC_SAVE_WRITE requires the real Vita runtime")
endif()
option(ISAAC_VITA_CRT_SEEK_SHADOW
       "Elide read-only SEEK_END/ftell pairs on newlib FILE tokens with a cached fstat size" OFF)
if(ISAAC_VITA_CRT_SEEK_SHADOW AND NOT ISAAC_VITA_ARCHIVE_FILE_CACHE)
  message(FATAL_ERROR
    "ISAAC_VITA_CRT_SEEK_SHADOW requires ISAAC_VITA_ARCHIVE_FILE_CACHE=ON")
endif()
option(ISAAC_VITA_VANILLA_KAGE_BUNDLE
       "Enable frozen Godhead and KAGE RenderBatch correctness fixes" OFF)
option(ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE
       "Use the common caustics/shockwave/filter settings from PCSB00676" OFF)
option(ISAAC_VITA_RENDERFRAME_FASTPATH
       "Borrow synchronized RenderFrame images without SmartPointer churn" OFF)
option(ISAAC_VITA_FXRAY_ALPHA_MASK
       "Store exact white 256x512 FXLayers ray images as alpha-only textures" OFF)
option(ISAAC_VITA_PILL_BLOOM_BYPASS
       "Performance mode: omit shared transient Bloom capture/composite (pill trigger)" OFF)
option(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES
       "Performance mode: render only transient Bloom at half width/height" OFF)
option(ISAAC_VITA_LASER_PROFILE
       "Profile exact active Entity_Laser Render/Shadow CPU and draw cost" OFF)
option(ISAAC_VITA_POOP_FX_PROFILE
       "Attribute exact AddPoopFx callers and active foreground cloud count" OFF)
option(ISAAC_VITA_PNG_DECODE_PROFILE
       "Profile an aligned 1/32 whole-image PNG/filter/inflate cohort" OFF)
option(ISAAC_VITA_PNG_NATIVE_UNFILTER
       "A/B the frozen png_read_filter_row target through guarded native code" OFF)
option(ISAAC_VITA_PNG_INFLATE_FLUSH_FASTPATH
       "Run the frozen zlib 1.1.4 inflate_flush leaf through guarded native code" OFF)
option(ISAAC_VITA_PNG_CRC32_FASTPATH
       "Run the frozen bulk PNG zlib CRC-32 leaf through native code" OFF)
option(ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH
       "Run the frozen miniz v1.15 archive coroutine through guarded native code" OFF)
option(ISAAC_VITA_SAVE_CHECKSUM_FASTPATH
       "Feed the frozen save checksum through exact guarded native code" OFF)
option(ISAAC_VITA_SAVE_READER_FASTPATH
       "Read the frozen memory Save Reader leaf through guarded native code" OFF)
option(ISAAC_VITA_SAVE_READ32_FUSED
       "Fuse the frozen scalar Save Reader/checksum dispatch wrapper" OFF)
option(ISAAC_VITA_SAVE_READER_DIRECT_EDGES
       "Direct-dispatch 326 authenticated Save Reader parser edges" OFF)
option(ISAAC_VITA_MEMSET_THUNK_FASTPATH
       "Direct-dispatch the authenticated 350-caller memset IAT thunk" OFF)
option(ISAAC_VITA_KAGE_MUTEX_SEAM
       "Complete the frozen KAGE Mutex::Lock(-1)/Unlock wrappers natively on the inline critical-section fast path (exact translated fallback for every other state)" OFF)
option(ISAAC_VITA_FLOOR_THUNK_FASTPATH
       "Execute the authenticated 438-caller floor IAT thunk inline at its root (needs a corpus generated with GUEST_FLOOR_THUNK_FASTPATH=1)" OFF)
option(ISAAC_VITA_SHADER_ATTRIB_FASTPATH
       "Replay the two authenticated Shader::EnableAttribs/DisableAttribs bodies into the typed GL wrappers on the host (needs GL_SHIM_FASTDISPATCH)" OFF)
option(ISAAC_VITA_POOP_FX_SINGLE_CLOUD
       "Visual performance mode: render one of the three PoopFx clouds" OFF)
option(ISAAC_VITA_STALL_PROBE
       "Enable the diagnostic guest-stall watchdog and breadcrumbs" OFF)
option(ISAAC_VITA_STAGE_HEARTBEAT
       "Log early/power-of-two and periodic render-stage progress" OFF)
# 2026-09-04 device evidence: the 14-31 record lines at the end of every
# 120-loop ph120 window (sceClibPrintf = kernel debug printf captured over
# the network) plus the per-event isaac_vita_log sceIoWrite lines stalled
# the game thread 60-100 ms every 2 s outside every profiled phase.  ON
# queues every line (exact bytes, program order) into one bounded ring that
# a lowest-priority logger thread on CPU 2 drains.  Perf/profiling builds
# should set this ON; the default stays OFF so every unrouted object is
# byte-identical.
option(ISAAC_VITA_LOG_ASYNC
       "Hand isaac_vita_log lines and per-window [kage-vita] records to one logger thread (priority 191, CPU 2) through a bounded ring instead of writing them synchronously from the producer (perf builds: ON)" OFF)
set(ISAAC_VITA_LOG_ASYNC_RING_KB 256 CACHE STRING
    "ISAAC_VITA_LOG_ASYNC ring capacity in KiB; a full ring drops and counts the line ([kage-vita] log-async dropped=N)")
option(ISAAC_VITA_PHASE_PROFILE
       "Aggregate loop timings plus dispatch/GL counts every 120 loops" OFF)
option(ISAAC_VITA_GL_TIME_PROFILE
       "Split Game::Render wall time into native GL call buckets and GXM scene waits (ph120.gt)" OFF)
option(ISAAC_VITA_GUEST_SAMPLER
       "Sample the guest return-address chain from a second core every 1 ms and print per-phase hot translated functions (ph120.hot)" OFF)
# The import-kinds census reads the one per-import array guest.c keeps for
# the phase profiler (guest.h g_guest_phase_profile_import_calls, also the
# dispatch table's ph120.i source).  Without ISAAC_VITA_DISPATCH_TABLE that
# array and its bounded indexed increment on every host import route (~7k per
# frame) exist only because of this option, and the scheduler gains its pace
# accounting either way, so a perf A/B build with PHASE_PROFILE=ON must keep
# the reference guest_call/scheduler bodies: the default follows the sampler
# (ON for a profiling build, OFF otherwise).  Like every option() the cached
# value wins on reconfigure.
if(ISAAC_VITA_GUEST_SAMPLER)
  set(ISAAC_VITA_PROFILE_IMPORT_KINDS_DEFAULT ON)
else()
  set(ISAAC_VITA_PROFILE_IMPORT_KINDS_DEFAULT OFF)
endif()
option(ISAAC_VITA_PROFILE_IMPORT_KINDS
       "Per-binding-kind import census, fourteen named hot imports and the g(c)=g(l)+imports+dynamic identity gate as ph120.ik/ph120.ih (active only with ISAAC_VITA_PHASE_PROFILE; default follows ISAAC_VITA_GUEST_SAMPLER)"
       ${ISAAC_VITA_PROFILE_IMPORT_KINDS_DEFAULT})
option(ISAAC_VITA_PROFILE_FUNCTION_ENTRIES
       "Count translated body entries (direct+tail+indirect) as ph120.ik ent= (recompiles every generated unit; needs ISAAC_VITA_PHASE_PROFILE)" OFF)
option(ISAAC_VITA_SIM_CADENCE_RECEIPT
       "Log real Game::Update cadence beside the 120-present heartbeat" OFF)
option(ISAAC_VITA_WORLD_SEAM_DIAG
       "Bounded Room/Lua/late-overlay GL-state receipt for Utero II" OFF)
option(ISAAC_VITA_TEXTURE_CHURN_PROFILE
       "Attribute texture-name scans, lifetimes and upload paths per phase window" OFF)
option(ISAAC_VITA_CONTINUE_PROFILE
       "Aggregate FILE-selection and Continue stage timings (diagnostic)" OFF)
option(ISAAC_VITA_EXIT_MENU_PROFILE
       "Attribute up to four qualified gameplay-to-menu rebuilds" OFF)
option(ISAAC_VITA_GAME_LOG_BATCH
       "Flush the exact guest INFO logger every eight records" OFF)
option(ISAAC_VITA_CONTINUE_OVERLAY
       "Show a coarse loading-status overlay during profiled FILE/Continue" OFF)
if(ISAAC_VITA_CONTINUE_OVERLAY AND NOT ISAAC_VITA_CONTINUE_PROFILE)
  message(FATAL_ERROR
    "ISAAC_VITA_CONTINUE_OVERLAY requires ISAAC_VITA_CONTINUE_PROFILE=ON")
endif()
if(ISAAC_VITA_CONTINUE_OVERLAY AND NOT ISAAC_VITA_KAGE)
  message(FATAL_ERROR
    "ISAAC_VITA_CONTINUE_OVERLAY requires ISAAC_VITA_KAGE=ON")
endif()
if(ISAAC_VITA_CONTINUE_PROFILE AND ISAAC_VITA_SCAFFOLD_ONLY)
  message(FATAL_ERROR
    "ISAAC_VITA_CONTINUE_PROFILE requires the generated guest runtime")
endif()
if(ISAAC_VITA_EXIT_MENU_PROFILE AND ISAAC_VITA_SCAFFOLD_ONLY)
  message(FATAL_ERROR
    "ISAAC_VITA_EXIT_MENU_PROFILE requires the generated guest runtime")
endif()
if(ISAAC_VITA_SAVE_CHECKSUM_FASTPATH AND ISAAC_VITA_SCAFFOLD_ONLY)
  message(FATAL_ERROR
    "ISAAC_VITA_SAVE_CHECKSUM_FASTPATH requires the generated guest runtime")
endif()
if(ISAAC_VITA_SAVE_READER_FASTPATH AND ISAAC_VITA_SCAFFOLD_ONLY)
  message(FATAL_ERROR
    "ISAAC_VITA_SAVE_READER_FASTPATH requires the generated guest runtime")
endif()
if(ISAAC_VITA_SAVE_READ32_FUSED AND ISAAC_VITA_SCAFFOLD_ONLY)
  message(FATAL_ERROR
    "ISAAC_VITA_SAVE_READ32_FUSED requires the generated guest runtime")
endif()
if(ISAAC_VITA_SAVE_READ32_FUSED AND
   (NOT ISAAC_VITA_SAVE_READER_FASTPATH OR
    NOT ISAAC_VITA_SAVE_CHECKSUM_FASTPATH))
  message(FATAL_ERROR
    "ISAAC_VITA_SAVE_READ32_FUSED requires both save fast paths")
endif()
if(ISAAC_VITA_SAVE_READER_DIRECT_EDGES AND ISAAC_VITA_SCAFFOLD_ONLY)
  message(FATAL_ERROR
    "ISAAC_VITA_SAVE_READER_DIRECT_EDGES requires generated guest runtime")
endif()
if(ISAAC_VITA_SAVE_READER_DIRECT_EDGES AND
   NOT ISAAC_VITA_SAVE_READER_FASTPATH)
  message(FATAL_ERROR
    "ISAAC_VITA_SAVE_READER_DIRECT_EDGES requires save-reader fast path")
endif()
if(ISAAC_VITA_MEMSET_THUNK_FASTPATH AND ISAAC_VITA_SCAFFOLD_ONLY)
  message(FATAL_ERROR
    "ISAAC_VITA_MEMSET_THUNK_FASTPATH requires generated guest runtime")
endif()
if(ISAAC_VITA_KAGE_MUTEX_SEAM AND ISAAC_VITA_SCAFFOLD_ONLY)
  message(FATAL_ERROR
    "ISAAC_VITA_KAGE_MUTEX_SEAM requires generated guest runtime")
endif()
if(ISAAC_VITA_KAGE_MUTEX_SEAM AND NOT ISAAC_VITA_KAGE)
  # The seam TU mirrors the phase-profile/dispatch-table census definitions
  # guest.c only receives under ISAAC_VITA_KAGE; without KAGE it would
  # reference counters guest.c never defines (a link error) -- fail at configure.
  message(FATAL_ERROR
    "ISAAC_VITA_KAGE_MUTEX_SEAM requires ISAAC_VITA_KAGE=ON")
endif()
if(ISAAC_VITA_FLOOR_THUNK_FASTPATH AND ISAAC_VITA_SCAFFOLD_ONLY)
  message(FATAL_ERROR
    "ISAAC_VITA_FLOOR_THUNK_FASTPATH requires generated guest runtime")
endif()
if(ISAAC_VITA_SHADER_ATTRIB_FASTPATH AND ISAAC_VITA_SCAFFOLD_ONLY)
  message(FATAL_ERROR
    "ISAAC_VITA_SHADER_ATTRIB_FASTPATH requires generated guest runtime")
endif()
if(ISAAC_VITA_SHADER_ATTRIB_FASTPATH AND NOT ISAAC_VITA_GL_SHIM_FASTDISPATCH)
  message(FATAL_ERROR
    "ISAAC_VITA_SHADER_ATTRIB_FASTPATH requires ISAAC_VITA_GL_SHIM_FASTDISPATCH")
endif()
if(ISAAC_VITA_GAME_LOG_BATCH AND ISAAC_VITA_SCAFFOLD_ONLY)
  message(FATAL_ERROR
    "ISAAC_VITA_GAME_LOG_BATCH requires the generated guest runtime")
endif()
option(ISAAC_VITA_FULLSPEED_SCHEDULER
       "Run the outer wrapper at 60 Hz and render only completed 30-Hz phases" OFF)
if(ISAAC_VITA_FULLSPEED_SCHEDULER AND NOT ISAAC_VITA_KAGE)
  message(FATAL_ERROR
    "ISAAC_VITA_FULLSPEED_SCHEDULER requires ISAAC_VITA_KAGE=ON")
endif()
option(ISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES
       "Accept the two hardware-observed Exit-to-menu zero-frame Game::Update returns instead of disabling the 60-Hz cadence" OFF)
if(ISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES AND
   NOT ISAAC_VITA_FULLSPEED_SCHEDULER)
  message(FATAL_ERROR
    "ISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES requires ISAAC_VITA_FULLSPEED_SCHEDULER=ON")
endif()
option(ISAAC_VITA_STABLE_30_PRESENTATION
       "Present only completed 30-Hz game phases and suppress interpolation" OFF)
if(ISAAC_VITA_STABLE_30_PRESENTATION AND NOT ISAAC_VITA_KAGE)
  message(FATAL_ERROR
    "ISAAC_VITA_STABLE_30_PRESENTATION requires ISAAC_VITA_KAGE=ON")
endif()
if(ISAAC_VITA_STABLE_30_PRESENTATION AND ISAAC_VITA_SCAFFOLD_ONLY)
  message(FATAL_ERROR
    "ISAAC_VITA_STABLE_30_PRESENTATION requires generated guest code")
endif()
if(ISAAC_VITA_STABLE_30_PRESENTATION AND ISAAC_VITA_FULLSPEED_SCHEDULER)
  message(FATAL_ERROR
    "stable 30-Hz presentation and the fullspeed scheduler are exclusive")
endif()
if(ISAAC_VITA_FXRAY_ALPHA_MASK AND NOT ISAAC_VITA_KAGE)
  message(FATAL_ERROR
    "ISAAC_VITA_FXRAY_ALPHA_MASK requires the translated KAGE runtime")
endif()
option(ISAAC_VITA_GUEST_LOOKUP_CACHE
       "Cache exact guest lookups, confirmed dispatches and the dense IAT" OFF)
option(ISAAC_VITA_SYNC_IMPORT_FASTPATH
       "Route validated Enter/LeaveCriticalSection IDs directly to sync" OFF)
option(ISAAC_VITA_DISPATCH_TABLE
       "Boot-built O(1) raw-VA dispatch table probed inline by guest_call" OFF)
if(ISAAC_VITA_SYNC_IMPORT_FASTPATH AND ISAAC_VITA_SCAFFOLD_ONLY)
  message(FATAL_ERROR
    "ISAAC_VITA_SYNC_IMPORT_FASTPATH requires the generated guest runtime")
endif()
option(ISAAC_VITA_SYNC_INLINE_FASTPATH
       "Complete single-owner Enter/LeaveCriticalSection success states inline (no CAS/dmb/stack validators); other states keep the registered endpoint" OFF)
if(ISAAC_VITA_SYNC_INLINE_FASTPATH AND NOT ISAAC_VITA_SYNC_IMPORT_FASTPATH)
  message(FATAL_ERROR
    "ISAAC_VITA_SYNC_INLINE_FASTPATH requires ISAAC_VITA_SYNC_IMPORT_FASTPATH")
endif()
if(ISAAC_VITA_KAGE_MUTEX_SEAM AND NOT ISAAC_VITA_SYNC_INLINE_FASTPATH)
  message(FATAL_ERROR
    "ISAAC_VITA_KAGE_MUTEX_SEAM requires ISAAC_VITA_SYNC_INLINE_FASTPATH")
endif()
option(ISAAC_VITA_IAT_DIRECT
       "Generated call/jmp [IAT] and IAT-loaded call-reg sites enter the validated import family endpoint directly (GUEST_IMPORT_DIRECT=1, guest_import_call); any other slot value keeps guest_call" OFF)
if(ISAAC_VITA_IAT_DIRECT AND ISAAC_VITA_SCAFFOLD_ONLY)
  message(FATAL_ERROR
    "ISAAC_VITA_IAT_DIRECT requires the generated guest runtime")
endif()
option(ISAAC_VITA_GL_REDUNDANCY_CACHE
       "Suppress proven program binds and exact scalar uniform writes" OFF)
option(ISAAC_VITA_GL_TYPED_STATE_CACHE
       "Suppress exact-valid duplicate typed GL state requests" OFF)
# GL shim dispatch trims (wf/opt-glshim, research-gl-shim-path findings 1/3):
# probe the 0x7e typed-GL family before the dispatch cache and IAT search,
# replace the 73-entry linear token scan and 73-case switch with the generated
# direct-mapped token index, and replace the two CAS loops + four dmb per GL
# call with a plain single-thread owner word pinned to the first dispatching
# CPU (any other CPU faults).  The location cache memoizes
# glGetAttribLocation/glGetUniformLocation per (program, generation, name
# bytes); every registry mutator of program/shader state bumps the generation.
# It costs 21.5 KiB of BSS and stays OFF until one device run with
# ISAAC_VITA_GL_LOCATION_CACHE_VERIFY=ON has logged ph120.a loc(...,m)=0
# (every hit shadowed by the native query, zero disagreements).
option(ISAAC_VITA_GL_SHIM_FASTDISPATCH
       "O(1) typed-GL token dispatch, 0x7e-first probe, plain GL owner word" OFF)
option(ISAAC_VITA_GL_LOCATION_CACHE
       "Memoize glGetAttribLocation/glGetUniformLocation in the GL shim (needs GL_SHIM_FASTDISPATCH; enable after a VERIFY device run)" OFF)
option(ISAAC_VITA_GL_LOCATION_CACHE_VERIFY
       "Device A/B: shadow every location-cache hit with a native query and count mismatches into ph120.a loc(...,m)" OFF)
# GL shim per-crossing cost (wf/opt-gltok).  TABLE_TOKENS registers the 73
# typed-GL tokens in the boot-built dispatch table (gl_bridge.c trampolines
# count the dynamic call and run the owned adapter), so a GL call is
# probe -> trampoline -> owner check -> adapter instead of probe miss ->
# guest_call_slow -> isaac_vita_gl_dynamic_counted -> guest_gl_dispatch_counted
# -> adapter; tokens are inserted only while no IAT slot key and no image
# window can alias the 0x7e family (the slow block's own g_gl_dynamic_first).
# RAW_ARGS makes the generated adapters validate the whole stdcall frame once
# inline, read the arguments with plain loads and retire the frame with one
# bounds-checked esp update; a frame the check rejects takes the legacy
# per-word validators and faults exactly where they fault.  Both default OFF;
# perf builds pass both ON.  Census: ph120.d gains the raw token-hit count as
# d(c,s,sf,g) (s no longer includes GL tokens; c - s - g = translated hits);
# ph120.c g(c,l,i), ph120.ik (dyn included) and ph120.i are unchanged.
option(ISAAC_VITA_GL_SHIM_TABLE_TOKENS
       "Register the typed-GL tokens in the dispatch table so guest_call's inline probe reaches the GL adapter directly (needs GL_SHIM_FASTDISPATCH and DISPATCH_TABLE)" OFF)
option(ISAAC_VITA_GL_SHIM_RAW_ARGS
       "Generated GL adapters validate the whole stdcall frame once and read the arguments with plain loads (needs GL_SHIM_FASTDISPATCH and DISPATCH_TABLE)" OFF)
if(ISAAC_VITA_GL_SHIM_TABLE_TOKENS AND
   NOT (ISAAC_VITA_GL_SHIM_FASTDISPATCH AND ISAAC_VITA_DISPATCH_TABLE))
  message(FATAL_ERROR
    "ISAAC_VITA_GL_SHIM_TABLE_TOKENS requires ISAAC_VITA_GL_SHIM_FASTDISPATCH and ISAAC_VITA_DISPATCH_TABLE")
endif()
if(ISAAC_VITA_GL_SHIM_RAW_ARGS AND
   NOT (ISAAC_VITA_GL_SHIM_FASTDISPATCH AND ISAAC_VITA_DISPATCH_TABLE))
  message(FATAL_ERROR
    "ISAAC_VITA_GL_SHIM_RAW_ARGS requires ISAAC_VITA_GL_SHIM_FASTDISPATCH and ISAAC_VITA_DISPATCH_TABLE")
endif()
# Display/default-FBO raster.  Render cost tracks pixel count (Bundle35: the
# same rooms cost 22 ms at 960x544 and 16 ms at 720x408), so the scanout is a
# tunable while the guest keeps its byte-identical 960x540 logical commands.
# 960 = native panel, 720 = 720x408 (3/4), 480 = 480x272 (1/2).  Every scaled
# raster keeps a centred viewport of 960x540 scaled by the same ratio.
set(ISAAC_VITA_DISPLAY_RASTER "960" CACHE STRING
    "Default framebuffer raster width: 960 (native), 720 (720x408) or 480 (480x272)")
set_property(CACHE ISAAC_VITA_DISPLAY_RASTER PROPERTY STRINGS 960 720 480)
# Legacy boolean alias.  ON with RASTER=960 selects 720; any scaled RASTER
# forces it ON so downstream consumers (the raw allocator gate reads this
# cache key as "scaled raster active") keep one truthful ON/OFF view.
option(ISAAC_VITA_DISPLAY_RASTER_720
       "Legacy alias: render the default framebuffer at 720x408 while retaining 960x540 guest coordinates" OFF)
if(ISAAC_VITA_DISPLAY_RASTER_720 AND ISAAC_VITA_DISPLAY_RASTER STREQUAL "960")
  set(ISAAC_VITA_DISPLAY_RASTER "720" CACHE STRING
      "Default framebuffer raster width: 960 (native), 720 (720x408) or 480 (480x272)"
      FORCE)
endif()
if(ISAAC_VITA_DISPLAY_RASTER STREQUAL "960")
  set(ISAAC_VITA_DISPLAY_RASTER_SCALED OFF)
  set(ISAAC_VITA_DISPLAY_RASTER_WIDTH 960)
  set(ISAAC_VITA_DISPLAY_RASTER_HEIGHT 544)
  set(ISAAC_VITA_DISPLAY_RASTER_NUM 1)
  set(ISAAC_VITA_DISPLAY_RASTER_DEN 1)
elseif(ISAAC_VITA_DISPLAY_RASTER STREQUAL "720")
  set(ISAAC_VITA_DISPLAY_RASTER_SCALED ON)
  set(ISAAC_VITA_DISPLAY_RASTER_WIDTH 720)
  set(ISAAC_VITA_DISPLAY_RASTER_HEIGHT 408)
  set(ISAAC_VITA_DISPLAY_RASTER_NUM 3)
  set(ISAAC_VITA_DISPLAY_RASTER_DEN 4)
elseif(ISAAC_VITA_DISPLAY_RASTER STREQUAL "480")
  set(ISAAC_VITA_DISPLAY_RASTER_SCALED ON)
  set(ISAAC_VITA_DISPLAY_RASTER_WIDTH 480)
  set(ISAAC_VITA_DISPLAY_RASTER_HEIGHT 272)
  set(ISAAC_VITA_DISPLAY_RASTER_NUM 1)
  set(ISAAC_VITA_DISPLAY_RASTER_DEN 2)
else()
  message(FATAL_ERROR
    "ISAAC_VITA_DISPLAY_RASTER must be 960, 720 or 480 "
    "(got '${ISAAC_VITA_DISPLAY_RASTER}')")
endif()
if(ISAAC_VITA_DISPLAY_RASTER_SCALED AND NOT ISAAC_VITA_DISPLAY_RASTER_720)
  set(ISAAC_VITA_DISPLAY_RASTER_720 ON CACHE BOOL
      "Legacy alias: render the default framebuffer at 720x408 while retaining 960x540 guest coordinates"
      FORCE)
endif()
option(ISAAC_VITA_FIRST_FRAME_PROBE
       "Run the bounded clear/blit/postprocess-bypass diagnostic" OFF)
option(ISAAC_VITA_FBO_RASTER_SCALE
       "Allocate screen-sized guest offscreen colour targets at NUM/DEN raster and scale their viewports" OFF)
set(ISAAC_VITA_FBO_RASTER_SCALE_NUM 1 CACHE STRING
    "FBO raster scale numerator (ISAAC_VITA_FBO_RASTER_SCALE=ON)")
set(ISAAC_VITA_FBO_RASTER_SCALE_DEN 2 CACHE STRING
    "FBO raster scale denominator (ISAAC_VITA_FBO_RASTER_SCALE=ON)")
option(ISAAC_VITA_FBO_CLEAR_ELISION
       "Stop guest glClear requests on offscreen targets that already hold exactly that clear value" OFF)
option(ISAAC_VITA_DIRECT_DEFAULT
       "Derive Game::Render with its three outer targets bypassed" OFF)
option(ISAAC_VITA_RENDER_SURFACE_NATIVE
       "Direct-default variant: keep the game's own Render Surface (world at its native 480x270, one plain upscale blit) and bypass only the HQX and colour-correction targets" OFF)
option(ISAAC_VITA_VITAGL_STOCK_REFERENCE
       "Use the pinned stock vitaGL hardware-reference profile" OFF)
option(ISAAC_VITA_STOCK_DISPLAY_RT_SCENES
       "Stock profile: size the display render target for 8 GXM scenes per frame" OFF)
option(ISAAC_VITA_VITAGL_CACHED_MEM
       "Call vglUseCachedMem(GL_TRUE) before vglInit so vitaGL pools are cacheable" OFF)
option(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS
       "Coalesce exact identical vertex streams and cache exact patched layouts" OFF)
option(ISAAC_VITA_VITAGL_SHADER_CACHE
       "Persist authenticated custom GXP programs for the pinned vitaGL build" OFF)
option(ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY
       "Use the pinned KAGE canonical quad index buffer at two exact PE callsites" OFF)
option(ISAAC_VITA_GXM_STATE_SHADOW
       "Suppress exact redundant GXM program and fragment-texture setters per scene" OFF)
option(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS
       "Enable exact stock ColorOffset identity and opaque no-blend variant" OFF)
if(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS AND NOT ISAAC_VITA_KAGE)
  message(FATAL_ERROR
    "ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS requires ISAAC_VITA_KAGE=ON")
endif()
if(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS AND
   NOT ISAAC_VITA_VITAGL_STOCK_REFERENCE)
  message(FATAL_ERROR
    "ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS requires the pinned stock reference profile")
endif()
if(ISAAC_VITA_VITAGL_SHADER_CACHE AND
   (NOT ISAAC_VITA_KAGE OR NOT ISAAC_VITA_VITAGL_STOCK_REFERENCE))
  message(FATAL_ERROR
    "ISAAC_VITA_VITAGL_SHADER_CACHE requires translated KAGE and the pinned stock reference profile")
endif()
if((ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY OR ISAAC_VITA_GXM_STATE_SHADOW) AND
   NOT ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
  message(FATAL_ERROR
    "The canonical-quad and GXM-state switches require ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS=ON")
endif()
if(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS AND
   NOT ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
  message(FATAL_ERROR
    "ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS requires the exact vitaGL GPU draw bundle")
endif()
option(ISAAC_VITA_RAW_GXM_PROBE
       "Record raw GXM return codes for the first four post-loading presents" OFF)
option(ISAAC_VITA_KNOWN_COLOR_PROBE
       "Force a green p2 clear plus one raw magenta p2 scanout rectangle" OFF)
option(ISAAC_VITA_SCREENSHOT_PROBE
       "Capture once after vglSwapBuffers returns for post-loading present 600" OFF)
option(ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC
       "On the first recognized texel-loader allocation failure, log bounded memory probes" OFF)
option(ISAAC_VITA_STAGE_MEMORY_DIAGNOSTICS
       "Synchronously snapshot and log heap state at level/room boundaries" OFF)
option(ISAAC_VITA_TEXEL_SCRATCH
       "Reserve recognized transient PNG/procedural texel backing outside newlib" OFF)
option(ISAAC_VITA_TEXEL_SCRATCH_8M
       "Diagnostic A/B: cap texel scratch requests at 8 MiB" OFF)
option(ISAAC_VITA_TEXTURE_ALIGN8_POLICY
       "Use KAGE's existing 8-pixel texture backing alignment policy" OFF)
option(ISAAC_VITA_ANM2_SCRATCH
       "Reserve the six exact ANM2 parser pools outside newlib" OFF)
option(ISAAC_VITA_ANM2_MISSING_LAYER_GUARD
       "Skip absent signed ANM2 layer-map entries before array indexing" OFF)
option(ISAAC_VITA_FXLAYERS_NULL_ROLLBACK
       "Rollback the exact last FXLayers record after a NULL image load" OFF)
option(ISAAC_VITA_HEAP_LEDGER_MEMBLOCK
       "Keep guest ownership-ledger generations in dedicated USER_RW blocks" ON)
option(ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
       "Delete guest ownership entries without tombstones or same-size rehashes" ON)
option(ISAAC_VITA_HEAP_OVERFLOW_MSPACE
       "Add a bounded USER_RW SceClibMspace after newlib exhaustion" OFF)
option(ISAAC_VITA_ROOM_ENTRY_SLAB
       "Pack exact frozen RoomConfig Entry arrays into retained mspace pages" OFF)
option(ISAAC_VITA_ROOM_ENTRY_HYBRID
       "Extend the RoomConfig Entry slab with bounded lazy USER_RW chunks" OFF)
option(ISAAC_VITA_GUEST_STACK_GUARD
       "Range-check generated synthetic-stack operations" ON)
option(ISAAC_VITA_MEMORY_RANGE_GUARD
       "Validate catastrophic guest movers against Vita memblock bounds" OFF)
option(ISAAC_VITA_CLOCK_BOOST
       "Raise CPU/BUS/GPU/XBAR clocks to 444/222/222/166 MHz at startup" ON)
# Translated-code CPU cost (backlog H / RECOMP-RESEARCH 1.1): three source-
# scoped changes to the generated corpus only.  Each sub-knob is consulted only
# while the umbrella is ON, so the default build keeps every compile command.
option(ISAAC_VITA_TRANSLATED_CPU
       "Cut translated-code CPU cost in generated units (see sub-knobs)" OFF)
option(ISAAC_VITA_TRANSLATED_CPU_COVERAGE_HOOKS_OFF
       "TRANSLATED_CPU: compile the always-NULL Vita coverage hooks out of generated units" ON)
option(ISAAC_VITA_TRANSLATED_CPU_FS_BASE_INLINE
       "TRANSLATED_CPU: answer fs-segment (TIB) accesses from the cached base inline" ON)
option(ISAAC_VITA_TRANSLATED_CPU_HOT_LAYOUT
       "TRANSLATED_CPU: cluster the frame-path hot set into one sorted text region" ON)
option(ISAAC_VITA_TRANSLATED_CPU_FLAGS_LOCAL
       "TRANSLATED_CPU: keep the lazy x86 flag state in per-function locals (GUEST_FLAGS_LOCAL=1) so producers store nothing and consumers fold" ON)
option(ISAAC_VITA_TRANSLATED_CPU_GPR_LOCAL
       "TRANSLATED_CPU: keep the eight x86 general registers in per-function locals (GUEST_GPR_LOCAL=1), published only at call/fault/seam edges" OFF)
option(ISAAC_VITA_TRANSLATED_CPU_LEAF_INLINE
       "TRANSLATED_CPU: at direct call sites of proven tiny leaf callees compile the copied statements instead of the C call (GUEST_LEAF_INLINE=1; the corpus carries both spellings, OFF selects the call by relink)" ON)
option(ISAAC_VITA_TRANSLATED_CPU_SSE_LOWER
       "TRANSLATED_CPU: expand the emitter's GUEST_XMM_* tokens to NEON vector operations and direct float relations (GUEST_SSE_LOWER=1); OFF spells the legacy lane loops" OFF)
option(ISAAC_VITA_TRANSLATED_CPU_F32_VFP
       "TRANSLATED_CPU: scalar float guest loads and stores (guest.h ldf/stf: movss, addss/mulss/comiss/... memory operands, fld/fstp dword) as VFP vldr/vstr behind a word-alignment test (GUEST_F32_VFP_ACCESS=1); OFF keeps the memcpy pun (ldr/str + vmov core<->VFP transfers). Header-only, no corpus change; turn ON in perf builds" OFF)
option(ISAAC_VITA_TRANSLATED_CPU_COLD_REST
       "TRANSLATED_CPU: mark every translated function outside the hot set cold (-Os, .text.unlikely)" OFF)
option(ISAAC_VITA_LAYOUT_HUB
       "Link .text as [cold generated half A][hot region + runtime + libraries][cold half B] from a generated GNU ld script so no Thumb-2 BL/B.W needs an ARM-state long-branch veneer (pure placement, objects untouched)" OFF)
set(ISAAC_VITA_LAYOUT_HUB_ORDER
    "${CMAKE_CURRENT_SOURCE_DIR}/translated_cpu_unit_order.txt" CACHE FILEPATH
    "Committed cold-unit order around the hub (vita_text_layout.py order); regenerate with 'ninja isaac_vita_layout_order' after any corpus change")
set(ISAAC_VITA_LAYOUT_HUB_MAX_VENEERS 0 CACHE STRING
    "LAYOUT_HUB: post-link limit on remaining long-branch veneers (cold<->cold only; 0 = fail closed once the corpus drifts past the committed order's headroom, a small value = soft mode for emitter-iteration days)")
set(ISAAC_VITA_TRANSLATED_CPU_HOT_SET
    "${CMAKE_CURRENT_SOURCE_DIR}/translated_cpu_hot_set.txt" CACHE FILEPATH
    "Ordered hot translated function list consumed by the hot layout header")
set(ISAAC_VITA_GUEST_ISA "thumb" CACHE STRING
    "Instruction set for generated units: thumb (Thumb-2, default) or arm")
set(ISAAC_VITA_VITAGL_OVERLAY_DIR
    "${CMAKE_BINARY_DIR}/isaac-vitagl-overlay" CACHE PATH
    "Build root for the project-local pinned softfp vitaGL")
set(ISAAC_VITA_VITAGL_STOCK_REFERENCE_DIR
    "${CMAKE_BINARY_DIR}/isaac-vitagl-stock-reference" CACHE PATH
    "Build root for the pinned stock vitaGL hardware reference")
set(ISAAC_VITA_OPENAL_OVERLAY_DIR
    "${CMAKE_BINARY_DIR}/isaac-openal-overlay" CACHE PATH
    "Build/install root for the project-local patched OpenAL Soft")

if(NOT ISAAC_VITA_GUEST_BUILD_OPT MATCHES "^O[123s]$")
  message(FATAL_ERROR
    "ISAAC_VITA_GUEST_BUILD_OPT must be one of O1, O2, O3, Os: "
    "${ISAAC_VITA_GUEST_BUILD_OPT}")
endif()
if(NOT ISAAC_VITA_GUEST_ISA MATCHES "^(thumb|arm)$")
  message(FATAL_ERROR
    "ISAAC_VITA_GUEST_ISA must be thumb or arm: ${ISAAC_VITA_GUEST_ISA}")
endif()
if(ISAAC_VITA_TRANSLATED_CPU AND ISAAC_VITA_SCAFFOLD_ONLY)
  message(FATAL_ERROR
    "ISAAC_VITA_TRANSLATED_CPU requires the translated corpus "
    "(ISAAC_VITA_SCAFFOLD_ONLY=OFF)")
endif()
if(ISAAC_VITA_TRANSLATED_CPU_COLD_REST AND
   NOT (ISAAC_VITA_TRANSLATED_CPU AND ISAAC_VITA_TRANSLATED_CPU_HOT_LAYOUT))
  message(FATAL_ERROR
    "ISAAC_VITA_TRANSLATED_CPU_COLD_REST requires ISAAC_VITA_TRANSLATED_CPU=ON "
    "and ISAAC_VITA_TRANSLATED_CPU_HOT_LAYOUT=ON")
endif()
if(ISAAC_VITA_TRANSLATED_CPU AND ISAAC_VITA_TRANSLATED_CPU_HOT_LAYOUT AND
   NOT EXISTS "${ISAAC_VITA_TRANSLATED_CPU_HOT_SET}")
  message(FATAL_ERROR
    "ISAAC_VITA_TRANSLATED_CPU_HOT_SET does not exist: "
    "${ISAAC_VITA_TRANSLATED_CPU_HOT_SET}")
endif()
if(ISAAC_VITA_LAYOUT_HUB)
  # The hub is built around SORT(.text.sorted.*); COLD_REST would move the whole
  # corpus into the hub's .text.unlikely preamble and defeat the reach budget.
  if(ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_TRANSLATED_CPU OR
     NOT ISAAC_VITA_TRANSLATED_CPU_HOT_LAYOUT OR ISAAC_VITA_TRANSLATED_CPU_COLD_REST)
    message(FATAL_ERROR
      "ISAAC_VITA_LAYOUT_HUB requires ISAAC_VITA_TRANSLATED_CPU=ON, "
      "ISAAC_VITA_TRANSLATED_CPU_HOT_LAYOUT=ON, ISAAC_VITA_TRANSLATED_CPU_COLD_REST=OFF "
      "and the translated corpus (ISAAC_VITA_SCAFFOLD_ONLY=OFF)")
  endif()
  if(NOT EXISTS "${ISAAC_VITA_LAYOUT_HUB_ORDER}")
    message(FATAL_ERROR
      "ISAAC_VITA_LAYOUT_HUB_ORDER does not exist: ${ISAAC_VITA_LAYOUT_HUB_ORDER}")
  endif()
endif()

foreach(ISAAC_BUILD_STAMP_VALUE IN ITEMS
        "${ISAAC_VITA_GUEST_LINK_ID}" "${ISAAC_VITA_GUEST_BUILD_OPT}")
  string(LENGTH "${ISAAC_BUILD_STAMP_VALUE}" ISAAC_BUILD_STAMP_LENGTH)
  if(ISAAC_BUILD_STAMP_LENGTH LESS 1 OR
     ISAAC_BUILD_STAMP_LENGTH GREATER 96 OR
     NOT ISAAC_BUILD_STAMP_VALUE MATCHES "^[A-Za-z0-9][A-Za-z0-9._:+-]*$")
    message(FATAL_ERROR
      "Vita guest build-stamp values must be 1..96 safe ASCII characters: "
      "${ISAAC_BUILD_STAMP_VALUE}")
  endif()
endforeach()

include("${ISAAC_VITA_STOCK_REFERENCE_SELECTION_MODULE}")
isaac_validate_vita_stock_reference_profile()

# Vita3K retains one size-derived alignment quantum after a USER_RW block.
# The accepted fixed-image layout therefore is not monotonic in the requested
# heap size: 64 and 80 MiB overlap 0x98000000, while 81 MiB has a 1 MiB
# quantum and remains below both the ANM2 scratch bound and the fixed PE.
if(NOT ISAAC_VITA_HEAP_MB MATCHES "^[0-9]+$" OR
   ISAAC_VITA_HEAP_MB LESS 1 OR
   ISAAC_VITA_HEAP_MB EQUAL 64 OR
   ISAAC_VITA_HEAP_MB EQUAL 80 OR
   ISAAC_VITA_HEAP_MB GREATER 81)
  message(FATAL_ERROR
    "ISAAC_VITA_HEAP_MB must be 1..63, 65..79, or the measured 81 MiB layout")
endif()
if((ISAAC_VITA_ANM2_SCRATCH OR ISAAC_VITA_TEXEL_SCRATCH OR
    ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC) AND
   NOT ISAAC_VITA_HEAP_MB EQUAL 81)
  message(FATAL_ERROR
    "ANM2/texel scratch contracts require ISAAC_VITA_HEAP_MB=81")
endif()
if(ISAAC_VITA_HEAP_OVERFLOW_MSPACE AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_HEAP_MB EQUAL 81 OR
    NOT ISAAC_VITA_HEAP_LEDGER_MEMBLOCK OR
    NOT ISAAC_VITA_HEAP_LEDGER_BACKSHIFT))
  message(FATAL_ERROR
    "ISAAC_VITA_HEAP_OVERFLOW_MSPACE requires translated runtime, "
    "ISAAC_VITA_HEAP_MB=81, HEAP_LEDGER_MEMBLOCK=ON and "
    "HEAP_LEDGER_BACKSHIFT=ON")
endif()
if(ISAAC_VITA_ROOM_ENTRY_SLAB AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_HEAP_OVERFLOW_MSPACE))
  message(FATAL_ERROR
    "ISAAC_VITA_ROOM_ENTRY_SLAB requires the translated runtime and "
    "ISAAC_VITA_HEAP_OVERFLOW_MSPACE=ON")
endif()
if(ISAAC_VITA_ROOM_ENTRY_HYBRID AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_ROOM_ENTRY_SLAB OR
    NOT ISAAC_VITA_HEAP_OVERFLOW_MSPACE))
  message(FATAL_ERROR
    "ISAAC_VITA_ROOM_ENTRY_HYBRID requires the translated runtime, "
    "ISAAC_VITA_ROOM_ENTRY_SLAB=ON and "
    "ISAAC_VITA_HEAP_OVERFLOW_MSPACE=ON")
endif()

if(ISAAC_VITA_IO_PROFILE AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))
  message(FATAL_ERROR
    "ISAAC_VITA_IO_PROFILE requires the translated KAGE runtime")
endif()
if(ISAAC_VITA_MEMORY_RANGE_GUARD AND ISAAC_VITA_SCAFFOLD_ONLY)
  message(FATAL_ERROR
    "ISAAC_VITA_MEMORY_RANGE_GUARD requires the translated guest runtime")
endif()
if(ISAAC_VITA_STALL_PROBE AND NOT ISAAC_VITA_KAGE)
  message(FATAL_ERROR "ISAAC_VITA_STALL_PROBE requires ISAAC_VITA_KAGE=ON")
endif()
if(ISAAC_VITA_STAGE_HEARTBEAT AND NOT ISAAC_VITA_KAGE)
  message(FATAL_ERROR "ISAAC_VITA_STAGE_HEARTBEAT requires ISAAC_VITA_KAGE=ON")
endif()
if(ISAAC_VITA_LOG_ASYNC AND ISAAC_VITA_SCAFFOLD_ONLY)
  message(FATAL_ERROR "ISAAC_VITA_LOG_ASYNC requires the runtime (ISAAC_VITA_SCAFFOLD_ONLY=OFF)")
endif()
if(NOT ISAAC_VITA_LOG_ASYNC_RING_KB MATCHES "^[1-9][0-9]*$" OR
   ISAAC_VITA_LOG_ASYNC_RING_KB LESS 4)
  message(FATAL_ERROR
    "ISAAC_VITA_LOG_ASYNC_RING_KB must be an integer >= 4 (KiB), got "
    "'${ISAAC_VITA_LOG_ASYNC_RING_KB}'")
endif()
if(ISAAC_VITA_PHASE_PROFILE AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))
  message(FATAL_ERROR
    "ISAAC_VITA_PHASE_PROFILE requires the translated KAGE runtime")
endif()
if(ISAAC_VITA_SIM_CADENCE_RECEIPT AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))
  message(FATAL_ERROR
    "ISAAC_VITA_SIM_CADENCE_RECEIPT requires the translated KAGE runtime")
endif()
if(ISAAC_VITA_WORLD_SEAM_DIAG AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))
  message(FATAL_ERROR
    "ISAAC_VITA_WORLD_SEAM_DIAG requires the translated KAGE runtime")
endif()
if(ISAAC_VITA_SIM_CADENCE_RECEIPT AND ISAAC_VITA_PHASE_PROFILE)
  message(FATAL_ERROR
    "ISAAC_VITA_SIM_CADENCE_RECEIPT requires ISAAC_VITA_PHASE_PROFILE=OFF")
endif()
if(ISAAC_VITA_TEXTURE_CHURN_PROFILE AND NOT ISAAC_VITA_PHASE_PROFILE)
  message(FATAL_ERROR
    "ISAAC_VITA_TEXTURE_CHURN_PROFILE requires ISAAC_VITA_PHASE_PROFILE=ON")
endif()
if(ISAAC_VITA_TEXTURE_CHURN_PROFILE AND
   NOT ISAAC_VITA_VITAGL_STOCK_REFERENCE)
  message(FATAL_ERROR
    "ISAAC_VITA_TEXTURE_CHURN_PROFILE requires the pinned stock vitaGL reference")
endif()
if(ISAAC_VITA_GL_TIME_PROFILE AND NOT ISAAC_VITA_PHASE_PROFILE)
  message(FATAL_ERROR
    "ISAAC_VITA_GL_TIME_PROFILE requires ISAAC_VITA_PHASE_PROFILE=ON")
endif()
option(ISAAC_VITA_PHASE_PROFILE_OTHER
       "Add oth=p50/p95/max to ph120.t: the loop time outside svc/upd/rnd/lim (render return -> limiter entry plus limiter exit -> next service entry); worst-case record 357 -> 394 bytes, inside the 512-byte allowance ph120.s already uses" OFF)
if(ISAAC_VITA_PHASE_PROFILE_OTHER AND NOT ISAAC_VITA_PHASE_PROFILE)
  message(FATAL_ERROR
    "ISAAC_VITA_PHASE_PROFILE_OTHER requires ISAAC_VITA_PHASE_PROFILE=ON")
endif()
if(ISAAC_VITA_GUEST_SAMPLER AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))
  message(FATAL_ERROR
    "ISAAC_VITA_GUEST_SAMPLER requires the translated KAGE runtime")
endif()
if(ISAAC_VITA_GUEST_SAMPLER AND NOT ISAAC_VITA_PHASE_PROFILE)
  message(FATAL_ERROR
    "ISAAC_VITA_GUEST_SAMPLER requires ISAAC_VITA_PHASE_PROFILE=ON")
endif()
if(ISAAC_VITA_PROFILE_FUNCTION_ENTRIES AND NOT ISAAC_VITA_PHASE_PROFILE)
  message(FATAL_ERROR
    "ISAAC_VITA_PROFILE_FUNCTION_ENTRIES requires ISAAC_VITA_PHASE_PROFILE=ON")
endif()
if(ISAAC_VITA_PROFILE_FUNCTION_ENTRIES AND NOT ISAAC_VITA_PROFILE_IMPORT_KINDS)
  # ent= is printed only by the ph120.ik record the census owns; without it
  # the corpus-wide increment would be paid and never reported.
  message(FATAL_ERROR
    "ISAAC_VITA_PROFILE_FUNCTION_ENTRIES requires ISAAC_VITA_PROFILE_IMPORT_KINDS=ON")
endif()
if(ISAAC_VITA_GL_TIME_PROFILE AND
   (NOT ISAAC_VITA_VITAGL_STOCK_REFERENCE OR
    NOT ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS))
  # The 0005 scene-timer patch is written against gxm.c as left by the 0002
  # draw patch of the stock reference recipe.
  message(FATAL_ERROR
    "ISAAC_VITA_GL_TIME_PROFILE requires ISAAC_VITA_VITAGL_STOCK_REFERENCE=ON "
    "and ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS=ON")
endif()
if(ISAAC_VITA_GL_REDUNDANCY_CACHE AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))
  message(FATAL_ERROR
    "ISAAC_VITA_GL_REDUNDANCY_CACHE requires the translated KAGE runtime")
endif()
if(ISAAC_VITA_GL_TYPED_STATE_CACHE AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))
  message(FATAL_ERROR
    "ISAAC_VITA_GL_TYPED_STATE_CACHE requires the translated KAGE runtime")
endif()
if(ISAAC_VITA_DISPLAY_RASTER_SCALED AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))
  message(FATAL_ERROR
    "ISAAC_VITA_DISPLAY_RASTER=${ISAAC_VITA_DISPLAY_RASTER} requires the "
    "translated KAGE runtime")
endif()
if(ISAAC_VITA_DISPLAY_RASTER_SCALED AND ISAAC_VITA_CONTINUE_PROFILE)
  message(FATAL_ERROR
    "ISAAC_VITA_DISPLAY_RASTER=${ISAAC_VITA_DISPLAY_RASTER} requires "
    "CONTINUE_PROFILE=OFF; kage_vita_continue_profile.c owns a fixed "
    "960-pixel scanout stride")
endif()
if(ISAAC_VITA_DISPLAY_RASTER_SCALED AND
   (ISAAC_VITA_FIRST_FRAME_PROBE OR ISAAC_VITA_RAW_GXM_PROBE OR
    ISAAC_VITA_KNOWN_COLOR_PROBE OR ISAAC_VITA_SCREENSHOT_PROBE))
  message(FATAL_ERROR
    "ISAAC_VITA_DISPLAY_RASTER=${ISAAC_VITA_DISPLAY_RASTER} requires "
    "FIRST_FRAME, RAW_GXM, KNOWN_COLOR and SCREENSHOT probes to be OFF; "
    "those diagnostics own a fixed 960-pixel scanout pitch")
endif()
if(ISAAC_VITA_FIRST_FRAME_PROBE AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))
  message(FATAL_ERROR
    "ISAAC_VITA_FIRST_FRAME_PROBE requires the translated KAGE runtime")
endif()
if((ISAAC_VITA_FBO_RASTER_SCALE OR ISAAC_VITA_FBO_CLEAR_ELISION) AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))
  message(FATAL_ERROR
    "ISAAC_VITA_FBO_RASTER_SCALE / ISAAC_VITA_FBO_CLEAR_ELISION require the translated KAGE runtime")
endif()
if(ISAAC_VITA_FBO_RASTER_SCALE)
  if(NOT ISAAC_VITA_FBO_RASTER_SCALE_NUM MATCHES "^[1-9][0-9]*$" OR
     NOT ISAAC_VITA_FBO_RASTER_SCALE_DEN MATCHES "^[1-9][0-9]*$" OR
     NOT ISAAC_VITA_FBO_RASTER_SCALE_NUM LESS ISAAC_VITA_FBO_RASTER_SCALE_DEN OR
     ISAAC_VITA_FBO_RASTER_SCALE_DEN GREATER 16)
    message(FATAL_ERROR
      "ISAAC_VITA_FBO_RASTER_SCALE_NUM/DEN must be a proper fraction of positive integers with DEN <= 16 "
      "(got ${ISAAC_VITA_FBO_RASTER_SCALE_NUM}/${ISAAC_VITA_FBO_RASTER_SCALE_DEN})")
  endif()
  if(ISAAC_VITA_FIRST_FRAME_PROBE OR ISAAC_VITA_KNOWN_COLOR_PROBE OR
     ISAAC_VITA_SCREENSHOT_PROBE)
    message(FATAL_ERROR
      "ISAAC_VITA_FBO_RASTER_SCALE requires FIRST_FRAME, KNOWN_COLOR and "
      "SCREENSHOT probes to be OFF; those diagnostics read offscreen targets pixel-exactly")
  endif()
endif()
if(ISAAC_VITA_FBO_CLEAR_ELISION AND
   (ISAAC_VITA_FIRST_FRAME_PROBE OR ISAAC_VITA_KNOWN_COLOR_PROBE OR
    ISAAC_VITA_SCREENSHOT_PROBE))
  message(FATAL_ERROR
    "ISAAC_VITA_FBO_CLEAR_ELISION requires FIRST_FRAME, KNOWN_COLOR and "
    "SCREENSHOT probes to be OFF; those diagnostics issue native glFinish/glReadPixels "
    "outside the guest GL boundary, which the owed-clear model does not see")
endif()
if(ISAAC_VITA_RAW_GXM_PROBE AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))
  message(FATAL_ERROR
    "ISAAC_VITA_RAW_GXM_PROBE requires the translated KAGE runtime")
endif()
if(ISAAC_VITA_SCREENSHOT_PROBE AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))
  message(FATAL_ERROR
    "ISAAC_VITA_SCREENSHOT_PROBE requires the translated KAGE runtime")
endif()
if(ISAAC_VITA_DIRECT_DEFAULT AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE OR
    (NOT ISAAC_VITA_FIRST_FRAME_PROBE AND
     NOT ISAAC_VITA_VITAGL_STOCK_REFERENCE)))
  message(FATAL_ERROR
    "ISAAC_VITA_DIRECT_DEFAULT requires the translated KAGE runtime and "
    "FIRST_FRAME_PROBE=ON unless VITAGL_STOCK_REFERENCE=ON")
endif()
if(ISAAC_VITA_RENDER_SURFACE_NATIVE AND NOT ISAAC_VITA_DIRECT_DEFAULT)
  message(FATAL_ERROR
    "ISAAC_VITA_RENDER_SURFACE_NATIVE is a derivation mode of "
    "ISAAC_VITA_DIRECT_DEFAULT and requires it ON")
endif()
if(ISAAC_VITA_KNOWN_COLOR_PROBE AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE OR
    NOT ISAAC_VITA_FIRST_FRAME_PROBE OR NOT ISAAC_VITA_DIRECT_DEFAULT OR
    NOT ISAAC_VITA_RAW_GXM_PROBE))
  message(FATAL_ERROR
    "ISAAC_VITA_KNOWN_COLOR_PROBE requires translated KAGE plus "
    "FIRST_FRAME_PROBE=ON, DIRECT_DEFAULT=ON and RAW_GXM_PROBE=ON")
endif()
if(ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC AND NOT ISAAC_VITA_KAGE)
  message(FATAL_ERROR
    "ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC requires ISAAC_VITA_KAGE=ON")
endif()
if(ISAAC_VITA_TEXEL_SCRATCH AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))
  message(FATAL_ERROR
    "ISAAC_VITA_TEXEL_SCRATCH requires the translated KAGE runtime")
endif()
if(ISAAC_VITA_TEXEL_SCRATCH_8M AND NOT ISAAC_VITA_TEXEL_SCRATCH)
  message(FATAL_ERROR
    "ISAAC_VITA_TEXEL_SCRATCH_8M requires ISAAC_VITA_TEXEL_SCRATCH=ON")
endif()
if(ISAAC_VITA_TEXTURE_ALIGN8_POLICY AND NOT ISAAC_VITA_KAGE)
  message(FATAL_ERROR
    "ISAAC_VITA_TEXTURE_ALIGN8_POLICY requires ISAAC_VITA_KAGE=ON")
endif()
if(ISAAC_VITA_VANILLA_KAGE_BUNDLE AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))
  message(FATAL_ERROR
    "ISAAC_VITA_VANILLA_KAGE_BUNDLE requires the translated KAGE runtime")
endif()
if(ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))
  message(FATAL_ERROR
    "ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE requires the translated KAGE runtime")
endif()
if(ISAAC_VITA_PILL_BLOOM_BYPASS AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))
  message(FATAL_ERROR
    "ISAAC_VITA_PILL_BLOOM_BYPASS requires the translated KAGE runtime")
endif()
if(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))
  message(FATAL_ERROR
    "ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES requires the translated KAGE runtime")
endif()
if(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES AND
   NOT ISAAC_VITA_TEXTURE_ALIGN8_POLICY)
  message(FATAL_ERROR
    "ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES requires ISAAC_VITA_TEXTURE_ALIGN8_POLICY=ON for the 480x272 auxiliary backing")
endif()
if(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES AND
   ISAAC_VITA_PILL_BLOOM_BYPASS)
  message(FATAL_ERROR
    "Bloom half-resolution and Bloom bypass are mutually exclusive performance modes")
endif()
if(ISAAC_VITA_LASER_PROFILE AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))
  message(FATAL_ERROR
    "ISAAC_VITA_LASER_PROFILE requires the translated KAGE runtime")
endif()
if(ISAAC_VITA_LASER_PROFILE AND NOT ISAAC_VITA_PHASE_PROFILE)
  message(FATAL_ERROR
    "ISAAC_VITA_LASER_PROFILE requires ISAAC_VITA_PHASE_PROFILE=ON")
endif()
if(ISAAC_VITA_POOP_FX_PROFILE AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))
  message(FATAL_ERROR
    "ISAAC_VITA_POOP_FX_PROFILE requires the translated KAGE runtime")
endif()
if(ISAAC_VITA_PNG_DECODE_PROFILE AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))
  message(FATAL_ERROR
    "ISAAC_VITA_PNG_DECODE_PROFILE requires the translated KAGE runtime")
endif()
if(ISAAC_VITA_PNG_NATIVE_UNFILTER AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE OR
    NOT ISAAC_VITA_HEAP_OVERFLOW_MSPACE))
  message(FATAL_ERROR
    "ISAAC_VITA_PNG_NATIVE_UNFILTER requires translated KAGE and "
    "ISAAC_VITA_HEAP_OVERFLOW_MSPACE=ON")
endif()
if(ISAAC_VITA_PNG_INFLATE_FLUSH_FASTPATH AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE OR
    NOT ISAAC_VITA_HEAP_OVERFLOW_MSPACE))
  message(FATAL_ERROR
    "ISAAC_VITA_PNG_INFLATE_FLUSH_FASTPATH requires translated KAGE and "
    "ISAAC_VITA_HEAP_OVERFLOW_MSPACE=ON")
endif()
if(ISAAC_VITA_PNG_CRC32_FASTPATH AND ISAAC_VITA_SCAFFOLD_ONLY)
  message(FATAL_ERROR
    "ISAAC_VITA_PNG_CRC32_FASTPATH requires the generated guest runtime")
endif()
if(ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE OR
    NOT ISAAC_VITA_HEAP_OVERFLOW_MSPACE))
  message(FATAL_ERROR
    "ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH requires translated KAGE and "
    "ISAAC_VITA_HEAP_OVERFLOW_MSPACE=ON")
endif()
if(ISAAC_VITA_POOP_FX_SINGLE_CLOUD AND NOT ISAAC_VITA_POOP_FX_PROFILE)
  message(FATAL_ERROR
    "ISAAC_VITA_POOP_FX_SINGLE_CLOUD requires ISAAC_VITA_POOP_FX_PROFILE=ON for proof counters")
endif()
if(ISAAC_VITA_ANM2_SCRATCH AND ISAAC_VITA_SCAFFOLD_ONLY)
  message(FATAL_ERROR
    "ISAAC_VITA_ANM2_SCRATCH requires the translated guest runtime")
endif()
if(ISAAC_VITA_ANM2_MISSING_LAYER_GUARD AND ISAAC_VITA_SCAFFOLD_ONLY)
  message(FATAL_ERROR
    "ISAAC_VITA_ANM2_MISSING_LAYER_GUARD requires the translated guest runtime")
endif()
if(ISAAC_VITA_RENDERFRAME_FASTPATH AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))
  message(FATAL_ERROR
    "ISAAC_VITA_RENDERFRAME_FASTPATH requires the translated KAGE runtime")
endif()
if(ISAAC_VITA_FXLAYERS_NULL_ROLLBACK AND
   (ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE))
  message(FATAL_ERROR
    "ISAAC_VITA_FXLAYERS_NULL_ROLLBACK requires translated KAGE runtime")
endif()
if(ISAAC_VITA_LOADING_SPECIALIST AND NOT ISAAC_VITA_KAGE)
  message(FATAL_ERROR
    "ISAAC_VITA_LOADING_SPECIALIST requires ISAAC_VITA_KAGE=ON")
endif()

set(ISAAC_VITA_AUDIO_ACTIVE OFF)
if(ISAAC_VITA_KAGE AND ISAAC_VITA_AUDIO)
  set(ISAAC_VITA_AUDIO_ACTIVE ON)
endif()

if(ISAAC_VITA_KAGE)
  find_program(ISAAC_BASH_EXECUTABLE bash REQUIRED)
  if(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS)
    set(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS_MODE 1)
  else()
    set(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS_MODE 0)
  endif()
  if(ISAAC_VITA_VITAGL_SHADER_CACHE)
    set(ISAAC_VITA_VITAGL_SHADER_CACHE_MODE 1)
  else()
    set(ISAAC_VITA_VITAGL_SHADER_CACHE_MODE 0)
  endif()
  if(ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY)
    set(ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY_MODE 1)
  else()
    set(ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY_MODE 0)
  endif()
  if(ISAAC_VITA_GXM_STATE_SHADOW)
    set(ISAAC_VITA_GXM_STATE_SHADOW_MODE 1)
  else()
    set(ISAAC_VITA_GXM_STATE_SHADOW_MODE 0)
  endif()
  set(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS_MODE_FILE
      "${CMAKE_CURRENT_BINARY_DIR}/isaac-vitagl-gpu-draw-optimizations.mode")
  file(GENERATE
       OUTPUT "${ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS_MODE_FILE}"
       CONTENT "${ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS_MODE}\n")
  set(ISAAC_VITA_VITAGL_SHADER_CACHE_MODE_FILE
      "${CMAKE_CURRENT_BINARY_DIR}/isaac-vitagl-shader-cache.mode")
  file(GENERATE
       OUTPUT "${ISAAC_VITA_VITAGL_SHADER_CACHE_MODE_FILE}"
       CONTENT "${ISAAC_VITA_VITAGL_SHADER_CACHE_MODE}\n")
  set(ISAAC_VITA_DRAW_SUBMISSION_MODE_FILE
      "${CMAKE_CURRENT_BINARY_DIR}/isaac-vitagl-draw-submission.mode")
  file(GENERATE OUTPUT "${ISAAC_VITA_DRAW_SUBMISSION_MODE_FILE}"
       CONTENT "quad=${ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY_MODE}\nstate=${ISAAC_VITA_GXM_STATE_SHADOW_MODE}\n")
  if(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
    set(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS_MODE 1)
  else()
    set(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS_MODE 0)
  endif()
  if(ISAAC_VITA_PHASE_PROFILE)
    set(ISAAC_VITA_PHASE_PROFILE_MODE 1)
  else()
    set(ISAAC_VITA_PHASE_PROFILE_MODE 0)
  endif()
  set(ISAAC_VITA_PHASE_PROFILE_MODE_FILE
      "${CMAKE_CURRENT_BINARY_DIR}/isaac-vitagl-phase-profile.mode")
  file(GENERATE
       OUTPUT "${ISAAC_VITA_PHASE_PROFILE_MODE_FILE}"
       CONTENT "${ISAAC_VITA_PHASE_PROFILE_MODE}\n")
  if(ISAAC_VITA_GL_TIME_PROFILE)
    set(ISAAC_VITA_GL_TIME_PROFILE_MODE 1)
  else()
    set(ISAAC_VITA_GL_TIME_PROFILE_MODE 0)
  endif()
  set(ISAAC_VITA_GL_TIME_PROFILE_MODE_FILE
      "${CMAKE_CURRENT_BINARY_DIR}/isaac-vitagl-gl-time-profile.mode")
  file(GENERATE
       OUTPUT "${ISAAC_VITA_GL_TIME_PROFILE_MODE_FILE}"
       CONTENT "${ISAAC_VITA_GL_TIME_PROFILE_MODE}\n")
  set(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS_MODE_FILE
      "${CMAKE_CURRENT_BINARY_DIR}/isaac-coloroffset-gpu-optimizations.mode")
  file(GENERATE
       OUTPUT "${ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS_MODE_FILE}"
       CONTENT "${ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS_MODE}\n")
  if(ISAAC_VITA_VITAGL_STOCK_REFERENCE)
    set(ISAAC_VITA_VITAGL_OVERLAY_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/vitagl-stock-reference/build.sh")
    set(ISAAC_VITA_VITAGL_OVERLAY_PATCH
        "${CMAKE_CURRENT_SOURCE_DIR}/vitagl-stock-reference/0001-deterministic-build-and-init-oob.patch")
    set(ISAAC_VITA_VITAGL_GPU_DRAW_PATCH
        "${CMAKE_CURRENT_SOURCE_DIR}/vitagl-stock-reference/0002-exact-gpu-draw-optimizations.patch")
    set(ISAAC_VITA_VITAGL_GPU_DRAW_POLICY
        "${CMAKE_CURRENT_SOURCE_DIR}/vitagl-stock-reference/isaac_gpu_draw_policy.h")
    set(ISAAC_VITA_VITAGL_GXM_STATE_POLICY
        "${CMAKE_CURRENT_SOURCE_DIR}/vitagl-stock-reference/isaac_gxm_state_policy.h")
    set(ISAAC_VITA_VITAGL_SHADER_CACHE_PATCH
        "${CMAKE_CURRENT_SOURCE_DIR}/vitagl-stock-reference/0004-hardened-custom-shader-cache.patch")
    set(ISAAC_VITA_VITAGL_SHADER_CACHE_POLICY
        "${CMAKE_CURRENT_SOURCE_DIR}/vitagl-stock-reference/isaac_shader_cache_policy.h")
    set(ISAAC_VITA_VITAGL_SHADER_CACHE_INTEGRATION
        "${CMAKE_CURRENT_SOURCE_DIR}/vitagl-stock-reference/isaac_shader_cache_vitagl.h")
    set(ISAAC_VITA_COLOROFFSET_GPU_PATCH
        "${CMAKE_CURRENT_SOURCE_DIR}/vitagl-stock-reference/0003-exact-coloroffset-gpu-optimizations.patch")
    set(ISAAC_VITA_COLOROFFSET_GPU_POLICY
        "${CMAKE_CURRENT_SOURCE_DIR}/vitagl-stock-reference/isaac_coloroffset_gpu_policy.h")
    set(ISAAC_VITA_VITAGL_SCENE_TIMER_PATCH
        "${CMAKE_CURRENT_SOURCE_DIR}/vitagl-stock-reference/0005-isaac-scene-timer.patch")
    set(ISAAC_VITA_VITAGL_BUILD_ROOT
        "${ISAAC_VITA_VITAGL_STOCK_REFERENCE_DIR}")
    set(ISAAC_VITA_VITAGL_STAMP
        "${ISAAC_VITA_VITAGL_BUILD_ROOT}/stock-reference-ready.stamp")
  else()
    set(ISAAC_VITA_VITAGL_OVERLAY_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/vitagl-overlay/build.sh")
    set(ISAAC_VITA_VITAGL_OVERLAY_PATCH
        "${CMAKE_CURRENT_SOURCE_DIR}/vitagl-overlay/0001-isaac-build-contract.patch")
    set(ISAAC_VITA_VITAGL_BUILD_ROOT "${ISAAC_VITA_VITAGL_OVERLAY_DIR}")
    if(ISAAC_VITA_FIRST_FRAME_PROBE)
      set(ISAAC_VITA_DISPLAY_QUEUE_PROBE 1)
    else()
      set(ISAAC_VITA_DISPLAY_QUEUE_PROBE 0)
    endif()
    set(ISAAC_VITA_DISPLAY_QUEUE_PROBE_MODE_FILE
        "${CMAKE_CURRENT_BINARY_DIR}/isaac-vitagl-display-queue-probe.mode")
    file(GENERATE OUTPUT "${ISAAC_VITA_DISPLAY_QUEUE_PROBE_MODE_FILE}"
         CONTENT "${ISAAC_VITA_DISPLAY_QUEUE_PROBE}\n")
    if(ISAAC_VITA_KNOWN_COLOR_PROBE)
      set(ISAAC_VITA_KNOWN_COLOR_OVERLAY 1)
    else()
      set(ISAAC_VITA_KNOWN_COLOR_OVERLAY 0)
    endif()
    set(ISAAC_VITA_KNOWN_COLOR_MODE_FILE
        "${CMAKE_CURRENT_BINARY_DIR}/isaac-vitagl-known-color-probe.mode")
    file(GENERATE OUTPUT "${ISAAC_VITA_KNOWN_COLOR_MODE_FILE}"
         CONTENT "${ISAAC_VITA_KNOWN_COLOR_OVERLAY}\n")
    set(ISAAC_VITA_VITAGL_STAMP
        "${ISAAC_VITA_VITAGL_OVERLAY_DIR}/overlay-ready.stamp")
  endif()

  set(ISAAC_VITA_VITAGL_LIBRARY
      "${ISAAC_VITA_VITAGL_BUILD_ROOT}/prefix/lib/libvitaGL.a")
  set(ISAAC_VITA_VITAGL_INCLUDE_DIR
      "${ISAAC_VITA_VITAGL_BUILD_ROOT}/prefix/include")
  set(ISAAC_VITA_VITAGL_HEADER
      "${ISAAC_VITA_VITAGL_INCLUDE_DIR}/vitaGL.h")
  file(MAKE_DIRECTORY "${ISAAC_VITA_VITAGL_INCLUDE_DIR}")
  if(ISAAC_VITA_VITAGL_STOCK_REFERENCE)
    add_custom_command(
      OUTPUT "${ISAAC_VITA_VITAGL_STAMP}"
      BYPRODUCTS
        "${ISAAC_VITA_VITAGL_LIBRARY}"
        "${ISAAC_VITA_VITAGL_HEADER}"
      COMMAND "${CMAKE_COMMAND}" -E env "VITASDK=${VITASDK}"
              "ISAAC_GPU_DRAW_OPTIMIZATIONS=${ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS_MODE}"
              "ISAAC_SHADER_CACHE=${ISAAC_VITA_VITAGL_SHADER_CACHE_MODE}"
              "ISAAC_CANONICAL_QUAD_ZERO_COPY=${ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY_MODE}"
              "ISAAC_GXM_STATE_SHADOW=${ISAAC_VITA_GXM_STATE_SHADOW_MODE}"
              "ISAAC_COLOROFFSET_GPU_OPTIMIZATIONS=${ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS_MODE}"
              "ISAAC_PHASE_PROFILE=${ISAAC_VITA_PHASE_PROFILE_MODE}"
              "ISAAC_GL_TIME_PROFILE=${ISAAC_VITA_GL_TIME_PROFILE_MODE}"
              "${ISAAC_BASH_EXECUTABLE}"
              "${ISAAC_VITA_VITAGL_OVERLAY_SCRIPT}"
              "${ISAAC_VITA_VITAGL_BUILD_ROOT}"
      COMMAND "${CMAKE_COMMAND}" -E touch
              "${ISAAC_VITA_VITAGL_STAMP}"
      DEPENDS
        "${ISAAC_VITA_VITAGL_OVERLAY_SCRIPT}"
        "${ISAAC_VITA_VITAGL_OVERLAY_PATCH}"
        "${ISAAC_VITA_VITAGL_GPU_DRAW_PATCH}"
        "${ISAAC_VITA_VITAGL_GPU_DRAW_POLICY}"
        "${ISAAC_VITA_VITAGL_GXM_STATE_POLICY}"
        "${ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS_MODE_FILE}"
        "${ISAAC_VITA_VITAGL_SHADER_CACHE_PATCH}"
        "${ISAAC_VITA_VITAGL_SHADER_CACHE_POLICY}"
        "${ISAAC_VITA_VITAGL_SHADER_CACHE_INTEGRATION}"
        "${ISAAC_VITA_VITAGL_SHADER_CACHE_MODE_FILE}"
        "${ISAAC_VITA_DRAW_SUBMISSION_MODE_FILE}"
        "${ISAAC_VITA_COLOROFFSET_GPU_PATCH}"
        "${ISAAC_VITA_COLOROFFSET_GPU_POLICY}"
        "${ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS_MODE_FILE}"
        "${ISAAC_VITA_PHASE_PROFILE_MODE_FILE}"
        "${ISAAC_VITA_VITAGL_SCENE_TIMER_PATCH}"
        "${ISAAC_VITA_GL_TIME_PROFILE_MODE_FILE}"
      COMMENT "Building pinned stock vitaGL 73dd57a with gpu-draw=${ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS_MODE}, shader-cache=${ISAAC_VITA_VITAGL_SHADER_CACHE_MODE}, quad=${ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY_MODE}, state=${ISAAC_VITA_GXM_STATE_SHADOW_MODE}, coloroffset=${ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS_MODE}, profile=${ISAAC_VITA_PHASE_PROFILE_MODE}, gl-time=${ISAAC_VITA_GL_TIME_PROFILE_MODE}"
      VERBATIM USES_TERMINAL)
  else()
    # Compile and consume the existing diagnostic overlay without mutating
    # VitaSDK. Its build contract owns all queue/GXM instrumentation.
    add_custom_command(
      OUTPUT "${ISAAC_VITA_VITAGL_STAMP}"
      BYPRODUCTS
        "${ISAAC_VITA_VITAGL_LIBRARY}"
        "${ISAAC_VITA_VITAGL_HEADER}"
      COMMAND "${CMAKE_COMMAND}" -E env "VITASDK=${VITASDK}"
              "ISAAC_DISPLAY_QUEUE_PROBE=${ISAAC_VITA_DISPLAY_QUEUE_PROBE}"
              "ISAAC_KNOWN_COLOR_PROBE=${ISAAC_VITA_KNOWN_COLOR_OVERLAY}"
              "${ISAAC_BASH_EXECUTABLE}"
              "${ISAAC_VITA_VITAGL_OVERLAY_SCRIPT}"
              "${ISAAC_VITA_VITAGL_BUILD_ROOT}"
      COMMAND "${CMAKE_COMMAND}" -E touch
              "${ISAAC_VITA_VITAGL_STAMP}"
      DEPENDS
        "${ISAAC_VITA_VITAGL_OVERLAY_SCRIPT}"
        "${ISAAC_VITA_VITAGL_OVERLAY_PATCH}"
        "${ISAAC_VITA_DISPLAY_QUEUE_PROBE_MODE_FILE}"
        "${ISAAC_VITA_KNOWN_COLOR_MODE_FILE}"
      COMMENT "Building pinned softfp vitaGL with queue probe=${ISAAC_VITA_DISPLAY_QUEUE_PROBE}, known-color=${ISAAC_VITA_KNOWN_COLOR_OVERLAY}"
      VERBATIM USES_TERMINAL)
  endif()
  add_custom_target(isaac_vita_vitagl_overlay_build
    DEPENDS "${ISAAC_VITA_VITAGL_STAMP}")
  add_library(isaac_vita_vitagl_overlay STATIC IMPORTED GLOBAL)
  set_target_properties(isaac_vita_vitagl_overlay PROPERTIES
    IMPORTED_LOCATION "${ISAAC_VITA_VITAGL_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${ISAAC_VITA_VITAGL_INCLUDE_DIR}")
  add_dependencies(isaac_vita_vitagl_overlay
    isaac_vita_vitagl_overlay_build)
endif()

if(ISAAC_VITA_AUDIO_ACTIVE)
  find_program(ISAAC_BASH_EXECUTABLE bash REQUIRED)
  set(ISAAC_VITA_OPENAL_OVERLAY_SCRIPT
      "${CMAKE_CURRENT_SOURCE_DIR}/openal-overlay/build.sh")
  set(ISAAC_VITA_OPENAL_OVERLAY_PATCH
      "${CMAKE_CURRENT_SOURCE_DIR}/openal-overlay/0001-limit-initial-voices.patch")
  set(ISAAC_VITA_OPENAL_REDIRECT_PATCH
      "${CMAKE_CURRENT_SOURCE_DIR}/openal-overlay/0002-route-direct-allocators.patch")
  set(ISAAC_VITA_OPENAL_REDIRECT_HEADER
      "${CMAKE_CURRENT_SOURCE_DIR}/openal-overlay/openal_pool_redirect.h")
  set(ISAAC_VITA_OPENAL_ARCHIVE_VERIFIER
      "${CMAKE_CURRENT_SOURCE_DIR}/openal-overlay/verify_deterministic_archive.py")
  set(ISAAC_VITA_OPENAL_POOL_HEADER
      "${ISAAC_RUNTIME}/host_vita_openal_pool.h")
  set(ISAAC_VITA_OPENAL_LIBRARY
      "${ISAAC_VITA_OPENAL_OVERLAY_DIR}/prefix/lib/libopenal.a")
  set(ISAAC_VITA_OPENAL_STAMP
      "${ISAAC_VITA_OPENAL_OVERLAY_DIR}/overlay-ready.stamp")
  set(ISAAC_VITA_OPENAL_CONFIG
      "${CMAKE_CURRENT_SOURCE_DIR}/alsoft.conf")
  set(ISAAC_VITA_OPENAL_CONFIG_DESTINATION "alsoft.conf")

  get_filename_component(ISAAC_VITA_OPENAL_CONFIG_NAME
      "${ISAAC_VITA_OPENAL_CONFIG}" NAME)
  if(NOT ISAAC_VITA_OPENAL_CONFIG_NAME STREQUAL "alsoft.conf" OR
     NOT ISAAC_VITA_OPENAL_CONFIG_DESTINATION STREQUAL "alsoft.conf")
    message(FATAL_ERROR
      "OpenAL config source and app0 destination must both be alsoft.conf")
  endif()

  # OpenAL Soft 1.19.1 reads this app0 file during its one-time init, before
  # alcOpenDevice returns.  Refuse a package that could silently restore its
  # 256-source/256-voice default while the runtime expects an exact 80-name
  # device: the frozen manager pool owns 64 and streaming owns 16 more.
  file(STRINGS "${ISAAC_VITA_OPENAL_CONFIG}" ISAAC_OPENAL_SOURCE_LINES
       REGEX "^[ \t]*sources[ \t]*=")
  list(LENGTH ISAAC_OPENAL_SOURCE_LINES ISAAC_OPENAL_SOURCE_LINE_COUNT)
  if(NOT ISAAC_OPENAL_SOURCE_LINE_COUNT EQUAL 1 OR
     NOT ISAAC_OPENAL_SOURCE_LINES MATCHES
        "^[ \t]*sources[ \t]*=[ \t]*80[ \t]*$")
    message(FATAL_ERROR
      "alsoft.conf must contain exactly one general sources = 80 setting")
  endif()

  # Keep the SDK archive immutable.  This pinned overlay reproduces the
  # VitaSDK 1.19.1-2 recipe and applies the bounded voice-pool and direct-
  # allocator redirect patches in a private build directory.  The archive is
  # an ordinary Ninja dependency,
  # so audio-OFF builds do not download or compile it.
  add_custom_command(
    OUTPUT "${ISAAC_VITA_OPENAL_STAMP}"
    BYPRODUCTS "${ISAAC_VITA_OPENAL_LIBRARY}"
    COMMAND "${CMAKE_COMMAND}" -E env "VITASDK=${VITASDK}"
            "${ISAAC_BASH_EXECUTABLE}"
            "${ISAAC_VITA_OPENAL_OVERLAY_SCRIPT}"
            "${ISAAC_VITA_OPENAL_OVERLAY_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E touch
            "${ISAAC_VITA_OPENAL_STAMP}"
    DEPENDS
      "${ISAAC_VITA_OPENAL_OVERLAY_SCRIPT}"
      "${ISAAC_VITA_OPENAL_OVERLAY_PATCH}"
      "${ISAAC_VITA_OPENAL_REDIRECT_PATCH}"
      "${ISAAC_VITA_OPENAL_REDIRECT_HEADER}"
      "${ISAAC_VITA_OPENAL_ARCHIVE_VERIFIER}"
      "${ISAAC_VITA_OPENAL_POOL_HEADER}"
    COMMENT "Building pinned Vita OpenAL Soft 1.19.1 memory overlay"
    VERBATIM USES_TERMINAL)
  add_custom_target(isaac_vita_openal_overlay_build
    DEPENDS "${ISAAC_VITA_OPENAL_STAMP}")
  add_library(isaac_vita_openal_overlay STATIC IMPORTED GLOBAL)
  set_target_properties(isaac_vita_openal_overlay PROPERTIES
    IMPORTED_LOCATION "${ISAAC_VITA_OPENAL_LIBRARY}")
  add_dependencies(isaac_vita_openal_overlay
    isaac_vita_openal_overlay_build)
endif()

set(ISAAC_TITLE_ID "ISAACR001")
set(ISAAC_APP_NAME "The Binding of Isaac: Repentance")
set(ISAAC_APP_VERSION "01.01")
set(ISAAC_VPK_NAME "the-binding-of-isaac-repentance")
set(ISAAC_VITA_PACKAGE_ASSETS_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/sce_sys")
set(ISAAC_VPK_LIVEAREA_FILES
  FILE "${ISAAC_VITA_PACKAGE_ASSETS_DIR}/icon0.png"
       "sce_sys/icon0.png"
  FILE "${ISAAC_VITA_PACKAGE_ASSETS_DIR}/pic0.png"
       "sce_sys/pic0.png"
  FILE "${ISAAC_VITA_PACKAGE_ASSETS_DIR}/livearea/contents/bg0.png"
       "sce_sys/livearea/contents/bg0.png"
  FILE "${ISAAC_VITA_PACKAGE_ASSETS_DIR}/livearea/contents/nicalis.png"
       "sce_sys/livearea/contents/nicalis.png"
  FILE "${ISAAC_VITA_PACKAGE_ASSETS_DIR}/livearea/contents/startup.png"
       "sce_sys/livearea/contents/startup.png"
  FILE "${ISAAC_VITA_PACKAGE_ASSETS_DIR}/livearea/contents/template.xml"
       "sce_sys/livearea/contents/template.xml")
set(ISAAC_VPK_MANAGER_FILE
  FILE "${CMAKE_CURRENT_BINARY_DIR}/isaac-manager.bin"
       "isaac-manager.bin")
set(ISAAC_VPK_MANAGER_LICENSE_FILE
  FILE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libftpvita/LICENSE"
       "licenses/libftpvita.txt")

add_executable(isaac_manager
  manager_main.c
  manager_ftp_lifecycle.c
  manager_ftp_path.c
  manager_ftp_protocol.c
  manager_ftp_server.c
  manager_sha256.c
  manager_storage.c
  manager_storage_vita.c)
target_compile_features(isaac_manager PRIVATE c_std_11)
target_compile_options(isaac_manager PRIVATE
  -std=gnu11 -O2
  -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb
  -ffunction-sections -fdata-sections
  -Wall -Wextra)
target_link_options(isaac_manager PRIVATE
  -Wl,--gc-sections)
target_link_libraries(isaac_manager PRIVATE
  SceAppMgr_stub
  SceCtrl_stub
  SceDisplay_stub
  SceIofilemgr_stub
  SceNet_stub
  SceNetCtl_stub
  SceSysmem_stub
  SceSysmodule_stub
  SceKernelThreadMgr_stub
  SceLibKernel_stub)

add_executable(isaac_first_arm_fault
  main.c
  fixed_image.c
  platform.c)

target_include_directories(isaac_first_arm_fault PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${ISAAC_RUNTIME}")

# Keep this explicit even though the softfp toolchain currently supplies the
# same defaults. It is a build-contract property, not an ambient SDK choice.
target_compile_features(isaac_first_arm_fault PRIVATE c_std_11)
target_compile_options(isaac_first_arm_fault PRIVATE
  -std=gnu11 -O2
  -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb
  -fno-strict-aliasing -ffunction-sections -fdata-sections
  -Wall -Wextra)
target_compile_definitions(isaac_first_arm_fault PRIVATE
  GUEST_IMAGE_BASE=${ISAAC_GUEST_BASE}u
  GUEST_STACK_REQUIRED=1
  ISAAC_VITA_HEAP_MB=64)

if(ISAAC_VITA_LUA)
  isaac_validate_strict_absolute_path(
    "${ISAAC_LUA53_SOURCE_DIR}" "ISAAC_LUA53_SOURCE_DIR")
  include("${CMAKE_CURRENT_SOURCE_DIR}/lua53_vanilla.cmake")
  # Keep pristine upstream sources in a separate archive.  The production
  # bridge creates both default and guest-allocator states through lua_newstate;
  # therefore lauxlib's private realloc/free luaL_newstate section is dead and
  # --gc-sections cannot expose an unowned allocation to the guest.
  add_library(isaac_lua53 STATIC ${ISAAC_LUA53_C_SOURCES})
  target_include_directories(isaac_lua53 PUBLIC
    "${ISAAC_LUA53_SOURCE_DIR}/src")
  target_compile_features(isaac_lua53 PRIVATE c_std_11)
  target_compile_options(isaac_lua53 PRIVATE
    -std=gnu11 -O2
    -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb
    -fno-strict-aliasing -ffunction-sections -fdata-sections
    -Wall -Wextra)
endif()

# The generated corpus does not use the legacy heap macro. Keep that dummy
# target-wide definition stable so a heap-placement change preserves all 171
# expensive compile commands; the real knob is scoped to its owning object.
set_property(SOURCE platform.c APPEND PROPERTY COMPILE_DEFINITIONS
  ISAAC_VITA_PLATFORM_HEAP_MB=${ISAAC_VITA_HEAP_MB})
set_property(SOURCE main.c APPEND PROPERTY COMPILE_DEFINITIONS
  ISAAC_VITA_BUILD_ID=\"${ISAAC_VITA_GUEST_LINK_ID}\")
# Scoped to main.c so the A/B flip relinks without recompiling the corpus.
# The frequencies in effect are logged before and after in every build, so
# both arms leave evidence in the hardware log.
if(ISAAC_VITA_CLOCK_BOOST)
  set_property(SOURCE main.c APPEND PROPERTY COMPILE_DEFINITIONS
    ISAAC_VITA_CLOCK_BOOST=1)
  message(STATUS
    "Isaac Vita clocks: boost to CPU 444 / BUS 222 / GPU 222 / XBAR 166 MHz")
else()
  message(STATUS
    "Isaac Vita clocks: system default (CPU 333 / GPU 111 MHz); logged only")
endif()

if(ISAAC_VITA_SCAFFOLD_ONLY)
  target_compile_definitions(isaac_first_arm_fault PRIVATE
    ISAAC_VITA_HAS_RUNTIME=0)
else()
  if(NOT IS_DIRECTORY "${ISAAC_GENERATED_DIR}")
    message(FATAL_ERROR
      "ISAAC_GENERATED_DIR must name fresh gen_all.py output. "
      "Use -DISAAC_VITA_SCAFFOLD_ONLY=ON only for the allocator smoke VPK.")
  endif()
  if(NOT EXISTS "${ISAAC_GENERATED_DIR}/manifest.json")
    message(FATAL_ERROR "generated manifest.json is missing")
  endif()
  if(NOT EXISTS "${ISAAC_PE_PATH}")
    message(FATAL_ERROR "ISAAC_PE_PATH must name the unpacked Repentance PE")
  endif()

  # Generated absolute operands contain the relocated image VA. Reject the old
  # PC corpus (0x30000000) instead of producing a Vita binary which is guaranteed
  # to dereference the wrong address.
  file(READ "${ISAAC_GENERATED_DIR}/manifest.json" ISAAC_GEN_MANIFEST)
  string(JSON ISAAC_EXPECTED_PE_SHA ERROR_VARIABLE ISAAC_PE_JSON_ERROR
         GET "${ISAAC_GEN_MANIFEST}" recipe inputs
             "input/isaac-ng.exe.unpacked.exe")
  string(LENGTH "${ISAAC_EXPECTED_PE_SHA}" ISAAC_EXPECTED_PE_SHA_LENGTH)
  if(ISAAC_PE_JSON_ERROR OR
     NOT ISAAC_EXPECTED_PE_SHA_LENGTH EQUAL 64 OR
     NOT ISAAC_EXPECTED_PE_SHA MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR
      "generated manifest has no valid unpacked-PE SHA-256: "
      "${ISAAC_PE_JSON_ERROR}")
  endif()
  file(SIZE "${ISAAC_PE_PATH}" ISAAC_ACTUAL_PE_SIZE)
  file(SHA256 "${ISAAC_PE_PATH}" ISAAC_ACTUAL_PE_SHA)
  if(NOT ISAAC_ACTUAL_PE_SIZE EQUAL 8650240 OR
     NOT ISAAC_ACTUAL_PE_SHA STREQUAL ISAAC_EXPECTED_PE_SHA)
    message(FATAL_ERROR
      "unpacked PE differs from the exact generation input: "
      "size=${ISAAC_ACTUAL_PE_SIZE}, sha256=${ISAAC_ACTUAL_PE_SHA}")
  endif()
  string(JSON ISAAC_GENERATED_BASE ERROR_VARIABLE ISAAC_JSON_ERROR
         GET "${ISAAC_GEN_MANIFEST}" recipe params image_base)
  if(ISAAC_JSON_ERROR)
    message(FATAL_ERROR
      "cannot read recipe.params.image_base from generated manifest: "
      "${ISAAC_JSON_ERROR}")
  endif()
  math(EXPR ISAAC_EXPECTED_BASE "${ISAAC_GUEST_BASE}")
  if(NOT ISAAC_GENERATED_BASE EQUAL ISAAC_EXPECTED_BASE)
    math(EXPR ISAAC_GENERATED_BASE_HEX
         "${ISAAC_GENERATED_BASE}" OUTPUT_FORMAT HEXADECIMAL)
    message(FATAL_ERROR
      "generated C is for ${ISAAC_GENERATED_BASE_HEX}, expected "
      "${ISAAC_GUEST_BASE}; regenerate before building")
  endif()
  string(JSON ISAAC_RECIPE_ID ERROR_VARIABLE ISAAC_RECIPE_JSON_ERROR
         GET "${ISAAC_GEN_MANIFEST}" recipe_id)
  string(LENGTH "${ISAAC_RECIPE_ID}" ISAAC_RECIPE_ID_LENGTH)
  if(ISAAC_RECIPE_JSON_ERROR OR
     NOT ISAAC_RECIPE_ID_LENGTH EQUAL 64 OR
     NOT ISAAC_RECIPE_ID MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR
      "generated manifest has no valid 64-hex recipe_id: "
      "${ISAAC_RECIPE_JSON_ERROR}")
  endif()

  # The base field alone cannot catch a mixed corpus: one stale numbered TU can
  # carry silent 0x30...... literals beside an otherwise correct 0x88 manifest.
  # Validate the manifest's exact output set and every byte before CMake is
  # allowed to offer any generated source to the compiler.
  string(JSON ISAAC_OUTPUT_SET_ID ERROR_VARIABLE ISAAC_SET_ID_JSON_ERROR
         GET "${ISAAC_GEN_MANIFEST}" output_set_id)
  string(LENGTH "${ISAAC_OUTPUT_SET_ID}" ISAAC_OUTPUT_SET_ID_LENGTH)
  if(ISAAC_SET_ID_JSON_ERROR OR
     NOT ISAAC_OUTPUT_SET_ID_LENGTH EQUAL 64 OR
     NOT ISAAC_OUTPUT_SET_ID MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR
      "generated manifest has no valid output_set_id: "
      "${ISAAC_SET_ID_JSON_ERROR}")
  endif()

  string(JSON ISAAC_MANIFEST_OUTPUT_COUNT ERROR_VARIABLE ISAAC_OUTPUT_JSON_ERROR
         LENGTH "${ISAAC_GEN_MANIFEST}" outputs)
  if(ISAAC_OUTPUT_JSON_ERROR OR ISAAC_MANIFEST_OUTPUT_COUNT LESS 1)
    message(FATAL_ERROR
      "generated manifest has no output inventory: ${ISAAC_OUTPUT_JSON_ERROR}")
  endif()
  math(EXPR ISAAC_LAST_OUTPUT "${ISAAC_MANIFEST_OUTPUT_COUNT} - 1")

  set(ISAAC_MANIFEST_OUTPUT_NAMES)
  set(ISAAC_MANIFEST_OUTPUT_FILES)
  set(ISAAC_GENERATED_C)
  set(ISAAC_VITA_DIRECT_DEFAULT_SOURCE_NAME "guest_0144.c")
  set(ISAAC_VITA_DIRECT_DEFAULT_SOURCE_SIZE 1115751)
  set(ISAAC_VITA_DIRECT_DEFAULT_SOURCE_SHA
      "2c15e0305d7dbe6f5bdbb122b47b10d5952646942da503961b8da50b8ab09513")
  set(ISAAC_VITA_DIRECT_DEFAULT_CANONICAL_SOURCE)
  set(ISAAC_VITA_DIRECT_DEFAULT_MANIFEST_SIZE)
  set(ISAAC_VITA_DIRECT_DEFAULT_MANIFEST_SHA)
  foreach(ISAAC_OUTPUT_INDEX RANGE 0 ${ISAAC_LAST_OUTPUT})
    string(JSON ISAAC_OUTPUT_NAME GET "${ISAAC_GEN_MANIFEST}"
           outputs ${ISAAC_OUTPUT_INDEX} path)
    string(JSON ISAAC_OUTPUT_SIZE GET "${ISAAC_GEN_MANIFEST}"
           outputs ${ISAAC_OUTPUT_INDEX} size)
    string(JSON ISAAC_OUTPUT_SHA GET "${ISAAC_GEN_MANIFEST}"
           outputs ${ISAAC_OUTPUT_INDEX} sha256)
    if(IS_ABSOLUTE "${ISAAC_OUTPUT_NAME}" OR
       ISAAC_OUTPUT_NAME MATCHES "(^|/)\\.\\.(/|$)")
      message(FATAL_ERROR
        "unsafe generated manifest path: ${ISAAC_OUTPUT_NAME}")
    endif()

    set(ISAAC_OUTPUT_FILE "${ISAAC_GENERATED_DIR}/${ISAAC_OUTPUT_NAME}")
    if(NOT EXISTS "${ISAAC_OUTPUT_FILE}" OR IS_DIRECTORY "${ISAAC_OUTPUT_FILE}")
      message(FATAL_ERROR
        "generated output is missing: ${ISAAC_OUTPUT_NAME}")
    endif()
    file(SIZE "${ISAAC_OUTPUT_FILE}" ISAAC_ACTUAL_SIZE)
    if(NOT ISAAC_ACTUAL_SIZE EQUAL ISAAC_OUTPUT_SIZE)
      message(FATAL_ERROR
        "generated output size mismatch for ${ISAAC_OUTPUT_NAME}: "
        "${ISAAC_ACTUAL_SIZE} != ${ISAAC_OUTPUT_SIZE}")
    endif()
    file(SHA256 "${ISAAC_OUTPUT_FILE}" ISAAC_ACTUAL_SHA)
    if(NOT ISAAC_ACTUAL_SHA STREQUAL ISAAC_OUTPUT_SHA)
      message(FATAL_ERROR
        "generated output SHA-256 mismatch for ${ISAAC_OUTPUT_NAME}: "
        "${ISAAC_ACTUAL_SHA} != ${ISAAC_OUTPUT_SHA}")
    endif()

    list(APPEND ISAAC_MANIFEST_OUTPUT_NAMES "${ISAAC_OUTPUT_NAME}")
    list(APPEND ISAAC_MANIFEST_OUTPUT_FILES "${ISAAC_OUTPUT_FILE}")
    if(ISAAC_OUTPUT_NAME MATCHES "^guest_.*\\.c$")
      list(APPEND ISAAC_GENERATED_C "${ISAAC_OUTPUT_FILE}")
    endif()
    if(ISAAC_OUTPUT_NAME STREQUAL ISAAC_VITA_DIRECT_DEFAULT_SOURCE_NAME)
      set(ISAAC_VITA_DIRECT_DEFAULT_CANONICAL_SOURCE "${ISAAC_OUTPUT_FILE}")
      set(ISAAC_VITA_DIRECT_DEFAULT_MANIFEST_SIZE "${ISAAC_OUTPUT_SIZE}")
      set(ISAAC_VITA_DIRECT_DEFAULT_MANIFEST_SHA "${ISAAC_OUTPUT_SHA}")
    endif()
  endforeach()

  isaac_literal_directory_files(
    "${ISAAC_GENERATED_DIR}" ISAAC_GENERATED_DIRECTORY_FILES)
  list(REMOVE_ITEM ISAAC_GENERATED_DIRECTORY_FILES "manifest.json")
  list(SORT ISAAC_GENERATED_DIRECTORY_FILES)
  list(SORT ISAAC_MANIFEST_OUTPUT_NAMES)
  if(NOT "${ISAAC_GENERATED_DIRECTORY_FILES}" STREQUAL
         "${ISAAC_MANIFEST_OUTPUT_NAMES}")
    message(FATAL_ERROR
      "generated directory differs from manifest output set; "
      "refuse a missing, extra, or mixed corpus")
  endif()

  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${ISAAC_GENERATED_DIR}/manifest.json"
    ${ISAAC_MANIFEST_OUTPUT_FILES})
  list(LENGTH ISAAC_GENERATED_C ISAAC_GENERATED_C_COUNT)
  if(ISAAC_GENERATED_C_COUNT LESS 3)
    message(FATAL_ERROR "generated C corpus is incomplete")
  endif()
  # Feature-owner discovery must inspect the canonical, freshly validated
  # corpus.  A build-owned derived source may not exist yet during a clean
  # configure and may still contain the previous build's feature set during an
  # incremental configure.
  set(ISAAC_GENERATED_CANONICAL_C ${ISAAC_GENERATED_C})
  if(ISAAC_VITA_DIRECT_DEFAULT)
    if(NOT "${ISAAC_VITA_DIRECT_DEFAULT_MANIFEST_SIZE}" STREQUAL
           "${ISAAC_VITA_DIRECT_DEFAULT_SOURCE_SIZE}" OR
       NOT "${ISAAC_VITA_DIRECT_DEFAULT_MANIFEST_SHA}" STREQUAL
           "${ISAAC_VITA_DIRECT_DEFAULT_SOURCE_SHA}")
      message(FATAL_ERROR
        "direct-default canonical guest_0144.c identity changed: "
        "size=${ISAAC_VITA_DIRECT_DEFAULT_MANIFEST_SIZE}/"
        "${ISAAC_VITA_DIRECT_DEFAULT_SOURCE_SIZE}, "
        "sha256=${ISAAC_VITA_DIRECT_DEFAULT_MANIFEST_SHA}/"
        "${ISAAC_VITA_DIRECT_DEFAULT_SOURCE_SHA}")
    endif()
    list(FIND ISAAC_GENERATED_C
      "${ISAAC_VITA_DIRECT_DEFAULT_CANONICAL_SOURCE}"
      ISAAC_VITA_DIRECT_DEFAULT_SOURCE_INDEX)
    if(ISAAC_VITA_DIRECT_DEFAULT_SOURCE_INDEX LESS 0)
      message(FATAL_ERROR
        "direct-default canonical guest_0144.c is absent from the source list")
    endif()
    set(ISAAC_VITA_DIRECT_DEFAULT_CODEGEN
        "${CMAKE_CURRENT_SOURCE_DIR}/vita_direct_default_codegen.py")
    set(ISAAC_VITA_DIRECT_DEFAULT_OUTPUT_DIR
        "${CMAKE_BINARY_DIR}/isaac-generated-overrides/direct-default")
    set(ISAAC_VITA_DIRECT_DEFAULT_OUTPUT
        "${ISAAC_VITA_DIRECT_DEFAULT_OUTPUT_DIR}/guest_0144.c")
    isaac_validate_strict_absolute_path(
      "${ISAAC_VITA_DIRECT_DEFAULT_OUTPUT}" "direct-default generated source")
    set_source_files_properties(
      "${ISAAC_VITA_DIRECT_DEFAULT_OUTPUT}" PROPERTIES GENERATED TRUE)
    list(REMOVE_AT ISAAC_GENERATED_C
      ${ISAAC_VITA_DIRECT_DEFAULT_SOURCE_INDEX})
    list(INSERT ISAAC_GENERATED_C
      ${ISAAC_VITA_DIRECT_DEFAULT_SOURCE_INDEX}
      "${ISAAC_VITA_DIRECT_DEFAULT_OUTPUT}")
    list(LENGTH ISAAC_GENERATED_C ISAAC_VITA_DIRECT_DEFAULT_REPLACED_COUNT)
    if(NOT ISAAC_VITA_DIRECT_DEFAULT_REPLACED_COUNT EQUAL
           ISAAC_GENERATED_C_COUNT)
      message(FATAL_ERROR
        "direct-default source replacement changed generated source count")
    endif()
  endif()

  set(ISAAC_VITA_RUNTIME_SOURCES
    "${ISAAC_RUNTIME}/guest.c"
    "${ISAAC_RUNTIME}/entry_vita.c"
    "${ISAAC_RUNTIME}/host_vita_first_fault.c"
    "${ISAAC_RUNTIME}/host_vita_import_id.c"
    "${ISAAC_RUNTIME}/host_vita_com.c"
    "${ISAAC_RUNTIME}/host_vita_console.c"
    "${ISAAC_RUNTIME}/host_vita_crt.c"
    "${ISAAC_RUNTIME}/host_vita_exception.c"
    "${ISAAC_RUNTIME}/host_vita_file_lock.c"
    "${ISAAC_RUNTIME}/host_vita_filesystem.c"
    "${ISAAC_RUNTIME}/host_vita_find.c"
    "${ISAAC_RUNTIME}/host_vita_fls.c"
    "${ISAAC_RUNTIME}/host_vita_gl.c"
    "${ISAAC_RUNTIME}/host_vita_heap.c"
    "${ISAAC_RUNTIME}/host_vita_ogg_emergency.c"
    "${ISAAC_RUNTIME}/kage_vita_fx_rollback.c"
    "${ISAAC_RUNTIME}/host_vita_math.c"
    "${ISAAC_RUNTIME}/host_vita_sync.c"
    "${ISAAC_RUNTIME}/host_vita_memory.c"
    "${ISAAC_RUNTIME}/host_vita_post_com.c"
    "${ISAAC_RUNTIME}/host_vita_rtti.c"
    "${ISAAC_RUNTIME}/host_vita_startup.c"
    "${ISAAC_RUNTIME}/host_vita_steam.c"
    "${ISAAC_RUNTIME}/host_vita_user32.c"
    "${ISAAC_RUNTIME}/vita_sync_services.c"
    "${ISAAC_RUNTIME}/gl_bridge.c"
    "${ISAAC_RUNTIME}/manual_portable.c")
  if(ISAAC_VITA_LUA)
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_lua.c")
    if(ISAAC_VITA_LUA_RECEIPT)
      # Bridge-side proof of execution: the game log only names chunks that
      # reached RunScript, and the frozen mod loader gates main.lua on an
      # _access probe of disable.it/update.it/main.lua (owner 0x00563120).
      # vita_sync_services.c adds the bounded userspace-mutex wait receipt;
      # host_vita_lua.c adds the startup beat thread (see the file comments).
      set_property(SOURCE
        "${ISAAC_RUNTIME}/host_vita_lua.c"
        "${ISAAC_RUNTIME}/host_vita_filesystem.c"
        "${ISAAC_RUNTIME}/vita_sync_services.c"
        APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_LUA_RECEIPT=1)
      message(STATUS
        "Isaac Lua: [isaac-lua] load/pcall/access/newstate receipts, startup beat and mutex-wait receipt enabled (bounded)")
    endif()
    if(ISAAC_VITA_LUA_GCSTEP_CLAMP_KB GREATER 0)
      set_property(SOURCE "${ISAAC_RUNTIME}/host_vita_lua.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_LUA_GCSTEP_CLAMP_KB=${ISAAC_VITA_LUA_GCSTEP_CLAMP_KB})
      message(STATUS
        "Isaac Lua: LuaEngine::Update GC step clamped at ${ISAAC_VITA_LUA_GCSTEP_CLAMP_KB} KB per frame")
    endif()
    if(ISAAC_VITA_LUA_GCCOLLECT_CLAMP OR ISAAC_VITA_LUA_GC_PROFILE)
      # Both lua_gc policies live in one owner TU behind the import; the
      # bridge gains only the delimited hook calls (host_vita_lua_gc.c).
      list(APPEND ISAAC_VITA_RUNTIME_SOURCES
        "${ISAAC_RUNTIME}/host_vita_lua_gc.c")
      if(ISAAC_VITA_LUA_GCCOLLECT_CLAMP)
        set_property(SOURCE
          "${ISAAC_RUNTIME}/host_vita_lua.c"
          "${ISAAC_RUNTIME}/host_vita_lua_gc.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_LUA_GCCOLLECT_CLAMP=1
            ISAAC_VITA_LUA_GCCOLLECT_STEP_KB=${ISAAC_VITA_LUA_GCCOLLECT_STEP_KB})
        message(STATUS
          "Isaac Lua: floor-transition GCCOLLECT (site 0x3b5870) clamped to one ${ISAAC_VITA_LUA_GCCOLLECT_STEP_KB} KB step")
      endif()
      if(ISAAC_VITA_LUA_GC_PROFILE)
        set_property(SOURCE
          "${ISAAC_RUNTIME}/host_vita_lua.c"
          "${ISAAC_RUNTIME}/host_vita_lua_gc.c"
          APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_LUA_GC_PROFILE=1)
        message(STATUS
          "Isaac Lua: GC profile ([isaac-lua] gc window per 120 LuaEngine::Update steps)")
      endif()
    endif()
    if(NOT ISAAC_VITA_LUA_SCOPE_FASTPATH)
      # host_vita_lua.c defaults the macro to 1; only the OFF state needs a
      # definition, so the ON object is compiled by the unchanged command.
      set_property(SOURCE "${ISAAC_RUNTIME}/host_vita_lua.c"
        APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_LUA_SCOPE_FASTPATH=0)
      message(STATUS
        "Isaac Lua: scope/trampoline fast path OFF (pre-knob code, byte-identical)")
    else()
      message(STATUS
        "Isaac Lua: scope/trampoline fast path ON (ISAAC_VITA_LUA_SCOPE_FASTPATH)")
    endif()
    if(ISAAC_VITA_LUA_ARENA_MB GREATER 0)
      # Measured: EID exhausts the 81 MiB guest heap ~27 s after launch and the
      # next archive fopen fails with ENOMEM (bundles 15/23/24/25).  The arena
      # is a private mspace for the LuaEngine state, reserved by the entry
      # owner before vitaGL absorbs the remaining USER_RW into its RAM pool.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/host_vita_lua.c"
        "${ISAAC_RUNTIME}/entry_vita.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_LUA_ARENA_MB=${ISAAC_VITA_LUA_ARENA_MB})
      message(STATUS
        "Isaac Lua: private LuaEngine arena ${ISAAC_VITA_LUA_ARENA_MB} MiB (USER_RW mspace, guest-heap fallback)")
    else()
      message(STATUS "Isaac Lua: LuaEngine allocations stay on the guest heap")
    endif()
    if(ISAAC_VITA_LUA_IMPORT_FASTDISPATCH)
      # Two owners only: the bridge (typed endpoints + name-bound table) and
      # the import-ID binder (both dispatch routes read that table).  guest.c,
      # the generated units and the corpus are byte-identical; a registration
      # that fails, or an inventory rename, leaves every Lua import on its
      # generic handler (recomp/vita/test_lua_import_fastdispatch.sh).
      set_property(SOURCE
        "${ISAAC_RUNTIME}/host_vita_lua.c"
        "${ISAAC_RUNTIME}/host_vita_import_id.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_LUA_IMPORT_FASTDISPATCH=1)
      message(STATUS
        "Isaac Lua import fast dispatch: 13 typed direct endpoints "
        "(rawgetp getmetatable type touserdata pushvalue rawget rawgeti settop "
        "gettop pushnil pushinteger pushnumber pushstring; plain owner word)")
    endif()
    if(ISAAC_VITA_LUA_NATIVE_INDEX)
      # perf:wf-prof-v6: 3f95c0 is 0.4 ms/frame of trampoline + import
      # dispatch in a quiet room and 1.4-1.8 ms with an EID description shown.
      set_property(SOURCE "${ISAAC_RUNTIME}/host_vita_lua.c"
        APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_LUA_NATIVE_INDEX=1)
      if(ISAAC_VITA_LUA_NATIVE_INDEX_VERIFY)
        set_property(SOURCE "${ISAAC_RUNTIME}/host_vita_lua.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_LUA_NATIVE_INDEX_VERIFY=1)
      endif()
      message(STATUS
        "Isaac Lua: native LuaBridge indexMetaMethod replay (sub_003f95c0), verify=${ISAAC_VITA_LUA_NATIVE_INDEX_VERIFY}")
    endif()
    if(ISAAC_VITA_LUA_NATIVE_GETCLASS)
      # Userdata::getClass (0x3f8e30) and Userdata::getExact (0x3f8c80) live in
      # guest_0120.c and have no caller inside that unit (917 + 65 direct
      # sites in other units plus the guest_table.c dispatch slot), so GNU ld
      # --wrap catches every edge at link time; __real_ keeps the translated
      # bodies for the fail-closed fallbacks.  The seam
      # (host_vita_lua_getclass.c) replays the happy path with the pinned
      # vanilla Lua 5.3.3 API under the bridge owner exported by
      # host_vita_lua.c.  No generated unit changes and the generator is
      # untouched.  Confirm both owners are present exactly once so a corpus
      # change fails loudly.
      foreach(ISAAC_LGC_OWNER_SYMBOL IN ITEMS sub_003f8e30 sub_003f8c80)
        set(ISAAC_LGC_OWNER)
        foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_CANONICAL_C)
          if(EXISTS "${ISAAC_GENERATED_SOURCE}")
            file(STRINGS "${ISAAC_GENERATED_SOURCE}"
              ISAAC_LGC_OWNER_MATCH
              LIMIT_COUNT 1
              REGEX "^void ${ISAAC_LGC_OWNER_SYMBOL}\\(CPU \\*__restrict c\\)$")
            if(ISAAC_LGC_OWNER_MATCH)
              list(APPEND ISAAC_LGC_OWNER "${ISAAC_GENERATED_SOURCE}")
            endif()
          endif()
        endforeach()
        list(LENGTH ISAAC_LGC_OWNER ISAAC_LGC_OWNER_COUNT)
        if(NOT ISAAC_LGC_OWNER_COUNT EQUAL 1)
          message(FATAL_ERROR
            "native getClass owner ${ISAAC_LGC_OWNER_SYMBOL} must resolve once; "
            "found ${ISAAC_LGC_OWNER_COUNT}")
        endif()
      endforeach()
      list(APPEND ISAAC_VITA_RUNTIME_SOURCES
        "${ISAAC_RUNTIME}/host_vita_lua_getclass.c")
      set_property(SOURCE
        "${ISAAC_RUNTIME}/host_vita_lua.c"
        "${ISAAC_RUNTIME}/host_vita_lua_getclass.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_LUA_NATIVE_GETCLASS=1)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/host_vita_lua_getclass.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_LUA_NATIVE_GETCLASS_WRAP=1
          ISAAC_VITA_LUA_NATIVE_GETCLASS_BUILD_ID=\"${ISAAC_VITA_GUEST_LINK_ID}\")
      if(ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY)
        set_property(SOURCE
          "${ISAAC_RUNTIME}/host_vita_lua_getclass.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY=1)
      endif()
      target_link_options(isaac_first_arm_fault PRIVATE
        -Wl,--wrap=sub_003f8e30
        -Wl,--wrap=sub_003f8c80)
      message(STATUS
        "Isaac Lua: native Userdata::getClass/getExact seam enabled "
        "(0x3f8e30/0x3f8c80 wrapped, verify=${ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY})")
    else()
      message(STATUS
        "Isaac Lua: Userdata::getClass/getExact stay translated")
    endif()
  endif()
  if(ISAAC_VITA_GAME_LOG_BATCH)
    # The policy lives exactly where the validated guest FILE token crosses
    # into native fflush.  Continue profiling reads only its cold snapshot.
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_crt.c"
      APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_GAME_LOG_BATCH=1)
    if(ISAAC_VITA_CONTINUE_PROFILE)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/kage_vita_continue_profile.c"
        APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_GAME_LOG_BATCH=1)
    endif()
    message(STATUS
      "Isaac guest logger: exact INFO flush batching enabled (N=8)")
  else()
    message(STATUS "Isaac guest logger: native per-record flush preserved")
  endif()
  if(ISAAC_VITA_SAVE_CHECKSUM_FASTPATH)
    # gen_all.py authenticates the complete frozen Checksum::Add body, ABI,
    # lazy table and caller census before emitting this marker.  Resolve its
    # manifest-verified owner without depending on batching-derived names and
    # reject a stale corpus which has the function but not the guarded seam.
    set(ISAAC_VITA_SAVE_CHECKSUM_FASTPATH_OWNER)
    set(ISAAC_VITA_SAVE_CHECKSUM_FASTPATH_DEFINITIONS 0)
    set(ISAAC_VITA_SAVE_CHECKSUM_FASTPATH_SEAMS 0)
    foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_CANONICAL_C)
      if(EXISTS "${ISAAC_GENERATED_SOURCE}")
        set(ISAAC_VITA_SAVE_CHECKSUM_ROOT_MATCH)
        set(ISAAC_VITA_SAVE_CHECKSUM_SEAM_MATCH)
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_SAVE_CHECKSUM_ROOT_MATCH
          LIMIT_COUNT 2
          REGEX "^void sub_0025b340\\(CPU \\*__restrict c\\)$")
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_SAVE_CHECKSUM_SEAM_MATCH
          LIMIT_COUNT 2
          REGEX "Frozen save checksum native fast path")
        list(LENGTH ISAAC_VITA_SAVE_CHECKSUM_ROOT_MATCH
          ISAAC_VITA_SAVE_CHECKSUM_ROOT_COUNT)
        list(LENGTH ISAAC_VITA_SAVE_CHECKSUM_SEAM_MATCH
          ISAAC_VITA_SAVE_CHECKSUM_SEAM_COUNT)
        math(EXPR ISAAC_VITA_SAVE_CHECKSUM_FASTPATH_DEFINITIONS
          "${ISAAC_VITA_SAVE_CHECKSUM_FASTPATH_DEFINITIONS} + ${ISAAC_VITA_SAVE_CHECKSUM_ROOT_COUNT}")
        math(EXPR ISAAC_VITA_SAVE_CHECKSUM_FASTPATH_SEAMS
          "${ISAAC_VITA_SAVE_CHECKSUM_FASTPATH_SEAMS} + ${ISAAC_VITA_SAVE_CHECKSUM_SEAM_COUNT}")
        if(ISAAC_VITA_SAVE_CHECKSUM_ROOT_COUNT GREATER 0)
          if(NOT ISAAC_VITA_SAVE_CHECKSUM_ROOT_COUNT EQUAL 1 OR
             NOT ISAAC_VITA_SAVE_CHECKSUM_SEAM_COUNT EQUAL 1)
            message(FATAL_ERROR
              "save-checksum owner has a missing or duplicate native seam: "
              "${ISAAC_GENERATED_SOURCE}")
          endif()
          list(APPEND ISAAC_VITA_SAVE_CHECKSUM_FASTPATH_OWNER
            "${ISAAC_GENERATED_SOURCE}")
        elseif(ISAAC_VITA_SAVE_CHECKSUM_SEAM_COUNT GREATER 0)
          message(FATAL_ERROR
            "save-checksum native seam escaped its frozen owner: "
            "${ISAAC_GENERATED_SOURCE}")
        endif()
      endif()
    endforeach()
    list(LENGTH ISAAC_VITA_SAVE_CHECKSUM_FASTPATH_OWNER
      ISAAC_VITA_SAVE_CHECKSUM_FASTPATH_OWNER_COUNT)
    if(NOT ISAAC_VITA_SAVE_CHECKSUM_FASTPATH_OWNER_COUNT EQUAL 1 OR
       NOT ISAAC_VITA_SAVE_CHECKSUM_FASTPATH_DEFINITIONS EQUAL 1 OR
       NOT ISAAC_VITA_SAVE_CHECKSUM_FASTPATH_SEAMS EQUAL 1)
      message(FATAL_ERROR
        "save-checksum fast path requires one generated owner/seam; found "
        "owners=${ISAAC_VITA_SAVE_CHECKSUM_FASTPATH_OWNER_COUNT}, "
        "definitions=${ISAAC_VITA_SAVE_CHECKSUM_FASTPATH_DEFINITIONS}, "
        "seams=${ISAAC_VITA_SAVE_CHECKSUM_FASTPATH_SEAMS}")
    endif()
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_save_checksum_native.c"
      "${ISAAC_RUNTIME}/host_vita_save_checksum_guest.c")
    set_property(SOURCE
      ${ISAAC_VITA_SAVE_CHECKSUM_FASTPATH_OWNER}
      "${ISAAC_RUNTIME}/host_vita_save_checksum_guest.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_SAVE_CHECKSUM_FASTPATH=1)
    message(STATUS
      "Isaac Vita save checksum: guarded native Checksum::Add in owner "
      "${ISAAC_VITA_SAVE_CHECKSUM_FASTPATH_OWNER}")
  endif()
  if(ISAAC_VITA_SAVE_READER_FASTPATH)
    # gen_all.py pins the complete 28-instruction leaf, its only address-take,
    # unique vtable slot and memcpy import before emitting this marker.
    set(ISAAC_VITA_SAVE_READER_FASTPATH_OWNER)
    set(ISAAC_VITA_SAVE_READER_FASTPATH_DEFINITIONS 0)
    set(ISAAC_VITA_SAVE_READER_FASTPATH_SEAMS 0)
    foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_CANONICAL_C)
      if(EXISTS "${ISAAC_GENERATED_SOURCE}")
        set(ISAAC_VITA_SAVE_READER_ROOT_MATCH)
        set(ISAAC_VITA_SAVE_READER_SEAM_MATCH)
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_SAVE_READER_ROOT_MATCH
          LIMIT_COUNT 2
          REGEX "^void sub_0025ba60\\(CPU \\*__restrict c\\)$")
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_SAVE_READER_SEAM_MATCH
          LIMIT_COUNT 2
          REGEX "Frozen memory Save Reader native fast path")
        list(LENGTH ISAAC_VITA_SAVE_READER_ROOT_MATCH
          ISAAC_VITA_SAVE_READER_ROOT_COUNT)
        list(LENGTH ISAAC_VITA_SAVE_READER_SEAM_MATCH
          ISAAC_VITA_SAVE_READER_SEAM_COUNT)
        math(EXPR ISAAC_VITA_SAVE_READER_FASTPATH_DEFINITIONS
          "${ISAAC_VITA_SAVE_READER_FASTPATH_DEFINITIONS} + ${ISAAC_VITA_SAVE_READER_ROOT_COUNT}")
        math(EXPR ISAAC_VITA_SAVE_READER_FASTPATH_SEAMS
          "${ISAAC_VITA_SAVE_READER_FASTPATH_SEAMS} + ${ISAAC_VITA_SAVE_READER_SEAM_COUNT}")
        if(ISAAC_VITA_SAVE_READER_ROOT_COUNT GREATER 0)
          if(NOT ISAAC_VITA_SAVE_READER_ROOT_COUNT EQUAL 1 OR
             NOT ISAAC_VITA_SAVE_READER_SEAM_COUNT EQUAL 1)
            message(FATAL_ERROR
              "save-reader owner has a missing or duplicate native seam: "
              "${ISAAC_GENERATED_SOURCE}")
          endif()
          list(APPEND ISAAC_VITA_SAVE_READER_FASTPATH_OWNER
            "${ISAAC_GENERATED_SOURCE}")
        elseif(ISAAC_VITA_SAVE_READER_SEAM_COUNT GREATER 0)
          message(FATAL_ERROR
            "save-reader native seam escaped its frozen owner: "
            "${ISAAC_GENERATED_SOURCE}")
        endif()
      endif()
    endforeach()
    list(LENGTH ISAAC_VITA_SAVE_READER_FASTPATH_OWNER
      ISAAC_VITA_SAVE_READER_FASTPATH_OWNER_COUNT)
    if(NOT ISAAC_VITA_SAVE_READER_FASTPATH_OWNER_COUNT EQUAL 1 OR
       NOT ISAAC_VITA_SAVE_READER_FASTPATH_DEFINITIONS EQUAL 1 OR
       NOT ISAAC_VITA_SAVE_READER_FASTPATH_SEAMS EQUAL 1)
      message(FATAL_ERROR
        "save-reader fast path requires one generated owner/seam; found "
        "owners=${ISAAC_VITA_SAVE_READER_FASTPATH_OWNER_COUNT}, "
        "definitions=${ISAAC_VITA_SAVE_READER_FASTPATH_DEFINITIONS}, "
        "seams=${ISAAC_VITA_SAVE_READER_FASTPATH_SEAMS}")
    endif()
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_save_reader_guest.c")
    set_property(SOURCE
      ${ISAAC_VITA_SAVE_READER_FASTPATH_OWNER}
      "${ISAAC_RUNTIME}/host_vita_save_reader_guest.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_SAVE_READER_FASTPATH=1)
    if(ISAAC_VITA_PHASE_PROFILE)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/host_vita_save_reader_guest.c"
        APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_PHASE_PROFILE=1)
    endif()
    message(STATUS
      "Isaac Vita Save Reader: guarded native memory-read leaf in owner "
      "${ISAAC_VITA_SAVE_READER_FASTPATH_OWNER}")
  endif()
  if(ISAAC_VITA_SAVE_READ32_FUSED)
    # gen_all.py pins the complete wrapper, all twenty direct callers and the
    # exact reader vtable edge before emitting this marker.
    set(ISAAC_VITA_SAVE_READ32_FUSED_OWNER)
    set(ISAAC_VITA_SAVE_READ32_FUSED_DEFINITIONS 0)
    set(ISAAC_VITA_SAVE_READ32_FUSED_SEAMS 0)
    foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_CANONICAL_C)
      if(EXISTS "${ISAAC_GENERATED_SOURCE}")
        set(ISAAC_VITA_SAVE_READ32_ROOT_MATCH)
        set(ISAAC_VITA_SAVE_READ32_SEAM_MATCH)
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_SAVE_READ32_ROOT_MATCH
          LIMIT_COUNT 2
          REGEX "^void sub_0052ea90\\(CPU \\*__restrict c\\)$")
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_SAVE_READ32_SEAM_MATCH
          LIMIT_COUNT 2
          REGEX "Frozen Save Reader read32/checksum dispatch fusion")
        list(LENGTH ISAAC_VITA_SAVE_READ32_ROOT_MATCH
          ISAAC_VITA_SAVE_READ32_ROOT_COUNT)
        list(LENGTH ISAAC_VITA_SAVE_READ32_SEAM_MATCH
          ISAAC_VITA_SAVE_READ32_SEAM_COUNT)
        math(EXPR ISAAC_VITA_SAVE_READ32_FUSED_DEFINITIONS
          "${ISAAC_VITA_SAVE_READ32_FUSED_DEFINITIONS} + ${ISAAC_VITA_SAVE_READ32_ROOT_COUNT}")
        math(EXPR ISAAC_VITA_SAVE_READ32_FUSED_SEAMS
          "${ISAAC_VITA_SAVE_READ32_FUSED_SEAMS} + ${ISAAC_VITA_SAVE_READ32_SEAM_COUNT}")
        if(ISAAC_VITA_SAVE_READ32_ROOT_COUNT GREATER 0)
          if(NOT ISAAC_VITA_SAVE_READ32_ROOT_COUNT EQUAL 1 OR
             NOT ISAAC_VITA_SAVE_READ32_SEAM_COUNT EQUAL 1)
            message(FATAL_ERROR
              "save-read32 owner has a missing or duplicate fused seam: "
              "${ISAAC_GENERATED_SOURCE}")
          endif()
          list(APPEND ISAAC_VITA_SAVE_READ32_FUSED_OWNER
            "${ISAAC_GENERATED_SOURCE}")
        elseif(ISAAC_VITA_SAVE_READ32_SEAM_COUNT GREATER 0)
          message(FATAL_ERROR
            "save-read32 fused seam escaped its frozen owner: "
            "${ISAAC_GENERATED_SOURCE}")
        endif()
      endif()
    endforeach()
    list(LENGTH ISAAC_VITA_SAVE_READ32_FUSED_OWNER
      ISAAC_VITA_SAVE_READ32_FUSED_OWNER_COUNT)
    if(NOT ISAAC_VITA_SAVE_READ32_FUSED_OWNER_COUNT EQUAL 1 OR
       NOT ISAAC_VITA_SAVE_READ32_FUSED_DEFINITIONS EQUAL 1 OR
       NOT ISAAC_VITA_SAVE_READ32_FUSED_SEAMS EQUAL 1)
      message(FATAL_ERROR
        "save-read32 fused path requires one generated owner/seam; found "
        "owners=${ISAAC_VITA_SAVE_READ32_FUSED_OWNER_COUNT}, "
        "definitions=${ISAAC_VITA_SAVE_READ32_FUSED_DEFINITIONS}, "
        "seams=${ISAAC_VITA_SAVE_READ32_FUSED_SEAMS}")
    endif()
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_save_read32_guest.c")
    set_property(SOURCE
      ${ISAAC_VITA_SAVE_READ32_FUSED_OWNER}
      "${ISAAC_RUNTIME}/host_vita_save_read32_guest.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_SAVE_READ32_FUSED=1)
    if(ISAAC_VITA_PHASE_PROFILE)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/host_vita_save_read32_guest.c"
        APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_PHASE_PROFILE=1)
    endif()
    message(STATUS
      "Isaac Vita Save read32: fused generated reader/checksum dispatch in "
      "${ISAAC_VITA_SAVE_READ32_FUSED_OWNER}")
  endif()
  if(ISAAC_VITA_SAVE_READER_DIRECT_EDGES)
    set(ISAAC_VITA_SAVE_READER_DIRECT_OWNER)
    set(ISAAC_VITA_SAVE_READER_DIRECT_DEFINITIONS 0)
    set(ISAAC_VITA_SAVE_READER_DIRECT_SEAMS 0)
    foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_CANONICAL_C)
      if(EXISTS "${ISAAC_GENERATED_SOURCE}")
        set(ISAAC_VITA_SAVE_READER_DIRECT_ROOT_MATCH)
        set(ISAAC_VITA_SAVE_READER_DIRECT_SEAM_MATCH)
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_SAVE_READER_DIRECT_ROOT_MATCH
          LIMIT_COUNT 4
          REGEX "^void sub_005(237a0|24320|282d0)\\(CPU \\*__restrict c\\)$")
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_SAVE_READER_DIRECT_SEAM_MATCH
          LIMIT_COUNT 327
          REGEX "isaac_vita_save_reader_direct_try\\(c, _target\\)")
        list(LENGTH ISAAC_VITA_SAVE_READER_DIRECT_ROOT_MATCH
          ISAAC_VITA_SAVE_READER_DIRECT_ROOT_COUNT)
        list(LENGTH ISAAC_VITA_SAVE_READER_DIRECT_SEAM_MATCH
          ISAAC_VITA_SAVE_READER_DIRECT_SEAM_COUNT)
        math(EXPR ISAAC_VITA_SAVE_READER_DIRECT_DEFINITIONS
          "${ISAAC_VITA_SAVE_READER_DIRECT_DEFINITIONS} + ${ISAAC_VITA_SAVE_READER_DIRECT_ROOT_COUNT}")
        math(EXPR ISAAC_VITA_SAVE_READER_DIRECT_SEAMS
          "${ISAAC_VITA_SAVE_READER_DIRECT_SEAMS} + ${ISAAC_VITA_SAVE_READER_DIRECT_SEAM_COUNT}")
        if(ISAAC_VITA_SAVE_READER_DIRECT_ROOT_COUNT GREATER 0)
          if(ISAAC_VITA_SAVE_READER_DIRECT_SEAM_COUNT EQUAL 0)
            message(FATAL_ERROR
              "save-reader direct owner has no authenticated seams: "
              "${ISAAC_GENERATED_SOURCE}")
          endif()
          list(APPEND ISAAC_VITA_SAVE_READER_DIRECT_OWNER
            "${ISAAC_GENERATED_SOURCE}")
        elseif(ISAAC_VITA_SAVE_READER_DIRECT_SEAM_COUNT GREATER 0)
          message(FATAL_ERROR
            "save-reader direct seam escaped its frozen owners: "
            "${ISAAC_GENERATED_SOURCE}")
        endif()
      endif()
    endforeach()
    list(LENGTH ISAAC_VITA_SAVE_READER_DIRECT_OWNER
      ISAAC_VITA_SAVE_READER_DIRECT_OWNER_COUNT)
    if(NOT ISAAC_VITA_SAVE_READER_DIRECT_OWNER_COUNT EQUAL 1 OR
       NOT ISAAC_VITA_SAVE_READER_DIRECT_DEFINITIONS EQUAL 3 OR
       NOT ISAAC_VITA_SAVE_READER_DIRECT_SEAMS EQUAL 326)
      message(FATAL_ERROR
        "save-reader direct edges require 3 roots/326 seams in one owner; "
        "owners=${ISAAC_VITA_SAVE_READER_DIRECT_OWNER_COUNT}, "
        "definitions=${ISAAC_VITA_SAVE_READER_DIRECT_DEFINITIONS}, "
        "seams=${ISAAC_VITA_SAVE_READER_DIRECT_SEAMS}")
    endif()
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_save_reader_direct.c")
    set_property(SOURCE
      ${ISAAC_VITA_SAVE_READER_DIRECT_OWNER}
      "${ISAAC_RUNTIME}/host_vita_save_reader_direct.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_SAVE_READER_DIRECT_EDGES=1)
    if(ISAAC_VITA_PHASE_PROFILE)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/host_vita_save_reader_direct.c"
        APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_PHASE_PROFILE=1)
    endif()
    message(STATUS
      "Isaac Vita Save Reader: 326 authenticated direct parser edges in "
      "${ISAAC_VITA_SAVE_READER_DIRECT_OWNER}")
  endif()
  if(ISAAC_VITA_MEMSET_THUNK_FASTPATH)
    # gen_all.py pins the one-instruction thunk, dense import identity and all
    # 350 rel32 callers before emitting the direct-dispatch marker.
    set(ISAAC_VITA_MEMSET_THUNK_FASTPATH_OWNER)
    set(ISAAC_VITA_MEMSET_THUNK_FASTPATH_DEFINITIONS 0)
    set(ISAAC_VITA_MEMSET_THUNK_FASTPATH_SEAMS 0)
    foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_CANONICAL_C)
      if(EXISTS "${ISAAC_GENERATED_SOURCE}")
        set(ISAAC_VITA_MEMSET_THUNK_ROOT_MATCH)
        set(ISAAC_VITA_MEMSET_THUNK_SEAM_MATCH)
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_MEMSET_THUNK_ROOT_MATCH
          LIMIT_COUNT 2
          REGEX "^void sub_005ec152\\(CPU \\*__restrict c\\)$")
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_MEMSET_THUNK_SEAM_MATCH
          LIMIT_COUNT 2
          REGEX "Authenticated memset IAT fast dispatch")
        list(LENGTH ISAAC_VITA_MEMSET_THUNK_ROOT_MATCH
          ISAAC_VITA_MEMSET_THUNK_ROOT_COUNT)
        list(LENGTH ISAAC_VITA_MEMSET_THUNK_SEAM_MATCH
          ISAAC_VITA_MEMSET_THUNK_SEAM_COUNT)
        math(EXPR ISAAC_VITA_MEMSET_THUNK_FASTPATH_DEFINITIONS
          "${ISAAC_VITA_MEMSET_THUNK_FASTPATH_DEFINITIONS} + ${ISAAC_VITA_MEMSET_THUNK_ROOT_COUNT}")
        math(EXPR ISAAC_VITA_MEMSET_THUNK_FASTPATH_SEAMS
          "${ISAAC_VITA_MEMSET_THUNK_FASTPATH_SEAMS} + ${ISAAC_VITA_MEMSET_THUNK_SEAM_COUNT}")
        if(ISAAC_VITA_MEMSET_THUNK_ROOT_COUNT GREATER 0)
          if(NOT ISAAC_VITA_MEMSET_THUNK_ROOT_COUNT EQUAL 1 OR
             NOT ISAAC_VITA_MEMSET_THUNK_SEAM_COUNT EQUAL 1)
            message(FATAL_ERROR
              "memset-thunk owner has a missing or duplicate direct seam: "
              "${ISAAC_GENERATED_SOURCE}")
          endif()
          list(APPEND ISAAC_VITA_MEMSET_THUNK_FASTPATH_OWNER
            "${ISAAC_GENERATED_SOURCE}")
        elseif(ISAAC_VITA_MEMSET_THUNK_SEAM_COUNT GREATER 0)
          message(FATAL_ERROR
            "memset-thunk direct seam escaped its frozen owner: "
            "${ISAAC_GENERATED_SOURCE}")
        endif()
      endif()
    endforeach()
    list(LENGTH ISAAC_VITA_MEMSET_THUNK_FASTPATH_OWNER
      ISAAC_VITA_MEMSET_THUNK_FASTPATH_OWNER_COUNT)
    if(NOT ISAAC_VITA_MEMSET_THUNK_FASTPATH_OWNER_COUNT EQUAL 1 OR
       NOT ISAAC_VITA_MEMSET_THUNK_FASTPATH_DEFINITIONS EQUAL 1 OR
       NOT ISAAC_VITA_MEMSET_THUNK_FASTPATH_SEAMS EQUAL 1)
      message(FATAL_ERROR
        "memset-thunk fast path requires one generated owner/seam; found "
        "owners=${ISAAC_VITA_MEMSET_THUNK_FASTPATH_OWNER_COUNT}, "
        "definitions=${ISAAC_VITA_MEMSET_THUNK_FASTPATH_DEFINITIONS}, "
        "seams=${ISAAC_VITA_MEMSET_THUNK_FASTPATH_SEAMS}")
    endif()
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_memset_thunk_direct.c")
    set_property(SOURCE
      ${ISAAC_VITA_MEMSET_THUNK_FASTPATH_OWNER}
      "${ISAAC_RUNTIME}/host_vita_memset_thunk_direct.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_MEMSET_THUNK_FASTPATH=1)
    if(ISAAC_VITA_PHASE_PROFILE)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/host_vita_memset_thunk_direct.c"
        APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_PHASE_PROFILE=1)
    endif()
    message(STATUS
      "Isaac Vita memset: authenticated direct import dispatch in "
      "${ISAAC_VITA_MEMSET_THUNK_FASTPATH_OWNER}")
  endif()
  if(ISAAC_VITA_KAGE_MUTEX_SEAM)
    # gen_all.py pins both frozen wrappers (refcount sync-IAT proof plus every
    # replayed instruction) and emits one guarded early return per root.  The
    # census requires each root definition exactly once with exactly one seam
    # line in the same generated owner; a seam anywhere else is a failure.
    set(ISAAC_VITA_KAGE_MUTEX_SEAM_OWNER)
    set(ISAAC_VITA_KAGE_MUTEX_SEAM_DEFINITIONS 0)
    set(ISAAC_VITA_KAGE_MUTEX_SEAM_SEAMS 0)
    foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_CANONICAL_C)
      if(EXISTS "${ISAAC_GENERATED_SOURCE}")
        set(ISAAC_VITA_KAGE_MUTEX_LOCK_ROOT_MATCH)
        set(ISAAC_VITA_KAGE_MUTEX_LOCK_SEAM_MATCH)
        set(ISAAC_VITA_KAGE_MUTEX_UNLOCK_ROOT_MATCH)
        set(ISAAC_VITA_KAGE_MUTEX_UNLOCK_SEAM_MATCH)
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_KAGE_MUTEX_LOCK_ROOT_MATCH
          LIMIT_COUNT 2
          REGEX "^void sub_00562e00\\(CPU \\*__restrict c\\)$")
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_KAGE_MUTEX_LOCK_SEAM_MATCH
          LIMIT_COUNT 2
          REGEX "KAGE Mutex::Lock\\(-1\\) native critical-section seam")
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_KAGE_MUTEX_UNLOCK_ROOT_MATCH
          LIMIT_COUNT 2
          REGEX "^void sub_00562ec0\\(CPU \\*__restrict c\\)$")
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_KAGE_MUTEX_UNLOCK_SEAM_MATCH
          LIMIT_COUNT 2
          REGEX "KAGE Mutex::Unlock native critical-section seam")
        list(LENGTH ISAAC_VITA_KAGE_MUTEX_LOCK_ROOT_MATCH
          ISAAC_VITA_KAGE_MUTEX_LOCK_ROOT_COUNT)
        list(LENGTH ISAAC_VITA_KAGE_MUTEX_LOCK_SEAM_MATCH
          ISAAC_VITA_KAGE_MUTEX_LOCK_SEAM_COUNT)
        list(LENGTH ISAAC_VITA_KAGE_MUTEX_UNLOCK_ROOT_MATCH
          ISAAC_VITA_KAGE_MUTEX_UNLOCK_ROOT_COUNT)
        list(LENGTH ISAAC_VITA_KAGE_MUTEX_UNLOCK_SEAM_MATCH
          ISAAC_VITA_KAGE_MUTEX_UNLOCK_SEAM_COUNT)
        math(EXPR ISAAC_VITA_KAGE_MUTEX_ROOT_COUNT
          "${ISAAC_VITA_KAGE_MUTEX_LOCK_ROOT_COUNT} + ${ISAAC_VITA_KAGE_MUTEX_UNLOCK_ROOT_COUNT}")
        math(EXPR ISAAC_VITA_KAGE_MUTEX_SEAM_COUNT
          "${ISAAC_VITA_KAGE_MUTEX_LOCK_SEAM_COUNT} + ${ISAAC_VITA_KAGE_MUTEX_UNLOCK_SEAM_COUNT}")
        math(EXPR ISAAC_VITA_KAGE_MUTEX_SEAM_DEFINITIONS
          "${ISAAC_VITA_KAGE_MUTEX_SEAM_DEFINITIONS} + ${ISAAC_VITA_KAGE_MUTEX_ROOT_COUNT}")
        math(EXPR ISAAC_VITA_KAGE_MUTEX_SEAM_SEAMS
          "${ISAAC_VITA_KAGE_MUTEX_SEAM_SEAMS} + ${ISAAC_VITA_KAGE_MUTEX_SEAM_COUNT}")
        if(ISAAC_VITA_KAGE_MUTEX_ROOT_COUNT GREATER 0)
          if(NOT ISAAC_VITA_KAGE_MUTEX_LOCK_ROOT_COUNT EQUAL
             ISAAC_VITA_KAGE_MUTEX_LOCK_SEAM_COUNT OR
             NOT ISAAC_VITA_KAGE_MUTEX_UNLOCK_ROOT_COUNT EQUAL
             ISAAC_VITA_KAGE_MUTEX_UNLOCK_SEAM_COUNT OR
             ISAAC_VITA_KAGE_MUTEX_LOCK_ROOT_COUNT GREATER 1 OR
             ISAAC_VITA_KAGE_MUTEX_UNLOCK_ROOT_COUNT GREATER 1)
            message(FATAL_ERROR
              "KAGE mutex owner has a missing or duplicate seam: "
              "${ISAAC_GENERATED_SOURCE}")
          endif()
          list(APPEND ISAAC_VITA_KAGE_MUTEX_SEAM_OWNER
            "${ISAAC_GENERATED_SOURCE}")
        elseif(ISAAC_VITA_KAGE_MUTEX_SEAM_COUNT GREATER 0)
          message(FATAL_ERROR
            "KAGE mutex seam escaped its frozen owner: "
            "${ISAAC_GENERATED_SOURCE}")
        endif()
      endif()
    endforeach()
    list(LENGTH ISAAC_VITA_KAGE_MUTEX_SEAM_OWNER
      ISAAC_VITA_KAGE_MUTEX_SEAM_OWNER_COUNT)
    if(ISAAC_VITA_KAGE_MUTEX_SEAM_OWNER_COUNT LESS 1 OR
       ISAAC_VITA_KAGE_MUTEX_SEAM_OWNER_COUNT GREATER 2 OR
       NOT ISAAC_VITA_KAGE_MUTEX_SEAM_DEFINITIONS EQUAL 2 OR
       NOT ISAAC_VITA_KAGE_MUTEX_SEAM_SEAMS EQUAL 2)
      message(FATAL_ERROR
        "KAGE mutex seam requires two generated roots with one seam each; found "
        "owners=${ISAAC_VITA_KAGE_MUTEX_SEAM_OWNER_COUNT}, "
        "definitions=${ISAAC_VITA_KAGE_MUTEX_SEAM_DEFINITIONS}, "
        "seams=${ISAAC_VITA_KAGE_MUTEX_SEAM_SEAMS}")
    endif()
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_kage_mutex_seam.c")
    set_property(SOURCE
      ${ISAAC_VITA_KAGE_MUTEX_SEAM_OWNER}
      "${ISAAC_RUNTIME}/host_vita_kage_mutex_seam.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_KAGE_MUTEX_SEAM=1)
    # The helper replays guest.c's inline critical-section path and its
    # census, so it must see exactly the census/sampler/latch definitions
    # guest.c sees (the blocks below add them to guest.c under the same
    # conditions), and the generated corpus's synthetic-stack mode, which
    # decides the low-water mark a translated run leaves.
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_kage_mutex_seam.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_SYNC_INLINE_FASTPATH=1)
    if(ISAAC_VITA_PHASE_PROFILE)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/host_vita_kage_mutex_seam.c"
        APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_PHASE_PROFILE=1)
      if(ISAAC_VITA_PROFILE_IMPORT_KINDS)
        set_property(SOURCE
          "${ISAAC_RUNTIME}/host_vita_kage_mutex_seam.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_PROFILE_IMPORT_KINDS=1)
      endif()
      if(ISAAC_VITA_GUEST_SAMPLER)
        set_property(SOURCE
          "${ISAAC_RUNTIME}/host_vita_kage_mutex_seam.c"
          APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_GUEST_SAMPLER=1)
      endif()
    endif()
    if(ISAAC_VITA_DISPATCH_TABLE)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/host_vita_kage_mutex_seam.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_GUEST_DISPATCH_TABLE=1)
    endif()
    if(NOT ISAAC_VITA_GUEST_STACK_GUARD)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/host_vita_kage_mutex_seam.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          GUEST_GENERATED_STACK_GUARD=0)
    endif()
    message(STATUS
      "Isaac Vita KAGE mutex: native Lock(-1)/Unlock seam in "
      "${ISAAC_VITA_KAGE_MUTEX_SEAM_OWNER}")
  endif()
  if(ISAAC_VITA_FLOOR_THUNK_FASTPATH)
    # gen_all.py pins the one-instruction thunk, dense import row 313 and all
    # 438 rel32 callers before emitting the seam; the seam text exists only
    # in a corpus generated with GUEST_FLOOR_THUNK_FASTPATH=1, so a default
    # corpus fails here loudly instead of silently building without it.
    set(ISAAC_VITA_FLOOR_THUNK_FASTPATH_OWNER)
    set(ISAAC_VITA_FLOOR_THUNK_FASTPATH_DEFINITIONS 0)
    set(ISAAC_VITA_FLOOR_THUNK_FASTPATH_SEAMS 0)
    foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_CANONICAL_C)
      if(EXISTS "${ISAAC_GENERATED_SOURCE}")
        set(ISAAC_VITA_FLOOR_THUNK_ROOT_MATCH)
        set(ISAAC_VITA_FLOOR_THUNK_SEAM_MATCH)
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_FLOOR_THUNK_ROOT_MATCH
          LIMIT_COUNT 2
          REGEX "^void sub_005ec3b2\\(CPU \\*__restrict c\\)$")
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_FLOOR_THUNK_SEAM_MATCH
          LIMIT_COUNT 2
          REGEX "Authenticated floor IAT fast dispatch")
        list(LENGTH ISAAC_VITA_FLOOR_THUNK_ROOT_MATCH
          ISAAC_VITA_FLOOR_THUNK_ROOT_COUNT)
        list(LENGTH ISAAC_VITA_FLOOR_THUNK_SEAM_MATCH
          ISAAC_VITA_FLOOR_THUNK_SEAM_COUNT)
        math(EXPR ISAAC_VITA_FLOOR_THUNK_FASTPATH_DEFINITIONS
          "${ISAAC_VITA_FLOOR_THUNK_FASTPATH_DEFINITIONS} + ${ISAAC_VITA_FLOOR_THUNK_ROOT_COUNT}")
        math(EXPR ISAAC_VITA_FLOOR_THUNK_FASTPATH_SEAMS
          "${ISAAC_VITA_FLOOR_THUNK_FASTPATH_SEAMS} + ${ISAAC_VITA_FLOOR_THUNK_SEAM_COUNT}")
        if(ISAAC_VITA_FLOOR_THUNK_ROOT_COUNT GREATER 0)
          if(NOT ISAAC_VITA_FLOOR_THUNK_ROOT_COUNT EQUAL 1 OR
             NOT ISAAC_VITA_FLOOR_THUNK_SEAM_COUNT EQUAL 1)
            message(FATAL_ERROR
              "floor-thunk owner has a missing or duplicate direct seam "
              "(regenerate with GUEST_FLOOR_THUNK_FASTPATH=1): "
              "${ISAAC_GENERATED_SOURCE}")
          endif()
          list(APPEND ISAAC_VITA_FLOOR_THUNK_FASTPATH_OWNER
            "${ISAAC_GENERATED_SOURCE}")
        elseif(ISAAC_VITA_FLOOR_THUNK_SEAM_COUNT GREATER 0)
          message(FATAL_ERROR
            "floor-thunk direct seam escaped its frozen owner: "
            "${ISAAC_GENERATED_SOURCE}")
        endif()
      endif()
    endforeach()
    list(LENGTH ISAAC_VITA_FLOOR_THUNK_FASTPATH_OWNER
      ISAAC_VITA_FLOOR_THUNK_FASTPATH_OWNER_COUNT)
    if(NOT ISAAC_VITA_FLOOR_THUNK_FASTPATH_OWNER_COUNT EQUAL 1 OR
       NOT ISAAC_VITA_FLOOR_THUNK_FASTPATH_DEFINITIONS EQUAL 1 OR
       NOT ISAAC_VITA_FLOOR_THUNK_FASTPATH_SEAMS EQUAL 1)
      message(FATAL_ERROR
        "floor-thunk fast path requires one generated owner/seam; found "
        "owners=${ISAAC_VITA_FLOOR_THUNK_FASTPATH_OWNER_COUNT}, "
        "definitions=${ISAAC_VITA_FLOOR_THUNK_FASTPATH_DEFINITIONS}, "
        "seams=${ISAAC_VITA_FLOOR_THUNK_FASTPATH_SEAMS}")
    endif()
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_floor_thunk_direct.c")
    set_property(SOURCE
      ${ISAAC_VITA_FLOOR_THUNK_FASTPATH_OWNER}
      "${ISAAC_RUNTIME}/host_vita_floor_thunk_direct.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_FLOOR_THUNK_FASTPATH=1)
    # The helper writes the receipts guest.c writes on the indexed direct
    # route, so it sees the same census/sampler macros as guest.c: the phase
    # profile counters, the per-import census array (dispatch table or
    # import-kinds owners) and the sampler's indirect-target word.
    if(ISAAC_VITA_PHASE_PROFILE)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/host_vita_floor_thunk_direct.c"
        APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_PHASE_PROFILE=1)
      if(ISAAC_VITA_DISPATCH_TABLE)
        set_property(SOURCE
          "${ISAAC_RUNTIME}/host_vita_floor_thunk_direct.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_GUEST_DISPATCH_TABLE=1)
      endif()
      if(ISAAC_VITA_PROFILE_IMPORT_KINDS)
        set_property(SOURCE
          "${ISAAC_RUNTIME}/host_vita_floor_thunk_direct.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_PROFILE_IMPORT_KINDS=1)
      endif()
      if(ISAAC_VITA_GUEST_SAMPLER)
        set_property(SOURCE
          "${ISAAC_RUNTIME}/host_vita_floor_thunk_direct.c"
          APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_GUEST_SAMPLER=1)
      endif()
    endif()
    message(STATUS
      "Isaac Vita floor: authenticated inline import at the thunk root in "
      "${ISAAC_VITA_FLOOR_THUNK_FASTPATH_OWNER}")
  endif()
  if(ISAAC_VITA_SHADER_ATTRIB_FASTPATH)
    # gen_all.py pins both Shader bodies (bytes, CFG order, format jump
    # tables, unique vtable slots, GL slot call operands) before emitting one
    # body marker each.  Exactly one generated owner with exactly one seam
    # per root; only the owner(s), the host replay TU, the bridge and the
    # dispatcher see the definition, so every other object - and each of
    # these with the option OFF - stays byte-identical.
    set(ISAAC_VITA_SHADER_ATTRIB_FASTPATH_OWNERS)
    foreach(ISAAC_SHADER_ATTRIB_ROOT IN ITEMS
        "sub_005672d0|Authenticated Shader::EnableAttribs host fast path"
        "sub_005673f0|Authenticated Shader::DisableAttribs host fast path")
      string(REPLACE "|" ";" ISAAC_SHADER_ATTRIB_ROOT "${ISAAC_SHADER_ATTRIB_ROOT}")
      list(GET ISAAC_SHADER_ATTRIB_ROOT 0 ISAAC_SHADER_ATTRIB_SYMBOL)
      list(GET ISAAC_SHADER_ATTRIB_ROOT 1 ISAAC_SHADER_ATTRIB_MARKER)
      set(ISAAC_SHADER_ATTRIB_OWNER)
      set(ISAAC_SHADER_ATTRIB_DEFINITIONS 0)
      set(ISAAC_SHADER_ATTRIB_SEAMS 0)
      foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_CANONICAL_C)
        if(EXISTS "${ISAAC_GENERATED_SOURCE}")
          set(ISAAC_SHADER_ATTRIB_ROOT_MATCH)
          set(ISAAC_SHADER_ATTRIB_SEAM_MATCH)
          file(STRINGS "${ISAAC_GENERATED_SOURCE}"
            ISAAC_SHADER_ATTRIB_ROOT_MATCH
            LIMIT_COUNT 2
            REGEX "^void ${ISAAC_SHADER_ATTRIB_SYMBOL}\\(CPU \\*__restrict c\\)$")
          file(STRINGS "${ISAAC_GENERATED_SOURCE}"
            ISAAC_SHADER_ATTRIB_SEAM_MATCH
            LIMIT_COUNT 2
            REGEX "${ISAAC_SHADER_ATTRIB_MARKER}")
          list(LENGTH ISAAC_SHADER_ATTRIB_ROOT_MATCH
            ISAAC_SHADER_ATTRIB_ROOT_COUNT)
          list(LENGTH ISAAC_SHADER_ATTRIB_SEAM_MATCH
            ISAAC_SHADER_ATTRIB_SEAM_COUNT)
          math(EXPR ISAAC_SHADER_ATTRIB_DEFINITIONS
            "${ISAAC_SHADER_ATTRIB_DEFINITIONS} + ${ISAAC_SHADER_ATTRIB_ROOT_COUNT}")
          math(EXPR ISAAC_SHADER_ATTRIB_SEAMS
            "${ISAAC_SHADER_ATTRIB_SEAMS} + ${ISAAC_SHADER_ATTRIB_SEAM_COUNT}")
          if(ISAAC_SHADER_ATTRIB_ROOT_COUNT GREATER 0)
            if(NOT ISAAC_SHADER_ATTRIB_ROOT_COUNT EQUAL 1 OR
               NOT ISAAC_SHADER_ATTRIB_SEAM_COUNT EQUAL 1)
              message(FATAL_ERROR
                "shader-attrib owner ${ISAAC_SHADER_ATTRIB_SYMBOL} has a "
                "missing or duplicate seam: ${ISAAC_GENERATED_SOURCE}")
            endif()
            list(APPEND ISAAC_SHADER_ATTRIB_OWNER "${ISAAC_GENERATED_SOURCE}")
          elseif(ISAAC_SHADER_ATTRIB_SEAM_COUNT GREATER 0)
            message(FATAL_ERROR
              "shader-attrib seam ${ISAAC_SHADER_ATTRIB_SYMBOL} escaped its "
              "frozen owner: ${ISAAC_GENERATED_SOURCE}")
          endif()
        endif()
      endforeach()
      list(LENGTH ISAAC_SHADER_ATTRIB_OWNER ISAAC_SHADER_ATTRIB_OWNER_COUNT)
      if(NOT ISAAC_SHADER_ATTRIB_OWNER_COUNT EQUAL 1 OR
         NOT ISAAC_SHADER_ATTRIB_DEFINITIONS EQUAL 1 OR
         NOT ISAAC_SHADER_ATTRIB_SEAMS EQUAL 1)
        message(FATAL_ERROR
          "shader-attrib fast path requires one generated owner/seam for "
          "${ISAAC_SHADER_ATTRIB_SYMBOL}; found "
          "owners=${ISAAC_SHADER_ATTRIB_OWNER_COUNT}, "
          "definitions=${ISAAC_SHADER_ATTRIB_DEFINITIONS}, "
          "seams=${ISAAC_SHADER_ATTRIB_SEAMS}")
      endif()
      list(APPEND ISAAC_VITA_SHADER_ATTRIB_FASTPATH_OWNERS
        ${ISAAC_SHADER_ATTRIB_OWNER})
    endforeach()
    list(REMOVE_DUPLICATES ISAAC_VITA_SHADER_ATTRIB_FASTPATH_OWNERS)
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_shader_attrib_fastpath.c")
    set_property(SOURCE
      ${ISAAC_VITA_SHADER_ATTRIB_FASTPATH_OWNERS}
      "${ISAAC_RUNTIME}/host_vita_shader_attrib_fastpath.c"
      "${ISAAC_RUNTIME}/gl_bridge.c"
      "${ISAAC_RUNTIME}/guest.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_SHADER_ATTRIB_FASTPATH=1)
    # The replay TU sits on the GL shim fast dispatch chain and mirrors the
    # sampler publish and the stall-probe breadcrumb guest_call would make.
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_shader_attrib_fastpath.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_GL_SHIM_FASTDISPATCH=1)
    if(ISAAC_VITA_GUEST_SAMPLER)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/host_vita_shader_attrib_fastpath.c"
        APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_GUEST_SAMPLER=1)
    endif()
    if(ISAAC_VITA_STALL_PROBE)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/host_vita_shader_attrib_fastpath.c"
        APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_STALL_PROBE=1)
    endif()
    message(STATUS
      "Isaac Vita Shader attributes: host replay of EnableAttribs/"
      "DisableAttribs in ${ISAAC_VITA_SHADER_ATTRIB_FASTPATH_OWNERS}")
  endif()
  if(ISAAC_VITA_FIOS_CACHE)
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_fios_cache.c")
    # Keep the policy out of generated code and unrelated host boundaries.
    # The entry owner starts it only after fixed image/stack reservation and
    # shuts it down on the common terminal tail.
    set_property(SOURCE
      "${ISAAC_RUNTIME}/entry_vita.c"
      APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_FIOS_CACHE=1)
    message(STATUS
      "Isaac Vita FIOS2 cache: enabled (0x9e000 USER_RW, 512 KiB data)")
  else()
    message(STATUS "Isaac Vita FIOS2 cache: disabled")
  endif()
  # The exact Open owner reaches this retained slot only after ordinary native
  # malloc failed.  It is a production safety net rather than an A/B policy;
  # keep its branch out of generated units and unrelated host boundaries.
  set_property(SOURCE
    "${ISAAC_RUNTIME}/host_vita_heap.c"
    APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_OGG_EMERGENCY=1)
  if(ISAAC_VITA_HEAP_LEDGER_MEMBLOCK)
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_heap_ledger_memblock.c")
    # Only the ownership-ledger table moves out of newlib.  Guest allocation
    # payloads retain the established exact-base newlib ABI.
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_heap.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_HEAP_LEDGER_MEMBLOCK=1)
  endif()
  if(ISAAC_VITA_HEAP_LEDGER_BACKSHIFT)
    # This is solely an implementation policy of the ownership table.  Keep
    # its A/B macro off headers, generated units and unrelated boundaries.
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_heap.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_HEAP_LEDGER_BACKSHIFT=1)
  endif()
  if(ISAAC_VITA_HEAP_OVERFLOW_MSPACE)
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_heap_overflow_mspace.c")
    # The public mspace module remains policy-free.  Only the heap router and
    # the startup boundary see the feature switch; generated units do not.
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_heap.c"
      "${ISAAC_RUNTIME}/entry_vita.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_HEAP_OVERFLOW_MSPACE=1)
  endif()
  if(ISAAC_VITA_ROOM_ENTRY_SLAB)
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_room_entry_slab.c")
    # Only the imported heap boundary recognizes the frozen operator-new
    # owner.  The slab module itself is policy-free and generated units never
    # see this A/B switch.
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_heap.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_ROOM_ENTRY_SLAB=1)
  endif()
  if(ISAAC_VITA_ROOM_ENTRY_HYBRID)
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_room_entry_external.c")
    # The manager is policy-free.  Only the heap initializer and slab page
    # allocator select the optional tier; generated units and unrelated host
    # boundaries retain byte-identical compile commands.
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_heap.c"
      "${ISAAC_RUNTIME}/host_vita_room_entry_slab.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_ROOM_ENTRY_HYBRID=1)
  endif()
  # Keep generic Vita guest.c oracles on the established name-dispatch ABI.
  # Only the production target links the full validated dense-ID closure.
  set_property(SOURCE
    "${ISAAC_RUNTIME}/guest.c"
    APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_IMPORT_ID_DISPATCH=1)
  if(ISAAC_VITA_SYNC_IMPORT_FASTPATH)
    # The complete frozen 413-row IAT registration remains the authority.
    # Only its dispatch owner skips the second family classification for the
    # two hot recursive-lock imports; the sync implementation is unchanged.
    set_property(SOURCE
      "${ISAAC_RUNTIME}/guest.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_SYNC_IMPORT_FASTPATH=1)
    message(STATUS
      "Isaac Vita sync import fast path: validated Enter/Leave IDs")
  endif()
  if(ISAAC_VITA_SYNC_INLINE_FASTPATH)
    # guest.c completes the provably single-owner success states of the two
    # validated imports inline at the pinned direct sites only
    # (guest_try_direct_sync_import_call; guest_call itself is byte-identical
    # ON/OFF); host_vita_sync.c owns the thread latch that disarms the inline
    # path for good if a second CPU identity ever binds or performs an
    # owner/depth operation.  Both compile the same header.
    set_property(SOURCE
      "${ISAAC_RUNTIME}/guest.c"
      "${ISAAC_RUNTIME}/host_vita_sync.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_SYNC_INLINE_FASTPATH=1)
    message(STATUS
      "Isaac Vita sync inline fast path: single-owner Enter/Leave states inline")
  endif()
  if(ISAAC_VITA_IAT_DIRECT)
    # guest.c owns the per-ID direct table (built from the same binding
    # guest_host_import_id uses, only after the 413-row registration passed)
    # and guest_import_call.  Generated units spell every IAT site through
    # GUEST_IMPORT_CALL/JMP; with GUEST_IMPORT_DIRECT=1 (below) those tokens
    # enter guest_import_call, otherwise the identical guest_call as before.
    set_property(SOURCE
      "${ISAAC_RUNTIME}/guest.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_IMPORT_DIRECT=1)
    message(STATUS
      "Isaac Vita IAT direct dispatch: generated import sites enter "
      "validated family endpoints (slot-token guarded, guest_call fallback)")
  endif()
  if(ISAAC_VITA_MEMORY_RANGE_GUARD)
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_memory.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_MEMORY_RANGE_GUARD=1)
  endif()
  if(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_archive_cache.c")
    # The cache is a Vita CRT implementation detail.  Keep its dispatch edge
    # off every generated unit and every unrelated host boundary.
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_crt.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_ARCHIVE_FILE_CACHE=1
        ISAAC_VITA_ARCHIVE_DIAG_BUILD_ID=\"${ISAAC_VITA_GUEST_LINK_ID}\")
  endif()
  if(ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT)
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_archive_receipt.c")
  endif()
  if(ISAAC_VITA_ASYNC_SAVE_WRITE)
    # Bundle35 evidence: the guest rewrites gamestateN.dat (68,741 bytes) on
    # every room change through a 1 KiB-buffered newlib "wb" FILE, so ~68
    # sceIoWrite syscalls plus open/truncate/close land inside Game::Update.
    # Only the CRT FILE-token owner, the four path-based import owners that
    # must observe read-your-writes, and the process-exit drain see the
    # macro.  Generated guest units retain byte-identical commands.
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_async_write.c")
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_crt.c"
      "${ISAAC_RUNTIME}/host_vita_filesystem.c"
      "${ISAAC_RUNTIME}/host_vita_startup.c"
      "${ISAAC_RUNTIME}/host_vita_find.c"
      "${ISAAC_RUNTIME}/host_vita_async_write.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_ASYNC_SAVE_WRITE=1)
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_async_write.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_ASYNC_WRITE_BUILD_ID=\"${ISAAC_VITA_GUEST_LINK_ID}\")
    message(STATUS
      "Isaac Vita asynchronous save writer: Documents/*.dat images "
      "written by one SceKernel worker thread")
  endif()
  if(ISAAC_VITA_CRT_SEEK_SHADOW)
    # prof-v6 sampler: File::GetSize = ftell; fseek(0,SEEK_END); ftell;
    # fseek(pos,SEEK_SET) burned 5.65 s of one 9.9 s floor load in newlib
    # fstat/lseek/16 KiB tail refills.  The CRT FILE-token owner records the
    # SEEK_END of a read-only newlib stream instead of performing it; the
    # loading screen only prints the counters.  No other TU sees the macro.
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_crt.c"
      "${ISAAC_RUNTIME}/kage_vita_loading.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_CRT_SEEK_SHADOW=1)
    message(STATUS
      "Isaac Vita CRT seek shadow: read-only SEEK_END/ftell answered from "
      "a cached fstat size (KAGE VITA CRT SEEK SHADOW stats lines)")
  endif()
  if(ISAAC_VITA_LOG_ASYNC)
    # Exactly these owners see the macro: the durable logger (platform.c),
    # the exit/flush path, the module, the ph120 emitter and the four other
    # game-thread per-window/per-event record owners.  Generated units and
    # the translated CPU core keep byte-identical commands; abort() is
    # wrapped so a failed assert drains the ring before the process dies.
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_log_async.c")
    set_property(SOURCE
      platform.c
      "${ISAAC_RUNTIME}/entry_vita.c"
      "${ISAAC_RUNTIME}/host_vita_log_async.c"
      "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"
      "${ISAAC_RUNTIME}/kage_vita_guest_sampler.c"
      "${ISAAC_RUNTIME}/kage_vita_backend.c"
      "${ISAAC_RUNTIME}/kage_vita_input.c"
      "${ISAAC_RUNTIME}/kage_vita_generated_hooks.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_LOG_ASYNC=1)
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_log_async.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_LOG_ASYNC_RING_KB=${ISAAC_VITA_LOG_ASYNC_RING_KB})
    target_link_options(isaac_first_arm_fault PRIVATE -Wl,--wrap=abort)
    message(STATUS
      "Isaac Vita asynchronous logger: ${ISAAC_VITA_LOG_ASYNC_RING_KB} KiB "
      "ring drained by one logger thread (priority 191, CPU 2); "
      "abort() flushes")
  else()
    message(STATUS
      "Isaac Vita asynchronous logger: disabled (synchronous sceIoWrite/"
      "sceClibPrintf on the producer thread)")
  endif()
  if(ISAAC_VITA_ARCHIVE_VALIDATION_SKIP)
    # The generator places the frozen seam in exactly one numbered unit, but
    # unit numbers are batching artefacts.  Find its exact public definition
    # in the already manifest-verified source set and change only that compile
    # command.  OFF therefore leaves every generated compile command intact.
    set(ISAAC_VITA_ARCHIVE_VALIDATION_SKIP_OWNER)
    foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_C)
      if(EXISTS "${ISAAC_GENERATED_SOURCE}")
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_ARCHIVE_VALIDATION_OWNER_MATCH
          LIMIT_COUNT 1
          REGEX "^void sub_00563880\\(CPU \\*__restrict c\\)$")
        if(ISAAC_ARCHIVE_VALIDATION_OWNER_MATCH)
          list(APPEND ISAAC_VITA_ARCHIVE_VALIDATION_SKIP_OWNER
            "${ISAAC_GENERATED_SOURCE}")
        endif()
      endif()
    endforeach()
    list(LENGTH ISAAC_VITA_ARCHIVE_VALIDATION_SKIP_OWNER
      ISAAC_VITA_ARCHIVE_VALIDATION_SKIP_OWNER_COUNT)
    if(NOT ISAAC_VITA_ARCHIVE_VALIDATION_SKIP_OWNER_COUNT EQUAL 1)
      message(FATAL_ERROR
        "archive-validation skip must own exactly one generated source; found "
        "${ISAAC_VITA_ARCHIVE_VALIDATION_SKIP_OWNER_COUNT}")
    endif()
    set_property(SOURCE
      ${ISAAC_VITA_ARCHIVE_VALIDATION_SKIP_OWNER}
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_ARCHIVE_VALIDATION_SKIP=1
        ISAAC_VITA_ARCHIVE_VALIDATION_SKIP_BUILD_ID=\"${ISAAC_VITA_GUEST_LINK_ID}\")
    if(ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT)
      # Only the exact generated owner calls the native receipt module.  The
      # module itself contains no generated-policy preprocessor branch.
      set_property(SOURCE
        ${ISAAC_VITA_ARCHIVE_VALIDATION_SKIP_OWNER}
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT=1)
    endif()
    message(STATUS
      "Isaac Vita archive validation skip: PE-checksum-gated owner "
      "${ISAAC_VITA_ARCHIVE_VALIDATION_SKIP_OWNER}")
  endif()
  if(ISAAC_VITA_VANILLA_KAGE_BUNDLE)
    # The Godhead seam is emitted through four shared Entity_Tear::Update
    # roots; the two KAGE seams each have one public root.  Resolve every
    # exact definition from the manifest-verified generated list rather than
    # relying on guest_XXXX.c batching names, then alter only those units.
    set(ISAAC_VITA_VANILLA_KAGE_BUNDLE_ROOTS
      sub_00242990
      sub_00242ff9
      sub_00245cc4
      sub_00245d42
      sub_005603f0
      sub_005607a0)
    set(ISAAC_VITA_VANILLA_KAGE_BUNDLE_OWNERS)
    foreach(ISAAC_VANILLA_KAGE_ROOT IN LISTS
            ISAAC_VITA_VANILLA_KAGE_BUNDLE_ROOTS)
      set(ISAAC_VANILLA_KAGE_ROOT_OWNER)
      foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_C)
        set(ISAAC_VANILLA_KAGE_ROOT_MATCH)
        if(EXISTS "${ISAAC_GENERATED_SOURCE}")
          file(STRINGS "${ISAAC_GENERATED_SOURCE}"
            ISAAC_VANILLA_KAGE_ROOT_MATCH
            LIMIT_COUNT 1
            REGEX "^void ${ISAAC_VANILLA_KAGE_ROOT}\\(CPU \\*__restrict c\\)$")
          if(ISAAC_VANILLA_KAGE_ROOT_MATCH)
            list(APPEND ISAAC_VANILLA_KAGE_ROOT_OWNER
              "${ISAAC_GENERATED_SOURCE}")
          endif()
        endif()
      endforeach()
      list(LENGTH ISAAC_VANILLA_KAGE_ROOT_OWNER
        ISAAC_VANILLA_KAGE_ROOT_OWNER_COUNT)
      if(NOT ISAAC_VANILLA_KAGE_ROOT_OWNER_COUNT EQUAL 1)
        message(FATAL_ERROR
          "vanilla/KAGE root ${ISAAC_VANILLA_KAGE_ROOT} must own exactly "
          "one generated source; found ${ISAAC_VANILLA_KAGE_ROOT_OWNER_COUNT}")
      endif()
      list(APPEND ISAAC_VITA_VANILLA_KAGE_BUNDLE_OWNERS
        ${ISAAC_VANILLA_KAGE_ROOT_OWNER})
    endforeach()
    list(REMOVE_DUPLICATES ISAAC_VITA_VANILLA_KAGE_BUNDLE_OWNERS)
    list(LENGTH ISAAC_VITA_VANILLA_KAGE_BUNDLE_OWNERS
      ISAAC_VITA_VANILLA_KAGE_BUNDLE_OWNER_COUNT)
    if(ISAAC_VITA_VANILLA_KAGE_BUNDLE_OWNER_COUNT LESS 1 OR
       ISAAC_VITA_VANILLA_KAGE_BUNDLE_OWNER_COUNT GREATER 6)
      message(FATAL_ERROR
        "vanilla/KAGE generated owner-unit census is impossible: "
        "${ISAAC_VITA_VANILLA_KAGE_BUNDLE_OWNER_COUNT}")
    endif()
    set_property(SOURCE
      ${ISAAC_VITA_VANILLA_KAGE_BUNDLE_OWNERS}
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_VANILLA_KAGE_BUNDLE=1)
    message(STATUS
      "Isaac Vita vanilla/KAGE bundle: enabled in "
      "${ISAAC_VITA_VANILLA_KAGE_BUNDLE_OWNER_COUNT} generated units")
  endif()
  if(ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE)
    # The generator pins the post-load seam in OptionsConfig::Load.  Locate
    # its manifest-verified owner by public definition instead of depending
    # on the batching-derived guest_XXXX.c name.
    set(ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE_OWNER)
    foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_C)
      if(EXISTS "${ISAAC_GENERATED_SOURCE}")
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_ORIGINAL_EFFECTS_PROFILE_OWNER_MATCH
          LIMIT_COUNT 1
          REGEX "^void sub_0047fbe0\\(CPU \\*__restrict c\\)$")
        if(ISAAC_ORIGINAL_EFFECTS_PROFILE_OWNER_MATCH)
          list(APPEND ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE_OWNER
            "${ISAAC_GENERATED_SOURCE}")
        endif()
      endif()
    endforeach()
    list(LENGTH ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE_OWNER
      ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE_OWNER_COUNT)
    if(NOT ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE_OWNER_COUNT EQUAL 1)
      message(FATAL_ERROR
        "original-Vita effects profile must own exactly one generated source; found "
        "${ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE_OWNER_COUNT}")
    endif()
    set_property(SOURCE
      ${ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE_OWNER}
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE=1)
    message(STATUS
      "Isaac Vita original effects profile: C/S/F disabled in owner "
      "${ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE_OWNER}")
  endif()
  if(ISAAC_VITA_PILL_BLOOM_BYPASS)
    # The generator pins both pill-timer Bloom branches inside Game::Render.
    # Resolve the exact public owner from the manifest-verified corpus; the
    # batching-derived guest_XXXX.c name is deliberately not part of policy.
    set(ISAAC_VITA_PILL_BLOOM_BYPASS_OWNER)
    foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_C)
      if(EXISTS "${ISAAC_GENERATED_SOURCE}")
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_PILL_BLOOM_BYPASS_OWNER_MATCH
          LIMIT_COUNT 1
          REGEX "^void sub_002ce770\\(CPU \\*__restrict c\\)$")
        if(ISAAC_PILL_BLOOM_BYPASS_OWNER_MATCH)
          list(APPEND ISAAC_VITA_PILL_BLOOM_BYPASS_OWNER
            "${ISAAC_GENERATED_SOURCE}")
        endif()
      endif()
    endforeach()
    list(LENGTH ISAAC_VITA_PILL_BLOOM_BYPASS_OWNER
      ISAAC_VITA_PILL_BLOOM_BYPASS_OWNER_COUNT)
    if(NOT ISAAC_VITA_PILL_BLOOM_BYPASS_OWNER_COUNT EQUAL 1)
      message(FATAL_ERROR
        "pill-Bloom bypass must own exactly one generated source; found "
        "${ISAAC_VITA_PILL_BLOOM_BYPASS_OWNER_COUNT}")
    endif()
    set_property(SOURCE
      ${ISAAC_VITA_PILL_BLOOM_BYPASS_OWNER}
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_PILL_BLOOM_BYPASS=1)
    if(ISAAC_VITA_PHASE_PROFILE)
      set_property(SOURCE
        ${ISAAC_VITA_PILL_BLOOM_BYPASS_OWNER}
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_PHASE_RECEIPTS=1)
    endif()
    set_property(SOURCE
      "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_PILL_BLOOM_BYPASS=1)
    message(STATUS
      "Isaac Vita selective pill Bloom bypass: enabled in owner "
      "${ISAAC_VITA_PILL_BLOOM_BYPASS_OWNER}")
  endif()
  if(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES)
    # One owner changes the exact Bloom Surface creation arguments and the
    # other counts its paired capture/composite.  Definition counting plus
    # source de-duplication keeps this independent of generated batching.
    set(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES_OWNERS)
    set(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES_DEFINITIONS 0)
    foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_C)
      if(EXISTS "${ISAAC_GENERATED_SOURCE}")
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES_MATCHES
          LIMIT_COUNT 2
          REGEX "^void sub_002c(7e70|e770)\\(CPU \\*__restrict c\\)$")
        list(LENGTH ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES_MATCHES
          ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES_MATCH_COUNT)
        if(ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES_MATCH_COUNT GREATER 0)
          math(EXPR ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES_DEFINITIONS
            "${ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES_DEFINITIONS} + ${ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES_MATCH_COUNT}")
          list(APPEND ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES_OWNERS
            "${ISAAC_GENERATED_SOURCE}")
        endif()
      endif()
    endforeach()
    if(NOT ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES_DEFINITIONS EQUAL 2)
      message(FATAL_ERROR
        "transient-Bloom half-res must own exactly two generated definitions; found "
        "${ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES_DEFINITIONS}")
    endif()
    list(REMOVE_DUPLICATES ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES_OWNERS)
    set_property(SOURCE
      ${ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES_OWNERS}
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES=1)
    if(ISAAC_VITA_PHASE_PROFILE)
      set_property(SOURCE
        ${ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES_OWNERS}
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_PHASE_RECEIPTS=1)
    endif()
    set_property(SOURCE
      "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES=1)
    message(STATUS
      "Isaac Vita transient-Bloom half-resolution: enabled in owners "
      "${ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES_OWNERS}")
  endif()
  if(ISAAC_VITA_LASER_PROFILE)
    # Both generated functions are exact frozen-PE owners.  Count definitions,
    # then de-duplicate source files so this remains independent of batching
    # (the two owners currently share one guest_XXXX.c file).
    set(ISAAC_VITA_LASER_PROFILE_OWNERS)
    set(ISAAC_VITA_LASER_PROFILE_DEFINITIONS 0)
    foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_C)
      if(EXISTS "${ISAAC_GENERATED_SOURCE}")
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_LASER_PROFILE_MATCHES
          LIMIT_COUNT 2
          REGEX "^void sub_004d(1330|5090)\\(CPU \\*__restrict c\\)$")
        list(LENGTH ISAAC_VITA_LASER_PROFILE_MATCHES
          ISAAC_VITA_LASER_PROFILE_MATCH_COUNT)
        if(ISAAC_VITA_LASER_PROFILE_MATCH_COUNT GREATER 0)
          math(EXPR ISAAC_VITA_LASER_PROFILE_DEFINITIONS
            "${ISAAC_VITA_LASER_PROFILE_DEFINITIONS} + ${ISAAC_VITA_LASER_PROFILE_MATCH_COUNT}")
          list(APPEND ISAAC_VITA_LASER_PROFILE_OWNERS
            "${ISAAC_GENERATED_SOURCE}")
        endif()
      endif()
    endforeach()
    if(NOT ISAAC_VITA_LASER_PROFILE_DEFINITIONS EQUAL 2)
      message(FATAL_ERROR
        "laser profile must own exactly two generated definitions; found "
        "${ISAAC_VITA_LASER_PROFILE_DEFINITIONS}")
    endif()
    list(REMOVE_DUPLICATES ISAAC_VITA_LASER_PROFILE_OWNERS)
    set_property(SOURCE
      ${ISAAC_VITA_LASER_PROFILE_OWNERS}
      APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_LASER_PROFILE=1)
    set_property(SOURCE
      "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"
      APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_LASER_PROFILE=1)
    message(STATUS
      "Isaac Vita laser Render/Shadow profile: enabled in owners "
      "${ISAAC_VITA_LASER_PROFILE_OWNERS}")
  endif()
  if(ISAAC_VITA_POOP_FX_PROFILE)
    # Pin both exact generated definitions, then de-duplicate source files so
    # owner selection is independent of the generator's batching layout.
    set(ISAAC_VITA_POOP_FX_OWNERS)
    set(ISAAC_VITA_POOP_FX_DEFINITIONS 0)
    foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_C)
      if(EXISTS "${ISAAC_GENERATED_SOURCE}")
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_POOP_FX_MATCHES
          LIMIT_COUNT 2
          REGEX "^void sub_004e(c3b0|daa0)\\(CPU \\*__restrict c\\)$")
        list(LENGTH ISAAC_VITA_POOP_FX_MATCHES
          ISAAC_VITA_POOP_FX_MATCH_COUNT)
        if(ISAAC_VITA_POOP_FX_MATCH_COUNT GREATER 0)
          math(EXPR ISAAC_VITA_POOP_FX_DEFINITIONS
            "${ISAAC_VITA_POOP_FX_DEFINITIONS} + ${ISAAC_VITA_POOP_FX_MATCH_COUNT}")
          list(APPEND ISAAC_VITA_POOP_FX_OWNERS
            "${ISAAC_GENERATED_SOURCE}")
        endif()
      endif()
    endforeach()
    if(NOT ISAAC_VITA_POOP_FX_DEFINITIONS EQUAL 2)
      message(FATAL_ERROR
        "PoopFx profile must own exactly two generated definitions; found "
        "${ISAAC_VITA_POOP_FX_DEFINITIONS}")
    endif()
    list(REMOVE_DUPLICATES ISAAC_VITA_POOP_FX_OWNERS)
    set_property(SOURCE
      ${ISAAC_VITA_POOP_FX_OWNERS}
      APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_POOP_FX_PROFILE=1)
    if(ISAAC_VITA_PHASE_PROFILE)
      set_property(SOURCE
        ${ISAAC_VITA_POOP_FX_OWNERS}
        APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_PHASE_RECEIPTS=1)
    endif()
    set_property(SOURCE
      "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"
      APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_POOP_FX_PROFILE=1)
    if(ISAAC_VITA_POOP_FX_SINGLE_CLOUD)
      set_property(SOURCE
        ${ISAAC_VITA_POOP_FX_OWNERS}
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_POOP_FX_SINGLE_CLOUD=1)
    endif()
    message(STATUS
      "Isaac Vita PoopFx profile: enabled in owners "
      "${ISAAC_VITA_POOP_FX_OWNERS}; single-cloud=${ISAAC_VITA_POOP_FX_SINGLE_CLOUD}")
  endif()
  if(ISAAC_VITA_PNG_NATIVE_UNFILTER)
    # The generated hook exists only in the exact frozen row target.  Resolve
    # its manifest-verified owner instead of depending on a batching-derived
    # guest_NNNN.c name.  The shim leases the exact three allocation bases,
    # then either consumes only the guest return word or leaves CPU/stack/row
    # state untouched for the translated body below it.
    set(ISAAC_VITA_PNG_NATIVE_UNFILTER_OWNER)
    foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_C)
      set(ISAAC_VITA_PNG_NATIVE_UNFILTER_MATCH)
      if(EXISTS "${ISAAC_GENERATED_SOURCE}")
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_PNG_NATIVE_UNFILTER_MATCH
          LIMIT_COUNT 1
          REGEX "^void sub_005c6bf0\\(CPU \\*__restrict c\\)$")
        if(ISAAC_VITA_PNG_NATIVE_UNFILTER_MATCH)
          list(APPEND ISAAC_VITA_PNG_NATIVE_UNFILTER_OWNER
            "${ISAAC_GENERATED_SOURCE}")
        endif()
      endif()
    endforeach()
    list(LENGTH ISAAC_VITA_PNG_NATIVE_UNFILTER_OWNER
      ISAAC_VITA_PNG_NATIVE_UNFILTER_OWNER_COUNT)
    if(NOT ISAAC_VITA_PNG_NATIVE_UNFILTER_OWNER_COUNT EQUAL 1)
      message(FATAL_ERROR
        "PNG native-unfilter target must own exactly one generated source; "
        "found ${ISAAC_VITA_PNG_NATIVE_UNFILTER_OWNER_COUNT}")
    endif()
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_png_unfilter_native.c"
      "${ISAAC_RUNTIME}/host_vita_png_unfilter_guest.c")
    set_property(SOURCE
      ${ISAAC_VITA_PNG_NATIVE_UNFILTER_OWNER}
      "${ISAAC_RUNTIME}/host_vita_heap.c"
      "${ISAAC_RUNTIME}/host_vita_png_unfilter_native.c"
      "${ISAAC_RUNTIME}/host_vita_png_unfilter_guest.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_PNG_NATIVE_UNFILTER=1)
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_png_unfilter_guest.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_HEAP_RANGE_LEASE=1)
    message(STATUS
      "Isaac Vita PNG native unfilter: guarded experimental A/B in owner "
      "${ISAAC_VITA_PNG_NATIVE_UNFILTER_OWNER}")
  endif()
  if(ISAAC_VITA_PNG_INFLATE_FLUSH_FASTPATH)
    # Resolve the exact generated inflate_flush owner.  Only it sees the
    # entry hook; the native shim and heap ledger share the opt-in definition
    # needed for exact-base leases.
    set(ISAAC_VITA_PNG_INFLATE_FLUSH_OWNER)
    foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_C)
      set(ISAAC_VITA_PNG_INFLATE_FLUSH_MATCH)
      if(EXISTS "${ISAAC_GENERATED_SOURCE}")
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_PNG_INFLATE_FLUSH_MATCH
          LIMIT_COUNT 1
          REGEX "^void sub_005d6720\\(CPU \\*__restrict c\\)$")
        if(ISAAC_VITA_PNG_INFLATE_FLUSH_MATCH)
          list(APPEND ISAAC_VITA_PNG_INFLATE_FLUSH_OWNER
            "${ISAAC_GENERATED_SOURCE}")
        endif()
      endif()
    endforeach()
    list(LENGTH ISAAC_VITA_PNG_INFLATE_FLUSH_OWNER
      ISAAC_VITA_PNG_INFLATE_FLUSH_OWNER_COUNT)
    if(NOT ISAAC_VITA_PNG_INFLATE_FLUSH_OWNER_COUNT EQUAL 1)
      message(FATAL_ERROR
        "PNG inflate_flush target must own exactly one generated source; "
        "found ${ISAAC_VITA_PNG_INFLATE_FLUSH_OWNER_COUNT}")
    endif()
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_zlib_inflate_flush_native.c"
      "${ISAAC_RUNTIME}/host_vita_zlib_inflate_flush_guest.c")
    set_property(SOURCE
      ${ISAAC_VITA_PNG_INFLATE_FLUSH_OWNER}
      "${ISAAC_RUNTIME}/host_vita_heap.c"
      "${ISAAC_RUNTIME}/host_vita_zlib_inflate_flush_guest.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_PNG_INFLATE_FLUSH_FASTPATH=1)
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_zlib_inflate_flush_guest.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_HEAP_RANGE_LEASE=1)
    message(STATUS
      "Isaac Vita PNG inflate_flush fast path: guarded native leaf in owner "
      "${ISAAC_VITA_PNG_INFLATE_FLUSH_OWNER}")
  endif()
  if(ISAAC_VITA_PNG_CRC32_FASTPATH)
    # gen_all.py pins the complete byte loop, standard CRC table and both
    # libpng callers before emitting the one entry seam.
    set(ISAAC_VITA_PNG_CRC32_OWNER)
    set(ISAAC_VITA_PNG_CRC32_DEFINITIONS 0)
    set(ISAAC_VITA_PNG_CRC32_SEAMS 0)
    foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_C)
      if(EXISTS "${ISAAC_GENERATED_SOURCE}")
        set(ISAAC_VITA_PNG_CRC32_ROOT_MATCH)
        set(ISAAC_VITA_PNG_CRC32_SEAM_MATCH)
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_PNG_CRC32_ROOT_MATCH
          LIMIT_COUNT 2
          REGEX "^void sub_005c3320\\(CPU \\*__restrict c\\)$")
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_PNG_CRC32_SEAM_MATCH
          LIMIT_COUNT 2
          REGEX "Frozen zlib CRC32 native byte-loop fast path")
        list(LENGTH ISAAC_VITA_PNG_CRC32_ROOT_MATCH
          ISAAC_VITA_PNG_CRC32_ROOT_COUNT)
        list(LENGTH ISAAC_VITA_PNG_CRC32_SEAM_MATCH
          ISAAC_VITA_PNG_CRC32_SEAM_COUNT)
        math(EXPR ISAAC_VITA_PNG_CRC32_DEFINITIONS
          "${ISAAC_VITA_PNG_CRC32_DEFINITIONS} + ${ISAAC_VITA_PNG_CRC32_ROOT_COUNT}")
        math(EXPR ISAAC_VITA_PNG_CRC32_SEAMS
          "${ISAAC_VITA_PNG_CRC32_SEAMS} + ${ISAAC_VITA_PNG_CRC32_SEAM_COUNT}")
        if(ISAAC_VITA_PNG_CRC32_ROOT_COUNT GREATER 0)
          list(APPEND ISAAC_VITA_PNG_CRC32_OWNER
            "${ISAAC_GENERATED_SOURCE}")
        elseif(ISAAC_VITA_PNG_CRC32_SEAM_COUNT GREATER 0)
          message(FATAL_ERROR
            "PNG CRC32 native seam escaped its frozen owner: "
            "${ISAAC_GENERATED_SOURCE}")
        endif()
      endif()
    endforeach()
    list(LENGTH ISAAC_VITA_PNG_CRC32_OWNER
      ISAAC_VITA_PNG_CRC32_OWNER_COUNT)
    if(NOT ISAAC_VITA_PNG_CRC32_OWNER_COUNT EQUAL 1 OR
       NOT ISAAC_VITA_PNG_CRC32_DEFINITIONS EQUAL 1 OR
       NOT ISAAC_VITA_PNG_CRC32_SEAMS EQUAL 1)
      message(FATAL_ERROR
        "PNG CRC32 fast path requires one generated owner/seam; found "
        "owners=${ISAAC_VITA_PNG_CRC32_OWNER_COUNT}, "
        "definitions=${ISAAC_VITA_PNG_CRC32_DEFINITIONS}, "
        "seams=${ISAAC_VITA_PNG_CRC32_SEAMS}")
    endif()
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_png_crc32_native.c"
      "${ISAAC_RUNTIME}/host_vita_png_crc32_guest.c")
    set_property(SOURCE
      ${ISAAC_VITA_PNG_CRC32_OWNER}
      "${ISAAC_RUNTIME}/host_vita_png_crc32_guest.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_PNG_CRC32_FASTPATH=1)
    message(STATUS
      "Isaac Vita PNG CRC32 fast path: native bulk byte loop in owner "
      "${ISAAC_VITA_PNG_CRC32_OWNER}")
  endif()
  if(ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH)
    # gen_all.py pins the complete miniz coroutine, all 34 switch targets and
    # its sole ArchivedFile caller before emitting the one entry seam.
    set(ISAAC_VITA_ARCHIVE_MINIZ_OWNER)
    set(ISAAC_VITA_ARCHIVE_MINIZ_DEFINITIONS 0)
    set(ISAAC_VITA_ARCHIVE_MINIZ_SEAMS 0)
    foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_C)
      if(EXISTS "${ISAAC_GENERATED_SOURCE}")
        set(ISAAC_VITA_ARCHIVE_MINIZ_ROOT_MATCH)
        set(ISAAC_VITA_ARCHIVE_MINIZ_SEAM_MATCH)
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_ARCHIVE_MINIZ_ROOT_MATCH
          LIMIT_COUNT 2
          REGEX "^void sub_005aeb00\\(CPU \\*__restrict c\\)$")
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_VITA_ARCHIVE_MINIZ_SEAM_MATCH
          LIMIT_COUNT 2
          REGEX "isaac_vita_archive_miniz_guest_try\\(c\\)")
        list(LENGTH ISAAC_VITA_ARCHIVE_MINIZ_ROOT_MATCH
          ISAAC_VITA_ARCHIVE_MINIZ_ROOT_COUNT)
        list(LENGTH ISAAC_VITA_ARCHIVE_MINIZ_SEAM_MATCH
          ISAAC_VITA_ARCHIVE_MINIZ_SEAM_COUNT)
        math(EXPR ISAAC_VITA_ARCHIVE_MINIZ_DEFINITIONS
          "${ISAAC_VITA_ARCHIVE_MINIZ_DEFINITIONS} + ${ISAAC_VITA_ARCHIVE_MINIZ_ROOT_COUNT}")
        math(EXPR ISAAC_VITA_ARCHIVE_MINIZ_SEAMS
          "${ISAAC_VITA_ARCHIVE_MINIZ_SEAMS} + ${ISAAC_VITA_ARCHIVE_MINIZ_SEAM_COUNT}")
        if(ISAAC_VITA_ARCHIVE_MINIZ_ROOT_COUNT GREATER 0)
          list(APPEND ISAAC_VITA_ARCHIVE_MINIZ_OWNER
            "${ISAAC_GENERATED_SOURCE}")
        elseif(ISAAC_VITA_ARCHIVE_MINIZ_SEAM_COUNT GREATER 0)
          message(FATAL_ERROR
            "Archive MiniZ native seam escaped its frozen owner: "
            "${ISAAC_GENERATED_SOURCE}")
        endif()
      endif()
    endforeach()
    list(LENGTH ISAAC_VITA_ARCHIVE_MINIZ_OWNER
      ISAAC_VITA_ARCHIVE_MINIZ_OWNER_COUNT)
    if(NOT ISAAC_VITA_ARCHIVE_MINIZ_OWNER_COUNT EQUAL 1 OR
       NOT ISAAC_VITA_ARCHIVE_MINIZ_DEFINITIONS EQUAL 1 OR
       NOT ISAAC_VITA_ARCHIVE_MINIZ_SEAMS EQUAL 1)
      message(FATAL_ERROR
        "Archive MiniZ fast path requires one generated owner/seam; found "
        "owners=${ISAAC_VITA_ARCHIVE_MINIZ_OWNER_COUNT}, "
        "definitions=${ISAAC_VITA_ARCHIVE_MINIZ_DEFINITIONS}, "
        "seams=${ISAAC_VITA_ARCHIVE_MINIZ_SEAMS}")
    endif()
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_archive_miniz_native.c"
      "${ISAAC_RUNTIME}/host_vita_archive_miniz_guest.c")
    set_property(SOURCE
      ${ISAAC_VITA_ARCHIVE_MINIZ_OWNER}
      "${ISAAC_RUNTIME}/host_vita_heap.c"
      "${ISAAC_RUNTIME}/host_vita_archive_miniz_guest.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH=1)
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_archive_miniz_guest.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_HEAP_RANGE_LEASE=1)
    message(STATUS
      "Isaac Vita archive MiniZ fast path: guarded native coroutine in owner "
      "${ISAAC_VITA_ARCHIVE_MINIZ_OWNER}")
  endif()
  if(ISAAC_VITA_PNG_DECODE_PROFILE)
    # gen_all proves eight unique frozen owners.  Six public roots contain the
    # seven image/filter/inflate/fast hooks; resolve each independently, then
    # de-duplicate batching-derived source files.  Target-only proof roots stay
    # byte-identical in profile-only builds.
    set(ISAAC_VITA_PNG_DECODE_PROFILE_ROOTS
      sub_005a0c50
      sub_005a0cd0
      sub_005b1500
      sub_005c5fb0
      sub_005c6fa0
      sub_005d6d80)
    set(ISAAC_VITA_PNG_DECODE_PROFILE_OWNERS)
    set(ISAAC_VITA_PNG_DECODE_PROFILE_DEFINITIONS 0)
    foreach(ISAAC_PNG_PROFILE_ROOT IN LISTS
            ISAAC_VITA_PNG_DECODE_PROFILE_ROOTS)
      set(ISAAC_PNG_PROFILE_ROOT_OWNER)
      foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_C)
        set(ISAAC_PNG_PROFILE_ROOT_MATCH)
        if(EXISTS "${ISAAC_GENERATED_SOURCE}")
          file(STRINGS "${ISAAC_GENERATED_SOURCE}"
            ISAAC_PNG_PROFILE_ROOT_MATCH
            LIMIT_COUNT 1
            REGEX "^void ${ISAAC_PNG_PROFILE_ROOT}\\(CPU \\*__restrict c\\)$")
          if(ISAAC_PNG_PROFILE_ROOT_MATCH)
            list(APPEND ISAAC_PNG_PROFILE_ROOT_OWNER
              "${ISAAC_GENERATED_SOURCE}")
          endif()
        endif()
      endforeach()
      list(LENGTH ISAAC_PNG_PROFILE_ROOT_OWNER
        ISAAC_PNG_PROFILE_ROOT_OWNER_COUNT)
      if(NOT ISAAC_PNG_PROFILE_ROOT_OWNER_COUNT EQUAL 1)
        message(FATAL_ERROR
          "PNG profile root ${ISAAC_PNG_PROFILE_ROOT} must own exactly one "
          "generated source; found ${ISAAC_PNG_PROFILE_ROOT_OWNER_COUNT}")
      endif()
      math(EXPR ISAAC_VITA_PNG_DECODE_PROFILE_DEFINITIONS
        "${ISAAC_VITA_PNG_DECODE_PROFILE_DEFINITIONS} + 1")
      list(APPEND ISAAC_VITA_PNG_DECODE_PROFILE_OWNERS
        ${ISAAC_PNG_PROFILE_ROOT_OWNER})
    endforeach()
    if(NOT ISAAC_VITA_PNG_DECODE_PROFILE_DEFINITIONS EQUAL 6)
      message(FATAL_ERROR
        "PNG decode profile must resolve exactly six hook definitions; found "
        "${ISAAC_VITA_PNG_DECODE_PROFILE_DEFINITIONS}")
    endif()
    list(REMOVE_DUPLICATES ISAAC_VITA_PNG_DECODE_PROFILE_OWNERS)
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/kage_vita_png_decode_profile.c")
    set(ISAAC_VITA_PNG_DECODE_PROFILE_NATIVE_SOURCES)
    if(ISAAC_VITA_PNG_NATIVE_UNFILTER)
      list(APPEND ISAAC_VITA_PNG_DECODE_PROFILE_NATIVE_SOURCES
        "${ISAAC_RUNTIME}/host_vita_png_unfilter_guest.c")
    endif()
    set_property(SOURCE
      ${ISAAC_VITA_PNG_DECODE_PROFILE_OWNERS}
      "${ISAAC_RUNTIME}/kage_vita_png_decode_profile.c"
      ${ISAAC_VITA_PNG_DECODE_PROFILE_NATIVE_SOURCES}
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_PNG_DECODE_PROFILE=1)
    set_property(SOURCE
      "${ISAAC_RUNTIME}/kage_vita_png_decode_profile.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_PNG_DECODE_PROFILE_BUILD_ID=\"${ISAAC_VITA_GUEST_LINK_ID}\")
    message(STATUS
      "Isaac Vita PNG decode profile: aligned whole-image 1/32 cohort; "
      "owners ${ISAAC_VITA_PNG_DECODE_PROFILE_OWNERS}")
  endif()
  if(ISAAC_VITA_ANM2_MISSING_LAYER_GUARD)
    # gen_all.py pins all three physical map loads in ANM2::RenderLayer.  Find
    # that exact public definition rather than depending on a batching-derived
    # guest_XXXX.c name, and alter only its compile command.  Direct CMake
    # stays default-OFF while the canonical build wrapper opts in explicitly.
    set(ISAAC_VITA_ANM2_MISSING_LAYER_GUARD_OWNER)
    foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_C)
      if(EXISTS "${ISAAC_GENERATED_SOURCE}")
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_ANM2_MISSING_LAYER_GUARD_OWNER_MATCH
          LIMIT_COUNT 1
          REGEX "^void sub_00005250\\(CPU \\*__restrict c\\)$")
        if(ISAAC_ANM2_MISSING_LAYER_GUARD_OWNER_MATCH)
          list(APPEND ISAAC_VITA_ANM2_MISSING_LAYER_GUARD_OWNER
            "${ISAAC_GENERATED_SOURCE}")
        endif()
      endif()
    endforeach()
    list(LENGTH ISAAC_VITA_ANM2_MISSING_LAYER_GUARD_OWNER
      ISAAC_VITA_ANM2_MISSING_LAYER_GUARD_OWNER_COUNT)
    if(NOT ISAAC_VITA_ANM2_MISSING_LAYER_GUARD_OWNER_COUNT EQUAL 1)
      message(FATAL_ERROR
        "ANM2 missing-layer guard must own exactly one generated source; found "
        "${ISAAC_VITA_ANM2_MISSING_LAYER_GUARD_OWNER_COUNT}")
    endif()
    set_property(SOURCE
      ${ISAAC_VITA_ANM2_MISSING_LAYER_GUARD_OWNER}
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_ANM2_MISSING_LAYER_GUARD=1)
    message(STATUS
      "Isaac Vita ANM2 missing-layer guard: enabled in owner "
      "${ISAAC_VITA_ANM2_MISSING_LAYER_GUARD_OWNER}")
  endif()
  if(ISAAC_VITA_RENDERFRAME_FASTPATH)
    # gen_all.py authenticates the complete J835 RenderFrame body, all nine
    # getter calls and four pure-consumer seams, the getter/SmartPointer/
    # reference bodies, exact RTTI vftable, KAGE observer callback/init/slot,
    # and whole-program caller census.  Locate the canonical owner by public
    # definition so generated batching stays free.
    set(ISAAC_VITA_RENDERFRAME_FASTPATH_OWNER)
    foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_CANONICAL_C)
      if(EXISTS "${ISAAC_GENERATED_SOURCE}")
        file(STRINGS "${ISAAC_GENERATED_SOURCE}"
          ISAAC_RENDERFRAME_FASTPATH_OWNER_MATCH
          LIMIT_COUNT 1
          REGEX "^void sub_00003e50\\(CPU \\*__restrict c\\)$")
        if(ISAAC_RENDERFRAME_FASTPATH_OWNER_MATCH)
          list(APPEND ISAAC_VITA_RENDERFRAME_FASTPATH_OWNER
            "${ISAAC_GENERATED_SOURCE}")
        endif()
      endif()
    endforeach()
    list(LENGTH ISAAC_VITA_RENDERFRAME_FASTPATH_OWNER
      ISAAC_VITA_RENDERFRAME_FASTPATH_OWNER_COUNT)
    if(NOT ISAAC_VITA_RENDERFRAME_FASTPATH_OWNER_COUNT EQUAL 1)
      message(FATAL_ERROR
        "RenderFrame fast path must own exactly one generated source; found "
        "${ISAAC_VITA_RENDERFRAME_FASTPATH_OWNER_COUNT}")
    endif()
    if(ISAAC_VITA_DIRECT_DEFAULT AND
       "${ISAAC_VITA_RENDERFRAME_FASTPATH_OWNER}" STREQUAL
         "${ISAAC_VITA_DIRECT_DEFAULT_CANONICAL_SOURCE}")
      set(ISAAC_VITA_RENDERFRAME_FASTPATH_OWNER
          "${ISAAC_VITA_DIRECT_DEFAULT_OUTPUT}")
    endif()
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_renderframe_fastpath.c")
    set_property(SOURCE
      ${ISAAC_VITA_RENDERFRAME_FASTPATH_OWNER}
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_RENDERFRAME_FASTPATH=1)
    message(STATUS
      "Isaac Vita RenderFrame fast path: four fail-closed image borrows in owner "
      "${ISAAC_VITA_RENDERFRAME_FASTPATH_OWNER}")
  endif()
  if(ISAAC_VITA_ANM2_SCRATCH)
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_anm2_scratch.c")
    # Only the imported malloc/free owner observes this workaround.  OFF
    # leaves that object and all generated translation units edge-neutral.
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_heap.c"
      APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_ANM2_SCRATCH=1)
  endif()
  if(ISAAC_VITA_TEXEL_SCRATCH)
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_texel_scratch.c")
    # Route only the imported heap boundary through the owner-scoped pool.
    # Keep the option off the generated translation units and every unrelated
    # native object so an A/B toggle has a bounded incremental rebuild.
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_heap.c"
      "${ISAAC_RUNTIME}/host_vita_texel_scratch.c"
      APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_TEXEL_SCRATCH=1)
    if(ISAAC_VITA_TEXEL_SCRATCH_8M)
      # Diagnostic capacity selection is internal to the texel owner.  It
      # must not rebuild the imported heap boundary or generated guest code.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/host_vita_texel_scratch.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_TEXEL_SCRATCH_8M=1)
    endif()
  endif()
  if(ISAAC_VITA_FXLAYERS_NULL_ROLLBACK)
    # Generated code always owns the exact failure-edge query.  The A/B
    # option changes only the native policy and heap-boundary objects, then
    # relinks.  The heap definition enables requested-extent range leases;
    # default-OFF retains the compact exact-base-only ledger.
    set_property(SOURCE
      "${ISAAC_RUNTIME}/kage_vita_fx_rollback.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_FXLAYERS_NULL_ROLLBACK=1)
  endif()
  if(ISAAC_VITA_FXLAYERS_NULL_ROLLBACK OR
     ISAAC_VITA_HEAP_OVERFLOW_MSPACE)
    # The overflow router needs exact requested sizes for cross-domain copy,
    # independently of whether the FXLayers rollback policy is selected.
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_heap.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_HEAP_RANGE_LEASE=1)
  endif()
  if(ISAAC_VITA_STAGE_MEMORY_DIAGNOSTICS)
    # The generated boundary hooks always retain the required bounded floor
    # lifetime state transition.  Expensive mallinfo snapshots and synchronous
    # log writes are diagnostic-only and stay scoped to their native owner.
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_heap.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_STAGE_MEMORY_DIAGNOSTICS=1)
    message(STATUS
      "Isaac Vita stage-memory snapshots/logging: enabled (diagnostic only)")
  else()
    message(STATUS
      "Isaac Vita stage-memory snapshots/logging: disabled (release fast path)")
  endif()
  # Both optional timing diagnostics observe these two immutable save-load
  # owners.  Keep one literal inventory so independent CMake oracles cannot
  # drift while either option remains OFF.
  set(ISAAC_VITA_SAVE_LOAD_PROFILE_ROOTS
    sub_004b4500
    sub_004b47a0)
  if(ISAAC_VITA_CONTINUE_PROFILE)
    # Only eight frozen-PE owners contain the 39 aggregate boundaries.  Find
    # their manifest-verified definitions instead of coupling the option to
    # batching-derived guest_XXXX.c names or rebuilding the whole corpus.
    set(ISAAC_VITA_CONTINUE_PROFILE_ROOTS
      sub_002cba60
      sub_00302950
      sub_003e5400
      sub_004b3fd0
      sub_0052f7c0
      ${ISAAC_VITA_SAVE_LOAD_PROFILE_ROOTS}
      sub_0055e330)
    set(ISAAC_VITA_CONTINUE_PROFILE_OWNERS)
    foreach(ISAAC_CONTINUE_ROOT IN LISTS
            ISAAC_VITA_CONTINUE_PROFILE_ROOTS)
      set(ISAAC_CONTINUE_ROOT_OWNER)
      foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_CANONICAL_C)
        set(ISAAC_CONTINUE_ROOT_MATCH)
        if(EXISTS "${ISAAC_GENERATED_SOURCE}")
          file(STRINGS "${ISAAC_GENERATED_SOURCE}"
            ISAAC_CONTINUE_ROOT_MATCH
            LIMIT_COUNT 1
            REGEX "^void ${ISAAC_CONTINUE_ROOT}\\(CPU \\*__restrict c\\)$")
          if(ISAAC_CONTINUE_ROOT_MATCH)
            list(APPEND ISAAC_CONTINUE_ROOT_OWNER
              "${ISAAC_GENERATED_SOURCE}")
          endif()
        endif()
      endforeach()
      list(LENGTH ISAAC_CONTINUE_ROOT_OWNER
        ISAAC_CONTINUE_ROOT_OWNER_COUNT)
      if(NOT ISAAC_CONTINUE_ROOT_OWNER_COUNT EQUAL 1)
        message(FATAL_ERROR
          "Continue-profile root ${ISAAC_CONTINUE_ROOT} must own exactly "
          "one generated source; found ${ISAAC_CONTINUE_ROOT_OWNER_COUNT}")
      endif()
      list(APPEND ISAAC_VITA_CONTINUE_PROFILE_OWNERS
        ${ISAAC_CONTINUE_ROOT_OWNER})
    endforeach()
    list(REMOVE_DUPLICATES ISAAC_VITA_CONTINUE_PROFILE_OWNERS)
    if(ISAAC_VITA_DIRECT_DEFAULT)
      list(FIND ISAAC_VITA_CONTINUE_PROFILE_OWNERS
        "${ISAAC_VITA_DIRECT_DEFAULT_CANONICAL_SOURCE}"
        ISAAC_VITA_CONTINUE_PROFILE_DIRECT_INDEX)
      if(NOT ISAAC_VITA_CONTINUE_PROFILE_DIRECT_INDEX LESS 0)
        list(REMOVE_ITEM ISAAC_VITA_CONTINUE_PROFILE_OWNERS
          "${ISAAC_VITA_DIRECT_DEFAULT_CANONICAL_SOURCE}")
        list(APPEND ISAAC_VITA_CONTINUE_PROFILE_OWNERS
          "${ISAAC_VITA_DIRECT_DEFAULT_OUTPUT}")
      endif()
    endif()
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/kage_vita_continue_profile.c")
    set_property(SOURCE
      ${ISAAC_VITA_CONTINUE_PROFILE_OWNERS}
      "${ISAAC_RUNTIME}/kage_vita_continue_profile.c"
      platform.c
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_CONTINUE_PROFILE=1)
    if(ISAAC_VITA_CONTINUE_OVERLAY)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/kage_vita_continue_profile.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_CONTINUE_OVERLAY=1)
      message(STATUS
        "Isaac Vita FILE/Continue overlay: enabled; coarse owner-thread swaps")
    else()
      message(STATUS
        "Isaac Vita FILE/Continue overlay: disabled (timing-only profile)")
    endif()
    list(LENGTH ISAAC_VITA_CONTINUE_PROFILE_OWNERS
      ISAAC_VITA_CONTINUE_PROFILE_OWNER_COUNT)
    message(STATUS
      "Isaac Vita FILE/Continue attribution: enabled in "
      "${ISAAC_VITA_CONTINUE_PROFILE_OWNER_COUNT} generated units")
  else()
    message(STATUS "Isaac Vita FILE/Continue attribution: disabled")
  endif()
  if(ISAAC_VITA_EXIT_MENU_PROFILE)
    # These five frozen owners contain all 22 scalar-only boundaries.  Resolve
    # definitions through the canonical manifest instead of baking batching
    # names into this switch; compile the CRT callback owner only while ON.
    set(ISAAC_VITA_EXIT_MENU_PROFILE_ROOTS
      sub_0025b340
      sub_004b0010
      sub_004b43d0
      ${ISAAC_VITA_SAVE_LOAD_PROFILE_ROOTS})
    set(ISAAC_VITA_EXIT_MENU_PROFILE_OWNERS)
    foreach(ISAAC_EXIT_ROOT IN LISTS ISAAC_VITA_EXIT_MENU_PROFILE_ROOTS)
      set(ISAAC_EXIT_ROOT_OWNER)
      foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_CANONICAL_C)
        set(ISAAC_EXIT_ROOT_MATCH)
        if(EXISTS "${ISAAC_GENERATED_SOURCE}")
          file(STRINGS "${ISAAC_GENERATED_SOURCE}"
            ISAAC_EXIT_ROOT_MATCH
            LIMIT_COUNT 1
            REGEX "^void ${ISAAC_EXIT_ROOT}\\(CPU \\*__restrict c\\)$")
          if(ISAAC_EXIT_ROOT_MATCH)
            list(APPEND ISAAC_EXIT_ROOT_OWNER "${ISAAC_GENERATED_SOURCE}")
          endif()
        endif()
      endforeach()
      list(LENGTH ISAAC_EXIT_ROOT_OWNER ISAAC_EXIT_ROOT_OWNER_COUNT)
      if(NOT ISAAC_EXIT_ROOT_OWNER_COUNT EQUAL 1)
        message(FATAL_ERROR
          "Exit-menu root ${ISAAC_EXIT_ROOT} must own exactly one generated "
          "source; found ${ISAAC_EXIT_ROOT_OWNER_COUNT}")
      endif()
      list(APPEND ISAAC_VITA_EXIT_MENU_PROFILE_OWNERS
        ${ISAAC_EXIT_ROOT_OWNER})
    endforeach()
    list(REMOVE_DUPLICATES ISAAC_VITA_EXIT_MENU_PROFILE_OWNERS)
    if(ISAAC_VITA_DIRECT_DEFAULT)
      list(FIND ISAAC_VITA_EXIT_MENU_PROFILE_OWNERS
        "${ISAAC_VITA_DIRECT_DEFAULT_CANONICAL_SOURCE}"
        ISAAC_VITA_EXIT_MENU_PROFILE_DIRECT_INDEX)
      if(NOT ISAAC_VITA_EXIT_MENU_PROFILE_DIRECT_INDEX LESS 0)
        list(REMOVE_ITEM ISAAC_VITA_EXIT_MENU_PROFILE_OWNERS
          "${ISAAC_VITA_DIRECT_DEFAULT_CANONICAL_SOURCE}")
        list(APPEND ISAAC_VITA_EXIT_MENU_PROFILE_OWNERS
          "${ISAAC_VITA_DIRECT_DEFAULT_OUTPUT}")
      endif()
    endif()
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/kage_vita_exit_menu_profile.c")
    set_property(SOURCE
      ${ISAAC_VITA_EXIT_MENU_PROFILE_OWNERS}
      "${ISAAC_RUNTIME}/kage_vita_exit_menu_profile.c"
      "${ISAAC_RUNTIME}/host_vita_crt.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_EXIT_MENU_PROFILE=1)
    list(LENGTH ISAAC_VITA_EXIT_MENU_PROFILE_OWNERS
      ISAAC_VITA_EXIT_MENU_PROFILE_OWNER_COUNT)
    message(STATUS
      "Isaac Vita Exit/menu attribution: enabled in "
      "${ISAAC_VITA_EXIT_MENU_PROFILE_OWNER_COUNT} generated units")
  else()
    message(STATUS "Isaac Vita Exit/menu attribution: disabled")
  endif()
  if(ISAAC_VITA_KAGE)
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/gl_vita_backend.c"
      "${ISAAC_RUNTIME}/host_vita_xinput.c"
      "${ISAAC_RUNTIME}/kage_vita_backend.c"
      "${ISAAC_RUNTIME}/kage_vita_input.c"
      "${ISAAC_RUNTIME}/kage_vita_touch.c"
      "${ISAAC_RUNTIME}/kage_vita_loading.c"
      "${ISAAC_RUNTIME}/kage_vita_generated_hooks.c"
      "${ISAAC_RUNTIME}/host_vita_audio_cooperative.c"
      "${ISAAC_RUNTIME}/manual_kage_vita.c")
    include("${ISAAC_VITA_LOADING_SELECTION_MODULE}")
    isaac_configure_vita_loading_source(
      "${ISAAC_RUNTIME}/kage_vita_loading.c"
      "${ISAAC_VITA_LOADING_FALLBACK_INCLUDE}"
      "${ISAAC_VITA_LOADING_SPECIALIST_INCLUDE}")
    message(STATUS "Isaac Vita loading screen: ${ISAAC_VITA_LOADING_MODE}")
    message(STATUS
      "Isaac Vita loading presentation: ${ISAAC_VITA_LOADING_PRESENTATION_MODE}")
    if(ISAAC_VITA_FXRAY_ALPHA_MASK)
      # The classifier, two bounded alpha shadows, mutation restoration and
      # texture-object registry all have one owner.  Keep the A/B definition
      # off generated guest code and every unrelated runtime object.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/gl_vita_backend.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_FXRAY_ALPHA_MASK=1)
      message(STATUS
        "Isaac Vita FXLayers ray alpha-mask storage: enabled (pixel-equivalent)")
    else()
      message(STATUS
        "Isaac Vita FXLayers ray alpha-mask storage: disabled")
    endif()
    # The shared entry boundary must not gain KAGE references in the default
    # non-vitaGL build.  Keep the lifecycle edge source-scoped so generated
    # translation units remain independent of this A/B option.
    set_property(SOURCE
      "${ISAAC_RUNTIME}/entry_vita.c"
      APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_KAGE=1)
    # The shared first-fault dispatcher must retain a link-clean non-KAGE
    # build.  Enable its XInput delegates only with the coherent sceCtrl cache.
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_first_fault.c"
      "${ISAAC_RUNTIME}/host_vita_import_id.c"
      APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_XINPUT=1)
    if(ISAAC_VITA_AUDIO_ACTIVE)
      list(APPEND ISAAC_VITA_RUNTIME_SOURCES
        "${ISAAC_RUNTIME}/host_vita_audio.c"
        "${ISAAC_RUNTIME}/host_vita_openal_pool.c")
      # The rebuilt archive always resolves its six direct C allocator imports
      # through this module.  OFF preserves a whole-session newlib fallback;
      # ON changes only the module's initialization policy.  The host adapter
      # owns the one pre-OpenAL initialization/logging edge.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/host_vita_audio.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_OPENAL_POOL_ROUTER=1)
      if(ISAAC_VITA_OPENAL_POOL)
        set_property(SOURCE
          "${ISAAC_RUNTIME}/host_vita_openal_pool.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_OPENAL_POOL_ACTIVE=1)
      endif()
      # Keep the A/B macro off all 170 generated translation units: toggling
      # audio recompiles only its dispatch/manual/entry owners, then relinks.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/entry_vita.c"
        "${ISAAC_RUNTIME}/host_vita_first_fault.c"
        "${ISAAC_RUNTIME}/host_vita_import_id.c"
        "${ISAAC_RUNTIME}/host_vita_audio_cooperative.c"
        "${ISAAC_RUNTIME}/manual_kage_vita.c"
        APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_AUDIO=1)
      if(ISAAC_VITA_AUDIO_WORKER OR ISAAC_VITA_AUDIO_REFILL_RECEIPT)
        # Bundle29 phase A: with audio ON the service phase p95 is 77-155 ms
        # because every layered music stream decodes a 64 KiB slot through
        # translated stb_vorbis in the same StreamSource::Update.  The pacer
        # lives at the AL_BUFFERS_PROCESSED answer and the receipts at the
        # alBufferData/alSourceQueueBuffers adapters, so only the OpenAL
        # adapter owner and the new module see the macros; generated units
        # and every oracle keep byte-identical commands.
        list(APPEND ISAAC_VITA_RUNTIME_SOURCES
          "${ISAAC_RUNTIME}/host_vita_audio_refill.c")
        set_property(SOURCE
          "${ISAAC_RUNTIME}/host_vita_audio.c"
          "${ISAAC_RUNTIME}/host_vita_audio_refill.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_AUDIO_REFILL_HOOKS=1)
        if(ISAAC_VITA_NATIVE_VORBIS_ASYNC)
          # Replay receipt (alSourcePlay from QueueData with the source
          # AL_STOPPED = the stream ran dry).  Tied to the ASYNC knob so
          # the audio adapter objects stay byte-identical with it off.
          set_property(SOURCE
            "${ISAAC_RUNTIME}/host_vita_audio.c"
            "${ISAAC_RUNTIME}/host_vita_audio_refill.c"
            APPEND PROPERTY COMPILE_DEFINITIONS
              ISAAC_VITA_AUDIO_REFILL_REPLAYS=1)
        endif()
        if(ISAAC_VITA_AUDIO_WORKER)
          set_property(SOURCE
            "${ISAAC_RUNTIME}/host_vita_audio_refill.c"
            APPEND PROPERTY COMPILE_DEFINITIONS
              ISAAC_VITA_AUDIO_WORKER=1
              ISAAC_VITA_AUDIO_REFILL_SPACING_US=${ISAAC_VITA_AUDIO_REFILL_SPACING_US}U)
          message(STATUS
            "Isaac Vita audio worker: stream refill pacing "
            "${ISAAC_VITA_AUDIO_REFILL_SPACING_US} us, OpenAL mixer thread "
            "pinned to CPU 2, owner thread to CPU 0/1")
        else()
          message(STATUS
            "Isaac Vita audio worker: disabled (refill receipts only)")
        endif()
      else()
        message(STATUS "Isaac Vita audio worker: disabled")
      endif()
      if(ISAAC_VITA_NATIVE_VORBIS)
        # The translated stb_vorbis_get_samples_short_interleaved (0x5bdf80,
        # guest_0177.c) is called from guest_0174.c, so GNU ld --wrap catches
        # every cross-unit edge at link time; __real_ keeps the translated
        # body for the fallbacks.  set_file_offset / vorbis_pump_first_frame
        # / vorbis_deinit are wrapped only to observe seek_start and
        # teardown.  The module compiles the vendored v1.04 itself; no
        # generated unit changes and the generator is untouched.
        list(APPEND ISAAC_VITA_RUNTIME_SOURCES
          "${ISAAC_RUNTIME}/host_vita_native_vorbis.c")
        set_property(SOURCE
          "${ISAAC_RUNTIME}/host_vita_native_vorbis.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_NATIVE_VORBIS=1
            ISAAC_VITA_NATIVE_VORBIS_BUILD_ID=\"${ISAAC_VITA_GUEST_LINK_ID}\")
        if(ISAAC_VITA_NATIVE_VORBIS_WORKER)
          set_property(SOURCE
            "${ISAAC_RUNTIME}/host_vita_native_vorbis.c"
            APPEND PROPERTY COMPILE_DEFINITIONS
              ISAAC_VITA_NATIVE_VORBIS_WORKER=1)
        endif()
        if(ISAAC_VITA_NATIVE_VORBIS_RECEIPT)
          set_property(SOURCE
            "${ISAAC_RUNTIME}/host_vita_native_vorbis.c"
            APPEND PROPERTY COMPILE_DEFINITIONS
              ISAAC_VITA_NATIVE_VORBIS_RECEIPT=1)
        endif()
        if(ISAAC_VITA_NATIVE_VORBIS_VERIFY)
          set_property(SOURCE
            "${ISAAC_RUNTIME}/host_vita_native_vorbis.c"
            APPEND PROPERTY COMPILE_DEFINITIONS
              ISAAC_VITA_NATIVE_VORBIS_VERIFY=1)
        endif()
        target_link_options(isaac_first_arm_fault PRIVATE
          -Wl,--wrap=sub_005bdf80
          -Wl,--wrap=sub_005b7740
          -Wl,--wrap=sub_005bb260
          -Wl,--wrap=sub_005bd310)
        if(ISAAC_VITA_NATIVE_VORBIS_ASYNC)
          # stb_vorbis_open_file (0x5bd740, guest_0177.c) is called from
          # StreamSourceOgg::Open (guest_0174.c) and the dispatch table, so
          # the same --wrap mechanism observes the open and clones early.
          set_property(SOURCE
            "${ISAAC_RUNTIME}/host_vita_native_vorbis.c"
            APPEND PROPERTY COMPILE_DEFINITIONS
              ISAAC_VITA_NATIVE_VORBIS_ASYNC=1
              ISAAC_NV_SLOT_MIN_FRAMES=${ISAAC_VITA_NATIVE_VORBIS_MIN_FRAMES}U
              ISAAC_NV_SYNC_BUDGET_US=${ISAAC_VITA_NATIVE_VORBIS_SYNC_BUDGET_US}U)
          target_link_options(isaac_first_arm_fault PRIVATE
            -Wl,--wrap=sub_005bd740)
          set(ISAAC_VITA_NATIVE_VORBIS_ASYNC_MODE
            "async: partial slots >= ${ISAAC_VITA_NATIVE_VORBIS_MIN_FRAMES} frames, sync budget ${ISAAC_VITA_NATIVE_VORBIS_SYNC_BUDGET_US} us, early clone at open, replay receipt")
        else()
          set(ISAAC_VITA_NATIVE_VORBIS_ASYNC_MODE "async=OFF")
        endif()
        if(ISAAC_VITA_NATIVE_VORBIS_WORKER)
          set(ISAAC_VITA_NATIVE_VORBIS_MODE "worker on CPU 2 (model b)")
        else()
          set(ISAAC_VITA_NATIVE_VORBIS_MODE "synchronous on the game thread (model a)")
        endif()
        message(STATUS
          "Isaac Vita native Vorbis: enabled, stb_vorbis v1.04 mirrors, "
          "${ISAAC_VITA_NATIVE_VORBIS_MODE}, receipt=${ISAAC_VITA_NATIVE_VORBIS_RECEIPT}, "
          "verify=${ISAAC_VITA_NATIVE_VORBIS_VERIFY}, ${ISAAC_VITA_NATIVE_VORBIS_ASYNC_MODE}")
      else()
        message(STATUS "Isaac Vita native Vorbis: disabled (translated stb_vorbis)")
      endif()
      if(ISAAC_VITA_AUDIO_STREAM_RECEIPT)
        # This diagnostic reads only the authenticated OGG object fields at
        # the same owner-thread boundary which immediately runs Manager::Update.
        # Keep it out of generated units and ordinary performance builds.
        set_property(SOURCE
          "${ISAAC_RUNTIME}/host_vita_audio_cooperative.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_AUDIO_STREAM_RECEIPT=1)
        message(STATUS
          "Isaac Vita cooperative OGG stream receipt: enabled")
      else()
        message(STATUS
          "Isaac Vita cooperative OGG stream receipt: disabled")
      endif()
    endif()
    if(ISAAC_VITA_IO_PROFILE)
      list(APPEND ISAAC_VITA_RUNTIME_SOURCES
        "${ISAAC_RUNTIME}/kage_vita_io_profile.c")
      # Keep all diagnostic bookkeeping absent from ordinary builds.  The
      # physical wrapper, logical CRT counters and texture timing form one
      # coherent profile and are enabled together.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/host_vita_crt.c"
        "${ISAAC_RUNTIME}/host_vita_first_fault.c"
        "${ISAAC_RUNTIME}/gl_vita_backend.c"
        "${ISAAC_RUNTIME}/kage_vita_backend.c"
        "${ISAAC_RUNTIME}/kage_vita_io_profile.c"
        APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_IO_PROFILE=1)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/kage_vita_backend.c"
        "${ISAAC_RUNTIME}/kage_vita_io_profile.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_IO_PROFILE_BUILD_ID=\"${ISAAC_VITA_GUEST_LINK_ID}\")
      if(ISAAC_VITA_HEAP_OVERFLOW_MSPACE)
        set_property(SOURCE
          "${ISAAC_RUNTIME}/kage_vita_io_profile.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_IO_PROFILE_HEAP=1)
      endif()
    endif()
    if(ISAAC_VITA_STALL_PROBE)
      list(APPEND ISAAC_VITA_RUNTIME_SOURCES
        "${ISAAC_RUNTIME}/kage_vita_stall_probe.c"
        "${ISAAC_RUNTIME}/kage_vita_preloop_phase.c")
      # The release/default objects contain no watchdog call or even the hot
      # guest_call argument evaluation.  ON rebuilds only these five native
      # owners plus the probe, never the generated corpus.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/guest.c"
        "${ISAAC_RUNTIME}/host_vita_first_fault.c"
        "${ISAAC_RUNTIME}/kage_vita_backend.c"
        "${ISAAC_RUNTIME}/kage_vita_generated_hooks.c"
        "${ISAAC_RUNTIME}/kage_vita_loading.c"
        APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_STALL_PROBE=1)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/kage_vita_stall_probe.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_STALL_BUILD_ID=\"${ISAAC_VITA_GUEST_LINK_ID}\")
      target_link_options(isaac_first_arm_fault PRIVATE
        # sub_0059fa30 and the GLFW error body it calls (sub_0059b7a0) are
        # emitted in one generated TU.  Wrap the sole cross-TU parent edge so
        # GNU ld cannot leave the assembler-resolved inner call unobserved.
        -Wl,--wrap=sub_0059fa30
        -Wl,--wrap=sub_00598c80
        -Wl,--wrap=sub_0050b240
        -Wl,--wrap=sub_00563880
        -Wl,--wrap=sub_004ab7a0
        -Wl,--wrap=sub_005aeb00
        -Wl,--wrap=sub_005c2fd0
        -Wl,--wrap=sub_005ce7d0)
    endif()
    if(ISAAC_VITA_NATIVE_PNG)
      # The translated png_read_row (0x5b1500, guest_0175.c) is called only
      # from the ImagePng loader loop in guest_0173.c (0x5a139d) and through
      # the guest_table.c dispatch slot, so GNU ld --wrap catches every edge
      # at link time; __real_ keeps the translated body for the fallbacks.
      # The module decodes with the in-tree public-domain tinfl
      # (host_vita_archive_miniz_native.c), which the archive fast path may
      # already compile; no generated unit changes and the generator is
      # untouched.
      list(APPEND ISAAC_VITA_RUNTIME_SOURCES
        "${ISAAC_RUNTIME}/host_vita_native_png.c")
      if(NOT ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH)
        list(APPEND ISAAC_VITA_RUNTIME_SOURCES
          "${ISAAC_RUNTIME}/host_vita_archive_miniz_native.c")
      endif()
      set_property(SOURCE
        "${ISAAC_RUNTIME}/host_vita_native_png.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_NATIVE_PNG=1
          ISAAC_VITA_NATIVE_PNG_RESERVE_MB=${ISAAC_VITA_NATIVE_PNG_RESERVE_MB}
          ISAAC_VITA_NATIVE_PNG_BUILD_ID=\"${ISAAC_VITA_GUEST_LINK_ID}\")
      # entry_vita.c reserves the decoder's scratch memblock before the
      # guest's first KAGE call runs vglInit* (same contract as the Lua arena
      # and the OpenAL pool): the vitaGL RAM pool takes every USER_RW byte
      # above ISAAC_VITA_VITAGL_RAM_RESERVE_MB, and the texel/ANM2 scratches
      # already live inside what is left.  Reserving ahead shrinks the pool
      # by the same amount; the post-init reserve itself is untouched.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/entry_vita.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_NATIVE_PNG=1
          ISAAC_VITA_NATIVE_PNG_RESERVE_MB=${ISAAC_VITA_NATIVE_PNG_RESERVE_MB})
      if(ISAAC_VITA_NATIVE_PNG_RECEIPT)
        set_property(SOURCE
          "${ISAAC_RUNTIME}/host_vita_native_png.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_NATIVE_PNG_RECEIPT=1)
      endif()
      if(ISAAC_VITA_NATIVE_PNG_VERIFY)
        set_property(SOURCE
          "${ISAAC_RUNTIME}/host_vita_native_png.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_NATIVE_PNG_VERIFY=1)
      endif()
      target_link_options(isaac_first_arm_fault PRIVATE
        -Wl,--wrap=sub_005b1500)
      message(STATUS
        "Isaac Vita native PNG: enabled, png_read_row wrapped, tinfl v1.15, "
        "scratch reserve ${ISAAC_VITA_NATIVE_PNG_RESERVE_MB} MiB before vitaGL, "
        "receipt=${ISAAC_VITA_NATIVE_PNG_RECEIPT}, "
        "verify=${ISAAC_VITA_NATIVE_PNG_VERIFY}")
    else()
      message(STATUS "Isaac Vita native PNG: disabled (translated libpng)")
    endif()
    if(ISAAC_VITA_NATIVE_INFLATE)
      # inflate_codes (0x5d6d80, guest_0181.c) has a single, cross-TU caller in
      # inflate_blocks (0x5cf07b, guest_0180.c), so GNU ld --wrap catches every
      # edge; __real_ keeps the translated body for the fail-closed fallbacks.
      # The seam (host_vita_native_inflate.c) runs the vendored, byte-identical
      # zlib 1.1.4 inflate_codes/inflate_fast/inflate_flush/adler32 natively
      # under the iz_ prefix IN PLACE on the guest's own inflate state; the
      # codes TU pins the ILP32 struct offsets at compile time; the hooks TU
      # counts the nested leaf calls the seam replays into the census.  No
      # generated unit changes and the generator is untouched.  Confirm the
      # generated owner is present so a corpus change fails loudly.
      set(ISAAC_VITA_NATIVE_INFLATE_OWNER)
      foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_CANONICAL_C)
        if(EXISTS "${ISAAC_GENERATED_SOURCE}")
          file(STRINGS "${ISAAC_GENERATED_SOURCE}"
            ISAAC_NATIVE_INFLATE_OWNER_MATCH
            LIMIT_COUNT 1
            REGEX "^void sub_005d6d80\\(CPU \\*__restrict c\\)$")
          if(ISAAC_NATIVE_INFLATE_OWNER_MATCH)
            list(APPEND ISAAC_VITA_NATIVE_INFLATE_OWNER
              "${ISAAC_GENERATED_SOURCE}")
          endif()
        endif()
      endforeach()
      list(LENGTH ISAAC_VITA_NATIVE_INFLATE_OWNER
        ISAAC_VITA_NATIVE_INFLATE_OWNER_COUNT)
      if(NOT ISAAC_VITA_NATIVE_INFLATE_OWNER_COUNT EQUAL 1)
        message(FATAL_ERROR
          "native inflate owner sub_005d6d80 must resolve once; found "
          "${ISAAC_VITA_NATIVE_INFLATE_OWNER_COUNT}")
      endif()
      set(ISAAC_VITA_ZLIB114_DIR "${ISAAC_RUNTIME}/vendor/zlib114")
      # Only the leaves reachable from inflate_codes are linked: infcodes.c
      # (through the pinning TU), inffast.c, infutil.c (inflate_flush +
      # inflate_mask) and adler32.c.  inflate()/inflate_blocks()/inftrees/
      # zutil stay translated (they would pull zalloc/calloc); the translated
      # body still drives the block and tree setup.
      set(ISAAC_VITA_NATIVE_INFLATE_CODES_TU
        "${ISAAC_RUNTIME}/host_vita_native_inflate_zlib114_codes.c")
      set(ISAAC_VITA_NATIVE_INFLATE_UTIL_TU
        "${ISAAC_VITA_ZLIB114_DIR}/infutil.c")
      set(ISAAC_VITA_NATIVE_INFLATE_PLAIN_TUS
        "${ISAAC_VITA_ZLIB114_DIR}/inffast.c"
        "${ISAAC_VITA_ZLIB114_DIR}/adler32.c"
        "${ISAAC_RUNTIME}/host_vita_native_inflate_hooks.c")
      list(APPEND ISAAC_VITA_RUNTIME_SOURCES
        "${ISAAC_RUNTIME}/host_vita_native_inflate.c"
        "${ISAAC_VITA_NATIVE_INFLATE_CODES_TU}"
        "${ISAAC_VITA_NATIVE_INFLATE_UTIL_TU}"
        ${ISAAC_VITA_NATIVE_INFLATE_PLAIN_TUS})
      # Every TU that sees zlib 1.1.4 headers gets the iz_ namespacing prefix
      # so nothing clashes with vitaGL's system zlib.  infcodes.c additionally
      # routes inflate_fast/inflate_flush through the counting hooks; infutil.c
      # builds NO_MEMCPY with zmemcpy routed through the memcpy-counting hook.
      # The vendored files are third-party and compile with -w.
      set_property(SOURCE
        "${ISAAC_VITA_ZLIB114_DIR}/inffast.c"
        "${ISAAC_VITA_ZLIB114_DIR}/adler32.c"
        APPEND PROPERTY COMPILE_OPTIONS -w)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/host_vita_native_inflate.c"
        ${ISAAC_VITA_NATIVE_INFLATE_PLAIN_TUS}
        APPEND PROPERTY COMPILE_OPTIONS
          "-I${ISAAC_VITA_ZLIB114_DIR}"
          "-include" "${ISAAC_VITA_ZLIB114_DIR}/izlib114_prefix.h")
      set_property(SOURCE "${ISAAC_VITA_NATIVE_INFLATE_CODES_TU}"
        APPEND PROPERTY COMPILE_OPTIONS
          -w
          "-I${ISAAC_VITA_ZLIB114_DIR}"
          "-include" "${ISAAC_VITA_ZLIB114_DIR}/izlib114_prefix_codes.h")
      set_property(SOURCE "${ISAAC_VITA_NATIVE_INFLATE_UTIL_TU}"
        APPEND PROPERTY COMPILE_OPTIONS
          -w
          "-I${ISAAC_VITA_ZLIB114_DIR}"
          "-include" "${ISAAC_VITA_ZLIB114_DIR}/izlib114_prefix_util.h")
      set_property(SOURCE
        "${ISAAC_RUNTIME}/host_vita_native_inflate.c"
        "${ISAAC_RUNTIME}/host_vita_native_inflate_hooks.c"
        "${ISAAC_VITA_NATIVE_INFLATE_CODES_TU}"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_NATIVE_INFLATE=1)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/host_vita_native_inflate.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_NATIVE_INFLATE_WRAP=1
          ISAAC_VITA_NATIVE_INFLATE_BUILD_ID=\"${ISAAC_VITA_GUEST_LINK_ID}\")
      target_link_options(isaac_first_arm_fault PRIVATE
        -Wl,--wrap=sub_005d6d80)
      message(STATUS
        "Isaac Vita native inflate: enabled, inflate_codes 0x5d6d80 wrapped, "
        "vendored zlib 1.1.4 core in place (owner "
        "${ISAAC_VITA_NATIVE_INFLATE_OWNER})")
    else()
      message(STATUS "Isaac Vita native inflate: disabled (translated zlib)")
    endif()
    if(ISAAC_VITA_SIM_CADENCE_RECEIPT)
      # Discover the exact generated Game::Update owner by its public
      # definition.  The generator independently authenticates the frozen
      # J835 body, entry prologue and real Manager caller before emitting the
      # guarded note.  No scheduler source or policy is part of this receipt.
      set(ISAAC_VITA_SIM_CADENCE_OWNER)
      foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_CANONICAL_C)
        if(EXISTS "${ISAAC_GENERATED_SOURCE}")
          file(STRINGS "${ISAAC_GENERATED_SOURCE}"
            ISAAC_SIM_CADENCE_OWNER_MATCH
            LIMIT_COUNT 1
            REGEX "^void sub_002cdcf0\\(CPU \\*__restrict c\\)$")
          if(ISAAC_SIM_CADENCE_OWNER_MATCH)
            list(APPEND ISAAC_VITA_SIM_CADENCE_OWNER
              "${ISAAC_GENERATED_SOURCE}")
          endif()
        endif()
      endforeach()
      list(LENGTH ISAAC_VITA_SIM_CADENCE_OWNER
        ISAAC_VITA_SIM_CADENCE_OWNER_COUNT)
      if(NOT ISAAC_VITA_SIM_CADENCE_OWNER_COUNT EQUAL 1)
        message(FATAL_ERROR
          "simulation cadence Game::Update owner must resolve once; found "
          "${ISAAC_VITA_SIM_CADENCE_OWNER_COUNT}")
      endif()
      list(APPEND ISAAC_VITA_RUNTIME_SOURCES
        "${ISAAC_RUNTIME}/kage_vita_sim_cadence_receipt.c")
      set_property(SOURCE
        ${ISAAC_VITA_SIM_CADENCE_OWNER}
        "${ISAAC_RUNTIME}/kage_vita_backend.c"
        "${ISAAC_RUNTIME}/kage_vita_sim_cadence_receipt.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_SIM_CADENCE_RECEIPT=1)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/kage_vita_sim_cadence_receipt.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_SIM_CADENCE_RECEIPT_BUILD_ID=\"${ISAAC_VITA_GUEST_LINK_ID}\")
      message(STATUS
        "Isaac Vita simulation cadence receipt: enabled in "
        "${ISAAC_VITA_SIM_CADENCE_OWNER}")
    endif()
    if(ISAAC_VITA_WORLD_SEAM_DIAG)
      # gen_all.py authenticates Game::Render, the Room/RoomTransition join,
      # the intervening Lua pcall path, the optional late Surface draw, and
      # HUD::Render before it emits four read-only boundaries.  Select only
      # that generated owner and the typed GL/receipt owners.
      set(ISAAC_VITA_WORLD_SEAM_OWNER)
      foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_CANONICAL_C)
        if(EXISTS "${ISAAC_GENERATED_SOURCE}")
          file(STRINGS "${ISAAC_GENERATED_SOURCE}"
            ISAAC_WORLD_SEAM_OWNER_MATCH
            LIMIT_COUNT 1
            REGEX "^void sub_002ce770\\(CPU \\*__restrict c\\)$")
          if(ISAAC_WORLD_SEAM_OWNER_MATCH)
            list(APPEND ISAAC_VITA_WORLD_SEAM_OWNER
              "${ISAAC_GENERATED_SOURCE}")
          endif()
        endif()
      endforeach()
      list(LENGTH ISAAC_VITA_WORLD_SEAM_OWNER
        ISAAC_VITA_WORLD_SEAM_OWNER_COUNT)
      if(NOT ISAAC_VITA_WORLD_SEAM_OWNER_COUNT EQUAL 1)
        message(FATAL_ERROR
          "world-seam Game::Render owner must resolve once; found "
          "${ISAAC_VITA_WORLD_SEAM_OWNER_COUNT}")
      endif()
      if(ISAAC_VITA_DIRECT_DEFAULT AND
         "${ISAAC_VITA_WORLD_SEAM_OWNER}" STREQUAL
           "${ISAAC_VITA_DIRECT_DEFAULT_CANONICAL_SOURCE}")
        set(ISAAC_VITA_WORLD_SEAM_OWNER
          "${ISAAC_VITA_DIRECT_DEFAULT_OUTPUT}")
      endif()
      list(APPEND ISAAC_VITA_RUNTIME_SOURCES
        "${ISAAC_RUNTIME}/kage_vita_world_seam_diag.c")
      set_property(SOURCE
        ${ISAAC_VITA_WORLD_SEAM_OWNER}
        "${ISAAC_RUNTIME}/gl_vita_backend.c"
        "${ISAAC_RUNTIME}/kage_vita_world_seam_diag.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_WORLD_SEAM_DIAG=1)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/kage_vita_world_seam_diag.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_WORLD_SEAM_DIAG_BUILD_ID=\"${ISAAC_VITA_GUEST_LINK_ID}\")
      message(STATUS
        "Isaac Vita Utero world seam: bounded Room/Lua/late receipt in "
        "${ISAAC_VITA_WORLD_SEAM_OWNER}")
    endif()
    if(ISAAC_VITA_PHASE_PROFILE)
      list(APPEND ISAAC_VITA_RUNTIME_SOURCES
        "${ISAAC_RUNTIME}/kage_vita_phase_profile.c")
      # Keep the switch off generated units and unrelated runtime owners.  The
      # native boundaries and guest lookup either call the accumulator or
      # compile its macros to no-ops; generated seam calls remain policy-free.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/guest.c"
        "${ISAAC_RUNTIME}/gl_vita_backend.c"
        "${ISAAC_RUNTIME}/kage_vita_backend.c"
        "${ISAAC_RUNTIME}/kage_vita_generated_hooks.c"
        "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"
        "${ISAAC_RUNTIME}/host_vita_post_com.c"
        APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_PHASE_PROFILE=1)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_PHASE_PROFILE_BUILD_ID=\"${ISAAC_VITA_GUEST_LINK_ID}\")
      message(STATUS
        "Isaac Vita aggregate phase profile: enabled (120 completed loops)")
      if(ISAAC_VITA_PHASE_PROFILE_OTHER)
        # Only the reporter changes: the seams it already owns start/stop
        # the oth accumulator; no other TU or generated unit sees the macro.
        set_property(SOURCE
          "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_PHASE_PROFILE_OTHER=1)
        message(STATUS
          "Isaac Vita aggregate phase profile: oth= (outside svc/upd/rnd/lim) in ph120.t")
      endif()
      if(ISAAC_VITA_PROFILE_IMPORT_KINDS)
        # Import-kinds census: guest.c keeps the one per-import array (one
        # bounded indexed increment at guest_call's import classification,
        # the sync fast path and the authenticated-import seam -- shared with
        # the dispatch table's ph120.i); host_vita_import_id.c sums it per
        # binding kind and pins the fourteen hot imports; the reporter appends
        # ph120.ik/ph120.ih with the g(c)=g(l)+imports+dynamic identity, and
        # the fullspeed scheduler accumulates the pace(c,us) split ph120.ik
        # prints.  Generated units and every other runtime owner see no macro.
        set_property(SOURCE
          "${ISAAC_RUNTIME}/guest.c"
          "${ISAAC_RUNTIME}/host_vita_import_id.c"
          "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"
          "${ISAAC_RUNTIME}/kage_vita_fullspeed_scheduler.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_PROFILE_IMPORT_KINDS=1)
        message(STATUS
          "Isaac Vita import census: per-kind totals + hot pins, "
          "ph120.ik/ph120.ih (default follows ISAAC_VITA_GUEST_SAMPLER)")
      endif()
      if(ISAAC_VITA_GL_TIME_PROFILE)
        # Two clock reads around every native GL boundary call (gl_vita_backend),
        # the present (kage_vita_backend) and the ph120.gt reporter.  Generated
        # guest units and vitaGL headers see no macro; vitaGL itself gets the
        # 0005 gxm.c scene timer through the stock-reference recipe above.
        set_property(SOURCE
          "${ISAAC_RUNTIME}/gl_vita_backend.c"
          "${ISAAC_RUNTIME}/kage_vita_backend.c"
          "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_GL_TIME_PROFILE=1)
        message(STATUS
          "Isaac Vita GL time profile: native GL buckets + GXM scene waits "
          "(ph120.gt)")
      endif()
      if(ISAAC_VITA_TEXTURE_CHURN_PROFILE)
        # The diagnostic definition reaches only the typed GL edge and its
        # aggregate reporter, never generated guest units or vitaGL sources.
        set_property(SOURCE
          "${ISAAC_RUNTIME}/gl_vita_backend.c"
          "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_TEXTURE_CHURN_PROFILE=1)
        message(STATUS
          "Isaac Vita texture-churn profile: enabled (one aggregate x record per phase window)")
      endif()
      if(ISAAC_VITA_GUEST_SAMPLER)
        # A second-core SceKernel thread samples the guest stack every 1 ms
        # and the phase reporter prints ph120.hot/hotc/hs after ph120.t.
        # The definition reaches the sampler, the reporter, the entry owner
        # that binds the CPU, guest.c (publish/restore of the indirect
        # target), the fullspeed scheduler (the "pace" bucket around its
        # Game::Update sleep) and the Lua bridge (restores the indirect
        # target after a Lua error unwound translated callback frames);
        # generated units and vitaGL see no macro.
        list(APPEND ISAAC_VITA_RUNTIME_SOURCES
          "${ISAAC_RUNTIME}/kage_vita_guest_sampler.c")
        set_property(SOURCE
          "${ISAAC_RUNTIME}/kage_vita_guest_sampler.c"
          "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"
          "${ISAAC_RUNTIME}/entry_vita.c"
          "${ISAAC_RUNTIME}/guest.c"
          "${ISAAC_RUNTIME}/kage_vita_fullspeed_scheduler.c"
          "${ISAAC_RUNTIME}/host_vita_lua.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_GUEST_SAMPLER=1)
        set_property(SOURCE
          "${ISAAC_RUNTIME}/kage_vita_guest_sampler.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_GUEST_SAMPLER_BUILD_ID=\"${ISAAC_VITA_GUEST_LINK_ID}\")
        message(STATUS
          "Isaac Vita guest sampler: 1 ms return-marker sampling on CPU 1, "
          "per-phase ph120.hot/hotc/hs records")
      endif()
    endif()
    if(ISAAC_VITA_STAGE_HEARTBEAT)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/kage_vita_generated_hooks.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_STAGE_HEARTBEAT=1)
      message(STATUS
        "Isaac Vita render-stage heartbeat: enabled (diagnostic logging)")
    endif()
    if(ISAAC_VITA_FULLSPEED_SCHEDULER)
      list(APPEND ISAAC_VITA_RUNTIME_SOURCES
        "${ISAAC_RUNTIME}/kage_vita_fullspeed_scheduler.c")
      # The cadence policy changes exactly three generated units: the frozen
      # Application::main owner, the Game::Update owner, and the co-located
      # Manager::Update/Render/Continue-publication owner. Discover them by
      # public definitions, never batching numbers.
      set(ISAAC_VITA_FULLSPEED_APP_OWNER)
      set(ISAAC_VITA_FULLSPEED_MANAGER_OWNER)
      set(ISAAC_VITA_FULLSPEED_GAME_UPDATE_OWNER)
      set(ISAAC_VITA_FULLSPEED_RENDER_OWNER)
      set(ISAAC_VITA_FULLSPEED_GAME_PUBLISH_OWNER)
      foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_CANONICAL_C)
        if(EXISTS "${ISAAC_GENERATED_SOURCE}")
          file(STRINGS "${ISAAC_GENERATED_SOURCE}"
            ISAAC_FULLSPEED_APP_OWNER_MATCH
            LIMIT_COUNT 1
            REGEX "^void sub_0048bc50\\(CPU \\*__restrict c\\)$")
          if(ISAAC_FULLSPEED_APP_OWNER_MATCH)
            list(APPEND ISAAC_VITA_FULLSPEED_APP_OWNER
              "${ISAAC_GENERATED_SOURCE}")
          endif()
          file(STRINGS "${ISAAC_GENERATED_SOURCE}"
            ISAAC_FULLSPEED_MANAGER_OWNER_MATCH
            LIMIT_COUNT 1
            REGEX "^void sub_004b0010\\(CPU \\*__restrict c\\)$")
          if(ISAAC_FULLSPEED_MANAGER_OWNER_MATCH)
            list(APPEND ISAAC_VITA_FULLSPEED_MANAGER_OWNER
              "${ISAAC_GENERATED_SOURCE}")
          endif()
          file(STRINGS "${ISAAC_GENERATED_SOURCE}"
            ISAAC_FULLSPEED_GAME_UPDATE_OWNER_MATCH
            LIMIT_COUNT 1
            REGEX "^void sub_002cdcf0\\(CPU \\*__restrict c\\)$")
          if(ISAAC_FULLSPEED_GAME_UPDATE_OWNER_MATCH)
            list(APPEND ISAAC_VITA_FULLSPEED_GAME_UPDATE_OWNER
              "${ISAAC_GENERATED_SOURCE}")
          endif()
          file(STRINGS "${ISAAC_GENERATED_SOURCE}"
            ISAAC_FULLSPEED_RENDER_OWNER_MATCH
            LIMIT_COUNT 1
            REGEX "^void sub_004b0600\\(CPU \\*__restrict c\\)$")
          if(ISAAC_FULLSPEED_RENDER_OWNER_MATCH)
            list(APPEND ISAAC_VITA_FULLSPEED_RENDER_OWNER
              "${ISAAC_GENERATED_SOURCE}")
          endif()
          file(STRINGS "${ISAAC_GENERATED_SOURCE}"
            ISAAC_FULLSPEED_GAME_PUBLISH_OWNER_MATCH
            LIMIT_COUNT 1
            REGEX "^void sub_004b3fd0\\(CPU \\*__restrict c\\)$")
          if(ISAAC_FULLSPEED_GAME_PUBLISH_OWNER_MATCH)
            list(APPEND ISAAC_VITA_FULLSPEED_GAME_PUBLISH_OWNER
              "${ISAAC_GENERATED_SOURCE}")
          endif()
        endif()
      endforeach()
      list(LENGTH ISAAC_VITA_FULLSPEED_APP_OWNER
        ISAAC_VITA_FULLSPEED_APP_OWNER_COUNT)
      list(LENGTH ISAAC_VITA_FULLSPEED_MANAGER_OWNER
        ISAAC_VITA_FULLSPEED_MANAGER_OWNER_COUNT)
      list(LENGTH ISAAC_VITA_FULLSPEED_GAME_UPDATE_OWNER
        ISAAC_VITA_FULLSPEED_GAME_UPDATE_OWNER_COUNT)
      list(LENGTH ISAAC_VITA_FULLSPEED_RENDER_OWNER
        ISAAC_VITA_FULLSPEED_RENDER_OWNER_COUNT)
      list(LENGTH ISAAC_VITA_FULLSPEED_GAME_PUBLISH_OWNER
        ISAAC_VITA_FULLSPEED_GAME_PUBLISH_OWNER_COUNT)
      if(NOT ISAAC_VITA_FULLSPEED_APP_OWNER_COUNT EQUAL 1 OR
         NOT ISAAC_VITA_FULLSPEED_MANAGER_OWNER_COUNT EQUAL 1 OR
         NOT ISAAC_VITA_FULLSPEED_GAME_UPDATE_OWNER_COUNT EQUAL 1 OR
         NOT ISAAC_VITA_FULLSPEED_RENDER_OWNER_COUNT EQUAL 1 OR
         NOT ISAAC_VITA_FULLSPEED_GAME_PUBLISH_OWNER_COUNT EQUAL 1)
        message(FATAL_ERROR
          "fullspeed cadence owners must each resolve once; found app="
          "${ISAAC_VITA_FULLSPEED_APP_OWNER_COUNT} manager="
          "${ISAAC_VITA_FULLSPEED_MANAGER_OWNER_COUNT} game="
          "${ISAAC_VITA_FULLSPEED_GAME_UPDATE_OWNER_COUNT} render="
          "${ISAAC_VITA_FULLSPEED_RENDER_OWNER_COUNT} publish="
          "${ISAAC_VITA_FULLSPEED_GAME_PUBLISH_OWNER_COUNT}")
      endif()
      if(NOT "${ISAAC_VITA_FULLSPEED_MANAGER_OWNER}" STREQUAL
             "${ISAAC_VITA_FULLSPEED_RENDER_OWNER}" OR
         NOT "${ISAAC_VITA_FULLSPEED_MANAGER_OWNER}" STREQUAL
             "${ISAAC_VITA_FULLSPEED_GAME_PUBLISH_OWNER}")
        message(FATAL_ERROR
          "fullspeed Manager::Update, Render and Game publication must "
          "retain one generated owner")
      endif()
      # Compile definitions belong to the derived direct-default source, but
      # its owner identity above comes from the canonical generated source.
      if(ISAAC_VITA_DIRECT_DEFAULT AND
         "${ISAAC_VITA_FULLSPEED_MANAGER_OWNER}" STREQUAL
           "${ISAAC_VITA_DIRECT_DEFAULT_CANONICAL_SOURCE}")
        set(ISAAC_VITA_FULLSPEED_MANAGER_OWNER
            "${ISAAC_VITA_DIRECT_DEFAULT_OUTPUT}")
        set(ISAAC_VITA_FULLSPEED_GAME_PUBLISH_OWNER
            "${ISAAC_VITA_DIRECT_DEFAULT_OUTPUT}")
      endif()
      set(ISAAC_VITA_FULLSPEED_GENERATED_OWNERS
        ${ISAAC_VITA_FULLSPEED_APP_OWNER}
        ${ISAAC_VITA_FULLSPEED_MANAGER_OWNER}
        ${ISAAC_VITA_FULLSPEED_GAME_UPDATE_OWNER}
        ${ISAAC_VITA_FULLSPEED_GAME_PUBLISH_OWNER})
      list(REMOVE_DUPLICATES ISAAC_VITA_FULLSPEED_GENERATED_OWNERS)
      list(LENGTH ISAAC_VITA_FULLSPEED_GENERATED_OWNERS
        ISAAC_VITA_FULLSPEED_GENERATED_OWNER_COUNT)
      if(NOT ISAAC_VITA_FULLSPEED_GENERATED_OWNER_COUNT EQUAL 3)
        message(FATAL_ERROR
          "fullspeed cadence must alter exactly three generated sources; found "
          "${ISAAC_VITA_FULLSPEED_GENERATED_OWNER_COUNT}")
      endif()
      set_property(SOURCE
        ${ISAAC_VITA_FULLSPEED_GENERATED_OWNERS}
        "${ISAAC_RUNTIME}/kage_vita_backend.c"
        "${ISAAC_RUNTIME}/kage_vita_generated_hooks.c"
        "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"
        "${ISAAC_RUNTIME}/kage_vita_fullspeed_scheduler.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_FULLSPEED_SCHEDULER=1)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/kage_vita_fullspeed_scheduler.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_FULLSPEED_SCHEDULER_BUILD_ID=\"${ISAAC_VITA_GUEST_LINK_ID}\")
      if(ISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES)
        # Device logs (bundle35 157.659 s, wf-int720-v2 394.796 s): leaving
        # a run returns from Game::Update at 0x2CE099/0x2CE0EA with the
        # frame unchanged, and the fail-open then ran the original limiter
        # for the rest of the session.  Only the scheduler unit changes.
        set_property(SOURCE
          "${ISAAC_RUNTIME}/kage_vita_fullspeed_scheduler.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES=1)
        message(STATUS
          "Isaac Vita cadence: Exit-to-menu zero-frame returns accepted")
      endif()
      message(STATUS
        "Isaac Vita cadence: fixed 60-Hz wrapper, native ~30-Hz Game parity, "
        "full-phase Render only; owners "
        "${ISAAC_VITA_FULLSPEED_GENERATED_OWNERS}")
    endif()
    if(ISAAC_VITA_STABLE_30_PRESENTATION)
      list(APPEND ISAAC_VITA_RUNTIME_SOURCES
        "${ISAAC_RUNTIME}/kage_vita_stable30.c")
      # This policy changes only the frozen Application::main limiter owner
      # and the co-located Manager::Update/Render parity owner.  It never
      # compiles into Game::Update and never writes guest state.
      set(ISAAC_VITA_STABLE30_APP_OWNER)
      set(ISAAC_VITA_STABLE30_MANAGER_OWNER)
      set(ISAAC_VITA_STABLE30_RENDER_OWNER)
      foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_CANONICAL_C)
        if(EXISTS "${ISAAC_GENERATED_SOURCE}")
          file(STRINGS "${ISAAC_GENERATED_SOURCE}"
            ISAAC_STABLE30_APP_OWNER_MATCH
            LIMIT_COUNT 1
            REGEX "^void sub_0048bc50\\(CPU \\*__restrict c\\)$")
          if(ISAAC_STABLE30_APP_OWNER_MATCH)
            list(APPEND ISAAC_VITA_STABLE30_APP_OWNER
              "${ISAAC_GENERATED_SOURCE}")
          endif()
          file(STRINGS "${ISAAC_GENERATED_SOURCE}"
            ISAAC_STABLE30_MANAGER_OWNER_MATCH
            LIMIT_COUNT 1
            REGEX "^void sub_004b0010\\(CPU \\*__restrict c\\)$")
          if(ISAAC_STABLE30_MANAGER_OWNER_MATCH)
            list(APPEND ISAAC_VITA_STABLE30_MANAGER_OWNER
              "${ISAAC_GENERATED_SOURCE}")
          endif()
          file(STRINGS "${ISAAC_GENERATED_SOURCE}"
            ISAAC_STABLE30_RENDER_OWNER_MATCH
            LIMIT_COUNT 1
            REGEX "^void sub_004b0600\\(CPU \\*__restrict c\\)$")
          if(ISAAC_STABLE30_RENDER_OWNER_MATCH)
            list(APPEND ISAAC_VITA_STABLE30_RENDER_OWNER
              "${ISAAC_GENERATED_SOURCE}")
          endif()
        endif()
      endforeach()
      list(LENGTH ISAAC_VITA_STABLE30_APP_OWNER
        ISAAC_VITA_STABLE30_APP_OWNER_COUNT)
      list(LENGTH ISAAC_VITA_STABLE30_MANAGER_OWNER
        ISAAC_VITA_STABLE30_MANAGER_OWNER_COUNT)
      list(LENGTH ISAAC_VITA_STABLE30_RENDER_OWNER
        ISAAC_VITA_STABLE30_RENDER_OWNER_COUNT)
      if(NOT ISAAC_VITA_STABLE30_APP_OWNER_COUNT EQUAL 1 OR
         NOT ISAAC_VITA_STABLE30_MANAGER_OWNER_COUNT EQUAL 1 OR
         NOT ISAAC_VITA_STABLE30_RENDER_OWNER_COUNT EQUAL 1)
        message(FATAL_ERROR
          "stable30 owners must resolve once; found app="
          "${ISAAC_VITA_STABLE30_APP_OWNER_COUNT} manager="
          "${ISAAC_VITA_STABLE30_MANAGER_OWNER_COUNT} render="
          "${ISAAC_VITA_STABLE30_RENDER_OWNER_COUNT}")
      endif()
      if(NOT "${ISAAC_VITA_STABLE30_MANAGER_OWNER}" STREQUAL
             "${ISAAC_VITA_STABLE30_RENDER_OWNER}")
        message(FATAL_ERROR
          "stable30 Manager::Update and Render must retain one owner")
      endif()
      if(ISAAC_VITA_DIRECT_DEFAULT AND
         "${ISAAC_VITA_STABLE30_MANAGER_OWNER}" STREQUAL
           "${ISAAC_VITA_DIRECT_DEFAULT_CANONICAL_SOURCE}")
        set(ISAAC_VITA_STABLE30_MANAGER_OWNER
            "${ISAAC_VITA_DIRECT_DEFAULT_OUTPUT}")
      endif()
      set(ISAAC_VITA_STABLE30_GENERATED_OWNERS
        ${ISAAC_VITA_STABLE30_APP_OWNER}
        ${ISAAC_VITA_STABLE30_MANAGER_OWNER})
      list(REMOVE_DUPLICATES ISAAC_VITA_STABLE30_GENERATED_OWNERS)
      list(LENGTH ISAAC_VITA_STABLE30_GENERATED_OWNERS
        ISAAC_VITA_STABLE30_GENERATED_OWNER_COUNT)
      if(NOT ISAAC_VITA_STABLE30_GENERATED_OWNER_COUNT EQUAL 2)
        message(FATAL_ERROR
          "stable30 must alter exactly two generated sources; found "
          "${ISAAC_VITA_STABLE30_GENERATED_OWNER_COUNT}")
      endif()
      set_property(SOURCE
        ${ISAAC_VITA_STABLE30_GENERATED_OWNERS}
        "${ISAAC_RUNTIME}/kage_vita_backend.c"
        "${ISAAC_RUNTIME}/kage_vita_generated_hooks.c"
        "${ISAAC_RUNTIME}/kage_vita_stable30.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_STABLE_30_PRESENTATION=1)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/kage_vita_stable30.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_STABLE30_BUILD_ID=\"${ISAAC_VITA_GUEST_LINK_ID}\")
      message(STATUS
        "Isaac Vita presentation: stable 30 Hz, one completed-phase render; "
        "owners ${ISAAC_VITA_STABLE30_GENERATED_OWNERS}")
    endif()
    if(ISAAC_VITA_GUEST_LOOKUP_CACHE)
      # Keep this experiment off every generated translation unit; only the
      # dispatch owner plus the already optional profiler record owner see it.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/guest.c"
        "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_GUEST_LOOKUP_CACHE=1)
      message(STATUS
        "Isaac Vita guest lookup cache: enabled (256 exact keys; "
        "confirmed dispatch bypass; dense IAT)")
    endif()
    if(ISAAC_VITA_DISPATCH_TABLE)
      # Owner scope (the lookup cache's two owners plus the import-ID map):
      # guest.c owns the table, the inline probe and the per-import census
      # array; the profiler owns the derived g(c,l) census and the ph120.d /
      # ph120.i records; host_vita_import_id.c owns the read-only binding-kind
      # query those records print.  Generated units are untouched, and with
      # the option OFF all three objects are byte-identical to the base.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/guest.c"
        "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"
        "${ISAAC_RUNTIME}/host_vita_import_id.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_GUEST_DISPATCH_TABLE=1)
      message(STATUS
        "Isaac Vita dispatch table: enabled (32768 raw-VA slots, two-level "
        "multiplicative perfect hash, inline guest_call probe, exhaustive "
        "boot gate)")
    endif()
    if(ISAAC_VITA_GL_REDUNDANCY_CACHE)
      # This policy lives entirely at the typed guest-to-vitaGL edge.  The
      # optional profiler owner receives only its aggregate suppression line;
      # generated code and pinned vitaGL remain byte-identical.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/gl_vita_backend.c"
        "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_GL_REDUNDANCY_CACHE=1)
      message(STATUS
        "Isaac Vita GL redundancy cache: enabled (linked/current programs; "
          "exact glUniform1i payloads; pointer uniforms native)")
    endif()
    if(ISAAC_VITA_GL_TYPED_STATE_CACHE)
      # The shadow owns only exact-valid typed calls at the guest-to-vitaGL
      # edge.  Invalid/unknown requests still reach the pinned native API;
      # generated code and vitaGL remain byte-identical.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/gl_vita_backend.c"
        "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_GL_TYPED_STATE_CACHE=1)
      message(STATUS
        "Isaac Vita typed GL state cache: enabled (exact texture/blend/depth/viewport/attrib tuples)")
    endif()
    if(ISAAC_VITA_GL_SHIM_FASTDISPATCH)
      # Four owners only: the dispatcher's delimited 0x7e-first block, the
      # bridge (token index + plain owner word), the Vita token resolver and
      # the typed backend (location cache).  Generated units and vitaGL are
      # byte-identical.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/guest.c"
        "${ISAAC_RUNTIME}/gl_bridge.c"
        "${ISAAC_RUNTIME}/host_vita_gl.c"
        "${ISAAC_RUNTIME}/gl_vita_backend.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_GL_SHIM_FASTDISPATCH=1)
      if(ISAAC_VITA_GL_LOCATION_CACHE)
        set_property(SOURCE
          "${ISAAC_RUNTIME}/gl_vita_backend.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_GL_LOCATION_CACHE=1)
        if(ISAAC_VITA_GL_LOCATION_CACHE_VERIFY)
          set_property(SOURCE
            "${ISAAC_RUNTIME}/gl_vita_backend.c"
            APPEND PROPERTY COMPILE_DEFINITIONS
              ISAAC_VITA_GL_LOCATION_CACHE_VERIFY=1)
          message(STATUS
            "Isaac Vita GL location cache: VERIFY variant (native shadow on every hit; ph120.a loc(...,m) must stay 0)")
        endif()
        message(STATUS
          "Isaac Vita GL shim fast dispatch: enabled (0x7e-first probe; direct token index; plain owner word; location cache)")
      else()
        message(STATUS
          "Isaac Vita GL shim fast dispatch: enabled (0x7e-first probe; direct token index; plain owner word; location cache OFF - default until a LOCATION_CACHE_VERIFY device run logs ph120.a m=0)")
      endif()
      if(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
        # Three owners (wf/opt-gltok): the table builder/verifier (guest.c),
        # the trampolines and their census (gl_bridge.c, which therefore also
        # sees the table define and, in profile builds, the profile define
        # guest.c compiles with) and the ph120.d / derived-g(l) reader.
        # Generated units, host_vita_gl.c and vitaGL are byte-identical.
        set_property(SOURCE
          "${ISAAC_RUNTIME}/guest.c"
          "${ISAAC_RUNTIME}/gl_bridge.c"
          "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_GL_SHIM_TABLE_TOKENS=1)
        set_property(SOURCE
          "${ISAAC_RUNTIME}/gl_bridge.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_GUEST_DISPATCH_TABLE=1)
        if(ISAAC_VITA_PHASE_PROFILE)
          set_property(SOURCE
            "${ISAAC_RUNTIME}/gl_bridge.c"
            APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_PHASE_PROFILE=1)
        endif()
        message(STATUS
          "Isaac Vita GL shim table tokens: enabled (73 typed-GL tokens are dispatch-table keys; ph120.d d(c,s,sf,g))")
      endif()
      if(ISAAC_VITA_GL_SHIM_RAW_ARGS)
        # One owner: the bridge's generated adapters (RAW_ARGS arm of
        # gl_surface_generated.inc).
        set_property(SOURCE
          "${ISAAC_RUNTIME}/gl_bridge.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_GL_SHIM_RAW_ARGS=1)
        message(STATUS
          "Isaac Vita GL shim raw args: enabled (one inline frame check per adapter; plain argument loads; one esp update)")
      endif()
    elseif(ISAAC_VITA_GL_LOCATION_CACHE)
      message(STATUS
        "Isaac Vita GL location cache: inactive without ISAAC_VITA_GL_SHIM_FASTDISPATCH")
    endif()
    if(ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS AND
       ISAAC_VITA_PHASE_PROFILE)
      # Read one cumulative POD snapshot per 120 loops.  No per-draw logging
      # or additional timer enters vitaGL's hot path.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS=1)
    endif()
    if(ISAAC_VITA_VITAGL_SHADER_CACHE)
      # Only the typed GL owner labels shader work and the game-present owner
      # emits aggregate cache counters.  Generated guest units remain
      # byte-identical and no display callback reads these statistics.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/gl_vita_backend.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_VITAGL_SHADER_CACHE=1
          HAVE_ISAAC_SHADER_CACHE=1)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/kage_vita_backend.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_VITAGL_SHADER_CACHE=1)
      message(STATUS
        "Isaac Vita shader cache: authenticated persistent GXP cache enabled")
    endif()
    if(ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/gl_vita_backend.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY=1)
      message(STATUS
        "Isaac Vita canonical quad zero-copy: exact KAGE callsites enabled")
    endif()
    if(ISAAC_VITA_GXM_STATE_SHADOW)
      message(STATUS
        "Isaac Vita GXM state shadow: exact per-context/per-scene setters enabled")
    endif()
    if(ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS)
      list(APPEND ISAAC_VITA_RUNTIME_SOURCES
        "${ISAAC_RUNTIME}/gl_vita_coloroffset_source.c")
      # Source authentication is observational at the typed GL boundary; the
      # original count/length partition and bytes still reach vitaGL.  The
      # aggregate owner reads one cumulative snapshot per 120 loops.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/gl_vita_backend.c"
        "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS=1)
      message(STATUS
        "Isaac Vita ColorOffset GPU bundle: byte-identical stock source and opaque no-blend proof enabled")
    endif()
    if(ISAAC_VITA_DISPLAY_RASTER_SCALED)
      # Only the context/display owner and typed guest-GL boundary see this
      # causal A/B.  Generated guest units retain byte-identical commands.
      # ISAAC_VITA_DISPLAY_RASTER_720 stays the "scaled raster active" gate
      # for the existing #if sites; WIDTH/HEIGHT/NUM/DEN carry the geometry.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/kage_vita_backend.c"
        "${ISAAC_RUNTIME}/gl_vita_backend.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_DISPLAY_RASTER_720=1)
      # The CPU loading presenter writes the scanout buffer directly, so it
      # needs the raster geometry (vitaGL pads the stride to 64 pixels).  It
      # deliberately does not receive the RASTER_720 gate: the raw allocator
      # gate pins that definition to exactly the two graphics owners above.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/kage_vita_backend.c"
        "${ISAAC_RUNTIME}/gl_vita_backend.c"
        "${ISAAC_RUNTIME}/kage_vita_loading.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_DISPLAY_RASTER_WIDTH=${ISAAC_VITA_DISPLAY_RASTER_WIDTH}
          ISAAC_VITA_DISPLAY_RASTER_HEIGHT=${ISAAC_VITA_DISPLAY_RASTER_HEIGHT}
          ISAAC_VITA_DISPLAY_RASTER_NUM=${ISAAC_VITA_DISPLAY_RASTER_NUM}
          ISAAC_VITA_DISPLAY_RASTER_DEN=${ISAAC_VITA_DISPLAY_RASTER_DEN})
      message(STATUS
        "Isaac Vita display/default-FBO raster: "
        "${ISAAC_VITA_DISPLAY_RASTER_WIDTH}x${ISAAC_VITA_DISPLAY_RASTER_HEIGHT} "
        "(scale ${ISAAC_VITA_DISPLAY_RASTER_NUM}/${ISAAC_VITA_DISPLAY_RASTER_DEN}; "
        "guest logical coordinates remain 960x540)")
    else()
      message(STATUS
        "Isaac Vita display/default-FBO raster: 960x544 (native panel)")
    endif()
    if(ISAAC_VITA_FBO_RASTER_SCALE)
      # Only the typed guest-GL boundary allocates and scales offscreen colour
      # targets; the phase profiler prints the ph120.e counters.  Generated
      # guest units retain byte-identical commands.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/gl_vita_backend.c"
        "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_FBO_RASTER_SCALE=1
          ISAAC_VITA_FBO_RASTER_SCALE_NUM=${ISAAC_VITA_FBO_RASTER_SCALE_NUM}
          ISAAC_VITA_FBO_RASTER_SCALE_DEN=${ISAAC_VITA_FBO_RASTER_SCALE_DEN})
      message(STATUS
        "Isaac Vita offscreen colour-target raster: "
        "${ISAAC_VITA_FBO_RASTER_SCALE_NUM}/${ISAAC_VITA_FBO_RASTER_SCALE_DEN} "
        "for NULL-defined targets >= 512x512 (guest logical coordinates remain)")
    endif()
    if(ISAAC_VITA_FBO_CLEAR_ELISION)
      # Exact clear elision on offscreen targets: the typed boundary and the
      # phase profiler as above, plus the two vglSwapBuffers owners, whose
      # present ends the GXM scene an owed depth/stencil clear belonged to.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/gl_vita_backend.c"
        "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"
        "${ISAAC_RUNTIME}/kage_vita_backend.c"
        "${ISAAC_RUNTIME}/kage_vita_continue_profile.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_FBO_CLEAR_ELISION=1)
      message(STATUS
        "Isaac Vita offscreen clear elision: colour no-op clears absorbed, "
        "depth/stencil clears owed to the first draw of the same GXM scene")
    endif()
    if(ISAAC_VITA_VITAGL_STOCK_REFERENCE)
      # Only the vitaGL lifecycle owner selects the stock initialization and
      # display-surface policy. The derived guest unit already contains the
      # direct-to-default gotos and sees no native policy macro.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/kage_vita_backend.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_VITAGL_STOCK_REFERENCE=1)
      if(ISAAC_VITA_VITAGL_CACHED_MEM)
        set_property(SOURCE
          "${ISAAC_RUNTIME}/kage_vita_backend.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_VITAGL_CACHED_MEM=1)
        message(STATUS "Isaac Vita graphics: vitaGL cached memory pools ON")
      endif()
      if(ISAAC_VITA_STOCK_DISPLAY_RT_SCENES)
        # Same single owner.  The overlay profile has requested eight scenes
        # per frame since 3575d1c; the stock profile fell back to vitaGL's
        # default of one, which Bundle33 measured as GL calls blocking on the
        # GPU once presentation exceeded ~30 per second.
        set_property(SOURCE
          "${ISAAC_RUNTIME}/kage_vita_backend.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_STOCK_DISPLAY_RT_SCENES=1)
        message(STATUS
          "Isaac Vita graphics: stock-vitagl-reference source=73dd57a "
          "display rt-scenes=8")
      else()
        message(STATUS
          "Isaac Vita graphics: stock-vitagl-reference source=73dd57a "
          "display rt-scenes=1 (vitaGL default)")
      endif()
    endif()
    if(ISAAC_VITA_FIRST_FRAME_PROBE)
      # The experiment owns only four native boundary objects.  In
      # particular, never rebuild or alter any generated guest translation.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/gl_vita_backend.c"
        "${ISAAC_RUNTIME}/kage_vita_backend.c"
        "${ISAAC_RUNTIME}/kage_vita_generated_hooks.c"
        "${ISAAC_RUNTIME}/manual_kage_vita.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_FIRST_FRAME_PROBE=1)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/gl_vita_backend.c"
        "${ISAAC_RUNTIME}/kage_vita_generated_hooks.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_FIRST_FRAME_BUILD_ID=\"${ISAAC_VITA_GUEST_LINK_ID}\")
      if(ISAAC_VITA_DIRECT_DEFAULT)
        # The derived guest unit contains unconditional control flow and must
        # not see this policy macro.  Only the two native boundaries that
        # report/select the direct-default path own the compile-time switch.
        set_property(SOURCE
          "${ISAAC_RUNTIME}/gl_vita_backend.c"
          "${ISAAC_RUNTIME}/kage_vita_generated_hooks.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_DIRECT_DEFAULT=1)
      endif()
      if(ISAAC_VITA_KNOWN_COLOR_PROBE)
        # The game present owner already brackets vglSwapBuffers with a
        # strong runtime tag.  Only the GL boundary consumes that tag and
        # performs/logs the p2 known-color clear/transfer handshake.
        set_property(SOURCE
          "${ISAAC_RUNTIME}/gl_vita_backend.c"
          APPEND PROPERTY COMPILE_DEFINITIONS
            ISAAC_VITA_KNOWN_COLOR_PROBE=1)
        message(STATUS
          "Isaac Vita p2 known-color probe: enabled (diagnostic only)")
      endif()
    endif()
    if(ISAAC_VITA_RAW_GXM_PROBE)
      list(APPEND ISAAC_VITA_RUNTIME_SOURCES
        "${ISAAC_RUNTIME}/kage_vita_raw_gxm_probe.c")
      # Only the two native lifecycle owners see the selection macro.  The
      # wrapper unit is present only in this branch, and no generated guest
      # translation is recompiled for the diagnostic.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/kage_vita_backend.c"
        "${ISAAC_RUNTIME}/kage_vita_generated_hooks.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_RAW_GXM_PROBE=1)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/kage_vita_raw_gxm_probe.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_RAW_GXM_BUILD_ID=\"${ISAAC_VITA_GUEST_LINK_ID}\")
      target_link_options(isaac_first_arm_fault PRIVATE
        -Wl,--wrap=sceGxmColorSurfaceInit
        -Wl,--wrap=sceGxmBeginScene
        -Wl,--wrap=sceGxmDraw
        -Wl,--wrap=sceGxmEndScene
        -Wl,--wrap=sceGxmDisplayQueueAddEntry)
      message(STATUS
        "Isaac Vita raw GXM first-four-present probe: enabled (diagnostic only)")
    endif()
    if(ISAAC_VITA_SCREENSHOT_PROBE)
      list(APPEND ISAAC_VITA_RUNTIME_SOURCES
        "${ISAAC_RUNTIME}/kage_vita_screenshot_probe.c")
      # Only the synchronous owner seam and the diagnostic implementation see
      # this switch.  No generated unit or unrelated native boundary changes.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/kage_vita_backend.c"
        "${ISAAC_RUNTIME}/kage_vita_screenshot_probe.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_SCREENSHOT_PROBE=1)
      set_property(SOURCE
        "${ISAAC_RUNTIME}/kage_vita_screenshot_probe.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_SCREENSHOT_BUILD_ID=\"${ISAAC_VITA_GUEST_LINK_ID}\")
      message(STATUS
        "Isaac Vita app-side screenshot at present 600: enabled (diagnostic only)")
    endif()
    if(ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC OR
       ISAAC_VITA_TEXTURE_ALIGN8_POLICY)
      list(APPEND ISAAC_VITA_RUNTIME_SOURCES
        "${ISAAC_RUNTIME}/kage_vita_texture_memory.c")
    endif()
    if(ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC)
      # This observes only the failed import and the native probe owner.
      # Generated translation units and the default/OFF heap path are intact.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/host_vita_heap.c"
        "${ISAAC_RUNTIME}/kage_vita_texture_memory.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC=1)
    endif()
    if(ISAAC_VITA_TEXTURE_ALIGN8_POLICY)
      # Set the frozen-PE KAGE latch once after image relocation and before
      # TLS/entry execution.  Keep the policy out of the generated corpus.
      set_property(SOURCE
        "${ISAAC_RUNTIME}/entry_vita.c"
        "${ISAAC_RUNTIME}/kage_vita_texture_memory.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
          ISAAC_VITA_TEXTURE_ALIGN8_POLICY=1)
    endif()
  else()
    list(APPEND ISAAC_VITA_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/kage_pc_disabled.c"
      "${ISAAC_RUNTIME}/manual_kage_disabled.c")
  endif()
  foreach(ISAAC_SOURCE IN LISTS ISAAC_VITA_RUNTIME_SOURCES)
    if(NOT EXISTS "${ISAAC_SOURCE}")
      message(FATAL_ERROR "required Vita runtime source is missing: ${ISAAC_SOURCE}")
    endif()
  endforeach()

  target_sources(isaac_first_arm_fault PRIVATE
    ${ISAAC_VITA_RUNTIME_SOURCES}
    ${ISAAC_GENERATED_C})
  target_include_directories(isaac_first_arm_fault PRIVATE
    "${ISAAC_GENERATED_DIR}")
  target_compile_definitions(isaac_first_arm_fault PRIVATE
    ISAAC_VITA_HAS_RUNTIME=1
    ISAAC_VITA_RAW_ALLOCATOR_GATE=1)
  if(ISAAC_VITA_PROFILE_FUNCTION_ENTRIES AND ISAAC_VITA_PHASE_PROFILE)
    # Every generated body's entry hook gains one increment (guest.h
    # guest_coverage_function), so this is target-wide and recompiles the
    # corpus; opt-in for a counting run only.  ph120.ik prints it as ent=.
    target_compile_definitions(isaac_first_arm_fault PRIVATE
      ISAAC_VITA_PROFILE_FUNCTION_ENTRIES=1)
    message(STATUS
      "Isaac Vita body-entry census: enabled (ph120.ik ent=, every unit)")
  endif()
  if(ISAAC_VITA_LUA_OFF_CLOSE_NOOP)
    # Only the dense import-ID dispatcher sees the macro; generated guest
    # units and every other runtime object keep byte-identical commands.
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_import_id.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_LUA_OFF_CLOSE_NOOP=1)
    message(STATUS
      "Isaac Lua: compiled out; shutdown lua_close returns as a no-op")
  endif()
  if(ISAAC_VITA_LUA)
    target_compile_definitions(isaac_first_arm_fault PRIVATE
      ISAAC_VITA_LUA=1)
    target_include_directories(isaac_first_arm_fault PRIVATE
      "${ISAAC_LUA53_SOURCE_DIR}/src")
    target_link_libraries(isaac_first_arm_fault PRIVATE isaac_lua53)
    message(STATUS "Isaac Lua: pinned vanilla 5.3.3 bridge enabled")
  endif()
  # Raw newlib allocation is an ownership boundary, not a general runtime API.
  # Force the poison into every production/runtime/generated translation unit;
  # these four measured owners are the complete exception set.
  # The post-link gate below independently rejects a leaked exemption and pins
  # every raw relocation by owning function.
  foreach(ISAAC_VITA_RAW_ALLOCATOR_OWNER IN ITEMS
          "${ISAAC_RUNTIME}/guest.c"
          "${ISAAC_RUNTIME}/host_vita_heap.c"
          "${ISAAC_RUNTIME}/host_vita_openal_pool.c"
          "${ISAAC_RUNTIME}/kage_vita_texture_memory.c")
    set_property(SOURCE "${ISAAC_VITA_RAW_ALLOCATOR_OWNER}"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_RAW_ALLOCATOR_EXEMPT=1)
  endforeach()
  target_compile_options(isaac_first_arm_fault PRIVATE
    -include "${ISAAC_VITA_RAW_ALLOCATOR_POISON}")
  if(ISAAC_VITA_GUEST_STACK_GUARD)
    message(STATUS
      "Isaac Vita generated synthetic-stack guard: enabled")
  else()
    # Preserve GUEST_STACK_REQUIRED=1 for guest.c, generic import/manual
    # helpers and CPU ownership metadata.  Only generated translation units
    # redirect the five hot names to the exact raw inline path.  The default
    # ON path adds no definition, so its compile commands stay unchanged.
    set_property(SOURCE
      ${ISAAC_GENERATED_C}
      APPEND PROPERTY COMPILE_DEFINITIONS
        GUEST_GENERATED_STACK_GUARD=0)
    message(STATUS
      "Isaac Vita generated synthetic-stack guard: disabled in generated corpus (raw A/B path)")
  endif()
  # Generated-unit code generation knobs.  All are source-scoped to the corpus
  # so guest.c and the runtime keep the checked contract; the defaults add no
  # compile option, so the default command lines stay byte-identical.
  if(NOT ISAAC_VITA_GUEST_BUILD_OPT STREQUAL "O2")
    # target_compile_options carries -O2; a later -O on the command line wins.
    set_property(SOURCE ${ISAAC_GENERATED_C} APPEND PROPERTY COMPILE_OPTIONS
      "-${ISAAC_VITA_GUEST_BUILD_OPT}")
    message(STATUS
      "Isaac Vita generated corpus optimisation: -${ISAAC_VITA_GUEST_BUILD_OPT} (source-scoped)")
  endif()
  if(ISAAC_VITA_GUEST_ISA STREQUAL "arm")
    # ld resolves ARM/Thumb interworking (blx, veneers) for the runtime edge.
    set_property(SOURCE ${ISAAC_GENERATED_C} APPEND PROPERTY COMPILE_OPTIONS
      -marm)
    message(STATUS "Isaac Vita generated corpus ISA: ARM (runtime stays Thumb-2)")
  endif()
  if(ISAAC_VITA_TRANSLATED_CPU)
    if(ISAAC_VITA_TRANSLATED_CPU_COVERAGE_HOOKS_OFF)
      # guest.c keeps the hooks (PC coverage transport); on Vita every pointer
      # stays NULL, so the generated entry hook is a dead load + branch.
      set_property(SOURCE ${ISAAC_GENERATED_C} APPEND PROPERTY
        COMPILE_DEFINITIONS GUEST_COVERAGE_HOOKS=0)
    endif()
    if(ISAAC_VITA_TRANSLATED_CPU_FS_BASE_INLINE)
      set_property(SOURCE ${ISAAC_GENERATED_C} APPEND PROPERTY
        COMPILE_DEFINITIONS GUEST_FS_BASE_INLINE=1)
    endif()
    if(ISAAC_VITA_TRANSLATED_CPU_FLAGS_LOCAL)
      # guest.h: every generated body declares `guest_flags _fl`; SET_FLAGS and
      # the cc_*/fl_* consumers address it through GUEST_FL.  Flags are dead
      # across CALL/RET; the emitter flushes them before an unresolved indirect
      # JMP and loads them when an entry block consumes them.
      set_property(SOURCE ${ISAAC_GENERATED_C} APPEND PROPERTY
        COMPILE_DEFINITIONS GUEST_FLAGS_LOCAL=1)
      message(STATUS "Isaac Vita translated CPU: flag state in per-function locals")
    endif()
    if(ISAAC_VITA_TRANSLATED_CPU_GPR_LOCAL)
      # guest.h: GR(reg)/GPUSH/GPOP/GESP_*/GSTACK_ADDR become eight locals per
      # body; GUEST_GPR_FLUSH/RELOAD publish them around every translated
      # call, guest_call, fault, int3, cpuid, host seam and return.  Without
      # the definition the same tokens spell exactly the old c->reg code.
      set_property(SOURCE ${ISAAC_GENERATED_C} APPEND PROPERTY
        COMPILE_DEFINITIONS GUEST_GPR_LOCAL=1)
      message(STATUS "Isaac Vita translated CPU: general registers in per-function locals")
    endif()
    if(ISAAC_VITA_IAT_DIRECT)
      # guest.h: GUEST_IMPORT_CALL/GUEST_IMPORT_JMP call guest_import_call
      # (guest.c, ISAAC_VITA_IMPORT_DIRECT above) instead of guest_call at the
      # 17.7k call [IAT], 1.2k jmp [IAT] and 3.1k IAT-loaded call-reg sites.
      # Measured (GCC 10.3): +7.7 KB .text ELF-wide, +6144 B bss.  Note that
      # OFF is not object-identical to the pre-token corpus either: the 1237
      # jmp [IAT] sites (12 units) no longer publish flags (guest.h).
      set_property(SOURCE ${ISAAC_GENERATED_C} APPEND PROPERTY
        COMPILE_DEFINITIONS GUEST_IMPORT_DIRECT=1)
      message(STATUS "Isaac Vita translated CPU: IAT sites dispatch directly (GUEST_IMPORT_DIRECT=1)")
    endif()
    if(NOT ISAAC_VITA_TRANSLATED_CPU_LEAF_INLINE)
      # guest.h defaults GUEST_LEAF_INLINE to 1; a corpus generated with
      # leaf inlining then compiles the copied callee statements at every
      # dual-spelled direct call site.  OFF selects the `#else` branch -- the
      # exact pre-inlining C call -- without regenerating.
      set_property(SOURCE ${ISAAC_GENERATED_C} APPEND PROPERTY
        COMPILE_DEFINITIONS GUEST_LEAF_INLINE=0)
      message(STATUS "Isaac Vita translated CPU: inlined leaf sites compile as calls (GUEST_LEAF_INLINE=0)")
    endif()
    if(ISAAC_VITA_TRANSLATED_CPU_SSE_LOWER)
      # guest.h: the GUEST_XMM_* statement tokens (recomp/sse_lower.py) become
      # one NEON operation each (veor/vand/vorr/vbic, vadd/vsub, vshl/vshr,
      # vext/vzip/vtbl, vcvt.f32.s32 q, unaligned vld1/vst1) and a paired
      # comis+jcc becomes one vcmp+vmrs.  Without the definition the same
      # tokens spell exactly the lane loops, union copies and lazy flag
      # producer generated code always had, so the device A/B bisects by
      # relink.  Only a corpus generated with GUEST_SSE_LOWER=1 contains the
      # tokens; on the reference corpus this option changes nothing.
      set_property(SOURCE ${ISAAC_GENERATED_C} APPEND PROPERTY
        COMPILE_DEFINITIONS GUEST_SSE_LOWER=1)
      message(STATUS "Isaac Vita translated CPU: SSE lowering (NEON packed ops, paired comis)")
    endif()
    if(ISAAC_VITA_TRANSLATED_CPU_F32_VFP)
      # guest.h: ldf/stf dereference a word-aligned guest address as a
      # 4-aligned may_alias float (one vldr/vstr, no core<->VFP vmov); a
      # misaligned address takes an out-of-line cold memcpy fallback (VLDR
      # faults on non-word alignment regardless of SCTLR.A).  Doubles keep
      # the pun (GCC 10.3 splits a 4-aligned double into ldr+ldr+vmov
      # anyway).  Pure header knob: the corpus text is untouched and OFF
      # compiles byte-identical objects, so the device A/B bisects by relink.
      # Measured on the 1dae8057 corpus (GCC 10.3): the seven hot render
      # bodies lose 161 of 189 hot-path vmov s,r and 77 of 82 vmov r,s; hot
      # path +15 % instructions (lsls/bne per access), unit .text +7..23 %.
      set_property(SOURCE ${ISAAC_GENERATED_C} APPEND PROPERTY
        COMPILE_DEFINITIONS GUEST_F32_VFP_ACCESS=1)
      message(STATUS "Isaac Vita translated CPU: scalar float accesses as VFP vldr/vstr (GUEST_F32_VFP_ACCESS=1)")
    endif()
    if(ISAAC_VITA_TRANSLATED_CPU_HOT_LAYOUT)
      if(NOT Python3_EXECUTABLE)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
      endif()
      set(ISAAC_VITA_TRANSLATED_CPU_LAYOUT_SCRIPT
          "${CMAKE_CURRENT_SOURCE_DIR}/vita_translated_cpu_layout.py")
      set(ISAAC_VITA_TRANSLATED_CPU_LAYOUT_DIR
          "${CMAKE_BINARY_DIR}/isaac-generated-overrides/translated-cpu")
      set(ISAAC_VITA_TRANSLATED_CPU_LAYOUT_HEADER
          "${ISAAC_VITA_TRANSLATED_CPU_LAYOUT_DIR}/guest_hot_layout.h")
      file(MAKE_DIRECTORY "${ISAAC_VITA_TRANSLATED_CPU_LAYOUT_DIR}")
      set(ISAAC_VITA_TRANSLATED_CPU_LAYOUT_ARGS)
      if(ISAAC_VITA_TRANSLATED_CPU_COLD_REST)
        list(APPEND ISAAC_VITA_TRANSLATED_CPU_LAYOUT_ARGS --cold-rest)
      endif()
      # Configure-time: the header is a pure function of the committed hot set
      # and the corpus declaration list, both already configure dependencies.
      execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${ISAAC_VITA_TRANSLATED_CPU_LAYOUT_SCRIPT}"
          --hot-set "${ISAAC_VITA_TRANSLATED_CPU_HOT_SET}"
          --funcs "${ISAAC_GENERATED_DIR}/guest_funcs.h"
          --output "${ISAAC_VITA_TRANSLATED_CPU_LAYOUT_HEADER}"
          ${ISAAC_VITA_TRANSLATED_CPU_LAYOUT_ARGS}
        RESULT_VARIABLE ISAAC_VITA_TRANSLATED_CPU_LAYOUT_RC
        OUTPUT_VARIABLE ISAAC_VITA_TRANSLATED_CPU_LAYOUT_SUMMARY
        ERROR_VARIABLE ISAAC_VITA_TRANSLATED_CPU_LAYOUT_ERROR
        OUTPUT_STRIP_TRAILING_WHITESPACE)
      if(NOT ISAAC_VITA_TRANSLATED_CPU_LAYOUT_RC EQUAL 0)
        message(FATAL_ERROR
          "translated-cpu hot layout header failed: "
          "${ISAAC_VITA_TRANSLATED_CPU_LAYOUT_ERROR}")
      endif()
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${ISAAC_VITA_TRANSLATED_CPU_HOT_SET}"
        "${ISAAC_VITA_TRANSLATED_CPU_LAYOUT_SCRIPT}")
      # A second forced include after the allocator poison; the allocator gate
      # accepts exactly this header on generated units while the knob is ON.
      set_property(SOURCE ${ISAAC_GENERATED_C} APPEND PROPERTY COMPILE_OPTIONS
        -include "${ISAAC_VITA_TRANSLATED_CPU_LAYOUT_HEADER}")
      message(STATUS
        "Isaac Vita translated-cpu hot layout: ${ISAAC_VITA_TRANSLATED_CPU_LAYOUT_SUMMARY}")
    endif()
    message(STATUS
      "Isaac Vita translated-cpu: coverage_hooks_off=${ISAAC_VITA_TRANSLATED_CPU_COVERAGE_HOOKS_OFF} "
      "fs_base_inline=${ISAAC_VITA_TRANSLATED_CPU_FS_BASE_INLINE} "
      "hot_layout=${ISAAC_VITA_TRANSLATED_CPU_HOT_LAYOUT} "
      "cold_rest=${ISAAC_VITA_TRANSLATED_CPU_COLD_REST}")
  endif()
  # main.c and guest.c print the same deterministic link identity.  Keeping it
  # source-scoped prevents every label change from invalidating all 171
  # numbered translation units; build-opt and generation remain guest.c-only.
  set_property(SOURCE "${ISAAC_RUNTIME}/guest.c" APPEND PROPERTY
    COMPILE_DEFINITIONS
      GUEST_GENERATION_SYMBOL=guest_generation_${ISAAC_RECIPE_ID}
      GUEST_LINK_ID=\"${ISAAC_VITA_GUEST_LINK_ID}\"
      GUEST_BUILD_OPT=\"${ISAAC_VITA_GUEST_BUILD_OPT}\")
  message(STATUS
    "Isaac Vita corpus: ${ISAAC_GENERATED_C_COUNT} C units, base "
    "${ISAAC_GUEST_BASE}")
  message(STATUS
    "Isaac Vita guest build stamp: ${ISAAC_VITA_GUEST_LINK_ID} "
    "opt:${ISAAC_VITA_GUEST_BUILD_OPT} (source-scoped)")
endif()

# These definitions live in platform.c, which is linked directly rather than
# hidden in an archive. --undefined both documents the requirement and prevents
# --gc-sections/weak newlib references from silently discarding them.
target_link_options(isaac_first_arm_fault PRIVATE
  -Wl,--gc-sections
  -Wl,-Map=isaac_first_arm_fault.map
  -Wl,--undefined=_newlib_heap_size_user
  -Wl,--undefined=sceUserMainThreadStackSize)

if(ISAAC_VITA_LAYOUT_HUB)
  # Placement only: the same objects link, their .text input sections are laid
  # out as [cold half A][hub][cold half B] by a script derived at configure time
  # from `arm-vita-eabi-ld --verbose` (vita_text_layout.py refuses any other
  # default-script shape).  -Wl,-q and the --wrap=sub_* edges are untouched.
  if(NOT Python3_EXECUTABLE)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
  endif()
  if(NOT CMAKE_NM OR NOT EXISTS "${CMAKE_NM}")
    find_program(ISAAC_VITA_LAYOUT_NM arm-vita-eabi-nm
      HINTS "${VITASDK}/bin" REQUIRED)
  else()
    set(ISAAC_VITA_LAYOUT_NM "${CMAKE_NM}")
  endif()
  if(NOT CMAKE_LINKER OR NOT EXISTS "${CMAKE_LINKER}")
    find_program(ISAAC_VITA_LAYOUT_LD arm-vita-eabi-ld
      HINTS "${VITASDK}/bin" REQUIRED)
  else()
    set(ISAAC_VITA_LAYOUT_LD "${CMAKE_LINKER}")
  endif()
  if(NOT CMAKE_READELF OR NOT EXISTS "${CMAKE_READELF}")
    find_program(ISAAC_VITA_LAYOUT_READELF arm-vita-eabi-readelf
      HINTS "${VITASDK}/bin" REQUIRED)
  else()
    set(ISAAC_VITA_LAYOUT_READELF "${CMAKE_READELF}")
  endif()
  isaac_vita_layout_hub(isaac_first_arm_fault
    PYTHON "${Python3_EXECUTABLE}"
    SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/vita_text_layout.py"
    ORDER "${ISAAC_VITA_LAYOUT_HUB_ORDER}"
    LINKER "${ISAAC_VITA_LAYOUT_LD}"
    NM "${ISAAC_VITA_LAYOUT_NM}"
    READELF "${ISAAC_VITA_LAYOUT_READELF}"
    OBJECT_SUFFIX ".c${CMAKE_C_OUTPUT_EXTENSION}"
    MAP "${CMAKE_BINARY_DIR}/isaac_first_arm_fault.map"
    ELF "${CMAKE_BINARY_DIR}/isaac_first_arm_fault${CMAKE_EXECUTABLE_SUFFIX}"
    OUTPUT_DIR "${CMAKE_BINARY_DIR}/isaac-generated-overrides/text-layout"
    MAX_VENEERS "${ISAAC_VITA_LAYOUT_HUB_MAX_VENEERS}"
    UNITS ${ISAAC_GENERATED_C})
  message(STATUS
    "Isaac Vita veneer-free .text layout: ${ISAAC_VITA_LAYOUT_HUB_SUMMARY} "
    "max_veneers=${ISAAC_VITA_LAYOUT_HUB_MAX_VENEERS}")
endif()

if(ISAAC_VITA_KAGE)
  add_dependencies(isaac_first_arm_fault
    isaac_vita_vitagl_overlay_build)
  if(ISAAC_VITA_IO_PROFILE)
    target_link_options(isaac_first_arm_fault PRIVATE
      -Wl,--wrap=sceIoRead
      -Wl,--wrap=sceIoLseek32
      -Wl,--wrap=sceIoLseek)
    message(STATUS
      "Isaac Vita startup native-I/O profiling: enabled (diagnostic only)")
  endif()
  target_link_libraries(isaac_first_arm_fault PRIVATE
    isaac_vita_vitagl_overlay
    vitashark
    SceShaccCgExt
    mathneon
    stdc++
    taihen_stub
    SceShaccCg_stub
    SceGxm_stub
    SceDisplay_stub
    SceCtrl_stub
    SceTouch_stub
    SceCommonDialog_stub
    SceKernelDmacMgr_stub)
  if(ISAAC_VITA_SCREENSHOT_PROBE)
    # The screenshot sysmodule is loaded on demand, so its APIs must remain
    # weak imports. Capture is omitted from the public VitaSDK header;
    # kage_vita_screenshot_probe.h owns the pinned app-side declaration.
    target_link_libraries(isaac_first_arm_fault PRIVATE
      SceScreenShot_stub_weak
      SceSysmodule_stub)
  endif()
  if(ISAAC_VITA_STALL_PROBE)
    # sceKernelPowerTick keeps a long unattended diagnostic run out of idle
    # auto-suspend; it is provided by SceProcessmgr, not SceLibKernel.
    target_link_libraries(isaac_first_arm_fault PRIVATE
      SceProcessmgr_stub)
  endif()
  if(ISAAC_VITA_AUDIO_ACTIVE)
    # OpenAL Soft's Vita playback/capture backend is one archive object, so a
    # playback-only game still needs both SceAudio and SceAudioIn at static
    # link time.  pthread owns OpenAL's native mixer thread; it never enters
    # the translated guest dispatcher.
    add_dependencies(isaac_first_arm_fault
      isaac_vita_openal_overlay_build)
    target_link_libraries(isaac_first_arm_fault PRIVATE
      isaac_vita_openal_overlay
      pthread
      SceAudio_stub
      SceAudioIn_stub)
    message(STATUS
      "Isaac Vita OpenAL audio: enabled (80 device voices: manager 64 + stream 16, native mixer; dedicated pool=${ISAAC_VITA_OPENAL_POOL})")
  else()
    message(STATUS
      "Isaac Vita OpenAL audio: disabled (measured unavailable fallback)")
  endif()
endif()

# No graphics, audio, SDL, Lua, shader compiler, or asset dependency belongs in
# the default boot frontier. ISAAC_VITA_KAGE adds the explicit vitaGL/sceCtrl
# boundary; ISAAC_VITA_AUDIO gates OpenAL independently for fast KAGE A/B runs.
# kubridge is needed solely for an exact fixed VA allocation; SceRtc supplies
# UTC and the native Win32 FILETIME conversion.
target_link_libraries(isaac_first_arm_fault PRIVATE
  kubridge_stub
  SceSysmem_stub
  SceKernelThreadMgr_stub
  SceIofilemgr_stub
  SceLibKernel_stub
  SceAppMgr_stub
  SceAppUtil_stub
  ScePower_stub
  SceRtc_stub
  m)
if(NOT ISAAC_VITA_SCAFFOLD_ONLY AND ISAAC_VITA_FIOS_CACHE)
  # VitaSDK has the import archive but intentionally no public FIOS2 header;
  # host_vita_fios_cache.c owns the scene-proven 32-bit ABI declarations.
  target_link_libraries(isaac_first_arm_fault PRIVATE SceFios2_stub)
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
if(NOT ISAAC_VITA_SCAFFOLD_ONLY)
  set(ISAAC_VITA_RAW_ALLOCATOR_OVERRIDE_ARGS)
  set(ISAAC_VITA_RAW_ALLOCATOR_OVERRIDE_DEPENDS)
  set(ISAAC_VITA_RAW_ALLOCATOR_LUA_ARGS)
  set(ISAAC_VITA_RAW_ALLOCATOR_LUA_DEPENDS)
  if(ISAAC_VITA_DIRECT_DEFAULT)
    # The derived unit's identity is pinned per mode inside the codegen and
    # re-checked by the raw allocator gate against the cache selection.
    set(ISAAC_VITA_DIRECT_DEFAULT_MODE "direct-default")
    if(ISAAC_VITA_RENDER_SURFACE_NATIVE)
      set(ISAAC_VITA_DIRECT_DEFAULT_MODE "render-surface-native")
    endif()
    message(STATUS
      "direct-default derivation mode: ${ISAAC_VITA_DIRECT_DEFAULT_MODE}")
    add_custom_command(
      OUTPUT "${ISAAC_VITA_DIRECT_DEFAULT_OUTPUT}"
      COMMAND "${Python3_EXECUTABLE}" "${ISAAC_VITA_DIRECT_DEFAULT_CODEGEN}"
        --input "${ISAAC_VITA_DIRECT_DEFAULT_CANONICAL_SOURCE}"
        --output "${ISAAC_VITA_DIRECT_DEFAULT_OUTPUT}"
        --mode "${ISAAC_VITA_DIRECT_DEFAULT_MODE}"
      DEPENDS
        "${ISAAC_VITA_DIRECT_DEFAULT_CODEGEN}"
        "${ISAAC_VITA_DIRECT_DEFAULT_CANONICAL_SOURCE}"
        "${ISAAC_GENERATED_DIR}/manifest.json"
      COMMENT "Deriving the Vita direct-default Game::Render translation unit"
      VERBATIM)
    list(APPEND ISAAC_VITA_RAW_ALLOCATOR_OVERRIDE_ARGS
      --generated-source-override "${ISAAC_VITA_DIRECT_DEFAULT_OUTPUT}")
    list(APPEND ISAAC_VITA_RAW_ALLOCATOR_OVERRIDE_DEPENDS
      "${ISAAC_VITA_DIRECT_DEFAULT_OUTPUT}")
  endif()
  if(ISAAC_VITA_LUA)
    list(APPEND ISAAC_VITA_RAW_ALLOCATOR_LUA_ARGS
      --lua-source "${ISAAC_LUA53_SOURCE_DIR}"
      --lua-manifest "${CMAKE_CURRENT_SOURCE_DIR}/lua53_vanilla.cmake"
      --lua-archive "$<TARGET_FILE:isaac_lua53>"
      --ar "${CMAKE_AR}")
    list(APPEND ISAAC_VITA_RAW_ALLOCATOR_LUA_DEPENDS
      isaac_lua53
      "${CMAKE_CURRENT_SOURCE_DIR}/lua53_vanilla.cmake")
  endif()
  if(NOT CMAKE_GENERATOR MATCHES "^Ninja")
    message(FATAL_ERROR
      "The Vita raw-allocator object/link closure gate requires Ninja")
  endif()
  if(NOT CMAKE_READELF OR NOT EXISTS "${CMAKE_READELF}")
    find_program(ISAAC_VITA_READELF arm-vita-eabi-readelf
      HINTS "${VITASDK}/bin" REQUIRED)
  else()
    set(ISAAC_VITA_READELF "${CMAKE_READELF}")
  endif()
  set(ISAAC_VITA_RAW_ALLOCATOR_RECEIPT
      "${CMAKE_BINARY_DIR}/vita-raw-allocator-gate.json")
  add_custom_command(
    OUTPUT "${ISAAC_VITA_RAW_ALLOCATOR_RECEIPT}"
    COMMAND "${Python3_EXECUTABLE}" "${ISAAC_VITA_RAW_ALLOCATOR_GATE}"
      --source-root "${ISAAC_ROOT}"
      --generated-manifest "${ISAAC_GENERATED_DIR}/manifest.json"
      --cmake-cache "${CMAKE_BINARY_DIR}/CMakeCache.txt"
      --compile-commands "${CMAKE_BINARY_DIR}/compile_commands.json"
      --build-ninja "${CMAKE_BINARY_DIR}/build.ninja"
      --poison-header "${ISAAC_VITA_RAW_ALLOCATOR_POISON}"
      --target "$<TARGET_FILE:isaac_first_arm_fault>"
      --readelf "${ISAAC_VITA_READELF}"
      --receipt "${ISAAC_VITA_RAW_ALLOCATOR_RECEIPT}"
      ${ISAAC_VITA_RAW_ALLOCATOR_OVERRIDE_ARGS}
      ${ISAAC_VITA_RAW_ALLOCATOR_LUA_ARGS}
    DEPENDS
      isaac_first_arm_fault
      "${ISAAC_VITA_RAW_ALLOCATOR_GATE}"
      "${ISAAC_VITA_RAW_ALLOCATOR_POISON}"
      "${ISAAC_VITA_READELF}"
      "${ISAAC_GENERATED_DIR}/manifest.json"
      "${CMAKE_BINARY_DIR}/compile_commands.json"
      "${CMAKE_BINARY_DIR}/build.ninja"
      ${ISAAC_VITA_RAW_ALLOCATOR_OVERRIDE_DEPENDS}
      ${ISAAC_VITA_RAW_ALLOCATOR_LUA_DEPENDS}
    COMMENT "Verifying Vita raw allocator source/object/link closure"
    VERBATIM USES_TERMINAL)
  add_custom_target(isaac_vita_raw_allocator_gate ALL
    DEPENDS "${ISAAC_VITA_RAW_ALLOCATOR_RECEIPT}")
endif()

if(DEFINED VITA_PACK_VPK_FLAGS AND
   NOT "${VITA_PACK_VPK_FLAGS}" STREQUAL "")
  message(FATAL_ERROR
    "deterministic VPK packaging does not permit VITA_PACK_VPK_FLAGS")
endif()
isaac_enable_deterministic_vpk(
  "${VITA_PACK_VPK}"
  "${Python3_EXECUTABLE}"
  "${ISAAC_VITA_DETERMINISTIC_VPK_WRAPPER}"
  "${SOURCE_DATE_EPOCH}")

vita_create_self(eboot.bin isaac_first_arm_fault UNSAFE NOASLR)
vita_create_self(isaac-manager.bin isaac_manager UNSAFE)

set(ISAAC_VPK_AUDIO_FILES)
if(ISAAC_VITA_AUDIO_ACTIVE)
  list(APPEND ISAAC_VPK_AUDIO_FILES
    FILE "${ISAAC_VITA_OPENAL_CONFIG}"
         "${ISAAC_VITA_OPENAL_CONFIG_DESTINATION}")
endif()

if(ISAAC_VITA_SCAFFOLD_ONLY)
  vita_create_vpk(${ISAAC_VPK_NAME}.vpk ${ISAAC_TITLE_ID} eboot.bin
    VERSION ${ISAAC_APP_VERSION} NAME "${ISAAC_APP_NAME} (scaffold)"
    ${ISAAC_VPK_LIVEAREA_FILES}
    ${ISAAC_VPK_MANAGER_FILE}
    ${ISAAC_VPK_MANAGER_LICENSE_FILE}
    ${ISAAC_VPK_AUDIO_FILES})
else()
  vita_create_vpk(${ISAAC_VPK_NAME}.vpk ${ISAAC_TITLE_ID} eboot.bin
    VERSION ${ISAAC_APP_VERSION} NAME "${ISAAC_APP_NAME}"
    ${ISAAC_VPK_LIVEAREA_FILES}
    ${ISAAC_VPK_MANAGER_FILE}
    ${ISAAC_VPK_MANAGER_LICENSE_FILE}
    FILE "${ISAAC_PE_PATH}" isaac-ng.exe.unpacked.exe
    ${ISAAC_VPK_AUDIO_FILES})
endif()
isaac_append_deterministic_vpk_dependencies(
  "${CMAKE_CURRENT_BINARY_DIR}/${ISAAC_VPK_NAME}.vpk.out"
  "${ISAAC_VITA_DETERMINISTIC_VPK_WRAPPER}"
  "${ISAAC_VITA_REAL_PACK_VPK}")

# vitaGL RAM-pool reserve (see kage_vita_backend.c KAGE_VITA_RAM_THRESHOLD).
set_property(SOURCE "${ISAAC_RUNTIME}/kage_vita_backend.c"
  APPEND PROPERTY COMPILE_DEFINITIONS
    KAGE_VITA_RAM_THRESHOLD=${ISAAC_VITA_VITAGL_RAM_RESERVE_BYTES})
message(STATUS
  "Isaac Vita vitaGL RAM pool reserve: ${ISAAC_VITA_VITAGL_RAM_RESERVE_MB} MiB of USER_RW left outside the pool")
