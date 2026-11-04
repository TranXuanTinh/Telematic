#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tcu::common {

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    using Array  = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;

    JsonValue() = default;
    JsonValue(std::nullptr_t) : type_(Type::Null) {}
    JsonValue(bool b) : type_(Type::Bool), bool_val_(b) {}
    JsonValue(int n) : type_(Type::Number), num_val_(static_cast<double>(n)) {}
    JsonValue(int64_t n) : type_(Type::Number), num_val_(static_cast<double>(n)) {}
    JsonValue(uint64_t n) : type_(Type::Number), num_val_(static_cast<double>(n)) {}
    JsonValue(uint32_t n) : type_(Type::Number), num_val_(static_cast<double>(n)) {}
    JsonValue(double d) : type_(Type::Number), num_val_(d) {}
    JsonValue(const char* s) : type_(Type::String), str_val_(s ? s : "") {}
    JsonValue(std::string s) : type_(Type::String), str_val_(std::move(s)) {}
    JsonValue(Array arr) : type_(Type::Array), arr_val_(std::move(arr)) {}
    JsonValue(Object obj) : type_(Type::Object), obj_val_(std::move(obj)) {}

    static JsonValue object() { return JsonValue(Object{}); }
    static JsonValue array() { return JsonValue(Array{}); }

    Type type() const noexcept { return type_; }
    bool isNull() const noexcept { return type_ == Type::Null; }
    bool isBool() const noexcept { return type_ == Type::Bool; }
    bool isNumber() const noexcept { return type_ == Type::Number; }
    bool isString() const noexcept { return type_ == Type::String; }
    bool isArray() const noexcept { return type_ == Type::Array; }
    bool isObject() const noexcept { return type_ == Type::Object; }

    bool asBool(bool def = false) const noexcept { return isBool() ? bool_val_ : def; }
    double asDouble(double def = 0.0) const noexcept { return isNumber() ? num_val_ : def; }
    int64_t asInt64(int64_t def = 0) const noexcept {
        return isNumber() ? static_cast<int64_t>(num_val_) : def;
    }
    uint64_t asUint64(uint64_t def = 0) const noexcept {
        return isNumber() ? static_cast<uint64_t>(num_val_) : def;
    }
    std::string asString(const std::string& def = "") const {
        return isString() ? str_val_ : def;
    }

    const Array& asArray() const;
    Array& asArray();
    const Object& asObject() const;
    Object& asObject();

    bool contains(const std::string& key) const;
    const JsonValue& operator[](const std::string& key) const;
    JsonValue& operator[](const std::string& key);
    void push_back(JsonValue v);

    std::string dump(bool pretty = false, int indent = 0) const;
    static std::optional<JsonValue> parse(std::string_view input);

private:
    Type        type_{Type::Null};
    bool        bool_val_{false};
    double      num_val_{0.0};
    std::string str_val_;
    Array       arr_val_;
    Object      obj_val_;
};

std::string bytesToHex(const uint8_t* data, size_t len);
inline std::string bytesToHex(const std::vector<uint8_t>& v) {
    return bytesToHex(v.data(), v.size());
}
std::vector<uint8_t> hexToBytes(std::string_view hex);

} // namespace tcu::common

