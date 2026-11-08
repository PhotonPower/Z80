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

// ---- writing ---------------------------------------------------------------------------------

DskImage DskImage::createFormatted(const std::string& fmt, unsigned tracks) {
    Byte first = 0;
    if (fmt == "data") first = 0xC1;
    else if (fmt == "system") first = 0x41;
    else throw DskError("format must be data or system");
    if (tracks < 3 || tracks > 84) throw DskError("a disk has 3 to 84 tracks");
    DskImage d;
    d.tracks_ = tracks;
    d.sides_ = 1;
    d.creator_ = "z80tool";
    for (unsigned t = 0; t < tracks; ++t) {
        Track track;
        track.number = t;
        for (unsigned k = 0; k < 9; ++k) {
            Sector s;
            s.c = static_cast<Byte>(t);
            s.r = static_cast<Byte>(first + k);
            s.n = 2;
            s.data.assign(512, 0xE5);  // a freshly formatted sector; &E5 also means "unused directory entry"
            track.sectors.push_back(std::move(s));
        }
        d.trackList_.push_back(std::move(track));
    }
    return d;
}

std::vector<Byte> DskImage::serialize() const {
    auto trackBytes = [](const Track& t) {
        std::size_t n = 256;
        for (const Sector& s : t.sectors) n += s.data.size();
        return (n + 255) / 256 * 256;
    };
    bool uniform = true;
    std::size_t size0 = 0;
    for (unsigned t = 0; t < tracks_; ++t) {
        for (unsigned side = 0; side < sides_; ++side) {
            const Track* tr = track(t, side);
            const std::size_t n = tr ? trackBytes(*tr) : 0;
            if (t == 0 && side == 0) size0 = n;
            uniform = uniform && n == size0 && n != 0;
            if (tr) {
                for (const Sector& s : tr->sectors) uniform = uniform && s.data.size() == (std::size_t{128} << std::min<unsigned>(s.n, 6));
            }
        }
    }
    std::vector<Byte> out(256, 0);
    const char* magic = uniform ? "MV - CPCEMU Disk-File\r\nDisk-Info\r\n" : "EXTENDED CPC DSK File\r\nDisk-Info\r\n";
    std::copy(magic, magic + 34, out.begin());
    for (std::size_t i = 0; i < creator_.size() && i < 14; ++i) out[0x22 + i] = static_cast<Byte>(creator_[i]);
    out[0x30] = static_cast<Byte>(tracks_);
    out[0x31] = static_cast<Byte>(sides_);
    if (uniform) {
        out[0x32] = static_cast<Byte>(size0);
        out[0x33] = static_cast<Byte>(size0 >> 8);
    }
    for (unsigned t = 0; t < tracks_; ++t) {
        for (unsigned side = 0; side < sides_; ++side) {
            const Track* tr = track(t, side);
            if (!uniform) out[0x34 + t * sides_ + side] = static_cast<Byte>(tr ? trackBytes(*tr) / 256 : 0);
            if (!tr) continue;
            std::vector<Byte> block(trackBytes(*tr), 0);
            const char* info = "Track-Info\r\n";
            std::copy(info, info + 12, block.begin());
            block[0x10] = static_cast<Byte>(tr->number);
            block[0x11] = static_cast<Byte>(tr->side);
            block[0x14] = tr->sectors.empty() ? Byte{2} : tr->sectors[0].n;
            block[0x15] = static_cast<Byte>(tr->sectors.size());
            block[0x16] = 0x4E;  // gap 3
            block[0x17] = 0xE5;  // filler
            std::size_t data = 256;
            for (std::size_t k = 0; k < tr->sectors.size() && k < 29; ++k) {
                const Sector& s = tr->sectors[k];
                Byte* e = &block[0x18 + 8 * k];
                e[0] = s.c;
                e[1] = s.h;
                e[2] = s.r;
                e[3] = s.n;
                e[4] = s.st1;
                e[5] = s.st2;
                if (!uniform) {
                    e[6] = static_cast<Byte>(s.data.size());
                    e[7] = static_cast<Byte>(s.data.size() >> 8);
                }
                std::copy(s.data.begin(), s.data.end(), block.begin() + static_cast<std::ptrdiff_t>(data));
                data += s.data.size();
            }
            out.insert(out.end(), block.begin(), block.end());
        }
    }
    return out;
}

