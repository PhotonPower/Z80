#include <filesystem>

#include "test_util.hpp"
#include "z80/assembler.hpp"
#include "z80/image_io.hpp"

using z80::Byte;

namespace {

std::vector<Byte> makeSna(std::size_t ramSize = 0x10000) {
    std::vector<Byte> file(z80::kSnaHeaderSize + ramSize, 0);
    const char magic[] = "MV - SNA";
    for (std::size_t i = 0; i < 8; ++i) file[i] = static_cast<Byte>(magic[i]);
    return file;
}

}  // namespace

int main() {
    // ---- SNA detection and loading ----
    {
        auto sna = makeSna();
        sna[z80::kSnaHeaderSize + 0x1234] = 0xAB;
        CHECK(z80::looksLikeSna(sna));
        const z80::Memory m = z80::loadSna(sna);
        CHECK_EQ(m.read(0x1234), Byte{0xAB});
        CHECK_EQ(m.read(0x0000), Byte{0x00});

        const std::vector<Byte> notSna{1, 2, 3};
        CHECK(!z80::looksLikeSna(notSna));

        bool threw = false;
        try {
            z80::loadSna(std::vector<Byte>(z80::kSnaHeaderSize, 0));
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
    }

    // ---- raw loading ----
    {
        const std::vector<Byte> raw{1, 2, 3};
        const z80::Memory m = z80::loadRaw(raw, 0x8000);
        CHECK_EQ(m.read(0x8000), Byte{1});
        CHECK_EQ(m.read(0x8002), Byte{3});
        CHECK_EQ(m.read(0x8003), Byte{0});
        // data that would pass the end of memory is truncated, not wrapped
        const z80::Memory m2 = z80::loadRaw(raw, 0xFFFF);
        CHECK_EQ(m2.read(0xFFFF), Byte{1});
        CHECK_EQ(m2.read(0x0000), Byte{0});
    }

    // ---- patching a snapshot with assembled code ----
    {
        auto sna = makeSna();
        const auto r = z80::Assembler().assemble("ORG &4000\nLD A,5\nRET\nORG &8000\nDB &77\n");
        CHECK(r.ok());
        z80::patchSna(sna, r.segments);
        CHECK_EQ(sna[z80::kSnaHeaderSize + 0x4000], Byte{0x3E});
        CHECK_EQ(sna[z80::kSnaHeaderSize + 0x4001], Byte{0x05});
        CHECK_EQ(sna[z80::kSnaHeaderSize + 0x4002], Byte{0xC9});
        CHECK_EQ(sna[z80::kSnaHeaderSize + 0x8000], Byte{0x77});
        CHECK_EQ(sna[0], Byte{'M'});  // header untouched

        auto tooSmall = makeSna(0x1000);
        bool threw = false;
        try {
            z80::patchSna(tooSmall, r.segments);
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
    }

    // ---- Intel HEX ----
    {
        const std::vector<z80::Segment> segments{{0x0100, {0x21, 0x46, 0x01, 0x36, 0x01}}};
        // sum 05+01+00+00+21+46+01+36+01 = 0xA5, checksum = 0x100 - 0xA5 = 0x5B
        const std::string hex = z80::toIntelHex(segments);
        CHECK(hex.find(":0501000021460136015B") != std::string::npos);  // 5 bytes @0100
        CHECK(hex.find(":00000001FF") != std::string::npos);            // EOF record
        // every record's bytes must sum to 0 mod 256
        std::size_t pos = 0;
        while ((pos = hex.find(':', pos)) != std::string::npos) {
            const std::size_t end = hex.find('\n', pos);
            unsigned sum = 0;
            for (std::size_t i = pos + 1; i + 1 < end; i += 2) {
                sum += static_cast<unsigned>(std::stoul(hex.substr(i, 2), nullptr, 16));
            }
            CHECK_EQ(sum & 0xFF, 0u);
            pos = end;
        }

        // long segments are split into 16-byte records
        std::vector<Byte> big(40, 0xAA);
        const std::string hex2 = z80::toIntelHex({{0x2000, big}});
        CHECK(hex2.find(":10200000") != std::string::npos);
        CHECK(hex2.find(":10201000") != std::string::npos);
        CHECK(hex2.find(":08202000") != std::string::npos);
    }

    // ---- files ----
    {
        const auto path = std::filesystem::temp_directory_path() / "z80tools_test_io.bin";
        const std::vector<Byte> data{9, 8, 7, 0, 255};
        z80::writeFile(path, data);
        CHECK(z80::readFile(path) == data);
        std::filesystem::remove(path);

        bool threw = false;
        try {
            z80::readFile(path);  // removed
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
    }

    return finish("image_io");
}
