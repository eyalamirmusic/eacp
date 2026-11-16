#include <eacp/WebView/WebView.h>
#include <eacp/WebView/WebView/JsStringLiteral.h>

using namespace eacp;
using namespace Graphics;

// Options::forwardUnhandledKeys, by hand. Every key the page sees gets a row:
// the page's verdict (stays in the page, or is passed on) and, for a key
// passed on, the KeyCode that reached onUnhandledKeyEvent. The page's own
// controls show which targets keep which keys, and the long section below
// is for Page Up/Down, Home and End. The checklist in the page is the manual
// test: it needs real keys, which no automated test sends.

namespace
{
struct KeyCodeName
{
    uint16_t keyCode;
    const char* name;
};

constexpr auto keyCodeNames = Array {KeyCodeName {KeyCode::PageUp, "PageUp"},
                                     KeyCodeName {KeyCode::PageDown, "PageDown"},
                                     KeyCodeName {KeyCode::Home, "Home"},
                                     KeyCodeName {KeyCode::End, "End"},
                                     KeyCodeName {KeyCode::LeftArrow, "LeftArrow"},
                                     KeyCodeName {KeyCode::RightArrow, "RightArrow"},
                                     KeyCodeName {KeyCode::UpArrow, "UpArrow"},
                                     KeyCodeName {KeyCode::DownArrow, "DownArrow"},
                                     KeyCodeName {KeyCode::Space, "Space"},
                                     KeyCodeName {KeyCode::Tab, "Tab"},
                                     KeyCodeName {KeyCode::Return, "Return"},
                                     KeyCodeName {KeyCode::Escape, "Escape"},
                                     KeyCodeName {KeyCode::Delete, "Delete"}};

std::string getKeyCodeName(uint16_t keyCode)
{
    for (auto& entry: keyCodeNames)
    {
        if (entry.keyCode == keyCode)
            return entry.name;
    }

    return "KeyCode " + std::to_string(keyCode);
}

std::string describeModifiers(const ModifierKeys& modifiers)
{
    auto text = std::string {};

    if (modifiers.shift)
        text += "Shift ";
    if (modifiers.control)
        text += "Ctrl ";
    if (modifiers.alt)
        text += "Alt ";
    if (modifiers.command)
        text += "Cmd ";

    return text;
}

std::string describeForwardedKey(const KeyEvent& event)
{
    auto kind = event.type == KeyEventType::Down ? "down" : "up";
    auto repeat = event.isRepeat ? " (repeat)" : "";
    return std::string {kind} + " " + describeModifiers(event.modifiers)
           + getKeyCodeName(event.keyCode) + repeat;
}

const std::string keysPageHtml = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><style>
  body { font: 14px/1.5 -apple-system, "Segoe UI", sans-serif; margin: 0;
         padding: 24px; background: #14161a; color: #e6e8ec; }
  h2 { font-size: 12px; text-transform: uppercase; letter-spacing: .1em;
       color: #8b93a1; margin: 24px 0 8px; }
  .targets { display: grid; grid-template-columns: repeat(3, 1fr); gap: 10px; }
  .targets > * { background: #1d2027; color: inherit; border: 1px solid #2b303a;
                 border-radius: 8px; padding: 10px; font: inherit; }
  #plain:focus { outline: 2px solid #6ea8fe; }
  table { width: 100%; border-collapse: collapse; font: 12px ui-monospace, Consolas, monospace; }
  th, td { text-align: left; padding: 3px 8px; border-bottom: 1px solid #2b303a; }
  td.page { color: #f0b46e; } td.passed { color: #7fd18b; }
  #log { max-height: 260px; overflow: auto; background: #1d2027;
         border: 1px solid #2b303a; border-radius: 8px; }
  ol { margin: 0; padding-left: 20px; }
  .section { height: 420px; border-top: 1px dashed #2b303a; padding-top: 8px; color: #8b93a1; }
</style></head>
<body>
  <h2>Click a target, then press keys</h2>
  <div class="targets">
    <div id="plain" tabindex="0">Plain area: every key but Tab is passed on</div>
    <input type="text" placeholder="Text input: keeps every key" />
    <textarea placeholder="Textarea: keeps every key"></textarea>
    <div contenteditable="true">Contenteditable: keeps every key</div>
    <input type="range" />
    <button>Button: keeps Space and Enter</button>
    <label><input type="checkbox" /> Checkbox: keeps Space and Enter</label>
    <div id="prevented" tabindex="0">This area calls preventDefault on every key</div>
  </div>

  <h2>Key events</h2>
  <div id="log"><table>
    <thead><tr><th>#</th><th>kind</th><th>key</th><th>keyCode</th><th>modifiers</th>
      <th>repeat</th><th>page verdict</th><th>native KeyCode</th></tr></thead>
    <tbody id="rows"></tbody>
  </table></div>

  <h2>Checklist (real keys)</h2>
  <ol>
    <li>Plain area: Page Up/Down, Home/End and the arrows are passed on with
        their KeyCode, and the page below scrolls.</li>
    <li>Plain area: Space and Shift+Space are passed on; Shift shows in modifiers.</li>
    <li>Tab moves focus and stays in the page.</li>
    <li>Holding a key shows repeat rows.</li>
    <li>Text input, textarea and contenteditable keep every key.</li>
    <li>Range input keeps the arrows and Page keys, passes Space on.</li>
    <li>The preventDefault area keeps every key.</li>
    <li>Typing with an IME or a dead key (Option+E then E on macOS, a
        dead-key layout on Windows) stays in the page while composing.</li>
  </ol>

  <h2>Scroll page</h2>
  <div id="sections"></div>

  <script>
    for (let i = 1; i <= 12; ++i) {
      const section = document.createElement('div');
      section.className = 'section';
      section.textContent = 'Section ' + i + ' of 12';
      document.getElementById('sections').appendChild(section);
    }

    document.getElementById('prevented').addEventListener('keydown', (e) => e.preventDefault());

    const rows = document.getElementById('rows');
    const awaitingNative = [];
    let count = 0;

    function addCell(row, text, className) {
      const cell = row.insertCell();
      cell.textContent = text;
      if (className) cell.className = className;
      return cell;
    }

    function describeModifiers(mask) {
      return ['Shift', 'Ctrl', 'Alt', 'Cmd'].filter((name, bit) => mask & (1 << bit)).join(' ');
    }

    function addReport(report) {
      const row = rows.insertRow(0);
      addCell(row, ++count);
      addCell(row, report.kind);
      addCell(row, JSON.stringify(report.key));
      addCell(row, report.keyCode);
      addCell(row, describeModifiers(report.modifiers));
      addCell(row, report.repeat ? 'yes' : '');
      addCell(row, report.consumed ? 'stays in page' : 'passed on',
              report.consumed ? 'page' : 'passed');
      const nativeCell = addCell(row, report.consumed ? '' : 'waiting');
      if (!report.consumed) awaitingNative.push(nativeCell);
    }

    window.showForwardedKey = (description) => {
      const cell = awaitingNative.shift();
      if (cell) cell.textContent = description;
    };

    const keyHandler = window.webkit.messageHandlers.__eacpKeyEvent;
    const postToNative = keyHandler.postMessage.bind(keyHandler);
    keyHandler.postMessage = (report) => {
      addReport(report);
      postToNative(report);
    };

    document.getElementById('plain').focus();
  </script>
</body></html>)HTML";

WebView::Options createKeyForwardingOptions()
{
    auto options = WebView::Options {};
    options.forwardUnhandledKeys = true;
    return options;
}
} // namespace

struct KeysView final : View
{
    KeysView()
    {
        webView.onUnhandledKeyEvent = [this](const KeyEvent& event)
        {
            webView.evaluateJavaScript("window.showForwardedKey("
                                       + jsStringLiteral(describeForwardedKey(event))
                                       + ")");
            return true;
        };
        webView.loadHTML(keysPageHtml);
        addChildren({webView});
    }

    void resized() override { scaleToFit({webView}); }

    WebView webView {createKeyForwardingOptions()};
};

int main()
{
    return Graphics::runWindowedApp<KeysView>();
}
