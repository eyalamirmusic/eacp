#pragma once

#include "../Utils/Common.h"

namespace eacp::Apps::Detail
{
// Called by utf8Arguments on its copy of an argv. Windows hands main() its
// arguments in the ANSI code page, which loses every character the code page
// lacks: there, when the list is as long as the process's own command line
// (CommandLineToArgvW(GetCommandLineW())), each entry that is exactly the ANSI
// form of the matching process argument is replaced by that argument as UTF-8.
// An entry main() changed does not match and is kept. A list of another length
// is not the command line and is kept whole, which also turns recovery off
// where the C runtime and CommandLineToArgvW split the line differently (a
// "prog"x argv[0], "" inside quotes, setargv wildcard expansion); that is the
// old behaviour, never a worse one. Characters the code page best-fits to a
// quote, a backslash or a space (U+FF02, U+FF3C, U+3000) can make the C runtime
// split the line differently with the same count; an entry recovered then holds
// the user's own text for that position in a list whose shape was already
// wrong. Elsewhere main() already has UTF-8 and nothing changes.
void recoverUtf8Arguments(Vector<std::string>& arguments);
} // namespace eacp::Apps::Detail
