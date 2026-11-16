#include "Common.h"

#include <NanoTest/NanoTest.h>

#include <string>

// What an editor does to its own string, which is the half that has nothing to
// do with drawing.
//
// Caret arithmetic is where a text field is actually wrong: a caret left inside
// a UTF-8 sequence splits a character the next time the string is cut at it, and
// a selection that forgets which end it grew from deletes the wrong side. Both
// are invisible until someone types an accent, so they are pinned here rather
// than tried by hand.
//
// The editor is put in a host so it can hold focus and measure text; nothing is
// rendered.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
KeyEvent typed(const std::string& characters)
{
    return keyOf(0, characters);
}

KeyEvent shifted(std::uint16_t code)
{
    auto event = keyOf(code);
    event.modifiers.shift = true;

    return event;
}

KeyEvent withModifiers(std::uint16_t code, ModifierKeys modifiers)
{
    auto event = keyOf(code);
    event.modifiers = modifiers;

    return event;
}

// What moves by word: Option on Apple, Control everywhere else.
ModifierKeys byWord(bool shift = false)
{
    if constexpr (Platform::isApple())
        return {.shift = shift, .alt = true};

    return {.shift = shift, .control = true};
}

struct TextEditorHarness
{
    TextEditorHarness(std::string startingText = {})
        : editor(std::move(startingText))
    {
        host.setBounds({0.f, 0.f, 300.f, 40.f});
        host.setRootComponent(root);

        root.setBounds({0.f, 0.f, 300.f, 40.f});
        root.addAndMakeVisible(editor);
        editor.setBounds({0.f, 0.f, 300.f, 30.f});

        editor.grabKeyboardFocus();
    }

    void press(const KeyEvent& event) { host.keyDown(event); }

    ComponentHost host;
    Component root;
    TextEditor editor;
};

// A two-byte character, so anything treating the string as bytes shows up.
const auto accented = std::string {"café"};
} // namespace

auto tTypingInserts = test("TextEditor/typingInsertsAtTheCaret") = []
{
    auto harness = TextEditorHarness {};

    harness.press(typed("a"));
    harness.press(typed("b"));

    check(harness.editor.getText() == "ab");
    check(harness.editor.getCaretPosition() == 2);
};

auto tTypingIsReported = test("TextEditor/everyEditIsReportedOnce") = []
{
    auto harness = TextEditorHarness {};
    auto changes = 0;

    harness.editor.onTextChange = [&changes](const std::string&) { ++changes; };

    harness.press(typed("a"));
    harness.press(typed("b"));

    check(changes == 2);
};

auto tControlCharactersAreNotTyped =
    test("TextEditor/aControlCharacterIsNotInsertedAsText") = []
{
    auto harness = TextEditorHarness {};

    // Tab produces a character, and an editor that inserted it would swallow
    // the key that moves focus out of the field.
    check(!harness.editor.keyDown(keyOf(eacp::Graphics::KeyCode::Tab, "\t")));
    check(harness.editor.getText().empty());
};

auto tBackspaceDeletesBackwards =
    test("TextEditor/backspaceTakesTheCharacterBefore") = []
{
    auto harness = TextEditorHarness {"abc"};

    harness.press(keyOf(eacp::Graphics::KeyCode::Delete));

    check(harness.editor.getText() == "ab");
    check(harness.editor.getCaretPosition() == 2);
};

auto tForwardDeleteTakesTheOneAfter =
    test("TextEditor/forwardDeleteTakesTheCharacterAfter") = []
{
    auto harness = TextEditorHarness {"abc"};

    harness.editor.setCaretPosition(0);
    harness.press(keyOf(eacp::Graphics::KeyCode::ForwardDelete));

    check(harness.editor.getText() == "bc");
    check(harness.editor.getCaretPosition() == 0);
};

auto tBackspaceAtTheStartDoesNothing =
    test("TextEditor/backspaceAtTheStartIsNotAnEdit") = []
{
    auto harness = TextEditorHarness {"abc"};
    auto changes = 0;

    harness.editor.onTextChange = [&changes](const std::string&) { ++changes; };
    harness.editor.setCaretPosition(0);
    harness.press(keyOf(eacp::Graphics::KeyCode::Delete));

    check(harness.editor.getText() == "abc");
    check(changes == 0, "and reports nothing, so a listener sees only real edits");
};

