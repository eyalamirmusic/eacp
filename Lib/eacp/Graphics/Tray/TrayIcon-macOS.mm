#import <Cocoa/Cocoa.h>

#include "TrayIcon.h"
#include "../Helpers/ImageConversion-macOS.h"
#include "../Menu/AppKitMenu.h"
#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/ObjC/Strings.h>

namespace eacp::Graphics
{

struct TrayIcon::Native
{
    Native()
    {
        if (eacp::Apps::getAppEnvironment().headless)
            return;

        // statusItemWithLength: hands back an item owned by the status bar.
        // Retain it so it survives past this call, and remove it on destruction.
        auto* item = [[NSStatusBar systemStatusBar]
            statusItemWithLength:NSVariableStatusItemLength];
        statusItem = ObjC::Ptr<NSStatusItem> {item, ObjC::RetainMode {}};
    }

    ~Native()
    {
        if (statusItem)
            [[NSStatusBar systemStatusBar] removeStatusItem:statusItem.get()];
    }

    void setIcon(const Image& icon)
    {
        if (!statusItem)
            return;

        auto* nsImage = toNSImage(icon);
        if (nsImage == nil)
            return;

        // Menu-bar icons read best at ~18pt tall; scale preserving aspect so
        // a high-resolution source still renders crisply on Retina.
        constexpr CGFloat menuBarHeight = 18.0;
        auto scale = menuBarHeight / icon.height();
        [nsImage setSize:NSMakeSize(icon.width() * scale, menuBarHeight)];

        [nsImage setTemplate:templateRendering ? YES : NO];
        [statusItem.get().button setImage:nsImage];
    }

    void setTooltip(const std::string& text)
    {
        if (!statusItem)
            return;

        [statusItem.get().button setToolTip:Strings::toNSString(text)];
    }

    void setMenu(const Menu& menu)
    {
        if (!statusItem)
            return;

        targets.clear();
        auto* nsMenu = buildAppKitMenu(menu, targets);
        menuPtr = ObjC::Ptr<NSMenu> {nsMenu};
        [statusItem.get() setMenu:nsMenu];
    }

    void setOnClick(Callback callback)
    {
        onClick = std::move(callback);

        if (!statusItem)
            return;

        // A status item with a menu opens it on click and never calls the
        // button action, so this only takes effect when no menu is set.
        clickTarget = makeActionTarget(onClick);
        auto* button = statusItem.get().button;
        [button setTarget:clickTarget.get()];
        [button setAction:@selector(trigger:)];
    }

    void setTemplateRendering(bool shouldRenderAsTemplate)
    {
        templateRendering = shouldRenderAsTemplate;

        if (statusItem)
            [[statusItem.get().button image]
                setTemplate:templateRendering ? YES : NO];
    }

    ObjC::Ptr<NSStatusItem> statusItem;
    ObjC::Ptr<NSMenu> menuPtr;
    ObjC::Ptr<NSObject> clickTarget;
    MenuTargets targets;
    bool templateRendering = true;
    Callback onClick;
};

TrayIcon::TrayIcon() = default;
TrayIcon::~TrayIcon() = default;

void TrayIcon::setIcon(const Image& icon)
{
    impl->setIcon(icon);
}

void TrayIcon::setTooltip(const std::string& text)
{
    impl->setTooltip(text);
}

void TrayIcon::setMenu(const Menu& menu)
{
    impl->setMenu(menu);
}

void TrayIcon::setOnClick(Callback callback)
{
    impl->setOnClick(std::move(callback));
}

void TrayIcon::setTemplateRendering(bool shouldRenderAsTemplate)
{
    impl->setTemplateRendering(shouldRenderAsTemplate);
}

} // namespace eacp::Graphics
