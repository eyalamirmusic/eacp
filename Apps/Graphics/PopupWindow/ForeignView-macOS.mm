#include "ForeignView.h"

#import <Cocoa/Cocoa.h>

#include <cstdio>

@interface EacpForeignPluginTarget: NSObject
- (void) buttonClicked: (id) sender;
@end

@implementation EacpForeignPluginTarget
- (void) buttonClicked: (id) sender
{
    (void) sender;
    std::printf("[plugin] the foreign NSButton fired\n");
    std::fflush(stdout);
}
@end

void addForeignContent(void* nativeParentHandle)
{
    auto* parent = (NSView*) nativeParentHandle;

    if (parent == nil)
        return;

    auto* editor = [[NSView alloc] initWithFrame:parent.bounds];
    editor.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    editor.wantsLayer = YES;
    editor.layer.backgroundColor = [NSColor colorWithSRGBRed:0.10f
                                                       green:0.24f
                                                        blue:0.36f
                                                       alpha:1.f]
                                       .CGColor;

    auto* label = [NSTextField
        labelWithString:@"Foreign native view — a plugin editor, not an eacp View"];
    label.frame = NSMakeRect(16.f, NSMaxY(editor.bounds) - 40.f, 440.f, 20.f);
    label.autoresizingMask = NSViewMinYMargin;
    label.textColor = NSColor.whiteColor;
    [editor addSubview:label];

    // Process-lifetime on purpose: a target is not the demo's to free, and
    // there is exactly one.
    static auto* target = [[EacpForeignPluginTarget alloc] init];

    auto* button = [NSButton buttonWithTitle:@"Plugin button"
                                      target:target
                                      action:@selector(buttonClicked:)];
    button.frame = NSMakeRect(16.f, NSMaxY(editor.bounds) - 88.f, 160.f, 32.f);
    button.autoresizingMask = NSViewMinYMargin;
    [editor addSubview:button];

    [parent addSubview:editor];
    [editor release];
}
