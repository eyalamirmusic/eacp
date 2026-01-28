#include "View.h"

namespace eacp::Graphics
{
View& View::setHandlesMouseEvents(bool value)
{
    properties.handlesMouseEvents = value;
    return *this;
}

View& View::setGrabsFocusOnMouseDown(bool value)
{
    properties.grabsFocusOnMouseDown = value;
    return *this;
}
} // namespace eacp::Graphics