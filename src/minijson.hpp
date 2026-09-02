#pragma once
// minijson.hpp - tiny JSON parser (no dependencies, UTF-8 strings).
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace mjson {

struct Value {
    enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<Value> arr;
    std::map<std::string, Value> obj;

    const Value* get(const std::string& key) const {
        auto it = obj.find(key);
        return it == obj.end() ? nullptr : &it->second;
    }
    bool is_null() const { return t == T::Null; }
    double num_or(double d) const { return t == T::Num ? num : d; }
    std::string str_or(const std::string& d) const { return t == T::Str ? str : d; }
    bool bool_or(bool d) const { return t == T::Bool ? b : d; }
};

namespace detail {

struct Parser {
    const std::string& s;
    size_t i = 0;
    std::string err;
    // Guards against stack exhaustion from maliciously deep nesting.
    static const int kMaxDepth = 512;

    explicit Parser(const std::string& text) : s(text) {}

    void skip_ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
            i++;
    }

    bool fail(const std::string& msg) { err = msg; return false; }

    bool parse(Value& out) { return parse_d(out, 0); }

    bool parse_d(Value& out, int depth) {
        skip_ws();
        if (i >= s.size()) return fail("empty input");
        if (depth > kMaxDepth) return fail("nesting too deep");
        char c = s[i];
        if (c == '{') return parse_obj(out, depth);
        if (c == '[') return parse_arr(out, depth);
        if (c == '"') return parse_str(out.str), out.t = Value::T::Str, true;
        if (c == 't' || c == 'f') {
            if (s.compare(i, 4, "true") == 0) { i += 4; out.t = Value::T::Bool; out.b = true; return true; }
            if (s.compare(i, 5, "false") == 0) { i += 5; out.t = Value::T::Bool; out.b = false; return true; }
            return fail("invalid literal");
        }
        if (c == 'n') {
            if (s.compare(i, 4, "null") == 0) { i += 4; out.t = Value::T::Null; return true; }
            return fail("invalid literal");
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            size_t start = i;
            if (c == '-') i++;
            while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.' ||
                                    s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-'))
                i++;
            out.t = Value::T::Num;
            out.num = std::strtod(s.substr(start, i - start).c_str(), nullptr);
            return true;
        }
        return fail(std::string("unexpected char '") + c + "'");
    }

    void parse_str(std::string& out) {
        i++;  // opening quote
        out.clear();
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\') {
                i++;
                if (i >= s.size()) break;
                char e = s[i++];
                auto read_hex4 = [&]() -> unsigned {
                    unsigned cp = 0;
                    for (int k = 0; k < 4 && i < s.size(); k++, i++) {
                        char h = s[i];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f')
                            cp |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F')
                            cp |= (unsigned)(h - 'A' + 10);
                    }
                    return cp;
                };
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        unsigned cp = read_hex4();
                        // Combine UTF-16 surrogate pairs (\uD800-\uDBFF
                        // followed by \uDC00-\uDFFF) into one code point.
                        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() &&
                            s[i] == '\\' && s[i + 1] == 'u') {
                            i += 2;
                            unsigned lo = read_hex4();
                            if (lo >= 0xDC00 && lo <= 0xDFFF)
                                cp = 0x10000 + ((cp - 0xD800) << 10) +
                                     (lo - 0xDC00);
                        }
                        // encode UTF-8
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else if (cp < 0x10000) {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else {
                            out += (char)(0xF0 | (cp >> 18));
                            out += (char)(0x80 | ((cp >> 12) & 0x3F));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: out += e; break;
                }
            } else {
                out += s[i++];
            }
        }
        if (i < s.size()) i++;  // closing quote
    }

    bool parse_arr(Value& out, int depth) {
        out.t = Value::T::Arr;
        i++;  // '['
        skip_ws();
        if (i < s.size() && s[i] == ']') { i++; return true; }
        while (true) {
            skip_ws();
            Value v;
            if (!parse_d(v, depth + 1)) return false;
            out.arr.push_back(std::move(v));
            skip_ws();
            if (i >= s.size()) return fail("unterminated array");
            if (s[i] == ',') { i++; continue; }
            if (s[i] == ']') { i++; return true; }
            return fail("expected ',' or ']'");
        }
    }

    bool parse_obj(Value& out, int depth) {
        out.t = Value::T::Obj;
        i++;  // '{'
        skip_ws();
        if (i < s.size() && s[i] == '}') { i++; return true; }
        while (true) {
            skip_ws();
            if (i >= s.size() || s[i] != '"') return fail("expected string key");
            std::string key;
            parse_str(key);
            skip_ws();
            if (i >= s.size() || s[i] != ':') return fail("expected ':'");
            i++;
            skip_ws();
            Value v;
            if (!parse_d(v, depth + 1)) return false;
            out.obj[key] = std::move(v);
            skip_ws();
            if (i >= s.size()) return fail("unterminated object");
            if (s[i] == ',') { i++; continue; }
            if (s[i] == '}') { i++; return true; }
            return fail("expected ',' or '}'");
        }
    }
};

}  // namespace detail

inline bool parse(const std::string& text, Value& out, std::string* err = nullptr) {
    detail::Parser p(text);
    bool ok = p.parse(out);
    if (ok) {
        p.skip_ws();
        ok = p.i == p.s.size();
        if (!ok) p.err = "trailing characters after JSON value";
    }
    if (!ok && err) *err = p.err;
    return ok;
}

}  // namespace mjson
