#include "tex_copy_conv.hpp"

#include "../internal.hpp"
#include "../gx/gx.hpp"
#include "../webgpu/gpu.hpp"
#include "texture.hpp"
#include "../gx/gx_fmt.hpp"

#include <absl/container/flat_hash_map.h>

#include "texture_convert.hpp"

using namespace std::string_literals;

namespace aurora::gfx::tex_copy_conv {
static Module Log("aurora::gfx::tex_copy_conv");

using webgpu::g_device;

static constexpr std::string_view ShaderPreamble = R"(
@group(0) @binding(0) var src_samp: sampler;
@group(0) @binding(1) var src: texture_2d<f32>;

struct UVTransform {
    offset: vec2f,
    scale: vec2f,
    // GXSetCopyFilter's seven vertical taps, weights already divided by their
    // sum, plus one source texel height in UV. Carried in the transform rather
    // than a second binding so every copy pipeline keeps one bind group layout.
    vf0: vec4f,
    vf1: vec4f,
};
@group(0) @binding(2) var<uniform> uv_xf: UVTransform;

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

var<private> positions: array<vec2f, 3> = array(
    vec2f(-1.0, 1.0),
    vec2f(-1.0, -3.0),
    vec2f(3.0, 1.0),
);
var<private> uvs: array<vec2f, 3> = array(
    vec2f(0.0, 0.0),
    vec2f(0.0, 2.0),
    vec2f(2.0, 0.0),
);

@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4f(positions[vi], 0.0, 1.0);
    out.uv = uvs[vi] * uv_xf.scale + uv_xf.offset;
    return out;
}

fn intensity(rgb: vec3f) -> f32 {
    // ITU-R BT.601 luma coefficients
    return dot(rgb, vec3f(0.257, 0.504, 0.098)) + 16.0 / 255.0;
}

fn quantize4(v: f32) -> f32 {
    return floor(v * 16.0) / 15.0;
}
)"sv;

static const std::string DepthShaderPreamble = R"(
@group(0) @binding(0) var src: texture_depth_2d;

struct UVTransform {
    offset: vec2f,
    scale: vec2f,
};
@group(0) @binding(1) var<uniform> uv_xf: UVTransform;

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

var<private> positions: array<vec2f, 3> = array(
    vec2f(-1.0, 1.0),
    vec2f(-1.0, -3.0),
    vec2f(3.0, 1.0),
);
var<private> uvs: array<vec2f, 3> = array(
    vec2f(0.0, 0.0),
    vec2f(0.0, 2.0),
    vec2f(2.0, 0.0),
);

@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4f(positions[vi], 0.0, 1.0);
    out.uv = uvs[vi] * uv_xf.scale + uv_xf.offset;
    return out;
}
)"s + (gx::UseReversedZ ? R"(
fn gx_z24(uv: vec2f) -> u32 {
    let texSize = vec2i(textureDimensions(src));
    let coord = clamp(vec2i(floor(uv * vec2f(texSize))), vec2i(0), texSize - vec2i(1));
    let depth = textureLoad(src, coord, 0);
    return min(u32(clamp(1.0 - depth, 0.0, 1.0) * 16777215.0 + 0.5), 0x00ffffffu);
}
)"s
                        : R"(
fn gx_z24(uv: vec2f) -> u32 {
    let texSize = vec2i(textureDimensions(src));
    let coord = clamp(vec2i(floor(uv * vec2f(texSize))), vec2i(0), texSize - vec2i(1));
    let depth = textureLoad(src, coord, 0);
    return min(u32(clamp(depth, 0.0, 1.0) * 16777215.0 + 0.5), 0x00ffffffu);
}
)"s);

// Passthrough blit (for scaling)
static constexpr std::string_view FragPassthrough = R"(
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    return textureSample(src, src_samp, in.uv);
}
)"sv;

// GX_TF_I4: 4-bit intensity -> R8Unorm (quantized)
static constexpr std::string_view FragI4 = R"(
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let rgb = textureSample(src, src_samp, in.uv).rgb;
    let i = quantize4(intensity(rgb));
    return vec4f(i, i, i, i);
}
)"sv;

// GX_TF_I8: 8-bit intensity -> R8Unorm
static constexpr std::string_view FragI8 = R"(
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let rgb = textureSample(src, src_samp, in.uv).rgb;
    let i = intensity(rgb);
    return vec4f(i, i, i, i);
}
)"sv;

