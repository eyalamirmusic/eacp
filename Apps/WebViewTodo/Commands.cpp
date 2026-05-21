#include "Types.h"

#include <utility>

namespace
{
long long nextTodoId = 1;

TodoState makeInitialState()
{
    auto state = TodoState {};
    state.items.push_back({nextTodoId++, "Try editing me (double-click)", false});
    state.items.push_back({nextTodoId++, "Toggle a checkbox", false});
    state.items.push_back({nextTodoId++, "Add a new todo above", true});
    return state;
}
} // namespace

eacp::StateValue<TodoState>& todoState()
{
    static auto state = eacp::StateValue<TodoState> {makeInitialState()};
    return state;
}

TodoState getTodos()
{
    return todoState().get();
}

void addTodo(const AddTodoRequest& req)
{
    auto trimmedStart = req.text.find_first_not_of(" \t\n");

    if (trimmedStart == std::string::npos)
        return;

    auto trimmedEnd = req.text.find_last_not_of(" \t\n");
    auto text = req.text.substr(trimmedStart, trimmedEnd - trimmedStart + 1);

    todoState().modify(
        [&](TodoState& s)
        { s.items.push_back({nextTodoId++, std::move(text), false}); });
}

void toggleTodo(const TodoIdRequest& req)
{
    todoState().modify(
        [&](TodoState& s)
        {
            for (auto& item: s.items)
            {
                if (item.id == req.id)
                {
                    item.completed = !item.completed;
                    return;
                }
            }
        });
}

void editTodo(const EditTodoRequest& req)
{
    todoState().modify(
        [&](TodoState& s)
        {
            for (auto& item: s.items)
            {
                if (item.id == req.id)
                {
                    item.text = req.text;
                    return;
                }
            }
        });
}

void removeTodo(const TodoIdRequest& req)
{
    todoState().modify(
        [&](TodoState& s)
        {
            std::erase_if(s.items,
                          [&](const TodoItem& item) { return item.id == req.id; });
        });
}

void clearCompleted()
{
    todoState().modify(
        [](TodoState& s)
        {
            std::erase_if(s.items,
                          [](const TodoItem& item) { return item.completed; });
        });
}

MIRO_EXPORT_COMMANDS(getTodos,
                     addTodo,
                     toggleTodo,
                     editTodo,
                     removeTodo,
                     clearCompleted)

MIRO_EXPORT_EVENT(todos, TodoState)
