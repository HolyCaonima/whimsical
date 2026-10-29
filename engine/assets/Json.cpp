#include "Json.h"
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace whimsical {
Json::Json(double v) : value_(v) {
    if (!std::isfinite(v))
        throw std::invalid_argument("JSON number must be finite");
}
uint32_t Json::uint() const {
    double n = number();
    if (n < 0 || n > UINT32_MAX || n != std::floor(n))
        throw std::invalid_argument("Expected unsigned integer");
    return uint32_t(n);
}
namespace {
// RFC 8259 text; the semantics of ECMAScript JSON.parse.
class Parser {
    const char *begin_, *p_, *end_;
    [[noreturn]] void fail(const char* what) const {
        throw std::invalid_argument("Invalid JSON: " + std::string(what) + " at offset " +
                                    std::to_string(p_ - begin_));
    }
    void space() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r'))
            ++p_;
    }
    void expect(char c) {
        space();
        if (p_ == end_ || *p_ != c)
            fail("unexpected character");
        ++p_;
    }
    void literal(const char* word) {
        const auto n = std::strlen(word);
        if (size_t(end_ - p_) < n || std::memcmp(p_, word, n) != 0)
            fail("unexpected character");
        p_ += n;
    }
    uint32_t hex4() {
        if (end_ - p_ < 4)
            fail("truncated escape");
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i, ++p_) {
            const char c = *p_;
            value = value * 16 + (c >= '0' && c <= '9'   ? c - '0'
                                  : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                  : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                         : (fail("invalid escape"), 0));
        }
        return value;
    }
    static void utf8(std::string& out, uint32_t code) {
        if (code < 0x80)
            out += char(code);
        else if (code < 0x800) {
            out += char(0xC0 | code >> 6);
            out += char(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += char(0xE0 | code >> 12);
            out += char(0x80 | (code >> 6 & 0x3F));
            out += char(0x80 | (code & 0x3F));
        } else {
            out += char(0xF0 | code >> 18);
            out += char(0x80 | (code >> 12 & 0x3F));
            out += char(0x80 | (code >> 6 & 0x3F));
            out += char(0x80 | (code & 0x3F));
        }
    }
    std::string string() {
        expect('"');
        std::string out;
        for (;;) {
            const char* run = p_;
            while (p_ < end_ && *p_ != '"' && *p_ != '\\' && uint8_t(*p_) >= 0x20)
                ++p_;
            out.append(run, p_);
            if (p_ == end_)
                fail("unterminated string");
            const char c = *p_++;
            if (c == '"')
                return out;
            if (c != '\\')
                fail("control character in string");
            if (p_ == end_)
                fail("unterminated string");
            switch (*p_++) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                auto code = hex4();
                if (code >= 0xD800 && code < 0xDC00 && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                    const auto mark = p_;
                    p_ += 2;
                    const auto low = hex4();
                    if (low >= 0xDC00 && low < 0xE000)
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    else
                        p_ = mark;
                }
                utf8(out, code);
                break;
            }
            default:
                fail("invalid escape");
            }
        }
    }
    Json number() {
        const char* first = p_;
        auto digits = [&] {
            const char* start = p_;
            while (p_ < end_ && *p_ >= '0' && *p_ <= '9')
                ++p_;
            if (p_ == start)
                fail("invalid number");
        };
        if (p_ < end_ && *p_ == '-')
            ++p_;
        if (p_ < end_ && *p_ == '0')
            ++p_;
        else
            digits();
        if (p_ < end_ && *p_ == '.') {
            ++p_;
            digits();
        }
        if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
            ++p_;
            if (p_ < end_ && (*p_ == '+' || *p_ == '-'))
                ++p_;
            digits();
        }
        double value = 0;
        if (std::from_chars(first, p_, value).ec != std::errc())
            value = std::strtod(std::string(first, p_).c_str(), nullptr); // Out of range: ±inf or 0.
        return value;
    }
    Json value(int depth) {
        if (depth > 1000)
            fail("nesting too deep");
        space();
        if (p_ == end_)
            fail("unexpected end");
        switch (*p_) {
        case '{': {
            ++p_;
            Json::Object members;
            space();
            if (p_ < end_ && *p_ == '}') {
                ++p_;
                return members;
            }
            for (;;) {
                auto key = string();
                expect(':');
                members.insert_or_assign(std::move(key), value(depth + 1));
                space();
                if (p_ < end_ && *p_ == ',') {
                    ++p_;
                    continue;
                }
                expect('}');
                return members;
            }
        }
        case '[': {
            ++p_;
            Json::Array elements;
            space();
            if (p_ < end_ && *p_ == ']') {
                ++p_;
                return elements;
            }
            for (;;) {
                elements.push_back(value(depth + 1));
                space();
                if (p_ < end_ && *p_ == ',') {
                    ++p_;
                    continue;
                }
                expect(']');
                return elements;
            }
        }
        case '"':
            return string();
        case 't':
            literal("true");
            return true;
        case 'f':
            literal("false");
            return false;
        case 'n':
            literal("null");
            return Json();
        default:
            return number();
        }
    }

  public:
    explicit Parser(const std::string& text) : begin_(text.data()), p_(begin_), end_(begin_ + text.size()) {}
    Json document() {
        auto result = value(0);
        space();
        if (p_ != end_)
            fail("trailing characters");
        return result;
    }
};
// ECMAScript Number::toString: the shortest round-trip digits, positioned by exponent.
void writeNumber(std::string& out, double v) {
    if (v == 0) {
        out += '0';
        return;
    }
    char text[32];
    const auto end = std::to_chars(text, text + sizeof(text), v, std::chars_format::scientific).ptr;
    const char* p = text;
    if (*p == '-') {
        out += '-';
        ++p;
    }
    char digits[20];
    int k = 0;
    for (; *p != 'e'; ++p)
        if (*p != '.')
            digits[k++] = *p;
    const int n = std::atoi(std::string(p + 1, static_cast<const char*>(end)).c_str()) + 1;
    if (k <= n && n <= 21) {
        out.append(digits, k);
        out.append(size_t(n - k), '0');
    } else if (0 < n && n <= 21) {
        out.append(digits, n);
        out += '.';
        out.append(digits + n, k - n);
    } else if (-6 < n && n <= 0) {
        out += "0.";
        out.append(size_t(-n), '0');
        out.append(digits, k);
    } else {
        out += digits[0];
        if (k > 1) {
            out += '.';
            out.append(digits + 1, k - 1);
        }
        out += n - 1 < 0 ? "e-" : "e+";
        out += std::to_string(std::abs(n - 1));
    }
}
void writeString(std::string& out, const std::string& s) {
    static const char hex[] = "0123456789abcdef";
    out += '"';
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (uint8_t(c) < 0x20) {
                out += "\\u00";
                out += hex[uint8_t(c) >> 4];
                out += hex[uint8_t(c) & 15];
            } else
                out += c;
        }
    }
    out += '"';
}
void write(std::string& out, const Json& j) {
    switch (j.value().index()) {
    case 0:
        out += "null";
        break;
    case 1:
        out += j.boolean() ? "true" : "false";
        break;
    case 2:
        writeNumber(out, j.number());
        break;
    case 3:
        writeString(out, j.string());
        break;
    case 4: {
        out += '[';
        bool first = true;
        for (const auto& item : j.elements()) {
            if (!first)
                out += ',';
            first = false;
            write(out, item);
        }
        out += ']';
        break;
    }
    case 5: {
        out += '{';
        bool first = true;
        for (const auto& [key, item] : j.members()) {
            if (!first)
                out += ',';
            first = false;
            writeString(out, key);
            out += ':';
            write(out, item);
        }
        out += '}';
        break;
    }
    }
}
} // namespace
Json Json::parse(const std::string& text) {
    return Parser(text).document();
}
std::string Json::dump() const {
    std::string out;
    write(out, *this);
    return out;
}
} // namespace whimsical
