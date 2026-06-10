// Caption buttons for frameless eacp WebViews. An element marked
// `--eacp-window-button: minimize | maximize | close` becomes a working
// window control: a click posts the action to native, which drives the real
// window — no per-app message handlers, no platform sniffing in page code.
// Same custom-property scheme as window-drag.js, and for the same reason:
// custom properties read back through getComputedStyle on both engines.
//
// The shim also mirrors native facts onto <html>, so the rest is pure CSS:
//   data-eacp-platform="mac" | "windows"  — which chrome to render
//   data-eacp-maximized                   — swap the maximize/restore glyph
//
// The maximized attribute is updated when a marked button toggles it; a
// maximize from outside the page (e.g. Win+Up) is not observed.
(function () {
  if (window.__eacpResolveWindowButton) return;

  function markPlatform() {
    var html = document.documentElement;
    if (!html) return false;
    if (window.__eacpPlatform)
      html.setAttribute('data-eacp-platform', window.__eacpPlatform);
    return true;
  }

  if (!markPlatform())
    document.addEventListener('DOMContentLoaded', markPlatform);

  // Called by native after servicing a maximize toggle, so the page always
  // reflects the real window state instead of guessing.
  window.__eacpSetMaximized = function (maximized) {
    var html = document.documentElement;
    if (!html) return;
    if (maximized) html.setAttribute('data-eacp-maximized', '');
    else html.removeAttribute('data-eacp-maximized');
  };

  function resolveWindowButton(node) {
    for (var n = node; n && n.nodeType === 1; n = n.parentElement) {
      var value = window
        .getComputedStyle(n)
        .getPropertyValue('--eacp-window-button')
        .trim();
      if (value === 'minimize' || value === 'maximize' || value === 'close')
        return value;
    }
    return '';
  }

  // Exposed so the native test harness can assert classification directly.
  window.__eacpResolveWindowButton = resolveWindowButton;

  document.addEventListener(
    'click',
    function (event) {
      var action = resolveWindowButton(event.target);
      if (!action) return;
      if (
        window.webkit &&
        window.webkit.messageHandlers &&
        window.webkit.messageHandlers.__eacpWindowControl
      ) {
        window.webkit.messageHandlers.__eacpWindowControl.postMessage(action);
      }
    },
    true,
  );
})();
