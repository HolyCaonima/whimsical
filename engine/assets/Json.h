#pragma once
#include <map>
#include <string>
#include <variant>
#include <vector>
#include <cstdint>

namespace whimsical {
// An owning document value. Parsing uses a private Duktape heap, never the gameplay VM.
class Json {
  public:
    using Object = std::map<std::string, Json>;
    using Array = std::vector<Json>;
    using Value = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;
    Json() : value_(nullptr) {}
    Json(bool v) : value_(v) {}
    Json(double v);
    Json(float v) : Json(double(v)) {}
    Json(int v) : Json(double(v)) {}
    Json(uint32_t v) : Json(double(v)) {}
    Json(const char* v) : value_(std::string(v)) {}
    Json(std::string v) : value_(std::move(v)) {}
    Json(Array v) : value_(std::move(v)) {}
    Json(Object v) : value_(std::move(v)) {}
    Json(std::initializer_list<Object::value_type> v) : value_(Object(v)) {}
    static Json array(Array v = {}) {
        return Json(std::move(v));
    }
    static Json object() {
        return Json(Object{});
    }
    static Json parse(const std::string&);
    std::string dump() const;
    bool null() const {
        return std::holds_alternative<std::nullptr_t>(value_);
    }
    const std::string& string() const {
        return std::get<std::string>(value_);
    }
    double number() const {
        return std::get<double>(value_);
    }
    uint32_t uint() const;
    bool boolean() const {
        return std::get<bool>(value_);
    }
    const Array& elements() const {
        return std::get<Array>(value_);
    }
    const Object& members() const {
        return std::get<Object>(value_);
    }
    const Json& at(const std::string& key) const {
        return members().at(key);
    }
    const Json& at(size_t i) const {
        return elements().at(i);
    }
    bool contains(const std::string& key) const {
        return members().count(key) != 0;
    }
    Json& operator[](const std::string& key) {
        return std::get<Object>(value_)[key];
    }
    void push(Json v) {
        std::get<Array>(value_).push_back(std::move(v));
    }
    const Value& value() const {
        return value_;
    }
    bool operator==(const Json& other) const {
        return value_ == other.value_;
    }

  private:
    Value value_;
};
} // namespace whimsical