// The whole reason the caret is moved a sequence at a time: one backspace takes
// one character, not one byte of one.
auto tDeletesWholeCharacters =
    test("TextEditor/backspaceTakesAWholeUtf8Character") = []
{
    auto harness = TextEditorHarness {accented};

    harness.press(keyOf(eacp::Graphics::KeyCode::Delete));

    check(harness.editor.getText() == "cafe");
};

auto tCaretNeverLandsInsideACharacter =
    test("TextEditor/theCaretIsNeverLeftInsideASequence") = []
{
    auto harness = TextEditorHarness {accented};

    // Straight into the middle of the two-byte accent, which is what a click
    // measured against the wrong metrics would ask for.
    harness.editor.setCaretPosition((int) accented.size() - 1);

    check(harness.editor.getCaretPosition() == (int) accented.size() - 2);
};

auto tArrowsMoveByCharacter = test("TextEditor/arrowsMoveByCharacterNotByByte") = []
{
    auto harness = TextEditorHarness {accented};

    harness.editor.setCaretPosition((int) accented.size());
    harness.press(keyOf(eacp::Graphics::KeyCode::LeftArrow));

    check(harness.editor.getCaretPosition() == (int) accented.size() - 2);

    harness.press(keyOf(eacp::Graphics::KeyCode::RightArrow));

    check(harness.editor.getCaretPosition() == (int) accented.size());
};

auto tHomeAndEnd = test("TextEditor/homeAndEndGoToTheEnds") = []
{
    auto harness = TextEditorHarness {"abc"};

    harness.press(keyOf(eacp::Graphics::KeyCode::Home));
    check(harness.editor.getCaretPosition() == 0);

    harness.press(keyOf(eacp::Graphics::KeyCode::End));
    check(harness.editor.getCaretPosition() == 3);
};

auto tShiftArrowSelects = test("TextEditor/shiftAndAnArrowGrowsASelection") = []
{
    auto harness = TextEditorHarness {"abc"};

    check(!harness.editor.hasSelection());

    harness.press(shifted(eacp::Graphics::KeyCode::LeftArrow));

    check(harness.editor.hasSelection());
    check(harness.editor.getSelectedText() == "c");

    harness.press(shifted(eacp::Graphics::KeyCode::LeftArrow));

    check(harness.editor.getSelectedText() == "bc");
};

// A selection grown leftwards has its caret at the *left* end, so what it
// replaces is still the range between them and not one side of it.
auto tTypingReplacesASelection =
    test("TextEditor/typingOverASelectionReplacesIt") = []
{
    auto harness = TextEditorHarness {"abc"};

    harness.press(shifted(eacp::Graphics::KeyCode::LeftArrow));
    harness.press(shifted(eacp::Graphics::KeyCode::LeftArrow));
    harness.press(typed("Z"));

    check(harness.editor.getText() == "aZ");
    check(harness.editor.getCaretPosition() == 2);
    check(!harness.editor.hasSelection(), "and the selection is gone with it");
};

auto tBackspaceDeletesTheSelection =
    test("TextEditor/backspaceOverASelectionTakesAllOfIt") = []
{
    auto harness = TextEditorHarness {"abcdef"};

    harness.editor.selectAll();
    harness.press(keyOf(eacp::Graphics::KeyCode::Delete));

    check(harness.editor.getText().empty());
};

auto tSelectAll = test("TextEditor/selectAllTakesTheWholeString") = []
{
    auto harness = TextEditorHarness {"abc"};

    harness.editor.selectAll();

    check(harness.editor.getSelectedText() == "abc");

    harness.editor.deselect();

    check(!harness.editor.hasSelection());
};

auto tSettingTextMovesTheCaretToTheEnd =
    test("TextEditor/settingTheTextPutsTheCaretAtTheEnd") = []
{
    auto harness = TextEditorHarness {"abc"};

    harness.editor.selectAll();
    harness.editor.setText("longer text");

    check(harness.editor.getCaretPosition() == 11);
    check(!harness.editor.hasSelection());
};

