#pragma once

#include <Miro/Miro.h>

#include <algorithm>
#include <cmath>

// Parameters stays at file scope so its qualifiedName matches the
// baseline TS exactly. Defaults double as the hook's initial value
// (toJSON(Parameters{}) is what defaultPayloadJson feeds the hooks
// codegen).
struct Parameters
{
    double level = 0.5;
    bool autoCycle = false;
    long long counter = 0;

    MIRO_REFLECT(level, autoCycle, counter)
};

namespace Api
{

// Replaces the (ParametersStore singleton + parametersStore() accessor +
// EACP_STATE macro + getParameters/setParameters/advanceTick free fns +
// MIRO_EXPORT_COMMANDS macro) tangle from the static-init flow with one
// class. reflect() lists the wire surface; the methods do the work.
//
// All method bodies are inline because the codegen executable ODR-uses
// the pmfs through the makePmfHandler lambda chain — and the codegen
// exe doesn't compile any .cpp in this app.
class ParametersApi
{
public:
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
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

    // Called from MyApp's timer — not exposed as a bridge command.
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
