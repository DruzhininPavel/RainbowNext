cmake_minimum_required(VERSION 3.24)
project(RainbowNext VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(RAINBOW_BUILD_TESTS "Build unit tests" ON)
option(RAINBOW_FETCH_IMGUI "Fetch Dear ImGui" ON)
option(RAINBOW_FETCH_SDL3 "Build SDL3 from source when no system SDL3 is found" ON)
option(RAINBOW_STATIC_SDL3
  "Ignore any system SDL3 and build a static copy from source, so the binary needs no SDL library at runtime" OFF)

include(FetchContent)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

if(NOT RAINBOW_STATIC_SDL3)
  find_package(SDL3 3.2 QUIET)
endif()
if(NOT SDL3_FOUND)
  if(RAINBOW_FETCH_SDL3)
    message(STATUS "System SDL3 not found - building SDL3 from source")
    FetchContent_Declare(
      SDL3
      GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
      GIT_TAG        release-3.4.18
      GIT_SHALLOW    TRUE
      GIT_SUBMODULES_RECURSE TRUE
    )
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(SDL3)
  else()
    message(FATAL_ERROR
      "SDL3 >= 3.2 not found. Install it (brew install sdl3 / apt install libsdl3-dev) "
      "or configure with -DRAINBOW_FETCH_SDL3=ON.")
  endif()
endif()

if(RAINBOW_FETCH_IMGUI)
  FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.93.0
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(imgui)

  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp
  )
  target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
  )
  target_link_libraries(imgui PUBLIC SDL3::SDL3)
  target_compile_features(imgui PUBLIC cxx_std_17)
endif()

# ---------------------------------------------------------------------------
# Core (deterministic, no OS dependencies)
# ---------------------------------------------------------------------------
add_library(rainbow_core STATIC
  src/core/z80.cpp
  src/core/disassembler.cpp
  src/core/machine.cpp
  src/core/rewind.cpp
  src/core/rzx.cpp
  src/core/rzx_session.cpp
  src/core/ula.cpp
  src/core/keyboard.cpp
  src/core/ay.cpp
  src/core/turbosound.cpp
  src/core/beeper.cpp
  src/core/tape.cpp
  src/core/upd765.cpp
  src/core/nextreg.cpp
  src/core/palette.cpp
  src/core/sprites.cpp
  src/core/copper.cpp
  src/core/dac.cpp
  src/core/dma.cpp
  src/core/expansion.cpp
  src/core/sdcard.cpp
  src/core/esxdos.cpp
  src/peripherals/kempston.cpp
  src/peripherals/kempston_mouse.cpp
  src/peripherals/printer.cpp
  src/peripherals/cartridge.cpp
  src/peripherals/divmmc.cpp
  src/formats/dsk.cpp
  src/formats/nex.cpp
  src/formats/sna.cpp
  src/formats/tape.cpp
  src/formats/z80file.cpp
)
target_include_directories(rainbow_core PUBLIC src)
target_compile_options(rainbow_core PRIVATE -Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# Plugin loader (the only part of the core that talks to the operating system)
# ---------------------------------------------------------------------------
add_library(rainbow_plugin STATIC src/plugin/loader.cpp)
target_link_libraries(rainbow_plugin PUBLIC rainbow_core)
target_include_directories(rainbow_plugin PUBLIC src)
target_compile_options(rainbow_plugin PRIVATE -Wall -Wextra -Wpedantic)
if(CMAKE_DL_LIBS)
  target_link_libraries(rainbow_plugin PUBLIC ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------------------
# Example peripheral plugin, built into build/peripherals/ so the emulator and
# the tests find it the same way a user's own plugin would be found.
# ---------------------------------------------------------------------------
add_library(rainbow_fuller MODULE plugins/fuller/fuller.cpp)
target_include_directories(rainbow_fuller PRIVATE src)
set_target_properties(rainbow_fuller PROPERTIES
  PREFIX ""
  OUTPUT_NAME "fuller_joystick"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/peripherals"
)
if(APPLE)
  set_target_properties(rainbow_fuller PROPERTIES SUFFIX ".dylib")
elseif(WIN32)
  set_target_properties(rainbow_fuller PROPERTIES SUFFIX ".dll")
else()
  set_target_properties(rainbow_fuller PROPERTIES SUFFIX ".so")
endif()

# ---------------------------------------------------------------------------
# Frontend (SDL3 + Dear ImGui)
# ---------------------------------------------------------------------------
add_executable(rainbow
  src/frontend/main.cpp
  src/frontend/app.cpp
  src/frontend/autoload.cpp
  src/frontend/config.cpp
  src/frontend/audio.cpp
  src/frontend/input.cpp
  src/frontend/debugger.cpp
  src/frontend/ui.cpp
  src/frontend/screen.cpp
)
target_link_libraries(rainbow PRIVATE rainbow_core rainbow_plugin SDL3::SDL3 imgui Threads::Threads)
# SDL puts the version in the app metadata, which is what a launcher and the
# About box show, so it comes from the project version rather than a literal.
target_compile_definitions(rainbow PRIVATE RAINBOW_VERSION="${PROJECT_VERSION}")
# The example plugin is built into build/peripherals, where the emulator looks.
add_dependencies(rainbow rainbow_fuller)
target_compile_options(rainbow PRIVATE -Wall -Wextra -Wpedantic)

if(APPLE)
  set_target_properties(rainbow PROPERTIES
    MACOSX_RPATH ON
    BUILD_WITH_INSTALL_RPATH OFF
  )
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(RAINBOW_BUILD_TESTS)
  enable_testing()
  add_executable(rainbow_tests
    tests/test_main.cpp
    tests/test_z80.cpp
    tests/test_z80n.cpp
    tests/test_disassembler.cpp
    tests/test_debugger.cpp
    tests/test_state.cpp
    tests/test_rzx.cpp
    tests/test_palette.cpp
    tests/test_sprites.cpp
    tests/test_sprites_relative.cpp
    tests/test_copper.cpp
    tests/test_dma.cpp
    tests/test_expansion.cpp
    tests/test_divmmc.cpp
    tests/test_nex.cpp
    tests/test_esxdos.cpp
    src/frontend/config.cpp
    tests/test_config.cpp
    src/frontend/autoload.cpp
    tests/test_turbosound.cpp
    tests/test_tilemap.cpp
    tests/test_nextreg.cpp
    tests/test_ula.cpp
    tests/test_timex.cpp
    tests/test_ulaplus.cpp
    tests/test_blending.cpp
    tests/test_formats.cpp
    tests/test_tape.cpp
    tests/test_ay.cpp
    tests/test_128k.cpp
    tests/test_128k_boot.cpp
    tests/test_dsk.cpp
    tests/test_boot.cpp
    tests/test_z80test.cpp
    tests/test_rom_selftest.cpp
    tests/test_next_boot.cpp
    tests/test_golden.cpp
    tests/test_lores.cpp
  )
  target_link_libraries(rainbow_tests PRIVATE rainbow_core rainbow_plugin)
  target_compile_options(rainbow_tests PRIVATE -Wall -Wextra -Wpedantic)
  # Lets the golden ROM test look for roms/48.rom in the source tree.
  target_compile_definitions(rainbow_tests PRIVATE
    RAINBOW_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    RAINBOW_PLUGIN_DIR="${CMAKE_BINARY_DIR}/peripherals")
  add_test(NAME rainbow_tests COMMAND rainbow_tests)
endif()
