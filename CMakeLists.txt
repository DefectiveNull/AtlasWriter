cmake_minimum_required(VERSION 3.24)

project(AtlasWriter VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

add_executable(AtlasWriter
    src/main.cpp
    src/App.cpp
    src/App.h
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
    src/markdown/Html.cpp
    src/markdown/Html.h
    src/markdown/Markdown.cpp
    src/markdown/Markdown.h
    src/markdown/MarkdownView.cpp
    src/markdown/MarkdownView.h
    src/markdown/OutlinePanel.cpp
    src/markdown/OutlinePanel.h
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
)

target_include_directories(AtlasWriter PRIVATE src)
target_link_libraries(AtlasWriter PRIVATE ${ATLASWRITER_SDL_TARGET} stb)

# Fonts etc. are loaded relative to the executable
add_custom_command(TARGET AtlasWriter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:AtlasWriter>/assets"
    COMMAND AtlasWriter --write-example-settings
            "$<TARGET_FILE_DIR:AtlasWriter>/settings.example.ccl"
)

if(MSVC)
    target_compile_options(AtlasWriter PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(AtlasWriter PRIVATE -Wall -Wextra -Wpedantic)
endif()

# The terminal uses forkpty(), which lives in libutil on Linux and the BSDs.
if(UNIX AND NOT APPLE)
    target_link_libraries(AtlasWriter PRIVATE util)
endif()
