#include "z80/format.hpp"

#include <algorithm>
#include <cctype>

namespace z80 {

std::string hexDigits(unsigned value, int digits) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    do {
        out.insert(out.begin(), kDigits[value & 0xF]);
        value >>= 4;
    } while (value != 0);
    while (static_cast<int>(out.size()) < digits) {
        out.insert(out.begin(), '0');
    }
    return out;
}

std::string formatHex(unsigned value, int digits, NumberStyle style) {
    const std::string h = hexDigits(value, digits);
    switch (style) {
        case NumberStyle::Ampersand:
            return "&" + h;
        case NumberStyle::Dollar:
            return "$" + h;
        case NumberStyle::CPrefix:
            return "0x" + h;
        case NumberStyle::HSuffix:
            return (std::isalpha(static_cast<unsigned char>(h.front())) ? "0" : "") + h + "H";
    }
    return h;
}

bool parseNumberStyle(const std::string& name, NumberStyle& out) {
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (n == "amp" || n == "ampersand" || n == "&") {
        out = NumberStyle::Ampersand;
    } else if (n == "dollar" || n == "$") {
        out = NumberStyle::Dollar;
    } else if (n == "c" || n == "0x") {
        out = NumberStyle::CPrefix;
    } else if (n == "h" || n == "suffix") {
        out = NumberStyle::HSuffix;
    } else {
        return false;
    }
    return true;
}

}  // namespace z80
