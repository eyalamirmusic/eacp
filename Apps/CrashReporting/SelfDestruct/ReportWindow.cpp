#include "ReportWindow.h"

#include <eacp/Graphics/Window/ViewWindow.h>
#include <eacp/UI/UI.h>

using namespace eacp;

namespace SelfDestruct
{
namespace
{
constexpr auto windowWidth = 1100;
constexpr auto windowHeight = 760;
constexpr auto padding = 20.f;
constexpr auto headingFontSize = 26.f;
constexpr auto messageFontSize = 16.f;
constexpr auto bodyFontSize = 13.f;
constexpr auto codeFontSize = 12.f;
constexpr auto headingHeight = 38.f;
constexpr auto messageHeight = 26.f;
constexpr auto lineHeight = 22.f;
constexpr auto sectionTitleHeight = 26.f;
constexpr auto rowHeight = 20.f;
constexpr auto cellInset = 8.f;
constexpr auto detailsShare = 0.3f;
constexpr auto indexColumnWidth = 44.f;
constexpr auto symbolColumnWidth = 600.f;
constexpr auto moduleColumnWidth = 220.f;
constexpr auto offsetColumnWidth = 140.f;
constexpr auto keyColumnWidth = 220.f;
constexpr auto valueColumnWidth = 800.f;

constexpr auto alertColour = UI::Color {1.f, 0.38f, 0.35f};
constexpr auto symbolColour = UI::Color {0.6f, 0.86f, 0.62f};

enum class FrameColumn
{
    index,
    symbol,
    module,
    offset
};

constexpr auto frameColumns = std::to_array<FrameColumn>({FrameColumn::index,
                                                          FrameColumn::symbol,
                                                          FrameColumn::module,
                                                          FrameColumn::offset});

std::string getModuleName(const CrashReporting::StackFrame& frame)
{
    if (frame.modulePath.empty())
        return "?";

    return FilePath {frame.modulePath}.fileName();
}

std::string getFrameSymbol(const CrashReporting::StackFrame& frame)
{
    if (frame.symbol.empty())
        return "(no symbol)";

    return frame.symbol;
}

std::string getFrameCellText(int row,
                             FrameColumn column,
                             const CrashReporting::StackFrame& frame)
{
    switch (column)
    {
        case FrameColumn::index:
            return std::to_string(row);
        case FrameColumn::symbol:
            return getFrameSymbol(frame);
        case FrameColumn::module:
            return getModuleName(frame);
        case FrameColumn::offset:
            break;
    }

    return "+ " + CrashReporting::formatHexAddress(frame.offset);
}

void paintBandedRow(UI::Graphics& g, int row, const UI::Rect& bounds, bool selected)
{
    const auto& theme = UI::defaultTheme();

    if (selected)
    {
        g.setColour(theme.get(UI::ThemeKeys::accentDim));
        g.fillRect(bounds);
    }
    else if (row % 2 == 1)
    {
        g.setColour(theme.get(UI::ThemeKeys::panel));
        g.fillRect(bounds);
    }
}

void setCodeFont(UI::Graphics& g)
{
    auto font = UI::Font {};
    font.pointSize = codeFontSize;
    g.setFont(font);
}
UI::Color getFrameCellColour(bool isResolvedSymbol)
{
    if (isResolvedSymbol)
        return symbolColour;

    return UI::defaultTheme().get(UI::ThemeKeys::dimText);
}

struct StackModel final : UI::ListBoxModel
{
    explicit StackModel(const Vector<CrashReporting::StackFrame>& framesToShow)
        : frames(framesToShow)
    {
    }

    int getNumRows() override { return frames.size(); }

    void paintRow(UI::Graphics& g,
                  int row,
                  const UI::Rect& bounds,
                  bool selected) override
    {
        paintBandedRow(g, row, bounds, selected);
    }

    void paintCell(
        UI::Graphics& g, int row, int column, const UI::Rect& bounds, bool) override
    {
        const auto& frame = frames[row];
        auto frameColumn = frameColumns[column];
        auto isResolvedSymbol =
            frameColumn == FrameColumn::symbol && !frame.symbol.empty();
        setCodeFont(g);
        g.setColour(getFrameCellColour(isResolvedSymbol));
        g.drawText(getFrameCellText(row, frameColumn, frame),
                   bounds.inset(cellInset, 0.f));
    }

    Vector<CrashReporting::StackFrame> frames;
};

struct DetailLine
{
    std::string key;
    std::string value;
};

Vector<DetailLine> makeDetailLines(const CrashReporting::CrashReport& report)
{
    auto lines = Vector<DetailLine> {};

    for (const auto& [key, value]: report.metadata)
        lines.add({key, value});

    for (const auto& [key, value]: report.tags)
        lines.add({"tag " + key, value});

    for (const auto& [key, value]: report.extra)
        lines.add({"extra " + key, value});

    if (!report.user.id.empty())
        lines.add({"user", report.user.id});

    for (const auto& breadcrumb: report.breadcrumbs)
        lines.add({"breadcrumb " + breadcrumb.category, breadcrumb.message});

    for (const auto& attachment: report.attachments)
        lines.add({"attachment", attachment.str()});

    lines.add({"report folder", report.folder.str()});
    lines.add(
        {"captured by",
         std::string {CrashReporting::getCaptureSourceName(report.capturedBy)}});

    return lines;
}

struct DetailsModel final : UI::ListBoxModel
{
    explicit DetailsModel(const CrashReporting::CrashReport& report)
        : lines(makeDetailLines(report))
    {
    }

