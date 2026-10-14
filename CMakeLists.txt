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

# Background transfers run on their own thread, so the app needs a real pthread
# link rather than whatever the SDL3 dependency happens to pull in.
find_package(Threads REQUIRED)

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

# Font rasterization. ImGui's built-in loader is stb_truetype, which does no
# hinting at all: stems land between pixels and come out grey. The FreeType
# loader ImGui ships in misc/ does, and hinting is what makes small monospace
# text legible. It stays optional - without freetype2 the app builds and runs on
# stb_truetype, and the Options dialog says so instead of offering a choice it
# cannot honour.
find_package(Freetype QUIET)
if (TARGET Freetype::Freetype)
    target_sources(imgui PRIVATE ${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp)
    target_include_directories(imgui PUBLIC ${IMGUI_DIR}/misc/freetype)
    target_link_libraries(imgui PUBLIC Freetype::Freetype)
    target_compile_definitions(imgui PUBLIC MC_HAVE_FREETYPE)
endif()

# Remote sites (FTP/FTPS/SFTP) go through libcurl rather than a command-line
# client, for two reasons a password rules out and archives do not: a password in
# argv is readable through /proc by anyone on the machine, and a login plus a TLS
# handshake per directory listing makes browsing unusable. libcurl keeps one
# control connection per site, and never puts a secret in the process table.
#
# Optional, and a build without it is a supported configuration rather than a
# broken one: src/remote.cpp keeps its URL parsing, sites file and listing
# parsers, and only the transport says this build has no FTP support.
find_package(CURL QUIET)
# remote.cpp is compiled either way. What is optional is the library and the
# MC_HAVE_CURL define, not the module: panes.cpp links against net_* in both
# configurations, and without the library the transport half of the module is what
# answers "this build has no FTP support".
set(MC_REMOTE_SOURCES src/remote.cpp)
if(CURL_FOUND)
    set(MC_REMOTE_LIBS CURL::libcurl)
    add_compile_definitions(MC_HAVE_CURL)
    message(STATUS "remote sites via libcurl: ${CURL_VERSION_STRING}")
else()
    message(STATUS "no libcurl found: FTP/FTPS/SFTP disabled, the rest of the app is unchanged")
endif()

# Build the application
add_executable(mc-clone src/main.cpp src/panes.cpp src/filesystem.cpp src/config.cpp src/theme.cpp src/archive.cpp src/proc.cpp src/sfv.cpp ${MC_REMOTE_SOURCES})
target_link_libraries(mc-clone PRIVATE imgui ${MC_REMOTE_LIBS} Threads::Threads)
target_include_directories(mc-clone PRIVATE src ${CMAKE_BINARY_DIR}/generated)

# Logic tests. tests/test_panes.cpp includes src/panes.cpp so the file-static
# helpers (quick filter, row->entry view map, marking, clipboard paths) can be
# checked without a display.
enable_testing()
add_executable(test-panes tests/test_panes.cpp src/filesystem.cpp src/config.cpp src/theme.cpp src/archive.cpp src/proc.cpp src/sfv.cpp ${MC_REMOTE_SOURCES})
target_link_libraries(test-panes PRIVATE imgui ${MC_REMOTE_LIBS} Threads::Threads)
target_include_directories(test-panes PRIVATE src ${CMAKE_BINARY_DIR}/generated)
add_test(NAME panes COMMAND test-panes)

# --version is handled before SDL is initialized, so it runs headless.
add_test(NAME version COMMAND mc-clone --version)
set_tests_properties(version PROPERTIES PASS_REGULAR_EXPRESSION "immc [0-9]+\\.[0-9]+\\.[0-9]+")

# Archive backend. It shells out to zip/unzip, tar and 7z, so the test builds
# real archives with those same tools and checks the listing parsers, the
# member round-trip and the progress accounting. No display needed.
add_executable(test-archive tests/test_archive.cpp src/archive.cpp src/proc.cpp)
target_include_directories(test-archive PRIVATE src)
add_test(NAME archive COMMAND test-archive)

# Remote backend. The parsers are checked against recorded server output, and the
# transport against a real FTP server started by the test on 127.0.0.1, so the
# MLSD and LIST dialects, the login, the transfers and the mutations are all
# exercised rather than assumed. The test skips the network half when this build
# has no libcurl, and still checks the parsers and the sites file.
add_executable(test-remote tests/test_remote.cpp src/remote.cpp src/filesystem.cpp)
target_include_directories(test-remote PRIVATE src)
target_link_libraries(test-remote PRIVATE ${MC_REMOTE_LIBS})
# The test starts the fixture server by absolute path, so it runs from any build
# directory and from ctest's own working directory.
target_compile_definitions(test-remote PRIVATE
    MC_FTP_SERVER="${CMAKE_CURRENT_SOURCE_DIR}/tests/ftp_server.py")
add_test(NAME remote COMMAND test-remote)

# .sfv verification. The CRC is checked against the published check value and the
# parser against the shapes real .sfv writers produce - names with spaces, CRLF,
# comments, backslash separators, checksums written first - so the format is read
# the way it arrives rather than the way it is documented. No display, no network.
add_executable(test-sfv tests/test_sfv.cpp src/sfv.cpp)
target_include_directories(test-sfv PRIVATE src)
add_test(NAME sfv COMMAND test-sfv)
