#include "FileType.h"

#include <SDL3/SDL.h>

#include <string>

namespace {

struct Extension
{
    const char* ext; // lowercase, without the dot
    FileKind kind;
    const char* name;
};

constexpr Extension kExtensions[] = {
    {"txt", FileKind::Text, "Plain Text"},
    {"text", FileKind::Text, "Plain Text"},
    {"log", FileKind::Text, "Log"},

    {"md", FileKind::Markdown, "Markdown"},
    {"markdown", FileKind::Markdown, "Markdown"},
    {"mdown", FileKind::Markdown, "Markdown"},
    {"mkd", FileKind::Markdown, "Markdown"},
    {"mkdn", FileKind::Markdown, "Markdown"},

    {"c", FileKind::Code, "C"},
    {"h", FileKind::Code, "C/C++ Header"},
    {"cpp", FileKind::Code, "C++"},
    {"cc", FileKind::Code, "C++"},
    {"cxx", FileKind::Code, "C++"},
    {"c++", FileKind::Code, "C++"},
    {"cppm", FileKind::Code, "C++"},
    {"ixx", FileKind::Code, "C++"},
    {"hpp", FileKind::Code, "C++ Header"},
    {"hh", FileKind::Code, "C++ Header"},
    {"hxx", FileKind::Code, "C++ Header"},
    {"h++", FileKind::Code, "C++ Header"},
    {"inl", FileKind::Code, "C++"},
    {"ipp", FileKind::Code, "C++"},
    {"m", FileKind::Code, "Objective-C"},
    {"mm", FileKind::Code, "Objective-C++"},
    {"cs", FileKind::Code, "C#"},
    {"java", FileKind::Code, "Java"},
    {"kt", FileKind::Code, "Kotlin"},
    {"kts", FileKind::Code, "Kotlin"},
    {"scala", FileKind::Code, "Scala"},
    {"go", FileKind::Code, "Go"},
    {"rs", FileKind::Code, "Rust"},
    {"zig", FileKind::Code, "Zig"},
    {"odin", FileKind::Code, "Odin"},
    {"nim", FileKind::Code, "Nim"},
    {"d", FileKind::Code, "D"},
    {"swift", FileKind::Code, "Swift"},
    {"dart", FileKind::Code, "Dart"},
    {"py", FileKind::Code, "Python"},
    {"pyw", FileKind::Code, "Python"},
    {"pyi", FileKind::Code, "Python"},
    {"rb", FileKind::Code, "Ruby"},
    {"php", FileKind::Code, "PHP"},
    {"pl", FileKind::Code, "Perl"},
    {"lua", FileKind::Code, "Lua"},
    {"r", FileKind::Code, "R"},
    {"jl", FileKind::Code, "Julia"},
    {"hs", FileKind::Code, "Haskell"},
    {"ml", FileKind::Code, "OCaml"},
    {"mli", FileKind::Code, "OCaml"},
    {"fs", FileKind::Code, "F#"},
    {"fsx", FileKind::Code, "F#"},
    {"ex", FileKind::Code, "Elixir"},
    {"exs", FileKind::Code, "Elixir"},
    {"erl", FileKind::Code, "Erlang"},
    {"clj", FileKind::Code, "Clojure"},
    {"gd", FileKind::Code, "GDScript"},
    {"js", FileKind::Code, "JavaScript"},
    {"mjs", FileKind::Code, "JavaScript"},
    {"cjs", FileKind::Code, "JavaScript"},
    {"jsx", FileKind::Code, "JavaScript JSX"},
    {"ts", FileKind::Code, "TypeScript"},
    {"mts", FileKind::Code, "TypeScript"},
    {"cts", FileKind::Code, "TypeScript"},
    {"tsx", FileKind::Code, "TypeScript JSX"},
    {"vue", FileKind::Code, "Vue"},
    {"svelte", FileKind::Code, "Svelte"},
    {"html", FileKind::Code, "HTML"},
    {"htm", FileKind::Code, "HTML"},
    {"css", FileKind::Code, "CSS"},
    {"scss", FileKind::Code, "SCSS"},
    {"sass", FileKind::Code, "Sass"},
    {"less", FileKind::Code, "Less"},
    {"xml", FileKind::Code, "XML"},
    {"svg", FileKind::Code, "SVG"},
    {"json", FileKind::Code, "JSON"},
    {"jsonc", FileKind::Code, "JSON"},
    {"yaml", FileKind::Code, "YAML"},
    {"yml", FileKind::Code, "YAML"},
    {"toml", FileKind::Code, "TOML"},
    {"ini", FileKind::Code, "INI"},
    {"ccl", FileKind::Code, "CCL"},
    {"cfg", FileKind::Code, "Config"},
    {"conf", FileKind::Code, "Config"},
    {"sql", FileKind::Code, "SQL"},
    {"sh", FileKind::Code, "Shell"},
    {"bash", FileKind::Code, "Shell"},
    {"zsh", FileKind::Code, "Shell"},
    {"fish", FileKind::Code, "Fish"},
    {"ps1", FileKind::Code, "PowerShell"},
    {"psm1", FileKind::Code, "PowerShell"},
    {"bat", FileKind::Code, "Batch"},
    {"cmd", FileKind::Code, "Batch"},
    {"cmake", FileKind::Code, "CMake"},
    {"mk", FileKind::Code, "Makefile"},
    {"glsl", FileKind::Code, "GLSL"},
    {"vert", FileKind::Code, "GLSL"},
    {"frag", FileKind::Code, "GLSL"},
    {"geom", FileKind::Code, "GLSL"},
    {"comp", FileKind::Code, "GLSL"},
    {"hlsl", FileKind::Code, "HLSL"},
    {"wgsl", FileKind::Code, "WGSL"},
    {"metal", FileKind::Code, "Metal"},
    {"asm", FileKind::Code, "Assembly"},
    {"s", FileKind::Code, "Assembly"},
};

// Files recognized by their whole name, checked before the extension
// (CMakeLists.txt is code, not plain text).
constexpr Extension kFileNames[] = {
    {"cmakelists.txt", FileKind::Code, "CMake"},
    {"makefile", FileKind::Code, "Makefile"},
    {"gnumakefile", FileKind::Code, "Makefile"},
    {"dockerfile", FileKind::Code, "Dockerfile"},
    {"meson.build", FileKind::Code, "Meson"},
    {".gitignore", FileKind::Code, "Git Config"},
    {".gitattributes", FileKind::Code, "Git Config"},
    {".gitmodules", FileKind::Code, "Git Config"},
    {".editorconfig", FileKind::Code, "EditorConfig"},
    {".clang-format", FileKind::Code, "YAML"},
};

std::string lowercase(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(SDL_tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string joinExtensions(bool (*wanted)(FileKind))
{
    std::string out;
    for (const Extension& e : kExtensions) {
        // SDL dialog patterns only allow [A-Za-z0-9._-]; drop "c++" and friends.
        if (!wanted(e.kind) || SDL_strchr(e.ext, '+'))
            continue;
        if (!out.empty())
            out += ';';
        out += e.ext;
    }
    return out;
}

} // namespace

FileType detectFileType(std::string_view path)
{
    const size_t slash = path.find_last_of("/\\");
    const std::string name = lowercase(slash == std::string_view::npos ? path : path.substr(slash + 1));

    for (const Extension& f : kFileNames) {
        if (name == f.ext)
            return {f.kind, f.name};
    }

    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0)
        return {};
    const std::string_view ext = std::string_view(name).substr(dot + 1);
    for (const Extension& e : kExtensions) {
        if (ext == e.ext)
            return {e.kind, e.name};
    }
    return {};
}

const char* markdownExtensions()
{
    static const std::string list = joinExtensions([](FileKind k) { return k == FileKind::Markdown; });
    return list.c_str();
}

const char* codeExtensions()
{
    static const std::string list = joinExtensions([](FileKind k) { return k == FileKind::Code; });
    return list.c_str();
}

const char* supportedExtensions()
{
    static const std::string list = joinExtensions([](FileKind) { return true; });
    return list.c_str();
}
