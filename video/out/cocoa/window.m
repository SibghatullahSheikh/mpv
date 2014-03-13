/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <libavutil/common.h>

#include "input/keycodes.h"

#include "osdep/macosx_application.h"
#include "osdep/macosx_events.h"
#include "osdep/macosx_compat.h"

#include "video/out/cocoa/additions.h"
#include "video/out/cocoa_common.h"

#include "window.h"

@implementation MpvVideoWindow {
    NSSize _queued_video_size;
}

@synthesize adapter = _adapter;
- (id)initWithContentRect:(NSRect)content_rect
                styleMask:(NSUInteger)style_mask
                  backing:(NSBackingStoreType)buffering_type
                    defer:(BOOL)flag
{
    if (self = [super initWithContentRect:content_rect
                                styleMask:style_mask
                                  backing:buffering_type
                                    defer:flag]) {
        [self setBackgroundColor:[NSColor blackColor]];
    }
    return self;
}

- (void)windowDidResize:(NSNotification *) notification
{
    [self.adapter setNeedsResize];
}

- (void)windowDidChangeBackingProperties:(NSNotification *)notification {
    [self.adapter setNeedsResize];
}

- (BOOL)isInFullScreenMode
{
    return (([self styleMask] & NSFullScreenWindowMask) ==
                NSFullScreenWindowMask);
}

- (void)setFullScreen:(BOOL)willBeFullscreen
{
    if (willBeFullscreen != [self isInFullScreenMode]) {
        [self toggleFullScreen:nil];
    }
}

- (BOOL)canBecomeMainWindow { return YES; }
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)windowShouldClose:(id)sender
{
    cocoa_put_key(MP_KEY_CLOSE_WIN);
    // We have to wait for MPlayer to handle this,
    // otherwise we are in trouble if the
    // MP_KEY_CLOSE_WIN handler is disabled
    return NO;
}

- (void)normalSize { [self mulSize:1.0f]; }

- (void)halfSize { [self mulSize:0.5f];}

- (void)doubleSize { [self mulSize:2.0f];}

- (void)mulSize:(float)multiplier
{
    char *cmd = ta_asprintf(NULL, "set window-scale %f", multiplier);
    [self.adapter putCommand:cmd];
}

- (void)setCenteredContentSize:(NSSize)ns
{
    NSRect f   = [self frame];
    NSRect nf  = [self frameRectForContentRect:NSMakeRect(0, 0, ns.width, ns.height)];
    CGFloat dx = f.size.width  - nf.size.width;
    CGFloat dy = f.size.height - nf.size.height;
    [self setFrame:NSInsetRect(f, dx/2, dy/2) display:NO animate:NO];
}

- (NSRect)constrainFrameRect:(NSRect)frameRect toScreen:(NSScreen *)screen {
    if([self isInFullScreenMode]) return frameRect;
    
    screen = screen ?: self.screen ?: [NSScreen mainScreen];
    NSRect vf = [screen visibleFrame];
    
    if(NSMinX(frameRect) >= NSMaxX(vf) || NSMaxX(frameRect) <= NSMinX(vf))
        frameRect.origin.x = NSMidX(vf) - NSWidth(frameRect)/2;
    if(NSMinY(frameRect) >= NSMaxY(vf) || NSMaxY(frameRect) <= NSMinY(vf))
        frameRect.origin.y = NSMidY(vf) - NSHeight(frameRect)/2;
    
    if(NSMaxY(frameRect) > NSMaxY(vf))
        frameRect.origin.y -= NSMaxY(frameRect) - NSMaxY(vf);
    
    NSRect cr = [self contentRectForFrameRect:frameRect];
    if(NSMaxY(cr) < NSMinY(vf))
        frameRect.origin.y += NSMinY(vf) - NSMaxY(cr);
    
    return frameRect;
}

- (void)windowDidEndLiveResize:(NSNotification *)notification
{
    [self setFrame:[self constrainFrameRect:self.frame toScreen:self.screen] display:NO];
}

- (void)queueNewVideoSize:(NSSize)new_size
{
    if (CGSizeEqualToSize(self->_queued_video_size, new_size)) return;
    self->_queued_video_size = new_size;

    if (![self.adapter isInFullScreenMode]) {
        [self setCenteredContentSize:self->_queued_video_size];
        [self setContentAspectRatio:self->_queued_video_size];
    }
}

- (NSSize)window:(NSWindow *)window willUseFullScreenContentSize:(NSSize)size {
    return window.screen.frame.size;
}

- (NSApplicationPresentationOptions)window:(NSWindow *)window
      willUseFullScreenPresentationOptions:(NSApplicationPresentationOptions)opts {
    return NSApplicationPresentationFullScreen      |
           NSApplicationPresentationAutoHideDock    |
           NSApplicationPresentationAutoHideMenuBar |
           NSApplicationPresentationAutoHideToolbar;
}

- (void)windowWillEnterFullScreen:(NSNotification *)notification {
    [self setContentResizeIncrements:NSMakeSize(1, 1)];
}
- (void)windowDidExitFullScreen:(NSNotification *)notification {
    [self setCenteredContentSize:self->_queued_video_size];
    [self setContentAspectRatio:self->_queued_video_size];
}

@end

