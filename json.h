#pragma once
#include <map>
#include <string>
#include <vector>

// Minimal JSON value type. Built for the subset of JSON we actually
// exchange with TikTok and persist in settings.dat — it does NOT
// implement Unicode surrogate pairs in \uXXXX escapes, but it round-trips
// UTF-8 strings correctly, handles nested objects/arrays, and is small
// enough to read end-to-end.
class Json {
public:
    enum Type { Null, Bool, Num, Str, Arr, Obj };

    Json() = default;
    Json(std::nullptr_t)         {}
    Json(bool b)         : t_(Bool), b_(b) {}
    Json(int n)          : t_(Num),  n_((double)n) {}
    Json(long long n)    : t_(Num),  n_((double)n) {}
    Json(double n)       : t_(Num),  n_(n) {}
    Json(const char* s)  : t_(Str),  s_(s) {}
    Json(std::string s)  : t_(Str),  s_(std::move(s)) {}

    static Json object() { Json j; j.t_ = Obj; return j; }
    static Json array()  { Json j; j.t_ = Arr; return j; }

    Type type() const { return t_; }
    bool isNull()   const { return t_ == Null; }
    bool isBool()   const { return t_ == Bool; }
    bool isNumber() const { return t_ == Num; }
    bool isString() const { return t_ == Str; }
    bool isArray()  const { return t_ == Arr; }
    bool isObject() const { return t_ == Obj; }

    bool        asBool() const { return t_ == Bool ? b_ : false; }
    double      asNum()  const { return t_ == Num  ? n_ : 0.0; }
    int         asInt()  const { return (int)asNum(); }
    long long   asI64()  const { return (long long)asNum(); }
    const std::string& asStr() const {
        static const std::string empty;
        return t_ == Str ? s_ : empty;
    }

    // Mutating object access — converts to Obj if currently Null.
    Json& operator[](const std::string& key);
    // Read-only access — returns a static null sentinel for missing keys
    // or non-objects, so callers can chain `.path("a")["b"].asStr()`
    // safely without checking each step.
    const Json& operator[](const std::string& key) const;

    Json& operator[](size_t i);
    const Json& operator[](size_t i) const;

    size_t size() const;
    bool   has(const std::string& key) const;
    std::vector<std::string> keys() const;

    Json& push(Json v);
    Json& set(const std::string& key, Json v);

    // Dotted path lookup: `path("data.publish_id")`. Each segment is an
    // object key (no array indices). Returns a null sentinel on miss.
    const Json& path(const std::string& dotted) const;

    static Json parse(const std::string& src, std::string* err = nullptr);
    std::string dump(int indent = 0) const;

private:
    Type        t_ = Null;
    bool        b_ = false;
    double      n_ = 0;
    std::string s_;
    std::vector<Json>           arr_;
    std::map<std::string, Json> obj_;

    static const Json& nullSentinel();

    void dumpInto(std::string& out, int indent, int depth) const;
    static bool parseValue (const std::string& s, size_t& i, Json& out, std::string* err);
    static bool parseString(const std::string& s, size_t& i, std::string& out, std::string* err);
    static bool parseNumber(const std::string& s, size_t& i, double& out, std::string* err);
    static void skipWs(const std::string& s, size_t& i);
};
