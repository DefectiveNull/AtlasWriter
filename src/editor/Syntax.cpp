#include "Syntax.h"

#include "editor/TextBuffer.h"

#include <algorithm>
#include <unordered_set>

namespace syntax {

// What a language looks like to the scanner. Word lists are space-separated.
struct Language
{
    const char* names;       // FileType names this applies to, '|'-separated
    const char* keywords;
    const char* types;
    const char* literals;    // true, false, null, ...
    const char* lineComment; // nullptr = none
    const char* lineComment2;
    const char* blockOpen;   // nullptr = none
    const char* blockClose;
    bool hashDirectives;     // C-style #include / #define lines
    bool charLiterals;       // 'x' is a character, a lone ' is not a string
    bool tripleQuotes;       // Python """ / ''' strings, may span lines
    bool backticks;          // `...` strings (JS templates, Go raw), may span lines
    bool caseInsensitive;    // SQL, CMake, Batch
    bool capitalizedTypes;   // Identifiers starting with A-Z are types
    bool annotations;        // @decorator / @Annotation
    bool variables;          // $var / ${var}

    std::unordered_set<std::string_view> keywordSet, typeSet, literalSet;
};

namespace {

enum : State { kNormal = 0, kBlockComment, kTripleDouble, kTripleSingle, kBacktick };

constexpr const char* kCKeywords =
    "auto break case const continue default do else enum extern for goto if inline register restrict return "
    "sizeof static struct switch typedef union volatile while _Alignas _Alignof _Atomic _Generic _Noreturn "
    "_Static_assert _Thread_local";
constexpr const char* kCppKeywords =
    "alignas alignof and asm auto break case catch class co_await co_return co_yield concept const consteval "
    "constexpr constinit const_cast continue decltype default delete do dynamic_cast else enum explicit export "
    "extern final for friend goto if import inline module mutable namespace new noexcept not operator or override "
    "private protected public register reinterpret_cast requires return sizeof static static_assert static_cast "
    "struct switch template this thread_local throw try typedef typeid typename union using virtual volatile "
    "while xor";
constexpr const char* kCTypes =
    "bool char char8_t char16_t char32_t double float int long short signed unsigned void wchar_t size_t "
    "ptrdiff_t intptr_t uintptr_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t FILE";
constexpr const char* kCLiterals = "true false nullptr NULL";

Language kLanguages[] = {
    {"C|C/C++ Header|Objective-C", kCKeywords, kCTypes, kCLiterals, "//", nullptr, "/*", "*/",
     true, true, false, false, false, false, false, false, {}, {}, {}},
    {"C++|C++ Header|Objective-C++|CUDA", kCppKeywords, kCTypes, kCLiterals, "//", nullptr, "/*", "*/",
     true, true, false, false, false, false, false, false, {}, {}, {}},
    {"C#",
     "abstract as async await base break case catch checked class const continue default delegate do else enum "
     "event explicit extern finally fixed for foreach get goto if implicit in init interface internal is lock "
     "namespace new operator out override params partial private protected public readonly record ref required "
     "return sealed set sizeof stackalloc static struct switch this throw try typeof unchecked unsafe using var "
     "virtual volatile when where while yield",
     "bool byte char decimal double dynamic float int long nint nuint object sbyte short string uint ulong ushort void",
     "true false null", "//", nullptr, "/*", "*/", true, true, false, false, false, true, false, false, {}, {}, {}},
    {"Java|Kotlin|Scala|Dart|Groovy",
     "abstract assert break case catch class const continue default do else enum extends final finally for goto "
     "if implements import instanceof interface native new package private protected public return static "
     "strictfp super switch synchronized this throw throws transient try volatile while var val fun object "
     "companion data sealed when is in out override open internal lateinit by init def trait extends with "
     "match yield lazy implicit async await late required",
     "boolean byte char double float int long short void String Int Long Unit Any",
     "true false null", "//", nullptr, "/*", "*/", false, true, false, false, false, true, true, false, {}, {}, {}},
    {"Go",
     "break case chan const continue default defer else fallthrough for func go goto if import interface map "
     "package range return select struct switch type var",
     "bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune string uint uint8 "
     "uint16 uint32 uint64 uintptr any",
     "true false nil iota", "//", nullptr, "/*", "*/", false, true, false, true, false, false, false, false, {}, {}, {}},
    {"Rust",
     "as async await break const continue crate dyn else enum extern fn for if impl in let loop match mod move "
     "mut pub ref return self Self static struct super trait type unsafe use where while macro_rules union",
     "bool char f32 f64 i8 i16 i32 i64 i128 isize str u8 u16 u32 u64 u128 usize String Vec Option Result Box",
     "true false None Some Ok Err", "//", nullptr, "/*", "*/", false, true, false, false, false, true, false, false,
     {}, {}, {}},
    {"Zig|Odin|Nim|D",
     "align allowzero and anyframe anytype asm async await break callconv catch comptime const continue defer else "
     "enum errdefer error export extern fn for if inline noalias nosuspend or orelse packed pub resume return "
     "struct suspend switch test threadlocal try union unreachable usingnamespace var volatile while proc import "
     "package when do case cast distinct using foreign proc_group map dynamic",
     "bool f16 f32 f64 i8 i16 i32 i64 isize u8 u16 u32 u64 usize void type anyerror int uint string rune",
     "true false null undefined nil", "//", nullptr, "/*", "*/", false, true, false, false, false, false, false,
     false, {}, {}, {}},
    {"Swift",
     "associatedtype break case catch class continue default defer deinit do else enum extension fallthrough "
     "fileprivate for func guard if import in init inout internal let open operator private protocol public "
     "repeat rethrows return self Self static struct subscript super switch throw throws try typealias var where "
     "while async await actor some any",
     "Int Double Float String Bool Character Void Any", "true false nil", "//", nullptr, "/*", "*/", true, false,
     false, false, false, true, true, false, {}, {}, {}},
    {"JavaScript|JavaScript JSX|TypeScript|TypeScript JSX|Vue|Svelte",
     "as async await break case catch class const continue debugger default delete do else enum export extends "
     "finally for from function get if implements import in instanceof interface let new of package private "
     "protected public readonly return set static super switch this throw try type typeof var void while with "
     "yield declare namespace abstract keyof satisfies",
     "any boolean number string symbol bigint object never unknown void",
     "true false null undefined NaN Infinity", "//", nullptr, "/*", "*/", false, false, false, true, false, true,
     true, false, {}, {}, {}},
    {"Python|GDScript",
     "and as assert async await break class continue def del elif else except finally for from global if import "
     "in is lambda nonlocal not or pass raise return try while with yield match case func extends var const "
     "signal onready export enum static",
     "int float str bytes bool list dict set tuple object type", "True False None self cls", "#", nullptr, nullptr,
     nullptr, false, false, true, false, false, true, true, false, {}, {}, {}},
    {"Ruby",
     "alias and begin break case class def defined? do else elsif end ensure for if in module next not or redo "
     "rescue retry return self super then undef unless until when while yield require attr_accessor",
     "", "true false nil", "#", nullptr, "=begin", "=end", false, false, false, false, false, true, false, true,
     {}, {}, {}},
    {"PHP",
     "abstract and as break callable case catch class clone const continue declare default do echo else elseif "
     "empty enddeclare endfor endforeach endif endswitch endwhile extends final finally fn for foreach function "
     "global goto if implements include include_once instanceof insteadof interface isset list match namespace "
     "new or print private protected public readonly require require_once return static switch throw trait try "
     "unset use var while xor yield",
     "array bool float int mixed object string void", "true false null", "//", "#", "/*", "*/", false, false,
     false, false, true, true, false, true, {}, {}, {}},
    {"Perl",
     "my our local sub if elsif else unless while until for foreach last next redo return use require package "
     "do eval print",
     "", "undef", "#", nullptr, nullptr, nullptr, false, false, false, false, false, false, false, true, {}, {}, {}},
    {"Lua",
     "and break do else elseif end for function goto if in local not or repeat return then until while", "",
     "true false nil", "--", nullptr, "--[[", "]]", false, false, false, false, false, false, false, false,
     {}, {}, {}},
    {"Shell|Fish|Dockerfile|Makefile|Meson",
     "if then else elif fi case esac for while until do done in function return local export readonly declare "
     "set unset source alias echo exit shift trap break continue end begin switch test FROM RUN CMD COPY ADD "
     "ENTRYPOINT ENV ARG WORKDIR EXPOSE USER VOLUME LABEL ifeq ifneq ifdef ifndef endif include project "
     "executable library dependency",
     "", "true false", "#", nullptr, nullptr, nullptr, false, false, false, true, false, false, false, true,
     {}, {}, {}},
    {"PowerShell",
     "begin break catch class continue data define do dynamicparam else elseif end enum exit filter finally for "
     "foreach from function if in param process return switch throw trap try until using var while",
     "", "$true $false $null", "#", nullptr, "<#", "#>", false, false, false, false, true, false, false, true,
     {}, {}, {}},
    {"Batch",
     "if else goto call set setlocal endlocal echo exit for in do not exist defined errorlevel shift pushd popd "
     "rem cd start", "", "", "::", nullptr, nullptr, nullptr, false, false, false, false, true, false, false, false,
     {}, {}, {}},
    {"SQL",
     "select from where insert into values update set delete create table drop alter add column index view join "
     "inner left right outer full on as and or not null is in between like order by group having limit offset "
     "union all distinct case when then else end primary key foreign references default unique check begin "
     "commit rollback transaction exists returning with",
     "int integer bigint smallint text varchar char boolean real double float numeric decimal date time "
     "timestamp blob serial",
     "true false", "--", nullptr, "/*", "*/", false, false, false, false, true, false, false, false, {}, {}, {}},
    {"CMake",
     "if elseif else endif foreach endforeach while endwhile function endfunction macro endmacro return set "
     "unset option project add_executable add_library target_link_libraries target_include_directories "
     "target_compile_options target_compile_definitions find_package include message cmake_minimum_required "
     "add_subdirectory install add_custom_command add_custom_target fetchcontent_declare "
     "fetchcontent_makeavailable list string file get_filename_component configure_file",
     "", "on off true false yes no", "#", nullptr, nullptr, nullptr, false, false, false, false, true, false,
     false, true, {}, {}, {}},
    {"CCL", "", "", "true false yes no on off", "/=", nullptr, nullptr, nullptr, false, false, false, false, false,
     false, false, false, {}, {}, {}},
    {"JSON", "", "", "true false null", "//", nullptr, "/*", "*/", false, false, false, false, false, false, false,
     false, {}, {}, {}},
    {"YAML|TOML|INI|Config|Git Config|EditorConfig", "", "", "true false null yes no on off", "#", ";", nullptr,
     nullptr, false, false, false, false, false, false, false, false, {}, {}, {}},
    {"GLSL|HLSL|WGSL|Metal",
     "attribute break case const continue default discard do else for if in inout layout out return struct "
     "switch uniform varying while precision highp mediump lowp buffer shared cbuffer register fn let var "
     "kernel vertex fragment device constant thread",
     "bool int uint float double void vec2 vec3 vec4 ivec2 ivec3 ivec4 uvec2 uvec3 uvec4 bvec2 bvec3 bvec4 mat2 "
     "mat3 mat4 sampler2D sampler3D samplerCube texture2D float2 float3 float4 float4x4 half f32 i32 u32 "
     "Texture2D SamplerState",
     "true false", "//", nullptr, "/*", "*/", true, false, false, false, false, false, true, false, {}, {}, {}},
    {"CSS|SCSS|Sass|Less",
     "import media keyframes font-face supports charset namespace page include mixin extend if else each for "
     "return function use forward",
     "", "important inherit initial unset none auto", "//", nullptr, "/*", "*/", false, false, false, false,
     false, false, true, true, {}, {}, {}},
    {"HTML|XML|SVG", "", "", "", nullptr, nullptr, "<!--", "-->", false, false, false, false, false, false, false,
     false, {}, {}, {}},
    {"Haskell|OCaml|F#|Elixir|Erlang|Clojure|R|Julia",
     "case class data default deriving do else if import in infix instance let module newtype of then type "
     "where match with fun function rec val open begin end def defmodule defp fn receive cond quote function "
     "return for while repeat next break struct mutable",
     "Int Integer Float Double String Bool Char", "True False true false nil NULL TRUE FALSE NA", "--", "#",
     "{-", "-}", false, false, false, false, false, true, false, false, {}, {}, {}},
    {"Assembly",
     "mov push pop call ret jmp je jne jz jnz jg jl add sub mul div inc dec cmp test and or xor not lea nop int "
     "section global extern db dw dd dq",
     "rax rbx rcx rdx rsi rdi rsp rbp eax ebx ecx edx esi edi esp ebp", "", ";", "#", nullptr, nullptr, false,
     true, false, false, true, false, false, false, {}, {}, {}},
};

void fillSet(std::unordered_set<std::string_view>& set, const char* words)
{
    std::string_view s = words ? words : "";
    while (!s.empty()) {
        const size_t space = std::min(s.find(' '), s.size());
        if (space > 0)
            set.insert(s.substr(0, space));
        s.remove_prefix(std::min(space + 1, s.size()));
    }
}

bool isIdentStart(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || static_cast<unsigned char>(c) >= 0x80;
}

bool isIdentChar(char c)
{
    return isIdentStart(c) || (c >= '0' && c <= '9');
}

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

bool startsWith(std::string_view s, size_t i, const char* prefix)
{
    return prefix && s.substr(i).starts_with(prefix);
}

// Index just past the closing `quote`, honoring backslash escapes, or npos.
size_t closeQuote(std::string_view s, size_t i, std::string_view quote, bool escapes = true)
{
    while (i < s.size()) {
        if (escapes && s[i] == '\\') {
            i += 2;
            continue;
        }
        if (s.substr(i).starts_with(quote))
            return i + quote.size();
        ++i;
    }
    return std::string_view::npos;
}

} // namespace

const Language* languageFor(std::string_view name)
{
    for (Language& lang : kLanguages) {
        std::string_view names = lang.names;
        while (!names.empty()) {
            const size_t bar = std::min(names.find('|'), names.size());
            if (names.substr(0, bar) == name) {
                if (lang.keywordSet.empty() && lang.typeSet.empty() && lang.literalSet.empty()) {
                    fillSet(lang.keywordSet, lang.keywords);
                    fillSet(lang.typeSet, lang.types);
                    fillSet(lang.literalSet, lang.literals);
                }
                return &lang;
            }
            names.remove_prefix(std::min(bar + 1, names.size()));
        }
    }
    return nullptr;
}

State scanLine(const Language& lang, std::string_view s, State state, std::vector<Span>* out)
{
    const auto emit = [&](size_t start, size_t end, Token token) {
        if (out && end > start)
            out->push_back({static_cast<uint32_t>(start), static_cast<uint32_t>(end), token});
    };
    const size_t n = s.size();
    size_t i = 0;

    // Finish whatever the previous line left open.
    if (state != kNormal) {
        size_t end = std::string_view::npos;
        Token token = Token::String;
        if (state == kBlockComment) {
            const size_t at = lang.blockClose ? s.find(lang.blockClose) : std::string_view::npos;
            end = at == std::string_view::npos ? at : at + std::char_traits<char>::length(lang.blockClose);
            token = Token::Comment;
        } else if (state == kTripleDouble) {
            end = closeQuote(s, 0, "\"\"\"");
        } else if (state == kTripleSingle) {
            end = closeQuote(s, 0, "'''");
        } else {
            end = closeQuote(s, 0, "`");
        }
        if (end == std::string_view::npos) {
            emit(0, n, token);
            return state;
        }
        emit(0, end, token);
        i = end;
    }

    // C-style preprocessor line: "#include", "#  define", ...
    bool include = false;
    if (lang.hashDirectives) {
        size_t j = i;
        while (j < n && (s[j] == ' ' || s[j] == '\t'))
            ++j;
        if (j < n && s[j] == '#' && i == 0) {
            size_t k = j + 1;
            while (k < n && (s[k] == ' ' || s[k] == '\t'))
                ++k;
            const size_t wordStart = k;
            while (k < n && isIdentChar(s[k]))
                ++k;
            include = s.substr(wordStart, k - wordStart) == "include" || s.substr(wordStart, k - wordStart) == "import";
            emit(j, k, Token::Preprocessor);
            i = k;
        }
    }

    std::string lower; // for case-insensitive languages
    while (i < n) {
        const char c = s[i];
        if (c == ' ' || c == '\t') {
            ++i;
            continue;
        }

        if (startsWith(s, i, lang.lineComment) || startsWith(s, i, lang.lineComment2)) {
            emit(i, n, Token::Comment);
            return kNormal;
        }
        if (startsWith(s, i, lang.blockOpen)) {
            const size_t open = std::char_traits<char>::length(lang.blockOpen);
            const size_t at = s.find(lang.blockClose, i + open);
            if (at == std::string_view::npos) {
                emit(i, n, Token::Comment);
                return kBlockComment;
            }
            const size_t end = at + std::char_traits<char>::length(lang.blockClose);
            emit(i, end, Token::Comment);
            i = end;
            continue;
        }

        if (lang.tripleQuotes && (startsWith(s, i, "\"\"\"") || startsWith(s, i, "'''"))) {
            const bool dbl = c == '"';
            const size_t end = closeQuote(s, i + 3, dbl ? "\"\"\"" : "'''");
            if (end == std::string_view::npos) {
                emit(i, n, Token::String);
                return dbl ? kTripleDouble : kTripleSingle;
            }
            emit(i, end, Token::String);
            i = end;
            continue;
        }
        if (lang.backticks && c == '`') {
            const size_t end = closeQuote(s, i + 1, "`");
            if (end == std::string_view::npos) {
                emit(i, n, Token::String);
                return kBacktick;
            }
            emit(i, end, Token::String);
            i = end;
            continue;
        }

        const bool charLiteral = c == '\'' && lang.charLiterals
                              && ((i + 1 < n && s[i + 1] == '\\') || (i + 2 < n && s[i + 2] == '\''));
        if (c == '"' || (c == '\'' && (!lang.charLiterals || charLiteral)) || (include && c == '<')) {
            const std::string_view quote = c == '<' ? ">" : s.substr(i, 1);
            size_t end = closeQuote(s, i + 1, quote, c != '<');
            if (end == std::string_view::npos)
                end = n; // unterminated: color to the end of the line
            emit(i, end, Token::String);
            i = end;
            continue;
        }

        if (isDigit(c) || (c == '.' && i + 1 < n && isDigit(s[i + 1]))) {
            size_t end = i + 1;
            while (end < n && (isIdentChar(s[end]) || s[end] == '.' || (s[end] == '\'' && lang.charLiterals)))
                ++end;
            emit(i, end, Token::Number);
            i = end;
            continue;
        }

        if ((lang.annotations && c == '@') || (lang.variables && c == '$')) {
            size_t end = i + 1;
            if (c == '$' && end < n && s[end] == '{') {
                const size_t close = s.find('}', end);
                end = close == std::string_view::npos ? n : close + 1;
            } else {
                while (end < n && (isIdentChar(s[end]) || s[end] == '.'))
                    ++end;
            }
            if (end > i + 1) {
                emit(i, end, c == '@' ? Token::Preprocessor : Token::Type);
                i = end;
                continue;
            }
        }

        if (isIdentStart(c)) {
            size_t end = i + 1;
            while (end < n && (isIdentChar(s[end]) || (s[end] == '-' && lang.caseInsensitive && lang.variables)))
                ++end;
            std::string_view word = s.substr(i, end - i);
            if (lang.caseInsensitive) {
                lower.assign(word);
                for (char& ch : lower)
                    ch = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
                word = lower;
            }
            Token token = Token::Text;
            if (lang.keywordSet.count(word))
                token = Token::Keyword;
            else if (lang.typeSet.count(word))
                token = Token::Type;
            else if (lang.literalSet.count(word))
                token = Token::Literal;
            else if (lang.capitalizedTypes && s[i] >= 'A' && s[i] <= 'Z')
                token = Token::Type;
            else {
                size_t k = end;
                while (k < n && (s[k] == ' ' || s[k] == '\t'))
                    ++k;
                if (k < n && s[k] == '(')
                    token = Token::Function;
            }
            if (token != Token::Text)
                emit(i, end, token);
            i = end;
            continue;
        }

        ++i; // operators and punctuation stay plain
    }
    return kNormal;
}

} // namespace syntax

