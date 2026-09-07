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
