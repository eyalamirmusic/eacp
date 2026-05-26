// eacp agentic-QA helper. Injected by eacp::Graphics::DebugControl in debug
// builds. Defines window.eacpQA: a small DOM driver an external agent calls
// through the /evaluate-javascript endpoint. No app code depends on this; it
// is generic DOM and works on any eacp WebView app.
(function () {
  if (window.eacpQA) {
    return;
  }

  const INTERACTIVE = [
    'button',
    'a[href]',
    'input:not([type=hidden])',
    'textarea',
    'select',
    '[contenteditable]:not([contenteditable=false])',
    '[role=button]',
    '[role=link]',
    '[role=textbox]',
    '[role=checkbox]',
    '[role=radio]',
    '[role=switch]',
    '[role=tab]',
    '[role=menuitem]',
    '[role=combobox]',
    '[role=option]',
    '[tabindex]:not([tabindex="-1"])',
  ].join(',');

  // Per-snapshot handle -> Element. Rebuilt on every ls(); handles are not
  // stable across snapshots (re-ls after navigation), same model as Playwright
  // refs / the accessibility tree.
  let nodes = [];

  function roleOf(el) {
    const explicit = el.getAttribute('role');
    if (explicit) {
      return explicit;
    }
    const tag = el.tagName.toLowerCase();
    if (tag === 'a' && el.hasAttribute('href')) {
      return 'link';
    }
    if (tag === 'button') {
      return 'button';
    }
    if (tag === 'select') {
      return 'combobox';
    }
    if (tag === 'textarea') {
      return 'textbox';
    }
    if (tag === 'input') {
      const type = (el.getAttribute('type') || 'text').toLowerCase();
      const map = {
        checkbox: 'checkbox',
        radio: 'radio',
        button: 'button',
        submit: 'button',
        reset: 'button',
        range: 'slider',
      };
      return map[type] || 'textbox';
    }
    return tag;
  }

  function accessibleName(el) {
    const aria = el.getAttribute('aria-label');
    if (aria) {
      return aria.trim();
    }
    const labelledby = el.getAttribute('aria-labelledby');
    if (labelledby) {
      const parts = labelledby
        .split(/\s+/)
        .map((id) => document.getElementById(id))
        .filter(Boolean)
        .map((n) => n.textContent.trim());
      if (parts.length) {
        return parts.join(' ');
      }
    }
    if (el.tagName === 'INPUT' && el.labels && el.labels.length) {
      return el.labels[0].textContent.trim();
    }
    const text = (el.textContent || '').trim();
    if (text) {
      return text.replace(/\s+/g, ' ').slice(0, 120);
    }
    return (
      el.getAttribute('placeholder') ||
      el.getAttribute('title') ||
      el.getAttribute('alt') ||
      el.getAttribute('name') ||
      ''
    ).trim();
  }

  function isVisible(el, rect) {
    if (rect.width <= 0 || rect.height <= 0) {
      return false;
    }
    const style = getComputedStyle(el);
    return (
      style.visibility !== 'hidden' &&
      style.display !== 'none' &&
      style.opacity !== '0'
    );
  }

  function inViewport(rect) {
    return (
      rect.bottom > 0 &&
      rect.right > 0 &&
      rect.top < (window.innerHeight || 0) &&
      rect.left < (window.innerWidth || 0)
    );
  }

  function describe(el, handle) {
    const rect = el.getBoundingClientRect();
    const node = {
      handle,
      role: roleOf(el),
      name: accessibleName(el),
      tag: el.tagName.toLowerCase(),
      enabled: !el.disabled && el.getAttribute('aria-disabled') !== 'true',
      visible: isVisible(el, rect),
      inViewport: inViewport(rect),
      rect: {
        x: Math.round(rect.x),
        y: Math.round(rect.y),
        w: Math.round(rect.width),
        h: Math.round(rect.height),
      },
    };
    const type = el.getAttribute('type');
    if (type) {
      node.type = type;
    }
    if ('value' in el && el.value !== undefined) {
      node.value = String(el.value);
    }
    const testid = el.getAttribute('data-testid');
    if (testid) {
      node.testid = testid;
    }
    return node;
  }

  function resolve(handle) {
    const el = nodes[handle];
    if (!el || !el.isConnected) {
      throw new Error(
        'stale handle ' + handle + ' — re-run ls() after navigation',
      );
    }
    return el;
  }

  function nativeSetValue(el, text) {
    const proto =
      el instanceof HTMLTextAreaElement
        ? HTMLTextAreaElement.prototype
        : HTMLInputElement.prototype;
    const setter = Object.getOwnPropertyDescriptor(proto, 'value').set;
    setter.call(el, text);
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  }

  function dispatchPointerSequence(el) {
    const rect = el.getBoundingClientRect();
    const clientX = rect.x + rect.width / 2;
    const clientY = rect.y + rect.height / 2;
    const opts = { bubbles: true, cancelable: true, clientX, clientY };
    el.dispatchEvent(new PointerEvent('pointerover', opts));
    el.dispatchEvent(new PointerEvent('pointerdown', opts));
    el.dispatchEvent(new MouseEvent('mousedown', opts));
    el.dispatchEvent(new PointerEvent('pointerup', opts));
    el.dispatchEvent(new MouseEvent('mouseup', opts));
    el.dispatchEvent(new MouseEvent('click', opts));
  }

  window.eacpQA = {
    ls(opts) {
      const includeHidden = opts && opts.all;
      nodes = Array.from(document.querySelectorAll(INTERACTIVE));
      return nodes
        .map((el, handle) => describe(el, handle))
        .filter((n) => includeHidden || n.visible);
    },

    // Locate an element by role + accessible-name substring (case-insensitive).
    // Rebuilds the snapshot first, so the returned handle is valid for the next
    // click/type. role null = any role. Returns null when nothing matches.
    find(role, name) {
      this.ls({ all: true });
      const want = String(name).toLowerCase();
      for (let handle = 0; handle < nodes.length; handle++) {
        const d = describe(nodes[handle], handle);
        if ((!role || d.role === role) && d.name.toLowerCase().includes(want)) {
          return d;
        }
      }
      return null;
    },

    click(handle, opts) {
      const el = resolve(handle);
      el.scrollIntoView({ block: 'center', inline: 'center' });
      if (opts && opts.full) {
        dispatchPointerSequence(el);
      } else {
        el.click();
      }
      return { ok: true };
    },

    type(handle, text, opts) {
      const el = resolve(handle);
      el.focus();
      if (el instanceof HTMLInputElement || el instanceof HTMLTextAreaElement) {
        nativeSetValue(el, String(text));
      } else if (el.isContentEditable) {
        el.textContent = String(text);
        el.dispatchEvent(new Event('input', { bubbles: true }));
      } else {
        throw new Error('handle ' + handle + ' is not typeable');
      }
      if (opts && opts.submit) {
        this.press('Enter');
      }
      return { ok: true, value: 'value' in el ? el.value : undefined };
    },

    fill(handle, text) {
      return this.type(handle, text);
    },

    press(key) {
      const el = document.activeElement || document.body;
      const opts = { key, bubbles: true, cancelable: true };
      el.dispatchEvent(new KeyboardEvent('keydown', opts));
      el.dispatchEvent(new KeyboardEvent('keyup', opts));
      return { ok: true };
    },

    // Apps expose their router (TanStack-shaped) as window.__eacpRouter to opt
    // into typed routes()/nav(). Without it we degrade to hash navigation and
    // DOM-derived routes.
    routes() {
      const router = window.__eacpRouter;
      const collect = (list) =>
        list
          .map((r) => ({
            id: r.id,
            path: r.fullPath || r.id,
            hasParams: /[$*]/.test(r.fullPath || r.id || ''),
          }))
          .filter((r) => r.path && r.id !== '__root__');
      if (router && Array.isArray(router.flatRoutes)) {
        return collect(router.flatRoutes);
      }
      if (router && router.routesById) {
        return collect(Object.values(router.routesById));
      }
      const hrefs = Array.from(document.querySelectorAll('a[href]'))
        .map((a) => a.getAttribute('href'))
        .filter((h) => h && (h.startsWith('/') || h.startsWith('#')));
      return Array.from(new Set(hrefs)).map((path) => ({ path }));
    },

    nav(to) {
      const router = window.__eacpRouter;
      if (router && typeof router.navigate === 'function') {
        router.navigate({ to });
        return { ok: true, to };
      }
      location.hash = to;
      return { ok: true, hash: location.hash };
    },

    state() {
      const router = window.__eacpRouter;
      return {
        url: location.href,
        hash: location.hash,
        title: document.title,
        pathname: router && router.state ? router.state.location.pathname : undefined,
      };
    },

    // Scroll the window (default) or a container element (opts.handle).
    // amount: 'up' | 'down' | 'top' | 'bottom' | <signed px number>.
    scroll(amount, opts) {
      const el =
        opts && opts.handle !== undefined
          ? resolve(opts.handle)
          : document.scrollingElement || document.documentElement;
      const page = el.clientHeight || window.innerHeight || 0;
      let top;
      if (amount === 'top') {
        top = 0;
      } else if (amount === 'bottom') {
        top = el.scrollHeight;
      } else if (amount === 'up') {
        top = el.scrollTop - page * 0.9;
      } else if (amount === 'down') {
        top = el.scrollTop + page * 0.9;
      } else {
        top = el.scrollTop + Number(amount);
      }
      el.scrollTo({ top, behavior: 'instant' });
      return {
        ok: true,
        scrollTop: Math.round(el.scrollTop),
        scrollHeight: Math.round(el.scrollHeight),
      };
    },

    scrollTo(handle) {
      const el = resolve(handle);
      el.scrollIntoView({ block: 'center', inline: 'center' });
      return { ok: true };
    },
  };
})();
