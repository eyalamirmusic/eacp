# The eacp agent harness

This document is for **coding agents** (and the humans configuring builds
for them) driving eacp WebView apps. It explains how to launch an app so
crashes are legible, connect to its embedded debug server, and drive its
UI — list elements, click, type, screenshot, read console output, call
into C++ — entirely from outside the process over HTTP.

Every eacp WebView app gets this for free in non-release builds. There is
no per-app code to write.

---

## TL;DR

```bash
# 1. Build (Debug — the agent affordances are on by default here)
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=OFF
cmake --build build --target Calculator

# 2. Launch through the agent harness (crashes print a backtrace)
build/agentharness/Calculator &

# 3. Drive it over MCP (default port 9696)
curl -s localhost:9696/mcp -d '{"jsonrpc":"2.0","id":1,"method":"tools/call",
  "params":{"name":"list_elements","arguments":{}}}'
```

Or point an MCP client straight at it:

```bash
claude mcp add --transport http calc http://localhost:9696/mcp
```

---

## The three affordances

A non-release build of an eacp app ships three independent developer
affordances. Each has its own `AUTO`/`ON`/`OFF` CMake switch; `AUTO`
means "on in non-release builds". They do not depend on one another.

| Affordance | What it does | Switch |
|---|---|---|
| **Agent harness launcher** | `build/agentharness/<Target>` runs the app under a batch-mode debugger; a crash prints a full backtrace and exits non-zero | `EACP_AGENT_HARNESS` |
| **AddressSanitizer** | the app executable is ASan-instrumented, so memory bugs abort with a precise report | `EACP_ASAN` |
| **MCP debug server** | the app serves an MCP endpoint so an agent can drive the live UI over HTTP | `EACP_DEBUG_SERVER` |

Release / RelWithDebInfo / MinSizeRel builds carry **none** of this. To
mix explicitly: `-DEACP_ASAN=ON -DEACP_AGENT_HARNESS=OFF` gives ASan with
no launcher, and so on.

---

## 1. Launching: the agent harness

Always launch app binaries through `build/agentharness/<Target>`, never
the bundle binary directly. The launcher:

- passes through arguments, environment, and stdout/stderr unchanged — it
  behaves exactly like running the binary, so the MCP server still comes
  up and the app renders normally;
- runs the process under a batch-mode debugger (lldb, then gdb, then a
  plain exec on POSIX; cdb on Windows), so an unhandled crash prints a
  symbolated all-threads backtrace to the captured output and exits
  non-zero — instead of the process dying silently and you finding only a
  dead port.

```bash
build/agentharness/Calculator                 # foreground
build/agentharness/Calculator > /tmp/app.log 2>&1 &   # background, capture log
EACP_DEBUG_PORT=9744 build/agentharness/Calculator    # env passes through
```

`EACP_NO_DEBUGGER=1` bypasses the debugger (launch the binary directly) —
useful only when the debugger itself is the problem.

There is also a build-and-run convenience target:

```bash
cmake --build build --target agentharness-Calculator
```

### Reading a crash

On a crash, the log ends with the ASan report (if the bug is a memory
error) followed by the debugger backtrace. The most useful lines:

```
==12345==ERROR: AddressSanitizer: heap-buffer-overflow ... WRITE of size 1 ...
    #0 Api::CalculatorApi::press(KeyRequest const&) Types.h:65
SUMMARY: AddressSanitizer: heap-buffer-overflow Types.h:65 in Api::CalculatorApi::press(...)
```

