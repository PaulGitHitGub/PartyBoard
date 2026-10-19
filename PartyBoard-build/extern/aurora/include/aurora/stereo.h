#ifndef AURORA_STEREO_H
#define AURORA_STEREO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Stereo rendering for a VR headset (Android; elsewhere these do nothing).
//
// Between AuroraStereoBegin and AuroraStereoEnd, each GX draw made with a
// perspective projection is also drawn for two eyes, into an image holding
// both side by side (left eye on the left); orthographic draws (the HUD) go
// into a second image of their own. The hidden screen's world and HUD are
// skipped.
// For eye e, the draw's projection is replaced by clip[e]: a row-major
// 4x4 matrix taking a view-space position (what GXLoadPosMtxImm gives) to GX
// clip space, with GX's depth conventions (z/w from -1 at near to 0 at far).
//
// The images are AHardwareBuffers (RGBA8, GPU color output), registered
// together, `count` of each: eye images width x height (twice as wide as an
// eye), and HUD images hudWidth x hudHeight, where orthographic draws keep
// their game projection. Image i and HUD image i are drawn by the same frame.
// Apart, the eyes' multisampled pass covers the eyes only: with the HUD below
// them in one image, 41% of its pixels were the HUD or unused (3360x2960 for
// 3360x1760 of eyes, Quest 3, 2026-09-29), all cleared and written out.
// When the frame that drew into image i has been submitted to the GPU,
// `callback` runs on Aurora's render thread with i, the frame tag given to
// AuroraStereoBegin and a sync fd that signals once the GPU has written both
// of its images (-1: nothing to wait for). The callee owns the fd.

typedef void (*AuroraStereoCallback)(uint32_t image, uint64_t frameTag, int syncFd, bool hasWorld, void* user);

bool AuroraStereoRegisterImages(void* const* eyeBuffers, void* const* hudBuffers, uint32_t count, uint32_t width,
                                uint32_t height, uint32_t hudWidth, uint32_t hudHeight,
                                AuroraStereoCallback callback, void* user);
void AuroraStereoBegin(uint32_t image, const float clip[2][16], uint64_t frameTag);
void AuroraStereoEnd(void);
// Suppress the unused screen world even when all eye buffers are busy.
void AuroraStereoWorldOnly(bool enabled);
bool AuroraStereoActive(void);
// Dynamic resolution, before AuroraStereoBegin: the part of each eye's half
// drawn this frame, from its top-left corner (0: all of it).
void AuroraStereoSetEyeSize(uint32_t width, uint32_t height);
// Before AuroraStereoBegin: whether this frame draws the HUD image (true until
// told otherwise). A frame that does not leaves it untouched: the headset
// copies the HUD only from frames that drew it.
void AuroraStereoSetHud(bool draw);
// The headset does not show the game's own frame (the model replaces it):
// the frame is still drawn for the eyes, but not presented to the screen.
void AuroraStereoHideScreen(bool hidden);
// The draws that follow cannot reach the other eye (the game tested their
// bounds): drawn for both eyes at once, they skip the cut at the eye's edge,
// which costs the GPU its early depth rejection. Back to false after them.
void AuroraStereoSetUncut(bool uncut);
// The draws being recorded belong to one object, at this distance from the
// eyes (any unit, the same for every object); < 0: outside any object. Opaque
// draws of objects can then go to the GPU front to back (debug.partyboard.sort_opaque).
void AuroraStereoSetSortKey(float key);
// The pixels of eye `eye` (0 left, 1 right) the headset's lenses never show:
// `count` rectangles {x0, y0, x1, y1}, fractions of the eye's rectangle from
// its top-left corner (whatever part of it dynamic resolution draws). Each
// eye's pass fills them with the nearest depth before its draws, which then
// skip them (debug.partyboard.visibility_mask 0: not filled). Any thread.
void AuroraStereoSetHiddenArea(uint32_t eye, const float* rects, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif
