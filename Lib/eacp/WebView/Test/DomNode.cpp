#include "DomNode.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace eacp::WebView::Test
{

namespace
{

struct AttrCondition
{
    std::string name;
    std::optional<std::string> value;
};

// One compound selector — e.g. "li.todo-item[data-testid='x']". A full
// selector is a sequence of these separated by the descendant combinator.
struct Compound
{
    std::string tag; // empty or "*" -> any tag
    std::string id;  // empty -> no id constraint
    std::vector<std::string> classes;
    std::vector<AttrCondition> attrs;
};

bool isNameBoundary(char c)
{
    return c == '.' || c == '#' || c == '[';
}

bool isAsciiSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

std::string trimAscii(std::string_view input)
{
    auto begin = input.find_first_not_of(" \t\n\r");
    if (begin == std::string_view::npos)
        return {};

    auto end = input.find_last_not_of(" \t\n\r");
    return std::string {input.substr(begin, end - begin + 1)};
}

std::string stripQuotes(std::string_view input)
{
    if (input.size() >= 2 && (input.front() == '"' || input.front() == '\'')
        && input.back() == input.front())
        return std::string {input.substr(1, input.size() - 2)};

    return std::string {input};
}

AttrCondition parseAttr(std::string_view body)
{
    auto eq = body.find('=');
    if (eq == std::string_view::npos)
        return AttrCondition {trimAscii(body), std::nullopt};

    auto name = trimAscii(body.substr(0, eq));
    auto value = stripQuotes(trimAscii(body.substr(eq + 1)));
    return AttrCondition {std::move(name), std::move(value)};
}

Compound parseCompound(std::string_view token)
{
    auto compound = Compound {};
    auto i = std::size_t {0};

    auto readName = [&]() -> std::string
    {
        auto start = i;
        while (i < token.size() && !isNameBoundary(token[i]))
            ++i;
        return std::string {token.substr(start, i - start)};
    };

    while (i < token.size())
    {
        auto c = token[i];
        if (c == '.')
        {
            ++i;
            compound.classes.push_back(readName());
        }
        else if (c == '#')
        {
            ++i;
            compound.id = readName();
        }
        else if (c == '[')
        {
            auto close = token.find(']', i);
            auto end = close == std::string_view::npos ? token.size() : close;
            compound.attrs.push_back(parseAttr(token.substr(i + 1, end - i - 1)));
            i = close == std::string_view::npos ? end : close + 1;
        }
        else
        {
            compound.tag = readName();
        }
    }

    return compound;
}

std::vector<Compound> parseSteps(std::string_view selector)
{
    auto steps = std::vector<Compound> {};
    auto token = std::string {};
    auto bracketDepth = 0;

    auto flush = [&]()
    {
        if (!token.empty())
        {
            steps.push_back(parseCompound(token));
            token.clear();
        }
    };

    for (auto c: selector)
    {
        if (c == '[')
            ++bracketDepth;
        else if (c == ']' && bracketDepth > 0)
            --bracketDepth;

        if (isAsciiSpace(c) && bracketDepth == 0)
            flush();
        else
            token.push_back(c);
    }
    flush();

    if (steps.empty())
        throw std::runtime_error("DomNode: empty selector");

    return steps;
}

bool matchesCompound(const DomNode& node, const Compound& compound)
{
    if (!compound.tag.empty() && compound.tag != "*" && compound.tag != node.tag())
        return false;

    if (!compound.id.empty() && node.attr("id") != compound.id)
        return false;

    for (const auto& className: compound.classes)
        if (!node.hasClass(className))
            return false;

    for (const auto& condition: compound.attrs)
    {
        if (condition.value)
        {
            if (node.attr(condition.name) != *condition.value)
                return false;
        }
        else if (!node.hasAttr(condition.name))
        {
            return false;
        }
    }

    return true;
}

void collectMatching(const DomNode& node,
                     const Compound& compound,
                     std::vector<const DomNode*>& out)
{
    for (const auto& child: node.children)
    {
        if (matchesCompound(child, compound))
            out.push_back(&child);

        collectMatching(child, compound, out);
    }
}

std::vector<const DomNode*> findChain(const DomNode& root,
                                      const std::vector<Compound>& steps,
                                      std::size_t stepIndex)
{
    auto matches = std::vector<const DomNode*> {};
    collectMatching(root, steps[stepIndex], matches);

    if (stepIndex + 1 == steps.size())
        return matches;

    auto out = std::vector<const DomNode*> {};
    for (const auto* match: matches)
        for (const auto* node: findChain(*match, steps, stepIndex + 1))
            if (std::find(out.begin(), out.end(), node) == out.end())
                out.push_back(node);

    return out;
}

} // namespace

const std::string& DomNode::tag() const
{
    return tagName;
}

const std::string& DomNode::text() const
{
    return textContent;
}

std::optional<std::string> DomNode::attr(std::string_view name) const
{
    auto it = attributes.find(std::string {name});
    if (it == attributes.end())
        return std::nullopt;

    return it->second;
}

bool DomNode::hasAttr(std::string_view name) const
{
    return attributes.find(std::string {name}) != attributes.end();
}

std::vector<std::string> DomNode::classes() const
{
    auto result = std::vector<std::string> {};

    auto it = attributes.find("class");
    if (it == attributes.end())
        return result;

    auto current = std::string {};
    for (auto c: it->second)
    {
        if (isAsciiSpace(c))
        {
            if (!current.empty())
            {
                result.push_back(current);
                current.clear();
            }
        }
        else
        {
            current.push_back(c);
        }
    }
    if (!current.empty())
        result.push_back(current);

    return result;
}

bool DomNode::hasClass(std::string_view className) const
{
    for (const auto& name: classes())
        if (name == className)
            return true;

    return false;
}

std::optional<DomNode> DomNode::tryFind(std::string_view selector) const
{
    auto steps = parseSteps(selector);
    auto matches = findChain(*this, steps, 0);
    if (matches.empty())
        return std::nullopt;

    return *matches.front();
}

DomNode DomNode::find(std::string_view selector) const
{
    auto found = tryFind(selector);
    if (!found)
        throw std::runtime_error("DomNode: no descendant matches selector: "
                                 + std::string {selector});

    return std::move(*found);
}

std::vector<DomNode> DomNode::findAll(std::string_view selector) const
{
    auto steps = parseSteps(selector);
    auto matches = findChain(*this, steps, 0);

    auto result = std::vector<DomNode> {};
    result.reserve(matches.size());
    for (const auto* node: matches)
        result.push_back(*node);

    return result;
}

} // namespace eacp::WebView::Test
