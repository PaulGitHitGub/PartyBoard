// btnart <outdir>: draws PartyBoard's controller button prompts for Xbox,
// PlayStation and Switch Pro in the style of Mario Party 4's own icons: a chunky
// glossy button with a darker side band (the GameCube icons' cylinder look),
// each controller's real colours and markings on top. Every glyph is drawn twice:
// <name>.png at 96x96 (for the 24x24 icon slots) and <name>_wide.png at 120x96
// (for the 30x24 Z slot). Files: <outdir>/<family>/<glyph>[_wide].png
#import <AppKit/AppKit.h>

static NSColor *rgb(int hex, CGFloat a) { return [NSColor colorWithSRGBRed:((hex >> 16) & 255) / 255.0 green:((hex >> 8) & 255) / 255.0 blue:(hex & 255) / 255.0 alpha:a]; }
static NSFont *heavy(CGFloat size) {
  NSFont *f = [NSFont systemFontOfSize:size weight:NSFontWeightHeavy];
  NSFontDescriptor *d = [f.fontDescriptor fontDescriptorWithDesign:NSFontDescriptorSystemDesignRounded];
  return [NSFont fontWithDescriptor:d size:size] ?: f;
}

// The button body: a side band (darker, offset down) under a glossy top face.
static void body(NSBezierPath *top, CGFloat depth, int topHi, int topLo, int side) {
  NSAffineTransform *down = [NSAffineTransform transform]; [down translateXBy:0 yBy:-depth];
  NSBezierPath *band = [top copy]; [band transformUsingAffineTransform:down];
  [rgb(side, 1) setFill]; [band fill];
  // Join the band to the face so the sides read as one solid.
  for (CGFloat t = 0; t < depth; t += 1) {
    NSAffineTransform *s = [NSAffineTransform transform]; [s translateXBy:0 yBy:-t];
    NSBezierPath *p = [top copy]; [p transformUsingAffineTransform:s]; [p fill];
  }
  NSGradient *g = [[NSGradient alloc] initWithStartingColor:rgb(topHi, 1) endingColor:rgb(topLo, 1)];
  [g drawInBezierPath:top angle:-60];
  // Gloss: a soft highlight across the upper part of the face.
  NSRect b = top.bounds;
  NSBezierPath *gloss = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(NSMinX(b) + b.size.width * 0.14, NSMinY(b) + b.size.height * 0.48,
                                                                          b.size.width * 0.72, b.size.height * 0.46)];
  [NSGraphicsContext saveGraphicsState]; [top addClip];
  NSGradient *gg = [[NSGradient alloc] initWithStartingColor:[NSColor colorWithWhite:1 alpha:0.30] endingColor:[NSColor colorWithWhite:1 alpha:0.0]];
  [gg drawInBezierPath:gloss angle:-90];
  [NSGraphicsContext restoreGraphicsState];
  [rgb(0x08090B, 1) setStroke]; top.lineWidth = 2.5; [top stroke];
  band.lineWidth = 2.5; [band stroke];
}

static void text(NSString *s, NSPoint c, CGFloat size, NSColor *col) {
  NSShadow *sh = [NSShadow new]; sh.shadowColor = [NSColor colorWithWhite:0 alpha:0.55]; sh.shadowOffset = NSMakeSize(0, -2); sh.shadowBlurRadius = 1.5;
  NSDictionary *a = @{NSFontAttributeName: heavy(size), NSForegroundColorAttributeName: col, NSShadowAttributeName: sh};
  NSSize z = [s sizeWithAttributes:a];
  [s drawAtPoint:NSMakePoint(c.x - z.width / 2, c.y - z.height / 2 + size * 0.04) withAttributes:a];
}

