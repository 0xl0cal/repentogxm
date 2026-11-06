cmake_minimum_required(VERSION 3.19)
project(isaac_manager_ftp_host_tests LANGUAGES C)

enable_testing()
add_executable(manager_ftp_host_test
  manager_ftp_host_test.c
  ../manager_ftp_lifecycle.c
  ../manager_ftp_path.c
  ../manager_ftp_protocol.c)
target_compile_features(manager_ftp_host_test PRIVATE c_std_11)
target_compile_options(manager_ftp_host_test PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>)
target_include_directories(manager_ftp_host_test PRIVATE ..)
add_test(NAME manager_ftp_host_test COMMAND manager_ftp_host_test)

add_executable(manager_storage_host_test
  manager_storage_host_test.c
  ../manager_storage.c
  ../manager_sha256.c)
target_compile_features(manager_storage_host_test PRIVATE c_std_11)
target_compile_options(manager_storage_host_test PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>)
target_include_directories(manager_storage_host_test PRIVATE ..)
add_test(NAME manager_storage_host_test COMMAND manager_storage_host_test)

# Same sources with ISAAC_VITA_MANAGER_SAVE_GAMESTATE: the legacy cases must
# still pass and the slot-pair cases run.
add_executable(manager_storage_gamestate_host_test
  manager_storage_host_test.c
  ../manager_storage.c
  ../manager_sha256.c)
target_compile_features(manager_storage_gamestate_host_test PRIVATE c_std_11)
target_compile_options(manager_storage_gamestate_host_test PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>)
target_compile_definitions(manager_storage_gamestate_host_test PRIVATE
  ISAAC_VITA_MANAGER_SAVE_GAMESTATE=1)
target_include_directories(manager_storage_gamestate_host_test PRIVATE ..)
add_test(NAME manager_storage_gamestate_host_test
  COMMAND manager_storage_gamestate_host_test)

# Manager settings core (port.ini load/save/undo/defaults, fenced text
# primitives of manager_storage.c, per-boot log head reader) with the shared
# fake filesystem of fake_fs.h; MANAGER_SETTINGS_UI=1 is the knob the manager
# target receives when ISAAC_VITA_MANAGER_SETTINGS_UI is ON.
add_executable(manager_settings_host_test
  manager_settings_host_test.c
  ../manager_settings.c
  ../manager_storage.c
  ../manager_sha256.c
  ../../runtime/host_vita_settings_parse.c)
target_compile_features(manager_settings_host_test PRIVATE c_std_11)
target_compile_options(manager_settings_host_test PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>)
target_compile_definitions(manager_settings_host_test PRIVATE
  MANAGER_SETTINGS_UI=1)
target_include_directories(manager_settings_host_test PRIVATE
  .. ../../runtime)
add_test(NAME manager_settings_host_test COMMAND manager_settings_host_test)

# ISAAC SETTINGS page model (manager_settings_ui.c) over the same core and
# fake filesystem: scripted press sequences against row/legend/status
# goldens, two-press arming, discard, UNDO visibility, FTP refusal and the
# glyph-set / width invariants of every emitted string.
add_executable(manager_settings_ui_host_test
  manager_settings_ui_host_test.c
  ../manager_settings_ui.c
  ../manager_settings.c
  ../manager_storage.c
  ../manager_sha256.c
  ../../runtime/host_vita_settings_parse.c)
target_compile_features(manager_settings_ui_host_test PRIVATE c_std_11)
target_compile_options(manager_settings_ui_host_test PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>)
target_compile_definitions(manager_settings_ui_host_test PRIVATE
  MANAGER_SETTINGS_UI=1)
target_include_directories(manager_settings_ui_host_test PRIVATE
  .. ../../runtime)
add_test(NAME manager_settings_ui_host_test
  COMMAND manager_settings_ui_host_test)

