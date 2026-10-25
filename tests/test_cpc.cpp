#include <algorithm>
#include <set>

#include "test_util.hpp"
#include "z80/cpc.hpp"
#include "z80/image_io.hpp"
#include "z80/png.hpp"

using namespace z80;
using namespace z80::cpc;

namespace {

/// CRC-32 over bytes (independent of the encoder's implementation).
std::uint32_t crc(const std::vector<Byte>& data) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (Byte b : data) {
        c ^= b;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return ~c;
}

Byte pixelAt(const IndexedImage& img, int x, int y) { return img.pixels[static_cast<std::size_t>(y * img.width + x)]; }

}  // namespace

int main() {
    // ---- firmware table ----
    {
        CHECK_EQ(kFirmwareSymbols.size(), std::size_t{231});
        CHECK_EQ(*firmwareName(0xBB5A), std::string("TXT_OUTPUT"));
        CHECK_EQ(*firmwareAddress("SCR_SET_MODE"), Word{0xBC0E});
        CHECK_EQ(*firmwareAddress("KL_CHOKE_OFF"), Word{0xBCC8});
        CHECK_EQ(*firmwareAddress("SCR_FLOOD_BOX"), Word{0xBC47});  // corrected: sources list it at BC17
        CHECK_EQ(*firmwareName(0xBC17), std::string("SCR_CHAR_LIMITS"));
        CHECK(!firmwareName(0x4000).has_value());
        CHECK(!firmwareAddress("NO_SUCH_ROUTINE").has_value());
        std::set<Word> addresses;
        std::set<std::string> names;
        for (const auto& s : kFirmwareSymbols) {
            CHECK(addresses.insert(s.address).second);  // no two names share an address
            CHECK(names.insert(s.name).second);
            CHECK(s.address >= 0xB900 && s.address <= 0xBDFF);
        }
        // the main jump block is dense: every third address from BB00 to BD37 has a name
        for (unsigned a = 0xBB00; a <= 0xBD37; a += 3) CHECK(addresses.count(static_cast<Word>(a)) == 1);
    }

    // ---- colours ----
    {
        // firmware inks map to hardware colours and back to the same RGB value
        CHECK_EQ(kInkToHardware[0], 20);   // black
        CHECK_EQ(kInkToHardware[26], 11);  // bright white
        CHECK_EQ(kHardwareColourRgb[20], 0x000000u);
        CHECK_EQ(kHardwareColourRgb[11], 0xFFFFFFu);
        CHECK_EQ(kHardwareColourRgb[0], 0x808080u);
        for (std::uint8_t hw : kInkToHardware) CHECK(hw < 32);
        CHECK_EQ(std::string(kHardwareColourNames[24]), std::string("magenta"));
    }

    // ---- ports ----
    {
        CHECK_EQ(*portExpression(0x7F8D), std::string("GATE_ARRAY+&8D"));
        CHECK_EQ(*portExpression(0x7F00), std::string("GATE_ARRAY"));
        CHECK_EQ(*portExpression(0xBC0C), std::string("CRTC_SELECT+&0C"));
        CHECK_EQ(*portExpression(0xF600), std::string("PPI_C"));
        CHECK_EQ(*portExpression(0xFB7F), std::string("FDC_DATA"));
        CHECK_EQ(*portExpression(0xFA7E), std::string("FDC_MOTOR"));
        CHECK(!portExpression(0x1234).has_value());
        CHECK(!portExpression(0xFA00).has_value());  // disc ports need their exact address
        CHECK_EQ(*portHighExpression(0x7F), std::string("HIGH(GATE_ARRAY)"));
        CHECK(!portHighExpression(0x12).has_value());

        CHECK_EQ(describePortWrite(0x7F8D, 0x8D), std::string("Gate Array: screen mode 1, lower ROM off, upper ROM off"));
        CHECK_EQ(describePortWrite(0x7F00, 0x8A), std::string("Gate Array: screen mode 2, lower ROM on, upper ROM off"));
        CHECK_EQ(describePortWrite(0x7F00, 0x03), std::string("Gate Array: select pen 3"));
        CHECK_EQ(describePortWrite(0x7F00, 0x10), std::string("Gate Array: select border"));
        CHECK(describePortWrite(0x7F00, 0x54).find("hardware colour 20 (black)") != std::string::npos);
        CHECK(describePortWrite(0xBC00, 12).find("display start address (high)") != std::string::npos);
        CHECK(describePortWrite(0xF600, 0x40).find("PPI port C") != std::string::npos);
    }

    // ---- video state follows port writes ----
    {
        VideoState v;
        CHECK_EQ(v.mode, 1);
        CHECK_EQ(v.screenBase(), Word{0xC000});
        CHECK(!v.writePort(0x7F00, 0x81));  // mode 1 -> mode 1 with ROMs on: nothing visible changed
        CHECK_EQ(v.mode, 1);
        CHECK(v.writePort(0x7F00, 0x8A));   // mode 2, upper ROM off (bit 3 set), lower ROM still on
        CHECK_EQ(v.mode, 2);
        CHECK(!v.upperRomOn);
        CHECK(v.lowerRomOn);
        v.writePort(0x7F00, 0x05);          // select pen 5
        v.writePort(0x7F00, 0x4B);          // pen 5 = hw colour 0x0B (bright white)
        CHECK_EQ(v.inks[5], 0x0B);
        v.writePort(0x7F00, 0x10);          // select border
        v.writePort(0x7F00, 0x54);          // black
        CHECK_EQ(v.inks[16], 0x14);
        v.writePort(0xBC00, 12);
        v.writePort(0xBD00, 0x10);          // screen page at &4000
        CHECK_EQ(v.screenBase(), Word{0x4000});
        v.writePort(0xDF00, 7);
        CHECK_EQ(v.upperRomNumber, 7);
    }

    // ---- rendering: pixel decoding per mode ----
    {
        Memory m;
        VideoState v;
        for (Byte& ink : v.inks) ink = 0;
        // pens 0..15 get distinct colours so that the pen number can be read back from the palette
        auto penOf = [&](const IndexedImage& img, int x, int y) { return pixelAt(img, x, y); };

        v.mode = 1;
        m.write(0xC000, 0x88);  // pixel 0: bit7 + bit3 -> pen 3
        m.write(0xC001, 0x80);  // pixel 0: pen 1
        m.write(0xC002, 0x08);  // pixel 0: pen 2
        m.write(0xC003, 0x41);  // pixel 1: bit 6 -> pen 1; pixel 3: bit 0 (high plane) -> pen 2
        IndexedImage img = renderScreen(m, v);
        CHECK_EQ(img.width, 640);
        CHECK_EQ(img.height, 400);
        CHECK_EQ(penOf(img, 0, 0), 3);
        CHECK_EQ(penOf(img, 1, 0), 3);      // each mode-1 pixel is two columns wide
        CHECK_EQ(penOf(img, 8, 0), 1);      // byte 1 starts at column 8
        CHECK_EQ(penOf(img, 16, 0), 2);
        CHECK_EQ(penOf(img, 0, 1), 3);      // every scan line is doubled
        CHECK_EQ(penOf(img, 2, 0), 0);      // pixel 1 of byte 0 is pen 0
        CHECK_EQ(penOf(img, 26, 0), 1);     // byte 3 = 0x41: pixel 1 (bit 6) -> pen 1
        // pixel 3 of 0x41: low plane bit 4 (clear), high plane bit 0 (set) -> pen 2
        CHECK_EQ(penOf(img, 30, 0), 2);

        v.mode = 2;
        m.write(0xC000, 0xA5);
        img = renderScreen(m, v);
        CHECK_EQ(img.width, 640);
        CHECK_EQ(penOf(img, 0, 0), 1);
        CHECK_EQ(penOf(img, 1, 0), 0);
        CHECK_EQ(penOf(img, 2, 0), 1);
        CHECK_EQ(penOf(img, 7, 0), 1);

        v.mode = 0;
        m.write(0xC000, 0x80);  // pixel 0: pen bit0 = D7 -> pen 1
        m.write(0xC001, 0x08);  // pixel 0: pen bit1 = D3 -> pen 2
        m.write(0xC002, 0x20);  // pixel 0: pen bit2 = D5 -> pen 4
        m.write(0xC003, 0x02);  // pixel 0: pen bit3 = D1 -> pen 8
        m.write(0xC004, 0x40);  // pixel 1: pen bit0 = D6 -> pen 1
        img = renderScreen(m, v);
        CHECK_EQ(img.width, 640);
        CHECK_EQ(penOf(img, 0, 0), 1);
        CHECK_EQ(penOf(img, 8, 0), 2);
        CHECK_EQ(penOf(img, 16, 0), 4);
        CHECK_EQ(penOf(img, 24, 0), 8);
        CHECK_EQ(penOf(img, 36, 0), 1);  // byte 4, pixel 1 starts 4 columns into the byte's 8
    }

    // ---- rendering: memory layout, palette, scrolling, border ----
    {
        Memory m;
        VideoState v;
        v.mode = 2;
        // scan line y of character row r lives at base + (y%8)*0x800 + r*80
        m.write(0xC000 + 3 * 0x800 + 5 * 80 + 7, 0xFF);  // character row 5, raster 3, byte 7 -> scan line 43
        IndexedImage img = renderScreen(m, v);
        CHECK_EQ(pixelAt(img, 7 * 8, 43 * 2), 1);
        CHECK_EQ(pixelAt(img, 7 * 8 + 7, 43 * 2 + 1), 1);
        CHECK_EQ(pixelAt(img, 7 * 8 - 1, 43 * 2), 0);
        CHECK_EQ(pixelAt(img, 7 * 8, 42 * 2), 0);

        // palette: pen 1 shown in its hardware colour
        v.inks[1] = 0x0B;
        img = renderScreen(m, v);
        CHECK_EQ(img.palette[1], 0xFFFFFFu);
        CHECK_EQ(img.palette.size(), std::size_t{17});

        // hardware scrolling: start offset of 1 word moves the picture 2 bytes (16 pixels) left
        v.crtc[13] = 1;
        img = renderScreen(m, v);
        CHECK_EQ(pixelAt(img, (7 - 2) * 8, 43 * 2), 1);

        // other screen page
        Memory m2;
        VideoState v2;
        v2.mode = 2;
        v2.crtc[12] = 0x10;  // page at &4000
        m2.write(0x4000, 0x80);
        CHECK_EQ(pixelAt(renderScreen(m2, v2), 0, 0), 1);
        m2.write(0xC000, 0x80);
        v2.crtc[12] = 0x30;
        CHECK_EQ(pixelAt(renderScreen(m2, v2), 0, 0), 1);

        // border margin
        RenderOptions withBorder;
        withBorder.border = true;
        img = renderScreen(m, VideoState{}, withBorder);
        CHECK_EQ(img.width, 640 + 64);
        CHECK_EQ(img.height, 400 + 64);
        CHECK_EQ(pixelAt(img, 0, 0), 16);  // border pen
        CHECK_EQ(pixelAt(img, 32, 32), 0);

        // overscan: more characters per line widen the image
        VideoState wide;
        wide.mode = 2;
        wide.crtc[1] = 48;
        CHECK_EQ(renderScreen(m, wide).width, 768);
    }

    // ---- image diff ----
    {
        Memory m;
        VideoState v;
        v.mode = 2;
        v.inks[1] = 0x0B;
        const IndexedImage a = renderScreen(m, v);
        CHECK_EQ(diffImages(a, a).changedPixels, std::uint64_t{0});
        CHECK(diffImages(a, a).x1 < diffImages(a, a).x0);
        m.write(0xC000 + 80 * 10, 0xFF);  // character row 10 -> scan line 80
        const IndexedImage b = renderScreen(m, v);
        const ImageDiff d = diffImages(a, b);
        CHECK_EQ(d.changedPixels, std::uint64_t{8 * 2});
        CHECK_EQ(d.x0, 0);
        CHECK_EQ(d.x1, 7);
        CHECK_EQ(d.y0, 160);
        CHECK_EQ(d.y1, 161);
        VideoState other;
        other.mode = 1;
        other.crtc[1] = 30;
        CHECK(!diffImages(a, renderScreen(m, other)).sameSize);
        // identical colours under different pen numbers are not a visible change
        VideoState same = v;
        same.inks[2] = same.inks[1];
        Memory m3, m4;
        m3.write(0xC000, 0x80);
        m4.write(0xC000, 0x80);
        CHECK_EQ(diffImages(renderScreen(m3, same), renderScreen(m4, same)).changedPixels, std::uint64_t{0});
    }

    // ---- crop / zoom and screen addresses ----
    {
        IndexedImage img;
        img.width = 4;
        img.height = 3;
        img.palette = {0, 0xFFFFFF};
        img.pixels = {0, 0, 0, 0,
                      0, 1, 1, 0,
                      0, 0, 1, 0};
        const IndexedImage c = cropAndZoom(img, 1, 1, 2, 2, 3);
        CHECK_EQ(c.width, 6);
        CHECK_EQ(c.height, 6);
        CHECK_EQ(pixelAt(c, 0, 0), 1);
        CHECK_EQ(pixelAt(c, 5, 2), 1);
        CHECK_EQ(pixelAt(c, 0, 3), 0);
        CHECK_EQ(pixelAt(c, 3, 5), 1);
        CHECK(c.palette == img.palette);
        // out-of-range boxes are clamped instead of failing
        const IndexedImage d = cropAndZoom(img, -5, -5, 100, 100, 1);
        CHECK(d == img);

        CHECK_EQ(describeScreenAddress(0xC000), std::string("char row 0, byte column 0, scan line 0"));
        CHECK_EQ(describeScreenAddress(0xC78A), std::string("char row 24, byte column 10, scan line 0"));
        CHECK_EQ(describeScreenAddress(0xC000 + 3 * 0x800 + 80 * 5 + 7), std::string("char row 5, byte column 7, scan line 3"));
        CHECK_EQ(describeScreenAddress(0xC7D0), std::string(""));   // unused tail of a 2 KiB block
        CHECK_EQ(describeScreenAddress(0x4000), std::string(""));
        CHECK_EQ(describeScreenAddress(0x4010, 0x4000), std::string("char row 0, byte column 16, scan line 0"));
    }

    // ---- PNG and Base64 ----
    {
        IndexedImage img;
        img.width = 3;
        img.height = 2;
        img.palette = {0x000000, 0xFF0000, 0x00FF00};
        img.pixels = {0, 1, 2, 2, 1, 0};
        const auto png = encodePng(img);
        CHECK(std::equal(png.begin(), png.begin() + 8, std::vector<Byte>{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}.begin()));
        // walk the chunks and verify every CRC independently
        std::size_t pos = 8;
        std::vector<std::string> types;
        while (pos < png.size()) {
            const std::uint32_t len = png[pos] << 24 | png[pos + 1] << 16 | png[pos + 2] << 8 | png[pos + 3];
            std::vector<Byte> body(png.begin() + static_cast<std::ptrdiff_t>(pos + 4), png.begin() + static_cast<std::ptrdiff_t>(pos + 8 + len));
            const std::uint32_t stored = png[pos + 8 + len] << 24 | png[pos + 9 + len] << 16 | png[pos + 10 + len] << 8 | png[pos + 11 + len];
            CHECK_EQ(crc(body), stored);
            types.emplace_back(body.begin(), body.begin() + 4);
            pos += 12 + len;
        }
        CHECK_EQ(pos, png.size());
        CHECK(types == std::vector<std::string>({"IHDR", "PLTE", "IDAT", "IEND"}));
        CHECK_EQ(png[16 + 3], Byte{3});  // width low byte
        CHECK_EQ(png[24], Byte{8});      // bit depth
        CHECK_EQ(png[25], Byte{3});      // colour type: palette

        // large image: the zlib stream must be split into several stored blocks
        IndexedImage big;
        big.width = 640;
        big.height = 400;
        big.palette = {0, 0xFFFFFF};
        big.pixels.assign(640 * 400, 1);
        CHECK(encodePng(big).size() > 256000);

        bool threw = false;
        try { IndexedImage bad; encodePng(bad); } catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);

        CHECK_EQ(base64Encode({}), std::string(""));
        CHECK_EQ(base64Encode({'M'}), std::string("TQ=="));
        CHECK_EQ(base64Encode({'M', 'a'}), std::string("TWE="));
        CHECK_EQ(base64Encode({'M', 'a', 'n'}), std::string("TWFu"));
        CHECK_EQ(base64Encode({0xFF, 0xFE, 0xFD, 0xFC}), std::string("//79/A=="));
    }

    // ---- .sna header ----
    {
        std::vector<Byte> sna(kSnaHeaderSize + 0x10000, 0);
        const char magic[] = "MV - SNA";
        std::copy(magic, magic + 8, sna.begin());
        sna[0x10] = 3;
        sna[0x11] = 0x44;  // F
        sna[0x12] = 0x55;  // A
        sna[0x13] = 0x11;  // C
        sna[0x14] = 0x22;  // B
        sna[0x17] = 0x33;  // L
        sna[0x18] = 0x66;  // H
        sna[0x1B] = 1;     // IFF1
        sna[0x1D] = 0x34;  // IX low
        sna[0x1E] = 0x12;
        sna[0x21] = 0x00;  // SP = BFFE
        sna[0x22] = 0xBF;
        sna[0x23] = 0x00;  // PC = 4000
        sna[0x24] = 0x40;
        sna[0x25] = 1;     // IM 1
        sna[0x2E] = 0x03;  // selected pen
        for (int i = 0; i < 16; ++i) sna[static_cast<std::size_t>(0x2F + i)] = static_cast<Byte>(0x40 | i);
        sna[0x3F] = 0x4B;  // border
        sna[0x40] = 0x8A;  // mode 2, lower ROM on, upper ROM off
        sna[0x43 + 1] = 48;   // CRTC R1
        sna[0x43 + 12] = 0x10;  // screen at &4000
        sna[0x6B] = 64;
        const auto s = parseSnaHeader(sna);
        CHECK(s.has_value());
        CHECK_EQ(s->version, 3);
        const Registers& r = s->cpu.regs;
        CHECK_EQ(r.af(), Word{0x5544});
        CHECK_EQ(r.bc(), Word{0x2211});
        CHECK_EQ(r.hl(), Word{0x6633});
        CHECK_EQ(r.ix, Word{0x1234});
        CHECK_EQ(r.sp, Word{0xBF00});
        CHECK_EQ(r.pc, Word{0x4000});
        CHECK_EQ(r.im, Byte{1});
        CHECK(r.iff1);
        CHECK_EQ(s->video.mode, 2);
        CHECK(s->video.lowerRomOn);
        CHECK(!s->video.upperRomOn);
        CHECK_EQ(s->video.selectedPen, 3);
        CHECK_EQ(s->video.inks[5], 5);   // upper bits of the stored value are masked
        CHECK_EQ(s->video.inks[16], 0x0B);
        CHECK_EQ(s->video.crtc[1], 48);
        CHECK_EQ(s->video.screenBase(), Word{0x4000});
        CHECK_EQ(s->memoryKiB, 64);
        CHECK(!parseSnaHeader(std::vector<Byte>(0x200, 0)).has_value());
    }

    return finish("cpc");
}
