#include "stereo.hpp"

#include <aurora/stereo.h>

#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "../gx/shader_info.hpp"
#include "../gx/fifo.hpp"
#include "pipeline_cache.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_set>

#if defined(__ANDROID__)
#include <android/hardware_buffer.h>
#include <linux/sync_file.h>
#include <sys/ioctl.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

namespace aurora::gfx::stereo {
namespace {
Module Log("aurora::gfx::stereo");

#if defined(__ANDROID__)
struct Image {
  wgpu::SharedTextureMemory memory;
  wgpu::Texture texture;
  wgpu::TextureView view;
  // The interface's image, drawn by the same frame in a pass of its own.
  wgpu::SharedTextureMemory hudMemory;
  wgpu::Texture hudTexture;
  wgpu::TextureView hudView;
};

// Set once by AuroraStereoRegisterImages (game thread), then read by both threads.
std::mutex g_imagesMutex;
std::vector<Image> g_images;
uint32_t g_width = 0;
uint32_t g_height = 0;
uint32_t g_eyeHeight = 0, g_hudWidth = 0, g_hudHeight = 0;
// The eyes' multisampling. On the headset's tiled GPU (Adreno 740) 4x takes
// four times the tile memory per pixel: smaller bins, and every draw is
// processed again in each bin it touches. For a headset A/B, before starting
// the game: `adb shell setprop debug.partyboard.stereo_msaa 1` (no MSAA).
// WebGPU only has 1 and 4 samples: any other value keeps 4 (2x aborted the
// game on its first eye frame, 2026-09-28).
uint32_t startup_samples() {
  static const uint32_t samples = [] {
    char value[PROP_VALUE_MAX] = {};
    __system_property_get("debug.partyboard.stereo_msaa", value);
    const uint32_t chosen = value[0] == '1' && value[1] == '\0' ? 1u : 4u;
    Log.info("Stereo: MSAA {}x (debug.partyboard.stereo_msaa={})", chosen, value[0] != '\0' ? value : "unset");
    return chosen;
  }();
  return samples;
}
// Both eyes in one draw halve the draws the board is limited by (414 per eye
// kept the Toad board at 43-45 images/s whatever the resolution, 2026-09-28).
// Quest Vulkan fails to create the clip-distance pipeline (VK_ERROR_UNKNOWN,
// build 88), so each instance is cut at its eye's edge by a fragment test by
// default, which needs no device feature. Before starting the game:
//   adb shell setprop debug.partyboard.instanced_stereo 0   one draw per eye
//   adb shell setprop debug.partyboard.instanced_stereo 1   clip distances
//   adb shell setprop debug.partyboard.instanced_stereo 2   fragment test (default)
// A pipeline that fails validation falls back to one draw per eye (gx/pipeline.cpp).
uint8_t instanced_stereo_mode() {
  static const uint8_t mode = []() -> uint8_t {
    char value[PROP_VALUE_MAX] = {};
    __system_property_get("debug.partyboard.instanced_stereo", value);
    const uint8_t chosen = value[0] == '0' ? gx::StereoOff : value[0] == '1' ? gx::StereoClipDistance : gx::StereoDiscard;
    if (chosen == gx::StereoClipDistance && !webgpu::g_clipDistancesSupported) {
      Log.warn("Stereo: clip distances unsupported, instanced drawing stays off");
      return gx::StereoOff;
    }
    Log.info("Stereo: {} (debug.partyboard.instanced_stereo={})",
             chosen == gx::StereoOff ? "one draw per eye" : chosen == gx::StereoClipDistance
                 ? "both eyes in one draw, cut by clip distances" : "both eyes in one draw, cut by a fragment test",
             value[0] != '\0' ? value : "unset");
    return chosen;
  }();
  return mode;
}
AuroraStereoCallback g_callback = nullptr;
void* g_callbackUser = nullptr;

// Game thread: the frame being recorded.
bool g_recording = false;
bool g_worldMergeable = false;
bool g_worldOnly = false;
// The draws being recorded cannot reach the other eye (AuroraStereoSetUncut).
bool g_uncut = false;
int32_t g_image = -1;
uint64_t g_frameTag = 0;
bool g_loggedGeometry = false;
// Dynamic resolution: the part of each eye's half drawn this frame (0: all of it).
uint32_t g_frameEyeWidth = 0, g_frameEyeHeight = 0;
// Whether this frame draws the HUD image (AuroraStereoSetHud).
bool g_frameDrawHud = true;
// The lenses' hidden area (AuroraStereoSetHiddenArea): per eye, rectangles
// {x0, y0, x1, y1} of the eye's rectangle, 0..1 from its top-left. Set by the
// headset's thread, copied per frame by the game's.
std::mutex g_hiddenMutex;
std::vector<float> g_hiddenArea[2];
std::vector<float> g_frameHidden[2];
// The game's own frame is not on show in the headset (aurora.cpp skips presenting it).
std::atomic_bool g_screenHidden = false;
// 8x once a headset uses the eyes' images (gx.cpp's samplers read it), or
// debug.partyboard.anisotropy (Switches).
std::atomic<uint16_t> g_forcedAnisotropy{1};
std::atomic_bool g_imagesRegistered = false;
std::array<Mat4x4<float>, 2> g_clip{};
std::array<std::vector<gx::DrawData>, 2> g_eyeDraws;
// Per world draw: 1 when both eyes are one instanced draw (g_eyeDraws[0]
// holds it, g_eyeDraws[1] a copy that is not drawn), 0 when drawn per eye.
std::vector<uint8_t> g_eyeInstanced;
// Where each instance goes, for the instanced shaders (shader.cpp's stereo_x
// and stereo_clip), from the image's and this frame's eye sizes.
Vec4<float> g_eyeX{}, g_eyeClip{};
// An instanced pipeline failed to build: every eye drawn on its own from then on.
std::atomic_bool g_instancedDisabled = false;
uint64_t g_statInstanced = 0, g_statPerEye = 0;
// The object the draws being recorded belong to (AuroraStereoSetSortKey), and
// per world draw its object and whether it may move (stereo_order.hpp).
float g_sortKey = -1.f;
uint32_t g_sortObject = 0;
std::vector<DrawOrder> g_eyeOrder;
uint64_t g_statSortable = 0, g_statCrossingPerEye = 0;
// The world draws by kind (DrawKind), for "Stereo kinds" and
// debug.partyboard.skip_draws: what each kind costs the GPU is what removing
// it saves, on a frozen scene (debug.partyboard.freeze).
enum DrawKind : uint32_t {
  KindOpaque = 1u << 0,    // sortable: an object's opaque draw
  KindBlend = 1u << 1,     // blended and not sortable (the game's translucent pass)
  KindAdditive = 1u << 2,  // blended onto the target (dst factor ONE)
  KindNoDepth = 1u << 3,   // no depth test: every fragment shaded
  KindAlphaTest = 1u << 4, // alpha compare: fragments discarded in the shader
  KindOutside = 1u << 5,   // outside any object (effects, scenery, shadows)
  KindTev3 = 1u << 6,      // three TEV stages or more
  KindFog = 1u << 7,       // fogged
  KindIndirect = 1u << 8,  // indirect texturing
};
constexpr std::pair<const char*, uint32_t> kDrawKinds[] = {
    {"opaque", KindOpaque}, {"blend", KindBlend}, {"additive", KindAdditive},   {"nodepth", KindNoDepth},
    {"alphatest", KindAlphaTest}, {"outside", KindOutside}, {"tev3", KindTev3}, {"fog", KindFog},
    {"indirect", KindIndirect},
};
uint64_t g_statKinds[std::size(kDrawKinds)]{};
uint64_t g_statKindsTotal = 0, g_statSkipped = 0;
// Headset A/B switches, read again every 2 s so a capture can flip them
// without restarting the game. On the Toad board (Quest 3, build 118,
// 2026-09-28) the sort with the opaque blend off shaded 35% fewer fragments
// per image than neither (59% GPU instead of 82%, 53 ms motion-to-photon
// instead of 68), with no difference the player could see: both are on by
// default.
//   debug.partyboard.sort_opaque 0       opaque draws of objects in the game's
//                                        order (default: front to back)
//   debug.partyboard.stereo_crossing 1   an object that reaches both eyes is
//                                        drawn once per eye: 11% fewer
//                                        fragments shaded (build 117,
//                                        2026-09-28), but twice its draws in
//                                        every bin, and the bins' draws are
//                                        what the eyes' pass spends most on
//                                        (2026-10-02: fragments +9%, GPU -11%
//                                        as one instanced draw). By default,
//                                        one instanced draw cut by a fragment
//                                        test; every draw already has one (the
//                                        game's alpha test, GEQUAL 1).
//   debug.partyboard.opaque_blend off    the game's opaque objects drawn without
//                                        their alpha blend (edges of translucent
//                                        textures turn hard). Build 119 found no
//                                        gain over keeping it with the sort on,
//                                        so it stays on by default
//   debug.partyboard.anisotropy 1|2|4    the anisotropic filtering forced on the
//                                        eyes' 3D textures (8 by default, 16 at
//                                        most). The board is seen at a slant
//                                        across the table, where 8x reads the
//                                        most texels: an A/B tells what the
//                                        textures cost before compressing them
//   debug.partyboard.visibility_mask 0   the pixels the lenses never show are
//                                        not filled with the nearest depth
//                                        first, so the draws shade them too
struct Switches {
  bool sortOpaque = true;
  bool crossingPerEye = false;
  bool opaqueBlendOff = false;
  uint16_t anisotropy = 8;
  bool hiddenArea = true;
  uint32_t skipKinds = 0; // debug.partyboard.skip_draws: DrawKind bits
  // debug.partyboard.alpha_test opaque-off: the opaque draws of objects lose
  // their alpha test (the game's GEQUAL 1, everywhere): no discard in their
  // shader, so the GPU tests and writes depth before shading them. A
  // measurement first: a cut-out texel (alpha 0) of such a draw then writes
  // depth and may hide what is behind it.
  bool alphaTestOff = false;
  // Live A/Bs of what the trace showed (docs/quest-optimization.md): the
  // eyes' pass in 63 bins (4x MSAA, 32-bit depth: 32 bytes a pixel in tile
  // memory) and 46-50 ALU instructions a fragment.
  //   debug.partyboard.eye_depth 32   32-bit depth (16 by default: 24 bytes a
  //                                   pixel, larger bins; -5% for the eyes' pass)
  //   debug.partyboard.eye_f16 0      the TEV in full precision (half by
  //                                   default where the GPU has it; -8%)
  //   debug.partyboard.eye_msaa 1|4   the eyes' samples (stereo_msaa: at start;
  //                                   1x was -22%, but its edges alias)
  // Measured on the frozen Toad board, Quest 3, 2026-10-02.
  bool depth16 = true;
  bool f16 = true;
  uint32_t msaa = 0; // 0: stereo_msaa's
};
Switches g_switches;

// The eyes' samples this frame: eye_msaa, else stereo_msaa's at start.
uint32_t stereo_samples() { return g_switches.msaa != 0 ? g_switches.msaa : startup_samples(); }
std::chrono::steady_clock::time_point g_switchesAt{};

void refresh_switches() {
  const auto now = std::chrono::steady_clock::now();
  if (g_switchesAt != std::chrono::steady_clock::time_point{} && now - g_switchesAt < std::chrono::seconds(2)) {
    return;
  }
  g_switchesAt = now;
  char value[PROP_VALUE_MAX] = {};
  Switches read;
  __system_property_get("debug.partyboard.sort_opaque", value);
  read.sortOpaque = value[0] != '0';
  value[0] = '\0';
  __system_property_get("debug.partyboard.stereo_crossing", value);
  read.crossingPerEye = value[0] == '1';
  value[0] = '\0';
  __system_property_get("debug.partyboard.opaque_blend", value);
  read.opaqueBlendOff = std::string_view{value} == "off";
  value[0] = '\0';
  __system_property_get("debug.partyboard.anisotropy", value);
  const std::string_view anisotropy{value};
  read.anisotropy = anisotropy == "1" ? 1 : anisotropy == "2" ? 2 : anisotropy == "4" ? 4 : anisotropy == "16" ? 16 : 8;
  value[0] = '\0';
  __system_property_get("debug.partyboard.visibility_mask", value);
  read.hiddenArea = value[0] != '0';
  value[0] = '\0';
  __system_property_get("debug.partyboard.eye_depth", value);
  read.depth16 = std::string_view{value} != "32";
  value[0] = '\0';
  __system_property_get("debug.partyboard.eye_f16", value);
  read.f16 = value[0] != '0' && webgpu::g_shaderF16Supported;
  value[0] = '\0';
  __system_property_get("debug.partyboard.eye_msaa", value);
  read.msaa = value[0] == '1' ? 1u : value[0] == '4' ? 4u : 0u;
  if (read.depth16 != g_switches.depth16 || read.f16 != g_switches.f16 || read.msaa != g_switches.msaa) {
    Log.info("Stereo: eyes' depth {} bits, TEV {}, MSAA {}x (eye_depth, eye_f16, eye_msaa)", read.depth16 ? 16 : 32,
             read.f16 ? "half precision" : "full precision", read.msaa != 0 ? read.msaa : startup_samples());
  }
  value[0] = '\0';
  __system_property_get("debug.partyboard.alpha_test", value);
  read.alphaTestOff = std::string_view{value} == "opaque-off";
  if (read.alphaTestOff != g_switches.alphaTestOff) {
    Log.info("Stereo: alpha test {} on opaque draws (debug.partyboard.alpha_test)", read.alphaTestOff ? "off" : "on");
  }
  value[0] = '\0';
  __system_property_get("debug.partyboard.skip_draws", value);
  for (const auto& [name, bit] : kDrawKinds) {
    if (std::strstr(value, name) != nullptr) read.skipKinds |= bit;
  }
  if (read.skipKinds != g_switches.skipKinds) {
    Log.info("Stereo: skipping world draws of kinds '{}' (debug.partyboard.skip_draws, a measurement)", value);
  }
  static bool logged = false;
  if (!logged || read.sortOpaque != g_switches.sortOpaque || read.crossingPerEye != g_switches.crossingPerEye ||
      read.opaqueBlendOff != g_switches.opaqueBlendOff || read.anisotropy != g_switches.anisotropy ||
      read.hiddenArea != g_switches.hiddenArea) {
    logged = true;
    Log.info("Stereo switches: sort_opaque={} stereo_crossing={} opaque_blend={} anisotropy={}x visibility_mask={}",
             read.sortOpaque ? 1 : 0, read.crossingPerEye ? 1 : 0, read.opaqueBlendOff ? "off" : "on", read.anisotropy,
             read.hiddenArea ? 1 : 0);
  }
  g_switches = read;
  // New samplers from the next draw on (gx.cpp builds them per descriptor).
  if (g_imagesRegistered.load(std::memory_order_relaxed)) {
    g_forcedAnisotropy.store(read.anisotropy, std::memory_order_relaxed);
  }
}
// Instanced pipelines seen built (game thread): asking the cache again would
// take its lock on every draw.
std::unordered_set<PipelineRef> g_readyInstanced;
std::vector<gx::DrawData> g_hudDraws;
webgpu::TextureWithSampler g_color; // multisampled, when the game's frame is
webgpu::TextureWithSampler g_depth;
webgpu::TextureWithSampler g_hudColor, g_hudDepth; // the same for the interface's pass

// Render thread: images drawn into by the frame being encoded.
struct Accessed {
  int32_t image;
  uint64_t tag;
  bool hasWorld;
  bool hud; // the HUD image was accessed too
};
std::vector<Accessed> g_accessed;

Mat4x4<float> to_mat(const float m[16]) noexcept {
  return {
      {m[0], m[1], m[2], m[3]},
      {m[4], m[5], m[6], m[7]},
      {m[8], m[9], m[10], m[11]},
      {m[12], m[13], m[14], m[15]},
  };
}

webgpu::TextureWithSampler create_target(wgpu::TextureFormat format, uint32_t sampleCount, uint32_t width,
                                         uint32_t height, const char* label) {
  // Multisampled color and depth are cleared, drawn and resolved within the
  // pass and never stored: on the headset's tiled GPU, transient attachments
  // live in tile memory only (no RAM, no bandwidth).
  const bool transient = sampleCount > 1 && webgpu::g_device.HasFeature(wgpu::FeatureName::TransientAttachments);
  const wgpu::TextureDescriptor descriptor{
      .label = label,
      .usage = transient ? wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TransientAttachment
                         : wgpu::TextureUsage::RenderAttachment,
      .dimension = wgpu::TextureDimension::e2D,
      .size = {width, height, 1},
      .format = format,
      .mipLevelCount = 1,
      .sampleCount = sampleCount,
  };
  webgpu::TextureWithSampler target;
  target.texture = webgpu::g_device.CreateTexture(&descriptor);
  target.view = target.texture.CreateView();
  target.size = descriptor.size;
  target.format = format;
  return target;
}

// One sync fd for both of a frame's images (the callee waits on one). They
// are written by the same submission, but each shared memory gives its own
// fence. -1 when neither has one.
int merge_fences(int first, int second) {
  if (first < 0) return second;
  if (second < 0) return first;
  sync_merge_data merge{};
  std::strncpy(merge.name, "stereo eyes+hud", sizeof(merge.name) - 1);
  merge.fd2 = second;
  if (ioctl(first, SYNC_IOC_MERGE, &merge) == 0) {
    close(first);
    close(second);
    return merge.fence;
  }
  // Not merged: the eyes' fence, the one the headset shows first. Both come
  // from the same submission, so it signals with the interface's.
  static bool warned = false;
  if (!warned) {
    warned = true;
    Log.warn("Stereo: could not merge the eyes' and HUD's fences ({}), waiting on the eyes'", std::strerror(errno));
  }
  close(second);
  return first;
}
#endif
} // namespace

bool recording() noexcept {
#if defined(__ANDROID__)
  return g_image >= 0;
#else
  return false;
#endif
}

uint16_t forced_anisotropy() noexcept {
#if defined(__ANDROID__)
  // Set once images are registered, i.e. a headset shows the eyes' images.
  return g_forcedAnisotropy.load(std::memory_order_relaxed);
#else
  return 1;
#endif
}

// The headset thread may flip it at any time; gfx::finish latches it once a
// frame, so skipping the final pass and not presenting agree (a skipped pass
// presented would show an old frame).
bool g_frameScreenHidden = false; // game thread

bool latch_screen_hidden() noexcept {
#if defined(__ANDROID__)
  g_frameScreenHidden = g_screenHidden.load(std::memory_order_relaxed);
#endif
  return g_frameScreenHidden;
}

bool screen_hidden() noexcept {
#if defined(__ANDROID__)
  return g_frameScreenHidden;
#else
  return false;
#endif
}

bool discard_world_draw() noexcept {
#if defined(__ANDROID__)
  return g_worldOnly && g_image < 0 && !is_offscreen() && gx::g_gxState.projType == GX_PERSPECTIVE;
#else
  return false;
#endif
}

void invalidate_world_merge() noexcept {
#if defined(__ANDROID__)
  g_worldMergeable = false;
#endif
}

gx::DrawData* last_world_draw() noexcept {
#if defined(__ANDROID__)
  if (g_worldMergeable && g_recording && g_image >= 0 && !is_offscreen() &&
      gx::g_gxState.projType == GX_PERSPECTIVE && !g_eyeDraws[0].empty()) {
    return &g_eyeDraws[0].back();
  }
#endif
  return nullptr;
}

void mirror_merged_world_draw() noexcept {
#if defined(__ANDROID__)
  auto& source = g_eyeDraws[0].back();
  auto& target = g_eyeDraws[1].back();
  target.vertRange = source.vertRange;
  target.idxRange = source.idxRange;
  target.vtxCount = source.vtxCount;
  target.indexCount = source.indexCount;
  // Keep the right eye's projection uniforms and all pipeline state intact.
#endif
}

bool record_gx_draw(gx::DrawData draw, const gx::PipelineConfig& config, const gx::ShaderInfo& info, uint32_t vtxStart,
                    const gx::BindGroupRanges& ranges) noexcept {
#if defined(__ANDROID__)
  g_worldMergeable = false;
  if (is_offscreen()) return false;
  if (g_image < 0) {
    return discard_world_draw();
  }
  const bool world = gx::g_gxState.projType == GX_PERSPECTIVE;
  if (world && !g_recording) {
    return false;
  }
  auto spatialConfig = config;
  spatialConfig.msaaSamples = stereo_samples();
  spatialConfig.shaderConfig.depth16 = g_switches.depth16 ? 1 : 0;
  if (g_switches.f16) {
    spatialConfig.shaderConfig.halfPrecision = 1;
  }
  if (spatialConfig.colorUpdate) {
    spatialConfig.alphaUpdate = true; // transparent MR targets need coverage
  }
  if (!world) {
    spatialConfig.depthCompare = false;
    spatialConfig.depthUpdate = false;
    spatialConfig.dstAlpha = UINT32_MAX;
    draw.dstAlpha = UINT32_MAX;
  }
  if (!world) {
    draw.pipeline = pipeline_ref(spatialConfig);
    // The original orthographic uniforms draw the interface once in an atlas
    // region. OpenXR presents it as a separate, sharp quad for both eyes.
    draw.uniformRange = gx::build_uniform(info, vtxStart, ranges);
    g_hudDraws.push_back(draw);
    // Once this frame has a world, its screen is hidden. Avoid drawing the
    // same interface again into that unused screen; empty frames retain it.
    return !g_eyeDraws[0].empty();
  }
  // Both eyes in one instanced draw once its pipeline is built (the first
  // request queues it); until then, and for lines and points, which already
  // use instances, one draw per eye.
  // An opaque draw of an object may go to the GPU in front-to-back order.
  const bool inObject = g_sortKey >= 0.f;
  const DrawOrder order{
      .key = g_sortKey,
      .object = g_sortObject,
      // The game draws its opaque objects with the usual alpha blend on
      // (hsfdraw.c's SetBlendMode); its translucent ones go through its own
      // back-to-front pass, outside ObjDraw. Additive and other blends stay put.
      .sortable = inObject && config.depthCompare && config.depthUpdate &&
                  (config.depthFunc == GX_LESS || config.depthFunc == GX_LEQUAL) && config.colorUpdate &&
                  (config.blendMode == GX_BM_NONE ||
                   (config.blendMode == GX_BM_BLEND && config.blendFacSrc == GX_BL_SRCALPHA &&
                    config.blendFacDst == GX_BL_INVSRCALPHA)),
  };
  g_statSortable += order.sortable;
  {
    const bool blended = config.blendMode == GX_BM_BLEND || config.blendMode == GX_BM_SUBTRACT;
    uint32_t kinds = 0;
    kinds |= order.sortable ? KindOpaque : 0;
    kinds |= blended && !order.sortable ? KindBlend : 0;
    kinds |= config.blendMode == GX_BM_BLEND && config.blendFacDst == GX_BL_ONE ? KindAdditive : 0;
    kinds |= !config.depthCompare ? KindNoDepth : 0;
    kinds |= static_cast<bool>(config.shaderConfig.alphaCompare) ? KindAlphaTest : 0;
    kinds |= !inObject ? KindOutside : 0;
    kinds |= config.shaderConfig.tevStageCount >= 3 ? KindTev3 : 0;
    kinds |= config.shaderConfig.fogType != GX_FOG_NONE ? KindFog : 0;
    kinds |= config.shaderConfig.numIndStages > 0 ? KindIndirect : 0;
    ++g_statKindsTotal;
    for (size_t k = 0; k < std::size(kDrawKinds); ++k) {
      g_statKinds[k] += (kinds & kDrawKinds[k].second) != 0;
    }
    if ((kinds & g_switches.skipKinds) != 0) {
      ++g_statSkipped;
      g_statSortable -= order.sortable;
      return true; // a measurement: neither the eyes nor the hidden screen draw it
    }
  }
  if (g_switches.opaqueBlendOff && order.sortable && spatialConfig.blendMode == GX_BM_BLEND) {
    spatialConfig.blendMode = GX_BM_NONE;
  }
  if (g_switches.alphaTestOff && order.sortable) {
    spatialConfig.shaderConfig.alphaCompare = gx::AlphaCompare{};
  }
  // An object that reaches both eyes, drawn once per eye when asked: no
  // fragment test, so the GPU keeps rejecting its hidden fragments early.
  const bool crossingPerEye = g_switches.crossingPerEye && inObject && !g_uncut;
  g_statCrossingPerEye += crossingPerEye;
  if (const uint8_t mode = instanced_stereo_mode(); mode != gx::StereoOff && !crossingPerEye &&
      !g_instancedDisabled.load(std::memory_order_relaxed) &&
      info.lineMode == 0 && draw.instanceCount == 1 && gx::stereo_instanced_fits(info)) {
    auto instancedConfig = spatialConfig;
    instancedConfig.shaderConfig.stereo = g_uncut ? gx::StereoUncut : mode;
    // One pipeline request per draw: pipeline_ref skips the cache's lock when
    // a draw asks for the same pipeline as the one before, the usual case.
    const auto instancedRef = pipeline_ref(instancedConfig);
    bool ready = g_readyInstanced.contains(instancedRef);
    if (!ready) {
      wgpu::RenderPipeline instancedPipeline;
      ready = get_pipeline(instancedRef, instancedPipeline) && instancedPipeline;
      if (ready) {
        g_readyInstanced.insert(instancedRef);
      }
    }
    if (ready) {
      draw.pipeline = instancedRef;
      draw.uniformRange =
          gx::build_uniform_stereo_instanced(info, vtxStart, ranges, g_clip[0], g_clip[1], g_eyeX, g_eyeClip);
      g_eyeDraws[0].push_back(draw);
      g_eyeDraws[1].push_back(draw);
      g_eyeInstanced.push_back(1);
      g_eyeOrder.push_back(order);
      ++g_statInstanced;
      g_worldMergeable = true;
      return true;
    }
  }
  ++g_statPerEye;
  g_eyeInstanced.push_back(0);
  g_eyeOrder.push_back(order);
  draw.pipeline = pipeline_ref(spatialConfig);
  // One uniform build for both eyes: only the projection differs.
  const auto uniforms = gx::build_uniform_stereo(info, vtxStart, ranges, g_clip[0], g_clip[1]);
  for (size_t eye = 0; eye < 2; ++eye) {
    draw.uniformRange = uniforms[eye];
    g_eyeDraws[eye].push_back(draw);
  }
  g_worldMergeable = world;
  // The eye passes own these world draws; avoid rendering the same scene a
  // third time into the flat game screen. Offscreen/shadow draws stay intact.
  return world;
#else
  return false;
#endif
}

void instanced_failed(const std::string& message) noexcept {
#if defined(__ANDROID__)
  if (!g_instancedDisabled.exchange(true)) {
    Log.error("Stereo: both-eyes pipeline failed, drawing each eye on its own: {}", message);
  }
#else
  (void)message;
#endif
}

void end_frame() noexcept {
#if defined(__ANDROID__)
  g_worldMergeable = false;
  g_uncut = false;
  g_recording = false;
  // Even without geometry, clear and submit the leased image. Dropping it
  // leaks a ring slot and leaves the preceding scene visible in the room.
  if (g_image < 0) {
    g_image = -1;
    return;
  }
  Image image;
  {
    std::lock_guard lock{g_imagesMutex};
    if (static_cast<size_t>(g_image) >= g_images.size()) {
      g_image = -1;
      g_eyeDraws[0].clear();
      g_eyeDraws[1].clear();
      g_eyeInstanced.clear();
  g_eyeOrder.clear();
      g_hudDraws.clear();
      return;
    }
    image = g_images[g_image];
  }

  // Same sample count and formats as the game's frame, so its pipelines draw here too.
  const auto& config = webgpu::g_graphicsConfig;
  const uint32_t samples = stereo_samples();
  const auto depthFormat = g_switches.depth16 ? wgpu::TextureFormat::Depth16Unorm : config.depthFormat;
  // Remade when the size, the samples or the depth format change (live switches).
  const auto stale = [&](const webgpu::TextureWithSampler& target, wgpu::TextureFormat format, uint32_t width,
                         uint32_t height) {
    return !target.texture || target.size.width != width || target.size.height != height || target.format != format ||
           target.texture.GetSampleCount() != samples;
  };
  if (samples > 1 && stale(g_color, config.surfaceConfiguration.format, g_width, g_height)) {
    g_color = create_target(config.surfaceConfiguration.format, samples, g_width, g_height, "Stereo color");
  }
  if (stale(g_depth, depthFormat, g_width, g_height)) {
    g_depth = create_target(depthFormat, samples, g_width, g_height, "Stereo depth");
  }
  if (samples > 1 && stale(g_hudColor, config.surfaceConfiguration.format, g_hudWidth, g_hudHeight)) {
    g_hudColor = create_target(config.surfaceConfiguration.format, samples, g_hudWidth, g_hudHeight, "Stereo HUD color");
  }
  if (stale(g_hudDepth, depthFormat, g_hudWidth, g_hudHeight)) {
    g_hudDepth = create_target(depthFormat, samples, g_hudWidth, g_hudHeight, "Stereo HUD depth");
  }

  const bool hasWorld = !g_eyeDraws[0].empty();
  const size_t worldDraws = g_eyeDraws[0].size();
  {
    std::lock_guard lock{g_hiddenMutex};
    for (int eye = 0; eye < 2; ++eye) {
      // The hidden area's clear pipeline has the game's depth format.
      if (g_switches.hiddenArea && hasWorld && !g_switches.depth16) {
        g_frameHidden[eye] = g_hiddenArea[eye];
      } else {
        g_frameHidden[eye].clear();
      }
    }
  }
  const uint32_t eyeStride = g_width / 2;
  PassDesc desc{
      .color = samples > 1 ? g_color.view : image.view,
      .resolve = samples > 1 ? image.view : wgpu::TextureView{},
      .depth = g_depth.view,
      .size = {g_width, g_height, 1},
      .sampleCount = samples,
      .eyeStride = eyeStride,
      .eyeWidth = g_frameEyeWidth != 0 ? std::min(g_frameEyeWidth, eyeStride) : eyeStride,
      .eyeHeight = g_frameEyeHeight != 0 ? std::min(g_frameEyeHeight, g_eyeHeight) : g_eyeHeight,
      .hudTop = g_eyeHeight,
      .image = g_image,
      .tag = g_frameTag,
      .hasWorld = hasWorld,
      .eyes = {&g_eyeDraws[0], &g_eyeDraws[1]},
      .instanced = &g_eyeInstanced,
      .order = &g_eyeOrder,
      .sortOpaque = g_switches.sortOpaque,
      .hidden = {&g_frameHidden[0], &g_frameHidden[1]},
      .drawHud = g_frameDrawHud,
  };
  enqueue_pass(desc);
  // The interface, in its own image, when this frame draws it (its image's
  // contents are not kept: a frame that draws it draws all of it): its pass
  // after the eyes', in the same submission. The eyes' pass began the access
  // to both images.
  static const std::vector<gx::DrawData> noDraws;
  const PassDesc hud{
      .label = "Stereo HUD",
      .color = samples > 1 ? g_hudColor.view : image.hudView,
      .resolve = samples > 1 ? image.hudView : wgpu::TextureView{},
      .depth = g_hudDepth.view,
      .size = {g_hudWidth, g_hudHeight, 1},
      .sampleCount = samples,
      .hudTop = 0,
      .hudWidth = g_hudWidth,
      .hudHeight = g_hudHeight,
      .image = -1,
      .eyes = {&noDraws, &noDraws},
      .hud = &g_hudDraws,
  };
  if (g_frameDrawHud) {
    enqueue_pass(hud);
  }
  if (!g_loggedGeometry && hasWorld) {
    Log.info("Stereo: first perspective pass ({} world draws per eye, {} HUD draws once, MSAA {}x)",
             worldDraws, g_hudDraws.size(), samples);
    g_loggedGeometry = true;
  }
  // Every 5 seconds: what the eyes cost in draws (each world draw is encoded
  // once per eye, the interface's once).
  static auto statsAt = std::chrono::steady_clock::now();
  static uint64_t statFrames = 0, statWorld = 0, statHud = 0;
  static size_t statWorldMax = 0;
  ++statFrames;
  statWorld += worldDraws;
  statHud += g_hudDraws.size();
  statWorldMax = std::max(statWorldMax, worldDraws);
  if (const auto now = std::chrono::steady_clock::now(); now - statsAt >= std::chrono::seconds(5)) {
    const uint64_t recorded = g_statInstanced + g_statPerEye;
    Log.info("Stereo draws: {} frames, world {} avg / {} max per eye ({}% both eyes at once), HUD {} avg, {}x{} image, "
             "HUD {}x{} apart, sortable {}%{}, crossing per eye {}%",
             statFrames, statWorld / statFrames, statWorldMax,
             recorded != 0 ? g_statInstanced * 100 / recorded : 0, statHud / statFrames, g_width, g_height,
             g_hudWidth, g_hudHeight,
             recorded != 0 ? g_statSortable * 100 / recorded : 0, g_switches.sortOpaque ? " (sorted)" : "",
             recorded != 0 ? g_statCrossingPerEye * 100 / recorded : 0);
    g_statInstanced = g_statPerEye = g_statSortable = g_statCrossingPerEye = 0;
    if (g_statKindsTotal != 0) {
      std::string kinds;
      for (size_t k = 0; k < std::size(kDrawKinds); ++k) {
        kinds += fmt::format(" {}={}%", kDrawKinds[k].first, g_statKinds[k] * 100 / g_statKindsTotal);
      }
      Log.info("Stereo kinds:{} skipped={}%", kinds, g_statSkipped * 100 / g_statKindsTotal);
    }
    std::fill(std::begin(g_statKinds), std::end(g_statKinds), 0);
    g_statKindsTotal = g_statSkipped = 0;
    statsAt = now;
    statFrames = statWorld = statHud = 0;
    statWorldMax = 0;
  }
  g_eyeDraws[0].clear();
  g_eyeDraws[1].clear();
  g_eyeInstanced.clear();
  g_eyeOrder.clear();
  g_hudDraws.clear();
  g_image = -1;
#endif
}

void begin_access(int32_t image, uint64_t tag, bool hasWorld, bool hud) noexcept {
#if defined(__ANDROID__)
  Image target;
  {
    std::lock_guard lock{g_imagesMutex};
    if (image < 0 || static_cast<size_t>(image) >= g_images.size()) {
      return;
    }
    target = g_images[image];
  }
  // The headset has finished with the image (it copies it out before handing
  // the index back), and every eye pass clears it: no fence, no contents.
  wgpu::SharedTextureMemoryBeginAccessDescriptor begin{};
  begin.concurrentRead = false;
  begin.initialized = false;
  begin.fenceCount = 0;
  // Dawn's Vulkan AHardwareBuffer import requires this chained state, even
  // when we discard the preceding contents (VK_IMAGE_LAYOUT_UNDEFINED = 0).
  wgpu::SharedTextureMemoryVkImageLayoutBeginState layoutBegin{};
  if (webgpu::g_backendType == wgpu::BackendType::Vulkan) {
    layoutBegin.oldLayout = 0;
    layoutBegin.newLayout = 0;
    begin.nextInChain = &layoutBegin;
  }
  if (target.memory.BeginAccess(target.texture, &begin) != wgpu::Status::Success) {
    Log.warn("Stereo image {} not accessible", image);
    return;
  }
  if (hud && target.hudMemory.BeginAccess(target.hudTexture, &begin) != wgpu::Status::Success) {
    // Drawn anyway: the eyes' image is what matters; the interface shows its previous one.
    Log.warn("Stereo HUD image {} not accessible", image);
  }
  g_accessed.push_back({image, tag, hasWorld, hud});
#endif
}

#if defined(__ANDROID__)
namespace {
// A frame's GPU timestamps: queries 0-1 the eyes' pass, 2-3 the HUD's.
struct PassTimer {
  enum class State { Free, Encoding, Encoded, Mapping };
  std::atomic<State> state{State::Free}; // the map callback may run on another thread
  wgpu::QuerySet queries;
  wgpu::Buffer resolve;  // QueryResolve, 512 bytes: the eyes' at 0, the HUD's at 256
  wgpu::Buffer readback; // MapRead
  bool eyes = false, hud = false;
  wgpu::PassTimestampWrites eyesWrites, hudWrites;
};
std::array<PassTimer, 8> g_passTimers;
int g_encodingTimer = -1;
std::mutex g_gpuStatsMutex;
double g_gpuEyesSum = 0, g_gpuEyesMax = 0, g_gpuHudSum = 0, g_gpuHudMax = 0;
uint32_t g_gpuEyesCount = 0, g_gpuHudCount = 0, g_gpuTimerBusy = 0;
std::chrono::steady_clock::time_point g_gpuStatsAt = std::chrono::steady_clock::now();

void record_gpu_times(double eyesMs, double hudMs) {
  std::lock_guard lock{g_gpuStatsMutex};
  if (eyesMs > 0) {
    g_gpuEyesSum += eyesMs;
    g_gpuEyesMax = std::max(g_gpuEyesMax, eyesMs);
    ++g_gpuEyesCount;
  }
  if (hudMs > 0) {
    g_gpuHudSum += hudMs;
    g_gpuHudMax = std::max(g_gpuHudMax, hudMs);
    ++g_gpuHudCount;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - g_gpuStatsAt < std::chrono::seconds(5)) return;
  Log.info("Stereo GPU: eyes' pass {:.2f} ms avg / {:.2f} max ({} images), HUD pass {:.2f} ms avg / {:.2f} max ({}), "
           "timers busy {}",
           g_gpuEyesCount ? g_gpuEyesSum / g_gpuEyesCount : 0.0, g_gpuEyesMax, g_gpuEyesCount,
           g_gpuHudCount ? g_gpuHudSum / g_gpuHudCount : 0.0, g_gpuHudMax, g_gpuHudCount, g_gpuTimerBusy);
  g_gpuEyesSum = g_gpuEyesMax = g_gpuHudSum = g_gpuHudMax = 0;
  g_gpuEyesCount = g_gpuHudCount = g_gpuTimerBusy = 0;
  g_gpuStatsAt = now;
}
} // namespace
#endif

const wgpu::PassTimestampWrites* pass_timestamps(bool hud) noexcept {
#if defined(__ANDROID__)
  if (!webgpu::g_timestampsEnabled) return nullptr;
  if (!hud) {
    g_encodingTimer = -1;
    for (size_t i = 0; i < g_passTimers.size(); ++i) {
      if (g_passTimers[i].state.load() == PassTimer::State::Free) {
        g_encodingTimer = static_cast<int>(i);
        break;
      }
    }
    if (g_encodingTimer < 0) {
      std::lock_guard lock{g_gpuStatsMutex};
      ++g_gpuTimerBusy;
      return nullptr;
    }
    auto& timer = g_passTimers[g_encodingTimer];
    if (!timer.queries) {
      const wgpu::QuerySetDescriptor queryDesc{.label = "Stereo pass timestamps", .type = wgpu::QueryType::Timestamp, .count = 4};
      timer.queries = webgpu::g_device.CreateQuerySet(&queryDesc);
      const wgpu::BufferDescriptor resolveDesc{
          .label = "Stereo timestamps resolve", .usage = wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc, .size = 512};
      timer.resolve = webgpu::g_device.CreateBuffer(&resolveDesc);
      const wgpu::BufferDescriptor readbackDesc{
          .label = "Stereo timestamps readback", .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst, .size = 512};
      timer.readback = webgpu::g_device.CreateBuffer(&readbackDesc);
      timer.eyesWrites = {.querySet = timer.queries, .beginningOfPassWriteIndex = 0, .endOfPassWriteIndex = 1};
      timer.hudWrites = {.querySet = timer.queries, .beginningOfPassWriteIndex = 2, .endOfPassWriteIndex = 3};
    }
    timer.state = PassTimer::State::Encoding;
    timer.eyes = timer.hud = false;
    return &timer.eyesWrites;
  }
  if (g_encodingTimer < 0) return nullptr;
  return &g_passTimers[g_encodingTimer].hudWrites;
#else
  (void)hud;
  return nullptr;
#endif
}

void pass_timestamps_resolve(wgpu::CommandEncoder& cmd, bool hud) noexcept {
#if defined(__ANDROID__)
  if (g_encodingTimer < 0) return;
  auto& timer = g_passTimers[g_encodingTimer];
  cmd.ResolveQuerySet(timer.queries, hud ? 2 : 0, 2, timer.resolve, hud ? 256 : 0);
  cmd.CopyBufferToBuffer(timer.resolve, hud ? 256 : 0, timer.readback, hud ? 256 : 0, 16);
  (hud ? timer.hud : timer.eyes) = true;
  timer.state = PassTimer::State::Encoded;
#else
  (void)cmd;
  (void)hud;
#endif
}

void after_submit() noexcept {
#if defined(__ANDROID__)
  // This frame's timestamps went to the GPU with it: read them once done.
  for (auto& timer : g_passTimers) {
    if (timer.state.load() != PassTimer::State::Encoded) continue;
    timer.state = PassTimer::State::Mapping;
    PassTimer* const self = &timer;
    timer.readback.MapAsync(wgpu::MapMode::Read, 0, 512, wgpu::CallbackMode::AllowSpontaneous,
                            [self](wgpu::MapAsyncStatus status, wgpu::StringView) {
                              double eyesMs = 0, hudMs = 0;
                              if (status == wgpu::MapAsyncStatus::Success) {
                                const auto* data = static_cast<const uint64_t*>(self->readback.GetConstMappedRange(0, 512));
                                if (data != nullptr) {
                                  if (self->eyes && data[1] > data[0]) eyesMs = (data[1] - data[0]) / 1e6;
                                  if (self->hud && data[33] > data[32]) hudMs = (data[33] - data[32]) / 1e6;
                                }
                                self->readback.Unmap();
                              }
                              record_gpu_times(eyesMs, hudMs);
                              self->state = PassTimer::State::Free;
                            });
  }
  g_encodingTimer = -1;
  for (const auto& accessed : g_accessed) {
    Image target;
    {
      std::lock_guard lock{g_imagesMutex};
      if (static_cast<size_t>(accessed.image) >= g_images.size()) {
        continue;
      }
      target = g_images[accessed.image];
    }
    // Each image's access ends with its own fences; the headset gets one fd.
    const auto end_access = [](wgpu::SharedTextureMemory& memory, wgpu::Texture& texture) {
      wgpu::SharedTextureMemoryEndAccessState end{};
      wgpu::SharedTextureMemoryVkImageLayoutEndState layoutEnd{};
      if (webgpu::g_backendType == wgpu::BackendType::Vulkan) {
        end.nextInChain = &layoutEnd;
      }
      if (!memory || memory.EndAccess(texture, &end) != wgpu::Status::Success) {
        return -1;
      }
      int fd = -1;
      for (size_t i = 0; i < end.fenceCount && fd < 0; ++i) {
        wgpu::SharedFenceSyncFDExportInfo syncFd{};
        wgpu::SharedFenceExportInfo info{};
        info.nextInChain = &syncFd;
        end.fences[i].ExportInfo(&info);
        if (info.type == wgpu::SharedFenceType::SyncFD && syncFd.handle >= 0) {
          fd = dup(syncFd.handle); // the fence keeps its own
        }
      }
      return fd;
    };
    const int eyesFd = end_access(target.memory, target.texture);
    const int fd = accessed.hud ? merge_fences(eyesFd, end_access(target.hudMemory, target.hudTexture)) : eyesFd;
    if (g_callback != nullptr) {
      g_callback(static_cast<uint32_t>(accessed.image), accessed.tag, fd, accessed.hasWorld, g_callbackUser);
    } else if (fd >= 0) {
      close(fd);
    }
  }
  g_accessed.clear();
#endif
}

} // namespace aurora::gfx::stereo

