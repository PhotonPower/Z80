// z80tool - command line front end for the Z80 assembler and disassembler.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
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

#include "z80/analysis.hpp"
#include "z80/assembler.hpp"
#include "z80/disassembler.hpp"
#include "z80/dsk.hpp"
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
        << "  z80tool dsk <info|ls|extract|boot|sector|new|put> <image.dsk> ...   CPC disk images\n"
        << "  z80tool monitor [image] [--org <hex>] [--no-files]   interactive machine-code monitor\n"
        << "  z80tool serve [--allow-files]   emulator server: JSON requests on stdin, replies on stdout\n"
        << "  z80tool help | version\n"
        << "\n"
        << "asm options:\n"
        << "  -o <file>        write the assembled code as a raw binary\n"
        << "  --hex <file>     write Intel HEX\n"
        << "  --sna <file>     patch the code into the RAM image of a CPC snapshot (in place)\n"
        << "  --amsdos <file>  write a binary with an AMSDOS header (load address = lowest address)\n"
        << "  --exec <addr>    entry address in the AMSDOS header, a number or a label (default: the load address)\n"
        << "  -l <file|->      write a listing ('-' = standard output)\n"
        << "  -s <file|->      write the symbol table\n"
        << "  --org <addr>     start address before the first ORG (default 0)\n"
        << "  -O               optimise: JP / JP NZ,Z,NC,C -> JR where the target is in reach\n"
        << "  (without -o/--hex/--sna the result is written to <source>.bin)\n"
        << "\n"
        << "dis options:\n"
        << "  --format <auto|sna|bin>  input format (default auto: .sna by its header, else raw;\n"
        << "                   a file with an AMSDOS header is loaded at its load address)\n"
        << "  --file <name>    the input is a disk image (.dsk): disassemble this file from it\n"
        << "  --org <addr>     load address of a raw binary (default 0, or the AMSDOS header's)\n"
        << "  -s <addr>        start address / entry point (default: load address or AMSDOS entry)\n"
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
        << "\n"
        << "dsk commands:\n"
        << "  info <image>                   format, tracks, sectors, files\n"
        << "  ls <image> [-a]                directory (-a: also system files)\n"
        << "  extract <image> <name> [-o <file>] [--keep-header]   copy a file out (the AMSDOS header is removed)\n"
        << "  boot <image> [-o <file>]       the boot sector of a SYSTEM disk (track 0, sector &41)\n"
        << "  sector <image> <track> <id>    hex dump of one sector (id e.g. 0xC1)\n"
        << "  new <image> [--format data|system] [--tracks n]   an empty, formatted disk (AMSDOS)\n"
        << "  put <image> <file> [--name NAME.EXT] [--user n] [--system-file] [--read-only]\n"
        << "                                 write a file (as it is: use asm --amsdos for a header)\n"
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
    std::string source, binOut, hexOut, snaFile, listing, symbols, amsdosOut;
    std::string execText;  // a number or a symbol of the program
    z80::AssemblerOptions options;

    while (!args.done()) {
        const std::string a = args.next();
        if (a == "-o") binOut = args.value(a);
        else if (a == "--hex") hexOut = args.value(a);
        else if (a == "--sna") snaFile = args.value(a);
        else if (a == "--amsdos") amsdosOut = args.value(a);
        else if (a == "--exec") execText = args.value(a);
        else if (a == "-l") listing = args.value(a);
        else if (a == "-s") symbols = args.value(a);
        else if (a == "--org") options.origin = parseAddress(args.value(a), "origin");
        else if (a == "-O") options.optimize = true;
        else if (!a.empty() && a[0] == '-') throw UsageError("unknown option " + a);
        else if (source.empty()) source = a;
        else throw UsageError("more than one source file given");
    }
    if (source.empty()) throw UsageError("no source file given");
    if (binOut.empty() && hexOut.empty() && snaFile.empty() && amsdosOut.empty()) {
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
    if (!amsdosOut.empty() && !result.empty()) {
        const auto bytes = result.flatten();
        const auto load = static_cast<z80::Word>(result.lowAddress());
        z80::Word exec = load;
        if (!execText.empty()) {
            std::string upper = execText;
            for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (auto it = result.symbols.find(upper); it != result.symbols.end()) exec = static_cast<z80::Word>(it->second.value);
            else exec = parseAddress(execText, "entry address");
        }
        std::vector<z80::Byte> file = z80::makeAmsdosHeader(fs::path(amsdosOut).filename().string(), load, exec,
                                                            static_cast<std::uint32_t>(bytes.size()));
        file.insert(file.end(), bytes.begin(), bytes.end());
        z80::writeFile(amsdosOut, file);
    }
    if (!snaFile.empty()) {
        auto file = z80::readFile(snaFile);
        z80::patchSna(file, result.segments);
        z80::writeFile(snaFile, file);
    }
    return kExitOk;
}

