// stack <out.png> <x> <y> <w> <h> <width> <label> <image> [<label> <image> ...]
// Crops the same region (fractions x1000 of each image) and stacks the crops
// vertically at <width> pixels wide, each with its label on the left.
#import <AppKit/AppKit.h>
int main(int argc, char **argv) {
  @autoreleasepool {
    double x = atoi(argv[2]) / 1000.0, y = atoi(argv[3]) / 1000.0, w = atoi(argv[4]) / 1000.0, h = atoi(argv[5]) / 1000.0;
    int W = atoi(argv[6]), n = (argc - 7) / 2, labelW = 210, gap = 8;
    NSImage *first = [[NSImage alloc] initWithContentsOfFile:@(argv[8])];
    double aspect = (first.size.height * h) / (first.size.width * w);
    int H = (int)(W * aspect), total = n * (H + gap);
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:W + labelW pixelsHigh:total
        bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
    NSGraphicsContext *ctx = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
    [NSGraphicsContext setCurrentContext:ctx]; ctx.imageInterpolation = NSImageInterpolationHigh;
    [[NSColor colorWithWhite:0.1 alpha:1] setFill]; NSRectFill(NSMakeRect(0, 0, W + labelW, total));
    for (int i = 0; i < n; ++i) {
      NSImage *img = [[NSImage alloc] initWithContentsOfFile:@(argv[8 + i * 2])];
      NSSize s = img.size; int top = total - (i + 1) * (H + gap) + gap / 2;
      [img drawInRect:NSMakeRect(labelW, top, W, H) fromRect:NSMakeRect(x * s.width, (1 - y - h) * s.height, w * s.width, h * s.height)
            operation:NSCompositingOperationCopy fraction:1];
      NSDictionary *a = @{NSFontAttributeName: [NSFont boldSystemFontOfSize:26], NSForegroundColorAttributeName: NSColor.whiteColor};
      NSString *label = @(argv[7 + i * 2]); NSSize ls = [label sizeWithAttributes:a];
      [label drawAtPoint:NSMakePoint(16, top + (H - ls.height) / 2) withAttributes:a];
    }
    [NSGraphicsContext setCurrentContext:nil];
    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(argv[1]) atomically:YES];
  }
  return 0;
}
