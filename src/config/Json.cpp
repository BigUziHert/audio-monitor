#include "config/Json.h"

#include <charconv>
#include <cstdio>

namespace audiomon {
namespace {

void escapeTo(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);   // UTF-8 passes through
                }
        }
    }
    out += '"';
}

// Encodes a code point as UTF-8.
void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

struct Parser {
    const std::string& s;
    size_t             i = 0;
    std::string        err;

    explicit Parser(const std::string& src) : s(src) {}

    void skipWs() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool fail(const char* m) { if (err.empty()) err = m; return false; }

    bool parseValue(JsonValue& out, int depth) {
        if (depth > 64) return fail("nesting too deep");
        skipWs();
        if (i >= s.size()) return fail("unexpected end of input");

        switch (s[i]) {
            case '{': return parseObject(out, depth);
            case '[': return parseArray(out, depth);
            case '"': {
                std::string str;
                if (!parseString(str)) return false;
                out = JsonValue(str);
                return true;
            }
            case 't':
                if (s.compare(i, 4, "true") == 0) { i += 4; out = JsonValue(true);  return true; }
                return fail("bad literal");
            case 'f':
                if (s.compare(i, 5, "false") == 0) { i += 5; out = JsonValue(false); return true; }
                return fail("bad literal");
            case 'n':
                if (s.compare(i, 4, "null") == 0) { i += 4; out = JsonValue(); return true; }
                return fail("bad literal");
            default:  return parseNumber(out);
        }
    }

    bool parseNumber(JsonValue& out) {
        const size_t start = i;
        const auto isDigit = [this] { return i < s.size() && s[i] >= '0' && s[i] <= '9'; };
        if (i < s.size() && s[i] == '-') ++i;
        if (!isDigit()) return fail("expected a number");
        if (s[i] == '0') {
            ++i;
            if (isDigit()) return fail("leading zero in number");
        } else {
            while (isDigit()) ++i;
        }
        if (i < s.size() && s[i] == '.') {
            ++i;
            if (!isDigit()) return fail("expected fraction digits");
            while (isDigit()) ++i;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
            if (!isDigit()) return fail("expected exponent digits");
            while (isDigit()) ++i;
        }
        double number = 0;
        const auto result = std::from_chars(s.data() + start, s.data() + i, number);
        if (result.ec != std::errc{} || result.ptr != s.data() + i)
            return fail("number out of range");
        out = JsonValue(number);
        return true;
    }

    bool parseHex4(unsigned& value) {
        if (i + 4 > s.size()) return fail("truncated \\u escape");
        value = 0;
        for (int digit = 0; digit < 4; ++digit) {
            const char c = s[i++];
            unsigned hex;
            if (c >= '0' && c <= '9') hex = c - '0';
            else if (c >= 'a' && c <= 'f') hex = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') hex = c - 'A' + 10;
            else return fail("invalid hex digit in \\u escape");
            value = (value << 4) | hex;
        }
        return true;
    }

