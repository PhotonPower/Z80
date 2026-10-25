#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace z80 {

struct JsonError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Minimal JSON value (enough for the agent protocol). Objects keep insertion order.
class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool v) : type_(Type::Bool), bool_(v) {}
    Json(int v) : Json(static_cast<std::int64_t>(v)) {}
    Json(unsigned v) : Json(static_cast<std::int64_t>(v)) {}
    Json(std::int64_t v) : type_(Type::Number), int_(v), num_(static_cast<double>(v)), isInt_(true) {}
    Json(std::uint64_t v) : Json(static_cast<std::int64_t>(v)) {}
    Json(double v) : type_(Type::Number), num_(v) {}
    Json(const char* v) : type_(Type::String), str_(v) {}
    Json(std::string v) : type_(Type::String), str_(std::move(v)) {}

    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }

    [[nodiscard]] Type type() const { return type_; }
    [[nodiscard]] bool isNull() const { return type_ == Type::Null; }
    [[nodiscard]] bool isBool() const { return type_ == Type::Bool; }
    [[nodiscard]] bool isNumber() const { return type_ == Type::Number; }
    [[nodiscard]] bool isString() const { return type_ == Type::String; }
    [[nodiscard]] bool isArray() const { return type_ == Type::Array; }
    [[nodiscard]] bool isObject() const { return type_ == Type::Object; }

    [[nodiscard]] bool asBool() const { return bool_; }
    [[nodiscard]] std::int64_t asInt() const { return isInt_ ? int_ : static_cast<std::int64_t>(num_); }
    [[nodiscard]] double asDouble() const { return num_; }
    [[nodiscard]] const std::string& asString() const { return str_; }
    [[nodiscard]] const std::vector<Json>& items() const { return items_; }
    [[nodiscard]] const std::vector<std::pair<std::string, Json>>& members() const { return members_; }

    /// Object lookup; nullptr if absent or not an object.
    [[nodiscard]] const Json* find(std::string_view key) const;

    /// Sets/creates a member and returns *this, so calls can be chained (becomes an object if null).
    Json& set(std::string key, Json value);
    /// Appends to an array (this becomes an array if it was null).
    Json& push(Json value);

    [[nodiscard]] std::string dump() const;
    static Json parse(std::string_view text);

private:
    void dumpTo(std::string& out) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    std::int64_t int_ = 0;
    double num_ = 0;
    bool isInt_ = false;
    std::string str_;
    std::vector<Json> items_;
    std::vector<std::pair<std::string, Json>> members_;
};

}  // namespace z80
