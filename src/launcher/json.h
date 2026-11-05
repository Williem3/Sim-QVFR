// Minimal JSON for OpenXR layer manifests: parse (UTF-8), edit, write back. Objects keep their key
// order so a rewritten manifest reads like the original. Numbers are kept as their source text.
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace simqvfr::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    std::string text;  // String: the decoded string; Number: the literal
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> members;

    const Value* find(const std::string& key) const {
        for (const auto& [k, v] : members)
            if (k == key)
                return &v;
        return nullptr;
    }
    Value* find(const std::string& key) {
        for (auto& [k, v] : members)
            if (k == key)
                return &v;
        return nullptr;
    }
    // The string member `key`, or "" when absent or not a string.
    std::string string_at(const std::string& key) const {
        const Value* v = find(key);
        return v && v->type == Type::String ? v->text : std::string();
    }
    void set_string(const std::string& key, const std::string& value) {
        Value s;
        s.type = Type::String;
        s.text = value;
        if (Value* v = find(key))
            *v = std::move(s);
        else
            members.emplace_back(key, std::move(s));
    }
    void erase(const std::string& key) {
        for (auto it = members.begin(); it != members.end(); ++it)
            if (it->first == key) {
                members.erase(it);
                return;
            }
    }
};

namespace detail {

struct Parser {
    const std::string& s;
    size_t i = 0;
    bool ok = true;

    void space() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            ++i;
    }
    bool eat(char c) {
        space();
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xc0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xe0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
    }
    bool hex4(unsigned& v) {
        if (i + 4 > s.size())
            return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= static_cast<unsigned>(c - 'A' + 10);
            else
                return false;
        }
        return true;
    }
    bool string(std::string& out) {
        if (!eat('"'))
            return false;
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"')
                return true;
            if (static_cast<unsigned char>(c) < 0x20)
                return false;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size())
                return false;
            const char e = s[i++];
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
                unsigned cp;
                if (!hex4(cp))
                    return false;
                if (cp >= 0xd800 && cp < 0xdc00) {  // surrogate pair
                    unsigned low;
                    if (i + 2 > s.size() || s[i] != '\\' || s[i + 1] != 'u')
                        return false;
                    i += 2;
                    if (!hex4(low) || low < 0xdc00 || low >= 0xe000)
                        return false;
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                }
                utf8(out, cp);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }
    bool literal(const char* word) {
        const std::string w(word);
        if (s.compare(i, w.size(), w) != 0)
            return false;
        i += w.size();
        return true;
    }
    bool value(Value& v, int depth) {
        if (depth > 64)
            return false;
        space();
        if (i >= s.size())
            return false;
        const char c = s[i];
        if (c == '{') {
            ++i;
            v.type = Value::Type::Object;
            if (eat('}'))
                return true;
            do {
                std::string key;
                Value member;
                if (!string(key) || !eat(':') || !value(member, depth + 1))
                    return false;
                v.members.emplace_back(std::move(key), std::move(member));
            } while (eat(','));
            return eat('}');
        }
        if (c == '[') {
            ++i;
            v.type = Value::Type::Array;
            if (eat(']'))
                return true;
            do {
                Value item;
                if (!value(item, depth + 1))
                    return false;
                v.items.push_back(std::move(item));
            } while (eat(','));
            return eat(']');
        }
        if (c == '"') {
            v.type = Value::Type::String;
            return string(v.text);
        }
        if (literal("true")) {
            v.type = Value::Type::Bool;
            v.boolean = true;
            return true;
        }
        if (literal("false")) {
            v.type = Value::Type::Bool;
            return true;
        }
        if (literal("null"))
            return true;
        const size_t start = i;
        while (i < s.size() && (std::string("+-.eE0123456789").find(s[i]) != std::string::npos))
            ++i;
        if (i == start)
            return false;
        v.type = Value::Type::Number;
        v.text = s.substr(start, i - start);
        return true;
    }
};

inline void quote(std::string& out, const std::string& text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                static const char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[(c >> 4) & 0xf];
                out += hex[c & 0xf];
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

inline void dump(std::string& out, const Value& v, int indent) {
    const std::string pad(static_cast<size_t>(indent) * 2, ' '), inner(static_cast<size_t>(indent + 1) * 2, ' ');
    switch (v.type) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.boolean ? "true" : "false"; break;
    case Value::Type::Number: out += v.text; break;
    case Value::Type::String: quote(out, v.text); break;
    case Value::Type::Array:
        out += "[";
        for (size_t k = 0; k < v.items.size(); ++k) {
            out += k ? ",\n" : "\n";
            out += inner;
            dump(out, v.items[k], indent + 1);
        }
        out += v.items.empty() ? "]" : "\n" + pad + "]";
        break;
    case Value::Type::Object:
        out += "{";
        for (size_t k = 0; k < v.members.size(); ++k) {
            out += k ? ",\n" : "\n";
            out += inner;
            quote(out, v.members[k].first);
            out += ": ";
            dump(out, v.members[k].second, indent + 1);
        }
        out += v.members.empty() ? "}" : "\n" + pad + "}";
        break;
    }
}

}  // namespace detail

// Parses a whole document (a leading UTF-8 BOM is skipped). False on any syntax error.
inline bool parse(const std::string& text, Value& out) {
    detail::Parser p{text};
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0)
        p.i = 3;
    out = Value{};
    if (!p.value(out, 0))
        return false;
    p.space();
    return p.i == text.size();
}

inline std::string dump(const Value& v) {
    std::string out;
    detail::dump(out, v, 0);
    out += "\n";
    return out;
}

}  // namespace simqvfr::json