auto tSetTextCanNotify = test("TextEditor/settingTheTextReportsOnlyWhenAsked") = []
{
    auto harness = TextEditorHarness {};
    auto changes = 0;

    harness.editor.onTextChange = [&changes](const std::string&) { ++changes; };

    harness.editor.setText("quiet");
    check(changes == 0);

    harness.editor.setText("loud", true);
    check(changes == 1);
};

auto tReturnAndEscape =
    test("TextEditor/returnAndEscapeAreReportedRatherThanTyped") = []
{
    auto harness = TextEditorHarness {"abc"};

    auto committed = std::string {};
    auto escapes = 0;

    harness.editor.onReturnKey = [&committed](const std::string& value)
    { committed = value; };
    harness.editor.onEscapeKey = [&escapes] { ++escapes; };

    harness.press(keyOf(eacp::Graphics::KeyCode::Return));
    harness.press(keyOf(eacp::Graphics::KeyCode::Escape));

    check(committed == "abc");
    check(escapes == 1);
    check(harness.editor.getText() == "abc", "and neither changed the text");
};

auto tEscapeIsKept = test("TextEditor/escapeIsKeptAndTheEditorStaysFocused") = []
{
    auto harness = TextEditorHarness {"abc"};

    auto escapes = 0;
    harness.editor.onEscapeKey = [&escapes] { ++escapes; };

    check(harness.host.getProperties().wantsTextInput);
    check(harness.host.dispatchKeyEvent(typed("d")), "typing is kept");

    check(harness.host.dispatchKeyEvent(keyOf(eacp::Graphics::KeyCode::Escape)),
          "escape is the editor's");
    check(escapes == 1);
    check(harness.editor.hasKeyboardFocus());
    check(harness.host.getProperties().wantsTextInput);
    check(harness.editor.getText() == "abcd");
};

// Android's soft keyboard takes the first Back itself and goes away; the second
// reaches the app, and an editor that kept it would be why the app could never
// be left.
auto tBackPassesOn = test("TextEditor/backIsPassedOnByAFocusedEditor") = []
{
    auto harness = TextEditorHarness {"abc"};

    auto escapes = 0;
    harness.editor.onEscapeKey = [&escapes] { ++escapes; };

    check(!harness.host.dispatchKeyEvent(keyOf(eacp::Graphics::KeyCode::Back)),
          "back is passed on");
    check(escapes == 0, "and is not an escape");
    check(harness.editor.getText() == "abc");
};

auto tReadOnlyRefusesEdits = test("TextEditor/aReadOnlyEditorTakesNoEdits") = []
{
    auto harness = TextEditorHarness {"abc"};

    harness.editor.setReadOnly(true);

    harness.press(typed("z"));
    harness.press(keyOf(eacp::Graphics::KeyCode::Delete));

    check(harness.editor.getText() == "abc");

    // Still navigable, which is what makes read-only different from disabled.
    harness.press(keyOf(eacp::Graphics::KeyCode::Home));
    check(harness.editor.getCaretPosition() == 0);
};

// An editor that swallowed Cmd+S would be why a document could not be saved
// while a field had focus.
auto tUnknownShortcutsAreLeftAlone =
    test("TextEditor/aShortcutTheEditorDoesNotUseCarriesOn") = []
{
    auto harness = TextEditorHarness {"abc"};

    auto event = typed("s");
    event.modifiers.command = true;

    check(!harness.editor.keyDown(event));
    check(harness.editor.getText() == "abc");
};

auto tSelectAllShortcut = test("TextEditor/theSelectAllShortcutIsTaken") = []
{
    auto harness = TextEditorHarness {"abc"};

    auto event = typed("a");
    event.modifiers.command = true;

    check(harness.editor.keyDown(event));
    check(harness.editor.getSelectedText() == "abc");
};

// Focus is what the caret and the selection are drawn from, so losing it has to
// leave the editor in the state it will be painted in.
auto tLosingFocusDropsTheSelection =
    test("TextEditor/losingFocusDropsTheSelection") = []
{
    auto harness = TextEditorHarness {"abc"};

    harness.editor.selectAll();
    harness.editor.giveAwayKeyboardFocus();

    check(!harness.editor.hasSelection());
    check(harness.editor.getText() == "abc");
};

