// Window dragging for frameless eacp WebViews. WKWebView/WebView2 ignore
// `-webkit-app-region` (an Electron/Chromium feature), so we re-create it: a
// left mousedown over a `--eacp-app-region: drag` region asks the native side
// to start a window drag. We read the CUSTOM property (not `-webkit-app-region`)
// because WKWebView drops unknown native props from getComputedStyle, while
// custom properties are exposed and inherit -- so the whole bar drags and
// children opt out with `no-drag`.
(function () {
  if (window.__eacpResolveAppRegion) return;

  function resolveAppRegion(node) {
    for (var n = node; n && n.nodeType === 1; n = n.parentElement) {
      var style = window.getComputedStyle(n);
      var value = style.getPropertyValue('--eacp-app-region').trim();
      if (!value) value = style.getPropertyValue('-webkit-app-region').trim();
      if (value === 'drag' || value === 'no-drag') return value;
    }
    return '';
  }

  // Exposed so the native test harness can assert classification directly.
  window.__eacpResolveAppRegion = resolveAppRegion;

  function armWindowDrag() {
    if (
      window.webkit &&
      window.webkit.messageHandlers &&
      window.webkit.messageHandlers.__eacpWindowDrag
    ) {
      // Must be a string: a numeric body throws in the native JSON path.
      window.webkit.messageHandlers.__eacpWindowDrag.postMessage('drag');
    }
  }

  document.addEventListener(
    'mousedown',
    function (event) {
      if (event.button !== 0) return;
      if (resolveAppRegion(event.target) === 'drag') armWindowDrag();
    },
    true,
  );
})();
