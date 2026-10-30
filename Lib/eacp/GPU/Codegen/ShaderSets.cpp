#include "ShaderSets.h"

#include <utility>

namespace eacp::GPU
{
namespace
{
// Puts the previous visitor back however define() leaves, a throw included.
struct ShaderSetVisitorScope
{
    ShaderSetVisitorScope(const ShaderGraphVisitor*& slotToUse,
                          const ShaderGraphVisitor& visitor)
        : slot(slotToUse)
        , previous(std::exchange(slotToUse, &visitor))
    {
    }

    ~ShaderSetVisitorScope() { slot = previous; }

    ShaderSetVisitorScope(const ShaderSetVisitorScope&) = delete;
    ShaderSetVisitorScope& operator=(const ShaderSetVisitorScope&) = delete;

    const ShaderGraphVisitor*& slot;
    const ShaderGraphVisitor* previous;
};

const ShaderGraphVisitor& ignoringVisitor()
{
    static const auto ignore = ShaderGraphVisitor {[](const ShaderGraph&) {}};
    return ignore;
}

Vector<const ShaderSetDefinition*>& registeredSets()
{
    static auto sets = Vector<const ShaderSetDefinition*> {};
    return sets;
}
} // namespace

void ShaderSetDefinition::run(const ShaderGraphVisitor& visitorToUse) const
{
    auto scope = ShaderSetVisitorScope {current, visitorToUse};
    define();
}

void ShaderSetDefinition::visit(const ShaderGraph& graph) const
{
    visitor()(graph);
}

const ShaderGraphVisitor& ShaderSetDefinition::visitor() const
{
    return current != nullptr ? *current : ignoringVisitor();
}

namespace ShaderSets
{
void registerSet(const ShaderSetDefinition& set)
{
    registeredSets().add(&set);
}

const Vector<const ShaderSetDefinition*>& registered()
{
    return registeredSets();
}
} // namespace ShaderSets
} // namespace eacp::GPU
