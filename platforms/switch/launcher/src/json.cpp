#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace partyboard::launcher::json {

namespace {

const Value kNull;
const std::string kEmpty;

class Parser {
public:
    explicit Parser(std::string_view text) : m_text(text) {}

    bool document(Value& out, std::string* error) {
        if (m_text.substr(0, 3) == "\xEF\xBB\xBF")
            m_pos = 3;
        skipSpace();
        const bool ok = value(out, 0) && (skipSpace(), m_pos == m_text.size());
        if (!ok && error) {
            char buffer[96];
            std::snprintf(buffer, sizeof(buffer), "invalid JSON near byte %zu", m_pos);
            *error = buffer;
        }
        return ok;
    }

private:
    static constexpr int kMaxDepth = 64;

    void skipSpace() {
        while (m_pos < m_text.size() &&
               (m_text[m_pos] == ' ' || m_text[m_pos] == '\t' || m_text[m_pos] == '\n' || m_text[m_pos] == '\r'))
            ++m_pos;
    }

    bool literal(std::string_view word) {
        if (m_text.substr(m_pos, word.size()) != word)
            return false;
        m_pos += word.size();
        return true;
    }

    bool value(Value& out, int depth) {
        if (depth > kMaxDepth || m_pos >= m_text.size())
            return false;
        switch (m_text[m_pos]) {
        case '{': return object(out, depth);
        case '[': return array(out, depth);
        case '"': {
            std::string s;
            if (!string(s))
                return false;
            out = Value::string(std::move(s));
            return true;
        }
        case 't':
            out = Value::boolean(true);
            return literal("true");
        case 'f':
            out = Value::boolean(false);
            return literal("false");
        case 'n':
            out = Value();
            return literal("null");
        default: return number(out);
        }
    }

    bool number(Value& out) {
        const size_t start = m_pos;
        if (m_pos < m_text.size() && m_text[m_pos] == '-')
            ++m_pos;
        bool digits = false;
        while (m_pos < m_text.size() &&
               ((m_text[m_pos] >= '0' && m_text[m_pos] <= '9') || m_text[m_pos] == '.' || m_text[m_pos] == 'e' ||
                m_text[m_pos] == 'E' || m_text[m_pos] == '+' || m_text[m_pos] == '-')) {
            digits |= m_text[m_pos] >= '0' && m_text[m_pos] <= '9';
            ++m_pos;
        }
        if (!digits)
            return false;
        const std::string token(m_text.substr(start, m_pos - start));
        char* end = nullptr;
        const double n = std::strtod(token.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(n))
            return false;
        out = Value::number(n);
        return true;
    }

