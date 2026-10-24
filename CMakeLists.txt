cmake_minimum_required(VERSION 3.20)
project(mc-clone VERSION 0.10.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)

# Identity. The app, its executable and the desktop entry are all immc (immc.exe
# on Windows); mc-clone survives as the CMake target and the settings folder, so
# an existing config is still found. version.h is generated so the version
# number exists once, in the project() line above.
set(MC_APP_NAME   "immc")
set(MC_APP_BINARY "${MC_APP_NAME}")
configure_file(${CMAKE_SOURCE_DIR}/src/version.h.in ${CMAKE_BINARY_DIR}/generated/version.h)

# Windows. The code is POSIX, and src/win/posix.h gives the slice of it the program
# uses a Windows meaning: it is force-included into every translation unit of the
# app and the tests, src/win holds the <unistd.h> and <dirent.h> the sources include,
# and the manifest sets the process code page to UTF-8 so a path is the same bytes in
# fopen() as everywhere else. Built with MSVC; ImGui itself is left alone.
if (MSVC)
    add_compile_options(/utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
endif()
function(mc_platform target)
    if (WIN32)
        target_sources(${target} PRIVATE ${CMAKE_SOURCE_DIR}/src/win/posix.cpp
                                          ${CMAKE_SOURCE_DIR}/src/win/utf8.manifest)
        target_include_directories(${target} BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/src/win)
        target_compile_options(${target} PRIVATE /FI${CMAKE_SOURCE_DIR}/src/win/posix.h)
        # netapi32 lists a server's shares, mpr asks for a login to one.
        target_link_libraries(${target} PRIVATE shell32 netapi32 mpr)
        # The 8 MB main-thread stack Linux gives, rather than Windows' 1 MB: the tests
        # keep whole AppPanes objects in main's frame.
        target_link_options(${target} PRIVATE /STACK:8388608)
    endif()
endfunction()
# SDL3 on Windows is a DLL, and a test that cannot find it does not start: put it
# next to every executable that links it.
function(mc_copy_runtime_dlls target)
    if (WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:${target}> $<TARGET_FILE_DIR:${target}>
            COMMAND_EXPAND_LISTS)
    endif()
endfunction()

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

# Pictures. Quick view decodes them with stb_image: one public domain header, no
# system library, fetched by scripts/get-deps.sh into extern_dont_commit/stb the
# same way ImGui is fetched into extern_dont_commit/imgui.
#
# Optional in the same shape as libcurl and FreeType: without the header the app
# builds, runs, and a picture row shows the hex dump it shows today. src/img.cpp
# answers with a reason instead of failing to link, so a clone that never ran the
# script is a supported build rather than a broken one.
set(STB_DIR ${CMAKE_SOURCE_DIR}/extern_dont_commit/stb)
set(MC_STB_FOUND FALSE)
if (EXISTS ${STB_DIR}/stb_image.h)
    set(MC_STB_FOUND TRUE)
else()
    message(STATUS "no stb_image.h: run scripts/get-deps.sh to get picture previews")
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
set(MC_REMOTE_SOURCES src/remote.cpp src/xfer.cpp)
if(CURL_FOUND)
    set(MC_REMOTE_LIBS CURL::libcurl)
    add_compile_definitions(MC_HAVE_CURL)
    message(STATUS "remote sites via libcurl: ${CURL_VERSION_STRING}")
else()
    message(STATUS "no libcurl found: FTP/FTPS/SFTP disabled, the rest of the app is unchanged")
endif()

# Build the application
add_executable(mc-clone src/main.cpp src/panes.cpp src/filesystem.cpp src/config.cpp src/theme.cpp src/archive.cpp src/proc.cpp src/sfv.cpp src/img.cpp src/texture.cpp src/icons.cpp src/text.cpp src/terminal.cpp src/platform.cpp ${MC_REMOTE_SOURCES})
target_link_libraries(mc-clone PRIVATE imgui ${MC_REMOTE_LIBS} Threads::Threads)
# A window, not a console program: on Windows no console opens behind it.
set_target_properties(mc-clone PROPERTIES WIN32_EXECUTABLE ON OUTPUT_NAME ${MC_APP_BINARY})
if (WIN32)
    # The icon and the version Explorer shows (assets/immc.ico is made by
    # scripts/make-icon.py).
    configure_file(${CMAKE_SOURCE_DIR}/src/win/immc.rc.in ${CMAKE_BINARY_DIR}/generated/immc.rc)
    target_sources(mc-clone PRIVATE ${CMAKE_BINARY_DIR}/generated/immc.rc)
    set_source_files_properties(${CMAKE_BINARY_DIR}/generated/immc.rc PROPERTIES
                                OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/assets/immc.ico)
endif()
mc_platform(mc-clone)
mc_copy_runtime_dlls(mc-clone)
target_include_directories(mc-clone PRIVATE src ${CMAKE_BINARY_DIR}/generated ${STB_DIR})
if (MC_STB_FOUND)
    target_compile_definitions(mc-clone PRIVATE MC_HAVE_STB_IMAGE=1)
endif()

# Logic tests. tests/test_panes.cpp includes src/panes.cpp so the file-static
# helpers (quick filter, row->entry view map, marking, clipboard paths) can be
# checked without a display.
enable_testing()
add_executable(test-panes tests/test_panes.cpp src/filesystem.cpp src/config.cpp src/theme.cpp src/archive.cpp src/proc.cpp src/sfv.cpp src/img.cpp src/texture.cpp src/icons.cpp src/text.cpp src/terminal.cpp src/platform.cpp ${MC_REMOTE_SOURCES})
target_link_libraries(test-panes PRIVATE imgui ${MC_REMOTE_LIBS} Threads::Threads)
mc_platform(test-panes)
mc_copy_runtime_dlls(test-panes)
target_include_directories(test-panes PRIVATE src ${CMAKE_BINARY_DIR}/generated ${STB_DIR})
target_compile_definitions(test-panes PRIVATE
    MC_IMAGE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")
if (MC_STB_FOUND)
    target_compile_definitions(test-panes PRIVATE MC_HAVE_STB_IMAGE=1)
endif()
add_test(NAME panes COMMAND test-panes)

# --version is handled before SDL is initialized, so it runs headless.
add_test(NAME version COMMAND mc-clone --version)
set_tests_properties(version PROPERTIES PASS_REGULAR_EXPRESSION "immc [0-9]+\\.[0-9]+\\.[0-9]+")

# Archive backend. It shells out to zip/unzip, tar and 7z, so the test builds
# real archives with those same tools and checks the listing parsers, the
# member round-trip and the progress accounting. No display needed.
add_executable(test-archive tests/test_archive.cpp src/archive.cpp src/proc.cpp src/platform.cpp)
target_include_directories(test-archive PRIVATE src)
mc_platform(test-archive)
add_test(NAME archive COMMAND test-archive)

# Remote backend. The parsers are checked against recorded server output, and the
# transport against a real FTP server started by the test on 127.0.0.1, so the
# MLSD and LIST dialects, the login, the transfers and the mutations are all
# exercised rather than assumed. The test skips the network half when this build
# has no libcurl, and still checks the parsers and the sites file.
add_executable(test-remote tests/test_remote.cpp src/remote.cpp src/xfer.cpp src/filesystem.cpp src/platform.cpp
    src/proc.cpp)
target_include_directories(test-remote PRIVATE src)
mc_platform(test-remote)
target_link_libraries(test-remote PRIVATE ${MC_REMOTE_LIBS} Threads::Threads)
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
mc_platform(test-sfv)
add_test(NAME sfv COMMAND test-sfv)

# Picture decoding. The fixtures are written by the test itself - a BMP byte by
# byte, a real PNG built from stored deflate blocks with the CRC-32 this repository
# already has in src/sfv.cpp, and a JPEG with an EXIF orientation tag - because CI
# installs no image tool and a test that needs one is a test that skips itself.
# No display, no network, no decoder dependency: the sniffing, the EXIF walk and
# the fit run with or without stb present.
add_executable(test-img tests/test_img.cpp src/img.cpp src/sfv.cpp)
target_include_directories(test-img PRIVATE src ${STB_DIR})
mc_platform(test-img)
# The committed JPEG fixtures, by absolute path, so the test runs from any build
# directory and from ctest's own working directory.
target_compile_definitions(test-img PRIVATE
    MC_IMAGE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")
if (MC_STB_FOUND)
    target_compile_definitions(test-img PRIVATE MC_HAVE_STB_IMAGE=1)
endif()
add_test(NAME img COMMAND test-img)

# Which alphabet a preview is written in: UTF-8 validation, the CP437 table, and the
# rule that tells ANSI art from a Western European text file.
add_executable(test-text tests/test_text.cpp src/text.cpp)
target_include_directories(test-text PRIVATE src)
mc_platform(test-text)
add_test(NAME text COMMAND test-text)

# Finding a terminal to run an editor in, and the command line each one actually wants.
add_executable(test-terminal tests/test_terminal.cpp src/terminal.cpp)
target_include_directories(test-terminal PRIVATE src)
mc_platform(test-terminal)
add_test(NAME terminal COMMAND test-terminal)

# The texture upload, against a real GL context. On a machine with no screen it says
# so and passes; on one with a screen it uploads the pixels a decoder produced.
add_executable(test-texture tests/test_texture.cpp src/img.cpp src/sfv.cpp src/texture.cpp)
target_include_directories(test-texture PRIVATE src ${STB_DIR})
target_link_libraries(test-texture PRIVATE SDL3::SDL3 OpenGL::GL)
mc_platform(test-texture)
mc_copy_runtime_dlls(test-texture)
if (MC_STB_FOUND)
    target_compile_definitions(test-texture PRIVATE MC_HAVE_STB_IMAGE=1)
endif()
add_test(NAME texture COMMAND test-texture)

# The whole feature, on screen: a fixture decoded, cached, textured and drawn by the
# real renderer, then read back out of the framebuffer. Skips itself where there is no
# display.
add_executable(test-gl-preview tests/test_gl_preview.cpp src/filesystem.cpp src/config.cpp
    src/theme.cpp src/archive.cpp src/proc.cpp src/sfv.cpp src/img.cpp src/texture.cpp src/icons.cpp src/text.cpp src/terminal.cpp
    src/platform.cpp ${MC_REMOTE_SOURCES})
target_link_libraries(test-gl-preview PRIVATE imgui ${MC_REMOTE_LIBS} Threads::Threads)
mc_platform(test-gl-preview)
mc_copy_runtime_dlls(test-gl-preview)
target_include_directories(test-gl-preview PRIVATE src ${CMAKE_BINARY_DIR}/generated ${STB_DIR})
target_compile_definitions(test-gl-preview PRIVATE MC_IMAGE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")
if (MC_STB_FOUND)
    target_compile_definitions(test-gl-preview PRIVATE MC_HAVE_STB_IMAGE=1)
endif()
add_test(NAME gl-preview COMMAND test-gl-preview)