auto tPasswordMask =
    test("TextEditor/aPasswordFieldKeepsTheTextAndDrawsTheMask") = []
{
    auto harness = TextEditorHarness {};

    harness.editor.setPasswordCharacter("\xE2\x80\xA2");
    harness.press(typed("a"));
    harness.press(typed(accented));

    check(harness.editor.getText() == "a" + accented, "the value is what was typed");
    check(harness.editor.getCaretPosition() == 1 + (int) accented.size());

    harness.press(keyOf(KeyCode::LeftArrow));
    check(harness.editor.getCaretPosition() == 1 + (int) accented.size() - 2,
          "and the caret still moves by the text's own characters");

    if (!GPU::Device::shared().isValid())
        return;

    auto list = DrawList {};
    auto ramps = GradientRamps {};
    auto text = Text::TextRenderer {};
    auto g = UI::Graphics {list, ramps, text, harness.editor.getLocalBounds()};

    harness.editor.paint(g);

    const auto& glyphs = list.getGlyphs();
    auto characters = 0;

    for (auto c: harness.editor.getText())
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80)
            ++characters;

    check(glyphs.size() == characters, "one glyph per character");

    auto allTheSame = true;

    for (const auto& glyph: glyphs)
        allTheSame =
            allTheSame && glyph.key == glyphs[0].key && glyph.face == glyphs[0].face;

    check(allTheSame, "and every one is the mask, not the letters typed");
};

auto tFrameless = test("TextEditor/anEditorWithoutAFrameDrawsOnlyItsText") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto harness = TextEditorHarness {};
    harness.editor.setText("abc");
    harness.editor.giveAwayKeyboardFocus();

    auto record = [&]
    {
        auto list = DrawList {};
        auto ramps = GradientRamps {};
        auto text = Text::TextRenderer {};
        auto g = UI::Graphics {list, ramps, text, harness.editor.getLocalBounds()};

        harness.editor.paint(g);

        return list.getShapes().size();
    };

    check(record() == 2, "the panel and its outline");

    harness.editor.setDrawsFrame(false);
    check(record() == 0, "and neither without the frame");
};

auto tFocusIsReported = test("TextEditor/takingAndLosingTheKeyboardIsReported") = []
{
    auto harness = TextEditorHarness {};
    auto reported = Vector<int> {};

    harness.editor.onFocusChange = [&](bool focused) { reported.add(focused); };

    harness.editor.giveAwayKeyboardFocus();
    harness.editor.grabKeyboardFocus();

    check(reported.size() == 2 && !reported[0] && reported[1]);
};

auto tCommandArrowsGoToTheEnds =
    test("TextEditor/commandAndAnArrowGoesToTheEndOfTheLine") = []
{
    if constexpr (!Platform::isApple())
        return;

    auto harness = TextEditorHarness {"hello world"};

    harness.editor.setCaretPosition(5);
    harness.press(withModifiers(KeyCode::LeftArrow, {.command = true}));
    check(harness.editor.getCaretPosition() == 0);

    harness.press(withModifiers(KeyCode::RightArrow, {.command = true}));
    check(harness.editor.getCaretPosition() == 11);

    harness.press(
        withModifiers(KeyCode::LeftArrow, {.shift = true, .command = true}));
    check(harness.editor.getSelectedText() == "hello world",
          "and with Shift it selects the way it went");
};

auto tCommandDeleteTakesTheLine =
    test("TextEditor/commandAndDeleteTakesTheLineToOneSide") = []
{
    if constexpr (!Platform::isApple())
        return;

    auto harness = TextEditorHarness {"hello world"};

    harness.editor.setCaretPosition(5);
    harness.press(withModifiers(KeyCode::Delete, {.command = true}));

    check(harness.editor.getText() == " world");
    check(harness.editor.getCaretPosition() == 0);

    harness.editor.setText("hello world");
    harness.editor.setCaretPosition(5);
    harness.press(withModifiers(KeyCode::ForwardDelete, {.command = true}));

    check(harness.editor.getText() == "hello");
    check(harness.editor.getCaretPosition() == 5);
};

