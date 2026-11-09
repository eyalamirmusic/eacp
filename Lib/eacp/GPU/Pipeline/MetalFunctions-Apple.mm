#import <Metal/Metal.h>

#include "MetalFunctions-Apple.h"

#include "../Device/MetalErrors-Apple.h"
#include "../Shader/ShaderLibrary.h"

namespace eacp::GPU::Detail
{
namespace
{
void setConstantValue(MTLFunctionConstantValues* values,
                      const PipelineConstant& constant)
{
    const auto index = (NSUInteger) constant.id;

    switch (constant.type)
    {
        case PipelineConstantType::Bool:
        {
            const auto value = constant.asBool();
            [values setConstantValue:&value type:MTLDataTypeBool atIndex:index];
            return;
        }
        case PipelineConstantType::Int:
        {
            const auto value = constant.asInt();
            [values setConstantValue:&value type:MTLDataTypeInt atIndex:index];
            return;
        }
        case PipelineConstantType::UInt:
        {
            const auto value = constant.asUInt();
            [values setConstantValue:&value type:MTLDataTypeUInt atIndex:index];
            return;
        }
        case PipelineConstantType::Float:
        {
            const auto value = constant.asFloat();
            [values setConstantValue:&value type:MTLDataTypeFloat atIndex:index];
            return;
        }
        case PipelineConstantType::Half:
        {
            const auto value = constant.asHalfBits();
            [values setConstantValue:&value type:MTLDataTypeHalf atIndex:index];
            return;
        }
    }
}
} // namespace

id<MTLFunction> makeMetalFunction(const ShaderLibrary& library,
                                  const std::string& name,
                                  const PipelineConstants& constants)
{
    auto metalLibrary = (__bridge id<MTLLibrary>) library.nativeLibrary();

    if (metalLibrary == nil)
        return nil;

    auto functionName = @(name.c_str());

    if (!library.hasPipelineConstants())
        return [metalLibrary newFunctionWithName:functionName];

    auto values = [[MTLFunctionConstantValues alloc] init];

    for (const auto& constant: constants.values)
        setConstantValue(values, constant);

    auto specialize = [metalLibrary, functionName, values](NSError** error)
    {
        return [metalLibrary newFunctionWithName:functionName
                                  constantValues:values
                                           error:error];
    };

    auto function = buildRetryingTransientFailures<id<MTLFunction>>(
        "specialising a Metal function", specialize);

    [values release];
    return function;
}
} // namespace eacp::GPU::Detail