ASan also prints the **allocation site** ("located 96 bytes after a
4-byte region allocated by..."), which usually points straight at the
bug. Grep the log for `ERROR: AddressSanitizer`, `SUMMARY:`, and
`stop reason` to extract the signal from the thread dump.

---

## 2. Connecting to the MCP debug server

The server speaks MCP (Model Context Protocol) over the streamable-HTTP
transport in its stateless plain-JSON form: each JSON-RPC message is one
HTTP POST answered with one `application/json` response. Any MCP client
connects; you can also drive it with raw `curl`.

### Port policy (`EACP_DEBUG_PORT`)

| `EACP_DEBUG_PORT` | Result |
|---|---|
| unset | listen on **9696** (the default) |
| a number | listen on that port (`0` = an ephemeral port) |
| `off` | disable the server for this run |

If the chosen port is taken (e.g. a second instance), the server falls
back to an ephemeral port and logs the actual one:

```
WindowDebugServer: MCP endpoint at http://127.0.0.1:49184/mcp
```

So when running more than one app, read the port from the launch log
rather than assuming 9696.

### Endpoints

- `POST /mcp` — the MCP JSON-RPC endpoint (use this).
- `POST /rpc` — the raw app bridge: `{"command","payload"}` → `{"result"}`.
  A lower-level way to call C++ commands without MCP framing.

### Handshake

MCP clients send `initialize` first; raw `curl` users can skip straight
to `tools/call`. A minimal handshake:

```bash
curl -s localhost:9696/mcp -d '{"jsonrpc":"2.0","id":1,"method":"initialize",
  "params":{"protocolVersion":"2025-03-26","capabilities":{},
            "clientInfo":{"name":"curl","version":"0"}}}'
curl -s localhost:9696/mcp -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
```

The `initialize` result includes an `instructions` string describing the
app's element-id attribute and the recommended tool flow.

---

## 3. Selecting elements: the `@id` shorthand

Elements meant for automation are tagged with a stable attribute —
**`data-testid`** by default. Every selector argument (in both the MCP
tools and the C++ test driver) accepts an `@id` shorthand that expands to
that attribute:

| Selector | Expands to |
|---|---|
| `@todo-add` | `[data-testid="todo-add"]` |
| `@todo-item.done` | `[data-testid="todo-item"].done` |
| `@todo-list li` | `[data-testid="todo-list"] li` |
| `@row:last-child @cell` | `[data-testid="row"]:last-child [data-testid="cell"]` |

Plain CSS still works (`ul.list > li`, `input[type=text]`), but the
supported CSS subset is limited (tag, `.class`, `#id`, `[attr]`,
`[attr=value]`, `*`, descendant combinator). Prefer `@id`.

`list_elements` with no argument dumps every tagged element — the fastest
way to discover the handles a page exposes. The attribute name is
configurable per app (`ElementIds::setAttributeName("data-myapp")`);
`page_info` reports the active one.

---

## 4. The tools

Call via `tools/call` with `{"name": <tool>, "arguments": {...}}`. Most
take a `selector`; interaction tools accept an optional `timeout_ms`.
Errors (missing selector, JS exception) come back as a tool result with
`isError: true`, not a transport failure.

### Capture (window-level — any app)
These attach to the app's **window**, so they work for any eacp app — GPU,
native drawing, SVG, WebView — capturing the *composited* window. macOS
needs Screen Recording permission and a visible window.
- **`screenshot`** — PNG of the composited app window, as image content.
- **`start_recording`** `{name?, fps?}` — begin recording the window to an
  MP4 (`name` is the file stem or an absolute path; default 30 fps).
  Recording runs while you keep issuing tool calls.
- **`stop_recording`** — finish the MP4 and return its path.

### Orientation (WebView)
- **`page_info`** — URL, title, load state, and the element-id attribute.
- **`list_elements`** `{selector?}` — one line per match: tag, `@id`,
  classes, value/checked, text. Defaults to every tagged element.
- **`dom`** `{selector?}` — outer HTML of the match (or whole document).
- **`snapshot`** `{name}` — writes `<name>.html` + `<name>.png` (in-process
  page render, works headless) to the snapshot dir and returns the paths.
- **`console_logs`** `{clear?}` — captured `console.*` output, uncaught
  errors, and unhandled promise rejections from the page.

### Interaction
- **`click`** `{selector}` — fires mousedown/mouseup/click.
- **`fill`** `{selector, value}` — sets a form value (React-aware) and
  fires input/change.
- **`press`** `{selector, key}` — keydown/keyup; `key` as in
  `KeyboardEvent.key` (e.g. `"Enter"`).
- **`submit`** `{selector}` — submits a form.
- **`navigate`** `{url}` — loads a URL in the WebView.

### Inspection
- **`text`** `{selector}` — trimmed `textContent`.
- **`attr`** `{selector, name}` — one attribute.
- **`count`** `{selector}` — number of matches.
- **`wait_for`** `{selector, timeout_ms?}` — poll until a match exists.
- **`evaluate_js`** `{expression}` — evaluate an expression, return its
  JSON value. The escape hatch for anything the tools don't cover.

### Into C++
- **`invoke_command`** `{command, payload?}` — call an app bridge command
  directly (the same typed handlers the page calls) and get its JSON
  result. Lets you set up or read app state without touching the UI.

---

## 5. Worked example

Drive the Calculator app (math lives in C++; the page only sends keys):

```bash
build/agentharness/Calculator > /tmp/calc.log 2>&1 &
P=localhost:9696/mcp
tool() { curl -s "$P" -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",
  \"params\":{\"name\":\"$1\",\"arguments\":$2}}"; }

tool wait_for   '{"selector":"@calc-display"}'
tool list_elements '{}'                       # discover the keypad handles
tool click      '{"selector":"@key-7"}'
tool click      '{"selector":"@key-plus"}'
tool click      '{"selector":"@key-5"}'
tool click      '{"selector":"@key-equals"}'
tool text       '{"selector":"@calc-display"}'   # -> "12"
tool screenshot '{}'                              # PNG (base64 image content)
```

Extract a screenshot from the response in Python:

```python
import json, base64, urllib.request
r = json.load(urllib.request.urlopen(urllib.request.Request(
    "http://localhost:9696/mcp",
    json.dumps({"jsonrpc":"2.0","id":1,"method":"tools/call",
        "params":{"name":"screenshot","arguments":{}}}).encode(),
    {"Content-Type":"application/json"})))
img = r["result"]["content"][0]
open("shot.png","wb").write(base64.b64decode(img["data"]))
```

---

## 6. Gotchas

- **Issue tool calls one at a time.** Each call pumps the app's event
  loop; a second concurrent call is refused with a clear error. Serialize.
- **Cold start is slow** with ASan + a debugger + WebView warmup — first
  MCP response can take several seconds. Poll (`wait_for`, or retry the
  request) rather than assuming a fixed sleep.
- **The page must be loaded** before DOM tools work. Gate on
  `wait_for` for a known element; a tool call during initial load may
  return a JS error.
- **One server per port.** Running several apps? Each after the first
  lands on an ephemeral port — read it from the launch log.
- **Build type controls everything.** A `Release` (or RelWithDebInfo /
  MinSizeRel) build has no harness, no ASan, no MCP server — by design.
  If `localhost:9696` doesn't answer, check `CMAKE_BUILD_TYPE`. IDEs that
  share the `build/` directory can silently reconfigure it; a dedicated
  Debug build directory avoids the flapping.

---

## 7. Build switches reference

All are `AUTO` | `ON` | `OFF`, default `AUTO` (= non-release builds),
resolved independently.

| Switch | Controls |
|---|---|
| `EACP_AGENT_HARNESS` | the `build/agentharness/<Target>` launcher |
| `EACP_ASAN` | AddressSanitizer on app executables |
| `EACP_DEBUG_SERVER` | the embedded MCP debug server (window capture for any app; WebView DOM tools when present) |

Runtime: `EACP_DEBUG_PORT` (port / `off`), `EACP_NO_DEBUGGER=1` (skip the
debugger in the launcher).

Per-app opt-outs on `eacp_add_webview_app(...)`: `NO_ASAN`,
`NO_DEBUG_SERVER`.

For a hand-rolled (non-WebView) executable target, opt into the whole set
in one call — launcher, ASan, and the window capture/MCP server, so the
app can be screenshotted / recorded over MCP with no app-code changes:

```cmake
add_executable(MyTool Main.cpp)
eacp_enable_dev_affordances(MyTool)   # launcher + ASan + window MCP server
```

Or pick them individually:

```cmake
eacp_add_agent_harness(MyTool)            # the launcher
eacp_enable_agent_asan(MyTool)            # ASan
eacp_enable_window_debug_server(MyTool)   # screenshot + recording over MCP
```

---

## Where this lives in the source

- `CMake/EacpAgentHarness.cmake` — launcher + ASan + window-debug-server
  functions and switches (`eacp_enable_dev_affordances`).
- `Lib/eacp/Network/MCP/` — the generic MCP server primitive.
- `Lib/eacp/Graphics/Remote/` — `WindowDebugServer` (window capture tools:
  screenshot + recording), the window auto-attach + port policy. The
  window-level server every app gets.
- `Lib/eacp/Graphics/Helpers/ScreenRecorder.*` — the ScreenCaptureKit
  capture/record backend behind those tools.
- `Lib/eacp/WebView/Remote/` — `WebViewTools` (the DOM tools, added onto
  the window server as an extension), `AutoAttach` (the bridge hook),
  `console-capture.js`.
- `Lib/eacp/WebView/WebView/ElementIds.*` — the `@id` / `data-testid`
  system.
- `Lib/eacp/WebView/Test/` — `AppDriver`, the in-process driver the tools
  are built on (usable directly from C++ tests; see
  `Apps/WebView/WebViewTodo/Tests/`).