# Manager UI2 (production knob
# ISAAC_VITA_MANAGER_UI2 -> MANAGER_UI2=1, which requires the settings knob,
# so every UI2 target is compiled with both definitions): IMF1 parser, text
# and canvas against the committed font files of assets/manager.
set(MANAGER_UI2_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../assets/manager")
add_executable(manager_font_host_test
  manager_font_host_test.c
  ../manager_font.c
  ../manager_canvas.c)
target_compile_features(manager_font_host_test PRIVATE c_std_11)
target_compile_options(manager_font_host_test PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>)
target_compile_definitions(manager_font_host_test PRIVATE
  MANAGER_SETTINGS_UI=1 MANAGER_UI2=1
  MANAGER_UI_ASSET_DIR="${MANAGER_UI2_ASSET_DIR}")
target_include_directories(manager_font_host_test PRIVATE .. ../../runtime)
add_test(NAME manager_font_host_test COMMAND manager_font_host_test)

# Manager UI2 string table: widths per slot, directive parity, glyph cover.
add_executable(manager_strings_host_test
  manager_strings_host_test.c
  ../manager_strings.c
  ../manager_font.c
  ../manager_canvas.c
  ../manager_theme.c)
target_compile_features(manager_strings_host_test PRIVATE c_std_11)
target_compile_options(manager_strings_host_test PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>)
target_compile_definitions(manager_strings_host_test PRIVATE
  MANAGER_SETTINGS_UI=1 MANAGER_UI2=1
  MANAGER_UI_ASSET_DIR="${MANAGER_UI2_ASSET_DIR}")
target_include_directories(manager_strings_host_test PRIVATE .. ../../runtime)
add_test(NAME manager_strings_host_test COMMAND manager_strings_host_test)

# Manager UI2 manager.ini parse/serialize/load/save over the fake FS.
add_executable(manager_ini_host_test
  manager_ini_host_test.c
  ../manager_ini.c
  ../manager_storage.c
  ../manager_sha256.c)
target_compile_features(manager_ini_host_test PRIVATE c_std_11)
target_compile_options(manager_ini_host_test PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>)
target_compile_definitions(manager_ini_host_test PRIVATE MANAGER_SETTINGS_UI=1 MANAGER_UI2=1)
target_include_directories(manager_ini_host_test PRIVATE .. ../../runtime)
add_test(NAME manager_ini_host_test COMMAND manager_ini_host_test)

# Manager UI2 mod display names from metadata.xml over the fake FS.
add_executable(manager_mods_meta_host_test
  manager_mods_meta_host_test.c
  ../manager_mods_meta.c
  ../manager_font.c
  ../manager_canvas.c
  ../manager_storage.c
  ../manager_sha256.c)
target_compile_features(manager_mods_meta_host_test PRIVATE c_std_11)
target_compile_options(manager_mods_meta_host_test PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>)
target_compile_definitions(manager_mods_meta_host_test PRIVATE MANAGER_SETTINGS_UI=1 MANAGER_UI2=1)
target_include_directories(manager_mods_meta_host_test PRIVATE .. ../../runtime)
add_test(NAME manager_mods_meta_host_test COMMAND manager_mods_meta_host_test)

# Manager UI2 page controller: scripted presses against the display list
# (manager_ui_host_test, with a stub game skin behind MANAGER_UI2_GAME_SKIN)
# and every page painted in dark/light x en/ru against frame-hash goldens
# (manager_ui_render_host_test, no skin compiled: theme=game falls back).
set(MANAGER_UI2_CONTROLLER_SOURCES
  ../manager_ui.c
  ../manager_ui_draw.c
  ../manager_theme.c
  ../manager_strings.c
  ../manager_font.c
  ../manager_canvas.c
  ../manager_ini.c
  ../manager_mods_meta.c
  ../manager_settings_ui.c
  ../manager_settings.c
  ../manager_storage.c
  ../manager_sha256.c
  ../../runtime/host_vita_settings_parse.c)
foreach(ui2_test IN ITEMS manager_ui_host_test manager_ui_render_host_test)
  add_executable(${ui2_test} ${ui2_test}.c ${MANAGER_UI2_CONTROLLER_SOURCES})
  target_compile_features(${ui2_test} PRIVATE c_std_11)
  target_compile_options(${ui2_test} PRIVATE
    $<$<C_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>)
  target_compile_definitions(${ui2_test} PRIVATE
    MANAGER_SETTINGS_UI=1 MANAGER_UI2=1
    MANAGER_UI_ASSET_DIR="${MANAGER_UI2_ASSET_DIR}")
  target_include_directories(${ui2_test} PRIVATE .. ../../runtime)
  add_test(NAME ${ui2_test} COMMAND ${ui2_test})
endforeach()
target_compile_definitions(manager_ui_host_test PRIVATE MANAGER_UI2_GAME_SKIN=1)
target_compile_definitions(manager_ui_render_host_test PRIVATE
  MANAGER_UI_RENDER_GOLDENS="${CMAKE_CURRENT_SOURCE_DIR}/manager_ui_render_goldens.txt")
# Manager UI2 rows behind their own knobs: the trophy import row
# (ISAAC_VITA_MANAGER_TROPHY_IMPORT -> MANAGER_TROPHY_IMPORT=1: request /
# result parsers, the ACHIEVEMENTS row over the fake FS, its frames) and the
# debug overlay row (ISAAC_VITA_MANAGER_DEBUG_OVERLAY_ROW ->
# MANAGER_DEBUG_OVERLAY_ROW=1: absent with a parser that lacks
# debug_overlay, the whole row path with one that has it); the string table
# test again with both rows' strings (widths per slot, glyphs in the
# atlases).
add_executable(manager_ui_trophy_host_test
  manager_ui_trophy_host_test.c ${MANAGER_UI2_CONTROLLER_SOURCES})
add_executable(manager_ui_overlay_host_test
  manager_ui_overlay_host_test.c ${MANAGER_UI2_CONTROLLER_SOURCES})
add_executable(manager_strings_knobs_host_test
  manager_strings_host_test.c
  ../manager_strings.c
  ../manager_font.c
  ../manager_canvas.c
  ../manager_theme.c)
foreach(row_test IN ITEMS manager_ui_trophy_host_test
    manager_ui_overlay_host_test manager_strings_knobs_host_test)
  target_compile_features(${row_test} PRIVATE c_std_11)
  target_compile_options(${row_test} PRIVATE
    $<$<C_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>)
  target_compile_definitions(${row_test} PRIVATE
    MANAGER_SETTINGS_UI=1 MANAGER_UI2=1
    MANAGER_UI_ASSET_DIR="${MANAGER_UI2_ASSET_DIR}")
  target_include_directories(${row_test} PRIVATE .. ../../runtime)
  add_test(NAME ${row_test} COMMAND ${row_test})
endforeach()
target_compile_definitions(manager_ui_trophy_host_test PRIVATE
  MANAGER_TROPHY_IMPORT=1
  MANAGER_UI_TROPHY_GOLDENS="${CMAKE_CURRENT_SOURCE_DIR}/manager_ui_trophy_goldens.txt")
target_compile_definitions(manager_ui_overlay_host_test PRIVATE
  MANAGER_DEBUG_OVERLAY_ROW=1)
target_compile_definitions(manager_strings_knobs_host_test PRIVATE
  MANAGER_TROPHY_IMPORT=1 MANAGER_DEBUG_OVERLAY_ROW=1)
# Manager game skin (knob
# ISAAC_VITA_MANAGER_UI2_GAME_SKIN): ARCH000 reader, PNG subset decoder and
# BMFont reader, compiled with the defines a game-skin manager build carries.
# manager_gameskin_fixtures regenerates the synthetic archives (built with
# tools/isaac_vita_repack.py's encoder), PNG fixtures (stdlib zlib) and
# expected outputs into the build tree on every ctest run; the arch and png
# tests require it.  The BMFont test writes its own font.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(MANAGER_GAMESKIN_FIXTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/gameskin_fixtures")
add_test(NAME manager_gameskin_fixtures
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/manager_gameskin_fixtures.py"
          "${MANAGER_GAMESKIN_FIXTURE_DIR}")
set_tests_properties(manager_gameskin_fixtures PROPERTIES
  FIXTURES_SETUP manager_gameskin)
set(MANAGER_GAMESKIN_SOURCES_manager_arch_host_test
  ../manager_arch.c ../manager_png.c ../manager_bmfont.c ../manager_tinfl.c)
set(MANAGER_GAMESKIN_SOURCES_manager_png_host_test
  ../manager_png.c ../manager_tinfl.c)
set(MANAGER_GAMESKIN_SOURCES_manager_bmfont_host_test
  ../manager_bmfont.c)
foreach(gameskin_test IN ITEMS
    manager_arch_host_test manager_png_host_test manager_bmfont_host_test)
  add_executable(${gameskin_test}
    ${gameskin_test}.c
    ${MANAGER_GAMESKIN_SOURCES_${gameskin_test}})
  target_compile_features(${gameskin_test} PRIVATE c_std_11)
  target_compile_options(${gameskin_test} PRIVATE
    $<$<C_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>)
  target_compile_definitions(${gameskin_test} PRIVATE
    MANAGER_SETTINGS_UI=1 MANAGER_UI2=1 MANAGER_UI2_GAME_SKIN=1)
  target_include_directories(${gameskin_test} PRIVATE
    .. ../../runtime)
endforeach()
add_test(NAME manager_arch_host_test
  COMMAND manager_arch_host_test "${MANAGER_GAMESKIN_FIXTURE_DIR}")
add_test(NAME manager_png_host_test
  COMMAND manager_png_host_test "${MANAGER_GAMESKIN_FIXTURE_DIR}")
add_test(NAME manager_bmfont_host_test COMMAND manager_bmfont_host_test)
set_tests_properties(manager_arch_host_test manager_png_host_test PROPERTIES
  FIXTURES_REQUIRED manager_gameskin)
# The skin itself (manager_skin_game.c) over the synthetic install the
# fixtures step writes into gameskin_fixtures/skin/: every UI2 page painted
# with theme=game in en/ru against frame-hash goldens, the BMFont face
# adapter, and every failure path back to the light theme.  Its own target:
# manager_ui_host_test keeps its stub skin.
add_executable(manager_skin_game_host_test
  manager_skin_game_host_test.c
  ../manager_skin_game.c
  ../manager_arch.c ../manager_png.c ../manager_bmfont.c ../manager_tinfl.c
  ${MANAGER_UI2_CONTROLLER_SOURCES})
target_compile_features(manager_skin_game_host_test PRIVATE c_std_11)
target_compile_options(manager_skin_game_host_test PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>)
target_compile_definitions(manager_skin_game_host_test PRIVATE
  MANAGER_SETTINGS_UI=1 MANAGER_UI2=1 MANAGER_UI2_GAME_SKIN=1
  MANAGER_UI_ASSET_DIR="${MANAGER_UI2_ASSET_DIR}"
  MANAGER_SKIN_GAME_GOLDENS="${CMAKE_CURRENT_SOURCE_DIR}/manager_skin_game_goldens.txt")
target_include_directories(manager_skin_game_host_test PRIVATE
  .. ../../runtime)
add_test(NAME manager_skin_game_host_test
  COMMAND manager_skin_game_host_test "${MANAGER_GAMESKIN_FIXTURE_DIR}")
set_tests_properties(manager_skin_game_host_test PROPERTIES
  FIXTURES_REQUIRED manager_gameskin)

add_executable(host_vita_log_async_oracle
  ../../runtime/host_vita_log_async_oracle.c
  ../../runtime/host_vita_log_async.c)
target_compile_features(host_vita_log_async_oracle PRIVATE c_std_11)
target_compile_options(host_vita_log_async_oracle PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>)
target_compile_definitions(host_vita_log_async_oracle PRIVATE
  ISAAC_VITA_LOG_ASYNC_ORACLE=1)
target_include_directories(host_vita_log_async_oracle PRIVATE
  ../../runtime ..)
add_test(NAME host_vita_log_async_oracle COMMAND host_vita_log_async_oracle)

foreach(profile_test IN ITEMS frame_spike_profile frame_spike_seams room_profile_format)
  add_executable(kage_vita_${profile_test}_test kage_vita_${profile_test}_test.c)
  target_compile_features(kage_vita_${profile_test}_test PRIVATE c_std_11)
  target_compile_options(kage_vita_${profile_test}_test PRIVATE
    $<$<C_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>)
  target_include_directories(kage_vita_${profile_test}_test PRIVATE ../../runtime ..)
  add_test(NAME kage_vita_${profile_test}_test COMMAND kage_vita_${profile_test}_test)
endforeach()
target_compile_definitions(kage_vita_frame_spike_seams_test PRIVATE
  ISAAC_VITA_FRAME_SPIKE_PROFILE=1)

add_executable(vita_fbo_rt_reuse_test
  vita_fbo_rt_reuse_test.c
  ../vitagl-stock-reference/isaac_fbo_rt_reuse.c)
target_compile_features(vita_fbo_rt_reuse_test PRIVATE c_std_11)
target_compile_options(vita_fbo_rt_reuse_test PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>)
target_compile_definitions(vita_fbo_rt_reuse_test PRIVATE HAVE_SINGLE_THREADED_GC=1)
add_test(NAME vita_fbo_rt_reuse_test COMMAND vita_fbo_rt_reuse_test)

# port.ini boot loader (receipt goldens) in the flavours the production
# CMake block can produce: with the nested fullspeed-scheduler definition
# (setter pushed), without it (fps_lock reported as ignored), and with the
# forwarded build raster of a 720x408 eboot (receipt says raster=720x408
# whenever port.ini leaves display_raster unset).
foreach(variant IN ITEMS fullspeed nofullspeed raster720)
  add_executable(host_vita_runtime_settings_oracle_${variant}
    ../../runtime/host_vita_runtime_settings_oracle.c
    ../../runtime/host_vita_runtime_settings.c
    ../../runtime/host_vita_settings_parse.c)
  target_compile_features(host_vita_runtime_settings_oracle_${variant}
    PRIVATE c_std_11)
  target_compile_options(host_vita_runtime_settings_oracle_${variant} PRIVATE
    $<$<C_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>)
  target_compile_definitions(host_vita_runtime_settings_oracle_${variant}
    PRIVATE ISAAC_VITA_RUNTIME_SETTINGS=1 ISAAC_VITA_RUNTIME_SETTINGS_ORACLE=1)
  target_include_directories(host_vita_runtime_settings_oracle_${variant}
    PRIVATE ../../runtime ..)
  add_test(NAME host_vita_runtime_settings_oracle_${variant}
    COMMAND host_vita_runtime_settings_oracle_${variant})
endforeach()
target_compile_definitions(host_vita_runtime_settings_oracle_fullspeed
  PRIVATE ISAAC_VITA_FULLSPEED_SCHEDULER=1)
target_compile_definitions(host_vita_runtime_settings_oracle_raster720
  PRIVATE ISAAC_VITA_FULLSPEED_SCHEDULER=1 ISAAC_VITA_DISPLAY_RASTER_WIDTH=720)

# launch-args.txt guest argv extension (ISAAC_VITA_LAUNCH_ARGS_FILE).
add_executable(host_vita_launch_args_oracle
  ../../runtime/host_vita_launch_args_oracle.c
  ../../runtime/host_vita_launch_args.c)
target_compile_features(host_vita_launch_args_oracle PRIVATE c_std_11)
target_compile_options(host_vita_launch_args_oracle PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>)
target_compile_definitions(host_vita_launch_args_oracle
  PRIVATE ISAAC_VITA_LAUNCH_ARGS_ORACLE=1)
target_include_directories(host_vita_launch_args_oracle
  PRIVATE ../../runtime ..)
add_test(NAME host_vita_launch_args_oracle COMMAND host_vita_launch_args_oracle)

# options.ini MaxRenderScale normalizer (render_scale setting): the pure
# transform, the publish protocol over a fake three-path filesystem with
# failure injection on every seam, recovery and the receipt goldens.
add_executable(host_vita_render_scale_oracle
  ../../runtime/host_vita_render_scale_oracle.c
  ../../runtime/host_vita_render_scale.c)
target_compile_features(host_vita_render_scale_oracle PRIVATE c_std_11)
target_compile_options(host_vita_render_scale_oracle PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>)
target_compile_definitions(host_vita_render_scale_oracle PRIVATE
  ISAAC_VITA_RENDER_SCALE_DEFAULT=1 ISAAC_VITA_RENDER_SCALE_ORACLE=1)
target_include_directories(host_vita_render_scale_oracle PRIVATE
  ../../runtime ..)
add_test(NAME host_vita_render_scale_oracle
  COMMAND host_vita_render_scale_oracle)

add_executable(host_vita_settings_oracle
  ../../runtime/host_vita_settings_oracle.c
  ../../runtime/host_vita_settings_parse.c)
target_compile_features(host_vita_settings_oracle PRIVATE c_std_11)
target_compile_options(host_vita_settings_oracle PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Werror>)
target_include_directories(host_vita_settings_oracle PRIVATE
  ../../runtime ..)
add_test(NAME host_vita_settings_oracle COMMAND host_vita_settings_oracle)