// GX_TF_IA4: 4-bit intensity + 4-bit alpha -> RG8Unorm
static constexpr std::string_view FragIA4 = R"(
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let c = textureSample(src, src_samp, in.uv);
    let i = quantize4(intensity(c.rgb));
    let a = quantize4(c.a);
    return vec4f(i, i, i, a);
}
)"sv;

// GX_TF_IA8: 8-bit intensity + 8-bit alpha -> RG8Unorm
static constexpr std::string_view FragIA8 = R"(
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let c = textureSample(src, src_samp, in.uv);
    let i = intensity(c.rgb);
    return vec4f(i, i, i, c.a);
}
)"sv;

// GX_TF_RGB565: Blit alpha to 1.0
// GXSetCopyFilter's vertical filter, which aurora ignored entirely until now.
// Seven taps down the source column, weights out of 64. Hardware applies this
// while copying the EFB; skipping it leaves every copy sharper than the console
// produced, and for an effect that samples its own previous copy each frame it
// removes the low-pass that keeps the feedback from compounding.
static constexpr std::string_view FragVFilter = R"(
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let h = uv_xf.vf1.w;
    var acc = vec4f(0.0);
    acc += textureSample(src, src_samp, vec2f(in.uv.x, in.uv.y - 3.0 * h)) * uv_xf.vf0.x;
    acc += textureSample(src, src_samp, vec2f(in.uv.x, in.uv.y - 2.0 * h)) * uv_xf.vf0.y;
    acc += textureSample(src, src_samp, vec2f(in.uv.x, in.uv.y - 1.0 * h)) * uv_xf.vf0.z;
    acc += textureSample(src, src_samp, in.uv) * uv_xf.vf0.w;
    acc += textureSample(src, src_samp, vec2f(in.uv.x, in.uv.y + 1.0 * h)) * uv_xf.vf1.x;
    acc += textureSample(src, src_samp, vec2f(in.uv.x, in.uv.y + 2.0 * h)) * uv_xf.vf1.y;
    acc += textureSample(src, src_samp, vec2f(in.uv.x, in.uv.y + 3.0 * h)) * uv_xf.vf1.z;
    return acc;
}
)"sv;

static constexpr std::string_view FragRGB565 = R"(
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let c = textureSample(src, src_samp, in.uv);
    return vec4f(c.rgb, 1.0);
}
)"sv;

// GX_CTF_R4: 4-bit red -> R8Unorm
static constexpr std::string_view FragR4 = R"(
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let r = quantize4(textureSample(src, src_samp, in.uv).r);
    return vec4f(r, r, r, r);
}
)"sv;

// GX_CTF_RA4: 4-bit red + 4-bit alpha -> RG8Unorm
static constexpr std::string_view FragRA4 = R"(
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let c = textureSample(src, src_samp, in.uv);
    let r = quantize4(c.r);
    return vec4f(r, r, r, quantize4(c.a));
}
)"sv;

// GX_CTF_RA8: 8-bit red + 8-bit alpha -> RG8Unorm
static constexpr std::string_view FragRA8 = R"(
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let c = textureSample(src, src_samp, in.uv);
    return vec4f(c.r, c.r, c.r, c.a);
}
)"sv;

// GX_CTF_A8: 8-bit alpha -> R8Unorm
static constexpr std::string_view FragA8 = R"(
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let a = textureSample(src, src_samp, in.uv).a;
    return vec4f(a, a, a, a);
}
)"sv;

// GX_CTF_R8: 8-bit red -> R8Unorm
static constexpr std::string_view FragR8 = R"(
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let r = textureSample(src, src_samp, in.uv).r;
    return vec4f(r, r, r, r);
}
)"sv;

// GX_CTF_G8: 8-bit green -> R8Unorm
static constexpr std::string_view FragG8 = R"(
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let g = textureSample(src, src_samp, in.uv).g;
    return vec4f(g, g, g, g);
}
)"sv;

// GX_CTF_B8: 8-bit blue -> R8Unorm
static constexpr std::string_view FragB8 = R"(
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let b = textureSample(src, src_samp, in.uv).b;
    return vec4f(b, b, b, b);
}
)"sv;

// GX_CTF_RG8: 8-bit red + 8-bit green -> RG8Unorm
static constexpr std::string_view FragRG8 = R"(
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let c = textureSample(src, src_samp, in.uv);
    return vec4f(c.r, c.r, c.r, c.g);
}
)"sv;

// GX_CTF_GB8: 8-bit green + 8-bit blue -> RG8Unorm
static constexpr std::string_view FragGB8 = R"(
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let c = textureSample(src, src_samp, in.uv);
    return vec4f(c.g, c.g, c.g, c.b);
}
)"sv;

