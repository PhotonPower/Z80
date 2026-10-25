#include "z80/cpc.hpp"

#include <algorithm>
#include <map>

#include "z80/format.hpp"
#include "z80/image_io.hpp"

namespace z80::cpc {
namespace {

std::string h2(unsigned v) { return hexDigits(v & 0xFF, 2); }

constexpr std::array<const char*, 18> kCrtcRegisterNames = {
    "horizontal total", "horizontal displayed", "horizontal sync position", "sync widths", "vertical total",
    "vertical total adjust", "vertical displayed", "vertical sync position", "interlace and skew",
    "maximum raster address", "cursor start raster", "cursor end raster", "display start address (high)",
    "display start address (low)", "cursor address (high)", "cursor address (low)", "light pen (high)",
    "light pen (low)"};

constexpr std::array<PortSymbol, 13> kPorts = {{
    {"GATE_ARRAY", 0x7F00, "Gate Array: pens, inks, screen mode, ROM/RAM configuration"},
    {"CRTC_SELECT", 0xBC00, "CRTC 6845: select register"},
    {"CRTC_DATA", 0xBD00, "CRTC 6845: write register data"},
    {"CRTC_STATUS", 0xBE00, "CRTC 6845: read status"},
    {"CRTC_READ", 0xBF00, "CRTC 6845: read register data"},
    {"ROM_SELECT", 0xDF00, "upper ROM select"},
    {"PRINTER_PORT", 0xEF00, "printer port"},
    {"PPI_A", 0xF400, "8255 PPI port A (PSG data / keyboard)"},
    {"PPI_B", 0xF500, "8255 PPI port B (vsync, cassette, printer busy)"},
    {"PPI_C", 0xF600, "8255 PPI port C (PSG control, keyboard row, cassette motor)"},
    {"PPI_CONTROL", 0xF700, "8255 PPI control register"},
    {"FDC_MOTOR", 0xFA7E, "disc drive motor"},
    {"FDC_STATUS", 0xFB7E, "disc controller: main status register"},
}};
constexpr PortSymbol kFdcData = {"FDC_DATA", 0xFB7F, "disc controller: data register"};

}  // namespace

// ---- firmware -----------------------------------------------------------------------------------

std::optional<std::string> firmwareName(Word address) {
    static const auto index = [] {
        std::map<Word, std::string> m;
        for (const auto& s : kFirmwareSymbols) m.emplace(s.address, s.name);
        return m;
    }();
    if (auto it = index.find(address); it != index.end()) return it->second;
    return std::nullopt;
}

std::optional<Word> firmwareAddress(const std::string& name) {
    for (const auto& s : kFirmwareSymbols) {
        if (name == s.name) return s.address;
    }
    return std::nullopt;
}

// ---- ports --------------------------------------------------------------------------------------

std::span<const PortSymbol> portSymbols() { return kPorts; }

namespace {
/// Port whose high byte is `high`, ignoring the low byte (except for the disc ports).
const PortSymbol* portByHigh(Byte high) {
    for (const auto& p : kPorts) {
        if ((p.address >> 8) == high) return &p;
    }
    return nullptr;
}
}  // namespace

std::optional<std::string> portExpression(Word port) {
    const auto high = static_cast<Byte>(port >> 8);
    const auto low = static_cast<Byte>(port & 0xFF);
    if (port == kFdcData.address) return std::string(kFdcData.name);
    const PortSymbol* p = portByHigh(high);
    if (!p) return std::nullopt;
    if (high == 0xFA || high == 0xFB) {  // disc ports have a fixed low byte
        if (port == p->address) return std::string(p->name);
        return std::nullopt;
    }
    if (low == 0) return std::string(p->name);
    return std::string(p->name) + "+&" + h2(low);
}

std::optional<std::string> portHighExpression(Byte high) {
    if (const PortSymbol* p = portByHigh(high)) return "HIGH(" + std::string(p->name) + ")";
    return std::nullopt;
}

std::string describePortWrite(Word port, Byte value) {
    const unsigned high = port >> 8;
    if ((high & 0xC0) == 0x40) {  // Gate Array
        switch (value >> 6) {
            case 0:
                return value & 0x10 ? "Gate Array: select border" : "Gate Array: select pen " + std::to_string(value & 0x0F);
            case 1:
                return "Gate Array: set colour of the selected pen to hardware colour " + std::to_string(value & 0x1F) + " (" +
                       kHardwareColourNames[value & 0x1F] + ")";
            case 2: {
                std::string s = "Gate Array: screen mode " + std::to_string(value & 3) + ", lower ROM " +
                                ((value & 4) ? "off" : "on") + ", upper ROM " + ((value & 8) ? "off" : "on");
                if (value & 0x10) s += ", reset interrupt counter";
                return s;
            }
            default:
                return "Gate Array: RAM banking, configuration &" + h2(value);
        }
    }
    if ((high & 0x40) == 0) {  // CRTC
        switch (high & 3) {
            case 0:
                if (value < kCrtcRegisterNames.size()) return "CRTC: select register " + std::to_string(value) + " (" + kCrtcRegisterNames[value] + ")";
                return "CRTC: select register " + std::to_string(value);
            case 1: return "CRTC: write &" + h2(value) + " to the selected register";
            default: return "CRTC: access";
        }
    }
    if ((high & 0x20) == 0) return "select upper ROM " + std::to_string(value);
    if ((high & 0x10) == 0) return "printer port: data &" + h2(value);
    if ((high & 0x08) == 0) {
        static constexpr const char* kNames[4] = {"port A (PSG data / keyboard)", "port B", "port C (PSG control / keyboard row / cassette motor)", "control"};
        return std::string("PPI ") + kNames[high & 3] + ": &" + h2(value);
    }
    return "";
}

std::string describePortRead(Word port) {
    const unsigned high = port >> 8;
    if ((high & 0x40) == 0 && (high & 3) >= 2) return "CRTC: read";
    if ((high & 0x08) == 0) {
        static constexpr const char* kNames[4] = {"port A (keyboard / PSG)", "port B (vsync flag in bit 0)", "port C", "control"};
        return std::string("PPI ") + kNames[high & 3];
    }
    return "";
}

// ---- video state --------------------------------------------------------------------------------

VideoState::VideoState() {
    // firmware default inks for the 16 pens (mode 1/2 use the first 4/2), border = blue
    static constexpr std::array<Byte, 17> kDefaultInks = {1, 24, 20, 6, 26, 0, 2, 8, 10, 12, 14, 16, 18, 22, 1, 24, 1};
    for (std::size_t i = 0; i < inks.size(); ++i) inks[i] = kInkToHardware[kDefaultInks[i]];
    crtc.fill(0);
    crtc[0] = 63;
    crtc[1] = 40;
    crtc[2] = 46;
    crtc[6] = 25;
    crtc[9] = 7;
    crtc[12] = 0x30;  // screen at &C000
}

bool VideoState::writePort(Word port, Byte value) {
    const unsigned high = port >> 8;
    bool romChanged = false;
    if ((high & 0xC0) == 0x40) {  // Gate Array
        switch (value >> 6) {
            case 0: selectedPen = (value & 0x10) ? 16 : (value & 0x0F); break;
            case 1: inks[selectedPen] = value & 0x1F; break;
            case 2: {
                mode = value & 3;
                const bool lower = !(value & 4), upper = !(value & 8);
                romChanged = lower != lowerRomOn || upper != upperRomOn;
                lowerRomOn = lower;
                upperRomOn = upper;
                break;
            }
            default: ramConfig = value; break;
        }
    }
    if ((high & 0x40) == 0) {  // CRTC
        if ((high & 3) == 0) crtcSelected = value & 0x1F;
        else if ((high & 3) == 1) crtc[crtcSelected] = value;
    }
    if ((high & 0x20) == 0) upperRomNumber = value;
    return romChanged;
}

Word VideoState::screenBase() const { return static_cast<Word>(((crtc[12] >> 4) & 3) * 0x4000); }

// ---- rendering ----------------------------------------------------------------------------------

IndexedImage renderScreen(const Memory& memory, const VideoState& video, const RenderOptions& options) {
    const int mode = video.mode == 3 ? 1 : video.mode;
    const int bytesPerLine = std::max(1, video.crtc[1] * 2);
    const int rows = std::max(1, static_cast<int>(video.crtc[6]));
    const int linesPerRow = std::max(1, (video.crtc[9] & 0x1F) + 1);
    const int scanLines = rows * linesPerRow;
    const int pixelsPerByte = mode == 0 ? 2 : mode == 1 ? 4 : 8;
    const int xScale = mode == 0 ? 4 : mode == 1 ? 2 : 1;  // makes every mode about 640 wide
    const int yScale = 2;
    const int borderX = options.border ? 32 : 0;
    const int borderY = options.border ? 32 : 0;

    IndexedImage img;
    img.width = bytesPerLine * pixelsPerByte * xScale + 2 * borderX;
    img.height = scanLines * yScale + 2 * borderY;
    for (int pen = 0; pen < 17; ++pen) img.palette.push_back(kHardwareColourRgb[video.inks[static_cast<std::size_t>(pen)] & 0x1F]);
    img.pixels.assign(static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height), 16);  // border pen

