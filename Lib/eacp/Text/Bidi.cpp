#include "Bidi.h"

#include "Utf8.h"

#include <algorithm>

namespace eacp::Text
{
namespace
{
using enum BidiClass;

// X1: the deepest the directional status stack goes, and BD16's limit on how
// many bracket pairs one isolating run sequence contributes.
constexpr int maxEmbeddingDepth = 125;
constexpr int maxBracketPairs = 63;

bool isIsolateInitiator(BidiClass value)
{
    return value == LRI || value == RLI || value == FSI;
}

// X9: the embeddings, the overrides, their terminator and BN itself take no
// part in the weak, neutral and implicit rules.
bool isRemovedByX9(BidiClass value)
{
    return value == RLE || value == LRE || value == RLO || value == LRO
           || value == PDF || value == BN;
}

// BD12: the "NI" of rules N0 to N2 - neutrals and isolate formatting.
bool isNeutralOrIsolate(BidiClass value)
{
    return value == B || value == S || value == WS || value == ON
           || isIsolateInitiator(value) || value == PDI;
}

// N0, N1 and N2 all read a number as right-to-left.
BidiClass strongDirectionOf(BidiClass value)
{
    return value == EN || value == AN ? R : value;
}

BidiClass directionOfLevel(int level)
{
    return (level & 1) != 0 ? R : L;
}

struct DirectionalStatus
{
    int level = 0;
    BidiClass overrideStatus = ON;
    bool isolate = false;
};

// One bracket pair BD16 found, by position within the isolating run sequence
// rather than within the paragraph, so N0's scans are plain index walks.
struct BracketRange
{
    int opening = 0;
    int closing = 0;
};

// The paragraph as the algorithm works on it: the codepoints, the types they
// started with, the types the rules keep rewriting, the levels, and BD9's
// isolate matching.
struct Paragraph
{
    Vector<char32_t> text;
    Vector<BidiClass> original;
    Vector<BidiClass> types;
    Vector<int> levels;
    Vector<std::uint8_t> removed;

    // For an isolate initiator, where its matching PDI is, or size() when it
    // has none; for a PDI, which initiator matched it, or -1.
    Vector<int> matchingPdi;
    Vector<int> matchingInitiator;

    int paragraphLevel = 0;

