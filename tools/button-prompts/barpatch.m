// barpatch <in4x.png> <out.png> <x0> <y0> <x1> <y1> <glyph.png> [<x0> <y0> <x1> <y1> <glyph.png>]...
// For each box (original-pixel coordinates of a button painted into a menu
// bar; the image is the 4x smooth upscale), paints the bar colour over the box
// (each row blended between the bar pixels just left and right of it) and
// (premultiplied buffer, which AppKit can draw into) and draws the controller glyph centred there, slightly larger than the box like
// the original's raised button. BARPATCH_NOFILL=1 skips the fill (when the
// background was already rebuilt), BARPATCH_SCALE sets glyph height / box height.
#import <AppKit/AppKit.h>
int main(int argc, char **argv) {
  @autoreleasepool {
    NSBitmapImageRep *src = [NSBitmapImageRep imageRepWithData:[NSData dataWithContentsOfFile:@(argv[1])]];
    int W = (int)src.pixelsWide, H = (int)src.pixelsHigh, k = 4;
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:W pixelsHigh:H bitsPerSample:8 samplesPerPixel:4
        hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:0 bytesPerRow:W * 4 bitsPerPixel:32];
    uint8_t *d = rep.bitmapData;
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { NSUInteger p[4] = {0, 0, 0, 255}; [src getPixel:p atX:x y:y]; if (src.samplesPerPixel < 4) p[3] = 255; for (int c = 0; c < 3; ++c) d[(y * W + x) * 4 + c] = (uint8_t)(p[c] * p[3] / 255); d[(y * W + x) * 4 + 3] = p[3]; }
    const bool fill = getenv("BARPATCH_NOFILL") == NULL;
    const float scale = getenv("BARPATCH_SCALE") ? atof(getenv("BARPATCH_SCALE")) : 1.25f;
    for (int i = 3; fill && i + 4 < argc; i += 5) {
      int x0 = (atoi(argv[i]) - 1) * k, y0 = (atoi(argv[i + 1]) - 1) * k, x1 = (atoi(argv[i + 2]) + 2) * k, y1 = (atoi(argv[i + 3]) + 2) * k;
      if (y0 < 0) y0 = 0; if (y1 > H) y1 = H;
      for (int y = y0; y < y1; ++y) {
        uint8_t *l = d + (y * W + x0 - k) * 4, *r = d + (y * W + x1 + k) * 4;
        for (int x = x0; x < x1; ++x) { float t = (float)(x - x0) / (x1 - x0); uint8_t *o = d + (y * W + x) * 4; for (int c = 0; c < 4; ++c) o[c] = (uint8_t)(l[c] + (r[c] - l[c]) * t); }
      }
    }
    NSGraphicsContext *ctx = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
    [NSGraphicsContext setCurrentContext:ctx]; ctx.imageInterpolation = NSImageInterpolationHigh;
    for (int i = 3; i + 4 < argc; i += 5) {
      float bx0 = atoi(argv[i]) * k, by0 = atoi(argv[i + 1]) * k, bx1 = (atoi(argv[i + 2]) + 1) * k, by1 = (atoi(argv[i + 3]) + 1) * k;
      NSImage *g = [[NSImage alloc] initWithContentsOfFile:@(argv[i + 4])];
      NSImageRep *gr = g.representations.firstObject;
      float aspect = (float)gr.pixelsWide / gr.pixelsHigh;
      float gh = (by1 - by0) * scale, gw = gh * aspect;
      float cx = fill ? bx0 + gw / 2 : (bx0 + bx1) / 2, cy = H - (by0 + by1) / 2;
      [g drawInRect:NSMakeRect(cx - gw / 2, cy - gh / 2, gw, gh) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1];
    }
    [NSGraphicsContext setCurrentContext:nil];
    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(argv[2]) atomically:YES];
  }
  return 0;
}
