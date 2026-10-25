#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "z80/cpu.hpp"
#include "z80/memory.hpp"
#include "z80/png.hpp"
#include "z80/types.hpp"

/// Amstrad CPC platform knowledge: firmware jump block names, hardware ports, video hardware.
namespace z80::cpc {

struct PlatformSymbol {
    const char* name;
    Word address;
};

// ---- generated tables (src/cpc_data.cpp) ---------------------------------------------------
extern const std::array<PlatformSymbol, 231> kFirmwareSymbols;
extern const std::array<std::uint32_t, 32> kHardwareColourRgb;   ///< hardware colour 0..31 -> 0xRRGGBB
extern const std::array<std::uint8_t, 27> kInkToHardware;        ///< firmware ink 0..26 -> hardware colour
extern const std::array<const char*, 32> kHardwareColourNames;

// ---- firmware ---------------------------------------------------------------------------------
/// Name of the firmware jump block entry at exactly `address` ("TXT_OUTPUT"), if any.
std::optional<std::string> firmwareName(Word address);
std::optional<Word> firmwareAddress(const std::string& name);

// ---- hardware ports -----------------------------------------------------------------------------
struct PortSymbol {
    const char* name;
    Word address;
    const char* description;
};
std::span<const PortSymbol> portSymbols();

/// "GATE_ARRAY+&8D" for a BC value that addresses a known device, nullopt otherwise.
std::optional<std::string> portExpression(Word port);
/// "HIGH(GATE_ARRAY)" for a B value that is the high byte of a known port.
std::optional<std::string> portHighExpression(Byte high);
/// What an OUT to `port` with `value` does, e.g. "Gate Array: screen mode 1, lower ROM off, upper ROM off".
std::string describePortWrite(Word port, Byte value);
std::string describePortRead(Word port);

// ---- video hardware state -----------------------------------------------------------------------
struct VideoState {
    Byte mode = 1;                            ///< 0, 1, 2 (3 is treated as 1)
    std::array<Byte, 17> inks{};              ///< hardware colour per pen 0..15; index 16 = border
    Byte selectedPen = 0;                     ///< 0..15, or 16 for the border
    std::array<Byte, 32> crtc{};              ///< CRTC registers
    Byte crtcSelected = 0;
    bool lowerRomOn = true;
    bool upperRomOn = true;
    Byte upperRomNumber = 0;
    Byte ramConfig = 0xC0;

    VideoState();  ///< firmware defaults (mode 1, default palette, screen at &C000)
    bool operator==(const VideoState&) const = default;

    /// Updates the state for a write to an I/O port (Gate Array, CRTC, ROM select). Returns true if
    /// the ROM visibility changed.
    bool writePort(Word port, Byte value);

    [[nodiscard]] Word screenBase() const;  ///< start of the 16 KiB screen page
};

struct RenderOptions {
    bool border = false;  ///< add a border margin in the border colour
};

/// The screen as the CRT would show it: 640 columns wide (CPC pixels are repeated to square
/// them), every scan line doubled vertically. Flashing inks are shown in their first colour.
IndexedImage renderScreen(const Memory& memory, const VideoState& video, const RenderOptions& options = {});

/// Cuts out the pixel box [x0,x1] x [y0,y1] (clamped to the image) and enlarges it by an integer
/// factor (nearest neighbour), so that small details become visible to a human or a vision model.
IndexedImage cropAndZoom(const IndexedImage& image, int x0, int y0, int x1, int y1, int zoom);

/// "char row 24, byte column 10, scan line 3" for an address inside the 16 KiB screen page at
/// `base`, or an empty string if the address is not in that page's visible area.
std::string describeScreenAddress(Word address, Word base = 0xC000);

struct ImageDiff {
    bool sameSize = true;
    std::uint64_t changedPixels = 0;
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;  ///< bounding box of the change (inclusive); empty if x1 < x0
};
ImageDiff diffImages(const IndexedImage& a, const IndexedImage& b);

// ---- snapshots ----------------------------------------------------------------------------------
struct SnaState {
    CpuState cpu;
    VideoState video;
    int version = 0;
    int memoryKiB = 64;
};
/// Parses the register/hardware part of a .sna header. nullopt if the data is not a CPC snapshot.
std::optional<SnaState> parseSnaHeader(std::span<const Byte> file);

}  // namespace z80::cpc
