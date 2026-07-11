/*******************************************************************************************
 *   macos_window.mm — native macOS integration for the brush editor.
 *
 *   1. Seamless titlebar (Godot-style): GL content extends under the titlebar,
 *      only the floating traffic-light buttons remain.
 *   2. Native menu bar: File/Edit/View live in the macOS top bar (freeing the
 *      whole in-window row), and the app menu is titled "Brush" instead of the
 *      process name. Menu actions post integer tags that the C++ frame loop
 *      polls — keep the tag values in sync with editor/main.cpp.
 ********************************************************************************************/

#import <Cocoa/Cocoa.h>

static int g_pendingMenuAction = 0;

@interface BrushMenuBridge : NSObject
- (void)onMenu:(NSMenuItem *)sender;
@end
@implementation BrushMenuBridge
- (void)onMenu:(NSMenuItem *)sender {
    g_pendingMenuAction = (int)sender.tag;
}
@end

static BrushMenuBridge *g_bridge = nil;

extern "C" void EditorMacSeamlessTitlebar(void *nsWindow) {
    NSWindow *win = (__bridge NSWindow *)nsWindow;
    if (win == nil) return;
    win.titlebarAppearsTransparent = YES;
    win.titleVisibility = NSWindowTitleHidden;
    win.styleMask |= NSWindowStyleMaskFullSizeContentView;
}

extern "C" void EditorMacDragWindow(void *nsWindow) {
    NSWindow *win = (__bridge NSWindow *)nsWindow;
    NSEvent *event = [NSApp currentEvent];
    if (win == nil || event == nil) return;
    [win performWindowDragWithEvent:event];
}

// Returns and clears the last activated menu action tag (0 = none).
extern "C" int EditorMacPollMenuAction(void) {
    int a = g_pendingMenuAction;
    g_pendingMenuAction = 0;
    return a;
}

static NSMenuItem *AddItem(NSMenu *menu, NSString *title, int tag,
                           NSString *key, NSEventModifierFlags mods) {
    NSMenuItem *item =
        [[NSMenuItem alloc] initWithTitle:title
                                   action:@selector(onMenu:)
                            keyEquivalent:key];
    item.target = g_bridge;
    item.tag = tag;
    if (key.length > 0) item.keyEquivalentModifierMask = mods;
    [menu addItem:item];
    return item;
}

extern "C" void EditorMacInstallMenu(void) {
    g_bridge = [[BrushMenuBridge alloc] init];

    NSMenu *mainMenu = [[NSMenu alloc] init];

    // App menu — its TITLE is what the top bar shows next to the Apple logo.
    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:appItem];
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"Brush"];
    AddItem(appMenu, @"About Brush", 100, @"", 0);
    [appMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *quit = [[NSMenuItem alloc] initWithTitle:@"Quit Brush"
                                                  action:@selector(onMenu:)
                                           keyEquivalent:@"q"];
    quit.target = g_bridge;
    quit.tag = 3;
    [appMenu addItem:quit];
    appItem.submenu = appMenu;

    NSMenuItem *fileItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:fileItem];
    NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
    AddItem(fileMenu, @"Save", 1, @"s", NSEventModifierFlagCommand);
    AddItem(fileMenu, @"Reload from Disk", 2, @"", 0);
    [fileMenu addItem:[NSMenuItem separatorItem]];
    AddItem(fileMenu, @"Play / Stop Game", 10, @"r", NSEventModifierFlagCommand);
    fileItem.submenu = fileMenu;

    NSMenuItem *editItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:editItem];
    NSMenu *editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
    AddItem(editMenu, @"Undo Sculpt", 11, @"z", NSEventModifierFlagCommand);
    [editMenu addItem:[NSMenuItem separatorItem]];
    AddItem(editMenu, @"Add Block", 4, @"", 0);
    AddItem(editMenu, @"Add Light", 5, @"", 0);
    [editMenu addItem:[NSMenuItem separatorItem]];
    AddItem(editMenu, @"Duplicate", 6, @"d", NSEventModifierFlagCommand);
    AddItem(editMenu, @"Delete", 7, [NSString stringWithFormat:@"%c", 0x08],
            NSEventModifierFlagCommand);
    editItem.submenu = editMenu;

    NSMenuItem *viewItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:viewItem];
    NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    AddItem(viewMenu, @"Frame Selection", 8, @"f", NSEventModifierFlagCommand);
    AddItem(viewMenu, @"Frame Scene", 9, @"f",
            NSEventModifierFlagCommand | NSEventModifierFlagShift);
    viewItem.submenu = viewMenu;

    [NSApp setMainMenu:mainMenu];
}
