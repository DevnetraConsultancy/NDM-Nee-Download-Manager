#pragma once
// ---------------------------------------------------------------------------
// Minimal JSON DOM (parse / serialize) used to merge Firefox policies.json
// without clobbering other entries. UTF-8 in, UTF-8 out. Object key order is
// preserved.
// ---------------------------------------------------------------------------
#include <string>
#include <vector>
#include <memory>
#include <utility>
#include <cstdio>
#include <cstring>

struct NdmJson
{
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool        b = false;
    std::string s;            // String value, or raw Number text
    std::vector<NdmJson> arr;
    std::vector<std::pair<std::string, NdmJson>> obj;

    static NdmJson MakeObject() { NdmJson j; j.type = Object; return j; }
    static NdmJson MakeString(const std::string& v) { NdmJson j; j.type = String; j.s = v; return j; }

    NdmJson* Find(const std::string& k)
    {
        if (type != Object) return nullptr;
        for (auto& kv : obj) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    NdmJson& Set(const std::string& k, NdmJson v)
    {
        if (type != Object) { *this = MakeObject(); }
        if (NdmJson* e = Find(k)) { *e = std::move(v); return *e; }
        obj.emplace_back(k, std::move(v));
        return obj.back().second;
    }
    bool Erase(const std::string& k)
    {
        if (type != Object) return false;
        for (size_t i = 0; i < obj.size(); i++)
            if (obj[i].first == k) { obj.erase(obj.begin() + i); return true; }
        return false;
    }

    // ---- parser ------------------------------------------------------------
    static bool Parse(const std::string& text, NdmJson& out)
    {
        size_t p = 0;
        if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
            (unsigned char)text[2] == 0xBF) p = 3;
        if (!ParseValue(text, p, out, 0)) return false;
        Ws(text, p);
        return p == text.size();
    }

    // ---- serializer --------------------------------------------------------
    std::string Dump(int indent = 0) const
    {
        std::string o; DumpTo(o, indent); o += "\n"; return o;
    }

private:
    static void Ws(const std::string& t, size_t& p)
    {
        while (p < t.size() && (t[p] == ' ' || t[p] == '\t' || t[p] == '\r' || t[p] == '\n')) p++;
    }
    static bool ParseString(const std::string& t, size_t& p, std::string& out)
    {
        if (p >= t.size() || t[p] != '"') return false;
        p++;
        while (p < t.size()) {
            char c = t[p++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (p >= t.size()) return false;
            char e = t[p++];
            switch (e) {
            case '"': out += '"'; break;  case '\\': out += '\\'; break; case '/': out += '/'; break;
            case 'b': out += '\b'; break; case 'f': out += '\f'; break;  case 'n': out += '\n'; break;
            case 'r': out += '\r'; break; case 't': out += '\t'; break;
            case 'u': {
                auto hex4 = [&](unsigned& v) {
                    if (p + 4 > t.size()) return false;
                    v = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = t[p++]; v <<= 4;
                        if (h >= '0' && h <= '9') v |= h - '0';
                        else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
                        else return false;
                    }
                    return true;
                };
                unsigned cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= t.size() && t[p] == '\\' && t[p + 1] == 'u') {
                    p += 2; unsigned lo;
                    if (!hex4(lo)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                if (cp < 0x80) out += (char)cp;
                else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
                else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    static bool ParseValue(const std::string& t, size_t& p, NdmJson& v, int depth)
    {
        if (depth > 64) return false;
        Ws(t, p);
        if (p >= t.size()) return false;
        char c = t[p];
        if (c == '{') {
            v = MakeObject(); p++; Ws(t, p);
            if (p < t.size() && t[p] == '}') { p++; return true; }
            for (;;) {
                Ws(t, p);
                std::string k;
                if (!ParseString(t, p, k)) return false;
                Ws(t, p);
                if (p >= t.size() || t[p] != ':') return false;
                p++;
                NdmJson child;
                if (!ParseValue(t, p, child, depth + 1)) return false;
                v.obj.emplace_back(std::move(k), std::move(child));
                Ws(t, p);
                if (p < t.size() && t[p] == ',') { p++; continue; }
                if (p < t.size() && t[p] == '}') { p++; return true; }
                return false;
            }
        }
        if (c == '[') {
            v = NdmJson(); v.type = Array; p++; Ws(t, p);
            if (p < t.size() && t[p] == ']') { p++; return true; }
            for (;;) {
                NdmJson child;
                if (!ParseValue(t, p, child, depth + 1)) return false;
                v.arr.push_back(std::move(child));
                Ws(t, p);
                if (p < t.size() && t[p] == ',') { p++; continue; }
                if (p < t.size() && t[p] == ']') { p++; return true; }
                return false;
            }
        }
        if (c == '"') { v = NdmJson(); v.type = String; return ParseString(t, p, v.s); }
        if (t.compare(p, 4, "true") == 0)  { v = NdmJson(); v.type = Bool; v.b = true;  p += 4; return true; }
        if (t.compare(p, 5, "false") == 0) { v = NdmJson(); v.type = Bool; v.b = false; p += 5; return true; }
        if (t.compare(p, 4, "null") == 0)  { v = NdmJson(); p += 4; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            size_t s0 = p;
            while (p < t.size() && (strchr("+-.eE", t[p]) || (t[p] >= '0' && t[p] <= '9'))) p++;
            v = NdmJson(); v.type = Number; v.s = t.substr(s0, p - s0);
            return true;
        }
        return false;
    }
    static void Esc(std::string& o, const std::string& s)
    {
        o += '"';
        for (unsigned char c : s) {
            switch (c) {
            case '"': o += "\\\""; break; case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break; case '\r': o += "\\r"; break; case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                else o += (char)c;
            }
        }
        o += '"';
    }
    void DumpTo(std::string& o, int ind) const
    {
        auto nl = [&](int n) { o += '\n'; o.append((size_t)n * 2, ' '); };
        switch (type) {
        case Null:   o += "null"; break;
        case Bool:   o += b ? "true" : "false"; break;
        case Number: o += s; break;
        case String: Esc(o, s); break;
        case Array:
            if (arr.empty()) { o += "[]"; break; }
            o += '[';
            for (size_t i = 0; i < arr.size(); i++) { if (i) o += ','; nl(ind + 1); arr[i].DumpTo(o, ind + 1); }
            nl(ind); o += ']';
            break;
        case Object:
            if (obj.empty()) { o += "{}"; break; }
            o += '{';
            for (size_t i = 0; i < obj.size(); i++) {
                if (i) o += ',';
                nl(ind + 1); Esc(o, obj[i].first); o += ": "; obj[i].second.DumpTo(o, ind + 1);
            }
            nl(ind); o += '}';
            break;
        }
    }
};
