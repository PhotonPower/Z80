// z80tool - command line front end for the Z80 assembler and disassembler.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define Z80_ISATTY(fd) _isatty(fd)
#else
#include <unistd.h>
#define Z80_ISATTY(fd) isatty(fd)
#endif

#include "z80/assembler.hpp"
#include "z80/disassembler.hpp"
#include "z80/expression.hpp"
#include "z80/image_io.hpp"
#include "z80/monitor.hpp"
#include "z80/session.hpp"
#include "z80/version.hpp"

namespace fs = std::filesystem;

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;  // assembly errors
constexpr int kExitUsage = 2;   // bad command line / I/O problems

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void printUsage(std::ostream& out) {
    out << "z80tool " << z80::kVersion << " - Z80 assembler and disassembler\n"
        << "\n"
        << "Usage:\n"
        << "  z80tool asm <source.asm> [options]\n"
        << "  z80tool dis <image>      [options]\n"
        << "  z80tool monitor [image] [--org <hex>] [--no-files]   interactive machine-code monitor\n"
        << "  z80tool serve [--allow-files]   emulator server: JSON requests on stdin, replies on stdout\n"
        << "  z80tool help | version\n"
        << "\n"
        << "asm options:\n"
        << "  -o <file>        write the assembled code as a raw binary\n"
        << "  --hex <file>     write Intel HEX\n"
        << "  --sna <file>     patch the code into the RAM image of a CPC snapshot (in place)\n"
        << "  -l <file|->      write a listing ('-' = standard output)\n"
        << "  -s <file|->      write the symbol table\n"
        << "  --org <addr>     start address before the first ORG (default 0)\n"
        << "  (without -o/--hex/--sna the result is written to <source>.bin)\n"
        << "\n"
        << "dis options:\n"
        << "  --format <auto|sna|bin>  input format (default auto: .sna by its header, else raw)\n"
        << "  --org <addr>     load address of a raw binary (default 0)\n"
        << "  -s <addr>        start address / entry point (default: load address)\n"
        << "  -e <addr>        end address: linear sweep from -s to -e.\n"
        << "                   Without -e the code flow is followed from -s (CALL/JP/JR targets)\n"
        << "  -c <file>        comment file with lines 'address,\"text\"'\n"
        << "  --entry <addr>   additional entry point (repeatable)\n"
        << "  --inline-string <addr>   CALL <addr> is followed by a string ending with a bit-7 byte\n"
        << "  --inline-zstring <addr>  CALL <addr> is followed by a zero-terminated string\n"
        << "  --asm            write re-assemblable source (ORG, labels, DB) instead of a listing\n"
        << "  --style <amp|dollar|c|h>  number style (default amp: &1F)\n"
        << "  --from <addr> --to <addr> restrict the printed range\n"
        << "  --no-bytes       omit the raw bytes column      --no-xrefs  omit caller lists\n"
        << "  -o <file>        write the output to a file instead of standard output\n"
        << "\n"
        << "Numbers: 123  &1F  $1F  0x1F  1Fh  %1010\n";
}

/// Minimal option reader: options may take one value.
class Args {
public:
    Args(int argc, char** argv, int first) {
        for (int i = first; i < argc; ++i) items_.emplace_back(argv[i]);
    }
    [[nodiscard]] bool done() const { return pos_ >= items_.size(); }
    std::string next() { return items_[pos_++]; }
    std::string value(const std::string& option) {
        if (done()) throw UsageError("option " + option + " needs a value");
        return items_[pos_++];
    }

private:
    std::vector<std::string> items_;
    std::size_t pos_ = 0;
};

z80::Word parseAddress(const std::string& text, const std::string& what) {
    const auto v = z80::parseConstant(text);
    if (!v || *v < 0 || *v > 0xFFFF) {
        throw UsageError("invalid " + what + ": '" + text + "'");
    }
    return static_cast<z80::Word>(*v);
}

std::string readText(const fs::path& path) {
    const auto bytes = z80::readFile(path);
    return std::string(bytes.begin(), bytes.end());
}

void writeText(const std::string& target, const std::string& text) {
    if (target == "-") {
        std::cout << text;
        return;
    }
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot open '" + target + "' for writing");
    out << text;
}

// ---- asm -------------------------------------------------------------------------------

