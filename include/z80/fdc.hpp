#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

#include "z80/dsk.hpp"
#include "z80/types.hpp"

namespace z80 {

/// The CPC's disc controller, a NEC uPD765A, with one drive (A) holding a DSK image. The CPU talks to it through the main status
/// register (&FB7E) and the data register (&FB7F) without DMA, byte by byte, like AMSDOS does; &FA7E switches the motor.
///
/// What is emulated: SPECIFY, SENSE DRIVE STATUS, RECALIBRATE, SEEK, SENSE INTERRUPT STATUS, READ ID, READ DATA, READ DELETED
/// DATA, WRITE DATA and WRITE DELETED DATA (into the image). The status bytes ST1/ST2 a protected image stores for a sector (data
/// errors, deleted data marks) come back in the result, the size of a sector is what its ID says (128 << N; an image that stores
/// less is repeated, one that stores more is cut). Without a terminal count on the CPC every read ends at the end of the cylinder
/// (ST0 &40, ST1 &80) - AMSDOS expects that. There is no timing: data is always ready, seeks are done at once.
class Fdc {
public:
    void insert(DskImage* disk) { disk_ = disk; }
    void motor(bool on) { motor_ = on; }
    [[nodiscard]] bool motorOn() const { return motor_; }

    [[nodiscard]] Byte status() const;   ///< &FB7E
    Byte readData();                     ///< &FB7F
    void writeData(Byte value);          ///< &FB7F

    struct Event {
        std::string what;   ///< "read 4/&C1", "seek 12", ...
        unsigned track = 0;
        Byte sector = 0;
        Byte st1 = 0, st2 = 0;
    };
    /// What the controller did (for disk_log); the oldest events are dropped after 256.
    std::deque<Event> log;
    [[nodiscard]] unsigned track() const { return track_; }

private:
    enum class Phase { Command, ExecRead, ExecWrite, Result };
    void execute();
    void startRead();
    bool nextSector();          ///< finds the next sector of a READ/WRITE (R) on the track; false = done or failed (result prepared)
    void finishTransfer(Byte st0, Byte st1, Byte st2);
    void result(std::vector<Byte> bytes);
    void note(std::string what, Byte sector = 0, Byte st1 = 0, Byte st2 = 0);
    [[nodiscard]] const DskImage::Track* currentTrack() const;
    [[nodiscard]] Byte st0(Byte flags) const { return static_cast<Byte>(flags | (head_ << 2) | unit_); }

    DskImage* disk_ = nullptr;
    bool motor_ = false;
    Phase phase_ = Phase::Command;
    std::vector<Byte> command_;
    std::vector<Byte> result_;
    std::size_t resultPos_ = 0;
    std::vector<Byte> buffer_;      // the sector being read or written
    std::size_t bufferPos_ = 0;
    unsigned track_ = 0;            // where the head is
    unsigned idIndex_ = 0;          // READ ID returns the sectors of a track in turn
    bool seekDone_ = false;         // an interrupt from SEEK / RECALIBRATE for SENSE INTERRUPT STATUS
    Byte unit_ = 0, head_ = 0;
    // the current READ/WRITE command
    bool deleted_ = false, skip_ = false, writing_ = false;
    Byte c_ = 0, h_ = 0, r_ = 0, n_ = 0, eot_ = 0, dtl_ = 0;
    DskImage::Sector* sector_ = nullptr;
    Byte st1_ = 0, st2_ = 0;
};

}  // namespace z80