    bool parseString(std::string& out) {
        if (i >= s.size() || s[i] != '"') return fail("expected string");
        ++i;
        out.clear();
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("unescaped control character");
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) break;
            const char e = s[i++];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'u': {
                    unsigned cp;
                    if (!parseHex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (i + 2 > s.size() || s[i] != '\\' || s[i + 1] != 'u')
                            return fail("missing low surrogate");
                        i += 2;
                        unsigned lo;
                        if (!parseHex4(lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return fail("invalid low surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("unpaired low surrogate");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool parseArray(JsonValue& out, int depth) {
        ++i;                       // consume '['
        out = JsonValue::array();
        skipWs();
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        for (;;) {
            JsonValue v;
            if (!parseValue(v, depth + 1)) return false;
            out.push(v);
            skipWs();
            if (i >= s.size()) return fail("unterminated array");
            if (s[i] == ',') { ++i; continue; }
            if (s[i] == ']') { ++i; return true; }
            return fail("expected , or ] in array");
        }
    }

    bool parseObject(JsonValue& out, int depth) {
        ++i;                       // consume '{'
        out = JsonValue::object();
        skipWs();
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        for (;;) {
            skipWs();
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (i >= s.size() || s[i] != ':') return fail("expected : after key");
            ++i;
            JsonValue v;
            if (!parseValue(v, depth + 1)) return false;
            out.set(key, v);
            skipWs();
            if (i >= s.size()) return fail("unterminated object");
            if (s[i] == ',') { ++i; continue; }
            if (s[i] == '}') { ++i; return true; }
            return fail("expected , or } in object");
        }
    }
};

} // namespace

bool JsonValue::asBool(bool def) const {
    if (type_ == Type::Bool)   return bool_;
    if (type_ == Type::Number) return num_ != 0.0;
    return def;
}

double JsonValue::asNumber(double def) const {
    if (type_ == Type::Number) return num_;
    if (type_ == Type::Bool)   return bool_ ? 1.0 : 0.0;
    return def;
}

std::string JsonValue::asString(const std::string& def) const {
    return type_ == Type::String ? str_ : def;
}

const JsonValue* JsonValue::find(const std::string& key) const {
    if (type_ != Type::Object) return nullptr;
    for (const auto& kv : obj_) if (kv.first == key) return &kv.second;
    return nullptr;
}

void JsonValue::set(const std::string& key, JsonValue v) {
    if (type_ != Type::Object) { type_ = Type::Object; obj_.clear(); }
    for (auto& kv : obj_) {
        if (kv.first == key) { kv.second = std::move(v); return; }
    }
    obj_.emplace_back(key, std::move(v));
}

void JsonValue::push(JsonValue v) {
    if (type_ != Type::Array) { type_ = Type::Array; arr_.clear(); }
    arr_.push_back(std::move(v));
}

void JsonValue::dumpTo(std::string& out, int indent, int depth) const {
    const std::string pad  (static_cast<size_t>(indent) * (depth + 1), ' ');
    const std::string close(static_cast<size_t>(indent) * depth, ' ');
    const char* nl = indent > 0 ? "\n" : "";

    switch (type_) {
        case Type::Null:   out += "null"; break;
        case Type::Bool:   out += bool_ ? "true" : "false"; break;
        case Type::Number: {
            char buf[40];
            // %.10g keeps a fader value readable rather than 0.69999999999
            std::snprintf(buf, sizeof(buf), "%.10g", num_);
            out += buf;
            break;
        }
        case Type::String: escapeTo(out, str_); break;
        case Type::Array: {
            if (arr_.empty()) { out += "[]"; break; }
            out += '[';
            out += nl;
            for (size_t k = 0; k < arr_.size(); ++k) {
                out += pad;
                arr_[k].dumpTo(out, indent, depth + 1);
                if (k + 1 < arr_.size()) out += ',';
                out += nl;
            }
            out += close;
            out += ']';
            break;
        }
        case Type::Object: {
            if (obj_.empty()) { out += "{}"; break; }
            out += '{';
            out += nl;
            for (size_t k = 0; k < obj_.size(); ++k) {
                out += pad;
                escapeTo(out, obj_[k].first);
                out += indent > 0 ? ": " : ":";
                obj_[k].second.dumpTo(out, indent, depth + 1);
                if (k + 1 < obj_.size()) out += ',';
                out += nl;
            }
            out += close;
            out += '}';
            break;
        }
    }
}

std::string JsonValue::dump(int indent) const {
    std::string out;
    out.reserve(1024);
    dumpTo(out, indent, 0);
    if (indent > 0) out += '\n';
    return out;
}

JsonValue JsonValue::parse(const std::string& text, std::string* error) {
    Parser    p(text);
    JsonValue v;
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        p.i = 3;
    }
    if (!p.parseValue(v, 0)) {
        if (error) *error = p.err.empty() ? "parse error" : p.err;
        return JsonValue();
    }
    p.skipWs();
    if (p.i != text.size()) {
        if (error) *error = "trailing data after JSON value";
        return JsonValue();
    }
    if (error) error->clear();
    return v;
}

} // namespace audiomon