bool DskImage::writeSector(unsigned trackNumber, Byte id, std::span<const Byte> data) {
    Sector* s = sectorRef(trackNumber, id);
    if (!s) return false;
    for (std::size_t i = 0; i < s->data.size(); ++i) s->data[i] = i < data.size() ? data[i] : Byte{0};
    return true;
}

DskImage::Sector* DskImage::sectorRef(unsigned trackNumber, Byte id) {
    for (Track& t : trackList_) {
        if (t.number != trackNumber || t.side != 0) continue;
        for (Sector& s : t.sectors) {
            if (s.r == id) return &s;
        }
    }
    return nullptr;
}

unsigned DskImage::blockCount(const Geometry& g) const {
    return (tracks_ - g.reservedTracks) * g.sectorsPerTrack / 2;
}

void DskImage::writeBlock(const Geometry& g, unsigned number, std::span<const Byte> data) {
    for (unsigned k = 0; k < 2; ++k) {
        const unsigned logical = number * 2 + k;
        const unsigned t = g.reservedTracks + logical / g.sectorsPerTrack;
        Sector* s = sectorRef(t, static_cast<Byte>(g.firstId + logical % g.sectorsPerTrack));
        if (!s) throw DskError("block " + std::to_string(number) + " lies on a missing sector");
        s->data.assign(512, 0);
        for (std::size_t i = 0; i < 512 && k * 512 + i < data.size(); ++i) s->data[i] = data[k * 512 + i];
    }
}

std::vector<Byte> DskImage::directory(const Geometry& g) const {
    std::vector<Byte> dir = block(g, 0);
    const auto second = block(g, 1);
    dir.insert(dir.end(), second.begin(), second.end());
    return dir;
}

void DskImage::writeDirectory(const Geometry& g, const std::vector<Byte>& dir) {
    writeBlock(g, 0, std::span<const Byte>(dir.data(), 1024));
    writeBlock(g, 1, std::span<const Byte>(dir.data() + 1024, 1024));
}

namespace {
bool sameName(std::span<const Byte> entry, const std::string& base, const std::string& ext) {
    for (std::size_t i = 0; i < 8; ++i) {
        if (static_cast<char>(entry[1 + i] & 0x7F) != (i < base.size() ? base[i] : ' ')) return false;
    }
    for (std::size_t i = 0; i < 3; ++i) {
        if (static_cast<char>(entry[9 + i] & 0x7F) != (i < ext.size() ? ext[i] : ' ')) return false;
    }
    return true;
}
std::pair<std::string, std::string> splitName(const std::string& name) {
    std::string up = upper(name), base = up, ext;
    if (const auto dot = up.find('.'); dot != std::string::npos) {
        base = up.substr(0, dot);
        ext = up.substr(dot + 1);
    }
    if (base.empty() || base.size() > 8 || ext.size() > 3) throw DskError("an AMSDOS name has 1-8 characters and an extension of up to 3: '" + name + "'");
    for (char c : base + ext) {
        if (c <= ' ' || c >= 0x7F || std::string("<>.,;:=?*[]").find(c) != std::string::npos) throw DskError("invalid character in the name '" + name + "'");
    }
    return {base, ext};
}
}  // namespace

bool DskImage::deleteFile(const std::string& name, unsigned user) {
    const auto g = geometry();
    if (!g) throw DskError("no AMSDOS file system on this disk (format not recognised)");
    const auto [base, ext] = splitName(name);
    std::vector<Byte> dir = directory(*g);
    bool any = false;
    for (std::size_t e = 0; e + 32 <= dir.size(); e += 32) {
        if (dir[e] == user && sameName(std::span<const Byte>(dir.data() + e, 32), base, ext)) {
            dir[e] = 0xE5;
            any = true;
        }
    }
    if (any) writeDirectory(*g, dir);
    return any;
}