static void psSymbol(NSString *which, NSPoint c, CGFloat r, NSColor *col) {
  NSBezierPath *p = [NSBezierPath bezierPath]; p.lineWidth = r * 0.30; p.lineCapStyle = NSLineCapStyleRound; p.lineJoinStyle = NSLineJoinStyleRound;
  if ([which isEqual:@"cross"]) {
    CGFloat d = r * 0.78; [p moveToPoint:NSMakePoint(c.x - d, c.y - d)]; [p lineToPoint:NSMakePoint(c.x + d, c.y + d)];
    [p moveToPoint:NSMakePoint(c.x - d, c.y + d)]; [p lineToPoint:NSMakePoint(c.x + d, c.y - d)];
  } else if ([which isEqual:@"circle"]) {
    [p appendBezierPathWithOvalInRect:NSMakeRect(c.x - r * 0.86, c.y - r * 0.86, r * 1.72, r * 1.72)];
  } else if ([which isEqual:@"square"]) {
    [p appendBezierPathWithRect:NSMakeRect(c.x - r * 0.74, c.y - r * 0.74, r * 1.48, r * 1.48)];
  } else {
    [p moveToPoint:NSMakePoint(c.x, c.y + r * 0.95)]; [p lineToPoint:NSMakePoint(c.x + r * 0.95, c.y - r * 0.68)];
    [p lineToPoint:NSMakePoint(c.x - r * 0.95, c.y - r * 0.68)]; [p closePath];
  }
  NSShadow *sh = [NSShadow new]; sh.shadowColor = [NSColor colorWithWhite:0 alpha:0.55]; sh.shadowOffset = NSMakeSize(0, -2); sh.shadowBlurRadius = 1.5;
  [NSGraphicsContext saveGraphicsState]; [sh set]; [col setStroke]; [p stroke]; [NSGraphicsContext restoreGraphicsState];
}

typedef enum { FACE, BUMPER, TRIGGER, STICK, START, DPAD } Kind;
typedef struct { const char *file; Kind kind; const char *label; int color; const char *symbol; } Glyph;

