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
        [this](Word) -> Byte {
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
