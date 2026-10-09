#pragma once

// Minimal JSON for the two documents the launcher shares with CubeShelf: the
// game catalogue and a game's installed.json mod list. Strict enough to
// reject garbage, small enough to need no third-party dependency on libnx.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace partyboard::launcher::json {

class Value {
public:
    enum class Type : unsigned char { Null, Bool, Number, String, Array, Object };

    Value() = default;
    static Value boolean(bool b);
    static Value number(double n);
    static Value string(std::string s);
    static Value array();
    static Value object();

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isObject() const { return m_type == Type::Object; }
    bool isArray() const { return m_type == Type::Array; }
    bool isString() const { return m_type == Type::String; }

    // Typed reads with a fallback for a missing or mistyped value.
    bool asBool(bool fallback = false) const;
    double asNumber(double fallback = 0.0) const;
    long long asInteger(long long fallback = 0) const;
    const std::string& asString() const; // empty unless a string

    // Object access. A missing key yields a shared null value.
    const Value& operator[](std::string_view key) const;
    bool has(std::string_view key) const;
    void set(std::string key, Value value);
    const std::vector<std::pair<std::string, Value>>& members() const { return m_members; }

    // Array access.
    const std::vector<Value>& items() const { return m_items; }
    void push(Value value) { m_items.push_back(std::move(value)); }

private:
    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0.0;
    std::string m_string;
    std::vector<Value> m_items;
    std::vector<std::pair<std::string, Value>> m_members; // insertion order kept
};

// Parses a complete document; a UTF-8 BOM is tolerated. On failure returns
// false and leaves `error` describing the first problem.
bool parse(std::string_view text, Value& out, std::string* error = nullptr);
bool parseFile(const std::string& path, Value& out, std::string* error = nullptr);

// Pretty-printed with two-space indentation, keys in insertion order.
std::string serialize(const Value& value);

} // namespace partyboard::launcher::json
