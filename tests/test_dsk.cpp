// CPC disk images (.dsk) and the AMSDOS file system; AMSDOS headers; load_file / dsk_ls in the session.
// The images are built here, byte by byte, the way CPCEMU and AMSDOS lay them out.

#include <filesystem>
#include <fstream>
#include <map>
#include <random>

#include "test_util.hpp"
#include "z80/dsk.hpp"
#include "z80/json.hpp"
#include "z80/session.hpp"

using namespace z80;

namespace {

struct FileSpec {
    std::string name;  // "NAME.EXT"
    std::vector<Byte> data;
    unsigned user = 0;
    bool system = false, readOnly = false, deleted = false;
};

/// Builds a 40-track single-sided disk in the given format with the files (1 KiB blocks, 64 directory entries).
std::vector<Byte> buildDisk(bool extended, Byte firstId, const std::vector<FileSpec>& files, bool interleave = true, std::vector<Byte> boot = {}) {
    const unsigned reserved = firstId == 0xC1 ? 0 : firstId == 0x41 ? 2 : 1;
    const unsigned spt = firstId == 0x01 ? 8 : 9;
    const unsigned tracks = 40;
    // logical sectors of the data area: sector k of track t -> image
    std::map<std::pair<unsigned, unsigned>, std::vector<Byte>> sectors;  // (track, id) -> data
    for (unsigned t = 0; t < tracks; ++t) {
        for (unsigned k = 0; k < spt; ++k) sectors[{t, firstId + k}] = std::vector<Byte>(512, 0xE5);
    }
    auto blockSector = [&](unsigned block, unsigned half) -> std::vector<Byte>& {
        const unsigned logical = block * 2 + half;
        return sectors[{reserved + logical / spt, firstId + logical % spt}];
    };
    std::vector<Byte> dir(2048, 0xE5);
    unsigned nextBlock = 2, entry = 0;
    for (const FileSpec& f : files) {
        const unsigned records = static_cast<unsigned>((f.data.size() + 127) / 128);
        unsigned extent = 0, record = 0;
        do {
            Byte* e = &dir[32 * entry++];
            std::fill(e, e + 32, Byte{0});
            e[0] = f.deleted ? 0xE5 : static_cast<Byte>(f.user);
            std::string base = f.name.substr(0, f.name.find('.')), ext = f.name.find('.') == std::string::npos ? "" : f.name.substr(f.name.find('.') + 1);
            for (unsigned i = 0; i < 8; ++i) e[1 + i] = static_cast<Byte>(i < base.size() ? base[i] : ' ');
            for (unsigned i = 0; i < 3; ++i) e[9 + i] = static_cast<Byte>(i < ext.size() ? ext[i] : ' ');
            if (f.readOnly) e[9] |= 0x80;
            if (f.system) e[10] |= 0x80;
            e[12] = static_cast<Byte>(extent & 0x1F);
            e[14] = static_cast<Byte>(extent >> 5);
            const unsigned here = std::min(128u, records - record);
            e[15] = static_cast<Byte>(here);
            for (unsigned b = 0; b < (here * 128 + 1023) / 1024; ++b) {
                const unsigned block = nextBlock++;
                e[16 + b] = static_cast<Byte>(block);
                for (unsigned half = 0; half < 2; ++half) {
                    auto& sec = blockSector(block, half);
                    for (unsigned i = 0; i < 512; ++i) {
                        const std::size_t at = static_cast<std::size_t>(record) * 128 + b * 1024 + half * 512 + i;
                        sec[i] = at < f.data.size() ? f.data[at] : 0x1A;
                    }
                }
            }
            record += here;
            ++extent;
        } while (record < records);
    }
    for (unsigned half = 0; half < 4; ++half) {
        auto& sec = blockSector(half / 2, half % 2);
        std::copy(dir.begin() + half * 512, dir.begin() + half * 512 + 512, sec.begin());
    }
    if (!boot.empty()) {
        boot.resize(512, 0);
        sectors[{0, 0x41}] = boot;
    }

    std::vector<Byte> img(256, 0);
    const char* magic = extended ? "EXTENDED CPC DSK File\r\nDisk-Info\r\n" : "MV - CPCEMU Disk-File\r\nDisk-Info\r\n";
    std::copy(magic, magic + 34, img.begin());
    const char* creator = "z80tool test";
    std::copy(creator, creator + 12, img.begin() + 0x22);
    img[0x30] = static_cast<Byte>(tracks);
    img[0x31] = 1;
    const unsigned trackSize = 256 + spt * 512;
    if (!extended) {
        img[0x32] = static_cast<Byte>(trackSize);
        img[0x33] = static_cast<Byte>(trackSize >> 8);
    }
    for (unsigned t = 0; t < tracks; ++t) {
        if (extended) img[0x34 + t] = static_cast<Byte>(trackSize / 256);
        std::vector<Byte> header(256, 0);
        const char* ti = "Track-Info\r\n";
        std::copy(ti, ti + 12, header.begin());
        header[0x10] = static_cast<Byte>(t);
        header[0x14] = 2;  // 512 bytes
        header[0x15] = static_cast<Byte>(spt);
        header[0x16] = 0x4E;
        header[0x17] = 0xE5;
        std::vector<unsigned> order;
        for (unsigned k = 0; k < spt; ++k) order.push_back(k);
        if (interleave && spt == 9) order = {0, 5, 1, 6, 2, 7, 3, 8, 4};  // the usual CPC interleave
        std::vector<Byte> data;
        for (unsigned i = 0; i < spt; ++i) {
            const unsigned id = firstId + order[i];
            Byte* info = &header[0x18 + 8 * i];
            info[0] = static_cast<Byte>(t);
            info[2] = static_cast<Byte>(id);
            info[3] = 2;
            info[6] = 0x00;
            info[7] = 0x02;
            const auto& sec = sectors[{t, id}];
            data.insert(data.end(), sec.begin(), sec.end());
        }
        img.insert(img.end(), header.begin(), header.end());
        img.insert(img.end(), data.begin(), data.end());
    }
    return img;
}

std::vector<Byte> pattern(std::size_t n, unsigned seed) {
    std::vector<Byte> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<Byte>((i * 31 + seed * 7 + (i >> 8)) & 0xFF);
    return v;
}

Json ask(Session& s, const std::string& request) { return Json::parse(s.handleLine(request)); }
bool ok(const Json& j) { return j.find("ok") && j.find("ok")->asBool(); }
std::string quote(const std::string& text) { return Json(text).dump(); }

}  // namespace

