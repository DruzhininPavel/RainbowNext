cmake_minimum_required(VERSION 3.24)
project(myNext VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(MYNEXT_BUILD_TESTS "Build unit tests" ON)
option(MYNEXT_FETCH_IMGUI "Fetch Dear ImGui" ON)
option(MYNEXT_FETCH_SDL3 "Build SDL3 from source when no system SDL3 is found" ON)

include(FetchContent)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

find_package(SDL3 3.2 QUIET)
if(NOT SDL3_FOUND)
  if(MYNEXT_FETCH_SDL3)
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
      "or configure with -DMYNEXT_FETCH_SDL3=ON.")
  endif()
endif()

if(MYNEXT_FETCH_IMGUI)
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
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
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
add_library(mynext_core STATIC
  src/core/z80.cpp
  src/core/machine.cpp
  src/core/ula.cpp
  src/core/keyboard.cpp
  src/core/beeper.cpp
  src/formats/sna.cpp
  src/formats/z80file.cpp
)
target_include_directories(mynext_core PUBLIC src)
target_compile_options(mynext_core PRIVATE -Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# Frontend (SDL3 + Dear ImGui)
# ---------------------------------------------------------------------------
add_executable(mynext
  src/frontend/main.cpp
  src/frontend/app.cpp
  src/frontend/audio.cpp
  src/frontend/input.cpp
  src/frontend/debugger.cpp
)
target_link_libraries(mynext PRIVATE mynext_core SDL3::SDL3 imgui Threads::Threads)
target_compile_options(mynext PRIVATE -Wall -Wextra -Wpedantic)

if(APPLE)
  set_target_properties(mynext PROPERTIES
    MACOSX_RPATH ON
    BUILD_WITH_INSTALL_RPATH OFF
  )
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(MYNEXT_BUILD_TESTS)
  enable_testing()
  add_executable(mynext_tests
    tests/test_main.cpp
    tests/test_z80.cpp
    tests/test_ula.cpp
    tests/test_formats.cpp
    tests/test_boot.cpp
  )
  target_link_libraries(mynext_tests PRIVATE mynext_core)
  target_compile_options(mynext_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME mynext_tests COMMAND mynext_tests)
endif()
