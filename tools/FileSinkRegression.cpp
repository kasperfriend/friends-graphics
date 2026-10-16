// The shared diagnostic file sink: what it buffers, when it opens a file, and
// what rotation does to the file a supporter is told to look for.
//
// Every diagnostic this renderer writes used to be its own `std::ofstream(path,
// app)` on the thread that draws - an open, a write and a close of a file in the
// GAME FOLDER per line. A crossing wrote six of them on one frame. The sink turns
// that into one open per file per flush, and Present flushes, so the lines still
// land in the frame that produced them.
//
// These checks pin the parts a reader of the logs depends on and that are easy to
// break while moving writes around: nothing is written until something flushes,
// one flush writes every line in order, a burst cannot be held past the frame that
// produced it, and rotation keeps the README's promise about which half survives
// and what the rotated file is called.
#include "../src/Diagnostics/FileSink.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace sink = renderer::filesink;

static int failures = 0;

#define Require(cond, message) \
    do { \
        if (cond) { \
            std::printf("PASS %s\n", message); \
        } else { \
            std::printf("FAIL %s\n", message); \
            ++failures; \
        } \
    } while (0)

static fs::path MakeSandbox()
{
    const fs::path dir = fs::temp_directory_path() / "filesink_regression";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

static bool Exists(const fs::path& path)
{
    std::error_code ec;
    return fs::exists(path, ec);
}

static std::string Read(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string bytes;
    char buffer[8192];
    while (in.read(buffer, sizeof buffer) || in.gcount())
        bytes.append(buffer, static_cast<size_t>(in.gcount()));
    return bytes;
}

static size_t Size(const fs::path& path)
{
    std::error_code ec;
    const auto bytes = fs::file_size(path, ec);
    return ec ? 0 : static_cast<size_t>(bytes);
}

static std::string ReadFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path.generic_string());
    std::string text;
    char buffer[8192];
    while (in.read(buffer, sizeof buffer) || in.gcount())
        text.append(buffer, static_cast<size_t>(in.gcount()));
    return text;
}

// The test is run from the repository root by CI and from wherever a developer
// happens to be, so the root is the directory that carries the solution.
static fs::path FindRoot()
{
    std::error_code error;
    for (const fs::path& candidate : {fs::path("."), fs::path(".."), fs::path("../..")})
        if (fs::exists(candidate / "ModernWoWRenderer.sln", error)) return candidate;
    throw std::runtime_error("cannot locate the repository root (looked for ModernWoWRenderer.sln in ., .. and ../..)");
}

// The two ways a buffered report can silently stop being what a supporter is
// sent: the module stops offering a way to complete the file, or a harness reads
// the file back without asking. Neither is a compile error and neither shows up
// in the log - the log is simply missing the lines that were still buffered when
// somebody looked. So the contract is checked as text, the way the shader-source
// and wind suites check theirs.
static void CheckReaderContract(const fs::path& root)
{
    const std::string report = ReadFile(root / "src/Environment/MenuDiagnostics.h");
    Require(report.find("inline void Flush()") != std::string::npos,
            "the menu report offers a flush, so a reader outside the frame loop can complete the file");

    std::error_code error;
    for (fs::directory_iterator it(root / "tools", error), end; it != end; it.increment(error)) {
        const fs::path& path = it->path();
        if (path.extension() != ".cpp") continue;
        if (path.filename() == "FileSinkRegression.cpp") continue;
        const std::string source = ReadFile(path);
        // Only a harness that reads the report back can be wrong about it.
        if (source.find("MenuDiagnostics.log") == std::string::npos) continue;
        if (source.find("std::ifstream") == std::string::npos) continue;
        const bool asks = source.find("menudiagnostics::Flush()") != std::string::npos ||
                          source.find("filesink::Flush") != std::string::npos;
        Require(asks, ("a harness that reads the report back asks for the flush first: " +
                       path.filename().string()).c_str());
    }
}

