#include "z80/session.hpp"
#include "session_impl.hpp"

#include <algorithm>
#include <cctype>
#include <functional>

#include "z80/decoder.hpp"
#include "z80/expression.hpp"
#include "z80/format.hpp"
#include "z80/image_io.hpp"

namespace z80 {

Session::Session(SessionOptions options)
    : options_(options), memoryStore_(std::make_unique<Memory>()), memory_(*memoryStore_), cpu_(memory_) {
    cpu_.setPortHandlers(
        [this](Word port) -> Byte {
            // the disc controller: A10 = 0, A8 = 1, A7 = 0; A0 = 0 main status, 1 data
            if (cpc_ && (port & 0x0580) == 0x0100) return (port & 1) ? fdc_.readData() : fdc_.status();
            if (cpc_ && input_.empty()) {
                // PPI port B (&F5xx): bit 0 = vertical sync, bits 1-3 = 111 (Amstrad), 50 Hz, expansion and printer idle.
                // A frame takes 312 lines * 64 us = 79872 T-states; the sync pulse lasts about 16 lines.
                const unsigned high = port >> 8;
                if ((high & 0x08) == 0 && (high & 3) == 1) return static_cast<Byte>(0x7E | (cpu_.cycles() % 79872 < 4096 ? 1 : 0));
                // PPI port A while the PSG is being read: register 14 is the keyboard row selected by port C (controls)
                if ((high & 0x08) == 0 && (high & 3) == 0 && (ppiC_ >> 6) == 1) {
                    return psgRegister_ == 14 ? keyboardRow(ppiC_ & 0x0F) : Byte{0xFF};
                }
            }
            if (input_.empty()) return 0xFF;
            const Byte b = input_.front();
            input_.pop_front();
            return b;
        },
        [this](Word port, Byte value) {
            ioLog_.push_back({port, value, static_cast<Word>(cpu_.regs().pc - 2)});  // OUT (n),A and OUT (C),r are 2 bytes long
            while (ioLog_.size() > options_.maxIoLog) ioLog_.pop_front();
            if (cpc_ && (port & 0x0480) == 0) {   // A10 = 0, A7 = 0: the disc controller (A8 = 1) or the motor (A8 = 0)
                if (port & 0x0100) {
                    if (port & 1) fdc_.writeData(value);
                } else {
                    fdc_.motor(value & 1);
                }
            }
            if (cpc_) {
                const cpc::VideoState before = video_;
                if (video_.writePort(port, value) || before.upperRomNumber != video_.upperRomNumber) applyRomState();
                if (before.mode != video_.mode || before.inks != video_.inks) noteVideoChange(before);
            }
            if (cpc_ && ((port >> 8) & 0x08) == 0) {  // PPI: remember what the PSG and the keyboard scanning need
                const unsigned which = (port >> 8) & 3;
                if (which == 0) psgLatch_ = value;
                if (which == 2) {
                    ppiC_ = value;
                    if ((value >> 6) == 3) psgRegister_ = psgLatch_;  // select a PSG register
                }
            }
        });
}

Json Session::handle(const Json& request) {
    Json reply = Json::object();
    reply.set("ok", true);
    Impl impl(*this, request);
    try {
        if (!request.isObject()) throw SessionError("request must be a JSON object");
        const Json body = impl.run();
        for (const auto& [k, v] : body.members()) reply.set(k, v);
    } catch (const std::exception& e) {
        Json err = Json::object();
        err.set("ok", false).set("error", e.what());
        if (impl.detailed_.isArray()) err.set("errors", impl.detailed_);
        return err;
    }
    return reply;
}

namespace {
constexpr std::uint64_t kFrameCycles = 79872;  // 312 lines of 64 us at 4 MHz
constexpr std::uint64_t kLineCycles = 256;
// The first line of the picture, counted from the start of the vertical sync (where the frame starts in this emulation):
// the sync begins at character row R7, the frame has (R4 + 1) rows of R9 + 1 lines plus R5 lines.
std::uint64_t displayStartCycles(const cpc::VideoState& v) {
    const unsigned lines = (v.crtc[9] & 0x1F) + 1u;
    const unsigned total = (v.crtc[4] + 1u) * lines + v.crtc[5];
    const unsigned sync = v.crtc[7] * lines;
    const unsigned start = total > sync ? total - sync : 0;
    return (start * kLineCycles) % kFrameCycles;
}
}  // namespace

void Session::noteVideoChange(const cpc::VideoState& before) {
    const std::uint64_t t = cpu_.cycles() + kFrameCycles - displayStartCycles(video_);
    const std::uint64_t frame = t / kFrameCycles;
    const int line = static_cast<int>((t % kFrameCycles) / kLineCycles);
    if (frame != rasterFrame_) {
        if (rasterFrame_ + 1 == frame) {
            lastRasterStart_ = rasterStart_;
            lastRasterChanges_ = std::move(rasterChanges_);
        } else {  // frames without a change in between: the last complete one was all `before`
            lastRasterStart_ = before;
            lastRasterChanges_.clear();
        }
        rasterFrame_ = frame;
        rasterStart_ = before;
        rasterChanges_.clear();
    }
    if (rasterChanges_.size() < 1000) rasterChanges_.push_back({line, video_});
}

std::pair<cpc::VideoState, std::vector<cpc::RasterChange>> Session::lastFrameRaster() const {
    const std::uint64_t frame = (cpu_.cycles() + kFrameCycles - displayStartCycles(video_)) / kFrameCycles;
    if (frame == rasterFrame_) return {lastRasterStart_, lastRasterChanges_};
    if (frame == rasterFrame_ + 1) return {rasterStart_, rasterChanges_};
    return {video_, {}};
}

bool Session::keyDown(unsigned key) const {
    if (keysDown_.count(static_cast<Byte>(key))) return true;
    if (key >= 72 && key <= 77) return (joystick_ >> (key - 72)) & 1;  // joystick 0 is keyboard line 9
    if (key >= 48 && key <= 53) return (joystick2_ >> (key - 48)) & 1;  // joystick 1 is line 6
    return false;
}

Byte Session::keyboardRow(unsigned row) const {
    Byte v = 0xFF;  // a key that is down reads as 0
    for (unsigned bit = 0; bit < 8; ++bit) {
        if (keyDown(row * 8 + bit)) v = static_cast<Byte>(v & ~(1u << bit));
    }
    return v;
}

std::string Session::handleLine(std::string_view line) {
    try {
        return handle(Json::parse(line)).dump();
    } catch (const std::exception& e) {
        Json err = Json::object();
        err.set("ok", false).set("error", e.what());
        return err.dump();
    }
}

}  // namespace z80