// ---------------------------------------------------------------------------
// Highlighter
// ---------------------------------------------------------------------------

Highlighter::Highlighter(const TextBuffer& buffer)
    : buffer_(buffer)
{
}

void Highlighter::setLanguage(const syntax::Language* language)
{
    language_ = language;
    reset();
}

void Highlighter::reset()
{
    states_.assign(language_ ? buffer_.lineCount() : 0, syntax::State{0});
    valid_ = 1;
}

void Highlighter::linesChanged(size_t first, size_t oldCount, size_t newCount)
{
    if (!language_)
        return;
    if (states_.size() != buffer_.lineCount() + oldCount - newCount) {
        reset(); // out of step (the buffer changed behind our back)
        return;
    }
    // The state at the start of `first` still holds; everything after may not.
    const auto begin = states_.begin() + static_cast<std::ptrdiff_t>(first + 1);
    if (newCount > oldCount)
        states_.insert(begin, newCount - oldCount, syntax::State{0});
    else if (oldCount > newCount)
        states_.erase(begin, begin + static_cast<std::ptrdiff_t>(oldCount - newCount));
    valid_ = std::min(valid_, first + 1);
}

syntax::State Highlighter::stateAt(size_t line)
{
    if (states_.size() != buffer_.lineCount())
        reset();
    while (valid_ <= line) {
        const std::string_view previous = buffer_.line(valid_ - 1, scratch_);
        states_[valid_] = syntax::scanLine(*language_, previous, states_[valid_ - 1], nullptr);
        ++valid_;
    }
    return states_[line];
}

void Highlighter::highlight(size_t line, std::string_view text, std::vector<syntax::Span>& out)
{
    out.clear();
    if (!language_)
        return;
    syntax::scanLine(*language_, text, stateAt(line), &out);
}
