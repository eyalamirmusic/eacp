#pragma once

#include "View.h"

namespace eacp::Graphics
{
// A view holding at most one child, which always fills it. set() destroys the
// previous child, which leaves this view as it goes: View::~View removes a view
// from its parent.
template <typename T = View>
class OptionalView : public View
{
public:
    void set(OwningPointer<T> contentToUse)
    {
        content.reset(contentToUse.release());

        if (content == nullptr)
            return;

        addSubview(*content);
        content->setBounds(getLocalBounds());
    }

    void clear() { content.reset(); }

    T* get() const { return content.get(); }
    T& operator*() const { return *content; }
    T* operator->() const { return content.get(); }

    explicit operator bool() const { return hasContent(); }
    bool hasContent() const { return content != nullptr; }

    void resized() override
    {
        if (content != nullptr)
            content->setBounds(getLocalBounds());
    }

private:
    OwningPointer<T> content;
};
} // namespace eacp::Graphics
