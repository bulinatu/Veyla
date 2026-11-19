#include "config/flat_json.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <locale>
#include <sstream>

namespace brp::next {
namespace {

void SkipSpace(std::string_view input, std::size_t& i) {
    while (i < input.size()) {
        const unsigned char c = static_cast<unsigned char>(input[i]);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < input.size() && input[i + 1] == '/') {
            while (i < input.size() && input[i] != '\n') {
                ++i;
            }
            continue;
        }
        break;
    }
}

bool ParseString(std::string_view input, std::size_t& i, std::string& out,
                 std::string& error) {
    if (i >= input.size() || input[i] != '"') {
        error = "expected a string";
        return false;
    }
    ++i;
    out.clear();
    while (i < input.size()) {
        const char c = input[i++];
        if (c == '"') {
            return true;
        }
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (i >= input.size()) {
            error = "unterminated escape";
            return false;
        }
        const char esc = input[i++];
        switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (i + 4 > input.size()) {
                    error = "truncated unicode escape";
                    return false;
                }
                unsigned value = 0;
                for (int k = 0; k < 4; ++k) {
                    const char h = input[i++];
                    value <<= 4;
                    if (h >= '0' && h <= '9') {
                        value |= static_cast<unsigned>(h - '0');
                    } else if (h >= 'a' && h <= 'f') {
                        value |= static_cast<unsigned>(h - 'a' + 10);
                    } else if (h >= 'A' && h <= 'F') {
                        value |= static_cast<unsigned>(h - 'A' + 10);
                    } else {
                        error = "bad unicode escape";
                        return false;
                    }
                }
                if (value <= 0x7F) {
                    out.push_back(static_cast<char>(value));
                } else if (value <= 0x7FF) {
                    out.push_back(static_cast<char>(0xC0 | (value >> 6)));
                    out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xE0 | (value >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
                }
                break;
            }
            default:
                error = "unknown escape";
                return false;
        }
    }
    error = "unterminated string";
    return false;
}

bool SkipNested(std::string_view input, std::size_t& i, char open, char close,
                std::string& error) {
    int depth = 0;
    bool in_string = false;
    while (i < input.size()) {
        const char c = input[i++];
        if (in_string) {
            if (c == '\\' && i < input.size()) {
                ++i;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == open) {
            ++depth;
        } else if (c == close) {
            --depth;
            if (depth == 0) {
                return true;
            }
        }
    }
    error = "unterminated nested value";
    return false;
}

std::string Escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}

std::string FormatDouble(double value) {
    if (!std::isfinite(value)) {
        return "0";
    }
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(15) << value;
    return stream.str();
}

}  // namespace

bool FlatJson::Parse(std::string_view input, FlatMap& out, std::string& error) {
    out.clear();
    std::size_t i = 0;
    SkipSpace(input, i);
    if (i >= input.size() || input[i] != '{') {
        error = "root is not an object";
        return false;
    }
    ++i;
    for (;;) {
        SkipSpace(input, i);
        if (i >= input.size()) {
            error = "unterminated object";
            return false;
        }
        if (input[i] == '}') {
            ++i;
            return true;
        }
        std::string key;
        if (!ParseString(input, i, key, error)) {
            return false;
        }
        SkipSpace(input, i);
        if (i >= input.size() || input[i] != ':') {
            error = "expected ':'";
            return false;
        }
        ++i;
        SkipSpace(input, i);
        if (i >= input.size()) {
            error = "missing value";
            return false;
        }
        if (input[i] == '"') {
            FlatValue value;
            value.quoted = true;
            if (!ParseString(input, i, value.text, error)) {
                return false;
            }
            out[key] = std::move(value);
        } else if (input[i] == '{' || input[i] == '[') {
            const char open = input[i];
            const char close = open == '{' ? '}' : ']';
            if (!SkipNested(input, i, open, close, error)) {
                return false;
            }
        } else {
            const std::size_t start = i;
            while (i < input.size() && input[i] != ',' && input[i] != '}' &&
                   !std::isspace(static_cast<unsigned char>(input[i]))) {
                ++i;
            }
            if (i == start) {
                error = "empty value";
                return false;
            }
            FlatValue value;
            value.text = std::string(input.substr(start, i - start));
            out[key] = std::move(value);
        }
        SkipSpace(input, i);
        if (i < input.size() && input[i] == ',') {
            ++i;
            continue;
        }
        if (i < input.size() && input[i] == '}') {
            ++i;
            return true;
        }
        error = "expected ',' or '}'";
        return false;
    }
}

std::string FlatJson::Serialize(const FlatMap& values) {
    std::string out = "{\n";
    std::size_t index = 0;
    for (const auto& [key, value] : values) {
        out += "  \"";
        out += Escape(key);
        out += "\": ";
        out += value.quoted ? "\"" + Escape(value.text) + "\"" : value.text;
        if (++index != values.size()) {
            out += ',';
        }
        out += '\n';
    }
    out += "}\n";
    return out;
}

void FlatJson::PutString(FlatMap& map, const std::string& key, const std::string& value) {
    map[key] = FlatValue{value, true};
}

void FlatJson::PutInt(FlatMap& map, const std::string& key, long long value) {
    map[key] = FlatValue{std::to_string(value), false};
}

void FlatJson::PutDouble(FlatMap& map, const std::string& key, double value) {
    map[key] = FlatValue{FormatDouble(value), false};
}

void FlatJson::PutBool(FlatMap& map, const std::string& key, bool value) {
    map[key] = FlatValue{value ? "true" : "false", false};
}

std::string FlatJson::GetString(const FlatMap& map, const std::string& key,
                                const std::string& fallback) {
    const auto it = map.find(key);
    return it == map.end() ? fallback : it->second.text;
}

long long FlatJson::GetInt(const FlatMap& map, const std::string& key, long long fallback) {
    const auto it = map.find(key);
    if (it == map.end()) {
        return fallback;
    }
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(it->second.text.c_str(), &end, 10);
    return (errno == 0 && end != it->second.text.c_str()) ? value : fallback;
}

double FlatJson::GetDouble(const FlatMap& map, const std::string& key, double fallback) {
    const auto it = map.find(key);
    if (it == map.end()) {
        return fallback;
    }
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(it->second.text.c_str(), &end);
    return (errno == 0 && end != it->second.text.c_str()) ? value : fallback;
}

bool FlatJson::GetBool(const FlatMap& map, const std::string& key, bool fallback) {
    const auto it = map.find(key);
    if (it == map.end()) {
        return fallback;
    }
    return it->second.text == "true" || it->second.text == "1";
}

}  // namespace brp::next
