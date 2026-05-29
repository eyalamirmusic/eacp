// Injected once at document-start by the embedded test server.
// Provides synchronous DOM helpers only — all "async-feeling"
// operations (waitFor) are driven from C++ by repeatedly evaluating
// __test.exists, so the agent itself never returns a Promise. WebKit's
// evaluateJavaScript callback hands back the *synchronous* result of
// the script's last expression, so async helpers wouldn't round-trip.
(function() {
  if (window.__test) return;

  function $(sel) {
    var el = document.querySelector(sel);
    if (!el) throw new Error("__test: element not found: " + sel);
    return el;
  }

  function fireMouse(el, type) {
    var rect = el.getBoundingClientRect();
    el.dispatchEvent(new MouseEvent(type, {
      bubbles: true, cancelable: true, view: window,
      clientX: rect.left + rect.width / 2,
      clientY: rect.top + rect.height / 2,
    }));
  }

  function serializeEl(el) {
    // Element subtree -> the JSON shape AppDriver deserialises into a
    // DomNode (see DomNode.h). Only element children are walked; text
    // nodes are folded into textContent so the tree stays element-only.
    var attrs = {};
    for (var i = 0; i < el.attributes.length; i++) {
      attrs[el.attributes[i].name] = el.attributes[i].value;
    }
    var kids = [];
    for (var j = 0; j < el.children.length; j++) {
      kids.push(serializeEl(el.children[j]));
    }
    return {
      tagName: (el.tagName || '').toLowerCase(),
      attributes: attrs,
      textContent: (el.textContent || '').trim(),
      value: (el.value != null ? String(el.value) : ''),
      checked: !!el.checked,
      children: kids,
    };
  }

  function setNativeValue(el, value) {
    // React tracks input values via an internal cached getter/setter;
    // assigning .value directly bypasses React's onChange. The trick
    // is to call the prototype setter, then dispatch the native input
    // event — that's what React's synthetic event system listens for.
    var proto = Object.getPrototypeOf(el);
    var desc = Object.getOwnPropertyDescriptor(proto, 'value');
    if (desc && desc.set) desc.set.call(el, value);
    else el.value = value;
  }

  window.__test = {
    click: function(sel) {
      var el = $(sel);
      fireMouse(el, 'mousedown');
      fireMouse(el, 'mouseup');
      el.click();
      return true;
    },
    fill: function(sel, value) {
      var el = $(sel);
      el.focus();
      setNativeValue(el, String(value));
      el.dispatchEvent(new Event('input', { bubbles: true }));
      el.dispatchEvent(new Event('change', { bubbles: true }));
      return true;
    },
    press: function(sel, key) {
      var el = $(sel);
      el.focus();
      var opts = { bubbles: true, cancelable: true, key: key };
      el.dispatchEvent(new KeyboardEvent('keydown', opts));
      el.dispatchEvent(new KeyboardEvent('keyup', opts));
      return true;
    },
    submit: function(sel) {
      var el = $(sel);
      if (typeof el.requestSubmit === 'function') el.requestSubmit();
      else el.submit();
      return true;
    },
    text: function(sel) {
      return ($(sel).textContent || '').trim();
    },
    attr: function(sel, name) {
      return $(sel).getAttribute(name);
    },
    exists: function(sel) {
      return !!document.querySelector(sel);
    },
    count: function(sel) {
      return document.querySelectorAll(sel).length;
    },
    evaluate: function(expr) {
      return Function('"use strict"; return (' + expr + ');')();
    },
    dom: function(sel) {
      // No selector → whole document. Avoids forcing tests to know
      // about <html> vs document.body; outerHTML on documentElement
      // serialises the full tree including the root tag.
      if (sel == null || sel === '') return document.documentElement.outerHTML;
      return $(sel).outerHTML;
    },
    queryNode: function(sel) {
      // null (not a throw) when nothing matches — AppDriver maps that to
      // an empty std::optional<DomNode> so callers can gate on it.
      var el = document.querySelector(sel);
      return el ? serializeEl(el) : null;
    },
    queryNodes: function(sel) {
      return Array.prototype.map.call(
        document.querySelectorAll(sel), serializeEl);
    }
  };
})();
