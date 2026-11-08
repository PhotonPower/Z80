// Self-extracting programs: track_writes, written, run_until_unpacked, save_memory; run limits and empty-memory fetches.

#include <filesystem>
#include <fstream>

#include "test_util.hpp"
#include "z80/json.hpp"
#include "z80/session.hpp"

using namespace z80;

namespace {

Json ask(Session& s, const std::string& request) { return Json::parse(s.handleLine(request)); }
bool ok(const Json& j) { return j.find("ok") && j.find("ok")->asBool(); }
std::string str(const Json& j, const char* key) {
    const Json* v = j.find(key);
    return v && v->isString() ? v->asString() : "<missing>";
}
std::string quote(const std::string& text) { return Json(text).dump(); }

/// The payload: stores 42 at &9000 and halts; padded with code-like filler to `size` bytes.
std::vector<Byte> payload(std::size_t size) {
    std::vector<Byte> p = {0x3E, 42, 0x32, 0x00, 0x90, 0x76};  // LD A,42 / LD (&9000),A / HALT
    while (p.size() < size) p.push_back(static_cast<Byte>(p.size() * 7 + 1));
    return p;
}

std::string dbLines(const std::vector<Byte>& bytes, Byte key) {
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 16) {
        out += "        DB ";
        for (std::size_t k = i; k < bytes.size() && k < i + 16; ++k) out += (k > i ? "," : "") + std::string("&") + hexDigits(bytes[k] ^ key, 2);
        out += "\n";
    }
    return out;
}

/// A one-stage unpacker: decodes `packed` (XOR &5A) to &8000 and jumps there.
std::string oneStage(std::size_t size) {
    std::string src =
        "        ORG &4000\n"
        "start:  LD HL,packed\n"
        "        LD DE,&8000\n"
        "        LD BC," + std::to_string(size) + "\n"
        "loop:   LD A,(HL)\n"
        "        XOR &5A\n"
        "        LD (DE),A\n"
        "        INC HL\n"
        "        INC DE\n"
        "        DEC BC\n"
        "        LD A,B\n"
        "        OR C\n"
        "        JR NZ,loop\n"
        "        JP &8000\n"
        "packed:\n";
    return src + dbLines(payload(size), 0x5A);
}

}  // namespace

