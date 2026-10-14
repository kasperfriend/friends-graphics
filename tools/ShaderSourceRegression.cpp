// Portable shader-source guard. The HLSL in this tree is compiled at runtime,
// and the only thing that compiles it before a player does is fxc, on the
// Windows runner (tools/ValidateShaders.ps1). That left shader text as the one
// kind of change this repository could not check on Linux - and a GLSL call
// (`mix`, which HLSL spells `lerp`) reached main() in WaterEffect.h's pixel
// shader and failed the first Windows run of the water rework with
// `error X3004: undeclared identifier`.
//
// Everything checked here is text, so it belongs in the portable suite, where
// the shader can be read without a device or a compiler:
//
//   * a GLSL spelling of an HLSL intrinsic, with the spelling that works;
//   * a GLSL type name (vec2, mat3, ...);
//   * a GLSL-only construct (#version, precision, varying, layout, gl_*).
//
// It walks the tree for raw HLSL literals (the marker is R"HLSL followed by an
// open parenthesis) instead of reading a hard-coded list, so a shader is
// covered the moment it is written. Shaders assembled from ordinary string
// literals (`ss << "..."`) cannot be seen this way and remain fxc's business.
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{

void Require(bool value, const std::string& name)
{
    if (!value) throw std::runtime_error(name);
    std::printf("PASS %s\n", name.c_str());
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The test is run from the repository root by CI and from wherever a developer
// happens to be, so the root is the directory that carries the solution.
std::filesystem::path FindRoot()
{
    const std::vector<std::filesystem::path> candidates = {".", "..", "../.."};
    for (const std::filesystem::path& candidate : candidates)
    {
        std::error_code error;
        if (std::filesystem::exists(candidate / "ModernWoWRenderer.sln", error)) return candidate;
    }
    throw std::runtime_error("cannot locate the repository root (looked for ModernWoWRenderer.sln in ., .. and ../..)");
}

bool IsIdentifierChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// Comments are not shader text: several of these files discuss GLSL and the
// terms it replaced, and a comment must never fail a build.
std::string StripComments(const std::string& source)
{
    std::string out;
    out.reserve(source.size());
    size_t at = 0;
    while (at < source.size())
    {
        if (source[at] == '/' && at + 1 < source.size() && source[at + 1] == '/')
        {
            while (at < source.size() && source[at] != '\n') ++at;
        }
        else if (source[at] == '/' && at + 1 < source.size() && source[at + 1] == '*')
        {
            at += 2;
            while (at + 1 < source.size() && !(source[at] == '*' && source[at + 1] == '/')) ++at;
            at = at + 1 < source.size() ? at + 2 : source.size();
        }
        else
        {
            out.push_back(source[at]);
            ++at;
        }
    }
    return out;
}

size_t LineOf(const std::string& text, size_t offset)
{
    size_t line = 1;
    for (size_t i = 0; i < offset && i < text.size(); ++i)
        if (text[i] == '\n') ++line;
    return line;
}

// A function call: the name, then optional spaces, then '('. Written as a
// lookup rather than a word scan so `mixBlend` or a sampler named `texture2D`
// cannot trip it.
const char* HlslSpelling(const std::string& name)
{
    if (name == "mix") return "lerp";
    if (name == "fract") return "frac";
    if (name == "mod") return "fmod";
    if (name == "inversesqrt") return "rsqrt";
    if (name == "texture2D") return "tex2D";
    if (name == "texture3D") return "tex3D";
    if (name == "textureCube") return "texCUBE";
    if (name == "textureLod") return "tex2Dlod (or tex2Dlod/tex3Dlod for that sampler)";
    if (name == "texture2DProj") return "tex2Dproj";
    return nullptr;
}

const char* HlslTypeFor(const std::string& name)
{
    if (name == "vec2" || name == "vec3" || name == "vec4") return "float2/float3/float4";
    if (name == "ivec2" || name == "ivec3" || name == "ivec4") return "int2/int3/int4";
    if (name == "bvec2" || name == "bvec3" || name == "bvec4") return "bool2/bool3/bool4";
    if (name == "mat2" || name == "mat3" || name == "mat4") return "float2x2/float3x3/float4x4";
    return nullptr;
}

struct Literal
{
    std::string file;
    std::string name;
    std::string body;
};

// Every `R"HLSL(` ... `)HLSL"` in one file, with the declared variable name
// when there is one. A shader whose declaration is spelled differently is
// still scanned; it just reports as `?`.
std::vector<Literal> ExtractLiterals(const std::string& file, const std::string& source)
{
    std::vector<Literal> literals;
    const std::string open = "R\"HLSL(";
    const std::string close = ")HLSL\"";
    for (size_t at = source.find(open); at != std::string::npos; at = source.find(open, at))
    {
        const size_t bodyStart = at + open.size();
        const size_t bodyEnd = source.find(close, bodyStart);
        if (bodyEnd == std::string::npos) break;   // unterminated: fxc's problem, not this test's

        std::string name = "?";
        const size_t newline = source.rfind('\n', at);
        const size_t lineStart = newline == std::string::npos ? 0 : newline + 1;
        for (size_t i = at; i > lineStart;)
        {
            --i;
            if (IsIdentifierChar(source[i]))
            {
                size_t begin = i;
                while (begin > lineStart && IsIdentifierChar(source[begin - 1])) --begin;
                name = source.substr(begin, i - begin + 1);
                break;
            }
        }
        literals.push_back({file, name, source.substr(bodyStart, bodyEnd - bodyStart)});
        at = bodyEnd + close.size();
    }
    return literals;
}

bool NextNonSpaceIsParen(const std::string& text, size_t after)
{
    for (size_t i = after; i < text.size(); ++i)
    {
        if (text[i] == ' ' || text[i] == '\t') continue;
        return text[i] == '(';
    }
    return false;
}

// Identifier occurrences in comment-free text, with the preceding character
// checked so `premix(` and `svec3` are not reported as GLSL.
std::vector<std::pair<size_t, std::string>> Identifiers(const std::string& text)
{
    std::vector<std::pair<size_t, std::string>> found;
    for (size_t at = 0; at < text.size();)
    {
        if (!IsIdentifierChar(text[at]) || std::isdigit(static_cast<unsigned char>(text[at])))
        {
            ++at;
            continue;
        }
        size_t end = at;
        while (end < text.size() && IsIdentifierChar(text[end])) ++end;
        const bool whole = at == 0 || !IsIdentifierChar(text[at - 1]);
        if (whole) found.emplace_back(at, text.substr(at, end - at));
        at = end;
    }
    return found;
}

std::string Scan(const Literal& literal)
{
    const std::string code = StripComments(literal.body);
    std::vector<std::string> problems;

    for (const auto& [offset, name] : Identifiers(code))
    {
        const size_t line = LineOf(code, offset);
        if (const char* spelling = HlslSpelling(name))
            if (NextNonSpaceIsParen(code, offset + name.size()))
                problems.push_back("line " + std::to_string(line) + ": `" + name + "(` is GLSL, HLSL spells it `" + spelling + "`");
        if (const char* type = HlslTypeFor(name))
            problems.push_back("line " + std::to_string(line) + ": `" + name + "` is a GLSL type, HLSL spells it `" + type + "`");
        if (name == "varying" || name == "attribute")
            problems.push_back("line " + std::to_string(line) + ": `" + name + "` is a GLSL stage qualifier, HLSL uses semantics");
        if (name == "highp" || name == "mediump" || name == "lowp")
            problems.push_back("line " + std::to_string(line) + ": `" + name + "` is a GLSL precision qualifier");
        if (name.rfind("gl_", 0) == 0)
            problems.push_back("line " + std::to_string(line) + ": `" + name + "` is a GLSL built-in");
    }
    if (code.find("#version") != std::string::npos)
        problems.push_back("line " + std::to_string(LineOf(code, code.find("#version"))) + ": `#version` is GLSL");

    std::string joined;
    for (const std::string& problem : problems)
        joined += (joined.empty() ? "" : "; ") + problem;
    return joined;
}

}   // namespace

int main() try
{
    const std::filesystem::path root = FindRoot();

    std::vector<Literal> literals;
    std::vector<std::string> files;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(root, error), end; it != end; it.increment(error))
    {
        const std::filesystem::path& path = it->path();
        const std::string extension = path.extension().string();
        if (extension != ".h" && extension != ".cpp") continue;
        // This test spells the marker out in ordinary C++ strings; they are not
        // shader sources, and the tool must not report on itself.
        if (path.filename() == "ShaderSourceRegression.cpp") continue;
        const std::string generic = path.generic_string();
        if (generic.find("build/") != std::string::npos || generic.find(".git/") != std::string::npos) continue;
        const std::string source = ReadFile(path);
        if (source.find("R\"HLSL(") == std::string::npos) continue;
        const std::string relative = path.lexically_relative(root).generic_string();
        files.push_back(relative);
        for (Literal& literal : ExtractLiterals(relative, source)) literals.push_back(std::move(literal));
    }

    // A scan that stops finding shaders would pass for the wrong reason, so the
    // inventory is asserted, not assumed. The floors are today's inventory with
    // room to spare: they fire when extraction breaks, not when a file is added.
    Require(files.size() >= 6, "every file that embeds HLSL was found (" + std::to_string(files.size()) + ")");
    Require(literals.size() >= 25, "every embedded HLSL literal was extracted (" + std::to_string(literals.size()) + ")");

    for (const Literal& literal : literals)
    {
        const std::string problems = Scan(literal);
        const std::string label = literal.file + " (" + literal.name + ")";
        Require(problems.empty(), problems.empty() ? label + " uses HLSL spellings only" : label + ": " + problems);
    }
    if (literals.empty()) throw std::runtime_error("no HLSL literals were scanned");

    std::printf("Shader source: %zu literals in %zu files carry HLSL spellings only\n",
                literals.size(), files.size());
    return 0;
}
catch (const std::exception& e)
{
    std::fprintf(stderr, "FAIL %s\n", e.what());
    return 1;
}
