#pragma once

#include <Miro/Miro.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

struct CalculatorState
{
    // The main readout: the entry being typed, or the latest result
    // ("Error" after an impossible operation, e.g. dividing by zero).
    std::string display = "0";

    // The pending half of the calculation, shown above the display
    // ("12 +"). Empty when no operation is pending.
    std::string expression;

    bool error = false;

    MIRO_REFLECT(display, expression, error)
};

struct KeyRequest
{
    // One calculator key: "0".."9", ".", "+", "-", "*", "/", "=",
    // "C" (all clear), "±" (negate), "%" (percent), "⌫" (backspace).
    std::string key;

    MIRO_REFLECT(key)
};

namespace Api
{

// All calculation lives here, on the C++ side: the page only sends key
// presses and renders the published CalculatorState. Classic
// immediate-execution semantics (an operator applies the previous
// pending one), with an error latch only "C" clears.
//
// All method bodies are inline because the codegen executable ODR-uses
// these pmfs through the makePmfHandler lambda chain.
class CalculatorApi
{
public:
    CalculatorApi() { publish(); }

    void reflect(Miro::ApiReflector& r)
    {
        using T = CalculatorApi;
        r.commands<&T::getCalculator, &T::press>();
        r.event(&T::calculator, "calculator");
    }

    CalculatorState getCalculator() const { return calculator.snapshot(); }

    void press(const KeyRequest& req)
    {
        handleKey(req.key);
        publish();
    }

    Miro::Event<CalculatorState> calculator;

private:
    static constexpr auto maxEntryLength = 15;

    void handleKey(const std::string& key)
    {
        if (key == "C")
        {
            reset();
            return;
        }

        if (error)
            return;

        if (key.size() == 1 && key[0] >= '0' && key[0] <= '9')
            typeDigit(key[0]);
        else if (key == ".")
            typeDecimal();
        else if (key == "+" || key == "-" || key == "*" || key == "/")
            typeOperator(key[0]);
        else if (key == "=")
            evaluate();
        else if (key == "±")
            negate();
        else if (key == "%")
            percent();
        else if (key == "⌫")
            backspace();
    }

    void reset()
    {
        accumulator = 0;
        pendingOp = 0;
        entry.clear();
        justEvaluated = false;
        error = false;
    }

    void typeDigit(char digit)
    {
        if (justEvaluated)
            startFreshEntry();

        if (entry.size() >= maxEntryLength)
            return;

        if (entry == "0")
            entry.clear();

        entry.push_back(digit);
    }

    void typeDecimal()
    {
        if (justEvaluated)
            startFreshEntry();

        if (entry.find('.') != std::string::npos)
            return;

        entry += entry.empty() ? "0." : ".";
    }

    void typeOperator(char op)
    {
        if (!entry.empty())
        {
            if (pendingOp != 0)
                applyPending();
            else
                accumulator = entryValue();

            entry.clear();
        }

        // Entry empty + an operator already pending -> the user is
        // changing their mind; just swap the operator.
        if (!error)
            pendingOp = op;

        justEvaluated = false;
    }

    void evaluate()
    {
        if (pendingOp == 0)
        {
            if (!entry.empty())
            {
                accumulator = entryValue();
                entry.clear();
            }
            justEvaluated = true;
            return;
        }

        // "12 + =" with no right-hand side: wait for one rather than
        // guessing.
        if (entry.empty())
            return;

        applyPending();
        pendingOp = 0;
        entry.clear();
        justEvaluated = true;
    }

    void negate()
    {
        if (entry.empty())
        {
            accumulator = -accumulator;
            return;
        }

        if (entry.front() == '-')
            entry.erase(entry.begin());
        else
            entry.insert(entry.begin(), '-');
    }

    void percent()
    {
        if (entry.empty())
            accumulator /= 100;
        else
            entry = formatNumber(entryValue() / 100);
    }

    void backspace()
    {
        if (justEvaluated || entry.empty())
            return;

        entry.pop_back();
        if (entry == "-" || entry == "0")
            entry.clear();
    }

    void startFreshEntry()
    {
        entry.clear();
        accumulator = 0;
        pendingOp = 0;
        justEvaluated = false;
    }

    void applyPending()
    {
        auto rhs = entryValue();

        switch (pendingOp)
        {
            case '+':
                accumulator += rhs;
                break;
            case '-':
                accumulator -= rhs;
                break;
            case '*':
                accumulator *= rhs;
                break;
            case '/':
                if (rhs == 0)
                {
                    error = true;
                    return;
                }
                accumulator /= rhs;
                break;
            default:
                break;
        }

        if (!std::isfinite(accumulator))
            error = true;
    }

    double entryValue() const { return std::strtod(entry.c_str(), nullptr); }

    std::string formatNumber(double value) const
    {
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, "%.12g", value);
        return buffer;
    }

    std::string opSymbol(char op) const
    {
        if (op == '*')
            return "×";
        if (op == '/')
            return "÷";
        return std::string(1, op);
    }

    void publish()
    {
        auto next = CalculatorState {};
        next.error = error;
        next.display = error           ? "Error"
                       : entry.empty() ? formatNumber(accumulator)
                                       : entry;

        if (pendingOp != 0 && !error)
            next.expression = formatNumber(accumulator) + " " + opSymbol(pendingOp);

        calculator.publish(std::move(next));
    }

    double accumulator = 0;
    char pendingOp = 0;
    std::string entry;
    bool justEvaluated = false;
    bool error = false;
};

} // namespace Api
