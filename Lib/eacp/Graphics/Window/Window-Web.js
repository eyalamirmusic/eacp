// The DOM half of Window-Web.cpp: one <canvas> per window id, its pointer,
// wheel, key, focus and layout events forwarded to the eacp_web_window_*
// exports, plus what Display, SystemAppearance and AnimationFrame ask of the
// page. Under node there is no document: has_dom answers 0 and every query
// answers as an empty page would. When the runtime exits every listener,
// observer and pending frame is released, since the program they would call
// into is gone.

addToLibrary({
  $EacpWebWindow__deps: ['$addOnExit', '$callUserCallback', '$withStackSave',
                         '$stringToUTF8OnStack',
                         'eacp_web_window_pointer', 'eacp_web_window_wheel',
                         'eacp_web_window_key', 'eacp_web_window_focus',
                         'eacp_web_window_resized', 'eacp_web_window_full_screen',
                         'eacp_web_window_scale_changed', 'eacp_web_frame'],
  $EacpWebWindow__postset: 'addOnExit(EacpWebWindow.releaseAll);',
  $EacpWebWindow: {
    // id -> {canvas, owned, display, buttons, release: [() => void]}
    windows: new Map(),

    // id -> {raf} or {timeout}
    frames: new Map(),

    scaleWatch: null,

    // Pointer event kinds, in WebPointerEvent's order.
    ENTERED: 0, LEFT: 1, MOVED: 2, PRESSED: 3, RELEASED: 4,

    // DOM button index -> its bit in MouseEvent.buttons.
    buttonBits: [1, 4, 2, 8, 16],

    hasDom() {
      return typeof document !== 'undefined' && typeof HTMLCanvasElement !== 'undefined';
    },

    claimed(canvas) {
      for (const entry of EacpWebWindow.windows.values())
        if (entry.canvas === canvas) return true;
      return false;
    },

    // Runs an export as a callback from the browser, and hands its result back.
    call(body) {
      let result = 0;
      callUserCallback(() => { result = body(); });
      return result;
    },

    modifiers(event) {
      return (event.shiftKey ? 1 : 0) | (event.ctrlKey ? 2 : 0) |
             (event.altKey ? 4 : 0) | (event.metaKey ? 8 : 0);
    },

    position(canvas, event) {
      const rect = canvas.getBoundingClientRect();
      return [event.clientX - rect.left, event.clientY - rect.top];
    },

    pointer(id, entry, event, kind, button) {
      const [x, y] = EacpWebWindow.position(entry.canvas, event);
      const locked = document.pointerLockElement === entry.canvas ? 1 : 0;
      EacpWebWindow.call(() => _eacp_web_window_pointer(
          id, kind, x, y, button, EacpWebWindow.modifiers(event), event.timeStamp,
          event.movementX || 0, event.movementY || 0, locked,
          event.pointerType === 'touch' ? 1 : 0));
    },

    // A pointer event carries every button held; only the first press and
    // the last release get pointerdown and pointerup, so each change of the
    // set is a press or a release of its own.
    syncButtons(id, entry, event, held) {
      EacpWebWindow.buttonBits.forEach((bit, button) => {
        const now = (held & bit) !== 0;
        const before = (entry.buttons & bit) !== 0;
        if (now === before) return;
        entry.buttons ^= bit;
        EacpWebWindow.pointer(id, entry, event,
                              now ? EacpWebWindow.PRESSED : EacpWebWindow.RELEASED,
                              button);
      });
    },

    key(id, event, down) {
      const kept = EacpWebWindow.call(() => withStackSave(() => _eacp_web_window_key(
          id, down, stringToUTF8OnStack(event.code || ''),
          stringToUTF8OnStack(event.key || ''), EacpWebWindow.modifiers(event),
          event.repeat ? 1 : 0, event.timeStamp)));
      if (kept) event.preventDefault();
    },

    listen(id, entry) {
      const canvas = entry.canvas;
      const on = (target, type, handler, options) => {
        target.addEventListener(type, handler, options);
        entry.release.push(() => target.removeEventListener(type, handler, options));
      };
      const bit = event => EacpWebWindow.buttonBits[event.button] || 0;

      on(canvas, 'pointerenter', event =>
          EacpWebWindow.pointer(id, entry, event, EacpWebWindow.ENTERED, 0));
      on(canvas, 'pointerleave', event =>
          EacpWebWindow.pointer(id, entry, event, EacpWebWindow.LEFT, 0));
      on(canvas, 'pointermove', event => {
        EacpWebWindow.syncButtons(id, entry, event, event.buttons);
        EacpWebWindow.pointer(id, entry, event, EacpWebWindow.MOVED, 0);
      });
      on(canvas, 'pointerdown', event => {
        canvas.focus({preventScroll: true});
        try { canvas.setPointerCapture(event.pointerId); } catch {}
        EacpWebWindow.syncButtons(id, entry, event, event.buttons | bit(event));
      });
      const released = event =>
          EacpWebWindow.syncButtons(id, entry, event, event.buttons & ~bit(event));
      on(canvas, 'pointerup', released);
      on(canvas, 'pointercancel', event =>
          EacpWebWindow.syncButtons(id, entry, event, 0));
      on(canvas, 'contextmenu', event => event.preventDefault());
      on(canvas, 'wheel', event => {
        event.preventDefault();
        const [x, y] = EacpWebWindow.position(canvas, event);
        EacpWebWindow.call(() => _eacp_web_window_wheel(
            id, x, y, event.deltaX, event.deltaY, event.deltaMode,
            EacpWebWindow.modifiers(event), event.timeStamp));
      }, {passive: false});
      on(canvas, 'keydown', event => EacpWebWindow.key(id, event, 1));
      on(canvas, 'keyup', event => EacpWebWindow.key(id, event, 0));
      on(canvas, 'focus', () =>
          EacpWebWindow.call(() => _eacp_web_window_focus(id, 1)));
      on(canvas, 'blur', () => {
        entry.buttons = 0;
        EacpWebWindow.call(() => _eacp_web_window_focus(id, 0));
      });
      on(document, 'fullscreenchange', () =>
          EacpWebWindow.call(() => _eacp_web_window_full_screen(
              id, document.fullscreenElement === canvas ? 1 : 0)));

      if (typeof ResizeObserver === 'function') {
        const observer = new ResizeObserver(entries => {
          const rect = entries[entries.length - 1].contentRect;
          if (rect.width > 0 && rect.height > 0)
            EacpWebWindow.call(() => _eacp_web_window_resized(id, rect.width, rect.height));
        });
        observer.observe(canvas);
        entry.release.push(() => observer.disconnect());
      }
    },

    // A zoom or a move to another display changes devicePixelRatio, which a
    // media query on the current ratio reports once; it is then armed again
    // for the new one.
    watchScale() {
      if (EacpWebWindow.scaleWatch || typeof matchMedia !== 'function') return;
      const query = matchMedia(`(resolution: ${devicePixelRatio}dppx)`);
      const changed = () => {
        EacpWebWindow.unwatchScale();
        EacpWebWindow.watchScale();
        callUserCallback(() => _eacp_web_window_scale_changed(devicePixelRatio));
      };
      query.addEventListener('change', changed);
      EacpWebWindow.scaleWatch = () => query.removeEventListener('change', changed);
    },

    unwatchScale() {
      if (EacpWebWindow.scaleWatch) EacpWebWindow.scaleWatch();
      EacpWebWindow.scaleWatch = null;
    },

    release(entry) {
      for (const undo of entry.release.splice(0)) undo();
    },

    releaseAll() {
      for (const entry of EacpWebWindow.windows.values()) EacpWebWindow.release(entry);
      EacpWebWindow.windows.clear();
      for (const id of [...EacpWebWindow.frames.keys()]) EacpWebWindow.cancelFrame(id);
      EacpWebWindow.unwatchScale();
    },

    cancelFrame(id) {
      const handle = EacpWebWindow.frames.get(id);
      if (!handle) return;
      EacpWebWindow.frames.delete(id);
      if (handle.raf !== undefined) cancelAnimationFrame(handle.raf);
      else clearTimeout(handle.timeout);
    },
  },

  eacp_web_window_has_dom__deps: ['$EacpWebWindow'],
  eacp_web_window_has_dom: () => EacpWebWindow.hasDom() ? 1 : 0,

  // The page's own canvas (Module.canvas) goes to the first window that asks;
  // every other window makes one, hidden until shown and sized by the window.
  // A page canvas with a layout keeps it, and reports it before this returns.
  eacp_web_window_create__deps: ['$EacpWebWindow', '$stringToNewUTF8'],
  eacp_web_window_create: (id, width, height) => {
    if (!EacpWebWindow.hasDom()) return 0;

    let canvas = Module['canvas'];
    let owned = false;

    if (!(canvas instanceof HTMLCanvasElement) || EacpWebWindow.claimed(canvas)) {
      canvas = document.createElement('canvas');
      owned = true;
      canvas.style.display = 'none';
      canvas.style.outline = 'none';
      canvas.style.width = width + 'px';
      canvas.style.height = height + 'px';
      (document.body || document.documentElement).appendChild(canvas);
    }

    if (!canvas.id) canvas.id = 'eacp-canvas-' + id;
    if (!canvas.hasAttribute('tabindex')) canvas.tabIndex = 0;
    canvas.style.touchAction = 'none';

    const entry = {canvas, owned, display: canvas.style.display, buttons: 0, release: []};
    EacpWebWindow.windows.set(id, entry);
    EacpWebWindow.listen(id, entry);
    EacpWebWindow.watchScale();

    if (!owned && canvas.clientWidth > 0 && canvas.clientHeight > 0)
      _eacp_web_window_resized(id, canvas.clientWidth, canvas.clientHeight);

    return stringToNewUTF8('#' + CSS.escape(canvas.id));
  },

  eacp_web_window_destroy__deps: ['$EacpWebWindow'],
  eacp_web_window_destroy: (id) => {
    const entry = EacpWebWindow.windows.get(id);
    if (!entry) return;
    EacpWebWindow.windows.delete(id);
    EacpWebWindow.release(entry);
    if (entry.owned) entry.canvas.remove();
    else entry.canvas.style.display = entry.display;
  },

  eacp_web_window_set_css_size__deps: ['$EacpWebWindow'],
  eacp_web_window_set_css_size: (id, width, height) => {
    const canvas = EacpWebWindow.windows.get(id)?.canvas;
    if (!canvas) return;
    canvas.style.width = width + 'px';
    canvas.style.height = height + 'px';
  },

  eacp_web_window_set_backing_size__deps: ['$EacpWebWindow'],
  eacp_web_window_set_backing_size: (id, pixelWidth, pixelHeight) => {
    const canvas = EacpWebWindow.windows.get(id)?.canvas;
    if (!canvas) return;
    if (canvas.width !== pixelWidth) canvas.width = pixelWidth;
    if (canvas.height !== pixelHeight) canvas.height = pixelHeight;
  },

  eacp_web_window_set_visible__deps: ['$EacpWebWindow'],
  eacp_web_window_set_visible: (id, visible) => {
    const entry = EacpWebWindow.windows.get(id);
    if (!entry) return;
    const shown = entry.owned ? 'block' : entry.display;
    entry.canvas.style.display = visible ? shown : 'none';
  },

  eacp_web_window_set_title__deps: ['$EacpWebWindow', '$UTF8ToString'],
  eacp_web_window_set_title: (id, title) => {
    const entry = EacpWebWindow.windows.get(id);
    if (!entry) return;
    const text = UTF8ToString(title);
    entry.canvas.setAttribute('aria-label', text);
    document.title = text;
  },

  eacp_web_window_set_cursor__deps: ['$EacpWebWindow', '$UTF8ToString'],
  eacp_web_window_set_cursor: (id, cursor) => {
    const canvas = EacpWebWindow.windows.get(id)?.canvas;
    if (canvas) canvas.style.cursor = UTF8ToString(cursor);
  },

  eacp_web_window_set_full_screen__deps: ['$EacpWebWindow'],
  eacp_web_window_set_full_screen: (id, fullScreen) => {
    const canvas = EacpWebWindow.windows.get(id)?.canvas;
    if (!canvas) return;
    if (fullScreen) canvas.requestFullscreen?.().catch(() => {});
    else if (document.fullscreenElement === canvas) document.exitFullscreen().catch(() => {});
  },

  eacp_web_window_set_pointer_lock__deps: ['$EacpWebWindow'],
  eacp_web_window_set_pointer_lock: (id, locked) => {
    const canvas = EacpWebWindow.windows.get(id)?.canvas;
    if (!canvas) return;
    if (!locked) {
      if (document.pointerLockElement === canvas) document.exitPointerLock();
      return;
    }
    try {
      canvas.requestPointerLock()?.catch?.(() => {});
    } catch {}
  },

  eacp_web_device_pixel_ratio: () =>
      typeof devicePixelRatio === 'number' && devicePixelRatio > 0 ? devicePixelRatio : 1,

  eacp_web_screen_metrics__deps: ['$EacpWebWindow'],
  eacp_web_screen_metrics: (metrics) => {
    const values = EacpWebWindow.hasDom() && typeof screen !== 'undefined'
        ? [screen.width, screen.height, innerWidth, innerHeight] : [0, 0, 0, 0];
    HEAPF64.set(values, metrics >> 3);
  },

  eacp_web_prefers_dark_scheme: () =>
      typeof matchMedia === 'function' && matchMedia('(prefers-color-scheme: dark)').matches
          ? 1 : 0,

  eacp_web_frame_request__deps: ['$EacpWebWindow'],
  eacp_web_frame_request: (id) => {
    if (EacpWebWindow.frames.has(id)) return;
    const fire = () => {
      if (!EacpWebWindow.frames.delete(id)) return;
      callUserCallback(() => _eacp_web_frame(id));
    };
    EacpWebWindow.frames.set(id, typeof requestAnimationFrame === 'function'
        ? {raf: requestAnimationFrame(fire)} : {timeout: setTimeout(fire, 1000 / 60)});
  },

  eacp_web_frame_cancel__deps: ['$EacpWebWindow'],
  eacp_web_frame_cancel: (id) => EacpWebWindow.cancelFrame(id),
});