int runAsm(Args args) {
    std::string source, binOut, hexOut, snaFile, listing, symbols;
    z80::AssemblerOptions options;

    while (!args.done()) {
        const std::string a = args.next();
        if (a == "-o") binOut = args.value(a);
        else if (a == "--hex") hexOut = args.value(a);
        else if (a == "--sna") snaFile = args.value(a);
        else if (a == "-l") listing = args.value(a);
        else if (a == "-s") symbols = args.value(a);
        else if (a == "--org") options.origin = parseAddress(args.value(a), "origin");
        else if (!a.empty() && a[0] == '-') throw UsageError("unknown option " + a);
        else if (source.empty()) source = a;
        else throw UsageError("more than one source file given");
    }
    if (source.empty()) throw UsageError("no source file given");
    if (binOut.empty() && hexOut.empty() && snaFile.empty()) {
        binOut = fs::path(source).replace_extension(".bin").string();
    }

    const z80::AssemblyResult result = z80::Assembler(options).assemble(readText(source));
    std::cerr << result.formatDiagnostics(source);
    if (!listing.empty()) writeText(listing, result.formatListing());
    if (!symbols.empty()) writeText(symbols, result.formatSymbols());

    std::cout << source << ": ";
    if (result.empty()) {
        std::cout << "no code generated";
    } else {
        std::cout << result.size() << " bytes, &" << z80::hexDigits(result.lowAddress(), 4) << " - &"
                  << z80::hexDigits(result.highAddress(), 4);
    }
    std::cout << ", " << result.errorCount() << " error(s)\n";

    if (!result.ok()) return kExitFailed;

    if (!binOut.empty()) z80::writeFile(binOut, result.flatten());
    if (!hexOut.empty()) writeText(hexOut, z80::toIntelHex(result.segments));
    if (!snaFile.empty()) {
        auto file = z80::readFile(snaFile);
        z80::patchSna(file, result.segments);
        z80::writeFile(snaFile, file);
    }
    return kExitOk;
}

// ---- dis -------------------------------------------------------------------------------

int runDis(Args args) {
    std::string input, format = "auto", commentFile, output;
    std::optional<z80::Word> start, end, renderFrom, renderTo;
    z80::Word org = 0;
    std::vector<z80::Word> entries;
    std::vector<std::pair<z80::Word, z80::InlineDataKind>> inlineData;
    z80::DisassemblerOptions options;
    z80::RenderOptions render;

    while (!args.done()) {
        const std::string a = args.next();
        if (a == "--format") format = args.value(a);
        else if (a == "--org") org = parseAddress(args.value(a), "load address");
        else if (a == "-s") start = parseAddress(args.value(a), "start address");
        else if (a == "-e") end = parseAddress(args.value(a), "end address");
        else if (a == "-c") commentFile = args.value(a);
        else if (a == "--entry") entries.push_back(parseAddress(args.value(a), "entry point"));
        else if (a == "--inline-string")
            inlineData.emplace_back(parseAddress(args.value(a), "address"), z80::InlineDataKind::StringBit7);
        else if (a == "--inline-zstring")
            inlineData.emplace_back(parseAddress(args.value(a), "address"), z80::InlineDataKind::StringZero);
        else if (a == "--asm") render.asmSource = true;
        else if (a == "--style") {
            if (!z80::parseNumberStyle(args.value(a), options.style)) throw UsageError("unknown number style");
        }
        else if (a == "--from") renderFrom = parseAddress(args.value(a), "address");
        else if (a == "--to") renderTo = parseAddress(args.value(a), "address");
        else if (a == "--no-bytes") render.showBytes = false;
        else if (a == "--no-xrefs") render.showXrefs = false;
        else if (a == "-o") output = args.value(a);
        else if (!a.empty() && a[0] == '-') throw UsageError("unknown option " + a);
        else if (input.empty()) input = a;
        else throw UsageError("more than one input file given");
    }
    if (input.empty()) throw UsageError("no input file given");
    if (format != "auto" && format != "sna" && format != "bin") throw UsageError("unknown format '" + format + "'");

    const auto file = z80::readFile(input);
    const bool sna = format == "sna" || (format == "auto" && z80::looksLikeSna(file));
    const z80::Memory memory = sna ? z80::loadSna(file) : z80::loadRaw(file, org);
    const z80::Word first = start.value_or(sna ? z80::Word{0} : org);

    z80::Disassembler dis(memory, options);
    if (!sna && !file.empty()) {
        // A raw binary only defines the bytes it contains; do not "disassemble" the empty rest.
        const std::size_t last = std::min<std::size_t>(0xFFFF, static_cast<std::size_t>(org) + file.size() - 1);
        dis.setAnalysisRange(org, static_cast<z80::Word>(last));
    }
    for (auto [address, kind] : inlineData) dis.addInlineData(address, kind);
    for (auto e : entries) dis.addEntryPoint(e);
    if (!commentFile.empty()) {
        std::istringstream in(readText(commentFile));
        for (const auto& w : dis.loadComments(in)) std::cerr << "warning: " << w << "\n";
    }

    if (end) {
        if (*end < first) throw UsageError("end address is below the start address");
        dis.analyzeRange(first, *end);
    } else {
        dis.analyzeFlow(first);
    }

    render.from = renderFrom;
    render.to = renderTo;
    if (!sna && !file.empty()) {
        // For a raw binary print the whole image by default, including data behind the code.
        const std::size_t last = std::min<std::size_t>(0xFFFF, static_cast<std::size_t>(org) + file.size() - 1);
        if (!render.from) render.from = org;
        if (!render.to) render.to = static_cast<z80::Word>(last);
    }
    const std::string text = dis.render(render);
    if (output.empty()) std::cout << text;
    else writeText(output, text);
    return kExitOk;
}

