#pragma once

// Stereo rendering for a VR headset (include/aurora/stereo.h).

#include "common.hpp"
#include "../gx/gx.hpp"
#include "../gx/pipeline.hpp"
#include "stereo_order.hpp"

#include <string>
#include <vector>

namespace aurora::gfx::stereo {

// GX draws are being copied for the eyes (a perspective draw since
// AuroraStereoBegin). Each eye keeps its own projection uniforms.
bool recording() noexcept;
// Backpressure: consume perspective vertices without uploading hidden draws.
bool discard_world_draw() noexcept;
// The game's own frame is not on show in the headset: aurora.cpp skips
// presenting it, and gfx::finish skips its final pass. Latched once a frame
// by gfx::finish (game thread); screen_hidden() returns that frame's value.
bool latch_screen_hidden() noexcept;
bool screen_hidden() noexcept;
// Anisotropic filtering every mipmapped texture gets while a headset uses the
// eyes' images (1: the game's own choice).
uint16_t forced_anisotropy() noexcept;

// Only consecutive perspective draws in the same camera may share geometry.
gx::DrawData* last_world_draw() noexcept;
void mirror_merged_world_draw() noexcept;
void invalidate_world_merge() noexcept;

// Where each instance of a both-eyes draw goes (shader.cpp's stereo_x and
// stereo_clip). The viewport spans both eyes' rectangles, stride + width
// pixels: the left eye's are [0, width), the right eye's [stride, stride +
// width). Each instance's clip x is squeezed into its own, and cut at its
// edge: x/w <= 2 width/V - 1 for the left eye, >= 2 stride/V - 1 for the right.
inline void instanced_placement(float stride, float width, Vec4<float>& eyeX, Vec4<float>& eyeClip) noexcept {
  const float span = stride + width;
  const float scale = width / span;
  eyeX = {scale, scale - 1.f, scale, (width + 2.f * stride) / span - 1.f};
  eyeClip = {2.f * width / span - 1.f, 2.f * stride / span - 1.f, 0.f, 0.f};
}

// A GX draw just pushed to the game's frame: its eye copies, each with the
// eye's projection in its own uniforms (command_processor.cpp).
bool record_gx_draw(gx::DrawData draw, const gx::PipelineConfig& config, const gx::ShaderInfo& info, uint32_t vtxStart,
                    const gx::BindGroupRanges& ranges) noexcept;

// At the end of the frame's recording (gfx::finish): the eyes' render pass.
void end_frame() noexcept;

// An instanced (both eyes) pipeline did not build (gx/pipeline.cpp): each eye
// is drawn on its own from then on.
void instanced_failed(const std::string& message) noexcept;

// On the render thread: the image is about to be drawn (render pass encode),
// and the frame using it was just submitted (aurora.cpp).
void begin_access(int32_t image, uint64_t tag, bool hasWorld, bool hud) noexcept;
// Render thread: GPU timestamps around the eyes' pass (hud false) and the
// HUD's (hud true) of a frame: the writes for its descriptor (nullptr: none),
// then, once the pass ended, their resolve into a readback buffer.
const wgpu::PassTimestampWrites* pass_timestamps(bool hud) noexcept;
void pass_timestamps_resolve(wgpu::CommandEncoder& cmd, bool hud) noexcept;
void after_submit() noexcept;

// What common.cpp turns into a render pass: the eyes' (hud null), or the
// interface's (eyes empty, image -1: the eyes' pass began the access to both).
struct PassDesc {
  const char* label = "Stereo eyes";
  wgpu::TextureView color;
  wgpu::TextureView resolve;
  wgpu::TextureView depth;
  wgpu::Extent3D size;
  uint32_t sampleCount = 1;
  uint32_t eyeStride = 0; // an eye's half of the image
  uint32_t eyeWidth = 0;  // the part of it drawn this frame (dynamic resolution)
  uint32_t eyeHeight = 0;
  uint32_t hudTop = 0;    // eyes' pass: the image's rows (eyes centered in them); HUD pass: 0
  uint32_t hudWidth = 0;
  uint32_t hudHeight = 0;
  int32_t image = -1;
  uint64_t tag = 0;
  bool hasWorld = false;
  bool drawHud = false; // eyes' pass: the HUD pass follows (access to both images)
  const std::vector<gx::DrawData>* eyes[2]{};
  // Per world draw: 1 when eyes[0]'s draw covers both eyes (two instances).
  const std::vector<uint8_t>* instanced = nullptr;
  // Per world draw: its object and whether it may move (stereo_order.hpp);
  // sortOpaque sends the sortable runs front to back.
  const std::vector<DrawOrder>* order = nullptr;
  bool sortOpaque = false;
  // Eyes' pass: per eye, the rectangles the lenses never show ({x0, y0, x1,
  // y1}, 0..1 of the eye's rectangle), filled with the nearest depth first.
  const std::vector<float>* hidden[2]{};
  const std::vector<gx::DrawData>* hud = nullptr;
};
void enqueue_pass(const PassDesc& desc) noexcept; // common.cpp

} // namespace aurora::gfx::stereo
