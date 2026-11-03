#pragma once

#include "Forward.h"

#include <eacp/Core/Utils/Containers.h>

#include <concepts>
#include <type_traits>
#include <utility>

// The shaders an app compiles at build time, listed in any of its shader
// translation units (the SOURCES of eacp_add_precompiled_shaders):
//
//   EACP_SHADER_SET()
//   {
//       visit<RotatingShader>();
//       visit<GainKernel, BlurKernel, SumKernel>();
//       visit<SpriteShader>(TextureSampling::nearest);
//       UI::LayerRenderer::forEachShaderGraph(visitor());
//   }
//
// The braces are the body of a ShaderSetDefinition's define(), and the macro
// registers one instance of it before main(), so a translation unit can hold
// any number of sets and nothing names them. The build's shader host
// executable (CMake/ShaderHostMain.cpp.in) links those translation units into
// itself and ShaderCompiler::runMain compiles every graph every registered set
// shows it.
namespace eacp::GPU
{
template <typename Shader>
concept HasShaderGraph = requires(const Shader& shader) {
    { shader.graph() } -> std::convertible_to<const ShaderGraph&>;
};

struct ShaderSetDefinition
{
    virtual ~ShaderSetDefinition() = default;

    virtual void define() const = 0;

    // Calls define() with every visit() it makes going to `visitor`.
    void run(const ShaderGraphVisitor& visitor) const;

protected:
    // Each shader built with no arguments.
    template <typename... Shaders>
    void visit() const
    {
        static_assert((std::is_default_constructible_v<Shaders> && ...),
                      "visit<Shader>() builds each shader with no arguments; a "
                      "shader whose constructor needs some is visited as "
                      "visit<Shader>(args...).");

        if constexpr ((std::is_default_constructible_v<Shaders> && ...))
            (visit(static_cast<const ShaderGraph&>(Shaders().graph())), ...);
    }

    // One shader built from `args`, once per variant the app draws with.
    template <HasShaderGraph Shader, typename... Args>
        requires(sizeof...(Args) > 0) && std::constructible_from<Shader, Args...>
    void visit(Args&&... args) const
    {
        visit(static_cast<const ShaderGraph&>(
            Shader(std::forward<Args>(args)...).graph()));
    }

    // For a module hook that hands over graphs rather than types.
    void visit(const ShaderGraph& graph) const;

    // The visitor itself, for a module that hands over shaders it keeps
    // private: forEachShaderGraph(visitor()).
    const ShaderGraphVisitor& visitor() const;

private:
    mutable const ShaderGraphVisitor* current = nullptr;
};

namespace ShaderSets
{
void registerSet(const ShaderSetDefinition& set);

// Every set registered in this process, in registration order.
const Vector<const ShaderSetDefinition*>& registered();
} // namespace ShaderSets
} // namespace eacp::GPU

#define EACP_CONCAT_INNER(first, second) first##second
#define EACP_CONCAT(first, second) EACP_CONCAT_INNER(first, second)

// __COUNTER__ is per translation unit, a unity batch included, and the
// anonymous namespace keeps two translation units' EacpShaderSet_0 apart.
#define EACP_SHADER_SET()                                                           \
    EACP_SHADER_SET_NAMED(EACP_CONCAT(EacpShaderSet_, __COUNTER__))

// One more expansion, so the name is a single token before ## pastes it.
#define EACP_SHADER_SET_NAMED(Name) EACP_SHADER_SET_IMPL(Name)

#define EACP_SHADER_SET_IMPL(Name)                                                  \
    namespace                                                                       \
    {                                                                               \
    struct Name final : eacp::GPU::ShaderSetDefinition                              \
    {                                                                               \
        void define() const override;                                               \
    };                                                                              \
                                                                                    \
    const Name Name##Instance {};                                                   \
                                                                                    \
    [[maybe_unused]] const bool Name##Registered =                                  \
        (eacp::GPU::ShaderSets::registerSet(Name##Instance), true);                 \
    }                                                                               \
                                                                                    \
    void Name::define() const