    const unsigned offset = ((video.crtc[12] & 3) << 8 | video.crtc[13]) * 2u;
    const Word base = video.screenBase();
    std::vector<Byte> line(static_cast<std::size_t>(bytesPerLine * pixelsPerByte));

    for (int y = 0; y < scanLines; ++y) {
        const int row = y / linesPerRow;
        const int raster = (y % linesPerRow) & 7;
        for (int x = 0; x < bytesPerLine; ++x) {
            const unsigned inRow = (offset + static_cast<unsigned>(row * bytesPerLine + x)) & 0x7FF;
            const Byte b = memory.readRam(static_cast<Word>(base + raster * 0x800 + static_cast<int>(inRow)));
            const std::size_t o = static_cast<std::size_t>(x * pixelsPerByte);
            if (mode == 2) {
                for (int p = 0; p < 8; ++p) line[o + static_cast<std::size_t>(p)] = (b >> (7 - p)) & 1;
            } else if (mode == 1) {
                for (int p = 0; p < 4; ++p) {
                    line[o + static_cast<std::size_t>(p)] = static_cast<Byte>(((b >> (7 - p)) & 1) | (((b >> (3 - p)) & 1) << 1));
                }
            } else {
                line[o] = static_cast<Byte>(((b >> 7) & 1) | (((b >> 3) & 1) << 1) | (((b >> 5) & 1) << 2) | (((b >> 1) & 1) << 3));
                line[o + 1] = static_cast<Byte>(((b >> 6) & 1) | (((b >> 2) & 1) << 1) | (((b >> 4) & 1) << 2) | ((b & 1) << 3));
            }
        }
        for (int dy = 0; dy < yScale; ++dy) {
            auto* dst = img.pixels.data() + static_cast<std::size_t>((borderY + y * yScale + dy) * img.width + borderX);
            for (std::size_t px = 0; px < line.size(); ++px) {
                for (int dx = 0; dx < xScale; ++dx) *dst++ = line[px];
            }
        }
    }
    return img;
}

IndexedImage cropAndZoom(const IndexedImage& image, int x0, int y0, int x1, int y1, int zoom) {
    x0 = std::clamp(x0, 0, image.width - 1);
    x1 = std::clamp(x1, x0, image.width - 1);
    y0 = std::clamp(y0, 0, image.height - 1);
    y1 = std::clamp(y1, y0, image.height - 1);
    zoom = std::clamp(zoom, 1, 16);
    IndexedImage out;
    out.palette = image.palette;
    out.width = (x1 - x0 + 1) * zoom;
    out.height = (y1 - y0 + 1) * zoom;
    out.pixels.resize(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height));
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            out.pixels[static_cast<std::size_t>(y * out.width + x)] =
                image.pixels[static_cast<std::size_t>((y0 + y / zoom) * image.width + x0 + x / zoom)];
        }
    }
    return out;
}

std::string describeScreenAddress(Word address, Word base) {
    if (address < base || address - base >= 0x4000) return "";
    const unsigned offset = address - base;
    const unsigned scanLine = offset >> 11;
    const unsigned inBlock = offset & 0x7FF;
    if (inBlock >= 2000) return "";  // the 48 unused bytes at the end of every 2 KiB block
    return "char row " + std::to_string(inBlock / 80) + ", byte column " + std::to_string(inBlock % 80) + ", scan line " + std::to_string(scanLine);
}

