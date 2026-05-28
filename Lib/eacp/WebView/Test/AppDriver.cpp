#include "AppDriver.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace eacp::WebView::Test
{

namespace
{

constexpr auto defaultSnapshotSubdir = "test-results/snapshots";

std::string resolveSnapshotDir(std::string fromOptions)
{
    if (!fromOptions.empty())
        return fromOptions;

    auto cwd = std::filesystem::current_path();
    return (cwd / defaultSnapshotSubdir).string();
}

// Mirror of the JS side's sanitizer (eacp-test-node/AppDriver.ts).
// Forward slashes pass through so callers can intentionally write into
// sub-directories; anything else outside the portable set becomes '_'.
std::string sanitizeSnapshotName(const std::string& name)
{
    auto out = std::string {};
    out.reserve(name.size());
    for (auto c: name)
    {
        auto ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                  || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '/'
                  || c == '-';
        out.push_back(ok ? c : '_');
    }
    return out;
}

std::vector<std::uint8_t> base64Decode(std::string_view encoded)
{
    static constexpr int sentinel = -1;
    static const auto lookup = []
    {
        auto table = std::array<int, 256> {};
        for (auto& v: table)
            v = sentinel;
        auto alphabet = std::string_view {
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
        for (auto i = 0u; i < alphabet.size(); ++i)
            table[static_cast<unsigned char>(alphabet[i])] = static_cast<int>(i);
        return table;
    }();

    auto out = std::vector<std::uint8_t> {};
    out.reserve(encoded.size() * 3 / 4);

    auto buffer = 0u;
    auto bits = 0;

    for (auto c: encoded)
    {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t')
            continue;

        auto idx = lookup[static_cast<unsigned char>(c)];
        if (idx == sentinel)
            throw std::runtime_error("AppDriver: invalid base64 character "
                                     "in screenshot payload");

        buffer = (buffer << 6) | static_cast<unsigned>(idx);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xff));
        }
    }

    return out;
}

void writeBinary(const std::filesystem::path& path,
                 const std::vector<std::uint8_t>& bytes)
{
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());

    auto file = std::ofstream {path, std::ios::binary | std::ios::trunc};
    if (!file)
        throw std::runtime_error("AppDriver: failed to open '" + path.string()
                                 + "' for writing");
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!file)
        throw std::runtime_error("AppDriver: short write to '" + path.string()
                                 + "'");
}

void writeText(const std::filesystem::path& path, const std::string& text)
{
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());

    auto file = std::ofstream {path, std::ios::trunc};
    if (!file)
        throw std::runtime_error("AppDriver: failed to open '" + path.string()
                                 + "' for writing");
    file << text;
    if (!file)
        throw std::runtime_error("AppDriver: short write to '" + path.string()
                                 + "'");
}

Miro::JSON makeObject()
{
    return Miro::JSON {Miro::Json::Object {}};
}

void putString(Miro::JSON& obj, const std::string& key, const std::string& value)
{
    obj.asObject()[key] = Miro::JSON {value};
}

void putInt(Miro::JSON& obj, const std::string& key, int value)
{
    obj.asObject()[key] = Miro::JSON {static_cast<double>(value)};
}

} // namespace

AppDriver::AppDriver(std::string rpcUrl, AppDriverOptions options)
    : client(rpcUrl)
    , rpcUrlValue(std::move(rpcUrl))
    , defaultTimeoutMs(options.defaultTimeoutMs)
    , snapshotDir(resolveSnapshotDir(std::move(options.snapshotDir)))
{
}

const std::string& AppDriver::rpcUrl() const
{
    return rpcUrlValue;
}

Miro::JSON AppDriver::invoke(const std::string& command,
                             const Miro::JSON& payload) const
{
    return client.invokeRaw(command, payload);
}

Miro::JSON AppDriver::runCommand(const std::string& command,
                                 Miro::JSON payload,
                                 const CallOptions& opts) const
{
    if (auto t = opts.timeoutMs; t)
        putInt(payload, "timeoutMs", *t);
    else if (defaultTimeoutMs)
        putInt(payload, "timeoutMs", *defaultTimeoutMs);

    return client.invokeRaw(command, payload);
}