std::size_t DskImage::freeBytes() const {
    const auto g = geometry();
    if (!g) return 0;
    std::vector<bool> used(blockCount(*g), false);
    used[0] = used[1] = true;
    const std::vector<Byte> dir = directory(*g);
    for (std::size_t e = 0; e + 32 <= dir.size(); e += 32) {
        if (dir[e] > 15) continue;
        for (std::size_t k = 16; k < 32; ++k) {
            if (dir[e + k] != 0 && dir[e + k] < used.size()) used[dir[e + k]] = true;
        }
    }
    return static_cast<std::size_t>(std::count(used.begin(), used.end(), false)) * 1024;
}

void DskImage::writeFile(const std::string& name, std::span<const Byte> data, unsigned user, bool system, bool readOnly) {
    const auto g = geometry();
    if (!g) throw DskError("no AMSDOS file system on this disk (format not recognised)");
    if (user > 15) throw DskError("user numbers are 0-15");
    const auto [base, ext] = splitName(name);
    (void)deleteFile(name, user);
    std::vector<Byte> dir = directory(*g);
    const unsigned blocks = blockCount(*g);
    if (blocks > 256) throw DskError("disks with more than 256 blocks need 16-bit block numbers (not supported)");
    std::vector<bool> used(blocks, false);
    used[0] = used[1] = true;  // the directory
    for (std::size_t e = 0; e + 32 <= dir.size(); e += 32) {
        if (dir[e] > 15) continue;
        for (std::size_t k = 16; k < 32; ++k) {
            if (dir[e + k] != 0 && dir[e + k] < blocks) used[dir[e + k]] = true;
        }
    }
    // the file in 128-byte records, the last one padded with &1A (CP/M end of file)
    std::vector<Byte> padded(data.begin(), data.end());
    padded.resize((padded.size() + 127) / 128 * 128, 0x1A);
    const std::size_t records = padded.size() / 128;
    const std::size_t neededBlocks = (padded.size() + 1023) / 1024;
    const std::size_t neededEntries = std::max<std::size_t>(1, (records + 127) / 128);
    std::vector<unsigned> freeBlocks;
    for (unsigned b = 2; b < blocks && freeBlocks.size() < neededBlocks; ++b) {
        if (!used[b]) freeBlocks.push_back(b);
    }
    if (freeBlocks.size() < neededBlocks) throw DskError("the disk is full (" + std::to_string(neededBlocks) + " KiB needed)");
    std::vector<std::size_t> freeEntries;
    for (std::size_t e = 0; e + 32 <= dir.size() && freeEntries.size() < neededEntries; e += 32) {
        if (dir[e] > 15) freeEntries.push_back(e);
    }
    if (freeEntries.size() < neededEntries) throw DskError("the directory is full");
    for (std::size_t b = 0; b < neededBlocks; ++b) {
        const std::size_t from = b * 1024;
        writeBlock(*g, freeBlocks[b], std::span<const Byte>(padded.data() + from, std::min<std::size_t>(1024, padded.size() - from)));
    }
    for (std::size_t x = 0; x < neededEntries; ++x) {
        Byte* e = &dir[freeEntries[x]];
        std::fill(e, e + 32, Byte{0});
        e[0] = static_cast<Byte>(user);
        for (std::size_t i = 0; i < 8; ++i) e[1 + i] = static_cast<Byte>(i < base.size() ? base[i] : ' ');
        for (std::size_t i = 0; i < 3; ++i) e[9 + i] = static_cast<Byte>(i < ext.size() ? ext[i] : ' ');
        if (readOnly) e[9] |= 0x80;
        if (system) e[10] |= 0x80;
        e[12] = static_cast<Byte>(x & 0x1F);   // extent (16 KiB each)
        e[14] = static_cast<Byte>(x >> 5);
        e[15] = static_cast<Byte>(std::min<std::size_t>(128, records - x * 128));
        for (std::size_t k = 0; k < 16; ++k) {
            const std::size_t b = x * 16 + k;
            if (b < neededBlocks) e[16 + k] = static_cast<Byte>(freeBlocks[b]);
        }
    }
    writeDirectory(*g, dir);
}

std::vector<Byte> DskImage::bootSector() const {
    if (format() != "system") throw DskError("only a SYSTEM disk has a boot sector (this one is " + (format().empty() ? std::string("unknown") : format()) + ")");
    const Sector* s = sector(0, 0x41);
    if (!s) throw DskError("track 0 has no sector &41");
    return s->data;
}

}  // namespace z80
