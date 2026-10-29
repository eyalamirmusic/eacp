# The Windows Error Reporting (WER) helper DLL: WerFault.exe loads it for a
# fast-fail or heap-corruption crash no in-process filter sees, so that crash
# still gets a stack. Copied beside each app by eacp_enable_crash_reporting; it
# links nothing of eacp.
add_library(eacp-windows-error-reporting-helper SHARED WindowsErrorReportingHelper-Windows.cpp)
set_default_target_setting(eacp-windows-error-reporting-helper)

# A PDB in every configuration, as the app's own gets, for SYMBOLS_DIR.
eacp_add_crash_symbols(eacp-windows-error-reporting-helper "")