    int getNumRows() override { return lines.size(); }

    void paintRow(UI::Graphics& g,
                  int row,
                  const UI::Rect& bounds,
                  bool selected) override
    {
        paintBandedRow(g, row, bounds, selected);
    }

    void paintCell(
        UI::Graphics& g, int row, int column, const UI::Rect& bounds, bool) override
    {
        const auto& line = lines[row];
        const auto& theme = UI::defaultTheme();
        auto isKey = column == 0;

        g.setColour(theme.get(isKey ? UI::ThemeKeys::dimText : UI::ThemeKeys::text));
        g.drawText(isKey ? line.key : line.value, bounds.inset(cellInset, 0.f));
    }

    Vector<DetailLine> lines;
};

Vector<UI::ListBox::Column> makeStackColumns()
{
    return {{"#", indexColumnWidth},
            {"Symbol", symbolColumnWidth},
            {"Module", moduleColumnWidth},
            {"Offset", offsetColumnWidth}};
}

Vector<UI::ListBox::Column> makeDetailColumns()
{
    return {{"Metadata and attachments", keyColumnWidth}, {"", valueColumnWidth}};
}

bool isHang(const CrashReporting::CrashReport& report)
{
    return report.kind == CrashReporting::ReportKind::hang;
}

std::string describeHeading(const CrashReporting::CrashReport& report)
{
    if (isHang(report))
        return "SelfDestruct hung: " + report.message;

    return "SelfDestruct crashed: " + report.reason;
}

std::string describeMessage(const CrashReporting::CrashReport& report)
{
    if (report.message.empty())
        return "The crash carried no message.";

    return report.message;
}

std::string describeFault(const CrashReporting::CrashReport& report)
{
    return "Fault address " + CrashReporting::formatHexAddress(report.faultAddress)
           + "    thread "
           + CrashReporting::formatHexAddress(report.crashingThread.threadId)
           + "    " + report.platform + "    report " + report.id;
}

std::string describeStackTitle(const CrashReporting::CrashReport& report)
{
    if (report.crashingThread.frames.empty())
        return "No stack was captured";

    if (isHang(report))
        return "Stack of the unresponsive main thread";

    return "Stack of the crashed thread";
}

struct ReportView final : UI::Component
{
    explicit ReportView(const CrashReporting::CrashReport& report)
        : heading(describeHeading(report))
        , message(describeMessage(report))
        , fault(describeFault(report))
        , stackTitle(describeStackTitle(report))
        , stackModel(report.crashingThread.frames)
        , detailsModel(report)
    {
        heading.setFontSize(headingFontSize);
        heading.setColour(alertColour);
        message.setFontSize(messageFontSize);
        fault.setThemeColour(UI::ThemeKeys::dimText);
        stackTitle.setThemeColour(UI::ThemeKeys::dimText);

        stack.setModel(&stackModel);
        stack.setRowHeight(rowHeight);
        stack.setColumns(makeStackColumns());
        details.setModel(&detailsModel);
        details.setRowHeight(rowHeight);
        details.setColumns(makeDetailColumns());

        addChildren({heading, message, fault, stackTitle, stack, details});
    }

    void paint(UI::Graphics& g) override
    {
        g.fillAll(UI::defaultTheme().get(UI::ThemeKeys::background));
    }

    void paintOverChildren(UI::Graphics& g) override
    {
        g.setColour(UI::defaultTheme().get(UI::ThemeKeys::outline));
        g.drawRect(stack.getBounds());
        g.drawRect(details.getBounds());
    }

    void resized() override
    {
        auto area = getLocalBounds().inset(padding);

        heading.setBounds(area.removeFromTop(headingHeight));
        message.setBounds(area.removeFromTop(messageHeight));
        fault.setBounds(area.removeFromTop(lineHeight));
        area.removeFromTop(padding);

        details.setBounds(area.removeFromBottom(area.h * detailsShare));
        area.removeFromBottom(padding);
        stackTitle.setBounds(area.removeFromTop(sectionTitleHeight));
        stack.setBounds(area);
    }

    UI::Label heading;
    UI::Label message;
    UI::Label fault;
    UI::Label stackTitle;
    StackModel stackModel;
    DetailsModel detailsModel;
    UI::ListBox stack;
    UI::ListBox details;
};

struct ReportHost final : UI::ComponentHost
{
    explicit ReportHost(const CrashReporting::CrashReport& report)
        : view(report)
    {
        setFontPointSize(bodyFontSize);
        setRootComponent(view);
    }

    ReportView view;
};

std::string describeWindowTitle(const CrashReporting::CrashReport& report)
{
    if (isHang(report))
        return "SelfDestruct hang report";

    return "SelfDestruct crash report";
}

Graphics::WindowOptions
    makeReportWindowOptions(const CrashReporting::CrashReport& report)
{
    auto options = Graphics::WindowOptions {};
    options.width = windowWidth;
    options.height = windowHeight;
    options.title = describeWindowTitle(report);

    return options;
}

struct ReportApp
{
    explicit ReportApp(const CrashReporting::CrashReport& report)
        : shown(makeReportWindowOptions(report), report)
    {
        Threads::callAsync([this] { shown.window.toFront(); });
    }

    Graphics::ViewWindow<ReportHost> shown;
};

} // namespace

int runReportWindow(const CrashReporting::CrashReport& report)
{
    return Apps::run<ReportApp>(report);
}
} // namespace SelfDestruct
