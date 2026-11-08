#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "z80/types.hpp"

namespace z80 {

struct DskError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// The 128-byte header AMSDOS puts in front of a file it writes with SAVE (binaries, protected BASIC). Recognised by its checksum.
struct AmsdosHeader {
    std::string name;          ///< "NAME.EXT" (blanks removed)
    std::uint8_t user = 0;
    std::uint8_t type = 0;     ///< 0 BASIC, 1 protected BASIC, 2 binary, 22 ASCII (bits 1-3: file type, bit 0: protected)
    Word load = 0;             ///< load address
    Word exec = 0;             ///< entry address
    std::uint32_t length = 0;  ///< length of the data behind the header
};

/// The AMSDOS header at the start of `file`, if the first 128 bytes are one (checksum over bytes 0-66 at 67-68).
std::optional<AmsdosHeader> parseAmsdosHeader(std::span<const Byte> file);
/// A header for `length` bytes at `load` with entry `exec` (type binary), checksum included.
std::vector<Byte> makeAmsdosHeader(const std::string& name, Word load, Word exec, std::uint32_t length, std::uint8_t type = 2);

/// A file in the AMSDOS directory of a disk.
struct DskFile {
    std::string name;          ///< "NAME.EXT"
    std::uint8_t user = 0;
    bool readOnly = false;
    bool system = false;       ///< hidden from CAT
    std::uint32_t size = 0;    ///< bytes, as the directory says (a multiple of 128; includes an AMSDOS header)
    std::vector<unsigned> blocks;  ///< allocation blocks in file order
};

/// An Amstrad CPC disk image (.dsk): "MV - CPCEMU" (standard) or "EXTENDED CPC DSK" files, with the AMSDOS file system in one of the
/// usual formats: DATA (sectors &C1-&C9, no reserved tracks), SYSTEM/VENDOR (&41-&49, two reserved tracks) or IBM (&01-&08, one).
/// Disks with other formats (copy protection, own loaders) can still be read sector by sector.
class DskImage {
public:
    /// Throws DskError if the bytes are not a disk image (or its track table is damaged).
    static DskImage parse(std::span<const Byte> bytes);
    [[nodiscard]] static bool looksLikeDsk(std::span<const Byte> bytes);

    struct Sector {
        Byte c = 0, h = 0, r = 0, n = 0;  ///< ID: cylinder, head, sector number, size code (128 << n bytes)
        Byte st1 = 0, st2 = 0;            ///< FDC status (copy protection sets error bits)
        std::vector<Byte> data;
    };
    struct Track {
        unsigned number = 0, side = 0;
        std::vector<Sector> sectors;      ///< in the order they are stored (the physical order on the track)
    };

    [[nodiscard]] bool extended() const { return extended_; }
    [[nodiscard]] unsigned trackCount() const { return tracks_; }
    [[nodiscard]] unsigned sides() const { return sides_; }
    [[nodiscard]] const std::string& creator() const { return creator_; }
    [[nodiscard]] const Track* track(unsigned number, unsigned side = 0) const;
    [[nodiscard]] const Sector* sector(unsigned track, Byte id, unsigned side = 0) const;

    /// "data", "system", "ibm" or "" (no AMSDOS format recognised: the first sector ID of track 0 tells).
    [[nodiscard]] std::string format() const;
    /// The directory (empty without a recognised format). Deleted entries (user &E5) are left out.
    [[nodiscard]] std::vector<DskFile> files() const;
    /// The file `name` ("NAME.EXT", case does not matter; without user: the first match), all of its bytes including an AMSDOS header.
    /// Throws DskError if there is no such file.
    [[nodiscard]] std::vector<Byte> readFile(const std::string& name, std::optional<unsigned> user = std::nullopt) const;
    /// The boot sector of a SYSTEM disk (track 0, sector &41: |CPM loads it to &0100 and runs it). Throws DskError on other formats.
    [[nodiscard]] std::vector<Byte> bootSector() const;

private:
    struct Geometry {
        Byte firstId = 0;
        unsigned reservedTracks = 0;
        unsigned sectorsPerTrack = 9;
    };
    [[nodiscard]] std::optional<Geometry> geometry() const;
    [[nodiscard]] std::vector<Byte> block(const Geometry& g, unsigned number) const;

    bool extended_ = false;
    unsigned tracks_ = 0, sides_ = 1;
    std::string creator_;
    std::vector<Track> trackList_;
};

}  // namespace z80
