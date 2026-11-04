#include "common/json_mini.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace tcu::common {

namespace {

std::string escapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

class Parser {
public:
    explicit Parser(std::string_view src) : src_(src) {}

    std::optional<JsonValue> parseRoot() {
        skipWs();
        auto val = parseValue();
        if (!val) return std::nullopt;
        skipWs();
        if (pos_ != src_.size()) return std::nullopt;
        return val;
    }

private:
    void skipWs() {
        while (pos_ < src_.size() && std::isspace(static_cast<unsigned char>(src_[pos_]))) {
            ++pos_;
        }
    }

    bool matchLiteral(std::string_view lit) {
        if (src_.substr(pos_, lit.size()) == lit) {
            pos_ += lit.size();
            return true;
        }
        return false;
    }

    std::optional<JsonValue> parseValue() {
        skipWs();
        if (pos_ >= src_.size()) return std::nullopt;
        char c = src_[pos_];
        if (c == 'n') {
            if (matchLiteral("null")) return JsonValue(nullptr);
            return std::nullopt;
        }
        if (c == 't') {
            if (matchLiteral("true")) return JsonValue(true);
            return std::nullopt;
        }
        if (c == 'f') {
            if (matchLiteral("false")) return JsonValue(false);
            return std::nullopt;
        }
        if (c == '"') {
            auto s = parseString();
            if (!s) return std::nullopt;
            return JsonValue(std::move(*s));
        }
        if (c == '[') return parseArray();
        if (c == '{') return parseObject();
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parseNumber();
        return std::nullopt;
    }

    std::optional<std::string> parseString() {
        if (pos_ >= src_.size() || src_[pos_] != '"') return std::nullopt;
        ++pos_;
        std::string out;
        while (pos_ < src_.size()) {
            char c = src_[pos_++];
            if (c == '"') return out;
            if (c == '\\') {
                if (pos_ >= src_.size()) return std::nullopt;
                char esc = src_[pos_++];
                switch (esc) {
                    case '"':  out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/'); break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        if (pos_ + 4 > src_.size()) return std::nullopt;
                        unsigned int code = 0;
                        for (int i = 0; i < 4; ++i) {
                            char hc = src_[pos_++];
                            code <<= 4U;
                            if (hc >= '0' && hc <= '9') code |= static_cast<unsigned int>(hc - '0');
                            else if (hc >= 'a' && hc <= 'f') code |= static_cast<unsigned int>(10 + hc - 'a');
                            else if (hc >= 'A' && hc <= 'F') code |= static_cast<unsigned int>(10 + hc - 'A');
                            else return std::nullopt;
                        }
                        out.push_back(static_cast<char>(code & 0x7FU));
                        break;
                    }
                    default:
                        return std::nullopt;
                }
            } else {
                out.push_back(c);
            }
        }
        return std::nullopt;
    }

    std::optional<JsonValue> parseNumber() {
        size_t start = pos_;
        if (src_[pos_] == '-') ++pos_;
        while (pos_ < src_.size() &&
               (std::isdigit(static_cast<unsigned char>(src_[pos_])) ||
                src_[pos_] == '.' || src_[pos_] == 'e' || src_[pos_] == 'E' ||
                src_[pos_] == '+' || src_[pos_] == '-')) {
            ++pos_;
        }
        std::string token(src_.substr(start, pos_ - start));
        char* endptr = nullptr;
        double val = std::strtod(token.c_str(), &endptr);
        if (endptr == token.c_str() || *endptr != '\0') return std::nullopt;
        return JsonValue(val);
    }