static void draw(const Glyph *g, int W, int H) {
  CGFloat cx = W / 2.0;
  const int hi = 0x4A4E55, lo = 0x17191C, side = 0x0B0C0E;
  switch (g->kind) {
  case FACE: {
    NSRect r = NSMakeRect(cx - 40, 18, 80, 72);
    NSBezierPath *top = [NSBezierPath bezierPathWithOvalInRect:r];
    body(top, 9, hi, lo, side);
    NSPoint c = NSMakePoint(NSMidX(r), NSMidY(r));
    if (g->symbol) psSymbol(@(g->symbol), c, 22, rgb(g->color, 1));
    else text(@(g->label), c, 50, rgb(g->color, 1));
    break;
  }
  case BUMPER: {
    NSRect r = NSMakeRect(cx - 44, 28, 88, 50);
    NSBezierPath *top = [NSBezierPath bezierPathWithRoundedRect:r xRadius:22 yRadius:22];
    body(top, 9, hi, lo, side);
    text(@(g->label), NSMakePoint(NSMidX(r), NSMidY(r)), strlen(g->label) > 1 ? 34 : 40, rgb(0xF2F2F2, 1));
    break;
  }
  case TRIGGER: {
    // A trigger seen from the front: wider rounded top, narrower base.
    NSBezierPath *top = [NSBezierPath bezierPath];
    CGFloat b = 22, t = 88, wb = 32, wt = 42;
    [top moveToPoint:NSMakePoint(cx - wb, b + 8)];
    [top curveToPoint:NSMakePoint(cx + wb, b + 8) controlPoint1:NSMakePoint(cx - wb, b - 2) controlPoint2:NSMakePoint(cx + wb, b - 2)];
    [top lineToPoint:NSMakePoint(cx + wt, t - 26)];
    [top curveToPoint:NSMakePoint(cx - wt, t - 26) controlPoint1:NSMakePoint(cx + wt, t + 4) controlPoint2:NSMakePoint(cx - wt, t + 4)];
    [top closePath];
    body(top, 9, hi, lo, side);
    text(@(g->label), NSMakePoint(cx, 52), strlen(g->label) > 1 ? 34 : 40, rgb(0xF2F2F2, 1));
    break;
  }
  case START: {
    // Small round menu button. label: "menu" draws the Xbox three-line icon,
    // "+" the Switch plus; anything else is text (PlayStation OPTIONS).
    NSRect r = NSMakeRect(cx - 34, 24, 68, 62);
    NSBezierPath *top = [NSBezierPath bezierPathWithOvalInRect:r];
    body(top, 8, hi, lo, side);
    NSPoint c = NSMakePoint(NSMidX(r), NSMidY(r));
    NSShadow *sh = [NSShadow new]; sh.shadowColor = [NSColor colorWithWhite:0 alpha:0.55]; sh.shadowOffset = NSMakeSize(0, -2); sh.shadowBlurRadius = 1.5;
    [NSGraphicsContext saveGraphicsState]; [sh set]; [rgb(0xF2F2F2, 1) set];
    if (!strcmp(g->label, "menu")) {
      for (int k = -1; k <= 1; ++k) { NSBezierPath *l = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(c.x - 17, c.y + k * 11 - 3, 34, 6) xRadius:3 yRadius:3]; [l fill]; }
    } else if (!strcmp(g->label, "+")) {
      [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(c.x - 18, c.y - 5, 36, 10) xRadius:3 yRadius:3] fill];
      [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(c.x - 5, c.y - 18, 10, 36) xRadius:3 yRadius:3] fill];
    } else {
      [NSGraphicsContext restoreGraphicsState];
      text(@(g->label), c, 16, rgb(0xF2F2F2, 1));
      break;
    }
    [NSGraphicsContext restoreGraphicsState];
    break;
  }
  case DPAD: {
    NSBezierPath *top = [NSBezierPath bezierPath];
    CGFloat a = 13, b = 36, y0 = 52;
    [top appendBezierPathWithRoundedRect:NSMakeRect(cx - a, y0 - b, 2 * a, 2 * b) xRadius:6 yRadius:6];
    [top appendBezierPathWithRoundedRect:NSMakeRect(cx - b, y0 - a, 2 * b, 2 * a) xRadius:6 yRadius:6];
    NSBezierPath *cross = [NSBezierPath bezierPath];
    [cross moveToPoint:NSMakePoint(cx - a, y0 + b)]; [cross lineToPoint:NSMakePoint(cx + a, y0 + b)]; [cross lineToPoint:NSMakePoint(cx + a, y0 + a)];
    [cross lineToPoint:NSMakePoint(cx + b, y0 + a)]; [cross lineToPoint:NSMakePoint(cx + b, y0 - a)]; [cross lineToPoint:NSMakePoint(cx + a, y0 - a)];
    [cross lineToPoint:NSMakePoint(cx + a, y0 - b)]; [cross lineToPoint:NSMakePoint(cx - a, y0 - b)]; [cross lineToPoint:NSMakePoint(cx - a, y0 - a)];
    [cross lineToPoint:NSMakePoint(cx - b, y0 - a)]; [cross lineToPoint:NSMakePoint(cx - b, y0 + a)]; [cross lineToPoint:NSMakePoint(cx - a, y0 + a)]; [cross closePath];
    body(cross, 8, hi, lo, side);
    if (!strcmp(g->label, "ps")) {  // PlayStation's d-pad has four separate arrow keys: mark the gaps
      [rgb(0x08090B, 1) setStroke];
      NSBezierPath *gap = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(cx - 9, y0 - 9, 18, 18)]; gap.lineWidth = 3; [gap stroke];
    }
    break;
  }
  case STICK: {
    NSRect r = NSMakeRect(cx - 40, 16, 80, 76);
    NSBezierPath *top = [NSBezierPath bezierPathWithOvalInRect:r];
    body(top, 7, hi, lo, side);
    // The thumb cap's dished centre and grip ring.
    NSRect in = NSInsetRect(r, 13, 12);
    NSGradient *dish = [[NSGradient alloc] initWithStartingColor:rgb(0x101114, 1) endingColor:rgb(0x34373D, 1)];
    [dish drawInBezierPath:[NSBezierPath bezierPathWithOvalInRect:in] angle:-60];
    NSBezierPath *ring = [NSBezierPath bezierPathWithOvalInRect:NSInsetRect(r, 6, 6)]; ring.lineWidth = 2; [rgb(0x5C6068, 0.7) setStroke]; [ring stroke];
    text(@(g->label), NSMakePoint(NSMidX(r), NSMidY(r)), 34, rgb(0xF2F2F2, 1));
    break;
  }
  }
}


