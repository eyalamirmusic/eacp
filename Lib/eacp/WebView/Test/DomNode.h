#pragma once

#include <Miro/Miro.h>

#include <eacp/Core/Utils/Containers.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace eacp::WebView::Test
{

// A captured snapshot of a DOM element and its element subtree, taken by
// AppDriver::query() / queryAll(). The raw fields below are filled by
// Miro::fromJSON from the JSON the test agent serialises; the accessor
// methods (tag/text/attr/find/...) are the ergonomic surface tests use.
//
// It is a value, not a live handle — once captured it never re-reads the
// page, so find()/findAll() walk the captured tree in C++ with no extra
// round-trips. Re-query through the driver to observe later DOM changes.
struct DomNode
{
    // Lower-cased tag name ("li", "input", ...).
    std::string tagName;

    // Every attribute on the element, keyed by name.
    std::map<std::string, std::string> attributes;

    // Trimmed textContent of the element (includes descendant text).
    std::string textContent;

    // Current value of a form control ("" for elements without one).
    std::string value;

    // Checkbox / radio checked state (false for everything else).
    bool checked = false;

    // Element children only — text nodes are folded into textContent.
    Vector<DomNode> children;

    MIRO_REFLECT(tagName, attributes, textContent, value, checked, children)

    const std::string& tag() const;
    const std::string& text() const;

    // Attribute lookup. attr() is empty when the attribute is absent;
    // hasAttr() distinguishes "absent" from "present but empty".
    std::optional<std::string> attr(std::string_view name) const;
    bool hasAttr(std::string_view name) const;

    // Class helpers, parsed from the class attribute.
    Vector<std::string> classes() const;
    bool hasClass(std::string_view className) const;

    // Descendant search over the captured subtree — self is never
    // matched, mirroring element.querySelector. Supports a CSS subset:
    // tag, .class, #id, [attr], [attr=value] (quoted or bare), * and the
    // descendant combinator (space), plus the @id shorthand for the
    // ElementIds attribute ("@todo-item" matches
    // [data-testid="todo-item"]). find() throws when nothing matches;
    // tryFind() returns nullopt; findAll() returns every match (document
    // order for a single compound selector).
    DomNode find(std::string_view selector) const;
    std::optional<DomNode> tryFind(std::string_view selector) const;
    Vector<DomNode> findAll(std::string_view selector) const;
};

} // namespace eacp::WebView::Test
