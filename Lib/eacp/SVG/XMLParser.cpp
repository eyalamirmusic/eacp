#include "XMLParser.h"

namespace eacp::SVG
{

struct XMLReader
{
    std::string_view src;
    size_t pos = 0;

    bool atEnd() const { return pos >= src.size(); }
    char peek() const { return src[pos]; }
    char advance() { return src[pos++]; }

    void skipWhitespace()
    {
        while (!atEnd() && std::isspace(static_cast<unsigned char>(peek())))
            advance();
    }

    bool match(std::string_view token)
    {
        if (src.substr(pos, token.size()) == token)
        {
            pos += token.size();
            return true;
        }
        return false;
    }

    void skipDeclaration()
    {
        while (!atEnd() && !match("?>"))
            advance();
    }

    void skipComment()
    {
        while (!atEnd() && !match("-->"))
            advance();
    }

    void skipDoctype()
    {
        auto depth = 1;
        while (!atEnd() && depth > 0)
        {
            if (peek() == '<')
                ++depth;
            else if (peek() == '>')
                --depth;
            advance();
        }
    }

    std::string readName()
    {
        std::string name;
        while (!atEnd() && !std::isspace(static_cast<unsigned char>(peek()))
               && peek() != '>' && peek() != '/' && peek() != '=')
        {
            name += advance();
        }
        return name;
    }

    std::string readQuotedValue()
    {
        if (atEnd())
            return "";
        auto quote = advance();
        std::string value;
        while (!atEnd() && peek() != quote)
            value += advance();
        if (!atEnd())
            advance();
        return value;
    }

    std::string stripNamespace(const std::string& name)
    {
        auto colon = name.find(':');
        if (colon != std::string::npos)
            return name.substr(colon + 1);
        return name;
    }

    std::optional<SVGElement> parseElement()
    {
        skipWhitespace();
        if (atEnd() || peek() != '<')
            return std::nullopt;

        advance(); // '<'

        if (match("?"))
        {
            skipDeclaration();
            return parseElement();
        }
        if (match("!--"))
        {
            skipComment();
            return parseElement();
        }
        if (match("!DOCTYPE") || match("!doctype"))
        {
            skipDoctype();
            return parseElement();
        }

        SVGElement element;
        element.tag = stripNamespace(readName());
        parseAttributes(element);

        skipWhitespace();
        if (match("/>"))
            return element;

        if (!atEnd() && peek() == '>')
            advance();

        parseChildren(element);
        return element;
    }

    void parseAttributes(SVGElement& element)
    {
        while (true)
        {
            skipWhitespace();
            if (atEnd() || peek() == '>' || peek() == '/')
                break;

            auto name = stripNamespace(readName());
            skipWhitespace();

            if (!atEnd() && peek() == '=')
            {
                advance();
                skipWhitespace();
                auto value = readQuotedValue();
                element.attributes[name] = value;
            }
        }
    }

    void parseChildren(SVGElement& element)
    {
        while (!atEnd())
        {
            skipWhitespace();
            if (atEnd())
                break;

            if (peek() == '<')
            {
                if (src.substr(pos, 2) == "</")
                {
                    skipClosingTag();
                    break;
                }
                if (src.substr(pos, 4) == "<!--")
                {
                    pos += 4;
                    skipComment();
                    continue;
                }
                auto child = parseElement();
                if (child)
                    element.children.push_back(std::move(*child));
            }
            else
            {
                element.textContent += readTextContent();
            }
        }
    }

    std::string readTextContent()
    {
        std::string text;
        while (!atEnd() && peek() != '<')
            text += advance();
        return text;
    }

    void skipClosingTag()
    {
        while (!atEnd() && peek() != '>')
            advance();
        if (!atEnd())
            advance();
    }
};

std::optional<SVGElement> parseXML(std::string_view input)
{
    XMLReader reader {input};
    return reader.parseElement();
}

} // namespace eacp::SVG