namespace
{

Miro::JSON selectorPayload(const std::string& selector)
{
    auto p = Miro::JSON {Miro::Json::Object {}};
    p.asObject()["selector"] = Miro::JSON {selector};
    return p;
}

bool asBool(const Miro::JSON& v)
{
    return v.isBool() && v.asBool();
}

std::string asString(const Miro::JSON& v)
{
    if (v.isString())
        return v.asString();
    if (v.isNull())
        return {};
    throw std::runtime_error("AppDriver: expected string result");
}

int asInt(const Miro::JSON& v)
{
    if (v.isNumber())
        return static_cast<int>(v.asNumber());
    throw std::runtime_error("AppDriver: expected numeric result");
}

} // namespace

bool AppDriver::click(const std::string& selector, CallOptions opts) const
{
    return asBool(runCommand("test.click", selectorPayload(selector), opts));
}

bool AppDriver::fill(const std::string& selector, const std::string& value,
                     CallOptions opts) const
{
    auto p = selectorPayload(selector);
    putString(p, "value", value);
    return asBool(runCommand("test.fill", p, opts));
}

bool AppDriver::press(const std::string& selector, const std::string& key,
                      CallOptions opts) const
{
    auto p = selectorPayload(selector);
    putString(p, "key", key);
    return asBool(runCommand("test.press", p, opts));
}

bool AppDriver::submit(const std::string& selector, CallOptions opts) const
{
    return asBool(runCommand("test.submit", selectorPayload(selector), opts));
}

std::string AppDriver::text(const std::string& selector, CallOptions opts) const
{
    return asString(runCommand("test.text", selectorPayload(selector), opts));
}

std::optional<std::string> AppDriver::attr(const std::string& selector,
                                           const std::string& name,
                                           CallOptions opts) const
{
    auto p = selectorPayload(selector);
    putString(p, "name", name);
    auto result = runCommand("test.attr", p, opts);
    if (result.isNull())
        return std::nullopt;
    return asString(result);
}

bool AppDriver::exists(const std::string& selector, CallOptions opts) const
{
    return asBool(runCommand("test.exists", selectorPayload(selector), opts));
}

int AppDriver::count(const std::string& selector, CallOptions opts) const
{
    return asInt(runCommand("test.count", selectorPayload(selector), opts));
}

bool AppDriver::waitFor(const std::string& selector, CallOptions opts) const
{
    return asBool(runCommand("test.waitFor", selectorPayload(selector), opts));
}

Miro::JSON AppDriver::evaluate(const std::string& expression,
                               CallOptions opts) const
{
    auto p = makeObject();
    putString(p, "expression", expression);
    return runCommand("test.evaluate", p, opts);
}

std::string AppDriver::dom(std::string_view selector, CallOptions opts) const
{
    auto p = makeObject();
    if (!selector.empty())
        putString(p, "selector", std::string(selector));
    return asString(runCommand("test.dom", p, opts));
}

ScreenshotResult AppDriver::screenshot(const ScreenshotOptions& options) const
{
    auto callOpts = CallOptions {.timeoutMs = options.timeoutMs};
    auto reply = runCommand("test.screenshot", makeObject(), callOpts);

    if (!reply.isObject())
        throw std::runtime_error("AppDriver: test.screenshot reply was not an "
                                 "object");

    auto& obj = reply.asObject();
    auto it = obj.find("pngBase64");
    if (it == obj.end() || !it->second.isString())
        throw std::runtime_error("AppDriver: test.screenshot reply missing "
                                 "pngBase64 string");

    auto result = ScreenshotResult {};
    result.png = base64Decode(it->second.asString());

    if (!options.path.empty())
    {
        writeBinary(options.path, result.png);
        result.path = options.path;
    }

    return result;
}

SnapshotResult AppDriver::snapshot(const std::string& name,
                                   const SnapshotOptions& options) const
{
    auto baseName = sanitizeSnapshotName(name);
    auto baseDir = options.dir.empty() ? snapshotDir : options.dir;

    auto htmlPath = std::filesystem::path {baseDir} / (baseName + ".html");
    auto pngPath = std::filesystem::path {baseDir} / (baseName + ".png");

    auto callOpts = CallOptions {.timeoutMs = options.timeoutMs};

    auto html = dom(options.selector, callOpts);

    auto shot = screenshot({.timeoutMs = options.timeoutMs,
                            .path = pngPath.string()});

    writeText(htmlPath, html);

    return SnapshotResult {.name = name,
                           .dom = std::move(html),
                           .png = std::move(shot.png),
                           .domPath = htmlPath.string(),
                           .screenshotPath = pngPath.string()};
}

} // namespace eacp::WebView::Test
