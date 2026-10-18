// gh_json.h -- a small self-contained JSON reader (Step 1 of the library-source
// plan: per-title knowledge as data, greenhouse.title/1).
//
// Deliberately minimal and dependency-free: no exceptions thrown by the parser,
// no WinRT, no Win32 -- so it is safe at early-knob time on any thread, before
// a game is chosen, and it builds on the Linux host for toolchain/test_gh_json.cpp.
//
// Covers RFC 8259 JSON: objects (member order kept, duplicate keys reported),
// arrays, strings with every escape including \uXXXX surrogate pairs (decoded to
// UTF-8), numbers, true/false/null. Numbers keep an exact int64 when they are
// integers in range; anything else is a double with isInt=false. Depth is capped
// so a hostile file cannot overflow the stack.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace ghjson {

enum class Type : uint8_t { Null, Bool, Num, Str, Arr, Obj };

struct Value {
    Type type = Type::Null;
    bool b = false;
    bool isInt = false;         // Num: an integer that fits int64 exactly
    int64_t i = 0;              // Num: valid when isInt
    double d = 0;               // Num: always set
    std::string s;              // Str (UTF-8)
    std::vector<Value> arr;     // Arr
    std::vector<std::pair<std::string, Value>> obj;   // Obj, in file order

    bool isNull() const { return type == Type::Null; }
    bool isObj()  const { return type == Type::Obj; }
    bool isArr()  const { return type == Type::Arr; }
    bool isStr()  const { return type == Type::Str; }
    bool isNum()  const { return type == Type::Num; }
    bool isBool() const { return type == Type::Bool; }

    // Object member lookup (first match), or nullptr.
    const Value* get(const char* key) const {
        if (type != Type::Obj) return nullptr;
        for (const auto& m : obj) if (m.first == key) return &m.second;
        return nullptr;
    }
};

inline const char* TypeName(Type t)
{
    switch (t){
    case Type::Null: return "null";   case Type::Bool: return "bool";
    case Type::Num:  return "number"; case Type::Str:  return "string";
    case Type::Arr:  return "array";  case Type::Obj:  return "object";
    }
    return "?";
}

class Parser {
public:
    static constexpr int kMaxDepth = 64;

    // Parse the whole buffer. On failure returns false and sets err to
    // "line L col C: <what>". A UTF-8 BOM is skipped.
    static bool Parse(const char* p, size_t n, Value& out, std::string& err)
    {
        Parser ps(p, n);
        if (n >= 3 && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) ps.pos_ = 3;
        ps.ws();
        if (!ps.value(out, 0)){ err = ps.where() + ps.err_; return false; }
        ps.ws();
        if (ps.pos_ != ps.n_){ err = ps.where() + "trailing characters after the value"; return false; }
        return true;
    }

private:
    Parser(const char* p, size_t n) : p_(p), n_(n) {}
    const char* p_; size_t n_; size_t pos_ = 0; std::string err_;

    std::string where() const
    {
        size_t line = 1, col = 1;
        for (size_t k = 0; k < pos_ && k < n_; ++k){ if (p_[k] == '\n'){ ++line; col = 1; } else ++col; }
        return "line " + std::to_string(line) + " col " + std::to_string(col) + ": ";
    }
    bool fail(const char* m){ if (err_.empty()) err_ = m; return false; }
    bool eof() const { return pos_ >= n_; }
    char peek() const { return eof() ? '\0' : p_[pos_]; }
    void ws(){ while (!eof() && (p_[pos_]==' '||p_[pos_]=='\t'||p_[pos_]=='\n'||p_[pos_]=='\r')) ++pos_; }
    bool lit(const char* w)
    {
        size_t L = strlen(w);
        if (n_ - pos_ < L || memcmp(p_ + pos_, w, L) != 0) return fail("invalid literal");
        pos_ += L; return true;
    }

    bool value(Value& v, int depth)
    {
        if (depth > kMaxDepth) return fail("nesting too deep");
        if (eof()) return fail("unexpected end of input");
        switch (peek()){
        case '{': return object(v, depth);
        case '[': return array(v, depth);
        case '"': v.type = Type::Str; return string(v.s);
        case 't': v.type = Type::Bool; v.b = true;  return lit("true");
        case 'f': v.type = Type::Bool; v.b = false; return lit("false");
        case 'n': v.type = Type::Null; return lit("null");
        default:  return number(v);
        }
    }

    bool object(Value& v, int depth)
    {
        v.type = Type::Obj; ++pos_; ws();
        if (peek() == '}'){ ++pos_; return true; }
        for (;;){
            if (peek() != '"') return fail("expected a string key");
            std::string key;
            if (!string(key)) return false;
            for (const auto& m : v.obj) if (m.first == key) return fail("duplicate key");
            ws();
            if (peek() != ':') return fail("expected ':'");
            ++pos_; ws();
            v.obj.emplace_back(std::move(key), Value());
            if (!value(v.obj.back().second, depth + 1)) return false;
            ws();
            if (peek() == ','){ ++pos_; ws(); continue; }
            if (peek() == '}'){ ++pos_; return true; }
            return fail("expected ',' or '}'");
        }
    }