int main() {
    // ---- AMSDOS header ----
    {
        const auto h = makeAmsdosHeader("game.bin", 0x4000, 0x4010, 300);
        CHECK_EQ(h.size(), std::size_t{128});
        const auto p = parseAmsdosHeader(h);
        CHECK(p.has_value());
        if (p) {
            CHECK_EQ(p->name, std::string("GAME.BIN"));
            CHECK_EQ(p->load, Word{0x4000});
            CHECK_EQ(p->exec, Word{0x4010});
            CHECK_EQ(p->length, std::uint32_t{300});
            CHECK_EQ(unsigned(p->type), 2u);
        }
        auto broken = h;
        broken[21] ^= 1;  // checksum no longer matches
        CHECK(!parseAmsdosHeader(broken));
        CHECK(!parseAmsdosHeader(std::vector<Byte>(128, 0)));  // zeros have a matching checksum but are no header
        CHECK(!parseAmsdosHeader(std::vector<Byte>(100, 1)));  // too short
    }

    // ---- a standard DATA disk: header, multi-extent file, deleted entry, system file, other user ----
    const auto game = pattern(5000, 1);
    std::vector<Byte> withHeader = makeAmsdosHeader("GAME.BIN", 0x1000, 0x1234, static_cast<std::uint32_t>(game.size()));
    withHeader.insert(withHeader.end(), game.begin(), game.end());
    const auto big = pattern(40000, 2);  // three extents of 16 KiB
    const std::vector<FileSpec> files = {
        {"GAME.BIN", withHeader},
        {"BIG.DAT", big},
        {"OLD.BAS", pattern(700, 3), 0, false, false, true},
        {"HIDDEN.SYS", pattern(130, 4), 0, true, true},
        {"USER1.TXT", pattern(256, 5), 1},
    };
    {
        const auto img = buildDisk(false, 0xC1, files);
        CHECK(DskImage::looksLikeDsk(img));
        const DskImage d = DskImage::parse(img);
        CHECK(!d.extended());
        CHECK_EQ(d.trackCount(), 40u);
        CHECK_EQ(d.format(), std::string("data"));
        CHECK_EQ(d.creator(), std::string("z80tool test"));
        const auto list = d.files();
        CHECK_EQ(list.size(), std::size_t{4});  // the deleted file is not listed
        bool sawSystem = false, sawUser1 = false;
        for (const auto& f : list) {
            if (f.name == "HIDDEN.SYS") sawSystem = f.system && f.readOnly;
            if (f.name == "USER1.TXT") sawUser1 = f.user == 1;
            CHECK(f.name != "OLD.BAS");
        }
        CHECK(sawSystem);
        CHECK(sawUser1);
        const auto g = d.readFile("game.bin");  // case does not matter
        CHECK_EQ(g.size(), (withHeader.size() + 127) / 128 * 128);
        CHECK(std::equal(withHeader.begin(), withHeader.end(), g.begin()));
        const auto h = parseAmsdosHeader(g);
        CHECK(h && h->load == 0x1000 && h->exec == 0x1234);
        const auto b = d.readFile("BIG.DAT");
        CHECK_EQ(b.size(), std::size_t{40064});  // whole records
        CHECK(std::equal(big.begin(), big.end(), b.begin()));
        CHECK_EQ(d.readFile("USER1.TXT", 1u).size(), std::size_t{256});
        bool threw = false;
        try {
            (void)d.readFile("USER1.TXT", 0u);
        } catch (const DskError&) {
            threw = true;
        }
        CHECK(threw);
        threw = false;
        try {
            (void)d.readFile("NOPE.BIN");
        } catch (const DskError&) {
            threw = true;
        }
        CHECK(threw);
        threw = false;
        try {
            (void)d.bootSector();  // a DATA disk has none
        } catch (const DskError&) {
            threw = true;
        }
        CHECK(threw);
        const auto* sec = d.sector(0, 0xC1);
        CHECK(sec && sec->data.size() == 512 && sec->data[0] == 0);  // the first directory entry: user 0
    }

    // ---- an extended SYSTEM disk with a boot sector; an IBM disk ----
    {
        std::vector<Byte> boot = {0x31, 0x00, 0x01, 0xC3, 0x00, 0x01};
        const auto img = buildDisk(true, 0x41, files, true, boot);
        const DskImage d = DskImage::parse(img);
        CHECK(d.extended());
        CHECK_EQ(d.format(), std::string("system"));
        CHECK_EQ(d.files().size(), std::size_t{4});
        const auto bs = d.bootSector();
        CHECK(bs.size() == 512 && bs[0] == 0x31 && bs[3] == 0xC3);
        CHECK(std::equal(big.begin(), big.end(), d.readFile("BIG.DAT").begin()));
        const DskImage ibm = DskImage::parse(buildDisk(false, 0x01, files, false));
        CHECK_EQ(ibm.format(), std::string("ibm"));
        CHECK(std::equal(game.begin(), game.end(), ibm.readFile("GAME.BIN").begin() + 128));
    }

    // ---- damaged or foreign images: an error, never a crash ----
    {
        bool threw = false;
        try {
            (void)DskImage::parse(std::vector<Byte>(300, 0));
        } catch (const DskError&) {
            threw = true;
        }
        CHECK(threw);
        auto img = buildDisk(false, 0xC1, files);
        img.resize(256 + 3 * 4864 + 1000);  // cut inside track 3
        threw = false;
        try {
            (void)DskImage::parse(img);
        } catch (const DskError&) {
            threw = true;
        }
        CHECK(threw);
        auto cut = buildDisk(false, 0xC1, files);
        cut.resize(256 + 20 * 4864);  // cut after track 19: readable, files that lie further out are not
        const DskImage d = DskImage::parse(cut);
        CHECK_EQ(d.format(), std::string("data"));
        auto ext = buildDisk(true, 0xC1, files);
        ext[0x34 + 39] = 0;  // the last track unformatted
        ext.resize(ext.size() - 4864);
        CHECK(DskImage::parse(ext).track(39) == nullptr);
        // a disk whose track 0 has other sector IDs (copy protection): no format, but sectors are readable
        auto odd = buildDisk(false, 0xC1, files);
        for (unsigned i = 0; i < 9; ++i) odd[256 + 0x18 + 8 * i + 2] = static_cast<Byte>(0x91 + i);
        const DskImage o = DskImage::parse(odd);
        CHECK_EQ(o.format(), std::string(""));
        CHECK(o.files().empty());
        CHECK(o.sector(0, 0x91) != nullptr);
        // random damage
        std::mt19937 rng(7);
        for (int round = 0; round < 1500; ++round) {
            auto bad = buildDisk(round % 2 == 0, round % 3 == 0 ? 0x41 : 0xC1, files);
            for (int k = 0; k < 1 + static_cast<int>(rng() % 20); ++k) bad[rng() % std::min<std::size_t>(bad.size(), 20000)] = static_cast<Byte>(rng());
            if (rng() % 4 == 0) bad.resize(rng() % bad.size());
            try {
                const DskImage x = DskImage::parse(bad);
                for (const auto& f : x.files()) (void)x.readFile(f.name, f.user);
                (void)x.format();
            } catch (const DskError&) {
            }
            ++g_checks;
        }
    }

    // ---- the session: dsk_ls and load_file with a name; a file with an AMSDOS header ----
    {
        const auto dir = std::filesystem::temp_directory_path();
        const auto diskPath = dir / "z80tool_test_disk.dsk";
        const auto binPath = dir / "z80tool_test_amsdos.bin";
        {
            const auto img = buildDisk(false, 0xC1, files);
            std::ofstream(diskPath, std::ios::binary).write(reinterpret_cast<const char*>(img.data()), static_cast<std::streamsize>(img.size()));
            std::vector<Byte> prog = makeAmsdosHeader("PROG.BIN", 0x8000, 0x8002, 4);
            for (Byte b : {Byte{0x00}, Byte{0x00}, Byte{0x3E}, Byte{0x07}}) prog.push_back(b);
            std::ofstream(binPath, std::ios::binary).write(reinterpret_cast<const char*>(prog.data()), static_cast<std::streamsize>(prog.size()));
        }
        SessionOptions options;
        options.allowFiles = true;
        Session s(options);
        const Json ls = ask(s, "{\"cmd\":\"dsk_ls\",\"file\":" + quote(diskPath.string()) + "}");
        CHECK(ok(ls));
        CHECK_EQ(ls.find("files")->items().size(), std::size_t{4});
        CHECK_EQ(ls.find("format")->asString(), std::string("data"));
        CHECK_EQ(ls.find("files")->items()[0].find("entry")->asString(), std::string("0x1234"));
        CHECK(!ok(ask(s, "{\"cmd\":\"load_file\",\"file\":" + quote(diskPath.string()) + "}")));  // which file?
        const Json loaded = ask(s, "{\"cmd\":\"load_file\",\"file\":" + quote(diskPath.string()) + ",\"name\":\"GAME.BIN\"}");
        CHECK(ok(loaded));
        CHECK_EQ(loaded.find("loaded")->asInt(), 5000);
        CHECK_EQ(loaded.find("amsdos")->find("load")->asString(), std::string("0x1000"));
        CHECK_EQ(s.cpu().regs().pc, Word{0x1234});
        CHECK_EQ(s.memory().read(0x1000), game[0]);
        CHECK_EQ(s.memory().read(0x1000 + 4999), game[4999]);
        CHECK(ok(ask(s, "{\"cmd\":\"load_file\",\"file\":" + quote(binPath.string()) + "}")));
        CHECK_EQ(s.memory().read(0x8002), Byte{0x3E});
        CHECK_EQ(s.cpu().regs().pc, Word{0x8002});
        // header:false loads the file as it is
        CHECK(ok(ask(s, "{\"cmd\":\"load_file\",\"file\":" + quote(binPath.string()) + ",\"addr\":\"0x9000\",\"header\":false}")));
        CHECK_EQ(s.memory().read(0x9000 + 128 + 2), Byte{0x3E});
        std::filesystem::remove(diskPath);
        std::filesystem::remove(binPath);
    }

    // ---- writing: format, put, read back; the written image is read by the parser and by an independent builder's layout ----
    {
        DskImage d = DskImage::createFormatted("data");
        CHECK_EQ(d.format(), std::string("data"));
        CHECK(d.files().empty());
        CHECK_EQ(d.freeBytes(), std::size_t{178 * 1024});  // 180 blocks, 2 for the directory
        d.writeFile("game.bin", withHeader);
        d.writeFile("BIG.DAT", big, 0, false, true);
        d.writeFile("HIDDEN.SYS", pattern(130, 4), 0, true);
        d.writeFile("USER1.TXT", pattern(256, 5), 1);
        const auto bytes = d.serialize();
        CHECK_EQ(bytes.size(), std::size_t{256 + 40 * 4864});  // standard layout: 40 tracks of 256 + 9 x 512 bytes
        CHECK(std::string(bytes.begin(), bytes.begin() + 8) == "MV - CPC");
        const DskImage r = DskImage::parse(bytes);
        CHECK_EQ(r.files().size(), std::size_t{4});
        const auto g = r.readFile("GAME.BIN");
        CHECK(std::equal(withHeader.begin(), withHeader.end(), g.begin()));
        CHECK_EQ(g.size(), (withHeader.size() + 127) / 128 * 128);
        CHECK_EQ(g.back(), Byte{0x1A});  // the last record is padded like CP/M does
        const auto bg = r.readFile("BIG.DAT");
        CHECK(std::equal(big.begin(), big.end(), bg.begin()));  // three extents
        bool sawAttrs = false;
        for (const auto& f : r.files()) sawAttrs = sawAttrs || (f.name == "BIG.DAT" && f.readOnly) || (f.name == "HIDDEN.SYS" && f.system);
        CHECK(sawAttrs);
        CHECK_EQ(r.readFile("USER1.TXT", 1u).size(), std::size_t{256});
        // the same layout as the hand-built disk: the parsed directory of both gives the same files
        const DskImage hand = DskImage::parse(buildDisk(false, 0xC1, files, false));
        CHECK(std::equal(big.begin(), big.end(), hand.readFile("BIG.DAT").begin()));
        // replace, delete, full disk, bad names
        DskImage e = DskImage::parse(bytes);
        const std::size_t before = e.freeBytes();
        e.writeFile("GAME.BIN", pattern(100, 9));
        CHECK_EQ(e.readFile("GAME.BIN").size(), std::size_t{128});
        CHECK_EQ(e.files().size(), std::size_t{4});
        CHECK(e.freeBytes() > before);  // 6 KiB freed, 1 used
        CHECK(e.deleteFile("BIG.DAT"));
        CHECK(!e.deleteFile("BIG.DAT"));
        CHECK_EQ(e.files().size(), std::size_t{3});
        bool threw = false;
        try {
            e.writeFile("HUGE.BIN", std::vector<Byte>(200 * 1024, 1));
        } catch (const DskError&) {
            threw = true;
        }
        CHECK(threw);
        for (const char* bad : {"", "TOOLONGNAME.BIN", "A.LONG", "A*B.BIN", ".BIN"}) {
            threw = false;
            try {
                e.writeFile(bad, std::vector<Byte>(10, 1));
            } catch (const DskError&) {
                threw = true;
            }
            CHECK(threw);
        }
        // 64 directory entries at most
        DskImage many = DskImage::createFormatted("data");
        threw = false;
        try {
            for (int i = 0; i < 70; ++i) many.writeFile("F" + std::to_string(i) + ".X", std::vector<Byte>(10, 1));
        } catch (const DskError&) {
            threw = true;
        }
        CHECK(threw);
        CHECK_EQ(many.files().size(), std::size_t{64});
        // SYSTEM format: two reserved tracks, the boot sector is untouched by files
        DskImage sys = DskImage::createFormatted("system");
        CHECK_EQ(sys.format(), std::string("system"));
        sys.writeFile("BIG.DAT", big);
        const DskImage sr = DskImage::parse(sys.serialize());
        CHECK(std::equal(big.begin(), big.end(), sr.readFile("BIG.DAT").begin()));
        CHECK_EQ(sr.bootSector()[0], Byte{0xE5});
        threw = false;
        try {
            (void)DskImage::createFormatted("ibm");
        } catch (const DskError&) {
            threw = true;
        }
        CHECK(threw);
    }

    return finish("dsk");
}
