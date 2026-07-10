// Unhandled-key reporting for embedded eacp WebViews. WKWebView swallows every
// key event whether or not the page cared, which strands host shortcuts (a
// DAW's spacebar transport, say) whenever the web view holds keyboard focus.
// This shim watches every key event and tells the native side whether the page
// consumed it, so the unconsumed ones can be re-dispatched up the responder
// chain. A page consumes a key by calling preventDefault(), or implicitly by
// having it land on a control that uses it (typing in a text field, arrows on
// a slider, Space/Enter on a button).
(function () {
  if (window.__eacpKeyEventsInstalled) return;
  window.__eacpKeyEventsInstalled = true;

  var rangeKeys = {
    ArrowLeft: 1,
    ArrowRight: 1,
    ArrowUp: 1,
    ArrowDown: 1,
    Home: 1,
    End: 1,
    PageUp: 1,
    PageDown: 1,
  };
  var activationKeys = { ' ': 1, Enter: 1 };
  var clickInputTypes = { checkbox: 1, radio: 1, button: 1, submit: 1, reset: 1 };

  // Whether the focused control's default action uses this key: only the keys
  // a control actually reacts to count, so Space still reaches the host while
  // a slider is focused, but arrows adjusting that slider stay in the page.
  function targetConsumes(event) {
    var el = event.target;
    if (!el || el.nodeType !== 1) return false;
    if (el.isContentEditable) return true;
    var tag = el.tagName;
    if (tag === 'TEXTAREA' || tag === 'SELECT') return true;
    if (tag === 'BUTTON') return !!activationKeys[event.key];
    if (tag !== 'INPUT') return false;
    var type = (el.getAttribute('type') || 'text').toLowerCase();
    if (type === 'range') return !!rangeKeys[event.key];
    if (clickInputTypes[type]) return !!activationKeys[event.key];
    return true; // text-like input: it eats whatever is typed
  }

  function report(kind, event) {
    // setTimeout(0) runs after the event's whole dispatch task (a microtask
    // would fire between listeners), so defaultPrevented is final by the time
    // we look. Same-delay timers fire FIFO, so the native side receives one
    // verdict per key event in delivery order -- which is what lets it match
    // verdicts against its queue of stashed native events.
    setTimeout(function () {
      var consumed =
        event.defaultPrevented ||
        event.isComposing ||
        event.key === 'Tab' || // in-page focus navigation
        targetConsumes(event);
      // Message: "<down|up>:<0|1>:<keyCode>:<mods>:<repeat>:<key>". macOS reads
      // only the first two fields -- it re-dispatches the native event it
      // stashed. Windows keeps no native stash, so it rebuilds the key from the
      // identity fields: keyCode (a Win32 virtual key), a modifier bitmask
      // (shift 1, ctrl 2, alt 4, meta 8), the repeat flag, and event.key last so
      // a ':' key can never split the message.
      var mods =
        (event.shiftKey ? 1 : 0) |
        (event.ctrlKey ? 2 : 0) |
        (event.altKey ? 4 : 0) |
        (event.metaKey ? 8 : 0);
      try {
        window.webkit.messageHandlers.__eacpKeyEvent.postMessage(
          kind +
            ':' +
            (consumed ? '1' : '0') +
            ':' +
            (event.keyCode || 0) +
            ':' +
            mods +
            ':' +
            (event.repeat ? '1' : '0') +
            ':' +
            (event.key || ''),
        );
      } catch (err) {}
    }, 0);
  }

  window.addEventListener(
    'keydown',
    function (event) {
      // Cmd combos travel AppKit's key-equivalent path, not keyDown:, so the
      // native side has nothing stashed for them -- reporting one here would
      // mispair a later verdict. Unhandled ones reach the host natively anyway.
      if (!event.metaKey) report('down', event);
    },
    true,
  );

  window.addEventListener(
    'keyup',
    function (event) {
      if (!event.metaKey) report('up', event);
    },
    true,
  );
})();
