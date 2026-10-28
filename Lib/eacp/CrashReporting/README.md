# CrashReporting

When an app crashes, or its main thread stops answering, a second process
writes a report with the crashed thread's stack, the app's metadata and its
attachments, and hands it to the app's own code. macOS and Windows.

```cpp
struct MyCrashHandler : CrashReporting::CrashHandler
{
    // In the handler process, after the app is gone. Upload from here.
    void crashReported(const CrashReporting::CrashReport& report) override;

    // In the handler process; the hung app keeps running.
    void hangReported(const CrashReporting::CrashReport& report) override;

    // In the app, on later launches, for reports not yet marked handled.
    void pendingReportFound(const CrashReporting::CrashReport& report) override;
};

EACP_REGISTER_CRASH_HANDLER(MyCrashHandler);

int main(int argc, char* argv[])
{
    if (auto code = CrashReporting::runHandlerIfRequested(argc, argv))
        return *code;

    CrashReporting::start({});
    CrashReporting::setTag("tier", "pro");
    CrashReporting::addBreadcrumb({"ui", "opened settings"});
    ...
}
```

Enable it with `eacp_enable_crash_reporting(<executable> [SYMBOLS_DIR <dir>])`,
which links `eacp-crash-reporting` and writes a dSYM or PDB every build.

Reports land in `<database>/pending/<id>/report.json` (`CrashReport`, read
anywhere, Linux included, through `eacp-crash-report`). What each fault
reports on each platform and thread is [`MATRIX.md`](MATRIX.md); the
deliberate faults behind it are `eacp-crash-zoo` and the `SelfDestruct` app.

Limits:
- macOS captures in-process with `backtrace()`; no minidump there.
- Windows sees `__fastfail` and heap corruption only in the exit code, with no stack.
- Only the main thread is watched for hangs.
- With the handler process gone, the app writes a minimal fallback report itself.
- `SIGKILL` and the OOM killer are never caught.

Options, the report fields, uploading, every fault and how capture works:
[`Docs/CrashReporting.md`](../../../Docs/CrashReporting.md).
