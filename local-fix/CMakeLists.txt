cmake_minimum_required(VERSION 3.28)
project(obs_pipewire_local_fix C)
find_package(PkgConfig REQUIRED)
find_package(libobs CONFIG REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET libpipewire-0.3 gio-unix-2.0 libdrm)
set(OBS_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(PLUGIN_DIR "${OBS_ROOT}/plugins/linux-pipewire")
set(COMMON_SOURCES
  ${PLUGIN_DIR}/pipewire.c
  ${PLUGIN_DIR}/formats.c
  ${OBS_ROOT}/deps/glad/src/glad.c)
add_library(linux-pipewire MODULE
  ${PLUGIN_DIR}/linux-pipewire.c
  ${PLUGIN_DIR}/camera-portal.c
  ${PLUGIN_DIR}/screencast-portal.c
  ${PLUGIN_DIR}/portal.c
  ${COMMON_SOURCES})
set_target_properties(linux-pipewire PROPERTIES PREFIX "")
target_include_directories(linux-pipewire PRIVATE ${OBS_ROOT}/deps/glad/include)
target_link_libraries(linux-pipewire PRIVATE OBS::libobs PkgConfig::DEPS dl)
target_link_options(linux-pipewire PRIVATE -Wl,-z,defs)
target_compile_options(linux-pipewire PRIVATE -Wall -Wextra)
option(SANITIZE "Sanitize regression executable" ON)
add_executable(test-session test-session.c ${COMMON_SOURCES})
target_include_directories(test-session PRIVATE ${OBS_ROOT}/deps/glad/include)
target_link_libraries(test-session PRIVATE OBS::libobs PkgConfig::DEPS dl)
target_compile_options(test-session PRIVATE -ffunction-sections -fdata-sections -Wall -Wextra)
target_link_options(test-session PRIVATE -Wl,--gc-sections)
if(SANITIZE)
  target_compile_options(test-session PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(test-session PRIVATE -fsanitize=address,undefined)
endif()
enable_testing()
add_test(NAME session-regression COMMAND dbus-run-session -- $<TARGET_FILE:test-session>)
set_tests_properties(session-regression PROPERTIES ENVIRONMENT "G_DEBUG=fatal-criticals;ASAN_OPTIONS=detect_leaks=1")
# Same harness against the original tagged source, for regression evidence.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/baseline")
foreach(source_name screencast-portal.c portal.c)
  execute_process(
    COMMAND git -C "${OBS_ROOT}" show "32.2.2:plugins/linux-pipewire/${source_name}"
    OUTPUT_FILE "${CMAKE_CURRENT_LIST_DIR}/baseline/${source_name}"
    COMMAND_ERROR_IS_FATAL ANY)
endforeach()
add_executable(test-session-original test-session.c ${COMMON_SOURCES})
target_include_directories(test-session-original PRIVATE ${OBS_ROOT}/deps/glad/include ${PLUGIN_DIR})
target_link_libraries(test-session-original PRIVATE OBS::libobs PkgConfig::DEPS dl)
target_compile_definitions(test-session-original PRIVATE
  SCREENCAST_SOURCE="${CMAKE_CURRENT_LIST_DIR}/baseline/screencast-portal.c"
  PORTAL_SOURCE="${CMAKE_CURRENT_LIST_DIR}/baseline/portal.c")
target_compile_options(test-session-original PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test-session-original PRIVATE -Wl,--gc-sections)
if(SANITIZE)
  target_compile_options(test-session-original PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(test-session-original PRIVATE -fsanitize=address,undefined)
endif()
