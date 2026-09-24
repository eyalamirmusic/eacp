#pragma once

#include <string>
#include <typeinfo>

namespace eacp::GPU
{
// A type's name as its source spells it, without the namespaces around it:
// "LinearF32" for eacp::ML::LinearF32, whichever compiler mangled it. What a
// ComputeProgram is called in a per-dispatch timing unless it names itself.
std::string readableTypeName(const std::type_info& type);
} // namespace eacp::GPU