// Label only (no button body), centred in a WxH transparent image, for
// engraving onto the game's own 3D button textures (repaint3d.m).
static void drawLabel(const Glyph *g, int W, int H) {
  NSPoint c = NSMakePoint(W / 2.0, H / 2.0);
  CGFloat s = MIN(W, H);
  NSShadow *sh = [NSShadow new]; sh.shadowColor = [NSColor colorWithWhite:0 alpha:0.6]; sh.shadowOffset = NSMakeSize(s * 0.02, -s * 0.025); sh.shadowBlurRadius = s * 0.01;
  NSColor *col = g->kind == FACE ? rgb(g->color, 1) : rgb(0xF2F2F2, 1);
  if (g->kind == FACE && g->symbol) { psSymbol(@(g->symbol), c, s * 0.34, col); return; }
  if (g->kind == START || g->kind == DPAD) {
    [NSGraphicsContext saveGraphicsState]; [sh set]; [col set];
    if (g->kind == DPAD) {
      CGFloat a = s * 0.13, b = s * 0.40;
      [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(c.x - a, c.y - b, 2 * a, 2 * b) xRadius:a * 0.4 yRadius:a * 0.4] fill];
      [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(c.x - b, c.y - a, 2 * b, 2 * a) xRadius:a * 0.4 yRadius:a * 0.4] fill];
    } else if (!strcmp(g->label, "+")) {
      CGFloat a = s * 0.09, b = s * 0.34;
      [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(c.x - b, c.y - a, 2 * b, 2 * a) xRadius:a * 0.5 yRadius:a * 0.5] fill];
      [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(c.x - a, c.y - b, 2 * a, 2 * b) xRadius:a * 0.5 yRadius:a * 0.5] fill];
    } else {
      for (int k = -1; k <= 1; ++k)
        [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(c.x - s * 0.32, c.y + k * s * 0.2 - s * 0.055, s * 0.64, s * 0.11) xRadius:s * 0.055 yRadius:s * 0.055] fill];
    }
    [NSGraphicsContext restoreGraphicsState];
    return;
  }
  const char *label = g->label;
  CGFloat size = strlen(label) > 1 ? s * 0.56 : s * 0.78;
  NSDictionary *a = @{NSFontAttributeName: heavy(size), NSForegroundColorAttributeName: col, NSShadowAttributeName: sh};
  NSString *str = @(label); NSSize z = [str sizeWithAttributes:a];
  if (z.width > W * 0.92) { size *= W * 0.92 / z.width; a = @{NSFontAttributeName: heavy(size), NSForegroundColorAttributeName: col, NSShadowAttributeName: sh}; z = [str sizeWithAttributes:a]; }
  [str drawAtPoint:NSMakePoint(c.x - z.width / 2, c.y - z.height / 2 + size * 0.04) withAttributes:a];
}

static void renderLabel(NSString *path, const Glyph *g, int W, int H) {
  NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:W pixelsHigh:H bitsPerSample:8 samplesPerPixel:4
      hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
  NSGraphicsContext *ctx = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
  [NSGraphicsContext saveGraphicsState]; [NSGraphicsContext setCurrentContext:ctx];
  [[NSColor clearColor] setFill]; NSRectFillUsingOperation(NSMakeRect(0, 0, W, H), NSCompositingOperationCopy);
  drawLabel(g, W, H);
  [NSGraphicsContext restoreGraphicsState];
  [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES];
}