using namespace aurora;
using namespace aurora::gfx::stereo;

bool AuroraStereoRegisterImages(void* const* eyeBuffers, void* const* hudBuffers, uint32_t count, uint32_t width,
                                uint32_t height, uint32_t hudWidth, uint32_t hudHeight,
                                AuroraStereoCallback callback, void* user) {
#if defined(__ANDROID__)
  if (!webgpu::g_device) {
    return false;
  }
  if (webgpu::g_graphicsConfig.surfaceConfiguration.format != wgpu::TextureFormat::RGBA8Unorm) {
    Log.warn("Stereo needs an RGBA8 frame, not {}",
             static_cast<uint32_t>(webgpu::g_graphicsConfig.surfaceConfiguration.format));
    return false;
  }
  const auto import = [](void* buffer, uint32_t w, uint32_t h, const char* label, wgpu::SharedTextureMemory& memory,
                          wgpu::Texture& texture, wgpu::TextureView& view) {
    wgpu::SharedTextureMemoryAHardwareBufferDescriptor ahb{};
    ahb.handle = buffer;
    wgpu::SharedTextureMemoryDescriptor descriptor{};
    descriptor.nextInChain = &ahb;
    descriptor.label = label;
    memory = webgpu::g_device.ImportSharedTextureMemory(&descriptor);
    if (!memory) {
      return false;
    }
    const wgpu::TextureDescriptor textureDescriptor{
        .label = label,
        .usage = wgpu::TextureUsage::RenderAttachment,
        .dimension = wgpu::TextureDimension::e2D,
        .size = {w, h, 1},
        .format = wgpu::TextureFormat::RGBA8Unorm,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    texture = memory.CreateTexture(&textureDescriptor);
    view = texture.CreateView();
    return true;
  };
  std::vector<Image> images;
  for (uint32_t i = 0; i < count; ++i) {
    Image image;
    if (!import(eyeBuffers[i], width, height, "Stereo image", image.memory, image.texture, image.view) ||
        !import(hudBuffers[i], hudWidth, hudHeight, "Stereo HUD image", image.hudMemory, image.hudTexture,
                image.hudView)) {
      Log.warn("Stereo image {} could not be imported", i);
      return false;
    }
    images.push_back(std::move(image));
  }
  std::lock_guard lock{g_imagesMutex};
  g_images = std::move(images);
  g_width = width;
  g_height = height;
  g_eyeHeight = height;
  g_hudWidth = hudWidth;
  g_hudHeight = hudHeight;
  g_callback = callback;
  g_callbackUser = user;
  g_loggedGeometry = false;
  g_forcedAnisotropy.store(g_switches.anisotropy, std::memory_order_relaxed);
  g_imagesRegistered.store(true, std::memory_order_relaxed);
  Log.info("Stereo: {} images of {}x{}, HUD {}x{} apart", count, width, height, hudWidth, hudHeight);
  return true;
#else
  (void)eyeBuffers;
  (void)hudBuffers;
  (void)count;
  (void)width;
  (void)height;
  (void)hudWidth;
  (void)hudHeight;
  (void)callback;
  (void)user;
  return false;
#endif
}

void AuroraStereoBegin(uint32_t image, const float clip[2][16], uint64_t frameTag) {
#if defined(__ANDROID__)
  // GX queues draws in its FIFO. Consume the preceding camera's commands
  // with its existing clip matrices before switching to this camera/view.
  gx::fifo::drain();
  g_worldMergeable = false;
  refresh_switches();
  g_clip[0] = to_mat(clip[0]);
  g_clip[1] = to_mat(clip[1]);
  g_image = static_cast<int32_t>(image);
  // Instanced draws: a viewport over both eyes' rectangles (common.cpp).
  {
    const float stride = static_cast<float>(g_width / 2);
    const float width = g_frameEyeWidth != 0 ? std::min(static_cast<float>(g_frameEyeWidth), stride) : stride;
    instanced_placement(stride, width, g_eyeX, g_eyeClip);
  }
  g_frameTag = frameTag;
  g_recording = true;
#else
  (void)image;
  (void)clip;
  (void)frameTag;
#endif
}

void AuroraStereoEnd(void) {
#if defined(__ANDROID__)
  // Closing recording before draining loses deferred display-list draws:
  // they would only be decoded at aurora_end_frame, after g_recording is off.
  gx::fifo::drain();
  g_recording = false;
  g_worldMergeable = false;
#endif
}

bool AuroraStereoActive(void) { return recording(); }

void AuroraStereoSetEyeSize(uint32_t width, uint32_t height) {
#if defined(__ANDROID__)
  g_frameEyeWidth = width;
  g_frameEyeHeight = height;
#else
  (void)width;
  (void)height;
#endif
}

void AuroraStereoSetHud(bool draw) {
#if defined(__ANDROID__)
  g_frameDrawHud = draw;
#else
  (void)draw;
#endif
}

void AuroraStereoHideScreen(bool hidden) {
#if defined(__ANDROID__)
  g_screenHidden.store(hidden, std::memory_order_relaxed);
#else
  (void)hidden;
#endif
}

extern "C" void AuroraStereoSetSortKey(float key) {
#if defined(__ANDROID__)
  using namespace aurora::gfx::stereo;
  if (key < 0.f && g_sortKey < 0.f) {
    return;
  }
  // The FIFO is deferred: its draws so far belong to the previous object.
  aurora::gx::fifo::drain();
  g_sortKey = key < 0.f ? -1.f : key;
  if (key >= 0.f) {
    ++g_sortObject;
  }
#else
  (void)key;
#endif
}

extern "C" void AuroraStereoSetHiddenArea(uint32_t eye, const float* rects, uint32_t count) {
#if defined(__ANDROID__)
  using namespace aurora::gfx::stereo;
  if (eye > 1) {
    return;
  }
  std::lock_guard lock{g_hiddenMutex};
  g_hiddenArea[eye].assign(rects, rects + (rects != nullptr ? count * 4 : 0));
  Log.info("Hidden area, eye {}: {} rectangles", eye, count);
#else
  (void)eye;
  (void)rects;
  (void)count;
#endif
}

extern "C" void AuroraStereoSetUncut(bool uncut) {
#if defined(__ANDROID__)
  if (aurora::gfx::stereo::g_uncut == uncut) {
    return;
  }
  // The FIFO is deferred: its draws so far go with the previous value, and
  // none is merged across the change.
  aurora::gx::fifo::drain();
  aurora::gfx::stereo::g_uncut = uncut;
  aurora::gfx::stereo::g_worldMergeable = false;
#else
  (void)uncut;
#endif
}

extern "C" void AuroraStereoWorldOnly(bool enabled) {
#if defined(__ANDROID__)
  // The FIFO is deferred: process camera draws before changing the scope.
  aurora::gx::fifo::drain();
  aurora::gfx::stereo::g_worldOnly = enabled;
#else
  (void)enabled;
#endif
}
