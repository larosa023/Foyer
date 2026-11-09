// Foyer - tiny JSON reader/writer (enough for settings, pins and themes)
#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>

namespace json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::wstring s;
    std::vector<Value> arr;
    std::vector<std::pair<std::wstring, Value>> obj;   // keeps key order

    static Value MakeObject() { Value v; v.type = Object; return v; }
    static Value MakeArray()  { Value v; v.type = Array;  return v; }
    static Value Str(const std::wstring& x) { Value v; v.type = String; v.s = x; return v; }
    static Value Num(double x) { Value v; v.type = Number; v.n = x; return v; }
    static Value Boolean(bool x) { Value v; v.type = Bool; v.b = x; return v; }

    const Value* Get(const std::wstring& key) const {
        if (type != Object) return nullptr;
        for (auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    void Set(const std::wstring& key, Value v) {
        type = Object;
        for (auto& kv : obj) if (kv.first == key) { kv.second = std::move(v); return; }
        obj.emplace_back(key, std::move(v));
    }
    std::wstring GetStr(const std::wstring& key, const std::wstring& def = L"") const {
        const Value* v = Get(key); return (v && v->type == String) ? v->s : def;
    }
    bool GetBool(const std::wstring& key, bool def) const {
        const Value* v = Get(key); return (v && v->type == Bool) ? v->b : def;
    }
    double GetNum(const std::wstring& key, double def) const {
        const Value* v = Get(key); return (v && v->type == Number) ? v->n : def;
    }
};

class Parser {
public:
    explicit Parser(const std::wstring& t) : t_(t) {}
    bool Parse(Value& out) { Ws(); if (!ParseValue(out)) return false; Ws(); return i_ == t_.size(); }
private:
    const std::wstring& t_;
    size_t i_ = 0;
    void Ws() { while (i_ < t_.size() && (t_[i_] == L' ' || t_[i_] == L'\t' || t_[i_] == L'\r' || t_[i_] == L'\n')) ++i_; }
    bool Lit(const wchar_t* w) {
        size_t n = wcslen(w);
        if (t_.compare(i_, n, w) == 0) { i_ += n; return true; }
        return false;
    }
    bool ParseValue(Value& v) {
        if (i_ >= t_.size()) return false;
        wchar_t c = t_[i_];
        if (c == L'{') return ParseObject(v);
        if (c == L'[') return ParseArray(v);
        if (c == L'"') { v.type = Value::String; return ParseString(v.s); }
        if (Lit(L"true")) { v.type = Value::Bool; v.b = true; return true; }
        if (Lit(L"false")) { v.type = Value::Bool; v.b = false; return true; }
        if (Lit(L"null")) { v.type = Value::Null; return true; }
        return ParseNumber(v);
    }
    bool ParseNumber(Value& v) {
        size_t start = i_;
        while (i_ < t_.size() && wcschr(L"+-0123456789.eE", t_[i_])) ++i_;
        if (i_ == start) return false;
        v.type = Value::Number;
        v.n = wcstod(t_.substr(start, i_ - start).c_str(), nullptr);
        return true;
    }
    static int Hex(wchar_t c) {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    }
    bool ParseString(std::wstring& s) {
        ++i_; // opening quote
        while (i_ < t_.size()) {
            wchar_t c = t_[i_++];
            if (c == L'"') return true;
            if (c == L'\\') {
                if (i_ >= t_.size()) return false;
                wchar_t e = t_[i_++];
                switch (e) {
                    case L'"': s += L'"'; break;
                    case L'\\': s += L'\\'; break;
                    case L'/': s += L'/'; break;
                    case L'b': s += L'\b'; break;
                    case L'f': s += L'\f'; break;
                    case L'n': s += L'\n'; break;
                    case L'r': s += L'\r'; break;
                    case L't': s += L'\t'; break;
                    case L'u': {
                        if (i_ + 4 > t_.size()) return false;
                        int code = 0;
                        for (int k = 0; k < 4; ++k) { int h = Hex(t_[i_++]); if (h < 0) return false; code = code * 16 + h; }
                        s += (wchar_t)code;
                        break;
                    }
                    default: return false;
                }
            } else {
                s += c;
            }
        }
        return false;
    }
    bool ParseArray(Value& v) {
        v.type = Value::Array; ++i_; Ws();
        if (i_ < t_.size() && t_[i_] == L']') { ++i_; return true; }
        while (true) {
            Value item; Ws();
            if (!ParseValue(item)) return false;
            v.arr.push_back(std::move(item)); Ws();
            if (i_ >= t_.size()) return false;
            if (t_[i_] == L',') { ++i_; continue; }
            if (t_[i_] == L']') { ++i_; return true; }
            return false;
        }
    }
    bool ParseObject(Value& v) {
        v.type = Value::Object; ++i_; Ws();
        if (i_ < t_.size() && t_[i_] == L'}') { ++i_; return true; }
        while (true) {
            Ws();
            if (i_ >= t_.size() || t_[i_] != L'"') return false;
            std::wstring key;
            if (!ParseString(key)) return false;
            Ws();
            if (i_ >= t_.size() || t_[i_] != L':') return false;
            ++i_; Ws();
            Value item;
            if (!ParseValue(item)) return false;
            v.obj.emplace_back(std::move(key), std::move(item)); Ws();
            if (i_ >= t_.size()) return false;
            if (t_[i_] == L',') { ++i_; continue; }
            if (t_[i_] == L'}') { ++i_; return true; }
            return false;
        }
    }
};

inline void Escape(const std::wstring& s, std::wstring& out) {
    out += L'"';
    for (wchar_t c : s) {
        switch (c) {
            case L'"': out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            default:
                if (c < 0x20) { wchar_t b[8]; swprintf(b, 8, L"\\u%04x", (unsigned)c); out += b; }
                else out += c;
        }
    }
    out += L'"';
}

inline void Write(const Value& v, std::wstring& out, int indent = 0) {
    std::wstring pad(indent * 2, L' '), pad2((indent + 1) * 2, L' ');
    switch (v.type) {
        case Value::Null: out += L"null"; break;
        case Value::Bool: out += v.b ? L"true" : L"false"; break;
        case Value::Number: {
            wchar_t b[64];
            if (v.n == (long long)v.n) swprintf(b, 64, L"%lld", (long long)v.n); else swprintf(b, 64, L"%.6g", v.n);
            out += b; break;
        }
        case Value::String: Escape(v.s, out); break;
        case Value::Array:
            if (v.arr.empty()) { out += L"[]"; break; }
            out += L"[\n";
            for (size_t i = 0; i < v.arr.size(); ++i) {
                out += pad2; Write(v.arr[i], out, indent + 1);
                out += (i + 1 < v.arr.size()) ? L",\n" : L"\n";
            }
            out += pad + L"]"; break;
        case Value::Object:
            if (v.obj.empty()) { out += L"{}"; break; }
            out += L"{\n";
            for (size_t i = 0; i < v.obj.size(); ++i) {
                out += pad2; Escape(v.obj[i].first, out); out += L": ";
                Write(v.obj[i].second, out, indent + 1);
                out += (i + 1 < v.obj.size()) ? L",\n" : L"\n";
            }
            out += pad + L"}"; break;
    }
}

} // namespace json
