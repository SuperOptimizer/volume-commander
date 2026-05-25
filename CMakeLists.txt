cmake_minimum_required(VERSION 3.28)
project(volume_commander VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# Default release flags; a preset (e.g. valgrind = AVX2-only) can override by
# setting CMAKE_CXX_FLAGS_RELEASE in its cache, so only set the default here.
if(NOT CMAKE_CXX_FLAGS_RELEASE)
  set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -funroll-loops -DNDEBUG")
endif()

# --- c3d volumetric codec + libs3 (vendored sibling repos by default) ---
# Disable OpenMP entirely: c3d decode stays single-threaded; ALL parallelism
# comes from the IO pool (one chunk per worker thread). Nested OMP otherwise
# spawns hundreds of threads and pegs the CPU.
set(CMAKE_DISABLE_FIND_PACKAGE_OpenMP ON)
set(C3D_DIR   "$ENV{HOME}/c3d"   CACHE PATH "Path to the c3d source tree")
set(LIBS3_DIR "$ENV{HOME}/libs3" CACHE PATH "Path to the libs3 source tree")
add_subdirectory(${C3D_DIR}   ${CMAKE_BINARY_DIR}/c3d-build)
add_subdirectory(${LIBS3_DIR} ${CMAKE_BINARY_DIR}/libs3-build)

# --- third-party ---
find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick Qml)
find_package(CURL REQUIRED)
find_package(nlohmann_json QUIET)
find_package(PkgConfig REQUIRED)
pkg_check_modules(BLOSC2 IMPORTED_TARGET blosc2)
pkg_check_modules(TIFF REQUIRED IMPORTED_TARGET libtiff-4)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

qt_standard_project_setup()

add_subdirectory(src)

# Headless render benchmark (no Qt) — replays a VC_TRACE frame log.
add_executable(replay bench/replay.cpp
  src/data/volume.cpp src/data/mask.cpp
  src/render/surface.cpp src/render/renderer.cpp
  src/utils/c3d_codec.cpp)
target_include_directories(replay PRIVATE src)
target_link_libraries(replay PRIVATE ZLIB::ZLIB Threads::Threads PkgConfig::TIFF c3d s3)
if(BLOSC2_FOUND)
  target_link_libraries(replay PRIVATE PkgConfig::BLOSC2)
endif()

# mirror: copy the S3 shards a trace touches into a local zarr (one-time, for
# fully-offline replay/tests).
add_executable(mirror bench/mirror.cpp
  src/data/volume.cpp src/data/mask.cpp
  src/render/surface.cpp src/render/renderer.cpp src/utils/c3d_codec.cpp)
target_include_directories(mirror PRIVATE src)
target_link_libraries(mirror PRIVATE ZLIB::ZLIB Threads::Threads PkgConfig::TIFF c3d s3)
if(BLOSC2_FOUND)
  target_link_libraries(mirror PRIVATE PkgConfig::BLOSC2)
endif()

# Self-contained unit tests (no S3). `ctest` or run ./build/test_render.
enable_testing()
add_executable(test_render tests/test_render.cpp src/render/surface.cpp src/utils/c3d_codec.cpp)
target_include_directories(test_render PRIVATE src)
target_link_libraries(test_render PRIVATE Threads::Threads PkgConfig::TIFF c3d s3)
add_test(NAME render_logic COMMAND test_render)
