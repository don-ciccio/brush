/*******************************************************************************************
 *   macos_window.mm — seamless titlebar for the brush editor (Godot-style).
 *
 *   Makes the GL content extend under the macOS titlebar and hides the title,
 *   leaving only the floating traffic-light buttons — the same NSWindow flags
 *   Godot/VS Code use. The ImGui menu bar row takes the titlebar's place
 *   (left-padded so it clears the buttons), and dragging its empty area moves
 *   the window like a native titlebar would.
 ********************************************************************************************/

#import <Cocoa/Cocoa.h>

extern "C" void EditorMacSeamlessTitlebar(void *nsWindow) {
    NSWindow *win = (__bridge NSWindow *)nsWindow;
    if (win == nil) return;
    win.titlebarAppearsTransparent = YES;
    win.titleVisibility = NSWindowTitleHidden;
    win.styleMask |= NSWindowStyleMaskFullSizeContentView;
}

// Begin a native window drag from the current mouse-down event (called when
// the user presses on empty menu-bar space).
extern "C" void EditorMacDragWindow(void *nsWindow) {
    NSWindow *win = (__bridge NSWindow *)nsWindow;
    NSEvent *event = [NSApp currentEvent];
    if (win == nil || event == nil) return;
    [win performWindowDragWithEvent:event];
}