auto tCommandDeleteTakesOnlyASelection =
    test("TextEditor/commandAndDeleteOverASelectionTakesOnlyTheSelection") = []
{
    if constexpr (!Platform::isApple())
        return;

    auto harness = TextEditorHarness {"hello world"};

    harness.press(shifted(KeyCode::LeftArrow));
    harness.press(withModifiers(KeyCode::Delete, {.command = true}));

    check(harness.editor.getText() == "hello worl");
};

auto tTheWindowsKeyIsNotCommand =
    test("TextEditor/theWindowsKeyIsNotTakenForCommand") = []
{
    if constexpr (Platform::isApple())
        return;

    auto harness = TextEditorHarness {"hello world"};

    harness.editor.setCaretPosition(5);

    check(!harness.editor.keyDown(
        withModifiers(KeyCode::LeftArrow, {.command = true})));
    check(
        !harness.editor.keyDown(withModifiers(KeyCode::Delete, {.command = true})));
    check(harness.editor.getText() == "hello world");
    check(harness.editor.getCaretPosition() == 5);
};

auto tArrowsMoveByWord = test("TextEditor/wordArrowsStopAtEachWord") = []
{
    // Accented, so a boundary is never found inside a character, and with
    // punctuation between words, which is skipped rather than stopped at.
    auto harness = TextEditorHarness {"caf\xC3\xA9 au-lait"};

    harness.press(withModifiers(KeyCode::LeftArrow, byWord()));
    check(harness.editor.getCaretPosition() == 9);

    harness.press(withModifiers(KeyCode::LeftArrow, byWord()));
    check(harness.editor.getCaretPosition() == 6);

    harness.press(withModifiers(KeyCode::LeftArrow, byWord()));
    check(harness.editor.getCaretPosition() == 0);

    harness.press(withModifiers(KeyCode::RightArrow, byWord()));
    check(harness.editor.getCaretPosition() == 5);

    harness.press(withModifiers(KeyCode::RightArrow, byWord()));
    check(harness.editor.getCaretPosition() == 8);

    harness.press(withModifiers(KeyCode::RightArrow, byWord(true)));
    check(harness.editor.getSelectedText() == "-lait");
};

auto tDeleteTakesAWord = test("TextEditor/wordDeleteTakesTheWordBeforeTheCaret") = []
{
    auto harness = TextEditorHarness {"hello, world"};

    harness.press(withModifiers(KeyCode::Delete, byWord()));
    check(harness.editor.getText() == "hello, ");

    harness.press(withModifiers(KeyCode::Delete, byWord()));
    check(harness.editor.getText().empty());
};

auto tPasswordWordsAreTheLine =
    test("TextEditor/aPasswordFieldMovesByWordToTheEnds") = []
{
    auto harness = TextEditorHarness {"hello world"};

    harness.editor.setPasswordCharacter("*");
    harness.press(withModifiers(KeyCode::LeftArrow, byWord()));

    check(harness.editor.getCaretPosition() == 0);
};

namespace
{
// Command on Apple, Control everywhere else: what undo and redo are held with.
KeyEvent undoKey(bool shift = false)
{
    auto event = typed(shift ? "Z" : "z");
    event.keyCode = KeyCode::Z;
    event.modifiers.shift = shift;

    if constexpr (Platform::isApple())
        event.modifiers.command = true;
    else
        event.modifiers.control = true;

    return event;
}

KeyEvent redoKey()
{
    return undoKey(true);
}

void typeEach(TextEditorHarness& harness, const std::string& characters)
{
    for (auto c: characters)
        harness.press(typed(std::string(1, c)));
}
} // namespace

auto tUndoAndRedo = test("TextEditor/undoAndRedoStepThroughTheEdits") = []
{
    auto harness = TextEditorHarness {"abc"};

    harness.editor.setCaretPosition(1);
    harness.press(shifted(KeyCode::RightArrow));
    harness.press(keyOf(KeyCode::Delete));

    check(harness.editor.getText() == "ac");

    check(harness.editor.keyDown(undoKey()));
    check(harness.editor.getText() == "abc");
    check(harness.editor.getSelectedText() == "b",
          "undo puts back the selection the edit was made over");

    check(harness.editor.keyDown(redoKey()));
    check(harness.editor.getText() == "ac");
    check(harness.editor.getCaretPosition() == 1);
};

