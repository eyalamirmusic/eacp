// The JS half of Http-Web.cpp: one fetch() per transfer, keyed by the id the
// C++ side chose. Each transfer reports, in order and never inside the call
// that started it, _eacp_web_http_head(id, status, headers, type), then
// _eacp_web_http_chunk(id, ptr, length) per piece of the body, then
// _eacp_web_http_done(id, error) - error null for a body read to its end.
// The C++ side ends a transfer early through eacp_web_http_abort, so after
// every call into it the transfer is looked up again. When the runtime exits
// every transfer is aborted, since the program it would report into is gone.

addToLibrary({
  $EacpWebHttp__deps: ['$withStackSave', '$stringToUTF8OnStack', '$FS', '$addOnExit',
                       'malloc', 'free',
                       'eacp_web_http_head', 'eacp_web_http_chunk', 'eacp_web_http_done'],
  $EacpWebHttp__postset: 'addOnExit(EacpWebHttp.abortAll);',
  $EacpWebHttp: {
    controllers: new Map(),

    abortAll() {
      const controllers = [...EacpWebHttp.controllers.values()];
      EacpWebHttp.controllers.clear();
      for (const controller of controllers) controller.abort();
    },

    describe(error) {
      if (!error) return 'The request failed';
      let text = error.message || String(error);
      const cause = error.cause;
      if (cause) text += ` (${cause.code || cause.message || cause})`;
      return text;
    },

    headersFrom(block) {
      const headers = new Headers();
      for (const line of block.split('\n')) {
        if (!line) continue;
        const colon = line.indexOf(':');
        headers.append(line.slice(0, colon), line.slice(colon + 1));
      }
      return headers;
    },

    // Never a view of the heap: fetch refuses one in browsers, and node
    // sends its String() instead of its bytes.
    bodyFrom(bodyPtr, bodyLength, filePath) {
      if (filePath) {
        try {
          return FS.readFile(filePath);
        } catch {
          throw new Error('Failed to open body file: ' + filePath);
        }
      }
      if (bodyLength > 0) return HEAPU8.slice(bodyPtr, bodyPtr + bodyLength);
      return undefined;
    },

    live(id) {
      return EacpWebHttp.controllers.has(id);
    },

    finish(id, error) {
      if (!EacpWebHttp.controllers.delete(id)) return;
      if (error)
        withStackSave(() => _eacp_web_http_done(id, stringToUTF8OnStack(error)));
      else
        _eacp_web_http_done(id, 0);
    },

    pushChunk(id, bytes) {
      const ptr = _malloc(bytes.length);
      HEAPU8.set(bytes, ptr);
      _eacp_web_http_chunk(id, ptr, bytes.length);
      _free(ptr);
    },

    async run(id, prepared) {
      if (prepared.error) throw prepared.error;

      const answer = await fetch(prepared.url, prepared.init);
      if (!EacpWebHttp.live(id)) return;

      let block = '';
      answer.headers.forEach((value, name) => { block += name + ': ' + value + '\n'; });
      withStackSave(() => _eacp_web_http_head(id, answer.status, stringToUTF8OnStack(block),
                                              stringToUTF8OnStack(answer.type)));

      if (answer.body) {
        const reader = answer.body.getReader();
        while (EacpWebHttp.live(id)) {
          const {done, value} = await reader.read();
          if (done || !EacpWebHttp.live(id)) break;
          if (value.length > 0) EacpWebHttp.pushChunk(id, value);
        }
        if (!EacpWebHttp.live(id)) reader.cancel().catch(() => {});
      }

      EacpWebHttp.finish(id, null);
    },
  },

  eacp_web_http_start__deps: ['$EacpWebHttp', '$UTF8ToString'],
  eacp_web_http_start: (id, urlPtr, methodPtr, headersPtr, bodyPtr, bodyLength,
                        filePathPtr, redirectPtr, cachePtr) => {
    const controller = new AbortController();
    EacpWebHttp.controllers.set(id, controller);

    // Everything the heap holds is read now: the pointers are the caller's.
    let prepared;
    try {
      const init = {
        method: UTF8ToString(methodPtr),
        headers: EacpWebHttp.headersFrom(UTF8ToString(headersPtr)),
        redirect: UTF8ToString(redirectPtr),
        cache: UTF8ToString(cachePtr),
        signal: controller.signal,
      };
      const body = EacpWebHttp.bodyFrom(bodyPtr, bodyLength, UTF8ToString(filePathPtr));
      if (body !== undefined) init.body = body;
      prepared = {url: UTF8ToString(urlPtr), init};
    } catch (error) {
      prepared = {error};
    }

    EacpWebHttp.run(id, prepared)
        .catch(error => EacpWebHttp.finish(id, EacpWebHttp.describe(error)));
  },

  eacp_web_http_abort__deps: ['$EacpWebHttp'],
  eacp_web_http_abort: (id) => {
    const controller = EacpWebHttp.controllers.get(id);
    if (!controller) return;
    EacpWebHttp.controllers.delete(id);
    controller.abort();
  },
});
