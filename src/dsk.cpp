#include "z80/dsk.hpp"

#include "z80/format.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace z80 {
namespace {

bool startsWith(std::span<const Byte> b, const char* text) {
    for (std::size_t i = 0; text[i]; ++i) {
        if (i >= b.size() || b[i] != static_cast<Byte>(text[i])) return false;
    }
    return true;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string trimmed(std::span<const Byte> b) {
    std::string s;
    for (Byte c : b) s += static_cast<char>(c & 0x7F);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    return s;
}

std::string fileName(std::span<const Byte> nameAndExt) {
    const std::string base = trimmed(nameAndExt.subspan(0, 8));
    const std::string ext = trimmed(nameAndExt.subspan(8, 3));
    return ext.empty() ? base : base + "." + ext;
}

}  // namespace

// ---- AMSDOS header -------------------------------------------------------------------------------

std::optional<AmsdosHeader> parseAmsdosHeader(std::span<const Byte> f) {
    if (f.size() < 128) return std::nullopt;
    unsigned sum = 0;
    for (std::size_t i = 0; i <= 66; ++i) sum += f[i];
    if ((sum & 0xFFFF) != static_cast<unsigned>(f[67] | (f[68] << 8))) return std::nullopt;
    if (sum == 0) return std::nullopt;  // 128 zero bytes have a "valid" checksum too
    for (std::size_t i = 1; i <= 11; ++i) {
        const Byte c = f[i] & 0x7F;
        if (c != 0 && (c < 0x20 || c >= 0x7F)) return std::nullopt;  // a name of printable characters
    }
    AmsdosHeader h;
    h.user = f[0];
    h.name = fileName(f.subspan(1, 11));
    h.type = f[18];
    h.load = static_cast<Word>(f[21] | (f[22] << 8));
    h.exec = static_cast<Word>(f[26] | (f[27] << 8));
    h.length = static_cast<std::uint32_t>(f[64] | (f[65] << 8) | (f[66] << 16));
    if (h.length == 0) h.length = static_cast<std::uint32_t>(f[24] | (f[25] << 8));
    return h;
}

std::vector<Byte> makeAmsdosHeader(const std::string& name, Word load, Word exec, std::uint32_t length, std::uint8_t type) {
    std::vector<Byte> h(128, 0);
    std::string base = upper(name), ext;
    if (const auto dot = base.find('.'); dot != std::string::npos) {
        ext = base.substr(dot + 1);
        base = base.substr(0, dot);
    }
    for (std::size_t i = 0; i < 8; ++i) h[1 + i] = static_cast<Byte>(i < base.size() ? base[i] : ' ');
    for (std::size_t i = 0; i < 3; ++i) h[9 + i] = static_cast<Byte>(i < ext.size() ? ext[i] : ' ');
    h[18] = type;
    h[21] = static_cast<Byte>(load);
    h[22] = static_cast<Byte>(load >> 8);
    h[24] = static_cast<Byte>(length);
    h[25] = static_cast<Byte>(length >> 8);
    h[26] = static_cast<Byte>(exec);
    h[27] = static_cast<Byte>(exec >> 8);
    h[64] = static_cast<Byte>(length);
    h[65] = static_cast<Byte>(length >> 8);
    h[66] = static_cast<Byte>(length >> 16);
    unsigned sum = 0;
    for (std::size_t i = 0; i <= 66; ++i) sum += h[i];
    h[67] = static_cast<Byte>(sum);
    h[68] = static_cast<Byte>(sum >> 8);
    return h;
}

// ---- the image ---------------------------------------------------------------------------------

bool DskImage::looksLikeDsk(std::span<const Byte> b) { return startsWith(b, "MV - CPC") || startsWith(b, "EXTENDED CPC DSK"); }

DskImage DskImage::parse(std::span<const Byte> b) {
    if (b.size() < 256 || !looksLikeDsk(b)) throw DskError("not a CPC disk image (no MV - CPCEMU or EXTENDED CPC DSK header)");
    DskImage d;
    d.extended_ = startsWith(b, "EXTENDED");
    d.creator_ = trimmed(b.subspan(0x22, 14));
    d.tracks_ = b[0x30];
    d.sides_ = b[0x31];
    if (d.sides_ < 1 || d.sides_ > 2) throw DskError("the header says " + std::to_string(d.sides_) + " sides");
    if (d.tracks_ == 0 || d.tracks_ > 204) throw DskError("the header says " + std::to_string(d.tracks_) + " tracks");
    const std::size_t standardSize = static_cast<std::size_t>(b[0x32] | (b[0x33] << 8));
    std::size_t offset = 256;
    for (unsigned t = 0; t < d.tracks_; ++t) {
        for (unsigned s = 0; s < d.sides_; ++s) {
            const std::size_t index = t * d.sides_ + s;
            const std::size_t size = d.extended_ ? (0x34 + index < 256 ? static_cast<std::size_t>(b[0x34 + index]) * 256 : 0) : standardSize;
            if (size == 0) continue;  // an unformatted track
            if (offset + size > b.size()) {
                if (offset >= b.size()) break;  // images are often cut after the last used track
                throw DskError("track " + std::to_string(t) + " is cut off (the file ends inside it)");
            }
            const auto tb = b.subspan(offset, size);
            if (!startsWith(tb, "Track-Info")) throw DskError("no Track-Info block for track " + std::to_string(t) + " side " + std::to_string(s));
            Track track;
            track.number = tb[0x10];
            track.side = tb[0x11];
            const unsigned trackN = tb[0x14];
            const unsigned count = std::min<unsigned>(tb[0x15], 29);  // the 256-byte block has room for 29 sector entries
            std::size_t data = 256;
            for (unsigned k = 0; k < count; ++k) {
                const auto info = tb.subspan(0x18 + 8 * k, 8);
                Sector sec;
                sec.c = info[0];
                sec.h = info[1];
                sec.r = info[2];
                sec.n = info[3];
                sec.st1 = info[4];
                sec.st2 = info[5];
                std::size_t length = d.extended_ ? static_cast<std::size_t>(info[6] | (info[7] << 8)) : (std::size_t{128} << std::min(trackN, 6u));
                if (data + length > size) length = data < size ? size - data : 0;  // damaged: take what is there
                sec.data.assign(tb.begin() + static_cast<std::ptrdiff_t>(data), tb.begin() + static_cast<std::ptrdiff_t>(data + length));
                data += length;
                track.sectors.push_back(std::move(sec));
            }
            d.trackList_.push_back(std::move(track));
            offset += size;
        }
    }
    return d;
}

const DskImage::Track* DskImage::track(unsigned number, unsigned side) const {
    for (const Track& t : trackList_) {
        if (t.number == number && t.side == side) return &t;
    }
    return nullptr;
}

const DskImage::Sector* DskImage::sector(unsigned trackNumber, Byte id, unsigned side) const {
    const Track* t = track(trackNumber, side);
    if (!t) return nullptr;
    for (const Sector& s : t->sectors) {
        if (s.r == id) return &s;
    }
    return nullptr;
}

std::optional<DskImage::Geometry> DskImage::geometry() const {
    const Track* t0 = track(0);
    if (!t0 || t0->sectors.empty()) return std::nullopt;
    Byte lowest = 0xFF;
    for (const Sector& s : t0->sectors) lowest = std::min(lowest, s.r);
    if (lowest == 0xC1) return Geometry{0xC1, 0, 9};
    if (lowest == 0x41) return Geometry{0x41, 2, 9};
    if (lowest == 0x01) return Geometry{0x01, 1, 8};
    return std::nullopt;
}

std::string DskImage::format() const {
    const auto g = geometry();
    if (!g) return "";
    return g->firstId == 0xC1 ? "data" : g->firstId == 0x41 ? "system" : "ibm";
}

std::vector<Byte> DskImage::block(const Geometry& g, unsigned number) const {
    std::vector<Byte> out;
    for (unsigned k = 0; k < 2; ++k) {  // a block is 1 KiB: two sectors of 512 bytes
        const unsigned logical = number * 2 + k;
        const unsigned t = g.reservedTracks + logical / g.sectorsPerTrack;
        const auto id = static_cast<Byte>(g.firstId + logical % g.sectorsPerTrack);
        const Sector* s = sector(t, id);
        if (!s) throw DskError("block " + std::to_string(number) + ": track " + std::to_string(t) + " has no sector &" + hexDigits(id, 2));
        std::vector<Byte> data = s->data;
        data.resize(512, 0);
        out.insert(out.end(), data.begin(), data.end());
    }
    return out;
}

std::vector<DskFile> DskImage::files() const {
    const auto g = geometry();
    if (!g) return {};
    std::vector<Byte> dir;
    for (unsigned b = 0; b < 2; ++b) {  // 64 entries of 32 bytes in the first two blocks
        const auto part = block(*g, b);
        dir.insert(dir.end(), part.begin(), part.end());
    }
    struct Extent {
        unsigned number;
        unsigned records;
        std::vector<unsigned> blocks;
    };
    std::map<std::pair<unsigned, std::string>, std::pair<DskFile, std::vector<Extent>>> byName;
    std::vector<std::pair<unsigned, std::string>> order;
    for (std::size_t e = 0; e + 32 <= dir.size(); e += 32) {
        const std::span<const Byte> entry(dir.data() + e, 32);
        if (entry[0] > 15) continue;  // &E5: deleted or never used
        const std::string name = fileName(entry.subspan(1, 11));
        if (name.empty()) continue;
        const auto key = std::make_pair(static_cast<unsigned>(entry[0]), name);
        auto [it, fresh] = byName.try_emplace(key);
        if (fresh) {
            order.push_back(key);
            it->second.first.name = name;
            it->second.first.user = entry[0];
            it->second.first.readOnly = (entry[9] & 0x80) != 0;
            it->second.first.system = (entry[10] & 0x80) != 0;
        }
        Extent x;
        x.number = (entry[12] & 0x1F) + 32u * (entry[14] & 0x3F);
        x.records = std::min<unsigned>(entry[15], 128);
        for (std::size_t k = 16; k < 32; ++k) {
            if (entry[k] != 0) x.blocks.push_back(entry[k]);
        }
        it->second.second.push_back(std::move(x));
    }
    std::vector<DskFile> out;
    for (const auto& key : order) {
        auto& [file, extents] = byName[key];
        std::sort(extents.begin(), extents.end(), [](const Extent& a, const Extent& b) { return a.number < b.number; });
        for (const Extent& x : extents) {
            const unsigned needed = (x.records * 128 + 1023) / 1024;  // blocks that hold the extent's records
            for (unsigned k = 0; k < needed && k < x.blocks.size(); ++k) file.blocks.push_back(x.blocks[k]);
            file.size += x.records * 128;
        }
        out.push_back(file);
    }
    return out;
}

std::vector<Byte> DskImage::readFile(const std::string& name, std::optional<unsigned> user) const {
    const auto g = geometry();
    if (!g) throw DskError("no AMSDOS file system on this disk (format not recognised)");
    const std::string wanted = upper(name);
    for (const DskFile& f : files()) {
        if (upper(f.name) != wanted || (user && *user != f.user)) continue;
        std::vector<Byte> out;
        for (unsigned b : f.blocks) {
            const auto part = block(*g, b);
            out.insert(out.end(), part.begin(), part.end());
        }
        out.resize(std::min<std::size_t>(out.size(), f.size));
        return out;
    }
    throw DskError("no file '" + name + "' on the disk");
}

std::vector<Byte> DskImage::bootSector() const {
    if (format() != "system") throw DskError("only a SYSTEM disk has a boot sector (this one is " + (format().empty() ? std::string("unknown") : format()) + ")");
    const Sector* s = sector(0, 0x41);
    if (!s) throw DskError("track 0 has no sector &41");
    return s->data;
}

}  // namespace z80
