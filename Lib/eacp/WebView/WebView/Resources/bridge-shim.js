(function() {
    if (window.eacp) return;

    var pending = new Map();
    var counter = 0;
    var listeners = new Map();
    var exposed = new Map();

    function post(envelope) {
        var raw = JSON.stringify(envelope);
        if (window.webkit && window.webkit.messageHandlers
            && window.webkit.messageHandlers.__eacpBridge) {
            window.webkit.messageHandlers.__eacpBridge.postMessage(raw);
        } else if (window.__eacpBridge && window.__eacpBridge.postMessage) {
            window.__eacpBridge.postMessage(raw);
        } else {
            console.error('eacp bridge: no transport available');
        }
    }

    window.eacp = {
        invoke: function(command, payload) {
            return new Promise(function(resolve, reject) {
                var id = ++counter;
                pending.set(id, { resolve: resolve, reject: reject });
                post({
                    id: id,
                    command: command,
                    payload: payload === undefined ? null : payload
                });
            });
        },
        on: function(event, handler) {
            var arr = listeners.get(event) || [];
            arr.push(handler);
            listeners.set(event, arr);
            return function() {
                var current = listeners.get(event) || [];
                listeners.set(event,
                    current.filter(function(h) { return h !== handler; }));
            };
        },
        // Registers a function the native side can call via
        // WebViewBridge::call(name, ...). The function may be sync or
        // async (return a value or a Promise); either way its resolved
        // result is sent back to C++.
        expose: function(name, fn) { exposed.set(name, fn); return fn; }
    };

    window.__eacp = {
        deliver: function(id, result, error) {
            var entry = pending.get(id);
            if (!entry) return;
            pending.delete(id);
            if (error) entry.reject(new Error(error));
            else entry.resolve(result);
        },
        dispatch: function(event, payload) {
            var arr = listeners.get(event) || [];
            for (var i = 0; i < arr.length; i++) {
                try { arr[i](payload); }
                catch (e) { console.error('eacp event handler error', e); }
            }
        },
        // Native -> page call. Looks up an exposed function, awaits its
        // result (Promise or plain value), and posts a reply envelope
        // keyed by `id` back to C++ — mirroring how invoke() replies are
        // delivered the other way.
        callFunction: function(id, name, payload) {
            Promise.resolve().then(function() {
                var fn = exposed.get(name);
                if (typeof fn !== 'function')
                    throw new Error("eacp: no exposed function '" + name + "'");
                return fn(payload);
            }).then(function(result) {
                post({ reply: id, result: result === undefined ? null : result });
            }, function(err) {
                post({ reply: id,
                       error: String(err && err.message ? err.message : err) });
            });
        }
    };
})();