// GX_TF_Z24X8: full 24-bit depth -> RGBA8, high byte in R down to low byte in B.
// Without this entry needs_conversion() is false for Z24X8 while is_depth_format()
// is true, so a scaled copy fell through to blit(), whose pipeline is built with
// g_bindGroupLayout while execute() binds a group laid out for g_depthBindGroupLayout.
// The layouts do not match and the whole pass is rejected.
static constexpr std::string_view FragZ24X8 = R"(
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let z24 = gx_z24(in.uv);
    let r = f32((z24 >> 16u) & 0xFFu) / 255.0;
    let g = f32((z24 >> 8u) & 0xFFu) / 255.0;
    let b = f32(z24 & 0xFFu) / 255.0;
    return vec4f(r, g, b, 1.0);
}
)"sv;

// GX_TF_Z16: Upper 16-bits depth -> IA8
static constexpr std::string_view FragZ16 = R"(
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let z16 = gx_z24(in.uv) >> 8u;
    let i = f32((z16 >> 8u) & 0xFFu) / 255.0;
    let a = f32(z16 & 0xFFu) / 255.0;
    return vec4f(i, i, i, a);
}
)"sv;

struct ConvPipeline {
  GXTexFmt fmt;
  std::string_view fragShader;
  wgpu::TextureFormat outputFormat;
  const char* label;
};

static constexpr std::array ConvPipelines{
    ConvPipeline{GX_TF_I4, FragI4, wgpu::TextureFormat::RGBA8Unorm, "TexCopyConv I4"},
    ConvPipeline{GX_TF_I8, FragI8, wgpu::TextureFormat::RGBA8Unorm, "TexCopyConv I8"},
    ConvPipeline{GX_TF_IA4, FragIA4, wgpu::TextureFormat::RGBA8Unorm, "TexCopyConv IA4"},
    ConvPipeline{GX_TF_IA8, FragIA8, wgpu::TextureFormat::RGBA8Unorm, "TexCopyConv IA8"},
    ConvPipeline{GX_TF_RGB565, FragRGB565, wgpu::TextureFormat::RGBA8Unorm, "TexCopyConv RGB565"},
    ConvPipeline{GX_CTF_R4, FragR4, wgpu::TextureFormat::RGBA8Unorm, "TexCopyConv R4"},
    ConvPipeline{GX_CTF_RA4, FragRA4, wgpu::TextureFormat::RGBA8Unorm, "TexCopyConv RA4"},
    ConvPipeline{GX_CTF_RA8, FragRA8, wgpu::TextureFormat::RGBA8Unorm, "TexCopyConv RA8"},
    ConvPipeline{GX_CTF_A8, FragA8, wgpu::TextureFormat::RGBA8Unorm, "TexCopyConv A8"},
    ConvPipeline{GX_CTF_R8, FragR8, wgpu::TextureFormat::RGBA8Unorm, "TexCopyConv R8"},
    ConvPipeline{GX_CTF_G8, FragG8, wgpu::TextureFormat::RGBA8Unorm, "TexCopyConv G8"},
    ConvPipeline{GX_CTF_B8, FragB8, wgpu::TextureFormat::RGBA8Unorm, "TexCopyConv B8"},
    ConvPipeline{GX_CTF_RG8, FragRG8, wgpu::TextureFormat::RGBA8Unorm, "TexCopyConv RG8"},
    ConvPipeline{GX_CTF_GB8, FragGB8, wgpu::TextureFormat::RGBA8Unorm, "TexCopyConv GB8"},
};

static constexpr std::array DepthConvPipelines{
    ConvPipeline{GX_TF_Z16, FragZ16, wgpu::TextureFormat::RGBA8Unorm, "TexCopyConv Z16"},
    ConvPipeline{GX_TF_Z24X8, FragZ24X8, wgpu::TextureFormat::RGBA8Unorm, "TexCopyConv Z24X8"},
};

static wgpu::BindGroupLayout g_bindGroupLayout;
static wgpu::BindGroupLayout g_depthBindGroupLayout;
static wgpu::Sampler g_nearestSampler;
static wgpu::Sampler g_linearSampler;
static absl::flat_hash_map<GXTexFmt, wgpu::RenderPipeline> g_pipelines;
static wgpu::RenderPipeline g_blitPipeline;
static wgpu::RenderPipeline g_vfilterPipeline;

