#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace eacp::ML::Json
{
class Value;

using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

class Value
{
public:
    Value();
    explicit Value(std::nullptr_t nullValue);
    explicit Value(bool boolValue);
    explicit Value(double numberValue);
    explicit Value(std::string stringValue);
    explicit Value(Array arrayValue);
    explicit Value(Object objectValue);

    bool isNull() const;
    bool isNumber() const;
    bool isString() const;
    bool isArray() const;
    bool isObject() const;

    double asNumber(double fallback = 0.0) const;
    const std::string& asString() const;
    const Array& asArray() const;
    const Object& asObject() const;

    const Value* find(const std::string& key) const;

private:
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> data;
};

std::optional<Value> parse(std::string_view text);
}
