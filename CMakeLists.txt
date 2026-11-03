cmake_minimum_required(VERSION 3.24)

project(AtlasWriter VERSION 1.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Static runtime, so a release build runs without Visual Studio's redistributables.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Ninja finds header dependencies by matching the compiler's localized
# "Note: including file:" prefix. CMake's own detection mangles non-ASCII
# prefixes, which silently disables header tracking, so read the real one.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    set(probe_dir "${CMAKE_BINARY_DIR}/CMakeFiles/showincludes")
    file(WRITE "${probe_dir}/probe.h" "")
    file(WRITE "${probe_dir}/probe.cpp" "#include \"probe.h\"\n")
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" /nologo /c /showIncludes /utf-8 probe.cpp
        WORKING_DIRECTORY "${probe_dir}"
        OUTPUT_VARIABLE probe_output
        ERROR_VARIABLE probe_output
        ENCODING UTF8)
    if(probe_output MATCHES "([^\r\n]*: )[A-Za-z]:[^\r\n]*probe\\.h")
        set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_1}")
        set(CMAKE_C_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_1}")
        set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_1}")
    endif()
endif()

# ---------------------------------------------------------------------------
# SDL3: use a system install if asked, otherwise download and build it
# statically as part of the project (no install step needed on any platform).
# ---------------------------------------------------------------------------
option(ATLASWRITER_USE_SYSTEM_SDL "Use an installed SDL3 instead of fetching it" OFF)

if(ATLASWRITER_USE_SYSTEM_SDL)
    find_package(SDL3 REQUIRED CONFIG)
    set(ATLASWRITER_SDL_TARGET SDL3::SDL3)
else()
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(SDL3)
    set(ATLASWRITER_SDL_TARGET SDL3::SDL3-static)
endif()

# stb_truetype and stb_image, compiled once in their own target. SYSTEM keeps
# their warnings out of our builds.
add_library(stb STATIC third_party/stb/stb_truetype.c third_party/stb/stb_image.c)
target_include_directories(stb SYSTEM PUBLIC third_party/stb)

# NanoSVG rasterizes icon.svg at startup.
add_library(nanosvg STATIC third_party/nanosvg/nanosvg.c)
target_include_directories(nanosvg SYSTEM PUBLIC third_party/nanosvg)
if(UNIX)
    target_link_libraries(nanosvg PUBLIC m)
endif()

# The audio tile's decoders: dr_wav, dr_mp3, dr_flac and stb_vorbis
# (public domain / MIT-0).
add_library(codecs STATIC third_party/dr_libs/dr_libs.c third_party/stb/stb_vorbis.c)
target_include_directories(codecs SYSTEM PUBLIC third_party/dr_libs third_party/stb)
if(UNIX)
    target_link_libraries(codecs PUBLIC m)
endif()

