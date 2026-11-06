# Doc comments in eacp headers

Headers carry Doxygen comments so `Scripts/generate-llm-docs.py` can build the
agent reference under `/llm-docs/`. Only public headers; no comments in `.cpp`.

## Module preamble

The module's umbrella header (`Lib/eacp/<Module>/<Module>.h`) opens with a
`@file` block: what the module is for, its main entry points, which platforms
have it (the `EACP_HAS_*` gate), and the two or three rules an agent gets wrong
(threading, ownership, what to use instead). 5–15 lines.

```cpp
/** @file
    Threads, timers and the event loop. Everything here delivers on the main
    thread unless it says otherwise. ...
*/
```

## Per header

A `/** @file <one sentence> */` saying what the header is for. This is the line
the module index shows.

## Per public symbol

One `///` line on each public class, struct, enum, free function and non-obvious
member. Say what the name can't:

- which thread a callback runs on; whether a call blocks
- who owns what; lifetime ("destroying it stops the timer")
- what happens on failure (returns empty, false, throws never)
- units and ranges (points vs pixels, 0..1, ms)
- what to use instead, when there's a better entry point

Never restate the name ("/// The timer class."). A self-explanatory getter gets
nothing. Plain short sentences, no acronyms left unexpanded.
