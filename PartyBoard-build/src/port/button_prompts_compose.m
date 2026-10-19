// Composes a controller label onto one of the game's 3D button textures
// (see button_prompts.cpp): the base is the original texture with its letter
// erased and recoloured (tools/button-prompts/repaint3d.m), the label one of
// ButtonPrompts/<family>/label/*.png; the label is drawn inside the box given
// in the original texture's pixel coordinates.
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>

int PartyBoard_ComposeButtonTexture(const char *basePath, const char *labelPath, const char *outPath,
    int origWidth, float x0, float y0, float x1, float y1)
{
    @autoreleasepool {
        NSBitmapImageRep *base = [NSBitmapImageRep imageRepWithData:[NSData dataWithContentsOfFile:@(basePath)]];
        NSImage *label = [[NSImage alloc] initWithContentsOfFile:@(labelPath)];
        if (base == nil || label == nil || origWidth <= 0) {
            return 0;
        }
        const NSInteger W = base.pixelsWide, H = base.pixelsHigh;
        const float k = (float)W / origWidth;
        NSBitmapImageRep *out = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:W pixelsHigh:H
            bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
            bytesPerRow:W * 4 bitsPerPixel:32];
        NSGraphicsContext *ctx = [NSGraphicsContext graphicsContextWithBitmapImageRep:out];
        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:ctx];
        ctx.imageInterpolation = NSImageInterpolationHigh;
        [[NSColor clearColor] setFill];
        NSRectFillUsingOperation(NSMakeRect(0, 0, W, H), NSCompositingOperationCopy);
        [base drawInRect:NSMakeRect(0, 0, W, H) fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1
          respectFlipped:NO hints:nil];
        NSImageRep *lr = label.representations.firstObject;
        const float bw = (x1 - x0) * k, bh = (y1 - y0) * k, aspect = (float)lr.pixelsWide / lr.pixelsHigh;
        float lw = bw, lh = bw / aspect;
        if (lh > bh) {
            lh = bh;
            lw = bh * aspect;
        }
        const float cx = (x0 + x1) / 2.f * k, cy = H - (y0 + y1) / 2.f * k;
        [label drawInRect:NSMakeRect(cx - lw / 2, cy - lh / 2, lw, lh) fromRect:NSZeroRect
                operation:NSCompositingOperationSourceAtop fraction:1];
        [NSGraphicsContext restoreGraphicsState];
        NSData *png = [out representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        return [png writeToFile:@(outPath) atomically:YES] ? 1 : 0;
    }
}

// A strip of button cells stacked top to bottom (Fish 'n' Drips keeps its
// buttons in one 30x420 image): each glyph is drawn into its cell at k times
// the original size; cells from dimFrom on are darkened like the original's
// "already pressed" copies.
int PartyBoard_ComposeButtonStrip(const char *outPath, int cellW, int cellH, int k, const char *const *glyphPaths,
    int count, int dimFrom, float dim)
{
    @autoreleasepool {
        const NSInteger W = cellW * k, H = cellH * k * count;
        NSBitmapImageRep *out = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:W pixelsHigh:H
            bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
            bytesPerRow:W * 4 bitsPerPixel:32];
        NSGraphicsContext *ctx = [NSGraphicsContext graphicsContextWithBitmapImageRep:out];
        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:ctx];
        ctx.imageInterpolation = NSImageInterpolationHigh;
        [[NSColor clearColor] setFill];
        NSRectFillUsingOperation(NSMakeRect(0, 0, W, H), NSCompositingOperationCopy);
        for (int i = 0; i < count; ++i) {
            NSImage *glyph = glyphPaths[i] ? [[NSImage alloc] initWithContentsOfFile:@(glyphPaths[i])] : nil;
            if (glyph == nil) {
                continue;
            }
            // Glyph art is drawn with a margin; scale it up so the button
            // fills the cell as the original's does.
            const CGFloat size = MIN(cellW, cellH) * k * 1.2;
            const CGFloat cx = W / 2.0, cy = H - (i + 0.5) * cellH * k;
            const NSRect cell = NSMakeRect(0, H - (i + 1) * cellH * k, W, cellH * k);
            [NSGraphicsContext saveGraphicsState];
            NSRectClip(cell);
            [glyph drawInRect:NSMakeRect(cx - size / 2, cy - size / 2, size, size) fromRect:NSZeroRect
                    operation:NSCompositingOperationSourceOver fraction:1];
            if (i >= dimFrom) {
                [[NSColor colorWithWhite:0 alpha:dim] setFill];
                NSRectFillUsingOperation(cell, NSCompositingOperationSourceAtop);
            }
            [NSGraphicsContext restoreGraphicsState];
        }
        [NSGraphicsContext restoreGraphicsState];
        NSData *png = [out representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        return [png writeToFile:@(outPath) atomically:YES] ? 1 : 0;
    }
}
#else
int PartyBoard_ComposeButtonStrip(const char *, int, int, int, const char *const *, int, int, float) { return 0; }
int PartyBoard_ComposeButtonTexture(const char *, const char *, const char *, int, float, float, float, float) { return 0; }
#endif