static void render(NSString *path, const Glyph *g, int W, int H) {
  NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:W pixelsHigh:H bitsPerSample:8 samplesPerPixel:4
      hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
  NSGraphicsContext *ctx = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
  [NSGraphicsContext saveGraphicsState]; [NSGraphicsContext setCurrentContext:ctx];
  [[NSColor clearColor] setFill]; NSRectFillUsingOperation(NSMakeRect(0, 0, W, H), NSCompositingOperationCopy);
  ctx.shouldAntialias = YES; draw(g, W, H);
  [NSGraphicsContext restoreGraphicsState];
  [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES];
}

int main(int argc, char **argv) {
  @autoreleasepool {
    NSString *out = @(argv[1]);
    // Glyphs are named by the controller's button position (SDL's names), so
    // the game can pick them from its own button mapping.
    struct { const char *family; Glyph glyphs[14]; int n; } families[] = {
      {"xbox", {
        {"south", FACE, "A", 0x6CC24A, NULL}, {"east", FACE, "B", 0xF0453A, NULL},
        {"west", FACE, "X", 0x3E8DF0, NULL}, {"north", FACE, "Y", 0xF6C61C, NULL},
        {"left_shoulder", BUMPER, "LB", 0, NULL}, {"right_shoulder", BUMPER, "RB", 0, NULL},
        {"left_trigger", TRIGGER, "LT", 0, NULL}, {"right_trigger", TRIGGER, "RT", 0, NULL},
        {"left_stick", STICK, "L", 0, NULL}, {"right_stick", STICK, "R", 0, NULL},
        {"start", START, "menu", 0, NULL}, {"dpad", DPAD, "", 0, NULL}}, 12},
      {"playstation", {
        {"south", FACE, NULL, 0x7DB4F0, "cross"}, {"east", FACE, NULL, 0xF26B6B, "circle"},
        {"west", FACE, NULL, 0xE897CF, "square"}, {"north", FACE, NULL, 0x3FCDB0, "triangle"},
        {"left_shoulder", BUMPER, "L1", 0, NULL}, {"right_shoulder", BUMPER, "R1", 0, NULL},
        {"left_trigger", TRIGGER, "L2", 0, NULL}, {"right_trigger", TRIGGER, "R2", 0, NULL},
        {"left_stick", STICK, "L", 0, NULL}, {"right_stick", STICK, "R", 0, NULL},
        {"start", START, "menu", 0, NULL}, {"dpad", DPAD, "ps", 0, NULL}}, 12},
      {"switch", {
        // Nintendo labels: the bottom button is B, the right one A.
        {"south", FACE, "B", 0xEDEDED, NULL}, {"east", FACE, "A", 0xEDEDED, NULL},
        {"west", FACE, "Y", 0xEDEDED, NULL}, {"north", FACE, "X", 0xEDEDED, NULL},
        {"left_shoulder", BUMPER, "L", 0, NULL}, {"right_shoulder", BUMPER, "R", 0, NULL},
        {"left_trigger", TRIGGER, "ZL", 0, NULL}, {"right_trigger", TRIGGER, "ZR", 0, NULL},
        {"left_stick", STICK, "L", 0, NULL}, {"right_stick", STICK, "R", 0, NULL},
        {"start", START, "+", 0, NULL}, {"dpad", DPAD, "", 0, NULL}}, 12},
    };
    for (int f = 0; f < 3; ++f) {
      NSString *dir = [out stringByAppendingPathComponent:@(families[f].family)];
      [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
      for (int i = 0; i < families[f].n; ++i) {
        const Glyph *g = &families[f].glyphs[i];
        render([dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%s.png", g->file]], g, 96, 96);
        render([dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%s_wide.png", g->file]], g, 120, 96);
        NSString *ldir = [dir stringByAppendingPathComponent:@"label"];
        [[NSFileManager defaultManager] createDirectoryAtPath:ldir withIntermediateDirectories:YES attributes:nil error:nil];
        renderLabel([ldir stringByAppendingPathComponent:[NSString stringWithFormat:@"%s.png", g->file]], g, 256, 256);
        renderLabel([ldir stringByAppendingPathComponent:[NSString stringWithFormat:@"%s_wide.png", g->file]], g, 512, 256);
      }
    }
  }
  return 0;
}
