// The JS half of WebSocket-Web.cpp: one browser WebSocket per id. Each socket
// reports, never inside the call that opened it, _eacp_web_socket_opened(id,
// protocol), _eacp_web_socket_message(id, ptr, length, binary) per message,
// and then exactly one of _eacp_web_socket_closed(id, code, reason) or
// _eacp_web_socket_failed(id, error). Released, a socket reports nothing more,
// and every socket is released when the runtime exits.

addToLibrary({
  $EacpWebSocket__deps: ['$withStackSave', '$stringToUTF8OnStack', '$addOnExit',
                         'malloc', 'free',
                         'eacp_web_socket_opened', 'eacp_web_socket_message',
                         'eacp_web_socket_closed', 'eacp_web_socket_failed'],
  $EacpWebSocket__postset: 'addOnExit(EacpWebSocket.releaseAll);',
  $EacpWebSocket: {
    // id -> {socket, timer}: the socket is null when it could not be made,
    // the timer the connect timeout, cleared once the socket settles.
    entries: new Map(),

    stopTimer(entry) {
      if (entry.timer !== null) clearTimeout(entry.timer);
      entry.timer = null;
    },

    // The one terminal report, after which the id is gone.
    end(id, report) {
      const entry = EacpWebSocket.entries.get(id);
      if (!entry) return;
      EacpWebSocket.stopTimer(entry);
      EacpWebSocket.entries.delete(id);
      report();
    },

    failed(id, error) {
      EacpWebSocket.end(id, () => withStackSave(
          () => _eacp_web_socket_failed(id, stringToUTF8OnStack(error))));
    },

    deliver(id, bytes, binary) {
      const ptr = _malloc(Math.max(1, bytes.length));
      HEAPU8.set(bytes, ptr);
      _eacp_web_socket_message(id, ptr, bytes.length, binary);
      _free(ptr);
    },

    attach(id, entry) {
      const socket = entry.socket;
      socket.binaryType = 'arraybuffer';

      socket.onopen = () => {
        EacpWebSocket.stopTimer(entry);
        if (!EacpWebSocket.entries.has(id)) return;
        withStackSave(() => _eacp_web_socket_opened(id, stringToUTF8OnStack(socket.protocol)));
      };

      socket.onmessage = event => {
        if (!EacpWebSocket.entries.has(id)) return;
        if (typeof event.data === 'string')
          EacpWebSocket.deliver(id, new TextEncoder().encode(event.data), 0);
        else
          EacpWebSocket.deliver(id, new Uint8Array(event.data), 1);
      };

      // A browser says nothing about why: an error is followed by a close
      // with 1006, and the error is the report.
      socket.onerror = () => EacpWebSocket.failed(id, 'The WebSocket connection failed');

      socket.onclose = event => EacpWebSocket.end(id, () => withStackSave(
          () => _eacp_web_socket_closed(id, event.code, stringToUTF8OnStack(event.reason))));
    },

    detach(socket) {
      socket.onopen = socket.onmessage = socket.onerror = socket.onclose = null;
      if (socket.readyState === WebSocket.CONNECTING || socket.readyState === WebSocket.OPEN)
        socket.close(1000);
    },

    release(entry) {
      EacpWebSocket.stopTimer(entry);
      if (entry.socket) EacpWebSocket.detach(entry.socket);
    },

    releaseAll() {
      const entries = [...EacpWebSocket.entries.values()];
      EacpWebSocket.entries.clear();
      for (const entry of entries) EacpWebSocket.release(entry);
    },
  },

  eacp_web_socket_open__deps: ['$EacpWebSocket', '$UTF8ToString'],
  eacp_web_socket_open: (id, urlPtr, protocolsPtr, connectTimeoutMs) => {
    const url = UTF8ToString(urlPtr);
    const protocols = UTF8ToString(protocolsPtr).split('\n').filter(Boolean);
    const entry = {socket: null, timer: null};
    EacpWebSocket.entries.set(id, entry);

    try {
      entry.socket = new WebSocket(url, protocols);
    } catch (error) {
      const message = error.message || String(error);
      queueMicrotask(() => EacpWebSocket.failed(id, message));
      return;
    }

    EacpWebSocket.attach(id, entry);

    if (connectTimeoutMs > 0)
      entry.timer = setTimeout(() => {
        entry.timer = null;
        EacpWebSocket.detach(entry.socket);
        EacpWebSocket.failed(id, 'connection timed out');
      }, connectTimeoutMs);
  },

  eacp_web_socket_send__deps: ['$EacpWebSocket'],
  eacp_web_socket_send: (id, data, length, binary) => {
    const socket = EacpWebSocket.entries.get(id)?.socket;
    if (!socket || socket.readyState !== WebSocket.OPEN) return;
    const bytes = HEAPU8.slice(data, data + length);
    socket.send(binary ? bytes : new TextDecoder().decode(bytes));
  },

  eacp_web_socket_close__deps: ['$EacpWebSocket', '$UTF8ToString'],
  eacp_web_socket_close: (id, code, reasonPtr) => {
    const socket = EacpWebSocket.entries.get(id)?.socket;
    if (!socket) return;
    try {
      socket.close(code, UTF8ToString(reasonPtr));
    } catch {
      socket.close(1000);
    }
  },

  eacp_web_socket_release__deps: ['$EacpWebSocket'],
  eacp_web_socket_release: (id) => {
    const entry = EacpWebSocket.entries.get(id);
    if (!entry) return;
    EacpWebSocket.entries.delete(id);
    EacpWebSocket.release(entry);
  },
});