    std::optional<JsonValue> parseArray() {
        ++pos_; // skip '['
        JsonValue::Array arr;
        skipWs();
        if (pos_ < src_.size() && src_[pos_] == ']') {
            ++pos_;
            return JsonValue(std::move(arr));
        }
        while (pos_ < src_.size()) {
            auto elem = parseValue();
            if (!elem) return std::nullopt;
            arr.push_back(std::move(*elem));
            skipWs();
            if (pos_ < src_.size() && src_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < src_.size() && src_[pos_] == ']') {
                ++pos_;
                return JsonValue(std::move(arr));
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    std::optional<JsonValue> parseObject() {
        ++pos_; // skip '{'
        JsonValue::Object obj;
        skipWs();
        if (pos_ < src_.size() && src_[pos_] == '}') {
            ++pos_;
            return JsonValue(std::move(obj));
        }
        while (pos_ < src_.size()) {
            skipWs();
            auto key = parseString();
            if (!key) return std::nullopt;
            skipWs();
            if (pos_ >= src_.size() || src_[pos_] != ':') return std::nullopt;
            ++pos_;
            auto val = parseValue();
            if (!val) return std::nullopt;
            obj[*key] = std::move(*val);
            skipWs();
            if (pos_ < src_.size() && src_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < src_.size() && src_[pos_] == '}') {
                ++pos_;
                return JsonValue(std::move(obj));
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    std::string_view src_;
    size_t           pos_{0};
};

} // namespace

const JsonValue::Array& JsonValue::asArray() const {
    static const Array kEmpty;
    return isArray() ? arr_val_ : kEmpty;
}

JsonValue::Array& JsonValue::asArray() {
    if (!isArray()) {
        type_ = Type::Array;
        arr_val_.clear();
    }
    return arr_val_;
}

const JsonValue::Object& JsonValue::asObject() const {
    static const Object kEmpty;
    return isObject() ? obj_val_ : kEmpty;
}

JsonValue::Object& JsonValue::asObject() {
    if (!isObject()) {
        type_ = Type::Object;
        obj_val_.clear();
    }
    return obj_val_;
}

bool JsonValue::contains(const std::string& key) const {
    return isObject() && (obj_val_.find(key) != obj_val_.end());
}

const JsonValue& JsonValue::operator[](const std::string& key) const {
    static const JsonValue kNull;
    if (!isObject()) return kNull;
    auto it = obj_val_.find(key);
    return (it != obj_val_.end()) ? it->second : kNull;
}

JsonValue& JsonValue::operator[](const std::string& key) {
    if (!isObject()) {
        type_ = Type::Object;
        obj_val_.clear();
    }
    return obj_val_[key];
}

void JsonValue::push_back(JsonValue v) {
    if (!isArray()) {
        type_ = Type::Array;
        arr_val_.clear();
    }
    arr_val_.push_back(std::move(v));
}

std::string JsonValue::dump(bool pretty, int indent) const {
    const std::string pad(pretty ? static_cast<size_t>(indent) : 0, ' ');
    const std::string pad_inner(pretty ? static_cast<size_t>(indent + 2) : 0, ' ');
    switch (type_) {
        case Type::Null:
            return "null";
        case Type::Bool:
            return bool_val_ ? "true" : "false";
        case Type::Number: {
            if (std::floor(num_val_) == num_val_ && std::fabs(num_val_) < 9e15) {
                return std::to_string(static_cast<int64_t>(num_val_));
            }
            std::ostringstream oss;
            oss.precision(8);
            oss << num_val_;
            return oss.str();
        }
        case Type::String:
            return "\"" + escapeJsonString(str_val_) + "\"";
        case Type::Array: {
            if (arr_val_.empty()) return "[]";
            std::string out = pretty ? "[\n" : "[";
            for (size_t i = 0; i < arr_val_.size(); ++i) {
                if (pretty) out += pad_inner;
                out += arr_val_[i].dump(pretty, indent + 2);
                if (i + 1 < arr_val_.size()) out += ",";
                if (pretty) out += "\n";
            }
            out += (pretty ? pad : "") + "]";
            return out;
        }
        case Type::Object: {
            if (obj_val_.empty()) return "{}";
            std::string out = pretty ? "{\n" : "{";
            size_t idx = 0;
            for (const auto& [k, v] : obj_val_) {
                if (pretty) out += pad_inner;
                out += "\"" + escapeJsonString(k) + "\":" + (pretty ? " " : "");
                out += v.dump(pretty, indent + 2);
                if (++idx < obj_val_.size()) out += ",";
                if (pretty) out += "\n";
            }
            out += (pretty ? pad : "") + "}";
            return out;
        }
    }
    return "null";
}

std::optional<JsonValue> JsonValue::parse(std::string_view input) {
    Parser p(input);
    return p.parseRoot();
}

std::string bytesToHex(const uint8_t* data, size_t len) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4U) & 0x0FU]);
        out.push_back(kHex[data[i] & 0x0FU]);
    }
    return out;
}

std::vector<uint8_t> hexToBytes(std::string_view hex) {
    if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex.remove_prefix(2);
    }
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        int hi = nibble(hex[i]);
        int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) break;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

} // namespace tcu::common

