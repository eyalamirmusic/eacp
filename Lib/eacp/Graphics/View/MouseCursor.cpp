#include "MouseCursor.h"
#include "../Image/Image.h"

namespace eacp::Graphics
{

struct MouseCursor::Custom
{
    Image image;
    Point hotspot;
    float scale = 1.f;

    std::shared_ptr<void> native;

    const void* pixelAddress() const { return image.pixelBuffer().lock().get(); }

    bool sameAs(const Custom& other) const
    {
        return pixelAddress() == other.pixelAddress()
               && image.pixelGeneration() == other.image.pixelGeneration()
               && image.width() == other.image.width()
               && image.height() == other.image.height()
               && hotspot.x == other.hotspot.x && hotspot.y == other.hotspot.y
               && scale == other.scale;
    }
};

MouseCursor::MouseCursor(Shape shapeToUse)
    : shape(shapeToUse)
{
}

MouseCursor::MouseCursor(const Image& image, Point hotspot, float scale)
{
    if (!image.isValid() || image.isEmpty())
        return;

    custom = std::make_shared<Custom>();
    custom->image = image;
    custom->hotspot = hotspot;
    custom->scale = scale > 0.f ? scale : 1.f;
}

MouseCursor::Shape MouseCursor::getShape() const
{
    return shape;
}

bool MouseCursor::isImage() const
{
    return custom != nullptr;
}

Image MouseCursor::getImage() const
{
    return custom != nullptr ? custom->image : Image {};
}

Point MouseCursor::getHotspot() const
{
    return custom != nullptr ? custom->hotspot : Point {};
}

float MouseCursor::getScale() const
{
    return custom != nullptr ? custom->scale : 1.f;
}

bool MouseCursor::operator==(const MouseCursor& other) const
{
    if (shape != other.shape)
        return false;

    if (custom == other.custom)
        return true;

    if (custom == nullptr || other.custom == nullptr)
        return false;

    return custom->sameAs(*other.custom);
}

std::shared_ptr<void>* MouseCursor::getNativeCache() const
{
    return custom != nullptr ? &custom->native : nullptr;
}

} // namespace eacp::Graphics
