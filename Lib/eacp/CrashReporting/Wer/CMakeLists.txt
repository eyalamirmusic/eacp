# The WER runtime exception helper WerFault.exe loads for a crash no
# in-process filter sees. Its own DLL, copied beside each app by
# eacp_enable_crash_reporting; it links nothing of eacp.
add_library(eacp-crash-wer SHARED WerModule-Windows.cpp)
set_default_target_setting(eacp-crash-wer)

# A PDB in every configuration, as the app's own gets, for SYMBOLS_DIR.
eacp_add_crash_symbols(eacp-crash-wer "")
