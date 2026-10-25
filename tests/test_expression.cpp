#include "test_util.hpp"
#include "z80/expression.hpp"

using z80::parseConstant;

static void number(const char* text, long long expected) {
    const auto v = parseConstant(text);
    ++g_checks;
    if (!v || *v != expected) {
        ++g_failures;
        std::cerr << "expression '" << text << "' -> " << (v ? std::to_string(*v) : "error")
                  << ", expected " << expected << "\n";
    }
}

static void invalid(const char* text) {
    ++g_checks;
    if (parseConstant(text)) {
        ++g_failures;
        std::cerr << "expression '" << text << "' should be rejected\n";
    }
}

int main() {
    // literals
    number("123", 123);
    number("&1F", 31);
    number("&H1F", 31);
    number("&hff", 255);
    number("$1F", 31);
    number("#1f", 31);
    number("0x1F", 31);
    number("1Fh", 31);
    number("0FFh", 255);
    number("%1010", 10);
    number("0b1010", 10);
    number("1010b", 10);
    number("'A'", 65);
    number("0", 0);

    // operators and precedence
    number("1+2*3", 7);
    number("(1+2)*3", 9);
    number("10-4-3", 3);
    number("20/3", 6);
    number("20%6", 2);
    number("1<<4", 16);
    number("&FF>>4", 15);
    number("&F0 & &3C", 0x30);
    number("&F0 | &0F", 0xFF);
    number("&FF ^ &0F", 0xF0);
    number("~0 & &FF", 255);
    number("-5+10", 5);
    number("+7", 7);
    number("HIGH(&1234)", 0x12);
    number("LOW(&1234)", 0x34);
    number(" 1 + 2 ", 3);

    // program counter and symbols
    {
        z80::ExpressionEvaluator ev(
            [](const std::string& name) -> std::optional<std::int64_t> {
                if (name == "FOO") return 100;
                return std::nullopt;
            },
            0x8000);
        CHECK_EQ(ev.evaluate("$").value, 0x8000);
        CHECK_EQ(ev.evaluate("$+5").value, 0x8005);
        CHECK_EQ(ev.evaluate("foo*2").value, 200);  // case-insensitive
        const auto r = ev.evaluate("bar+1");
        CHECK(r.ok);
        CHECK_EQ(r.undefinedNames.size(), std::size_t{1});
        CHECK_EQ(r.undefinedNames[0], std::string("BAR"));
    }

    // errors
    invalid("");
    invalid("1+");
    invalid("(1+2");
    invalid("1/0");
    invalid("12G");
    invalid("&");
    invalid("1 2");
    invalid("foo");  // undefined symbol is not a constant

    return finish("expression");
}