int main() {
    // ---- one stage ----
    {
        Session s;
        CHECK(ok(ask(s, "{\"cmd\":\"asm\",\"source\":" + quote(oneStage(1500)) + ",\"entry\":\"start\"}")));
        const Json r = ask(s, R"({"cmd":"run_until_unpacked"})");
        CHECK(ok(r));
        CHECK_EQ(str(r, "stop"), std::string("unpacked"));
        CHECK_EQ(str(r, "pc"), std::string("0x8000"));
        const Json* region = r.find("region");
        CHECK(region != nullptr);
        if (region) {
            CHECK_EQ(str(*region, "start"), std::string("0x8000"));
            CHECK_EQ(region->find("size")->asInt(), 1500);
        }
        CHECK(r.find("written_bytes")->asInt() >= 1500);
        // the program has not run yet: the payload's store is still to come
        CHECK_EQ(s.memory().read(0x9000), Byte{0});
        const Json w = ask(s, R"({"cmd":"written"})");
        CHECK(w.find("tracking")->asBool());
        CHECK_EQ(str(w.find("ranges")->items()[0], "start"), std::string("0x8000"));
        CHECK(ok(ask(s, R"({"cmd":"run"})")));
        CHECK_EQ(s.memory().read(0x9000), Byte{42});

        // save_memory needs file access
        CHECK(!ok(ask(s, R"({"cmd":"save_memory","addr":"0x8000","len":16,"file":"x.bin"})")));
    }
    {
        SessionOptions options;
        options.allowFiles = true;
        Session s(options);
        CHECK(ok(ask(s, "{\"cmd\":\"asm\",\"source\":" + quote(oneStage(1200)) + ",\"entry\":\"start\"}")));
        CHECK_EQ(str(ask(s, R"({"cmd":"run_until_unpacked"})"), "stop"), std::string("unpacked"));
        const auto file = std::filesystem::temp_directory_path() / "z80_unpacked_test.bin";
        const Json saved = ask(s, "{\"cmd\":\"save_memory\",\"addr\":\"0x8000\",\"len\":1200,\"file\":" + quote(file.string()) + "}");
        CHECK(ok(saved));
        std::ifstream in(file, std::ios::binary);
        std::vector<Byte> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(data == payload(1200));
        in.close();
        std::filesystem::remove(file);
    }

    // ---- a small payload is below the threshold; min_written finds it ----
    {
        Session s;
        CHECK(ok(ask(s, "{\"cmd\":\"asm\",\"source\":" + quote(oneStage(300)) + ",\"entry\":\"start\"}")));
        const Json r = ask(s, R"({"cmd":"run_until_unpacked","max_instructions":100000})");
        CHECK(ok(r));
        CHECK_EQ(str(r, "stop"), std::string("halted"));  // ran through: 300 bytes are below the default of 1024
        Session s2;
        CHECK(ok(ask(s2, "{\"cmd\":\"asm\",\"source\":" + quote(oneStage(300)) + ",\"entry\":\"start\"}")));
        CHECK_EQ(str(ask(s2, R"({"cmd":"run_until_unpacked","min_written":256})"), "pc"), std::string("0x8000"));
    }

    // ---- two stages: the unpacker copies itself to &A000 first; continue skips the stage that was found ----
    {
        std::string src =
            "        ORG &4000\n"
            "start:  LD HL,stage2\n"
            "        LD DE,&A000\n"
            "        LD BC,stage2_end-stage2\n"
            "        LDIR\n"
            "        JP &A000\n"
            "stage2:\n"                 // assembled for &4000+, copied to &A000: only relative jumps and absolute data addresses
            "        LD HL,packed\n"
            "        LD DE,&8000\n"
            "        LD BC,1400\n"
            "s2loop: LD A,(HL)\n"
            "        XOR &5A\n"
            "        LD (DE),A\n"
            "        INC HL\n"
            "        INC DE\n"
            "        DEC BC\n"
            "        LD A,B\n"
            "        OR C\n"
            "        JR NZ,s2loop\n"
            "        JP &8000\n"
            "stage2_end:\n"
            "packed:\n" + dbLines(payload(1400), 0x5A);
        Session s;
        CHECK(ok(ask(s, "{\"cmd\":\"asm\",\"source\":" + quote(src) + ",\"entry\":\"start\"}")));
        Json r = ask(s, R"({"cmd":"run_until_unpacked","min_written":16})");
        CHECK_EQ(str(r, "pc"), std::string("0xA000"));  // the copied unpacker
        r = ask(s, R"({"cmd":"run_until_unpacked","min_written":16,"continue":true})");
        CHECK_EQ(str(r, "stop"), std::string("unpacked"));
        CHECK_EQ(str(r, "pc"), std::string("0x8000"));
        // without continue and with the default threshold the small stage does not count at all
        Session s2;
        CHECK(ok(ask(s2, "{\"cmd\":\"asm\",\"source\":" + quote(src) + ",\"entry\":\"start\"}")));
        CHECK_EQ(str(ask(s2, R"({"cmd":"run_until_unpacked"})"), "pc"), std::string("0x8000"));
        // ignore takes a range out by hand
        Session s3;
        CHECK(ok(ask(s3, "{\"cmd\":\"asm\",\"source\":" + quote(src) + ",\"entry\":\"start\"}")));
        CHECK_EQ(str(ask(s3, R"({"cmd":"run_until_unpacked","min_written":16,"ignore":[["0xA000","0xA0FF"]]})"), "pc"), std::string("0x8000"));
    }

    // ---- track_writes / written by hand ----
    {
        Session s;
        CHECK(ok(ask(s, R"({"cmd":"asm","source":"        ORG &4000\n        LD HL,&5000\n        LD (HL),1\n        INC HL\n        LD (HL),2\n        HALT\n","entry":"0x4000"})")));
        CHECK(!ask(s, R"({"cmd":"written"})").find("tracking")->asBool());
        CHECK(ok(ask(s, R"({"cmd":"track_writes"})")));
        CHECK(ok(ask(s, R"({"cmd":"run"})")));
        const Json w = ask(s, R"({"cmd":"written","min_length":1})");
        CHECK_EQ(w.find("bytes")->asInt(), 2);
        CHECK_EQ(str(w.find("ranges")->items()[0], "start"), std::string("0x5000"));
        CHECK_EQ(str(w.find("ranges")->items()[0], "end"), std::string("0x5001"));
        CHECK(!ask(s, R"({"cmd":"track_writes","on":false})").find("tracking")->asBool());
    }

    // ---- limits: a model cannot block the server with a huge limit ----
    {
        Session s;
        CHECK(!ok(ask(s, R"({"cmd":"run","max_instructions":200000000})")));
        CHECK(!ok(ask(s, R"({"cmd":"run","max_instructions":0})")));            // no limit at all
        CHECK(ok(ask(s, R"({"cmd":"run","max_instructions":0,"max_cycles":1000})")));
        CHECK(!ok(ask(s, R"({"cmd":"step","n":2000000})")));
    }

    // ---- empty memory: a call through a pointer that was never set slides through zeros ----
    {
        Session s;
        CHECK(ok(ask(s, R"({"cmd":"asm","source":"        ORG &4000\n        LD HL,(&5000)\n        JP (HL)\n"})")));
        const Json r = ask(s, R"({"cmd":"call","addr":"0x4000","max_instructions":1000})");
        CHECK(ok(r));
        CHECK(!r.find("returned")->asBool());
        CHECK(r.find("fetched_from_empty_memory") != nullptr);
        CHECK_EQ(str(r, "first_empty_fetch"), std::string("0x0000"));
        // a routine that returns normally reports nothing of the kind
        CHECK(ok(ask(s, R"({"cmd":"asm","source":"        ORG &4100\n        LD A,1\n        RET\n"})")));
        const Json fine = ask(s, R"({"cmd":"call","addr":"0x4100"})");
        CHECK(fine.find("returned")->asBool());
        CHECK(fine.find("fetched_from_empty_memory") == nullptr);
    }

    return finish("unpack");
}
