// Copyright (c) 2026 ProtocolTeam (Zhandos). All rights reserved.
// Viewing allowed. No copying, modification, commercial use, or redistribution
// without written permission from ProtocolTeam (Zhandos).

#pragma once

#include <cctype>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    const Json* get(const std::string& key) const {
        for (const auto& item : obj) {
            if (item.first == key) return &item.second;
        }
        return nullptr;
    }

    const Json& at(const std::string& key) const {
        const Json* found = get(key);
        if (!found) throw std::runtime_error("JSON has no key " + key);
        return *found;
    }
};

inline Json parse_json(const std::string& text) {
    struct Parser {
        const std::string& s;
        size_t i = 0;

        explicit Parser(const std::string& in) : s(in) {}

        [[noreturn]] void fail(const char* what) const { throw std::runtime_error(what); }

        void skip() {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        }

        char peek() {
            skip();
            if (i >= s.size()) fail("unexpected end of JSON");
            return s[i];
        }

        char getc() {
            char c = peek();
            ++i;
            return c;
        }

        Json parse() {
            Json value = parse_value();
            skip();
            return value;
        }

        Json parse_value() {
            char c = peek();
            if (c == '{') return parse_object();
            if (c == '[') return parse_array();
            if (c == '"') {
                Json v;
                v.type = Json::Type::String;
                v.str = parse_string();
                return v;
            }
            if (c == 't' || c == 'f') return parse_bool();
            if (c == 'n') return parse_null();
            if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parse_number();
            fail("invalid JSON value");
        }

        Json parse_object() {
            if (getc() != '{') fail("expected object");
            Json v;
            v.type = Json::Type::Object;
            skip();
            if (peek() == '}') {
                ++i;
                return v;
            }
            while (true) {
                if (peek() != '"') fail("expected object key");
                std::string key = parse_string();
                if (getc() != ':') fail("expected colon");
                v.obj.emplace_back(std::move(key), parse_value());
                char sep = getc();
                if (sep == '}') break;
                if (sep != ',') fail("expected comma in object");
            }
            return v;
        }

        Json parse_array() {
            if (getc() != '[') fail("expected array");
            Json v;
            v.type = Json::Type::Array;
            skip();
            if (peek() == ']') {
                ++i;
                return v;
            }
            while (true) {
                v.arr.push_back(parse_value());
                char sep = getc();
                if (sep == ']') break;
                if (sep != ',') fail("expected comma in array");
            }
            return v;
        }

        Json parse_bool() {
            Json v;
            v.type = Json::Type::Bool;
            if (s.compare(i, 4, "true") == 0) {
                v.b = true;
                i += 4;
                return v;
            }
            if (s.compare(i, 5, "false") == 0) {
                v.b = false;
                i += 5;
                return v;
            }
            fail("invalid boolean");
        }

        Json parse_null() {
            if (s.compare(i, 4, "null") != 0) fail("expected null");
            i += 4;
            Json v;
            v.type = Json::Type::Null;
            return v;
        }

        Json parse_number() {
            size_t start = i;
            if (s[i] == '-') ++i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
            if (i < s.size() && s[i] == '.') {
                ++i;
                while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
            }
            if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
                ++i;
                if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
                while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
            }
            Json v;
            v.type = Json::Type::Number;
            try {
                v.num = std::stod(s.substr(start, i - start));
            } catch (...) {
                fail("invalid number");
            }
            return v;
        }

        std::string parse_string() {
            if (getc() != '"') fail("expected string");
            std::string out;
            while (i < s.size()) {
                unsigned char c = static_cast<unsigned char>(s[i++]);
                if (c == '"') return out;
                if (c != '\\') {
                    out.push_back(static_cast<char>(c));
                    continue;
                }
                if (i >= s.size()) fail("bad escape");
                char e = s[i++];
                switch (e) {
                    case '"':
                    case '\\':
                    case '/': out.push_back(e); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (i + 4 > s.size()) fail("bad unicode escape");
                        int cp = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = s[i++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp += h - '0';
                            else if (h >= 'a' && h <= 'f') cp += h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') cp += h - 'A' + 10;
                            else fail("bad unicode escape");
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default: fail("bad escape");
                }
            }
            fail("unterminated string");
        }

        static void append_utf8(std::string& out, int cp) {
            if (cp < 0x80) {
                out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }
    };
    return Parser(text).parse();
}

inline std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 0xF]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}
