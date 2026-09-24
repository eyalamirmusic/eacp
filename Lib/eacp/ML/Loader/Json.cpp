#include "Json.h"

#include <cctype>
#include <cstdlib>

namespace eacp::ML::Json
{
Value::Value()
    : data(nullptr)
{
}

Value::Value(std::nullptr_t nullValue)
    : data(nullValue)
{
}

Value::Value(bool boolValue)
    : data(boolValue)
{
}

Value::Value(double numberValue)
    : data(numberValue)
{
}

Value::Value(std::string stringValue)
    : data(std::move(stringValue))
{
}

Value::Value(Array arrayValue)
    : data(std::move(arrayValue))
{
}

Value::Value(Object objectValue)
    : data(std::move(objectValue))
{
}

bool Value::isNull() const
{
    return std::holds_alternative<std::nullptr_t>(data);
}

bool Value::isNumber() const
{
    return std::holds_alternative<double>(data);
}

bool Value::isString() const
{
    return std::holds_alternative<std::string>(data);
}

bool Value::isArray() const
{
    return std::holds_alternative<Array>(data);
}

bool Value::isObject() const
{
    return std::holds_alternative<Object>(data);
}

double Value::asNumber(double fallback) const
{
    auto number = std::get_if<double>(&data);
    return number != nullptr ? *number : fallback;
}

const std::string& Value::asString() const
{
    static const auto empty = std::string {};
    auto text = std::get_if<std::string>(&data);
    return text != nullptr ? *text : empty;
}

const Array& Value::asArray() const
{
    static const auto empty = Array {};
    auto array = std::get_if<Array>(&data);
    return array != nullptr ? *array : empty;
}

const Object& Value::asObject() const
{
    static const auto empty = Object {};
    auto object = std::get_if<Object>(&data);
    return object != nullptr ? *object : empty;
}

const Value* Value::find(const std::string& key) const
{
    auto object = std::get_if<Object>(&data);

    if (object == nullptr)
        return nullptr;

    auto found = object->find(key);
    return found == object->end() ? nullptr : &found->second;
}

namespace
{
class Parser
{
public:
    explicit Parser(std::string_view textToUse)
        : text(textToUse)
    {
    }

    std::optional<Value> parseDocument()
    {
        skipWhitespace();
        auto value = parseValue();

        if (!value.has_value())
            return std::nullopt;

        skipWhitespace();
        return value;
    }

private:
    std::string_view text;
    std::size_t cursor = 0;

    bool atEnd() const { return cursor >= text.size(); }

    char peek() const { return atEnd() ? '\0' : text[cursor]; }

    void advance() { ++cursor; }

    void skipWhitespace()
    {
        while (!atEnd()
               && (peek() == ' ' || peek() == '\t' || peek() == '\n'
                   || peek() == '\r'))
            advance();
    }

    bool consume(char expected)
    {
        if (peek() != expected)
        {
            return false;
        }

        advance();
        return true;
    }

    bool consumeLiteral(std::string_view literal)
    {
        if (text.substr(cursor, literal.size()) != literal)
        {
            return false;
        }

        cursor += literal.size();
        return true;
    }

    std::optional<Value> parseValue()
    {
        skipWhitespace();

        if (atEnd())
        {
            return std::nullopt;
        }

        switch (peek())
        {
            case '{':
                return parseObject();
            case '[':
                return parseArray();
            case '"':
                return parseStringValue();
            case 't':
                return consumeLiteral("true") ? std::optional {Value {true}}
                                              : std::nullopt;
            case 'f':
                return consumeLiteral("false") ? std::optional {Value {false}}
                                               : std::nullopt;
            case 'n':
                return consumeLiteral("null") ? std::optional {Value {nullptr}}
                                              : std::nullopt;
            default:
                return parseNumber();
        }
    }

    std::optional<Value> parseObject()
    {
        if (!consume('{'))
            return std::nullopt;

        auto object = Object {};
        skipWhitespace();

        if (peek() == '}')
        {
            advance();
            return Value {std::move(object)};
        }

        while (true)
        {
            skipWhitespace();
            auto key = parseRawString();

            if (!key.has_value())
                return std::nullopt;

            skipWhitespace();

            if (!consume(':'))
                return std::nullopt;

            auto value = parseValue();

            if (!value.has_value())
                return std::nullopt;

            object.emplace(std::move(*key), std::move(*value));
            skipWhitespace();

            if (peek() == ',')
            {
                advance();
                continue;
            }

            break;
        }

        skipWhitespace();

        if (!consume('}'))
            return std::nullopt;

        return Value {std::move(object)};
    }

    std::optional<Value> parseArray()
    {
        if (!consume('['))
            return std::nullopt;

        auto array = Array {};
        skipWhitespace();

        if (peek() == ']')
        {
            advance();
            return Value {std::move(array)};
        }

        while (true)
        {
            auto value = parseValue();

            if (!value.has_value())
                return std::nullopt;

            array.push_back(std::move(*value));
            skipWhitespace();

            if (peek() == ',')
            {
                advance();
                continue;
            }

            break;
        }

        skipWhitespace();

        if (!consume(']'))
            return std::nullopt;

        return Value {std::move(array)};
    }

    std::optional<Value> parseStringValue()
    {
        auto raw = parseRawString();

        if (!raw.has_value())
            return std::nullopt;

        return Value {std::move(*raw)};
    }

    std::optional<std::string> parseRawString()
    {
        if (!consume('"'))
            return std::nullopt;

        auto result = std::string {};

        while (!atEnd() && peek() != '"')
        {
            auto character = peek();

            if (character == '\\')
            {
                advance();

                if (atEnd())
                {
                    return std::nullopt;
                }

                auto escaped = peek();

                switch (escaped)
                {
                    case '"':
                        result += '"';
                        break;
                    case '\\':
                        result += '\\';
                        break;
                    case '/':
                        result += '/';
                        break;
                    case 'n':
                        result += '\n';
                        break;
                    case 't':
                        result += '\t';
                        break;
                    case 'r':
                        result += '\r';
                        break;
                    case 'b':
                        result += '\b';
                        break;
                    case 'f':
                        result += '\f';
                        break;
                    case 'u':
                        result += '?';
                        cursor += 4;
                        break;
                    default:
                        result += escaped;
                        break;
                }

                advance();
                continue;
            }

            result += character;
            advance();
        }

        if (!consume('"'))
            return std::nullopt;

        return result;
    }

    std::optional<Value> parseNumber()
    {
        auto start = cursor;

        if (peek() == '-')
            advance();

        while (!atEnd() && std::isdigit((unsigned char) peek()))
            advance();

        if (peek() == '.')
        {
            advance();

            while (!atEnd() && std::isdigit((unsigned char) peek()))
                advance();
        }

        if (peek() == 'e' || peek() == 'E')
        {
            advance();

            if (peek() == '+' || peek() == '-')
                advance();

            while (!atEnd() && std::isdigit((unsigned char) peek()))
                advance();
        }

        if (cursor == start)
        {
            return std::nullopt;
        }

        auto token = std::string {text.substr(start, cursor - start)};
        return Value {std::strtod(token.c_str(), nullptr)};
    }
};
}

std::optional<Value> parse(std::string_view text)
{
    auto parser = Parser {text};
    return parser.parseDocument();
}
}