# PDFium for the reader tile: prebuilt binaries (bblanchon/pdfium-binaries,
# BSD-3), loaded at run time from next to the executable. Only the library
# is needed; AtlasWriter declares the few functions it calls itself.
option(ATLASWRITER_PDFIUM "Download PDFium so the reader tile can show PDFs" ON)
set(ATLASWRITER_PDFIUM_LIBRARY "")
if(ATLASWRITER_PDFIUM)
    set(pdfium_release "chromium%2F8100")
    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(pdfium_archive pdfium-win-x64.tgz)
        set(pdfium_file bin/pdfium.dll)
    elseif(APPLE)
        set(pdfium_archive pdfium-mac-univ.tgz)
        set(pdfium_file lib/libpdfium.dylib)
    elseif(UNIX AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        set(pdfium_archive pdfium-linux-x64.tgz)
        set(pdfium_file lib/libpdfium.so)
    endif()
    if(pdfium_archive)
        include(FetchContent)
        FetchContent_Declare(pdfium
            URL https://github.com/bblanchon/pdfium-binaries/releases/download/${pdfium_release}/${pdfium_archive}
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_MakeAvailable(pdfium)
        set(ATLASWRITER_PDFIUM_LIBRARY "${pdfium_SOURCE_DIR}/${pdfium_file}")
    else()
        message(STATUS "No prebuilt PDFium for this platform: PDFs won't open")
    endif()
endif()

# WebView2 for websites in the web tile (Windows): the SDK's headers and its
# static loader, from the NuGet package. Pages need the WebView2 Runtime,
# which comes with Microsoft Edge on Windows 10 and 11.
option(ATLASWRITER_WEBVIEW2 "Show websites in web tiles with WebView2 (Windows)" ON)
set(ATLASWRITER_USE_WEBVIEW2 OFF)
if(ATLASWRITER_WEBVIEW2 AND WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    include(FetchContent)
    FetchContent_Declare(webview2
        URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.4314.35
        DOWNLOAD_NAME webview2.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(webview2)
    set(ATLASWRITER_USE_WEBVIEW2 ON)
endif()

add_executable(AtlasWriter
    src/main.cpp
    src/App.cpp
    src/App.h
    src/audio/AudioDecoder.cpp
    src/audio/AudioDecoder.h
    src/audio/AudioWorkspace.cpp
    src/audio/AudioWorkspace.h
    src/config/Ccl.cpp
    src/config/Ccl.h
    src/config/Commands.h
    src/config/Settings.cpp
    src/config/Settings.h
    src/config/Shortcut.cpp
    src/config/Shortcut.h
    src/editor/EditorView.cpp
    src/editor/EditorView.h
    src/editor/FileType.cpp
    src/editor/FileType.h
    src/editor/LineWrap.cpp
    src/editor/LineWrap.h
    src/editor/Syntax.cpp
    src/editor/Syntax.h
    src/editor/TextBuffer.cpp
    src/editor/TextBuffer.h
    src/editor/UndoHistory.cpp
    src/editor/UndoHistory.h
    src/embed/Embed.cpp
    src/embed/Embed.h
    src/embed/SessionAssets.cpp
    src/embed/SessionAssets.h
    src/markdown/Html.cpp
    src/markdown/Html.h
    src/markdown/Markdown.cpp
    src/markdown/Markdown.h
    src/markdown/FileMeta.cpp
    src/markdown/FileMeta.h
    src/markdown/Meta.cpp
    src/markdown/Meta.h
    src/markdown/MarkdownView.cpp
    src/markdown/MarkdownView.h
    src/markdown/OutlinePanel.cpp
    src/markdown/OutlinePanel.h
    src/reader/Epub.cpp
    src/reader/Epub.h
    src/reader/Pdf.cpp
    src/reader/Pdf.h
    src/reader/ReaderWorkspace.cpp
    src/reader/ReaderWorkspace.h
    src/reader/Zip.cpp
    src/reader/Zip.h
    src/terminal/Pty.cpp
    src/terminal/Pty.h
    src/terminal/TerminalScreen.cpp
    src/terminal/TerminalScreen.h
    src/terminal/TerminalView.cpp
    src/terminal/TerminalView.h
    src/text/Font.cpp
    src/text/Font.h
    src/text/GlyphAtlas.cpp
    src/text/GlyphAtlas.h
    src/text/TextRenderer.cpp
    src/text/TextRenderer.h
    src/text/Utf8.h
    src/ui/Draw.h
    src/ui/ImageCache.cpp
    src/ui/ImageCache.h
    src/ui/Keys.h
    src/ui/MenuBar.cpp
    src/ui/MenuBar.h
    src/ui/Picker.cpp
    src/ui/Picker.h
    src/ui/PopupMenu.cpp
    src/ui/PopupMenu.h
    src/ui/Toolbar.cpp
    src/ui/Toolbar.h
    src/update/Sha256.h
    src/update/Updater.cpp
    src/update/Updater.h
    src/workspace/Document.cpp
    src/workspace/Document.h
    src/workspace/Layout.cpp
    src/workspace/Layout.h
    src/workspace/TileTree.cpp
    src/workspace/TileTree.h
    src/workspace/Workspace.h
    src/workspace/Workspaces.cpp
    src/web/WebView.cpp
    src/web/WebView.h
    src/web/WebWorkspace.cpp
    src/web/WebWorkspace.h
)

# A GUI executable: as a console one, Windows groups the console with the main
# window on one taskbar button and shows the .exe's icon instead of icon.svg.
if(WIN32)
    target_sources(AtlasWriter PRIVATE src/app.rc)
    set_target_properties(AtlasWriter PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

target_compile_definitions(AtlasWriter PRIVATE ATLASWRITER_VERSION="${PROJECT_VERSION}")
target_include_directories(AtlasWriter PRIVATE src)
target_link_libraries(AtlasWriter PRIVATE ${ATLASWRITER_SDL_TARGET} stb nanosvg codecs)
if(ATLASWRITER_USE_WEBVIEW2)
    target_compile_definitions(AtlasWriter PRIVATE ATLASWRITER_WEBVIEW2)
    target_include_directories(AtlasWriter SYSTEM PRIVATE "${webview2_SOURCE_DIR}/build/native/include")
    target_link_libraries(AtlasWriter PRIVATE "${webview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib"
                          version shlwapi)
endif()
if(ATLASWRITER_PDFIUM_LIBRARY)
    add_custom_command(TARGET AtlasWriter POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ATLASWRITER_PDFIUM_LIBRARY}" "$<TARGET_FILE_DIR:AtlasWriter>")
endif()

# Fonts etc. are loaded relative to the executable
add_custom_command(TARGET AtlasWriter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:AtlasWriter>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/assets/icon.svg" "$<TARGET_FILE_DIR:AtlasWriter>/icon.svg"
    COMMAND AtlasWriter --write-example-settings
            "$<TARGET_FILE_DIR:AtlasWriter>/settings.example.ccl"
)

# Windows: the Explorer property handler that shows a Markdown file's ```meta
# block (title, tags, author...) in File Explorer. See src/explorer.
if(WIN32)
    add_library(AtlasWriterProps SHARED
        src/explorer/PropertyHandler.cpp
        src/explorer/AtlasWriterProps.def
        src/markdown/Meta.cpp
        src/markdown/Meta.h
        src/config/Ccl.cpp
        src/config/Ccl.h
    )
    target_include_directories(AtlasWriterProps PRIVATE src)
    target_compile_definitions(AtlasWriterProps PRIVATE UNICODE _UNICODE)
    target_link_libraries(AtlasWriterProps PRIVATE propsys shlwapi)
    if(MSVC)
        target_compile_options(AtlasWriterProps PRIVATE /W4 /permissive- /utf-8)
    endif()
    # Next to the executable, so a build folder works with install.ps1 too.
    set_target_properties(AtlasWriterProps PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_dependencies(AtlasWriter AtlasWriterProps)
endif()

# The release layout: cmake --install build --prefix dist/AtlasWriter
install(TARGETS AtlasWriter RUNTIME DESTINATION .)
install(DIRECTORY assets/ DESTINATION assets)
install(FILES
    assets/icon.svg
    LICENSE
    README.md
    "$<TARGET_FILE_DIR:AtlasWriter>/settings.example.ccl"
    DESTINATION .)
if(WIN32)
    install(FILES "$<TARGET_FILE:AtlasWriterProps>" DESTINATION .) # the DLL only, no import library
    install(FILES install/install.ps1 install/uninstall.ps1 install/explorer.ps1 DESTINATION .)
else()
    install(PROGRAMS install/install.sh install/uninstall.sh DESTINATION .)
endif()
install(FILES third_party/nanosvg/LICENSE.txt DESTINATION licenses RENAME nanosvg.txt)
if(NOT ATLASWRITER_USE_SYSTEM_SDL)
    install(FILES "${sdl3_SOURCE_DIR}/LICENSE.txt" DESTINATION licenses RENAME SDL3.txt)
endif()
if(ATLASWRITER_PDFIUM_LIBRARY)
    install(FILES "${ATLASWRITER_PDFIUM_LIBRARY}" DESTINATION .)
    install(FILES "${pdfium_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME PDFium.txt)
    install(DIRECTORY "${pdfium_SOURCE_DIR}/licenses/" DESTINATION licenses/PDFium)
endif()
if(ATLASWRITER_USE_WEBVIEW2)
    install(FILES "${webview2_SOURCE_DIR}/LICENSE.txt" DESTINATION licenses RENAME WebView2.txt)
endif()

if(MSVC)
    target_compile_options(AtlasWriter PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(AtlasWriter PRIVATE -Wall -Wextra -Wpedantic)
endif()

# The terminal uses forkpty(), which lives in libutil on Linux and the BSDs.
if(UNIX AND NOT APPLE)
    target_link_libraries(AtlasWriter PRIVATE util)
endif()