ImageDiff diffImages(const IndexedImage& a, const IndexedImage& b) {
    ImageDiff d;
    if (a.width != b.width || a.height != b.height) {
        d.sameSize = false;
        return d;
    }
    d.x0 = a.width;
    d.y0 = a.height;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            const auto i = static_cast<std::size_t>(y * a.width + x);
            const auto colourA = a.palette[a.pixels[i]];
            const auto colourB = b.palette[b.pixels[i]];
            if (colourA == colourB) continue;
            ++d.changedPixels;
            d.x0 = std::min(d.x0, x);
            d.y0 = std::min(d.y0, y);
            d.x1 = std::max(d.x1, x);
            d.y1 = std::max(d.y1, y);
        }
    }
    if (d.changedPixels == 0) {
        d.x0 = d.y0 = 0;
        d.x1 = d.y1 = -1;
    }
    return d;
}

// ---- snapshots ----------------------------------------------------------------------------------

std::optional<SnaState> parseSnaHeader(std::span<const Byte> f) {
    if (!looksLikeSna(f) || f.size() < kSnaHeaderSize) return std::nullopt;
    SnaState s;
    Registers& r = s.cpu.regs;
    s.version = f[0x10];
    const auto w = [&](std::size_t lo) { return static_cast<Word>(f[lo] | (f[lo + 1] << 8)); };
    // header order: F A C B E D L H
    r.main[6] = f[0x11];
    r.main[7] = f[0x12];
    r.main[1] = f[0x13];
    r.main[0] = f[0x14];
    r.main[3] = f[0x15];
    r.main[2] = f[0x16];
    r.main[5] = f[0x17];
    r.main[4] = f[0x18];
    r.r = f[0x19];
    r.i = f[0x1A];
    r.iff1 = f[0x1B] & 1;
    r.iff2 = f[0x1C] & 1;
    r.ix = w(0x1D);
    r.iy = w(0x1F);
    r.sp = w(0x21);
    r.pc = w(0x23);
    r.im = std::min<Byte>(f[0x25], 2);
    r.alt[6] = f[0x26];
    r.alt[7] = f[0x27];
    r.alt[1] = f[0x28];
    r.alt[0] = f[0x29];
    r.alt[3] = f[0x2A];
    r.alt[2] = f[0x2B];
    r.alt[5] = f[0x2C];
    r.alt[4] = f[0x2D];

    VideoState& v = s.video;
    v.selectedPen = f[0x2E] & 0x1F;
    if (v.selectedPen > 16) v.selectedPen = 16;
    for (std::size_t i = 0; i < 16; ++i) v.inks[i] = f[0x2F + i] & 0x1F;
    v.inks[16] = f[0x3F] & 0x1F;
    const Byte config = f[0x40];
    v.mode = config & 3;
    v.lowerRomOn = !(config & 4);
    v.upperRomOn = !(config & 8);
    v.ramConfig = f[0x41];
    v.crtcSelected = f[0x42] & 0x1F;
    for (std::size_t i = 0; i < 18; ++i) v.crtc[i] = f[0x43 + i];
    v.upperRomNumber = f[0x55];
    s.memoryKiB = f[0x6B] | (f[0x6C] << 8);
    if (s.memoryKiB == 0) s.memoryKiB = 64;
    return s;
}

}  // namespace z80::cpc
