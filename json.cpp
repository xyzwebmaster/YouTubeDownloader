#include "json.h"

#include <cstdio>
#include <cstdlib>

const Json& Json::nullSentinel() {
    static const Json n;
    return n;
}

Json& Json::operator[](const std::string& key) {
    if (t_ == Null) t_ = Obj;
    return obj_[key];
}
const Json& Json::operator[](const std::string& key) const {
    if (t_ != Obj) return nullSentinel();
    auto it = obj_.find(key);
    return it == obj_.end() ? nullSentinel() : it->second;
}

Json& Json::operator[](size_t i) {
    if (t_ == Null) t_ = Arr;
    if (i >= arr_.size()) arr_.resize(i + 1);
    return arr_[i];
}
const Json& Json::operator[](size_t i) const {
    if (t_ != Arr || i >= arr_.size()) return nullSentinel();
    return arr_[i];
}

size_t Json::size() const {
    if (t_ == Arr) return arr_.size();
    if (t_ == Obj) return obj_.size();
    return 0;
}

bool Json::has(const std::string& key) const {
    return t_ == Obj && obj_.find(key) != obj_.end();
}

std::vector<std::string> Json::keys() const {
    std::vector<std::string> r;
    if (t_ == Obj) for (auto& kv : obj_) r.push_back(kv.first);
    return r;
}

Json& Json::push(Json v) {
    if (t_ == Null) t_ = Arr;
    arr_.push_back(std::move(v));
    return arr_.back();
}

Json& Json::set(const std::string& key, Json v) {
    if (t_ == Null) t_ = Obj;
    obj_[key] = std::move(v);
    return obj_[key];
}

const Json& Json::path(const std::string& dotted) const {
    const Json* cur = this;
    size_t i = 0;
    while (i <= dotted.size()) {
        size_t e = dotted.find('.', i);
        if (e == std::string::npos) e = dotted.size();
        std::string seg = dotted.substr(i, e - i);
        if (!seg.empty()) cur = &(*cur)[seg];
        if (cur->isNull() && e != dotted.size()) return nullSentinel();
        if (e == dotted.size()) break;
        i = e + 1;
    }
    return *cur;
}

// ---------- Parser ----------

void Json::skipWs(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) ++i;
}

bool Json::parseString(const std::string& s, size_t& i, std::string& out, std::string* err) {
    if (i >= s.size() || s[i] != '"') { if (err) *err = "expected string"; return false; }
    ++i;
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\') {
            ++i;
            if (i >= s.size()) { if (err) *err = "bad escape"; return false; }
            switch (s[i]) {
            case '"':  out += '"';  break;
            case '\\': out += '\\'; break;
            case '/':  out += '/';  break;
            case 'b':  out += '\b'; break;
            case 'f':  out += '\f'; break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case 'u': {
                if (i + 4 >= s.size()) { if (err) *err = "bad unicode"; return false; }
                unsigned u = 0;
                for (int k = 1; k <= 4; ++k) {
                    char c = s[i + k];
                    u <<= 4;
                    if      (c >= '0' && c <= '9') u |= (unsigned)(c - '0');
                    else if (c >= 'a' && c <= 'f') u |= (unsigned)(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') u |= (unsigned)(c - 'A' + 10);
                    else { if (err) *err = "bad unicode"; return false; }
                }
                i += 4;
                // Encode as UTF-8 (no surrogate-pair handling).
                if (u < 0x80) out += (char)u;
                else if (u < 0x800) {
                    out += (char)(0xC0 | (u >> 6));
                    out += (char)(0x80 | (u & 0x3F));
                } else {
                    out += (char)(0xE0 | (u >> 12));
                    out += (char)(0x80 | ((u >> 6) & 0x3F));
                    out += (char)(0x80 | (u & 0x3F));
                }
                break;
            }
            default: if (err) *err = "bad escape"; return false;
            }
            ++i;
        } else {
            out += s[i++];
        }
    }
    if (i >= s.size()) { if (err) *err = "unterminated string"; return false; }
    ++i;  // closing quote
    return true;
}

