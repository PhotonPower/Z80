// Randomised (but deterministic) tests:
//  * for arbitrary memory contents, disassembling to source and assembling again must
//    reproduce the original bytes exactly,
//  * neither tool may crash on garbage input.

#include <random>

#include "test_util.hpp"
#include "z80/annotations.hpp"
#include "z80/assembler.hpp"
#include "z80/cpc.hpp"
#include "z80/disassembler.hpp"
#include "z80/image_io.hpp"
#include "z80/json.hpp"
#include "z80/session.hpp"
#include "z80/snapshot.hpp"

using z80::Byte;
using z80::Word;

int main() {
    std::mt19937 rng(0xC0FFEE);

    // ---- disassemble -> assemble round trip on random data ----
    for (int iteration = 0; iteration < 150; ++iteration) {
        z80::Memory memory;
        const Word base = static_cast<Word>(rng() % 0xE000);
        const std::size_t size = 16 + rng() % 1500;
        // mix of random bytes and plausible code-like bytes to exercise flow following
        for (std::size_t i = 0; i < size; ++i) {
            Byte b = static_cast<Byte>(rng());
            if (rng() % 7 == 0) b = 0x00;
            if (rng() % 50 == 0) b = 0xC9;  // RET
            memory.write(static_cast<Word>(base + i), b);
        }

        z80::DisassemblerOptions options;
        options.zeroRunLimit = rng() % 2 ? 16 : 0;
        z80::Disassembler d(memory, options);
        d.setAnalysisRange(base, static_cast<Word>(base + size - 1));
        if (iteration % 3 == 0) {
            d.analyzeRange(base, static_cast<Word>(base + size - 1));
        } else {
            d.addEntryPoint(static_cast<Word>(base + rng() % size));
            d.analyzeFlow(base);
        }

        z80::RenderOptions render;
        render.asmSource = true;
        render.from = base;
        render.to = static_cast<Word>(base + size - 1);
        const std::string source = d.render(render);

        const auto result = z80::Assembler().assemble(source);
        ++g_checks;
        if (!result.ok()) {
            ++g_failures;
            std::cerr << "iteration " << iteration << ": generated source does not assemble\n"
                      << result.formatDiagnostics("generated") << "\n";
            continue;
        }
        const std::vector<Byte> bytes = result.flatten();
        ++g_checks;
        bool same = bytes.size() == size && result.lowAddress() == base;
        for (std::size_t i = 0; same && i < size; ++i) {
            same = bytes[i] == memory.read(static_cast<Word>(base + i));
        }
        if (!same) {
            ++g_failures;
            std::cerr << "iteration " << iteration << ": round trip mismatch (size " << bytes.size()
                      << " vs " << size << ")\n";
        }

        // the listing renderer must also cope with the same data
        const std::string listing = d.render({});
        CHECK(!listing.empty());
    }

    // ---- assembler must survive garbage ----
    {
        const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 ,;:()+-*/$&%#'\"\t._@?!<>=~^|";
        for (int iteration = 0; iteration < 3000; ++iteration) {
            std::string source;
            const int lines = 1 + static_cast<int>(rng() % 6);
            for (int l = 0; l < lines; ++l) {
                const int length = static_cast<int>(rng() % 40);
                for (int i = 0; i < length; ++i) source += alphabet[rng() % alphabet.size()];
                source += '\n';
            }
            const auto r = z80::Assembler().assemble(source);
            (void)r.formatListing();
            (void)r.formatDiagnostics("fuzz");
            ++g_checks;  // reaching this point without crashing is the assertion
        }

        // structured garbage: real mnemonics with random operands
        const char* mnemonics[] = {"LD", "ADD", "JP", "JR", "CALL", "BIT", "IN", "OUT", "PUSH", "EX", "RST", "IM", "DB", "DW", "DS", "ORG", "EQU"};
        const char* operands[] = {"A", "B,C", "(HL)", "(IX+5)", "(IY-200)", "HL,(&1234)", "NZ,$", "7,(IX+1)", "&FFFF", "-1", "(", ")", ",", ",,", "1+", "AF'", "(C),A", "\"str\"", "'x'", "$+300"};
        for (int iteration = 0; iteration < 3000; ++iteration) {
            std::string source = "  ";
            source += mnemonics[rng() % std::size(mnemonics)];
            source += ' ';
            source += operands[rng() % std::size(operands)];
            if (rng() % 2) {
                source += ',';
                source += operands[rng() % std::size(operands)];
            }
            source += '\n';
            const auto r = z80::Assembler().assemble(source);
            ++g_checks;
            // an accepted line must produce exactly what the decoder reads back as a valid instruction
            if (r.ok() && r.size() > 0 && r.size() <= 4 && source.find("DB") == std::string::npos &&
                source.find("DW") == std::string::npos && source.find("DS") == std::string::npos &&
                source.find("ORG") == std::string::npos && source.find("EQU") == std::string::npos) {
                z80::Memory m;
                r.applyTo(m);
                const auto ins = z80::decode(m, r.lowAddress());
                ++g_checks;
                if (!ins.valid || ins.length != r.size()) {
                    ++g_failures;
                    std::cerr << "assembled '" << source << "' to bytes the decoder rejects or reads with another length\n";
                }
            }
        }
    }

    // ---- JSON parser: garbage and truncated documents throw JsonError, nothing else ----
    {
        const std::string alphabet = "{}[]\":,0123456789.-+eEtruefalsn \\\"abc";
        for (int iteration = 0; iteration < 5000; ++iteration) {
            std::string text;
            const int length = static_cast<int>(rng() % 60);
            for (int i = 0; i < length; ++i) text += alphabet[rng() % alphabet.size()];
            try {
                (void)z80::Json::parse(text).dump();
            } catch (const z80::JsonError&) {
            }
            ++g_checks;
        }
    }

    // ---- the server: random commands with arguments of every JSON type never crash and always answer ----
    {
        z80::SessionOptions options;
        options.allowFiles = false;  // file names below are random: nothing must be read or written
        z80::Session session(options);
        (void)session.handleLine(R"({"cmd":"asm","source":"        ORG &4000\nstart: LD HL,tab\n LD A,(HL)\n CALL &BB5A\n JP (HL)\ntab: DW start,start\n DB \"Hello\",0\n RET\n"})");
        const char* commands[] = {"asm", "dis", "read", "write", "regs", "set", "step", "step_over", "run", "run_until_return", "call", "break", "unbreak",
                                  "breaks", "clear_breaks", "watch", "unwatch", "checkpoint", "rollback", "diff", "drop", "checkpoints", "state", "set_state",
                                  "interrupt", "nmi", "io", "input", "reset", "symbols", "help", "info", "machine", "video", "screen", "screen_diff", "screens",
                                  "screen_drop", "firmware_log", "console", "analyze", "routines", "listing", "jump_tables", "data_regions", "references",
                                  "ann_label", "ann_routine", "ann_variable", "ann_data", "ann_comment", "ann_constant", "ann_remove", "ann_title", "ann_list",
                                  "track_writes", "written", "run_until_unpacked", "save_memory", "load_file", "ann_save", "insert_disk", "disk_log",
                                  "save_disk", "controls", "coverage", "nonsense"};
        const char* keys[] = {"addr", "count", "len", "hex", "bytes", "regs", "n", "trace", "max_instructions", "max_cycles", "until", "name", "kind",
                              "source", "origin", "entry", "entries", "from", "to", "type", "text", "size", "length", "block", "apply", "min_written",
                              "continue", "ignore", "order", "max_chars", "irq_every_cycles", "file", "state", "a", "b", "crop", "remove", "on", "tables",
                              "joystick", "keys", "addresses", "history", "inline_words", "inline_control_args", "raster"};
        auto randomValue = [&](int depth, auto& self) -> z80::Json {
            switch (rng() % 9) {
                case 0: return z80::Json(nullptr);
                case 1: return z80::Json(rng() % 2 == 0);
                case 2: return z80::Json(static_cast<std::int64_t>(rng() % 70000) - 1000);
                case 3: return z80::Json(static_cast<double>(rng() % 1000) / 7.0);
                case 4: {
                    const char* strings[] = {"0x4000", "start", "&FFFF", "HL+1", "x", "", "0x10000", "-5", "cpc", "pointers", "text", "40", "1,2"};
                    return z80::Json(std::string(strings[rng() % std::size(strings)]));
                }
                case 5: {
                    z80::Json a = z80::Json::array();
                    if (depth < 2) {
                        for (unsigned i = rng() % 4; i > 0; --i) a.push(self(depth + 1, self));
                    }
                    return a;
                }
                case 6: {
                    z80::Json o = z80::Json::object();
                    if (depth < 2) {
                        for (unsigned i = rng() % 3; i > 0; --i) o.set(keys[rng() % std::size(keys)], self(depth + 1, self));
                    }
                    return o;
                }
                case 7: return z80::Json(static_cast<std::int64_t>(rng() % 3) == 0 ? std::int64_t{1} << 40 : std::int64_t{0});
                default: return z80::Json(static_cast<std::int64_t>(rng() % 256));
            }
        };
        for (int iteration = 0; iteration < 6000; ++iteration) {
            z80::Json request = z80::Json::object();
            request.set("cmd", std::string(commands[rng() % std::size(commands)]));
            // runs get a small limit unless a random one replaces it (the default of a million instructions makes the test slow under sanitizers)
            request.set("max_instructions", static_cast<std::int64_t>(rng() % 5000));
            request.set("max_cycles", static_cast<std::int64_t>(rng() % 20000));
            for (unsigned i = rng() % 5; i > 0; --i) request.set(keys[rng() % std::size(keys)], randomValue(0, randomValue));
            const std::string reply = session.handleLine(request.dump());
            const z80::Json parsed = z80::Json::parse(reply);
            ++g_checks;
            if (!parsed.find("ok")) {
                ++g_failures;
                std::cerr << "reply without ok: " << reply << "\n";
            }
        }
        // garbage lines
        for (int iteration = 0; iteration < 500; ++iteration) {
            std::string line;
            for (int i = static_cast<int>(rng() % 40); i > 0; --i) line += static_cast<char>(32 + rng() % 95);
            (void)session.handleLine(line);
            ++g_checks;
        }
    }

    // ---- loaders: snapshots, .sna, Intel HEX and project files with random or truncated contents ----
    {
        for (int iteration = 0; iteration < 2000; ++iteration) {
            std::vector<Byte> data(rng() % 300);
            for (Byte& b : data) b = static_cast<Byte>(rng());
            if (iteration % 3 == 0 && data.size() > 8) std::copy_n("Z80EMUS1", 8, data.begin());
            if (iteration % 3 == 1 && data.size() > 8) std::copy_n("MV - SNA", 8, data.begin());
            try {
                (void)z80::deserializeSnapshot(data);
            } catch (const std::exception&) {
            }
            try {
                (void)z80::loadSna(data);
            } catch (const std::exception&) {
            }
            (void)z80::cpc::parseSnaHeader(data);
            ++g_checks;
        }
        const char* projects[] = {R"({"labels":{"0x4000":"main"},"routines":[{"address":"0x4000","name":"main"}]})", R"({"labels":{"zz":"x"}})",
                                  R"({"routines":[{"name":"x"}]})", R"({"data":[{"address":"0x10","length":-1,"kind":"text"}]})",
                                  R"({"variables":[{"address":"0xFFFF","name":"v","size":99}]})", R"({"constants":["0x4000",{"a":1},null]})",
                                  R"([])", R"({"comments":{"0x4000":5}})"};
        for (const char* p : projects) {
            try {
                (void)z80::Annotations::fromJson(z80::Json::parse(p)).toJson().dump();
            } catch (const std::exception&) {
            }
            ++g_checks;
        }
    }

    return finish("robustness");
}