auto tNothingToUndoCarriesOn =
    test("TextEditor/undoWithNothingToUndoCarriesOnToTheWindow") = []
{
    auto harness = TextEditorHarness {"abc"};

    check(!harness.editor.keyDown(undoKey()));
    check(!harness.editor.keyDown(redoKey()));
    check(harness.editor.getText() == "abc");
};

auto tTypingCoalesces = test("TextEditor/typingUndoesAWordAtATime") = []
{
    auto harness = TextEditorHarness {};

    typeEach(harness, "hello world");

    harness.press(undoKey());
    check(harness.editor.getText() == "hello ");

    harness.press(undoKey());
    check(harness.editor.getText().empty());
    check(!harness.editor.canUndo());
};

auto tMovingTheCaretStartsAStep =
    test("TextEditor/movingTheCaretOrSwitchingToDeletingStartsAStep") = []
{
    auto harness = TextEditorHarness {};

    typeEach(harness, "abc");
    harness.press(keyOf(KeyCode::LeftArrow));
    typeEach(harness, "xy");

    check(harness.editor.getText() == "abxyc");

    harness.press(keyOf(KeyCode::Delete));
    harness.press(keyOf(KeyCode::Delete));

    check(harness.editor.getText() == "abc");

    harness.press(undoKey());
    check(harness.editor.getText() == "abxyc", "the deletes are one step");

    harness.press(undoKey());
    check(harness.editor.getText() == "abc", "the typing after the move another");

    harness.press(undoKey());
    check(harness.editor.getText().empty());
};

auto tLineKeysAreSteps = test("TextEditor/aLineOrWordDeleteIsAStepOfItsOwn") = []
{
    auto harness = TextEditorHarness {};

    typeEach(harness, "abc");
    harness.press(keyOf(KeyCode::Delete));
    harness.press(withModifiers(KeyCode::Delete, byWord()));

    check(harness.editor.getText().empty());

    harness.press(undoKey());
    check(harness.editor.getText() == "ab");
};

auto tANewEditDropsRedo = test("TextEditor/aNewEditDropsTheRedo") = []
{
    auto harness = TextEditorHarness {};

    typeEach(harness, "ab");
    harness.press(undoKey());
    check(harness.editor.canRedo());

    harness.press(typed("c"));

    check(!harness.editor.canRedo());
    check(!harness.editor.keyDown(redoKey()));
    check(harness.editor.getText() == "c");
};

auto tCommitClearsUndo = test("TextEditor/returnAndLosingFocusEndTheHistory") = []
{
    auto harness = TextEditorHarness {};

    typeEach(harness, "ab");
    harness.press(keyOf(KeyCode::Return));

    check(!harness.editor.canUndo(), "Return commits");

    harness.press(typed("c"));
    harness.editor.giveAwayKeyboardFocus();

    check(!harness.editor.canUndo(), "and so does losing focus");
    check(harness.editor.getText() == "abc");
};

auto tSetTextClearsUndo = test("TextEditor/setTextStartsANewHistory") = []
{
    auto harness = TextEditorHarness {};

    typeEach(harness, "ab");
    harness.press(undoKey());
    harness.editor.setText("other");

    check(!harness.editor.canUndo());
    check(!harness.editor.canRedo());
};

auto tApplyEditIsUndoable = test("TextEditor/anAppliedEditUndoesLikeTyping") = []
{
    auto harness = TextEditorHarness {"hello world"};

    harness.editor.setCaretPosition(5);
    harness.editor.applyEdit("hello", 2);

    check(harness.editor.getText() == "hello");
    check(harness.editor.getCaretPosition() == 2);

    harness.press(undoKey());
    check(harness.editor.getText() == "hello world");
    check(harness.editor.getCaretPosition() == 5);

    harness.press(redoKey());
    check(harness.editor.getText() == "hello");
};

auto tClearUndo = test("TextEditor/clearUndoIsTheAppsOwnCommit") = []
{
    auto harness = TextEditorHarness {};

    typeEach(harness, "ab");
    harness.editor.clearUndo();

    check(!harness.editor.canUndo());
    check(harness.editor.getText() == "ab");
};
