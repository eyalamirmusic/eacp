// Installed at document-start by Remote::DebugServer. Captures console
// output, uncaught errors, and unhandled promise rejections into a
// bounded in-page buffer so the console_logs tool can read them
// without devtools attached.
(function() {
  if (window.__eacpConsole) return;

  var entries = [];
  var MAX_ENTRIES = 1000;
  var dropped = 0;

  function push(level, args) {
    var parts = [];
    for (var i = 0; i < args.length; i++) {
      var arg = args[i];
      if (typeof arg === 'string') { parts.push(arg); continue; }
      try { parts.push(JSON.stringify(arg)); }
      catch (e) { parts.push(String(arg)); }
    }
    entries.push({ level: level, message: parts.join(' '), time: Date.now() });
    if (entries.length > MAX_ENTRIES) { entries.shift(); dropped++; }
  }

  ['log', 'info', 'warn', 'error', 'debug'].forEach(function(level) {
    var original = console[level];
    console[level] = function() {
      push(level, arguments);
      if (original) original.apply(console, arguments);
    };
  });

  window.addEventListener('error', function(event) {
    push('uncaught', [
      event.message + ' (' + event.filename + ':' + event.lineno + ')']);
  });

  window.addEventListener('unhandledrejection', function(event) {
    var reason = event.reason;
    var message = reason && reason.stack ? reason.stack : String(reason);
    push('unhandledrejection', [message]);
  });

  window.__eacpConsole = {
    drain: function(clear) {
      var out = { entries: entries.slice(), dropped: dropped };
      if (clear) { entries.length = 0; dropped = 0; }
      return out;
    }
  };
})();