    bool array(Value& v, int depth)
    {
        v.type = Type::Arr; ++pos_; ws();
        if (peek() == ']'){ ++pos_; return true; }
        for (;;){
            v.arr.emplace_back();
            if (!value(v.arr.back(), depth + 1)) return false;
            ws();
            if (peek() == ','){ ++pos_; ws(); continue; }
            if (peek() == ']'){ ++pos_; return true; }
            return fail("expected ',' or ']'");
        }
    }

    static void utf8(std::string& o, uint32_t c)
    {
        if (c < 0x80) o += (char)c;
        else if (c < 0x800){ o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000){ o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
        else { o += (char)(0xF0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 0x3F));
               o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
    }
    bool hex4(uint32_t& c)
    {
        if (n_ - pos_ < 4) return fail("truncated \\u escape");
        c = 0;
        for (int k = 0; k < 4; ++k){
            char h = p_[pos_++]; c <<= 4;
            if (h >= '0' && h <= '9') c |= (uint32_t)(h - '0');
            else if (h >= 'a' && h <= 'f') c |= (uint32_t)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') c |= (uint32_t)(h - 'A' + 10);
            else return fail("bad hex digit in \\u escape");
        }
        return true;
    }

    bool string(std::string& o)
    {
        ++pos_;   // opening quote
        for (;;){
            if (eof()) return fail("unterminated string");
            unsigned char c = (unsigned char)p_[pos_++];
            if (c == '"') return true;
            if (c < 0x20) return fail("control character in string");
            if (c != '\\'){ o += (char)c; continue; }
            if (eof()) return fail("unterminated escape");
            char e = p_[pos_++];
            switch (e){
            case '"': o += '"'; break;   case '\\': o += '\\'; break;  case '/': o += '/'; break;
            case 'b': o += '\b'; break;  case 'f': o += '\f'; break;   case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;  case 't': o += '\t'; break;
            case 'u': {
                uint32_t cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF){
                    if (n_ - pos_ < 2 || p_[pos_] != '\\' || p_[pos_ + 1] != 'u') return fail("unpaired high surrogate");
                    pos_ += 2;
                    uint32_t lo;
                    if (!hex4(lo)) return false;
                    if (lo < 0xDC00 || lo > 0xDFFF) return fail("bad low surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) return fail("unpaired low surrogate");
                utf8(o, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
    }

    bool number(Value& v)
    {
        const size_t start = pos_;
        bool neg = false, frac = false;
        if (peek() == '-'){ neg = true; ++pos_; }
        if (peek() == '0') ++pos_;
        else if (peek() >= '1' && peek() <= '9'){ while (peek() >= '0' && peek() <= '9') ++pos_; }
        else return fail("invalid value");
        if (peek() == '.'){
            frac = true; ++pos_;
            if (!(peek() >= '0' && peek() <= '9')) return fail("digit expected after '.'");
            while (peek() >= '0' && peek() <= '9') ++pos_;
        }
        if (peek() == 'e' || peek() == 'E'){
            frac = true; ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            if (!(peek() >= '0' && peek() <= '9')) return fail("digit expected in exponent");
            while (peek() >= '0' && peek() <= '9') ++pos_;
        }
        std::string t(p_ + start, pos_ - start);
        v.type = Type::Num;
        v.d = strtod(t.c_str(), nullptr);
        v.isInt = false;
        if (!frac){
            // Exact int64 parse with overflow detection (strtoll's ERANGE is errno-based).
            uint64_t mag = 0; bool ok = true;
            for (size_t k = neg ? 1 : 0; k < t.size(); ++k){
                uint64_t dgt = (uint64_t)(t[k] - '0');
                if (mag > (UINT64_MAX - dgt) / 10){ ok = false; break; }
                mag = mag * 10 + dgt;
            }
            if (ok && !neg && mag <= (uint64_t)INT64_MAX){ v.isInt = true; v.i = (int64_t)mag; }
            else if (ok && neg && mag <= (uint64_t)INT64_MAX + 1){ v.isInt = true; v.i = (int64_t)(0 - mag); }
        }
        return true;
    }
};

inline bool Parse(const char* p, size_t n, Value& out, std::string& err)
{
    out = Value();
    return Parser::Parse(p, n, out, err);
}
inline bool Parse(const std::string& s, Value& out, std::string& err)
{
    return Parse(s.data(), s.size(), out, err);
}

} // namespace ghjson
