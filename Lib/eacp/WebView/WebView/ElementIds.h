#pragma once

#include <string>
#include <string_view>

namespace eacp::Graphics::ElementIds
{

// Process-wide name of the DOM attribute that marks an element with a
// stable, explicit id: <button data-testid="add-todo">. These ids are
// the contract between the page and everything that drives or observes
// it from outside — AppDriver tests, the remote debug server, and
// event tracking — so treat them as part of the app's surface, not
// test-only markup.
//
// Apps that want their own branding set the name once at startup,
// before constructing any WebView / bridge / driver:
//     ElementIds::setAttributeName("data-myapp");
std::string attributeName();
void setAttributeName(std::string name);

// CSS attribute selector matching one id: [data-testid="add-todo"].
std::string selectorFor(std::string_view id);

// Expands the @id shorthand inside a CSS selector:
//     "@todo-list li"    -> '[data-testid="todo-list"] li'
//     "@todo-item.done"  -> '[data-testid="todo-item"].done'
// An @id is recognised at the start of each compound (after
// whitespace, a comma, or an opening paren) and may be followed by
// further constraints. Selectors without @ pass through unchanged.
std::string resolveSelector(std::string_view selector);

} // namespace eacp::Graphics::ElementIds