static wgpu::RenderPipeline create_pipeline(const ConvPipeline& conv, const std::string_view shaderPreamble,
                                            const wgpu::BindGroupLayout& bindGroupLayout) {
  std::string shaderSource;
  shaderSource.reserve(shaderPreamble.size() + conv.fragShader.size());
  shaderSource += shaderPreamble;
  shaderSource += conv.fragShader;

  const wgpu::ShaderSourceWGSL wgslSource{wgpu::ShaderSourceWGSL::Init{
      .code = shaderSource.c_str(),
  }};
  const wgpu::ShaderModuleDescriptor moduleDescriptor{
      .nextInChain = &wgslSource,
      .label = conv.label,
  };
  const auto module = g_device.CreateShaderModule(&moduleDescriptor);

  const std::array colorTargets{wgpu::ColorTargetState{
      .format = conv.outputFormat,
  }};
  const wgpu::FragmentState fragmentState{
      .module = module,
      .entryPoint = "fs_main",
      .targetCount = colorTargets.size(),
      .targets = colorTargets.data(),
  };

  const wgpu::PipelineLayoutDescriptor layoutDescriptor{
      .bindGroupLayoutCount = 1,
      .bindGroupLayouts = &bindGroupLayout,
  };
  const auto pipelineLayout = g_device.CreatePipelineLayout(&layoutDescriptor);

  const wgpu::RenderPipelineDescriptor pipelineDescriptor{
      .label = conv.label,
      .layout = pipelineLayout,
      .vertex =
          wgpu::VertexState{
              .module = module,
              .entryPoint = "vs_main",
          },
      .primitive =
          wgpu::PrimitiveState{
              .topology = wgpu::PrimitiveTopology::TriangleList,
          },
      .fragment = &fragmentState,
  };
  return g_device.CreateRenderPipeline(&pipelineDescriptor);
}

bool needs_conversion(const GXTexFmt fmt) { return g_pipelines.contains(fmt); }

void initialize() {
  static constexpr std::array bindGroupLayoutEntries{
      wgpu::BindGroupLayoutEntry{
          .binding = 0,
          .visibility = wgpu::ShaderStage::Fragment,
          .sampler =
              wgpu::SamplerBindingLayout{
                  .type = wgpu::SamplerBindingType::Filtering,
              },
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 1,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture =
              wgpu::TextureBindingLayout{
                  .sampleType = wgpu::TextureSampleType::Float,
                  .viewDimension = wgpu::TextureViewDimension::e2D,
              },
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 2,
          // Fragment too: the vertical copy filter reads its taps from the same
          // uniform, which keeps one bind group layout for every copy pipeline.
          .visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment,
          .buffer =
              wgpu::BufferBindingLayout{
                  .type = wgpu::BufferBindingType::Uniform,
              },
      },
  };
  static constexpr wgpu::BindGroupLayoutDescriptor bindGroupLayoutDescriptor{
      .label = "TexCopyConv Bind Group Layout",
      .entryCount = bindGroupLayoutEntries.size(),
      .entries = bindGroupLayoutEntries.data(),
  };
  g_bindGroupLayout = g_device.CreateBindGroupLayout(&bindGroupLayoutDescriptor);

  static constexpr std::array depthBindGroupLayoutEntries{
      wgpu::BindGroupLayoutEntry{
          .binding = 0,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture =
              wgpu::TextureBindingLayout{
                  .sampleType = wgpu::TextureSampleType::Depth,
                  .viewDimension = wgpu::TextureViewDimension::e2D,
              },
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 1,
          .visibility = wgpu::ShaderStage::Vertex,
          .buffer =
              wgpu::BufferBindingLayout{
                  .type = wgpu::BufferBindingType::Uniform,
              },
      },
  };
  static constexpr wgpu::BindGroupLayoutDescriptor depthBindGroupLayoutDescriptor{
      .label = "TexCopyConv Depth Bind Group Layout",
      .entryCount = depthBindGroupLayoutEntries.size(),
      .entries = depthBindGroupLayoutEntries.data(),
  };
  g_depthBindGroupLayout = g_device.CreateBindGroupLayout(&depthBindGroupLayoutDescriptor);

  g_blitPipeline = create_pipeline(
      {GX_TF_RGBA8, FragPassthrough, webgpu::g_graphicsConfig.surfaceConfiguration.format, "TexCopyConv Blit"},
      ShaderPreamble, g_bindGroupLayout);
  g_vfilterPipeline = create_pipeline(
      {GX_TF_RGBA8, FragVFilter, webgpu::g_graphicsConfig.surfaceConfiguration.format, "TexCopyConv VFilter"},
      ShaderPreamble, g_bindGroupLayout);
  for (const auto& conv : ConvPipelines) {
    g_pipelines[conv.fmt] = create_pipeline(conv, ShaderPreamble, g_bindGroupLayout);
    if (conv.outputFormat != to_wgpu(conv.fmt)) {
      Log.fatal("Output format mismatch for {}", conv.fmt);
    }
  }
  for (const auto& conv : DepthConvPipelines) {
    g_pipelines[conv.fmt] = create_pipeline(conv, DepthShaderPreamble, g_depthBindGroupLayout);
    if (conv.outputFormat != to_wgpu(conv.fmt)) {
      Log.fatal("Output format mismatch for {}", conv.fmt);
    }
  }

  static constexpr wgpu::SamplerDescriptor nearestSamplerDescriptor{
      .label = "TexCopyConv Nearest Sampler",
      .magFilter = wgpu::FilterMode::Nearest,
      .minFilter = wgpu::FilterMode::Nearest,
  };
  g_nearestSampler = g_device.CreateSampler(&nearestSamplerDescriptor);

  static constexpr wgpu::SamplerDescriptor linearSamplerDescriptor{
      .label = "TexCopyConv Linear Sampler",
      .magFilter = wgpu::FilterMode::Linear,
      .minFilter = wgpu::FilterMode::Linear,
  };
  g_linearSampler = g_device.CreateSampler(&linearSamplerDescriptor);
}

