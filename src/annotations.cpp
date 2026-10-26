#include "z80/annotations.hpp"

#include <algorithm>
#include <cctype>

#include "z80/cpc.hpp"
#include "z80/encoder.hpp"
#include "z80/format.hpp"

namespace z80 {
namespace {

std::string upperCase(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string hex4(unsigned v) { return "0x" + hexDigits(v & 0xFFFF, 4); }

Word parseAddress(const Json& j, const char* what) {
    if (j.isNumber()) {
        if (j.asInt() < 0 || j.asInt() > 0xFFFF) throw JsonError(std::string(what) + ": address out of range");
        return static_cast<Word>(j.asInt());
    }
    if (j.isString()) {
        std::string s = j.asString();
        // "0x4000" / "&4000" / "$4000" / decimal
        try {
            std::size_t used = 0;
            long long v;
            if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) v = std::stoll(s.substr(2), &used, 16), used += 2;
            else if (!s.empty() && (s[0] == '&' || s[0] == '$')) v = std::stoll(s.substr(1), &used, 16), used += 1;
            else v = std::stoll(s, &used, 10);
            if (used == s.size() && v >= 0 && v <= 0xFFFF) return static_cast<Word>(v);
        } catch (const std::exception&) {
        }
    }
    throw JsonError(std::string(what) + ": invalid address");
}

std::string strOr(const Json& j, const char* key) {
    const Json* v = j.find(key);
    return v && v->isString() ? v->asString() : std::string();
}

}  // namespace

const char* toString(DataKind kind) {
    switch (kind) {
        case DataKind::Bytes: return "bytes";
        case DataKind::Words: return "words";
        case DataKind::Text: return "text";
        case DataKind::Pointers: return "pointers";
    }
    return "bytes";
}

std::optional<DataKind> dataKindFromString(const std::string& text) {
    if (text == "bytes") return DataKind::Bytes;
    if (text == "words") return DataKind::Words;
    if (text == "text") return DataKind::Text;
    if (text == "pointers") return DataKind::Pointers;
    return std::nullopt;
}

std::optional<std::string> Annotations::checkName(const std::string& name) {
    if (name.empty()) return "name is empty";
    if (name.size() > 48) return "name is too long (max 48 characters)";
    if (!(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) return "name must start with a letter or '_'";
    for (char c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return "name may only contain letters, digits and '_'";
    }
    const std::string up = upperCase(name);
    if (isReservedName(up) || up == "HIGH" || up == "LOW" || up == "ORG" || up == "EQU" || up == "END" || up == "DB" || up == "DW" ||
        up == "DM" || up == "DS" || up == "DEFB" || up == "DEFW" || up == "DEFM" || up == "DEFS") {
        return "'" + name + "' is a reserved word (register, condition, instruction or directive)";
    }
    for (const auto& s : cpc::kFirmwareSymbols) {
        if (up == s.name) return "'" + name + "' is a firmware symbol name; choose a different name";
    }
    for (const auto& p : cpc::portSymbols()) {
        if (up == p.name) return "'" + name + "' is a hardware port name; choose a different name";
    }
    for (const char* prefix : {"L_", "SUB_", "LOC_", "DATA_"}) {
        const std::string pre = prefix;
        if (up.rfind(pre, 0) == 0 && up.size() == pre.size() + 4 &&
            std::all_of(up.begin() + static_cast<std::ptrdiff_t>(pre.size()), up.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; })) {
            return "names like " + pre + "xxxx (xxxx = hex address) are reserved for automatic labels";
        }
    }
    return std::nullopt;
}

std::optional<std::string> Annotations::claimName(Word address, const std::string& name) {
    if (auto err = checkName(name)) return err;
    const std::string up = upperCase(name);
    if (auto it = byName_.find(up); it != byName_.end() && it->second != address) {
        return "name '" + name + "' is already used for address " + hex4(it->second);
    }
    if (auto old = labels_.find(address); old != labels_.end()) byName_.erase(upperCase(old->second));
    labels_[address] = name;
    byName_[up] = address;
    return std::nullopt;
}

std::optional<std::string> Annotations::setLabel(Word address, const std::string& name) { return claimName(address, name); }

std::optional<std::string> Annotations::setRoutine(Word address, const RoutineDoc& doc) {
    if (!doc.confidence.empty() && doc.confidence != "high" && doc.confidence != "medium" && doc.confidence != "low") {
        return "confidence must be high, medium or low";
    }
    if (auto err = claimName(address, doc.name)) return err;
    routines_[address] = doc;
    return std::nullopt;
}

std::optional<std::string> Annotations::setVariable(const VariableDoc& v) {
    if (v.size == 0 || v.size > 0x4000) return "variable size must be between 1 and 16384 bytes";
    if (static_cast<unsigned>(v.address) + v.size > 0x10000) return "variable extends beyond 0xFFFF";
    if (auto err = claimName(v.address, v.name)) return err;
    std::erase_if(variables_, [&](const VariableDoc& o) { return o.address == v.address; });
    variables_.push_back(v);
    std::sort(variables_.begin(), variables_.end(), [](const auto& a, const auto& b) { return a.address < b.address; });
    return std::nullopt;
}

std::optional<std::string> Annotations::setData(const DataRegion& r) {
    if (r.length == 0 || static_cast<unsigned>(r.address) + r.length > 0x10000) return "invalid data region length";
    if ((r.kind == DataKind::Words || r.kind == DataKind::Pointers) && r.length % 2 != 0) return "word/pointer regions need an even length";
    if (!r.name.empty()) {
        if (auto err = claimName(r.address, r.name)) return err;
    }
    data_[r.address] = r;
    return std::nullopt;
}

bool Annotations::removeLabel(Word address) {
    auto it = labels_.find(address);
    if (it == labels_.end()) return false;
    byName_.erase(upperCase(it->second));
    labels_.erase(it);
    routines_.erase(address);
    std::erase_if(variables_, [&](const VariableDoc& v) { return v.address == address; });
    if (auto d = data_.find(address); d != data_.end()) d->second.name.clear();
    return true;
}

bool Annotations::removeComment(Word address) { return comments_.erase(address) + blockComments_.erase(address) > 0; }

void Annotations::clear() { *this = Annotations{}; }

std::optional<std::string> Annotations::nameAt(Word address) const {
    if (auto it = labels_.find(address); it != labels_.end()) return it->second;
    return std::nullopt;
}

std::optional<std::string> Annotations::expressionFor(Word address) const {
    if (auto exact = nameAt(address)) return exact;
    for (const auto& v : variables_) {
        if (address > v.address && address < v.address + v.size) return v.name + "+" + std::to_string(address - v.address);
    }
    for (const auto& [start, region] : data_) {
        if (!region.name.empty() && address > start && address < start + region.length) return region.name + "+" + std::to_string(address - start);
    }
    return std::nullopt;
}

std::optional<Word> Annotations::addressOf(const std::string& name) const {
    if (auto it = byName_.find(upperCase(name)); it != byName_.end()) return it->second;
    return std::nullopt;
}

const RoutineDoc* Annotations::routineAt(Word a) const {
    auto it = routines_.find(a);
    return it == routines_.end() ? nullptr : &it->second;
}
const std::string* Annotations::commentAt(Word a) const {
    auto it = comments_.find(a);
    return it == comments_.end() ? nullptr : &it->second;
}
const std::string* Annotations::blockCommentAt(Word a) const {
    auto it = blockComments_.find(a);
    return it == blockComments_.end() ? nullptr : &it->second;
}
const DataRegion* Annotations::dataAt(Word a) const {
    auto it = data_.find(a);
    return it == data_.end() ? nullptr : &it->second;
}

Json Annotations::toJson() const {
    Json j = Json::object();
    j.set("version", 1).set("title", title_);
    Json labels = Json::object();
    for (const auto& [a, name] : labels_) {
        if (routines_.count(a) == 0) {
            bool isVariable = std::any_of(variables_.begin(), variables_.end(), [&](const auto& v) { return v.address == a; });
            bool isData = data_.count(a) != 0 && data_.at(a).name == name;
            if (!isVariable && !isData) labels.set(hex4(a), name);
        }
    }
    j.set("labels", labels);
    Json routines = Json::array();
    for (const auto& [a, r] : routines_) {
        Json e = Json::object();
        e.set("address", hex4(a)).set("name", r.name).set("summary", r.summary).set("inputs", r.inputs).set("outputs", r.outputs).set("clobbers", r.clobbers);
        if (!r.confidence.empty()) e.set("confidence", r.confidence);
        routines.push(std::move(e));
    }
    j.set("routines", routines);
    Json vars = Json::array();
    for (const auto& v : variables_) {
        Json e = Json::object();
        e.set("address", hex4(v.address)).set("name", v.name).set("size", v.size).set("comment", v.comment);
        vars.push(std::move(e));
    }
    j.set("variables", vars);
    Json data = Json::array();
    for (const auto& [a, r] : data_) {
        Json e = Json::object();
        e.set("address", hex4(a)).set("length", r.length).set("kind", toString(r.kind)).set("name", r.name).set("comment", r.comment);
        data.push(std::move(e));
    }
    j.set("data", data);
    Json comments = Json::object();
    for (const auto& [a, text] : comments_) comments.set(hex4(a), text);
    j.set("comments", comments);
    Json blocks = Json::object();
    for (const auto& [a, text] : blockComments_) blocks.set(hex4(a), text);
    j.set("block_comments", blocks);
    return j;
}

Annotations Annotations::fromJson(const Json& j) {
    if (!j.isObject()) throw JsonError("annotations must be a JSON object");
    Annotations a;
    auto check = [](const std::optional<std::string>& err) {
        if (err) throw JsonError(*err);
    };
    a.title_ = strOr(j, "title");
    if (const Json* labels = j.find("labels"); labels && labels->isObject()) {
        for (const auto& [key, value] : labels->members()) check(a.setLabel(parseAddress(Json(key), "label"), value.asString()));
    }
    if (const Json* routines = j.find("routines"); routines && routines->isArray()) {
        for (const Json& e : routines->items()) {
            RoutineDoc d{strOr(e, "name"), strOr(e, "summary"), strOr(e, "inputs"), strOr(e, "outputs"), strOr(e, "clobbers"), strOr(e, "confidence")};
            check(a.setRoutine(parseAddress(*e.find("address"), "routine"), d));
        }
    }
    if (const Json* vars = j.find("variables"); vars && vars->isArray()) {
        for (const Json& e : vars->items()) {
            VariableDoc v;
            v.address = parseAddress(*e.find("address"), "variable");
            v.name = strOr(e, "name");
            v.size = e.find("size") ? static_cast<unsigned>(e.find("size")->asInt()) : 1u;
            v.comment = strOr(e, "comment");
            check(a.setVariable(v));
        }
    }
    if (const Json* data = j.find("data"); data && data->isArray()) {
        for (const Json& e : data->items()) {
            DataRegion r;
            r.address = parseAddress(*e.find("address"), "data");
            r.length = static_cast<unsigned>(e.find("length")->asInt());
            const auto kind = dataKindFromString(strOr(e, "kind"));
            if (!kind) throw JsonError("unknown data kind '" + strOr(e, "kind") + "'");
            r.kind = *kind;
            r.name = strOr(e, "name");
            r.comment = strOr(e, "comment");
            check(a.setData(r));
        }
    }
    if (const Json* c = j.find("comments"); c && c->isObject()) {
        for (const auto& [key, value] : c->members()) a.setComment(parseAddress(Json(key), "comment"), value.asString());
    }
    if (const Json* c = j.find("block_comments"); c && c->isObject()) {
        for (const auto& [key, value] : c->members()) a.setBlockComment(parseAddress(Json(key), "block comment"), value.asString());
    }
    return a;
}

}  // namespace z80
