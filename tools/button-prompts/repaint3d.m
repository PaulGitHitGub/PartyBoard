// repaint3d <orig.png> <out.png> <x0> <y0> <x1> <y1> <label.png> [threshold]
// Repaints one of the game's 3D button textures for another controller while
// keeping the original art's shading: the letter inside the box (original
// pixel coordinates) is found as what stands out from the smooth surface
// around it, erased by diffusion inpainting, the plastic is recoloured to the
// dark grey of Xbox/PlayStation/Switch buttons with its lighting kept, the
// result is upscaled 4x (Catmull-Rom), and the new label is drawn in the box
// ("-" for no label: the base the game composes labels onto at run time).
#import <AppKit/AppKit.h>
static int w, h;
static float *R, *G, *B, *A;
static float lum(int i) { return 0.299f * R[i] + 0.587f * G[i] + 0.114f * B[i]; }
static float cr(float p0, float p1, float p2, float p3, float t) {
  return 0.5f * (2 * p1 + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
}
static float S(float *c, int x, int y) { x = x < 0 ? 0 : x >= w ? w - 1 : x; y = y < 0 ? 0 : y >= h ? h - 1 : y; return c[y * w + x]; }
int main(int argc, char **argv) {
  @autoreleasepool {
    NSBitmapImageRep *rep = [NSBitmapImageRep imageRepWithData:[NSData dataWithContentsOfFile:@(argv[1])]];
    w = (int)rep.pixelsWide; h = (int)rep.pixelsHigh;
    int x0 = atoi(argv[3]), y0 = atoi(argv[4]), x1 = atoi(argv[5]), y1 = atoi(argv[6]);
    float T = argc > 8 ? atof(argv[8]) : 0.07f;
    R = calloc(w * h, 4); G = calloc(w * h, 4); B = calloc(w * h, 4); A = calloc(w * h, 4);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
      NSUInteger p[4] = {0, 0, 0, 255}; [rep getPixel:p atX:x y:y];
      int i = y * w + x; R[i] = p[0] / 255.f; G[i] = p[1] / 255.f; B[i] = p[2] / 255.f; A[i] = rep.samplesPerPixel >= 4 ? p[3] / 255.f : 1;
    }
    // Letter mask: pixels that differ from a wide local average, twice (the
    // second pass compares with the first inpainting), then grown by 1px.
    uint8_t *mask = calloc(w * h, 1);
    float *bg = malloc(w * h * 4);
    int rad = MAX(3, MIN(w, h) / 10);
    for (int pass = 0; pass < 2; ++pass) {
      for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        float s = 0; int n = 0;
        for (int dy = -rad; dy <= rad; ++dy) for (int dx = -rad; dx <= rad; ++dx) {
          int xx = x + dx, yy = y + dy; if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
          int j = yy * w + xx; if (pass && mask[j]) continue; s += lum(j); ++n;
        }
        bg[y * w + x] = n ? s / n : lum(y * w + x);
      }
      for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) if (fabsf(lum(y * w + x) - bg[y * w + x]) > T) mask[y * w + x] = 1;
    }
    uint8_t *m2 = calloc(w * h, 1);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) if (mask[y * w + x])
      for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) { int xx = x + dx, yy = y + dy; if (xx >= 0 && yy >= 0 && xx < w && yy < h) m2[yy * w + xx] = 1; }
    // Inpaint: masked pixels relax to the average of their neighbours.
    for (int it = 0; it < 600; ++it) for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
      int i = y * w + x; if (!m2[i]) continue;
      float r = 0, g = 0, b = 0; int n = 0;
      int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
      for (int k = 0; k < 4; ++k) { int xx = x + nb[k][0], yy = y + nb[k][1]; if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue; int j = yy * w + xx; r += R[j]; g += G[j]; b += B[j]; ++n; }
      R[i] = r / n; G[i] = g / n; B[i] = b / n;
    }
    // Recolour to dark plastic, keeping relative lighting.
    double mean = 0; int cnt = 0;
    for (int i = 0; i < w * h; ++i) if (A[i] > 0.5f) { mean += lum(i); ++cnt; }
    mean = cnt ? mean / cnt : 0.5;
    for (int i = 0; i < w * h; ++i) {
      float v = fminf(1, lum(i) / mean * 0.20f);
      v = v < 0.6f ? v : 0.6f + (v - 0.6f) * 0.5f;
      R[i] = v * 0.94f; G[i] = v * 0.97f; B[i] = fminf(1, v * 1.06f);
    }
    // 4x upscale.
    int k = 4, W = w * k, H = h * k;
    NSBitmapImageRep *out = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:W pixelsHigh:H bitsPerSample:8 samplesPerPixel:4
        hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:0 bytesPerRow:W * 4 bitsPerPixel:32];
    uint8_t *d = out.bitmapData;
    float *ch[4] = {R, G, B, A};
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
      float fx = (x + 0.5f) / k - 0.5f, fy = (y + 0.5f) / k - 0.5f; int ix = floorf(fx), iy = floorf(fy); float tx = fx - ix, ty = fy - iy;
      float v[4];
      for (int c = 0; c < 4; ++c) { float col[4]; for (int j = 0; j < 4; ++j) col[j] = cr(S(ch[c], ix - 1, iy - 1 + j), S(ch[c], ix, iy - 1 + j), S(ch[c], ix + 1, iy - 1 + j), S(ch[c], ix + 2, iy - 1 + j), tx); v[c] = fmaxf(0, fminf(1, cr(col[0], col[1], col[2], col[3], ty))); }
      uint8_t *o = d + ((size_t)y * W + x) * 4;
      for (int c = 0; c < 3; ++c) o[c] = (uint8_t)(v[c] * v[3] * 255 + 0.5f);  // premultiplied
      o[3] = (uint8_t)(v[3] * 255 + 0.5f);
    }
    NSGraphicsContext *ctx = [NSGraphicsContext graphicsContextWithBitmapImageRep:out];
    [NSGraphicsContext setCurrentContext:ctx]; ctx.imageInterpolation = NSImageInterpolationHigh;
    NSImage *label = strcmp(argv[7], "-") ? [[NSImage alloc] initWithContentsOfFile:@(argv[7])] : nil;
    NSImageRep *lr = label ? label.representations.firstObject : nil;
    if (label) {
    float bw = (x1 - x0) * k, bh = (y1 - y0) * k, la = (float)lr.pixelsWide / lr.pixelsHigh;
    float lw = bw, lh = bw / la; if (lh > bh) { lh = bh; lw = bh * la; }
    float cx = (x0 + x1) / 2.f * k, cy = H - (y0 + y1) / 2.f * k;
    [label drawInRect:NSMakeRect(cx - lw / 2, cy - lh / 2, lw, lh) fromRect:NSZeroRect operation:NSCompositingOperationSourceAtop fraction:1];
    }
    [NSGraphicsContext setCurrentContext:nil];
    [[out representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(argv[2]) atomically:YES];
  }
  return 0;
}
