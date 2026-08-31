#pragma once
//
// Minimal JSON, deliberately dependency-free.
//
// The config file is a handful of scalars per channel; pulling in a JSON
// library for that would be more weight than the whole feature. Objects keep
// insertion order so a hand-edited config round-trips without being reshuffled.
//
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace audiomon {

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;
    explicit JsonValue(bool b)               : type_(Type::Bool),   bool_(b) {}
    explicit JsonValue(double n)             : type_(Type::Number), num_(n) {}
    explicit JsonValue(int n)                : type_(Type::Number), num_(n) {}
    explicit JsonValue(const std::string& s) : type_(Type::String), str_(s) {}
    explicit JsonValue(const char* s)        : type_(Type::String), str_(s) {}

    static JsonValue object() { JsonValue v; v.type_ = Type::Object; return v; }
    static JsonValue array()  { JsonValue v; v.type_ = Type::Array;  return v; }

    Type type() const { return type_; }
    bool isObject() const { return type_ == Type::Object; }
    bool isArray()  const { return type_ == Type::Array; }
    bool isNull()   const { return type_ == Type::Null; }

    // Tolerant readers: a missing key, a wrong type or a corrupt file all fall
    // back to the supplied default rather than throwing. A config file is not
    // worth crashing over.
    bool        asBool(bool def) const;
    double      asNumber(double def) const;
    std::string asString(const std::string& def) const;

    const JsonValue* find(const std::string& key) const;
    void             set(const std::string& key, JsonValue v);

    const std::vector<JsonValue>& items() const { return arr_; }
    void push(JsonValue v);

    std::string dump(int indent = 2) const;
    static JsonValue parse(const std::string& text, std::string* error = nullptr);

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    Type                                        type_ = Type::Null;
    bool                                        bool_ = false;
    double                                      num_  = 0.0;
    std::string                                 str_;
    std::vector<JsonValue>                      arr_;
    std::vector<std::pair<std::string, JsonValue>> obj_;   // insertion-ordered
};

} // namespace audiomon
