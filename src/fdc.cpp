// The uPD765A disc controller of the CPC (see fdc.hpp).

#include "z80/fdc.hpp"

#include <algorithm>

namespace z80 {

namespace {
constexpr Byte kRqm = 0x80, kDio = 0x40, kExm = 0x20, kBusy = 0x10;
// ST0
constexpr Byte kAbnormal = 0x40, kInvalid = 0x80, kSeekEnd = 0x20, kNotReady = 0x08;
// ST1
constexpr Byte kEndOfCylinder = 0x80, kNoData = 0x04, kMissingAddress = 0x01;
// ST2
constexpr Byte kControlMark = 0x40;

/// Bytes of each command (the first byte's low 5 bits).
unsigned commandLength(Byte cmd) {
    switch (cmd & 0x1F) {
        case 0x02: return 9;   // read track
        case 0x03: return 3;   // specify
        case 0x04: return 2;   // sense drive status
        case 0x05: return 9;   // write data
        case 0x06: return 9;   // read data
        case 0x07: return 2;   // recalibrate
        case 0x08: return 1;   // sense interrupt status
        case 0x09: return 9;   // write deleted data
        case 0x0A: return 2;   // read ID
        case 0x0C: return 9;   // read deleted data
        case 0x0D: return 6;   // format track
        case 0x0F: return 3;   // seek
        case 0x11: case 0x19: case 0x1D: return 9;  // scan
        default: return 1;     // invalid
    }
}
}  // namespace

Byte Fdc::status() const {
    switch (phase_) {
        case Phase::Command: return command_.empty() ? kRqm : static_cast<Byte>(kRqm | kBusy);
        case Phase::ExecRead: return kRqm | kDio | kExm | kBusy;
        case Phase::ExecWrite: return kRqm | kExm | kBusy;
        case Phase::Result: return kRqm | kDio | kBusy;
    }
    return kRqm;
}

void Fdc::note(std::string what, Byte sector, Byte st1, Byte st2) {
    log.push_back({std::move(what), track_, sector, st1, st2});
    while (log.size() > 256) log.pop_front();
}

const DskImage::Track* Fdc::currentTrack() const {
    if (!disk_ || unit_ != 0) return nullptr;
    const DskImage::Track* t = disk_->track(track_);
    return t && !t->sectors.empty() ? t : nullptr;
}

void Fdc::result(std::vector<Byte> bytes) {
    result_ = std::move(bytes);
    resultPos_ = 0;
    phase_ = result_.empty() ? Phase::Command : Phase::Result;
}

Byte Fdc::readData() {
    if (phase_ == Phase::ExecRead) {
        const Byte b = bufferPos_ < buffer_.size() ? buffer_[bufferPos_] : Byte{0xFF};
        if (++bufferPos_ >= buffer_.size()) {
            // the sector is through: its stored errors end the command, a deleted mark too (when not skipped), else on to R + 1
            if (st1_ || (st2_ & ~kControlMark)) {
                finishTransfer(kAbnormal, st1_, st2_);
            } else if (st2_ & kControlMark) {
                ++r_;
                finishTransfer(kAbnormal, 0, kControlMark);   // the other kind of mark: the read stops after this sector
            } else if (r_ == eot_) {
                ++r_;
                finishTransfer(kAbnormal, kEndOfCylinder, 0);  // no terminal count on the CPC
            } else {
                ++r_;
                if (nextSector()) startRead();
            }
        }
        return b;
    }
    if (phase_ == Phase::Result) {
        const Byte b = resultPos_ < result_.size() ? result_[resultPos_] : Byte{0xFF};
        if (++resultPos_ >= result_.size()) {
            phase_ = Phase::Command;
            command_.clear();
        }
        return b;
    }
    return 0xFF;
}

void Fdc::writeData(Byte value) {
    if (phase_ == Phase::ExecWrite) {
        if (bufferPos_ < buffer_.size()) buffer_[bufferPos_] = value;
        if (++bufferPos_ >= buffer_.size()) {
            if (sector_) {
                for (std::size_t i = 0; i < sector_->data.size(); ++i) sector_->data[i] = buffer_[i % buffer_.size()];
                sector_->st2 = static_cast<Byte>(deleted_ ? (sector_->st2 | kControlMark) : (sector_->st2 & ~kControlMark));
                note("write " + std::to_string(track_) + "/&" + "0123456789ABCDEF"[r_ >> 4] + "0123456789ABCDEF"[r_ & 15], r_);
            }
            if (r_ == eot_) {
                ++r_;
                finishTransfer(kAbnormal, kEndOfCylinder, 0);
            } else {
                ++r_;
                if (nextSector()) {
                    buffer_.assign(std::size_t{128} << std::min<Byte>(n_, 6), 0);
                    bufferPos_ = 0;
                }
            }
        }
        return;
    }
    if (phase_ != Phase::Command) return;   // ignored while results are pending
    command_.push_back(value);
    if (command_.size() >= commandLength(command_[0])) execute();
}

void Fdc::finishTransfer(Byte st0Flags, Byte st1, Byte st2) {
    result({st0(st0Flags), st1, st2, c_, h_, r_, n_});
    command_.clear();
}

bool Fdc::nextSector() {
    const DskImage::Track* t = currentTrack();
    if (!t) {
        finishTransfer(static_cast<Byte>(kAbnormal | (unit_ ? kNotReady : 0)), kMissingAddress, 0);
        return false;
    }
    for (;;) {
        DskImage::Track* track = disk_->trackRef(track_);
        sector_ = nullptr;
        for (auto& s : track->sectors) {
            if (s.c == c_ && s.h == h_ && s.r == r_ && s.n == n_) { sector_ = &s; break; }
        }
        if (!sector_) {
            note("missing " + std::to_string(track_) + "/R" + std::to_string(r_), r_, kNoData);
            finishTransfer(kAbnormal, kNoData, 0);
            return false;
        }
        const bool marked = (sector_->st2 & kControlMark) != 0;
        if (!writing_ && marked != deleted_ && skip_) {   // SK: sectors of the other kind are skipped
            if (r_ == eot_) {
                ++r_;
                finishTransfer(kAbnormal, kEndOfCylinder, 0);
                return false;
            }
            ++r_;
            continue;
        }
        st1_ = sector_->st1;
        st2_ = static_cast<Byte>((sector_->st2 & ~kControlMark) | (!writing_ && marked != deleted_ ? kControlMark : 0));
        return true;
    }
}

void Fdc::startRead() {
    const std::size_t size = n_ == 0 ? std::max<std::size_t>(dtl_, 1) : std::size_t{128} << std::min<Byte>(n_, 6);
    buffer_.assign(size, 0);
    const auto& data = sector_->data;
    for (std::size_t i = 0; i < size; ++i) buffer_[i] = data.empty() ? Byte{0xE5} : data[i % data.size()];
    bufferPos_ = 0;
    phase_ = Phase::ExecRead;
    note("read " + std::to_string(track_) + "/&" + "0123456789ABCDEF"[r_ >> 4] + "0123456789ABCDEF"[r_ & 15] +
             (st2_ & kControlMark ? " (deleted mark)" : "") + (st1_ || (st2_ & ~kControlMark) ? " (error bits)" : ""),
         r_, st1_, st2_);
}

void Fdc::execute() {
    const Byte cmd = command_[0];
    const std::vector<Byte> c = command_;
    command_.clear();
    if (c.size() > 1) {
        unit_ = c[1] & 3;
        head_ = (c[1] >> 2) & 1;
    }
    switch (cmd & 0x1F) {
        case 0x03:   // SPECIFY
            phase_ = Phase::Command;
            return;
        case 0x04: {  // SENSE DRIVE STATUS
            Byte st3 = static_cast<Byte>((head_ << 2) | unit_);
            if (unit_ == 0 && disk_) st3 |= 0x20 | (track_ == 0 ? 0x10 : 0);   // ready, track 0
            result({st3});
            return;
        }
        case 0x07:   // RECALIBRATE
            if (unit_ == 0) track_ = 0;
            seekDone_ = true;
            note("recalibrate");
            phase_ = Phase::Command;
            return;
        case 0x0F:   // SEEK
            if (unit_ == 0) track_ = c[2];
            seekDone_ = true;
            note("seek " + std::to_string(c[2]));
            phase_ = Phase::Command;
            return;
        case 0x08:   // SENSE INTERRUPT STATUS
            if (seekDone_) {
                seekDone_ = false;
                result({static_cast<Byte>(kSeekEnd | (unit_ && !disk_ ? kNotReady : 0) | unit_), static_cast<Byte>(track_)});
            } else {
                result({kInvalid});
            }
            return;
        case 0x0A: {  // READ ID
            const DskImage::Track* t = currentTrack();
            if (!t) {
                result({st0(static_cast<Byte>(kAbnormal | (unit_ ? kNotReady : 0))), kMissingAddress, 0, 0, 0, 0, 0});
                return;
            }
            const auto& s = t->sectors[idIndex_++ % t->sectors.size()];
            result({st0(0), 0, 0, s.c, s.h, s.r, s.n});
            return;
        }
        case 0x06: case 0x0C: case 0x05: case 0x09: {  // READ DATA, READ DELETED DATA, WRITE DATA, WRITE DELETED DATA
            writing_ = (cmd & 0x1F) == 0x05 || (cmd & 0x1F) == 0x09;
            deleted_ = (cmd & 0x1F) == 0x0C || (cmd & 0x1F) == 0x09;
            skip_ = (cmd & 0x20) != 0;
            c_ = c[2]; h_ = c[3]; r_ = c[4]; n_ = c[5]; eot_ = c[6]; dtl_ = c[8];
            if (!nextSector()) return;
            if (writing_) {
                buffer_.assign(std::size_t{128} << std::min<Byte>(n_, 6), 0);
                bufferPos_ = 0;
                phase_ = Phase::ExecWrite;
            } else {
                startRead();
            }
            return;
        }
        default:     // not emulated (format, scan, read track) or invalid
            note("command &" + std::string(1, "0123456789ABCDEF"[cmd >> 4]) + "0123456789ABCDEF"[cmd & 15] + " not emulated");
            result({kInvalid});
            return;
    }
}

}  // namespace z80
