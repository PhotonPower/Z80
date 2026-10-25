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
            if (cpc_ && input_.empty()) {
                // PPI port B (&F5xx): bit 0 = vertical sync, bits 1-3 = 111 (Amstrad), 50 Hz, expansion and printer idle.
                // A frame takes 312 lines * 64 us = 79872 T-states; the sync pulse lasts about 16 lines.
                const unsigned high = port >> 8;
                if ((high & 0x08) == 0 && (high & 3) == 1) return static_cast<Byte>(0x7E | (cpu_.cycles() % 79872 < 4096 ? 1 : 0));
            }
            if (input_.empty()) return 0xFF;
            const Byte b = input_.front();
            input_.pop_front();
            return b;
        },
        [this](Word port, Byte value) {
            ioLog_.emplace_back(port, value);
            while (ioLog_.size() > options_.maxIoLog) ioLog_.pop_front();
            if (cpc_ && video_.writePort(port, value)) applyRomState();
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
