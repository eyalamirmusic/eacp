#pragma once
#include <string>

namespace
{

constexpr auto kHomePage = R"HTML(
<!doctype html>
<html>
<head>
    <meta charset="utf-8">
    <title>EACP WebView contract</title>
    <style>
        body { font: 15px/1.5 -apple-system, system-ui, sans-serif;
               max-width: 560px; margin: 48px auto; padding: 0 24px; color: #111; }
        h1   { font-size: 20px; margin-bottom: 28px; }
        ol   { padding-left: 22px; }
        li   { margin: 18px 0; }
        code { font: 13px ui-monospace, SFMono-Regular, Menlo, monospace;
               background: #f4f4f6; padding: 1px 5px; border-radius: 3px; }
        .hint { color: #666; font-size: 13px; margin-top: 4px; }
        a, button { font: inherit; color: #0a58ff; }
        button { background: none; border: 0; padding: 0;
                 cursor: pointer; text-decoration: underline; }
    </style>
</head>
<body>
    <h1>EACP WebView contract</h1>
    <ol>
        <li>
            <a href="https://www.eyalamir.org/">Same-view navigation</a>
            <div class="hint">
                <code>onNavigationDecision</code> returns <code>Allow</code>.
                Loads in this window.
            </div>
        </li>
        <li>
            <a href="https://example.com/" target="_blank">Open in new app window</a>
            <div class="hint">
                <code>target="_blank"</code> &rarr;
                <code>onNewWindowRequested</code> returns a fresh
                <code>WebView</code> in a new <code>Window</code>.
            </div>
        </li>
        <li>
            <a href="https://news.ycombinator.com/">Open in default browser</a>
            <div class="hint">
                External origin &rarr;
                <code>onNavigationDecision</code> returns
                <code>OpenExternally</code>.
            </div>
        </li>
        <li>
            <button onclick="window.open('https://github.com/', '_blank')">
                window.open() from JS
            </button>
            <div class="hint">
                Same path as #2 but driven by JS instead of a link click.
            </div>
        </li>
    </ol>
</body>
</html>
)HTML";

bool isSameOrigin(const std::string& url)
{
    return url.find("eyalamir.org") != std::string::npos;
}

} // namespace