    int size() const { return text.size(); }
};

// BD9.
void matchIsolates(Paragraph& paragraph)
{
    const auto count = paragraph.size();

    paragraph.matchingPdi = Vector<int>(count);
    paragraph.matchingInitiator = Vector<int>(count);

    for (auto index = 0; index < count; ++index)
    {
        paragraph.matchingPdi[index] = -1;
        paragraph.matchingInitiator[index] = -1;
    }

    for (auto index = 0; index < count; ++index)
    {
        if (!isIsolateInitiator(paragraph.original[index]))
            continue;

        auto depth = 1;
        paragraph.matchingPdi[index] = count;

        for (auto scan = index + 1; scan < count; ++scan)
        {
            const auto type = paragraph.original[scan];

            if (isIsolateInitiator(type))
            {
                ++depth;
            }
            else if (type == PDI && --depth == 0)
            {
                paragraph.matchingPdi[index] = scan;
                paragraph.matchingInitiator[scan] = index;
                break;
            }
        }
    }
}

// P2-P3, over a range, skipping what an isolate encloses.
int firstStrongLevel(const Paragraph& paragraph, int begin, int end)
{
    for (auto index = begin; index < end; ++index)
    {
        const auto type = paragraph.original[index];

        if (isIsolateInitiator(type))
        {
            index = paragraph.matchingPdi[index];

            if (index >= end)
                break;

            continue;
        }

        if (type == L)
            return 0;

        if (type == R || type == AL)
            return 1;
    }

    return 0;
}

int nextOddLevel(int level)
{
    return (level + 1) | 1;
}

int nextEvenLevel(int level)
{
    return (level + 2) & ~1;
}

// X1-X8.
void resolveExplicitLevels(Paragraph& paragraph)
{
    auto stack = Vector<DirectionalStatus> {};
    stack.add({paragraph.paragraphLevel, ON, false});

    auto overflowIsolates = 0;
    auto overflowEmbeddings = 0;
    auto validIsolates = 0;

    for (auto index = 0; index < paragraph.size(); ++index)
    {
        const auto type = paragraph.original[index];
        paragraph.levels[index] = stack.back().level;
        paragraph.removed[index] = isRemovedByX9(type) ? 1 : 0;

        switch (type)
        {
            case RLE:
            case LRE:
            case RLO:
            case LRO:
            {
                // X2-X5. The character itself is removed by X9, so only what
                // it pushes matters.
                const auto rightToLeft = type == RLE || type == RLO;
                const auto level = rightToLeft ? nextOddLevel(stack.back().level)
                                               : nextEvenLevel(stack.back().level);

                if (level <= maxEmbeddingDepth && overflowIsolates == 0
                    && overflowEmbeddings == 0)
                {
                    const auto status = type == RLO ? R : type == LRO ? L : ON;

                    stack.add({level, status, false});
                }
                else if (overflowIsolates == 0)
                    ++overflowEmbeddings;

                break;
            }

            case RLI:
            case LRI:
            case FSI:
            {
                // X5a-X5c. An isolate initiator belongs to the run it opens
                // in, and only after it does the deeper level start.
                if (stack.back().overrideStatus != ON)
                    paragraph.types[index] = stack.back().overrideStatus;

                auto rightToLeft = type == RLI;

                if (type == FSI)
                    rightToLeft = firstStrongLevel(paragraph,
                                                   index + 1,
                                                   paragraph.matchingPdi[index])
                                  == 1;

                const auto level = rightToLeft ? nextOddLevel(stack.back().level)
                                               : nextEvenLevel(stack.back().level);

                if (level <= maxEmbeddingDepth && overflowIsolates == 0
                    && overflowEmbeddings == 0)
                {
                    ++validIsolates;
                    stack.add({level, ON, true});
                }
                else
                {
                    ++overflowIsolates;
                }

                break;
            }

            case PDI:
            {
                // X6a.
                if (overflowIsolates > 0)
                {
                    --overflowIsolates;
                }
                else if (validIsolates > 0)
                {
                    overflowEmbeddings = 0;

                    while (!stack.back().isolate)
                        stack.erase(stack.end() - 1);

                    stack.erase(stack.end() - 1);
                    --validIsolates;
                }

                paragraph.levels[index] = stack.back().level;

                if (stack.back().overrideStatus != ON)
                    paragraph.types[index] = stack.back().overrideStatus;

                break;
            }

            case PDF:
            {
                // X7.
                if (overflowIsolates > 0)
                    break;

                if (overflowEmbeddings > 0)
                    --overflowEmbeddings;
                else if (!stack.back().isolate && stack.size() >= 2)
                    stack.erase(stack.end() - 1);

                break;
            }

            case B:
            {
                // X8: a paragraph separator terminates every embedding.
                stack.clear();
                stack.add({paragraph.paragraphLevel, ON, false});

                overflowIsolates = 0;
                overflowEmbeddings = 0;
                validIsolates = 0;

                paragraph.levels[index] = paragraph.paragraphLevel;
                break;
            }

            default:
            {
                // X6.
                if (stack.back().overrideStatus != ON)
                    paragraph.types[index] = stack.back().overrideStatus;

                break;
            }
        }
    }
}

// One isolating run sequence (BD13) and the boundary types X10 gives it.
struct RunSequence
{
    Vector<int> indices;
    int level = 0;
    BidiClass sos = L;
    BidiClass eos = L;
};

// BD13 and X10.
Vector<RunSequence> isolatingRunSequences(const Paragraph& paragraph)
{
    auto sequences = Vector<RunSequence> {};
    auto kept = Vector<int> {};

    for (auto index = 0; index < paragraph.size(); ++index)
        if (paragraph.removed[index] == 0)
            kept.add(index);

    if (kept.empty())
        return sequences;

    // Level runs, and which one every character X9 kept belongs to.
    auto runs = Vector<Vector<int>> {};
    auto runOf = Vector<int>(paragraph.size());

    for (auto position = 0; position < kept.size(); ++position)
    {
        const auto index = kept[position];

        const auto starts =
            position == 0
            || paragraph.levels[kept[position - 1]] != paragraph.levels[index];

        if (starts)
            runs.create();

        runs.back().add(index);
        runOf[index] = runs.size() - 1;
    }

    for (auto run = 0; run < runs.size(); ++run)
    {
        // A run opening with a PDI that matched an initiator continues that
        // initiator's sequence rather than starting one.
        const auto first = runs[run].front();

        if (paragraph.original[first] == PDI
            && paragraph.matchingInitiator[first] >= 0)
            continue;

        auto sequence = RunSequence {};
        sequence.level = paragraph.levels[first];

        for (auto at = run;;)
        {
            for (const auto index: runs[at])
                sequence.indices.add(index);

            const auto last = runs[at].back();

            if (!isIsolateInitiator(paragraph.original[last])
                || paragraph.matchingPdi[last] >= paragraph.size())
                break;

            at = runOf[paragraph.matchingPdi[last]];
        }

        sequences.add(std::move(sequence));
    }

    // X10: sos and eos compare the sequence's level with its neighbours'.
    for (auto& sequence: sequences)
    {
        const auto first = sequence.indices.front();
        const auto last = sequence.indices.back();

        auto before = paragraph.paragraphLevel;

        for (auto index = first - 1; index >= 0; --index)
            if (paragraph.removed[index] == 0)
            {
                before = paragraph.levels[index];
                break;
            }

        auto after = paragraph.paragraphLevel;

        // An isolate initiator with no matching PDI ends its sequence at the
        // paragraph, whatever follows it in the text.
        const auto danglingIsolate =
            isIsolateInitiator(paragraph.original[last])
            && paragraph.matchingPdi[last] >= paragraph.size();

        if (!danglingIsolate)
            for (auto index = last + 1; index < paragraph.size(); ++index)
                if (paragraph.removed[index] == 0)
                {
                    after = paragraph.levels[index];
                    break;
                }

        sequence.sos = directionOfLevel(std::max(sequence.level, before));
        sequence.eos = directionOfLevel(std::max(sequence.level, after));
    }

    return sequences;
}

// W1-W7.
void resolveWeakTypes(Paragraph& paragraph, const RunSequence& sequence)
{
    auto& types = paragraph.types;
    const auto& at = sequence.indices;
    const auto count = at.size();

    // W1: a mark takes the type of what it sits on.
    auto previous = sequence.sos;

    for (auto position = 0; position < count; ++position)
    {
        if (types[at[position]] == NSM)
            types[at[position]] =
                isIsolateInitiator(previous) || previous == PDI ? ON : previous;

        previous = types[at[position]];
    }

    // W2: a European number after an Arabic letter is an Arabic number.
    auto strong = sequence.sos;

    for (auto position = 0; position < count; ++position)
    {
        const auto type = types[at[position]];

        if (type == L || type == R || type == AL)
            strong = type;
        else if (type == EN && strong == AL)
            types[at[position]] = AN;
    }

    // W3.
    for (auto position = 0; position < count; ++position)
        if (types[at[position]] == AL)
            types[at[position]] = R;

    // W4: a lone separator between two numbers of a kind joins them.
    for (auto position = 1; position + 1 < count; ++position)
    {
        const auto type = types[at[position]];
        const auto before = types[at[position - 1]];
        const auto after = types[at[position + 1]];

        if (type == ES && before == EN && after == EN)
            types[at[position]] = EN;
        else if (type == CS && before == after && (before == EN || before == AN))
            types[at[position]] = before;
    }

    // W5: terminators beside a European number become one.
    for (auto position = 0; position < count;)
    {
        if (types[at[position]] != ET)
        {
            ++position;
            continue;
        }

        auto end = position;

        while (end < count && types[at[end]] == ET)
            ++end;

        const auto before = position > 0 ? types[at[position - 1]] : sequence.sos;
        const auto after = end < count ? types[at[end]] : sequence.eos;

        if (before == EN || after == EN)
            for (auto index = position; index < end; ++index)
                types[at[index]] = EN;

        position = end;
    }

    // W6.
    for (auto position = 0; position < count; ++position)
    {
        const auto type = types[at[position]];

        if (type == ET || type == ES || type == CS)
            types[at[position]] = ON;
    }

    // W7: a European number under left-to-right text is left-to-right.
    strong = sequence.sos;

    for (auto position = 0; position < count; ++position)
    {
        const auto type = types[at[position]];

        if (type == L || type == R)
            strong = type;
        else if (type == EN && strong == L)
            types[at[position]] = L;
    }
}

// BD16, by position within the sequence.
Vector<BracketRange> bracketPairsOf(const Paragraph& paragraph,
                                    const RunSequence& sequence)
{
    struct Opening
    {
        char32_t expected = 0;
        int position = 0;
    };

    auto stack = Vector<Opening> {};
    auto pairs = Vector<BracketRange> {};
    const auto& at = sequence.indices;

    for (auto position = 0; position < at.size(); ++position)
    {
        if (paragraph.types[at[position]] != ON)
            continue;

        const auto* bracket = bidiBracketOf(paragraph.text[at[position]]);

        if (bracket == nullptr)
            continue;

        if (bracket->type == BidiBracketType::Open)
        {
            // A sequence with more than 63 open brackets stops being
            // examined, rather than reporting the pairs it found so far.
            if (stack.size() == maxBracketPairs)
                return {};

            stack.add({bidiCanonicalBracket(bracket->paired), position});
            continue;
        }

        const auto closing = bidiCanonicalBracket(bracket->codepoint);

        for (auto entry = stack.size() - 1; entry >= 0; --entry)
        {
            if (stack[entry].expected != closing)
                continue;

            pairs.add({stack[entry].position, position});
            stack.erase(stack.begin() + entry, stack.end());
            break;
        }
    }

    std::sort(pairs.begin(),
              pairs.end(),
              [](const BracketRange& a, const BracketRange& b)
              { return a.opening < b.opening; });

    return pairs;
}

// N0's second half: the marks that followed a bracket before W1 rewrote them
// follow it again once the bracket has taken a direction.
void carryBracketTypeToMarks(Paragraph& paragraph,
                             const RunSequence& sequence,
                             int position,
                             BidiClass type)
{
    const auto& at = sequence.indices;

    for (auto next = position + 1; next < at.size(); ++next)
    {
        if (paragraph.original[at[next]] != NSM)
            return;

        paragraph.types[at[next]] = type;
    }
}

// N0.
void resolveBracketPairs(Paragraph& paragraph, const RunSequence& sequence)
{
    const auto& at = sequence.indices;
    const auto embedding = directionOfLevel(sequence.level);
    const auto opposite = embedding == L ? R : L;

    for (const auto& pair: bracketPairsOf(paragraph, sequence))
    {
        auto foundEmbedding = false;
        auto foundOpposite = false;

        for (auto position = pair.opening + 1; position < pair.closing; ++position)
        {
            const auto strong = strongDirectionOf(paragraph.types[at[position]]);

            if (strong == embedding)
                foundEmbedding = true;
            else if (strong == opposite)
                foundOpposite = true;
        }

        if (!foundEmbedding && !foundOpposite)
            continue;

        auto resolved = embedding;

        if (!foundEmbedding)
        {
            // Only the opposite direction inside: the text before the pair
            // decides whether that is what the brackets take.
            auto before = sequence.sos;

            for (auto position = pair.opening - 1; position >= 0; --position)
            {
                const auto strong = strongDirectionOf(paragraph.types[at[position]]);

                if (strong == L || strong == R)
                {
                    before = strong;
                    break;
                }
            }

            resolved = before == opposite ? opposite : embedding;
        }

        paragraph.types[at[pair.opening]] = resolved;
        paragraph.types[at[pair.closing]] = resolved;

        carryBracketTypeToMarks(paragraph, sequence, pair.opening, resolved);
        carryBracketTypeToMarks(paragraph, sequence, pair.closing, resolved);
    }
}

// N1 and N2.
void resolveNeutralTypes(Paragraph& paragraph, const RunSequence& sequence)
{
    resolveBracketPairs(paragraph, sequence);

    auto& types = paragraph.types;
    const auto& at = sequence.indices;
    const auto count = at.size();
    const auto embedding = directionOfLevel(sequence.level);

    for (auto position = 0; position < count;)
    {
        if (!isNeutralOrIsolate(types[at[position]]))
        {
            ++position;
            continue;
        }

        auto end = position;

        while (end < count && isNeutralOrIsolate(types[at[end]]))
            ++end;

        const auto before =
            position > 0 ? strongDirectionOf(types[at[position - 1]]) : sequence.sos;
        const auto after =
            end < count ? strongDirectionOf(types[at[end]]) : sequence.eos;

        // N1 when the surroundings agree, N2 - the embedding direction -
        // when they do not.
        const auto resolved = before == after ? before : embedding;

        for (auto index = position; index < end; ++index)
            types[at[index]] = resolved;

        position = end;
    }
}

// I1 and I2.
void resolveImplicitLevels(Paragraph& paragraph, const RunSequence& sequence)
{
    for (const auto index: sequence.indices)
    {
        const auto type = paragraph.types[index];
        auto& level = paragraph.levels[index];

        if ((level & 1) == 0)
        {
            if (type == R)
                level += 1;
            else if (type == AN || type == EN)
                level += 2;
        }
        else if (type == L || type == EN || type == AN)
        {
            level += 1;
        }
    }
}

// L1, on the types the paragraph started with: separators and the run of
// whitespace before them - or before the end of the line - go back to the
// paragraph level, so a trailing space in Hebrew sits where the eye expects.
void resetSeparatorLevels(Paragraph& paragraph)
{
    auto runStart = 0;

    for (auto index = 0; index < paragraph.size(); ++index)
    {
        const auto type = paragraph.original[index];

        if (type == B || type == S)
        {
            for (auto reset = runStart; reset <= index; ++reset)
                paragraph.levels[reset] = paragraph.paragraphLevel;

            runStart = index + 1;
        }
        else if (!(type == WS || isIsolateInitiator(type) || type == PDI
                   || isRemovedByX9(type)))
        {
            runStart = index + 1;
        }
    }

    for (auto reset = runStart; reset < paragraph.size(); ++reset)
        paragraph.levels[reset] = paragraph.paragraphLevel;
}

int baseLevelOf(const Paragraph& paragraph, BidiBaseDirection base)
{
    if (base == BidiBaseDirection::LeftToRight)
        return 0;

    if (base == BidiBaseDirection::RightToLeft)
        return 1;

    return firstStrongLevel(paragraph, 0, paragraph.size());
}

Paragraph analyse(Span<const char32_t> text, BidiBaseDirection base)
{
    auto paragraph = Paragraph {};
    paragraph.text = Vector<char32_t>(text.size());

    for (auto index = 0; index < text.size(); ++index)
        paragraph.text[index] = text[index];

    paragraph.original = Vector<BidiClass>(text.size());
    paragraph.types = Vector<BidiClass>(text.size());
    paragraph.levels = Vector<int>(text.size());
    paragraph.removed = Vector<std::uint8_t>(text.size());

    for (auto index = 0; index < text.size(); ++index)
    {
        paragraph.original[index] = bidiClassOf(text[index]);
        paragraph.types[index] = paragraph.original[index];
    }

    matchIsolates(paragraph);

    paragraph.paragraphLevel = baseLevelOf(paragraph, base);

    resolveExplicitLevels(paragraph);

    for (const auto& sequence: isolatingRunSequences(paragraph))
    {
        resolveWeakTypes(paragraph, sequence);
        resolveNeutralTypes(paragraph, sequence);
        resolveImplicitLevels(paragraph, sequence);
    }

    resetSeparatorLevels(paragraph);

    return paragraph;
}

// L2, over anything indexed by level: reverse every stretch at or above each
// level from the highest down to the lowest odd one.
template <typename LevelOf>
void reverseByLevel(Vector<int>& order, LevelOf levelOf)
{
    auto highest = 0;
    auto lowestOdd = maxEmbeddingDepth + 2;

    for (const auto entry: order)
    {
        const auto level = levelOf(entry);
        highest = std::max(highest, level);

        if ((level & 1) != 0)
            lowestOdd = std::min(lowestOdd, level);
    }

    for (auto level = highest; level >= lowestOdd; --level)
        for (auto position = 0; position < order.size(); ++position)
        {
            if (levelOf(order[position]) < level)
                continue;

            auto end = position;

            while (end < order.size() && levelOf(order[end]) >= level)
                ++end;

            std::reverse(order.begin() + position, order.begin() + end);
            position = end;
        }
}
} // namespace

BidiLevels bidiLevels(Span<const char32_t> text, BidiBaseDirection base)
{
    const auto paragraph = analyse(text, base);

    auto result = BidiLevels {};
    result.paragraphLevel = paragraph.paragraphLevel;
    result.levels = Vector<std::uint8_t>(paragraph.size());

    for (auto index = 0; index < paragraph.size(); ++index)
        result.levels[index] =
            paragraph.removed[index] != 0
                ? bidiRemovedLevel
                : static_cast<std::uint8_t>(paragraph.levels[index]);

    return result;
}

Vector<int> bidiVisualOrder(const BidiLevels& levels)
{
    auto order = Vector<int> {};

    for (auto index = 0; index < levels.levels.size(); ++index)
        if (levels.levels[index] != bidiRemovedLevel)
            order.add(index);

    reverseByLevel(order, [&](int index) { return (int) levels.levels[index]; });

    return order;
}

bool isLeftToRightOnly(std::string_view text)
{
    auto index = 0;

    while (index < (int) text.size())
    {
        const auto value = decodeUtf8(text, index);

        if (value < 0x0590)
            continue;

        switch (bidiClassOf(value))
        {
            case BidiClass::R:
            case BidiClass::AL:
            case BidiClass::AN:
            case BidiClass::LRE:
            case BidiClass::LRO:
            case BidiClass::RLE:
            case BidiClass::RLO:
            case BidiClass::PDF:
            case BidiClass::LRI:
            case BidiClass::RLI:
            case BidiClass::FSI:
            case BidiClass::PDI:
                return false;

            default:
                break;
        }
    }

    return true;
}

Vector<BidiRun> bidiRuns(std::string_view text, BidiBaseDirection base)
{
    auto runs = Vector<BidiRun> {};

    if (text.empty())
        return runs;

    if (base != BidiBaseDirection::RightToLeft && isLeftToRightOnly(text))
    {
        runs.add({0, (int) text.size(), 0});
        return runs;
    }

    auto codepoints = Vector<char32_t> {};
    auto starts = Vector<int> {};
    auto index = 0;

    while (index < (int) text.size())
    {
        starts.add(index);
        codepoints.add(decodeUtf8(text, index));
    }

    starts.add((int) text.size());

    const auto paragraph = analyse(codepoints, base);
    auto levels = Vector<int>(paragraph.size());

    // A codepoint X9 removed still occupies bytes the shaper must be handed,
    // so it joins the run before it rather than becoming a run of its own.
    auto carried = paragraph.paragraphLevel;

    for (auto at = 0; at < paragraph.size(); ++at)
    {
        if (paragraph.removed[at] == 0)
            carried = paragraph.levels[at];

        levels[at] = carried;
    }

    for (auto at = 0; at < paragraph.size(); ++at)
    {
        if (!runs.empty() && runs.back().level == levels[at])
            runs.back().end = starts[at + 1];
        else
            runs.add({starts[at], starts[at + 1], levels[at]});
    }

    auto order = Vector<int> {};

    for (auto at = 0; at < runs.size(); ++at)
        order.add(at);

    reverseByLevel(order, [&](int run) { return runs[run].level; });

    auto visual = Vector<BidiRun> {};

    for (const auto run: order)
        visual.add(runs[run]);

    return visual;
}
} // namespace eacp::Text
