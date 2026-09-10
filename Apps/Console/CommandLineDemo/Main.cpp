#include <eacp/Core/Core.h>

#include <iostream>

// Every switch below is a field. Try:
//
//   CommandLineDemo --help
//   CommandLineDemo say --times 5 --interval-ms 100 --style shout
//   CommandLineDemo say --prefix "> " --tag a --tag b --output.width 20
//   CommandLineDemo --times abc --bogus
//   CommandLineDemo --message "hi there" --colour   (note the spelling)

using namespace eacp;

enum class Style
{
    plain,
    shout,
    whisper
};

struct Output
{
    int width = 40;
    bool color = false;

    MIRO_REFLECT(width, color)
};

struct Options
{
    std::string message = "hello";
    int times = 3;
    int intervalMs = 250;
    Style style = Style::plain;
    std::optional<std::string> prefix;
    Vector<std::string> tag;
    Output output;

    MIRO_REFLECT(message, times, intervalMs, style, prefix, tag, output)
};

constexpr auto programName = "CommandLineDemo";

std::string toUpper(const std::string& text)
{
    auto upper = text;

    for (auto& character: upper)
        if (character >= 'a' && character <= 'z')
            character = static_cast<char>(character - 'a' + 'A');

    return upper;
}

std::string styled(const std::string& text, Style style)
{
    switch (style)
    {
        case Style::shout:
            return Strings::concat(toUpper(text), "!");
        case Style::whisper:
            return Strings::concat("(", Strings::toLower(text), ")");
        case Style::plain:
            return text;
    }

    return text;
}

std::string colored(const std::string& text, bool color)
{
    return color ? Strings::concat("\033[32m", text, "\033[0m") : text;
}

std::string padded(const std::string& text, int width)
{
    if (static_cast<int>(text.size()) >= width)
        return text;

    return text + std::string(width - static_cast<int>(text.size()), '.');
}

std::string joined(const Vector<std::string>& items)
{
    auto text = std::string {};

    for (const auto& item: items)
        text += (text.empty() ? "" : ", ") + item;

    return text;
}

struct App
{
    App()
    {
        std::cout << "command:     " << commandLine.command << '\n'
                  << "positionals: " << joined(commandLine.positionals) << '\n'
                  << "tags:        " << joined(options.tag) << '\n'
                  << "seed:        " << commandLine["seed"].toInt(42) << "\n\n";
    }

    void tick()
    {
        const auto line = padded(options.prefix.value_or("")
                                     + styled(options.message, options.style),
                                 options.output.width);

        std::cout << colored(line, options.output.color) << std::endl;

        if (++count >= options.times)
            Apps::quit();
    }

    Apps::CommandLine commandLine;
    Options options = commandLine.get<Options>();
    int count = 0;
    Threads::Timer timer {[&] { tick(); }, Time::MS {options.intervalMs}};
};

int main(int argc, char* argv[])
{
    const auto commandLine = Apps::CommandLine::fromArgv(argc, argv);

    if (commandLine.getBool("help"))
    {
        std::cout << Apps::CommandLine::usage<Options>(programName);
        return 0;
    }

    auto options = Options {};

    if (const auto report = commandLine.fill(options); !report.ok())
    {
        std::cerr << report.toString() << '\n'
                  << Apps::CommandLine::usage<Options>(programName);
        return 1;
    }

    return Apps::run<App>(argc, argv);
}