    static void appendUtf8(std::string& s, uint32_t cp) {
        if (cp < 0x80) {
            s.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool hex4(uint32_t& out) {
        if (m_pos + 4 > m_text.size())
            return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_text[m_pos++];
            out <<= 4;
            if (c >= '0' && c <= '9')
                out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                out |= static_cast<uint32_t>(c - 'A' + 10);
            else
                return false;
        }
        return true;
    }

    bool string(std::string& out) {
        ++m_pos; // opening quote
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos++];
            if (c == '"')
                return true;
            if (static_cast<unsigned char>(c) < 0x20)
                return false;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (m_pos >= m_text.size())
                return false;
            const char e = m_text[m_pos++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint32_t cp = 0;
                if (!hex4(cp))
                    return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    uint32_t low = 0;
                    if (!literal("\\u") || !hex4(low) || low < 0xDC00 || low > 0xDFFF)
                        return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                appendUtf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool array(Value& out, int depth) {
        ++m_pos;
        out = Value::array();
        skipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == ']') {
            ++m_pos;
            return true;
        }
        while (true) {
            Value item;
            skipSpace();
            if (!value(item, depth + 1))
                return false;
            out.push(std::move(item));
            skipSpace();
            if (m_pos >= m_text.size())
                return false;
            if (m_text[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_text[m_pos] == ']') {
                ++m_pos;
                return true;
            }
            return false;
        }
    }

    bool object(Value& out, int depth) {
        ++m_pos;
        out = Value::object();
        skipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == '}') {
            ++m_pos;
            return true;
        }
        while (true) {
            skipSpace();
            std::string key;
            if (m_pos >= m_text.size() || m_text[m_pos] != '"' || !string(key))
                return false;
            skipSpace();
            if (m_pos >= m_text.size() || m_text[m_pos] != ':')
                return false;
            ++m_pos;
            skipSpace();
            Value item;
            if (!value(item, depth + 1))
                return false;
            out.set(std::move(key), std::move(item));
            skipSpace();
            if (m_pos >= m_text.size())
                return false;
            if (m_text[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_text[m_pos] == '}') {
                ++m_pos;
                return true;
            }
            return false;
        }
    }

    std::string_view m_text;
    size_t m_pos = 0;
};

void escape(std::string& out, const std::string& s) {
    out.push_back('"');
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buffer;
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
}

void write(std::string& out, const Value& v, int indent) {
    const std::string pad(static_cast<size_t>(indent) * 2, ' ');
    const std::string inner(static_cast<size_t>(indent + 1) * 2, ' ');
    switch (v.type()) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.asBool() ? "true" : "false"; break;
    case Value::Type::Number: {
        const double n = v.asNumber();
        char buffer[40];
        if (std::fabs(n) < 9.0e15 && n == std::floor(n))
            std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(n));
        else
            std::snprintf(buffer, sizeof(buffer), "%.17g", n);
        out += buffer;
        break;
    }
    case Value::Type::String: escape(out, v.asString()); break;
    case Value::Type::Array:
        if (v.items().empty()) {
            out += "[]";
            break;
        }
        out += "[\n";
        for (size_t i = 0; i < v.items().size(); ++i) {
            out += inner;
            write(out, v.items()[i], indent + 1);
            out += i + 1 < v.items().size() ? ",\n" : "\n";
        }
        out += pad + "]";
        break;
    case Value::Type::Object:
        if (v.members().empty()) {
            out += "{}";
            break;
        }
        out += "{\n";
        for (size_t i = 0; i < v.members().size(); ++i) {
            out += inner;
            escape(out, v.members()[i].first);
            out += ": ";
            write(out, v.members()[i].second, indent + 1);
            out += i + 1 < v.members().size() ? ",\n" : "\n";
        }
        out += pad + "}";
        break;
    }
}

} // namespace

Value Value::boolean(bool b) {
    Value v;
    v.m_type = Type::Bool;
    v.m_bool = b;
    return v;
}

Value Value::number(double n) {
    Value v;
    v.m_type = Type::Number;
    v.m_number = n;
    return v;
}

Value Value::string(std::string s) {
    Value v;
    v.m_type = Type::String;
    v.m_string = std::move(s);
    return v;
}

Value Value::array() {
    Value v;
    v.m_type = Type::Array;
    return v;
}

Value Value::object() {
    Value v;
    v.m_type = Type::Object;
    return v;
}

bool Value::asBool(bool fallback) const { return m_type == Type::Bool ? m_bool : fallback; }
double Value::asNumber(double fallback) const { return m_type == Type::Number ? m_number : fallback; }

long long Value::asInteger(long long fallback) const {
    return m_type == Type::Number ? static_cast<long long>(std::llround(m_number)) : fallback;
}

const std::string& Value::asString() const { return m_type == Type::String ? m_string : kEmpty; }

const Value& Value::operator[](std::string_view key) const {
    for (const auto& member : m_members) {
        if (member.first == key)
            return member.second;
    }
    return kNull;
}

bool Value::has(std::string_view key) const {
    for (const auto& member : m_members) {
        if (member.first == key)
            return true;
    }
    return false;
}

void Value::set(std::string key, Value value) {
    for (auto& member : m_members) {
        if (member.first == key) {
            member.second = std::move(value);
            return;
        }
    }
    m_members.emplace_back(std::move(key), std::move(value));
}

bool parse(std::string_view text, Value& out, std::string* error) {
    Parser parser(text);
    return parser.document(out, error);
}

bool parseFile(const std::string& path, Value& out, std::string* error) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        if (error)
            *error = "cannot open " + path;
        return false;
    }
    std::string text;
    char buffer[4096];
    size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        text.append(buffer, got);
        if (text.size() > (8u << 20)) {
            std::fclose(file);
            if (error)
                *error = path + " is too large";
            return false;
        }
    }
    std::fclose(file);
    return parse(text, out, error);
}

std::string serialize(const Value& value) {
    std::string out;
    write(out, value, 0);
    out.push_back('\n');
    return out;
}

} // namespace partyboard::launcher::json
