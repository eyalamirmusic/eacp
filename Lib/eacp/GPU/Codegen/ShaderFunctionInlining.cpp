#include "ShaderGraph.h"

#include <initializer_list>
#include <map>

// The CPU executor runs no call: every function's body is copied to where it
// is called. A copy is a Call statement holding the body, its parameters
// declared from the arguments, a Return in it assigning the result first. The
// functions' own variables are shared by every copy, which is safe because
// none recurses and each copy declares them again; a call's result has a
// variable of its own, so two calls in one expression keep two values.

namespace eacp::GPU
{
class FunctionInliner
{
public:
    explicit FunctionInliner(ShaderGraph& graphToEdit)
        : graph(graphToEdit)
    {
    }

    void run() { rewriteBlock(ShaderGraph::rootBlock); }

private:
    using Substitutions = std::map<int, int>;

    int addStatement(const Statement& statement)
    {
        graph.statementList.add(statement);
        return graph.statementList.size() - 1;
    }

    int addBlock(BlockRole role)
    {
        graph.blocks.add(Block {{}, role});
        return graph.blocks.size() - 1;
    }

    void rewriteBlock(int block)
    {
        auto original = graph.blocks[block].statements;
        auto rewritten = Vector<int> {};

        for (auto index: original)
            rewriteStatement(index, rewritten);

        graph.blocks[block].statements = rewritten;
    }

    static bool isOriginalCall(const Statement& statement)
    {
        return statement.kind == StatementKind::Call && statement.body < 0;
    }

    void rewriteStatement(int index, Vector<int>& out)
    {
        auto statement = graph.statementList[index];

        if (isOriginalCall(statement))
        {
            rewriteCall(statement, out);
            return;
        }

        auto substitutions = Substitutions {};
        auto ahead = Vector<int> {};

        for (auto* field: {&statement.value,
                           &statement.index,
                           &statement.indexY,
                           &statement.layer,
                           &statement.stride,
                           &statement.compare,
                           &statement.record})
            *field = substitute(*field, ahead, substitutions);

        for (auto body: {statement.body, statement.elseBody, statement.continuing})
            if (body >= 0)
                rewriteBlock(body);

        if (statement.kind == StatementKind::Loop && !ahead.empty())
        {
            testConditionInBody(statement, ahead);
            ahead.clear();
        }

        for (auto added: ahead)
            out.add(added);

        graph.statementList[index] = statement;
        out.add(index);
    }

    // The calls a loop's condition reads run every pass, so they and the test
    // move to the top of the body and the loop itself tests true.
    void testConditionInBody(Statement& loop, const Vector<int>& calls)
    {
        auto exit = addBlock(BlockRole::Plain);
        graph.blocks[exit].statements.add(
            addStatement(Statement {StatementKind::Break}));

        auto test = Statement {StatementKind::If};
        test.value = graph.addUnary(ValueType::Bool, '!', loop.value);
        test.body = exit;

        auto top = calls;
        top.add(addStatement(test));

        for (auto existing: graph.blocks[loop.body].statements)
            top.add(existing);

        graph.blocks[loop.body].statements = top;
        loop.value = graph.addBoolConstant(true);
    }

    void rewriteCall(const Statement& statement, Vector<int>& out)
    {
        auto substitutions = Substitutions {};
        auto call = graph.nodes[statement.value];

        for (auto& argument: call.args)
            argument = substitute(argument, out, substitutions);

        inlineCall(call.index, call.args, statement.slot, out);
    }

    int substitute(int node, Vector<int>& out, Substitutions& substitutions)
    {
        if (node < 0)
            return node;

        if (auto found = substitutions.find(node); found != substitutions.end())
            return found->second;

        auto expr = graph.nodes[node];
        auto changed = false;

        for (auto& argument: expr.args)
        {
            auto replaced = substitute(argument, out, substitutions);
            changed = changed || replaced != argument;
            argument = replaced;
        }

        auto result = node;

        if (expr.kind == ExprKind::FunctionCall)
            result = graph.addVarRead(inlineCall(expr.index, expr.args, -1, out));
        else if (changed)
            result = graph.add(expr);

        substitutions[node] = result;
        return result;
    }

    int zeroOf(int variable)
    {
        auto structId = graph.variableStructs[variable];

        if (structId >= 0)
            return graph.addStructZero(structId);

        return graph.addZero(graph.variableTypes[variable]);
    }

    int addResultVariable(const FunctionDeclaration& declaration)
    {
        graph.variableTypes.add(declaration.returnType);
        graph.vertexVariables.add(char {0});
        graph.variableSizes.add(0);
        graph.variableStructs.add(declaration.returnStruct);
        graph.variableFunctions.add(-1);
        return graph.variableTypes.size() - 1;
    }

    int inlineCall(int function,
                   const Vector<int>& arguments,
                   int result,
                   Vector<int>& out)
    {
        auto declaration = graph.functionList[function];

        if (declaration.returnsValue && result < 0)
            result = addResultVariable(declaration);

        if (result >= 0)
        {
            auto declare = Statement {StatementKind::Declare};
            declare.slot = result;
            declare.value = zeroOf(result);
            out.add(addStatement(declare));
        }

        auto body = addBlock(BlockRole::Plain);

        for (auto which = 0; which < arguments.size(); ++which)
        {
            auto parameter = Statement {StatementKind::Declare};
            parameter.slot = declaration.parameters[which];
            parameter.value = arguments[which];
            graph.blocks[body].statements.add(addStatement(parameter));
        }

        copyStatements(declaration.body, body, result);
        rewriteBlock(body);

        auto call = Statement {StatementKind::Call};
        call.body = body;
        out.add(addStatement(call));
        return result;
    }

    int copyBlock(int source, int result)
    {
        auto copy = addBlock(graph.blocks[source].role);
        copyStatements(source, copy, result);
        return copy;
    }

    void copyStatements(int source, int target, int result)
    {
        auto statements = graph.blocks[source].statements;

        for (auto index: statements)
        {
            auto statement = graph.statementList[index];

            if (statement.kind == StatementKind::Return && statement.value >= 0)
            {
                auto assign = Statement {StatementKind::Assign};
                assign.slot = result;
                assign.value = statement.value;
                graph.blocks[target].statements.add(addStatement(assign));
                statement.value = -1;
            }

            for (auto* body:
                 {&statement.body, &statement.elseBody, &statement.continuing})
                if (*body >= 0)
                    *body = copyBlock(*body, result);

            graph.blocks[target].statements.add(addStatement(statement));
        }
    }

    ShaderGraph& graph;
};

ShaderGraph ShaderGraph::withFunctionsInlined() const
{
    auto inlined = *this;

    if (!functionList.empty())
        FunctionInliner {inlined}.run();

    return inlined;
}
} // namespace eacp::GPU
