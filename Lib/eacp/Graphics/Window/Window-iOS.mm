#include "Window.h"
#include "../Graphics/Keyboard.h"
#include "../Primitives/GraphicUtils.h"
#import <UIKit/UIKit.h>

@interface WindowViewController : UIViewController
@end

@implementation WindowViewController

- (BOOL)prefersStatusBarHidden
{
    return YES;
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations
{
    return UIInterfaceOrientationMaskAll;
}

@end

namespace eacp::Graphics
{
namespace
{
// The part of the window under the keyboard's frame; the notification gives
// that frame in screen coordinates.
float heightCoveredByKeyboard(UIWindow* window, CGRect keyboardFrameOnScreen)
{
    auto keyboardFrame = [window convertRect:keyboardFrameOnScreen
                         fromCoordinateSpace:window.screen.coordinateSpace];
    auto covered = CGRectIntersection(window.bounds, keyboardFrame);

    if (CGRectIsNull(covered))
        return 0.f;

    return (float) covered.size.height;
}

SoftwareKeyboardChange softwareKeyboardChangeFrom(UIWindow* window,
                                                  NSNotification* notification)
{
    auto* info = notification.userInfo;
    auto endFrame = [info[UIKeyboardFrameEndUserInfoKey] CGRectValue];

    auto change = SoftwareKeyboardChange {};
    change.coveredHeight = heightCoveredByKeyboard(window, endFrame);
    change.animationSeconds =
        [info[UIKeyboardAnimationDurationUserInfoKey] doubleValue];
    return change;
}
} // namespace

struct Window::Native
{
    Native(const WindowOptions&, WindowEvents& events)
    {
        UIWindowScene* windowScene = nil;
        for (UIScene* scene in [UIApplication sharedApplication].connectedScenes)
        {
            if ([scene isKindOfClass:[UIWindowScene class]])
            {
                windowScene = (UIWindowScene*) scene;
                break;
            }
        }

        if (windowScene)
        {
            window = [[UIWindow alloc] initWithWindowScene:windowScene];
        }
        else
        {
            window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
        }

        viewController = [[WindowViewController alloc] init];
        window.get().rootViewController = viewController.get();
        window.get().backgroundColor = [UIColor blackColor];

        [window.get() makeKeyAndVisible];

        observeSoftwareKeyboard(events);
    }

    // UIKeyboardWillChangeFrame covers coming up, changing height and going
    // down alike: a keyboard going down ends below the window.
    void observeSoftwareKeyboard(WindowEvents& events)
    {
        auto* uiWindow = window.get();
        auto* windowEvents = &events;
        auto* center = [NSNotificationCenter defaultCenter];
        auto* observer =
            [center addObserverForName:UIKeyboardWillChangeFrameNotification
                                object:nil
                                 queue:nil
                            usingBlock:^(NSNotification* notification) {
                              windowEvents->onSoftwareKeyboardChanged(
                                  softwareKeyboardChangeFrom(uiWindow, notification));
                            }];

        keyboardObserver = ObjC::Ptr<NSObject>(observer, ObjC::RetainMode {});
    }

    void setTitle(const std::string&)
    {
    }

    void setContentView(void* contentView)
    {
        auto v = (__bridge UIView*) contentView;
        viewController.get().view = v;
        v.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    }

    UIWindow* getWindow() { return window.get(); }
    UIWindow* getWindow() const { return const_cast<UIWindow*>(window.get()); }

    ~Native()
    {
        [[NSNotificationCenter defaultCenter] removeObserver:keyboardObserver.get()];
        window.get().hidden = YES;
        window.get().rootViewController = nil;
    }

    ObjC::Ptr<UIWindow> window;
    ObjC::Ptr<WindowViewController> viewController;
    ObjC::Ptr<NSObject> keyboardObserver;
};

Window::Window(const WindowOptions& optionsToUse)
    : options(optionsToUse)
    , impl(options, events)
{
    events.input.activationChanged(true);
}

void Window::setTitle(const std::string& title)
{
    impl->setTitle(title);
}

void Window::setContentView(View& view)
{
    contentLink.attach(&view, this);
    impl->setContentView(view.getHandle());
}

void Window::toFront()
{
    // iOS apps are single-window; there's nothing to bring to the front.
}

void Window::setVisible(bool)
{
    // iOS apps are single-window; there's nothing to hide or show.
}

bool Window::isVisible()
{
    // iOS apps are single-window; the window is always on screen.
    return true;
}

Point Window::getPosition() const
{
    // iOS apps are single-window and fill the screen; there is no position to
    // report.
    return {};
}

void Window::setPosition(Point)
{
    // iOS apps are single-window; there's nowhere to move to.
}

Point Window::getSize() const
{
    auto bounds = impl->getWindow().bounds.size;
    return {(float) bounds.width, (float) bounds.height};
}

void Window::setSize(Point)
{
    // iOS apps are single-window and fill the screen; the size is not ours.
}

void Window::minimize()
{
    // iOS apps are single-window; there's nothing to minimize.
}

void Window::toggleMaximize()
{
    // iOS windows always fill the screen; there's nothing to toggle.
}

void Window::setFullScreen(bool)
{
    // iOS windows always fill the screen; there's nothing to switch.
}

bool Window::isFullScreen() const
{
    return true;
}

void* Window::getHandle()
{
    return impl->getWindow();
}

Window::~Window() = default;

// No pointer on iOS; mouse lock is meaningless there.
void Window::setMouseLocked(bool) {}

bool Window::isMouseLocked() const
{
    return false;
}

bool Window::isKeyPressed(uint16_t virtualKeyCode) const
{
    return Keyboard::isKeyPressed(virtualKeyCode);
}

bool Window::isShiftPressed() const
{
    return Keyboard::isShiftPressed();
}

bool Window::isControlPressed() const
{
    return Keyboard::isControlPressed();
}

bool Window::isAltPressed() const
{
    return Keyboard::isAltPressed();
}

bool Window::isCommandPressed() const
{
    return Keyboard::isCommandPressed();
}

ModifierKeys Window::getModifiers() const
{
    return Keyboard::getModifiers();
}

} // namespace eacp::Graphics
