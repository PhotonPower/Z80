#include "z80/png.hpp"

#include <array>
#include <stdexcept>
#include <string>

namespace z80 {
namespace {

std::uint32_t crc32(const Byte* data, std::size_t size, std::uint32_t crc = 0xFFFFFFFFu) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

void put32(std::vector<Byte>& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<Byte>(v >> shift));
}

void chunk(std::vector<Byte>& out, const char (&type)[5], const std::vector<Byte>& data) {
    put32(out, static_cast<std::uint32_t>(data.size()));
    std::vector<Byte> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put32(out, crc32(body.data(), body.size()) ^ 0xFFFFFFFFu);
}

}  // namespace

std::vector<Byte> encodePng(const IndexedImage& image) {
    if (image.width <= 0 || image.height <= 0 || image.palette.empty() || image.palette.size() > 256 ||
        image.pixels.size() != static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height)) {
        throw std::invalid_argument("invalid image");
    }
    // raw scanlines: filter byte 0 + indices
    std::vector<Byte> raw;
    raw.reserve(image.pixels.size() + static_cast<std::size_t>(image.height));
    for (int y = 0; y < image.height; ++y) {
        raw.push_back(0);
        const auto* row = image.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width);
        raw.insert(raw.end(), row, row + image.width);
    }
    // zlib stream with stored (uncompressed) blocks
    std::vector<Byte> z{0x78, 0x01};
    std::uint32_t a = 1, b = 0;
    for (Byte v : raw) {
        a = (a + v) % 65521u;
        b = (b + a) % 65521u;
    }
    for (std::size_t pos = 0; pos < raw.size();) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back(static_cast<Byte>(n));
        z.push_back(static_cast<Byte>(n >> 8));
        z.push_back(static_cast<Byte>(~n));
        z.push_back(static_cast<Byte>((~n) >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
    }
    put32(z, (b << 16) | a);

    std::vector<Byte> out{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<Byte> ihdr;
    put32(ihdr, static_cast<std::uint32_t>(image.width));
    put32(ihdr, static_cast<std::uint32_t>(image.height));
    ihdr.insert(ihdr.end(), {8, 3, 0, 0, 0});  // 8 bit, palette, deflate, no filter, no interlace
    chunk(out, "IHDR", ihdr);
    std::vector<Byte> plte;
    for (std::uint32_t c : image.palette) {
        plte.push_back(static_cast<Byte>(c >> 16));
        plte.push_back(static_cast<Byte>(c >> 8));
        plte.push_back(static_cast<Byte>(c));
    }
    chunk(out, "PLTE", plte);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return out;
}

std::string base64Encode(const std::vector<Byte>& data) {
    static constexpr char kDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16 |
                                (i + 1 < data.size() ? static_cast<std::uint32_t>(data[i + 1]) << 8 : 0u) |
                                (i + 2 < data.size() ? static_cast<std::uint32_t>(data[i + 2]) : 0u);
        out += kDigits[(v >> 18) & 63];
        out += kDigits[(v >> 12) & 63];
        out += i + 1 < data.size() ? kDigits[(v >> 6) & 63] : '=';
        out += i + 2 < data.size() ? kDigits[v & 63] : '=';
    }
    return out;
}

}  // namespace z80
