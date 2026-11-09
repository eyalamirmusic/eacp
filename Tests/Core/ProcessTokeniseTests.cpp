// tokeniseCommandLine is the split ProcessOptions::commandLine gets on POSIX,
// pinned to what JUCE's StringArray::fromTokens(line, true) produces so a
// command line that worked through juce::ChildProcess::start(String) reaches
// the same argv.

#include "Common.h"

using namespace nano;
using eacp::Processes::tokeniseCommandLine;
using Tokens = eacp::Vector<std::string>;

auto tTokeniseSplitsOnWhitespace = test("ProcessTokenise/splitsOnWhitespace") = []
{
    check(tokeniseCommandLine("cmake --build build")
          == Tokens {"cmake", "--build", "build"});
    check(tokeniseCommandLine("a\tb\r\nc") == Tokens {"a", "b", "c"});
};

auto tTokeniseDropsEmpty = test("ProcessTokenise/dropsEmptyTokens") = []
{
    check(tokeniseCommandLine("  a   b  ") == Tokens {"a", "b"});
    check(tokeniseCommandLine("").empty());
    check(tokeniseCommandLine(" \t\n").empty());
};

auto tTokeniseKeepsQuotes = test("ProcessTokenise/keepsQuotesInArguments") = []
{
    check(tokeniseCommandLine("tool \"a b\" c") == Tokens {"tool", "\"a b\"", "c"});
    check(tokeniseCommandLine("tool --x=\"a b\"") == Tokens {"tool", "--x=\"a b\""});
};

auto tTokeniseSingleQuotesDoNotGroup =
    test("ProcessTokenise/singleQuotesDoNotGroup") = []
{ check(tokeniseCommandLine("tool 'a b'") == Tokens {"tool", "'a", "b'"}); };

auto tTokeniseUnclosedQuote = test("ProcessTokenise/unclosedQuoteRunsToEnd") = []
{ check(tokeniseCommandLine("tool \"a b c") == Tokens {"tool", "\"a b c"}); };

auto tTokeniseUnquotesExecutable =
    test("ProcessTokenise/unquotesExecutableOnly") = []
{
    check(tokeniseCommandLine("\"/Applications/My App/tool\" \"x\"")
          == Tokens {"/Applications/My App/tool", "\"x\""});
    check(tokeniseCommandLine("'tool' 'x'") == Tokens {"tool", "'x'"});
    check(tokeniseCommandLine("\"\" x") == Tokens {"", "x"});
};
