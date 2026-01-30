#include "Layer.h"
#include "../View/View.h"

namespace eacp::Graphics
{
void Layer::attachTo(View& view)
{
    detachFromView();

    if (parent != &view)
    {
        parent = &view;
        attachTo(view.getNativeLayer());
    }
}

void Layer::detachFromView()
{
    if (parent != nullptr)
    {
        parent->removeLayer(*this);
        parent = nullptr;
        detachFromLayer();
    }
}
} // namespace eacp::Graphics