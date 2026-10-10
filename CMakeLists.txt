cmake_minimum_required(VERSION 3.20)
project(mc-clone VERSION 0.9.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)

# Identity. The repository and the desktop entry call the app immc; the
# executable it builds is still mc-clone. version.h is generated so the version
# number exists once, in the project() line above.
set(MC_APP_NAME   "immc")
set(MC_APP_BINARY "${PROJECT_NAME}")
configure_file(${CMAKE_SOURCE_DIR}/src/version.h.in ${CMAKE_BINARY_DIR}/generated/version.h)

# Find SDL3
find_package(SDL3 REQUIRED)

# OpenGL
find_package(OpenGL REQUIRED)

# ImGui from local extern_dont_commit
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/extern_dont_commit/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_link_libraries(imgui PUBLIC SDL3::SDL3 OpenGL::GL)

# Build the application
add_executable(mc-clone src/main.cpp src/panes.cpp src/filesystem.cpp src/config.cpp src/theme.cpp src/archive.cpp)
target_link_libraries(mc-clone PRIVATE imgui)
target_include_directories(mc-clone PRIVATE src ${CMAKE_BINARY_DIR}/generated)

# Logic tests. tests/test_panes.cpp includes src/panes.cpp so the file-static
# helpers (quick filter, row->entry view map, marking, clipboard paths) can be
# checked without a display.
enable_testing()
add_executable(test-panes tests/test_panes.cpp src/filesystem.cpp src/config.cpp src/theme.cpp src/archive.cpp)
target_link_libraries(test-panes PRIVATE imgui)
target_include_directories(test-panes PRIVATE src ${CMAKE_BINARY_DIR}/generated)
add_test(NAME panes COMMAND test-panes)

# --version is handled before SDL is initialized, so it runs headless.
add_test(NAME version COMMAND mc-clone --version)
set_tests_properties(version PROPERTIES PASS_REGULAR_EXPRESSION "immc [0-9]+\\.[0-9]+\\.[0-9]+")

# Archive backend. It shells out to zip/unzip, tar and 7z, so the test builds
# real archives with those same tools and checks the listing parsers, the
# member round-trip and the progress accounting. No display needed.
add_executable(test-archive tests/test_archive.cpp src/archive.cpp)
target_include_directories(test-archive PRIVATE src)
add_test(NAME archive COMMAND test-archive)