// ---- serve -----------------------------------------------------------------------------

int runServe(Args args) {
    z80::SessionOptions options;
    while (!args.done()) {
        const std::string a = args.next();
        if (a == "--allow-files") options.allowFiles = true;
        else throw UsageError("unknown option " + a);
    }
    z80::Session session(options);
    std::ios::sync_with_stdio(false);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        std::cout << session.handleLine(line) << '\n' << std::flush;
    }
    return kExitOk;
}

// ---- monitor ---------------------------------------------------------------------------

int runMonitor(Args args) {
    z80::MonitorOptions options;
    std::string image;
    z80::Word org = 0;
    while (!args.done()) {
        const std::string a = args.next();
        if (a == "--no-files") options.allowFiles = false;
        else if (a == "--org") {  // monitor convention: plain digits are hexadecimal
            const std::string text = args.value(a);
            const auto r = z80::ExpressionEvaluator({}, 0, true).evaluate(text);
            if (!r.ok || !r.undefinedNames.empty() || r.value < 0 || r.value > 0xFFFF) throw UsageError("invalid load address: '" + text + "'");
            org = static_cast<z80::Word>(r.value);
        }
        else if (!a.empty() && a[0] == '-') throw UsageError("unknown option " + a);
        else if (image.empty()) image = a;
        else throw UsageError("more than one image given");
    }
    z80::Monitor monitor(options);
    if (!image.empty()) {
        const auto data = z80::readFile(image);
        if (z80::looksLikeSna(data)) monitor.memory().load(0, z80::loadSna(data).bytes());
        else monitor.memory().load(org, data);
    }

    const bool interactive = Z80_ISATTY(0) != 0;  // no banner/prompts when a script is piped in
    if (interactive) std::cout << z80::Monitor::banner();
    std::string line;
    for (;;) {
        if (interactive) std::cout << monitor.prompt() << std::flush;
        if (!std::getline(std::cin, line)) break;
        std::cout << monitor.execute(line) << std::flush;
        if (monitor.finished()) break;
    }
    return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            printUsage(std::cerr);
            return kExitUsage;
        }
        const std::string command = argv[1];
        if (command == "asm") return runAsm(Args(argc, argv, 2));
        if (command == "dis") return runDis(Args(argc, argv, 2));
        if (command == "serve") return runServe(Args(argc, argv, 2));
        if (command == "monitor") return runMonitor(Args(argc, argv, 2));
        if (command == "version" || command == "--version") {
            std::cout << "z80tool " << z80::kVersion << "\n";
            return kExitOk;
        }
        if (command == "help" || command == "--help" || command == "-h") {
            printUsage(std::cout);
            return kExitOk;
        }
        throw UsageError("unknown command '" + command + "'");
    } catch (const UsageError& e) {
        std::cerr << "z80tool: " << e.what() << "\n\nTry 'z80tool help'.\n";
        return kExitUsage;
    } catch (const std::exception& e) {
        std::cerr << "z80tool: " << e.what() << "\n";
        return kExitUsage;
    }
}