void shutdown() {
  g_pipelines.clear();
  g_blitPipeline = {};
  g_vfilterPipeline = {};
  g_bindGroupLayout = {};
  g_depthBindGroupLayout = {};
  g_nearestSampler = {};
  g_linearSampler = {};
}

static void execute(const wgpu::CommandEncoder& cmd, const ConvRequest& req, const wgpu::RenderPipeline& pipeline) {
  wgpu::BindGroup bindGroup;
  if (gx::is_depth_format(req.fmt)) {
    const std::array bindGroupEntries{
        wgpu::BindGroupEntry{
            .binding = 0,
            .textureView = req.srcView,
        },
        wgpu::BindGroupEntry{
            .binding = 1,
            .buffer = g_uniformBuffer,
            .offset = req.uniformRange.offset,
            .size = req.uniformRange.size,
        },
    };
    const wgpu::BindGroupDescriptor bindGroupDescriptor{
        .layout = g_depthBindGroupLayout,
        .entryCount = bindGroupEntries.size(),
        .entries = bindGroupEntries.data(),
    };
    bindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
  } else {
    const auto& sampler = req.sampleFilter == SampleFilter::Linear ? g_linearSampler : g_nearestSampler;
    const std::array bindGroupEntries{
        wgpu::BindGroupEntry{
            .binding = 0,
            .sampler = sampler,
        },
        wgpu::BindGroupEntry{
            .binding = 1,
            .textureView = req.srcView,
        },
        wgpu::BindGroupEntry{
            .binding = 2,
            .buffer = g_uniformBuffer,
            .offset = req.uniformRange.offset,
            .size = req.uniformRange.size,
        },
    };
    const wgpu::BindGroupDescriptor bindGroupDescriptor{
        .layout = g_bindGroupLayout,
        .entryCount = bindGroupEntries.size(),
        .entries = bindGroupEntries.data(),
    };
    bindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
  }

  const std::array colorAttachments{
      wgpu::RenderPassColorAttachment{
          .view = req.dst->attachmentTextureView,
          .loadOp = wgpu::LoadOp::Clear,
          .storeOp = wgpu::StoreOp::Store,
          .clearValue = {0.0, 0.0, 0.0, 0.0},
      },
  };
  const wgpu::RenderPassDescriptor renderPassDescriptor{
      .label = "TexCopyConv Pass",
      .colorAttachmentCount = colorAttachments.size(),
      .colorAttachments = colorAttachments.data(),
  };
  const auto pass = cmd.BeginRenderPass(&renderPassDescriptor);
  pass.SetPipeline(pipeline);
  pass.SetBindGroup(0, bindGroup);
  pass.Draw(3);
  pass.End();
}

void run(const wgpu::CommandEncoder& cmd, const ConvRequest& req) {
  const auto it = g_pipelines.find(req.fmt);
  if (it == g_pipelines.end()) {
    Log.fatal("No copy conversion pipeline for format {}", static_cast<int>(req.fmt));
  }
  execute(cmd, req, it->second);
}

void blit(const wgpu::CommandEncoder& cmd, const ConvRequest& req) { execute(cmd, req, g_blitPipeline); }

void vfilter_blit(const wgpu::CommandEncoder& cmd, const ConvRequest& req) {
  execute(cmd, req, g_vfilterPipeline);
}

} // namespace aurora::gfx::tex_copy_conv