// ---- dis -------------------------------------------------------------------------------

int runDis(Args args) {
    std::string input, format = "auto", commentFile, output, dskName;
    std::optional<z80::Word> start, end, renderFrom, renderTo, orgGiven;
    z80::Word org = 0;
    std::vector<z80::Word> entries;
    std::vector<std::pair<z80::Word, z80::InlineDataKind>> inlineData;
    z80::DisassemblerOptions options;
    z80::RenderOptions render;

    while (!args.done()) {
        const std::string a = args.next();
        if (a == "--format") format = args.value(a);
        else if (a == "--org") orgGiven = org = parseAddress(args.value(a), "load address");
        else if (a == "--file") dskName = args.value(a);
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

    auto file = z80::readFile(input);
    if (z80::DskImage::looksLikeDsk(file)) {
        if (dskName.empty()) throw UsageError("the input is a disk image: name the file with --file (z80tool dsk ls lists them)");
        file = z80::DskImage::parse(file).readFile(dskName);
    } else if (!dskName.empty()) {
        throw UsageError("--file needs a disk image (.dsk)");
    }
    if (format == "auto") {
        if (const auto h = z80::parseAmsdosHeader(file)) {
            // a file copied from a CPC disk: the header says where it belongs and where it starts
            file.erase(file.begin(), file.begin() + 128);
            if (h->length < file.size()) file.resize(h->length);
            if (!orgGiven) org = h->load;
            if (!start && !orgGiven) start = h->exec;
            std::cerr << "AMSDOS header: " << h->name << ", load &" << z80::hexDigits(h->load, 4) << ", entry &" << z80::hexDigits(h->exec, 4)
                      << ", " << h->length << " bytes\n";
        }
    }
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
        // follow jump tables: their cases are code, not DB rows
        std::vector<std::uint8_t> tableBytes(z80::kAddressSpace, 0);
        dis.setDataBarrier([&tableBytes](z80::Word a) { return tableBytes[a] != 0; });
        z80::AnalysisContext context;
        if (!sna && !file.empty()) {
            context.first = org;
            context.last = static_cast<unsigned>(std::min<std::size_t>(0xFFFF, static_cast<std::size_t>(org) + file.size() - 1));
        } else if (dis.hasResult()) {
            context.first = dis.lowestAddress();
            context.last = dis.highestAddress();
        }
        for (int round = 0; round < 16; ++round) {
            bool grown = false;
            for (const z80::JumpTable& t : z80::findJumpTables(memory, dis, context)) {
                if (!t.followed) continue;
                if (t.kind == z80::JumpTable::Kind::Pointers) {
                    for (unsigned i = 0; i < t.bytes && t.table + i < z80::kAddressSpace; ++i) tableBytes[t.table + i] = 1;
                }
                std::vector<z80::Word> starts = t.targets;
                if (t.kind == z80::JumpTable::Kind::JumpRow) starts.insert(starts.end(), t.slots.begin(), t.slots.end());
                for (z80::Word target : starts) {
                    if (dis.isInstructionStart(target)) continue;
                    dis.addEntryPoint(target);
                    dis.analyzeFlow(target);
                    grown = true;
                }
            }
            if (!grown) break;
        }
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

// ---- dsk -------------------------------------------------------------------------------

int runDsk(Args args) {
    if (args.done()) throw UsageError("dsk needs a sub-command: info, ls, extract, boot, sector, new or put");
    const std::string sub = args.next();
    std::vector<std::string> positional;
    std::string output, fileName, diskFormat = "data";
    bool all = false, keepHeader = false, systemFile = false, readOnly = false;
    unsigned user = 0, tracks = 40;
    while (!args.done()) {
        const std::string a = args.next();
        if (a == "-o") output = args.value(a);
        else if (a == "-a") all = true;
        else if (a == "--keep-header") keepHeader = true;
        else if (a == "--name") fileName = args.value(a);
        else if (a == "--format") diskFormat = args.value(a);
        else if (a == "--system-file") systemFile = true;
        else if (a == "--read-only") readOnly = true;
        else if (a == "--user" || a == "--tracks") {
            const auto v = z80::parseConstant(args.value(a));
            if (!v || *v < 0 || *v > 255) throw UsageError("invalid value for " + a);
            (a == "--user" ? user : tracks) = static_cast<unsigned>(*v);
        }
        else if (!a.empty() && a[0] == '-' && a.size() > 1 && !std::isdigit(static_cast<unsigned char>(a[1]))) throw UsageError("unknown option " + a);
        else positional.push_back(a);
    }
    if (positional.empty()) throw UsageError("no disk image given");
    if (sub == "new") {
        if (fs::exists(positional[0])) throw UsageError("'" + positional[0] + "' exists already");
        const z80::DskImage fresh = z80::DskImage::createFormatted(diskFormat, tracks);
        z80::writeFile(positional[0], fresh.serialize());
        std::cout << "formatted " << positional[0] << ": " << diskFormat << " format, " << tracks << " tracks, " << fresh.freeBytes() / 1024 << " KiB free\n";
        return kExitOk;
    }
    if (sub == "put") {
        if (positional.size() < 2) throw UsageError("put needs the disk image and the file");
        z80::DskImage target = z80::DskImage::parse(z80::readFile(positional[0]));
        const auto data = z80::readFile(positional[1]);
        std::string name = fileName.empty() ? fs::path(positional[1]).filename().string() : fileName;
        target.writeFile(name, data, user, systemFile, readOnly);
        z80::writeFile(positional[0], target.serialize());
        std::cout << "written " << data.size() << " bytes as " << user << ":" << name << " to " << positional[0] << " ("
                  << (z80::parseAmsdosHeader(data) ? "with" : "without") << " AMSDOS header), " << target.freeBytes() / 1024 << " KiB free\n";
        return kExitOk;
    }
    const z80::DskImage disk = z80::DskImage::parse(z80::readFile(positional[0]));
    auto writeBytes = [&](const std::vector<z80::Byte>& bytes, const std::string& fallback) {
        const std::string target = output.empty() ? fallback : output;
        z80::writeFile(target, bytes);
        std::cout << "written " << bytes.size() << " bytes to " << target << "\n";
    };
    if (sub == "info") {
        std::cout << (disk.extended() ? "extended" : "standard") << " disk image, " << disk.trackCount() << " tracks, " << disk.sides() << " side(s)";
        if (!disk.creator().empty()) std::cout << ", created by " << disk.creator();
        std::cout << "\n";
        if (const auto* t0 = disk.track(0)) {
            std::cout << "track 0: " << t0->sectors.size() << " sectors, IDs";
            for (const auto& sec : t0->sectors) std::cout << " &" << z80::hexDigits(sec.r, 2);
            std::cout << "\n";
        }
        const std::string fmt = disk.format();
        if (fmt.empty()) {
            std::cout << "no AMSDOS format recognised (a copy-protected disk or one with its own loader): use 'sector'\n";
        } else {
            const auto files = disk.files();
            std::cout << "format: " << fmt << ", " << files.size() << " file(s) in the directory\n";
        }
        return kExitOk;
    }
    if (sub == "ls") {
        for (const auto& f : disk.files()) {
            if (f.system && !all) continue;
            std::cout << std::to_string(f.user) << ":" << f.name;
            for (std::size_t i = f.name.size(); i < 13; ++i) std::cout << ' ';
            std::cout << f.size << " bytes" << (f.readOnly ? "  read-only" : "") << (f.system ? "  system" : "");
            try {
                const auto data = disk.readFile(f.name, f.user);
                if (const auto h = z80::parseAmsdosHeader(data)) {
                    std::cout << "  AMSDOS type " << unsigned(h->type) << ", load &" << z80::hexDigits(h->load, 4) << ", entry &" << z80::hexDigits(h->exec, 4)
                              << ", " << h->length << " bytes";
                }
            } catch (const z80::DskError& e) {
                std::cout << "  (unreadable: " << e.what() << ")";
            }
            std::cout << "\n";
        }
        return kExitOk;
    }
    if (sub == "extract") {
        if (positional.size() < 2) throw UsageError("extract needs the file name");
        auto data = disk.readFile(positional[1]);
        if (const auto h = z80::parseAmsdosHeader(data); h && !keepHeader) {
            std::cout << "AMSDOS header: type " << unsigned(h->type) << ", load &" << z80::hexDigits(h->load, 4) << ", entry &" << z80::hexDigits(h->exec, 4) << "\n";
            data.erase(data.begin(), data.begin() + 128);
            if (h->length < data.size()) data.resize(h->length);
        }
        writeBytes(data, positional[1]);
        return kExitOk;
    }
    if (sub == "boot") {
        writeBytes(disk.bootSector(), "boot.bin");
        return kExitOk;
    }
    if (sub == "sector") {
        if (positional.size() < 3) throw UsageError("sector needs the track and the sector ID");
        const auto track = z80::parseConstant(positional[1]);
        const auto id = z80::parseConstant(positional[2]);
        if (!track || !id || *track < 0 || *track > 255 || *id < 0 || *id > 255) throw UsageError("invalid track or sector ID");
        const auto* sec = disk.sector(static_cast<unsigned>(*track), static_cast<z80::Byte>(*id));
        if (!sec) throw std::runtime_error("track " + positional[1] + " has no sector " + positional[2]);
        std::cout << "C=" << unsigned(sec->c) << " H=" << unsigned(sec->h) << " R=&" << z80::hexDigits(sec->r, 2) << " N=" << unsigned(sec->n)
                  << " ST1=&" << z80::hexDigits(sec->st1, 2) << " ST2=&" << z80::hexDigits(sec->st2, 2) << ", " << sec->data.size() << " bytes\n";
        for (std::size_t i = 0; i < sec->data.size(); i += 16) {
            std::string ascii;
            std::cout << z80::hexDigits(static_cast<unsigned>(i), 4) << ' ';
            for (std::size_t k = i; k < i + 16 && k < sec->data.size(); ++k) {
                std::cout << ' ' << z80::hexDigits(sec->data[k], 2);
                ascii += (sec->data[k] >= 0x20 && sec->data[k] < 0x7F) ? static_cast<char>(sec->data[k]) : '.';
            }
            std::cout << "  " << ascii << "\n";
        }
        return kExitOk;
    }
    throw UsageError("unknown dsk sub-command '" + sub + "'");
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
        auto data = z80::readFile(image);
        if (z80::looksLikeSna(data)) {
            monitor.memory().load(0, z80::loadSna(data).bytes());
        } else if (const auto h = z80::parseAmsdosHeader(data)) {
            std::cout << "AMSDOS header: load &" << z80::hexDigits(h->load, 4) << ", entry &" << z80::hexDigits(h->exec, 4) << "\n";
            data.erase(data.begin(), data.begin() + 128);
            if (h->length < data.size()) data.resize(h->length);
            monitor.memory().load(h->load, data);
        } else {
            monitor.memory().load(org, data);
        }
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
        if (command == "dsk") return runDsk(Args(argc, argv, 2));
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
