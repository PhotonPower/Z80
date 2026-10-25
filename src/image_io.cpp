#include "z80/image_io.hpp"

#include <iterator>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "z80/format.hpp"

namespace z80 {

std::vector<Byte> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open '" + path.string() + "' for reading");
    }
    return std::vector<Byte>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeFile(const std::filesystem::path& path, std::span<const Byte> bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot open '" + path.string() + "' for writing");
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        throw std::runtime_error("error while writing '" + path.string() + "'");
    }
}

bool looksLikeSna(std::span<const Byte> file) {
    static constexpr char kMagic[] = "MV - SNA";
    return file.size() >= sizeof(kMagic) - 1 &&
           std::equal(kMagic, kMagic + sizeof(kMagic) - 1, file.begin(),
                      [](char m, Byte b) { return static_cast<Byte>(m) == b; });
}

Memory loadSna(std::span<const Byte> file) {
    if (file.size() <= kSnaHeaderSize) {
        throw std::runtime_error("snapshot file is too short");
    }
    Memory memory;
    memory.load(0, file.subspan(kSnaHeaderSize));
    return memory;
}

Memory loadRaw(std::span<const Byte> file, Word origin) {
    Memory memory;
    memory.load(origin, file);
    return memory;
}

void patchSna(std::vector<Byte>& file, const std::vector<Segment>& segments) {
    for (const auto& segment : segments) {
        const std::size_t offset = kSnaHeaderSize + segment.start;
        if (offset + segment.bytes.size() > file.size()) {
            throw std::runtime_error("assembled code at &" + hexDigits(segment.start, 4) +
                                     " lies beyond the end of the snapshot file");
        }
        std::copy(segment.bytes.begin(), segment.bytes.end(), file.begin() + static_cast<long>(offset));
    }
}

std::string toIntelHex(const std::vector<Segment>& segments) {
    std::ostringstream out;
    auto record = [&out](unsigned address, unsigned type, const std::vector<Byte>& data) {
        unsigned sum = static_cast<unsigned>(data.size()) + (address >> 8) + (address & 0xFF) + type;
        out << ':' << hexDigits(static_cast<unsigned>(data.size()), 2) << hexDigits(address, 4)
            << hexDigits(type, 2);
        for (Byte b : data) {
            out << hexDigits(b, 2);
            sum += b;
        }
        out << hexDigits((0x100 - (sum & 0xFF)) & 0xFF, 2) << "\n";
    };
    for (const auto& segment : segments) {
        for (std::size_t i = 0; i < segment.bytes.size(); i += 16) {
            const std::size_t n = std::min<std::size_t>(16, segment.bytes.size() - i);
            record(segment.start + static_cast<unsigned>(i), 0,
                   std::vector<Byte>(segment.bytes.begin() + static_cast<long>(i),
                                     segment.bytes.begin() + static_cast<long>(i + n)));
        }
    }
    record(0, 1, {});
    return out.str();
}

}  // namespace z80
