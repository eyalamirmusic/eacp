#pragma once

#include <eacp/Core/ObjC/RuntimeClass.h>
#import <QuartzCore/QuartzCore.h>

namespace eacp::Graphics
{
// Suppresses implicit animations so property changes apply immediately.
inline id<CAAction> immediateActionForKey(id, SEL, NSString*)
{
    return (id<CAAction>) [NSNull null];
}

// Runtime-registered CALayer subclass whose only override is actionForKey:.
template <typename LayerType>
Class makeImmediateLayerClass(const char* nameRoot)
{
    auto builder = new ObjC::RuntimeClass<LayerType>(nameRoot);
    builder->addMethod(@selector(actionForKey:), immediateActionForKey);
    builder->registerClass();
    return builder->get();
}
} // namespace eacp::Graphics
