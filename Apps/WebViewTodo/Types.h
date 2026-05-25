#pragma once

#include <Miro/Miro.h>

#include <string>
#include <utility>

struct TodoItem
{
    long long id = 0;
    std::string text;
    bool completed = false;

    MIRO_REFLECT(id, text, completed)
};

struct TodoState
{
    EA::Vector<TodoItem> items;

    MIRO_REFLECT(items)
};

struct AddTodoRequest
{
    std::string text;

    MIRO_REFLECT(text)
};

struct TodoIdRequest
{
    long long id = 0;

    MIRO_REFLECT(id)
};

struct EditTodoRequest
{
    long long id = 0;
    std::string text;

    MIRO_REFLECT(id, text)
};

namespace Api
{

namespace Detail
{
inline std::string trim(const std::string& input)
{
    auto start = input.find_first_not_of(" \t\n");
    if (start == std::string::npos)
        return {};

    auto end = input.find_last_not_of(" \t\n");
    return input.substr(start, end - start + 1);
}
} // namespace Detail

// Replaces (TodoStore singleton + EACP_KEYED_STATE + the six free-fn
// commands + MIRO_EXPORT_COMMANDS). reflect() lists the surface; each
// method mutates the state and publishes a new snapshot.
//
// All method bodies are inline because the codegen executable ODR-uses
// these pmfs through the makePmfHandler lambda chain.
class TodosApi
{
public:
    TodosApi()
    {
        auto seed = TodoState {};
        seed.items.create(nextId++, "Try editing me (double-click)", false);
        seed.items.create(nextId++, "Toggle a checkbox", false);
        seed.items.create(nextId++, "Add a new todo above", true);
        todos.publish(std::move(seed));
    }

    void reflect(Miro::ApiReflector& r)
    {
        using T = TodosApi;
        r.commands<&T::getTodos,
                   &T::addTodo,
                   &T::toggleTodo,
                   &T::editTodo,
                   &T::removeTodo,
                   &T::clearCompleted>();
        // keyedEvent matches the old EACP_KEYED_STATE: tells the hooks
        // codegen that this state's payload is a collection (items)
        // indexed by id, so useTodos / useTodoIds / useTodoItem get
        // emitted with per-id selector semantics.
        r.keyedEvent(&T::todos, "todos", "items", "id");
    }

    TodoState getTodos() const { return todos.snapshot(); }

    void addTodo(const AddTodoRequest& req)
    {
        auto trimmed = Detail::trim(req.text);
        if (trimmed.empty())
            return;

        auto next = todos.snapshot();
        next.items.create(nextId++, std::move(trimmed), false);
        todos.publish(std::move(next));
    }

    void toggleTodo(const TodoIdRequest& req)
    {
        auto next = todos.snapshot();
        for (auto& item: next.items)
        {
            if (item.id == req.id)
            {
                item.completed = !item.completed;
                todos.publish(std::move(next));
                return;
            }
        }
    }

    void editTodo(const EditTodoRequest& req)
    {
        auto next = todos.snapshot();
        for (auto& item: next.items)
        {
            if (item.id == req.id)
            {
                item.text = req.text;
                todos.publish(std::move(next));
                return;
            }
        }
    }

    void removeTodo(const TodoIdRequest& req)
    {
        auto next = todos.snapshot();
        auto erased = next.items.eraseIf([&](const TodoItem& item)
                                         { return item.id == req.id; });
        if (erased)
            todos.publish(std::move(next));
    }

    void clearCompleted()
    {
        auto next = todos.snapshot();
        auto erased =
            next.items.eraseIf([](const TodoItem& item) { return item.completed; });
        if (erased)
            todos.publish(std::move(next));
    }

    Miro::Event<TodoState> todos;

private:
    long long nextId = 1;
};

} // namespace Api
