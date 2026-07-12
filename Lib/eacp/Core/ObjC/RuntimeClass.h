#pragma once

#include <objc/message.h>
#include <objc/runtime.h>
#include <string>

namespace eacp::ObjC
{
// Builds and registers an Objective-C class at runtime under a
// process-unique name: the ObjC class registry is global to the process, so
// a compile-time @implementation collides as soon as a host and a
// dlopen-loaded plugin both carry this eacp copy's classes ("Class X is
// implemented in both..."). Probing objc_lookUpClass and suffixing _1, _2...
// keeps every eacp copy's classes distinct while staying readable in crash
// logs. Method implementations are plain C functions taking (id self,
// SEL _cmd, ...); their type encodings are generated from the signature.
//
// Build one per class inside a function-local static so the class outlives
// its instances and is disposed only at image teardown.
template <typename SuperType>
class RuntimeClass
{
public:
    explicit RuntimeClass(const char* nameRoot)
    {
        auto name = std::string(nameRoot);
        auto iteration = 0;

        while (objc_lookUpClass(name.c_str()) != nullptr)
            name = std::string(nameRoot) + "_" + std::to_string(++iteration);

        cls = objc_allocateClassPair([SuperType class], name.c_str(), 0);
    }

    // The runtime subclasses us behind our back for KVO; disposing a class
    // that still has a live NSKVONotifying_ subclass is fatal, so leak the
    // pair in that (rare, teardown-only) case.
    ~RuntimeClass()
    {
        if (cls == nullptr)
            return;

        auto kvoName = std::string("NSKVONotifying_") + class_getName(cls);

        if (objc_lookUpClass(kvoName.c_str()) == nullptr)
            objc_disposeClassPair(cls);
    }

    RuntimeClass(const RuntimeClass&) = delete;
    RuntimeClass& operator=(const RuntimeClass&) = delete;

    template <typename ReturnType, typename... Args>
    void addMethod(SEL selector, ReturnType (*implementation)(id, SEL, Args...))
    {
        auto encoding = encodeSignature<ReturnType, Args...>();
        class_addMethod(cls, selector, (IMP) implementation, encoding.c_str());
    }

    void addProtocol(Protocol* protocol)
    {
        class_addProtocol(cls, protocol);
    }

    template <typename T>
    void addIvar(const char* name)
    {
        class_addIvar(cls,
                      name,
                      sizeof(T),
                      (uint8_t) __builtin_ctzl(alignof(T)),
                      @encode(T));
    }

    Class registerClass()
    {
        objc_registerClassPair(cls);
        return cls;
    }

    Class get() const
    {
        return cls;
    }

private:
    template <typename ReturnType, typename... Args>
    static std::string encodeSignature()
    {
        auto result = std::string(@encode(ReturnType)) + @encode(id)
                      + @encode(SEL);
        ((result += @encode(Args)), ...);
        return result;
    }

    Class cls = nullptr;
};

// Calls the superclass implementation from a runtime-class method — the
// equivalent of [super selector]. Safe for void/scalar returns on both
// arm64 and x86_64; large struct returns would need objc_msgSendSuper_stret
// on x86_64 and are deliberately unsupported.
template <typename ReturnType, typename... Args>
ReturnType sendSuper(id self, Class superclass, SEL selector, Args... args)
{
    auto super = objc_super {self, superclass};
    using Function = ReturnType (*)(objc_super*, SEL, Args...);
    auto function = (Function) objc_msgSendSuper;
    return function(&super, selector, args...);
}

template <typename T>
T& getIvar(id object, const char* name)
{
    auto* ivar = class_getInstanceVariable(object_getClass(object), name);
    auto offset = ivar_getOffset(ivar);
    return *(T*) ((uint8_t*) object + offset);
}
} // namespace eacp::ObjC
