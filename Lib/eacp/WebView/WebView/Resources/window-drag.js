// Frameless-window dragging for eacp WebViews.
//
// Electron's Chromium honours `-webkit-app-region: drag` and moves the window
// when you drag such a region. WKWebView does not -- it ignores the property
// entirely -- so a page that only sets `-webkit-app-region` has no draggable
// chrome under eacp on macOS. We re-implement the affordance here: on a left
// mousedown over a region that resolves to "drag", we tell the native side to
// arm a window drag, and the next mouseDragged: starts the OS move loop
// (mirroring how native file drag-out is armed on mousedown and fired on drag).
//
// Regions are marked with the `--eacp-app-region` CSS custom property
// ("drag" / "no-drag"). We key off a custom property rather than reading
// `-webkit-app-region` directly because WKWebView drops unknown native
// properties from getComputedStyle (so it would always read empty), whereas
// custom properties are always exposed AND inherit -- which gives us the same
// "the whole bar drags, opt-out individual controls" semantics Electron has,
// for free. `-webkit-app-region` is still consulted as a fallback so pages
// authored for Electron keep working wherever the engine does expose it.
(function () {
  if (window.__eacpResolveAppRegion) return;

  // Nearest-ancestor-wins resolution. The custom property inherits, so for the
  // custom-property path the answer is already on `node` itself; the walk
  // matters only for the non-inheriting `-webkit-app-region` fallback.
  function resolveAppRegion(node) {
    for (var n = node; n && n.nodeType === 1; n = n.parentElement) {
      var style = window.getComputedStyle(n);
      var value = style.getPropertyValue('--eacp-app-region').trim();
      if (!value) value = style.getPropertyValue('-webkit-app-region').trim();
      if (value === 'drag' || value === 'no-drag') return value;
    }
    return '';
  }

  // Exposed (not just used internally) so the native test harness can assert
  // the classification without synthesising real mouse events.
  window.__eacpResolveAppRegion = resolveAppRegion;

  function armWindowDrag() {
    if (
      window.webkit &&
      window.webkit.messageHandlers &&
      window.webkit.messageHandlers.__eacpWindowDrag
    ) {
      window.webkit.messageHandlers.__eacpWindowDrag.postMessage(1);
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