int main()
{
    const fs::path dir = MakeSandbox();
    CheckReaderContract(FindRoot());
    // The sink's state is process-wide and keyed by path, so every check gets its
    // own file: one check's rotation limit or pending bytes cannot leak into the
    // next one's.
    const fs::path buffered = dir / "buffered.log";
    const fs::path verbatim = dir / "verbatim.log";
    const fs::path burst = dir / "burst.log";
    const fs::path held = dir / "held.log";
    const fs::path written = dir / "written.log";
    const fs::path rotated = dir / "rotated.log";
    const fs::path previous = dir / "previous.log";
    const fs::path small = dir / "small.log";

    // ---- nothing touches the disk until something flushes ----
    sink::AppendLine(buffered.wstring(), "first");
    sink::AppendLine(buffered.wstring(), "second");
    sink::AppendLine(buffered.wstring(), "third");
    Require(!Exists(buffered), "buffered lines do not open a file until a flush");
    Require(sink::PendingBytes() == std::string("first\nsecond\nthird\n").size(),
            "what is buffered is counted, so a report can say a line is unwritten rather than missing");

    // ---- one flush writes every line, in order, in one open ----
    sink::Flush();
    Require(Exists(buffered), "a flush writes the file");
    Require(Read(buffered) == "first\nsecond\nthird\n", "one flush writes every buffered line in order");
    Require(sink::PendingBytes() == 0, "nothing is left buffered after a flush");

    // A second flush with nothing waiting touches no file at all - that is what
    // makes a quiet frame free.
    const size_t afterFirst = Size(buffered);
    sink::Flush();
    Require(Size(buffered) == afterFirst, "a flush with nothing waiting changes nothing");

    // ---- Append is verbatim: a converted site writes the bytes it wrote before ----
    sink::Append(verbatim.wstring(), "keeps its own line ending\n");
    sink::Append(verbatim.wstring(), "and no extra one");
    sink::Flush();
    Require(Read(verbatim) == "keeps its own line ending\nand no extra one",
            "Append writes a caller's bytes verbatim, newline and all");

    // ---- a burst cannot be held past the frame that produced it ----
    // The whole point of buffering is that it costs nothing on a quiet frame. The
    // whole risk of buffering is a crash taking lines with it, so past a few
    // kilobytes the sink writes on the spot, with no flush asked for.
    for (int i = 0; i < 400; ++i)
        sink::AppendLine(burst.wstring(), "a line long enough that a few hundred of them pass the burst limit");
    Require(Exists(burst), "a burst past the limit goes out on the spot");
    Require(Size(burst) >= sink::kBurstBytes, "the burst that went out is at least the burst limit");
    sink::Flush();
    Require(Size(burst) > sink::kBurstBytes, "the tail of a burst goes out on the next flush");

    // ---- FlushFile writes one file and leaves the rest buffered ----
    sink::AppendLine(held.wstring(), "still waiting");
    sink::AppendLine(written.wstring(), "out now");
    sink::FlushFile(written.wstring());
    Require(Read(written) == "out now\n", "FlushFile writes the file it is asked about");
    Require(!Exists(held), "FlushFile leaves every other file buffered");
    sink::Flush();
    Require(Read(held) == "still waiting\n", "and a later flush writes it");

    // ---- rotation keeps the recent half, under the name a reader is told ----
    // The limit is deliberately tiny here; in the renderer it is two megabytes for
    // the main log and one for the menu report. What matters is which half
    // survives: the recent lines, because that is the part anyone reading a log
    // after a crash is looking for.
    //
    // The file is put over its limit before the sink has ever looked at it, because
    // the sink re-measures a file at most once a second - a stat per line was what
    // made rotation part of the cost being removed here. In the renderer that means
    // a log can sit past its limit for up to a second before it turns over, which
    // is nothing against a two-megabyte cap, and it means this check has to be
    // written against the first look rather than a second flush in the same second.
    {
        std::ofstream seed(rotated, std::ios::binary | std::ios::trunc);
        seed << "old content that should end up in the rotated file\n";
    }
    sink::SetRotationLimit(rotated.wstring(), 32);
    Require(Size(rotated) > 32, "the file starts out past its rotation limit");
    sink::AppendLine(rotated.wstring(), "new content");
    sink::Flush();
    Require(Exists(dir / "rotated.log.1"), "a file past its limit rotates to <name>.1 by default");
    Require(Read(dir / "rotated.log.1").find("old content") != std::string::npos,
            "the rotated half carries the content that was already there");
    Require(Read(rotated) == "new content\n", "the live file starts fresh after a rotation");

    // ---- the suffix belongs to the file ----
    // README tells a supporter that MenuDiagnostics.log rotates to
    // MenuDiagnostics.log.previous. That name is load-bearing, so it is a per-file
    // setting rather than the sink's default.
    {
        std::ofstream seed(previous, std::ios::binary | std::ios::trunc);
        seed << "old report lines that are already past the limit\n";
    }
    sink::SetRotationLimit(previous.wstring(), 16);
    sink::SetRotationSuffix(previous.wstring(), L".previous");
    sink::AppendLine(previous.wstring(), "new report");
    sink::Flush();
    Require(Exists(dir / "previous.log.previous"), "a file rotates to the suffix it was given");
    Require(!Exists(dir / "previous.log.1"), "and not to the default one");
    Require(Read(previous) == "new report\n", "and the live report file starts fresh");

    // ---- a file nowhere near its limit is left alone ----
    sink::AppendLine(small.wstring(), "one line");
    sink::Flush();
    sink::AppendLine(small.wstring(), "another line");
    sink::Flush();
    Require(!Exists(dir / "small.log.1"), "a file under its limit is never rotated");
    Require(Read(small) == "one line\nanother line\n", "and keeps appending");

    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL FILE SINK CHECKS PASSED\n", failures);
    return failures ? 1 : 0;
}
