# The Windows Error Reporting (WER) helper DLL: WerFault.exe loads it for a
# fast-fail or heap-corruption crash no in-process filter sees, so that crash
# still gets a stack. Copied beside each app by eacp_enable_crash_reporting; it
# links nothing of eacp.
add_library(eacp-windows-error-reporting-helper SHARED WindowsErrorReportingHelper-Windows.cpp)
set_default_target_setting(eacp-windows-error-reporting-helper)

# A PDB (debug symbols) in every configuration, as the app gets, so
# SYMBOLS_DIR can collect it.
eacp_add_crash_symbols(eacp-windows-error-reporting-helper "")
