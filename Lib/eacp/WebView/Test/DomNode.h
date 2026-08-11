#pragma once

#include "../Common.h"

#include <map>

namespace eacp::WebView::Test
{

// A value, not a live handle: a captured element subtree that never
// re-reads the page. Re-query through AppDriver to see later DOM changes.
struct DomNode
{
    const std::string& tag() const;
    const std::string& text() const;

    std::optional<std::string> attr(std::string_view name) const;
    bool hasAttr(std::string_view name) const;

    Vector<std::string> classes() const;
    bool hasClass(std::string_view className) const;

    // Descendants only, like element.querySelector. Supports tag, .class,
    // #id, [attr], [attr=value], * and the descendant combinator.
    // find() throws when nothing matches; tryFind() returns nullopt.
    DomNode find(std::string_view selector) const;
    std::optional<DomNode> tryFind(std::string_view selector) const;
    Vector<DomNode> findAll(std::string_view selector) const;

    // Lower-cased ("li", "input", ...).
    std::string tagName;

    std::map<std::string, std::string> attributes;

    // Trimmed, and includes descendant text.
    std::string textContent;

    // Form-control value, "" for elements without one.
    std::string value;

    bool checked = false;

    // Element children only — text nodes fold into textContent.
    Vector<DomNode> children;

    MIRO_REFLECT(tagName, attributes, textContent, value, checked, children)
};

} // namespace eacp::WebView::Test
