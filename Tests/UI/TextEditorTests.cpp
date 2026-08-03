#include <eacp/UI/UI.h>

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
KeyEvent keyOf(std::uint16_t code, const std::string& characters = {})
{
    auto event = KeyEvent {};

    event.keyCode = code;
    event.characters = characters;
    event.charactersIgnoringModifiers = characters;

    return event;
}

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

struct Harness
{
    Harness(std::string startingText = {})
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
    auto harness = Harness {};

    harness.press(typed("a"));
    harness.press(typed("b"));

    check(harness.editor.getText() == "ab");
    check(harness.editor.getCaretPosition() == 2);
};

auto tTypingIsReported = test("TextEditor/everyEditIsReportedOnce") = []
{
    auto harness = Harness {};
    auto changes = 0;

    harness.editor.onTextChange = [&changes](const std::string&) { ++changes; };

    harness.press(typed("a"));
    harness.press(typed("b"));

    check(changes == 2);
};

auto tControlCharactersAreNotTyped =
    test("TextEditor/aControlCharacterIsNotInsertedAsText") = []
{
    auto harness = Harness {};

    // Tab produces a character, and an editor that inserted it would swallow
    // the key that moves focus out of the field.
    check(!harness.editor.keyDown(keyOf(eacp::Graphics::KeyCode::Tab, "\t")));
    check(harness.editor.getText().empty());
};

auto tBackspaceDeletesBackwards =
    test("TextEditor/backspaceTakesTheCharacterBefore") = []
{
    auto harness = Harness {"abc"};

    harness.press(keyOf(eacp::Graphics::KeyCode::Delete));

    check(harness.editor.getText() == "ab");
    check(harness.editor.getCaretPosition() == 2);
};

auto tForwardDeleteTakesTheOneAfter =
    test("TextEditor/forwardDeleteTakesTheCharacterAfter") = []
{
    auto harness = Harness {"abc"};

    harness.editor.setCaretPosition(0);
    harness.press(keyOf(eacp::Graphics::KeyCode::ForwardDelete));

    check(harness.editor.getText() == "bc");
    check(harness.editor.getCaretPosition() == 0);
};

auto tBackspaceAtTheStartDoesNothing =
    test("TextEditor/backspaceAtTheStartIsNotAnEdit") = []
{
    auto harness = Harness {"abc"};
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
    auto harness = Harness {accented};

    harness.press(keyOf(eacp::Graphics::KeyCode::Delete));

    check(harness.editor.getText() == "cafe");
};

auto tCaretNeverLandsInsideACharacter =
    test("TextEditor/theCaretIsNeverLeftInsideASequence") = []
{
    auto harness = Harness {accented};

    // Straight into the middle of the two-byte accent, which is what a click
    // measured against the wrong metrics would ask for.
    harness.editor.setCaretPosition((int) accented.size() - 1);

    check(harness.editor.getCaretPosition() == (int) accented.size() - 2);
};

auto tArrowsMoveByCharacter = test("TextEditor/arrowsMoveByCharacterNotByByte") = []
{
    auto harness = Harness {accented};

    harness.editor.setCaretPosition((int) accented.size());
    harness.press(keyOf(eacp::Graphics::KeyCode::LeftArrow));

    check(harness.editor.getCaretPosition() == (int) accented.size() - 2);

    harness.press(keyOf(eacp::Graphics::KeyCode::RightArrow));

    check(harness.editor.getCaretPosition() == (int) accented.size());
};

auto tHomeAndEnd = test("TextEditor/homeAndEndGoToTheEnds") = []
{
    auto harness = Harness {"abc"};

    harness.press(keyOf(eacp::Graphics::KeyCode::Home));
    check(harness.editor.getCaretPosition() == 0);

    harness.press(keyOf(eacp::Graphics::KeyCode::End));
    check(harness.editor.getCaretPosition() == 3);
};

auto tShiftArrowSelects = test("TextEditor/shiftAndAnArrowGrowsASelection") = []
{
    auto harness = Harness {"abc"};

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
    auto harness = Harness {"abc"};

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
    auto harness = Harness {"abcdef"};

    harness.editor.selectAll();
    harness.press(keyOf(eacp::Graphics::KeyCode::Delete));

    check(harness.editor.getText().empty());
};

auto tSelectAll = test("TextEditor/selectAllTakesTheWholeString") = []
{
    auto harness = Harness {"abc"};

    harness.editor.selectAll();

    check(harness.editor.getSelectedText() == "abc");

    harness.editor.deselect();

    check(!harness.editor.hasSelection());
};

auto tSettingTextMovesTheCaretToTheEnd =
    test("TextEditor/settingTheTextPutsTheCaretAtTheEnd") = []
{
    auto harness = Harness {"abc"};

    harness.editor.selectAll();
    harness.editor.setText("longer text");

    check(harness.editor.getCaretPosition() == 11);
    check(!harness.editor.hasSelection());
};

auto tSetTextCanNotify = test("TextEditor/settingTheTextReportsOnlyWhenAsked") = []
{
    auto harness = Harness {};
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
    auto harness = Harness {"abc"};

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

auto tReadOnlyRefusesEdits = test("TextEditor/aReadOnlyEditorTakesNoEdits") = []
{
    auto harness = Harness {"abc"};

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
    auto harness = Harness {"abc"};

    auto event = typed("s");
    event.modifiers.command = true;

    check(!harness.editor.keyDown(event));
    check(harness.editor.getText() == "abc");
};

auto tSelectAllShortcut = test("TextEditor/theSelectAllShortcutIsTaken") = []
{
    auto harness = Harness {"abc"};

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
    auto harness = Harness {"abc"};

    harness.editor.selectAll();
    harness.editor.giveAwayKeyboardFocus();

    check(!harness.editor.hasSelection());
    check(harness.editor.getText() == "abc");
};
