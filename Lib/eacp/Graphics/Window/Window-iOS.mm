#include "Window.h"
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

struct Window::Native
{
    Native(const WindowOptions&)
    {
        // Get the key window scene
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

    ~Native()
    {
        window.get().hidden = YES;
        window.get().rootViewController = nil;
    }

    ObjC::Ptr<UIWindow> window;
    ObjC::Ptr<WindowViewController> viewController;
};

Window::Window(const WindowOptions& optionsToUse)
    : options(optionsToUse)
    , impl(options)
{
}

void Window::setTitle(const std::string& title)
{
    impl->setTitle(title);
}

void Window::setContentView(View& view)
{
    impl->setContentView(view.getHandle());
}

void* Window::getHandle()
{
    return impl->getWindow();
}

Window::~Window()
{
    options.onQuit();
}

} // namespace eacp::Graphics
