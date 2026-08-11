#pragma once

#include <Miro/Bridge.h>
#include <Miro/Reflect.h>

#include <algorithm>
#include <cmath>

// File scope keeps qualifiedName matching the generated TS; the defaults are
// also what the codegen feeds the hooks as their initial value.
struct Parameters
{
    double level = 0.5;
    bool autoCycle = false;
    long long counter = 0;

    MIRO_REFLECT(level, autoCycle, counter)
};

namespace Api
{

// Method bodies must stay inline: the codegen executable ODR-uses these pmfs
// but compiles none of this app's .cpp files.
class ParametersApi
{
public:
    void reflect(Miro::ApiReflector& r)
    {
        using T = ParametersApi;

        r.commands<&T::getParameters, &T::setParameters>();
        r.events<&T::parameters>();
    }

    Parameters getParameters() const { return parameters.snapshot(); }

    void setParameters(const Parameters& req)
    {
        auto next = parameters.snapshot();
        next.level = std::clamp(req.level, 0.0, 1.0);
        next.autoCycle = req.autoCycle;
        cyclePhase = std::asin(next.level * 2.0 - 1.0);
        parameters.publish(next);
    }

    void advanceTick()
    {
        auto next = parameters.snapshot();
        next.counter++;

        if (next.autoCycle)
        {
            cyclePhase += 0.05;
            next.level = 0.5 + 0.5 * std::sin(cyclePhase);
        }

        parameters.publish(next);
    }

    Miro::Event<Parameters> parameters;

private:
    double cyclePhase = 0.0;
};

} // namespace Api