bool Json::parseNumber(const std::string& s, size_t& i, double& out, std::string* err) {
    size_t start = i;
    if (i < s.size() && s[i] == '-') ++i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    }
    if (start == i) { if (err) *err = "expected number"; return false; }
    char* end = nullptr;
    out = std::strtod(s.c_str() + start, &end);
    return true;
}

bool Json::parseValue(const std::string& s, size_t& i, Json& out, std::string* err) {
    skipWs(s, i);
    if (i >= s.size()) { if (err) *err = "unexpected end"; return false; }
    char c = s[i];
    if (c == '{') {
        ++i;
        out = Json::object();
        skipWs(s, i);
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        for (;;) {
            skipWs(s, i);
            std::string key;
            if (!parseString(s, i, key, err)) return false;
            skipWs(s, i);
            if (i >= s.size() || s[i] != ':') { if (err) *err = "expected ':'"; return false; }
            ++i;
            Json v;
            if (!parseValue(s, i, v, err)) return false;
            out.set(key, std::move(v));
            skipWs(s, i);
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            if (err) *err = "expected ',' or '}'";
            return false;
        }
    }
    if (c == '[') {
        ++i;
        out = Json::array();
        skipWs(s, i);
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        for (;;) {
            Json v;
            if (!parseValue(s, i, v, err)) return false;
            out.push(std::move(v));
            skipWs(s, i);
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            if (err) *err = "expected ',' or ']'";
            return false;
        }
    }
    if (c == '"') {
        std::string str;
        if (!parseString(s, i, str, err)) return false;
        out = Json(std::move(str));
        return true;
    }
    if (c == 't' || c == 'f') {
        if (c == 't' && s.compare(i, 4, "true") == 0)  { i += 4; out = Json(true);  return true; }
        if (c == 'f' && s.compare(i, 5, "false") == 0) { i += 5; out = Json(false); return true; }
        if (err) *err = "bad bool literal";
        return false;
    }
    if (c == 'n') {
        if (s.compare(i, 4, "null") == 0) { i += 4; out = Json(); return true; }
        if (err) *err = "bad null literal";
        return false;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        double d = 0;
        if (!parseNumber(s, i, d, err)) return false;
        out = Json(d);
        return true;
    }
    if (err) *err = "unexpected char";
    return false;
}

Json Json::parse(const std::string& src, std::string* err) {
    Json out;
    size_t i = 0;
    if (!parseValue(src, i, out, err)) return Json();
    skipWs(src, i);
    // trailing garbage is ignored — TikTok responses sometimes have it
    return out;
}

// ---------- Serializer ----------

static void dumpString(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                out += buf;
            } else {
                out += (char)c;
            }
        }
    }
    out += '"';
}

static void newline(std::string& out, int indent, int depth) {
    if (indent <= 0) return;
    out += '\n';
    out.append((size_t)(indent * depth), ' ');
}

void Json::dumpInto(std::string& out, int indent, int depth) const {
    switch (t_) {
    case Null: out += "null"; break;
    case Bool: out += b_ ? "true" : "false"; break;
    case Num: {
        // Emit an int form if the value is integral and fits in long long.
        long long iv = (long long)n_;
        if ((double)iv == n_) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lld", iv);
            out += buf;
        } else {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.17g", n_);
            out += buf;
        }
        break;
    }
    case Str: dumpString(s_, out); break;
    case Arr: {
        out += '[';
        bool first = true;
        for (const auto& v : arr_) {
            if (!first) out += ',';
            newline(out, indent, depth + 1);
            v.dumpInto(out, indent, depth + 1);
            first = false;
        }
        if (!first) newline(out, indent, depth);
        out += ']';
        break;
    }
    case Obj: {
        out += '{';
        bool first = true;
        for (const auto& kv : obj_) {
            if (!first) out += ',';
            newline(out, indent, depth + 1);
            dumpString(kv.first, out);
            out += indent > 0 ? ": " : ":";
            kv.second.dumpInto(out, indent, depth + 1);
            first = false;
        }
        if (!first) newline(out, indent, depth);
        out += '}';
        break;
    }
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpInto(out, indent, 0);
    return out;
}
