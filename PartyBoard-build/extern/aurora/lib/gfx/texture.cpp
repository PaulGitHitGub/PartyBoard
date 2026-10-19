#include "common.hpp"

#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "aurora/aurora.h"
#include "texture.hpp"
#include "rgba_mips.hpp"
#include "texture_convert.hpp"
#include "../gx/gx_fmt.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

#include <fmt/format.h>
#include <tracy/Tracy.hpp>
#include <webgpu/webgpu_cpp.h>

namespace aurora::gfx {
using webgpu::g_device;
using webgpu::g_queue;

namespace {
Module Log("aurora::gfx");

wgpu::Extent3D physical_size(wgpu::Extent3D size, TextureFormatInfo info) {
  const uint32_t width = ((size.width + info.blockWidth - 1) / info.blockWidth) * info.blockWidth;
  const uint32_t height = ((size.height + info.blockHeight - 1) / info.blockHeight) * info.blockHeight;
  return {.width = width, .height = height, .depthOrArrayLayers = size.depthOrArrayLayers};
}

bool setup_swizzle(wgpu::TextureComponentSwizzleDescriptor& swizzle, u32 format) noexcept {
  if (!webgpu::g_textureComponentSwizzleSupported) {
    return false;
  }

  switch (format) {
  case GX_TF_R8_PC:
    swizzle.swizzle.r = wgpu::ComponentSwizzle::R;
    swizzle.swizzle.g = wgpu::ComponentSwizzle::R;
    swizzle.swizzle.b = wgpu::ComponentSwizzle::R;
    swizzle.swizzle.a = wgpu::ComponentSwizzle::R;
    return true;
  case GX_TF_RG8_PC:
    swizzle.swizzle.r = wgpu::ComponentSwizzle::R;
    swizzle.swizzle.g = wgpu::ComponentSwizzle::R;
    swizzle.swizzle.b = wgpu::ComponentSwizzle::R;
    swizzle.swizzle.a = wgpu::ComponentSwizzle::G;
    return true;
  default:
    return false;
  }
}
} // namespace

TextureHandle new_static_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 format, ArrayRef<uint8_t> data,
                                    bool tlut, const char* label, bool worldMips) noexcept {
  ZoneScoped;

  ConvertedTexture converted;
  if (format != InvalidTextureFormat) {
    if (tlut) {
      CHECK(height == 1 && mips == 1, "TLUT must have one row and one mip");
      converted = convert_tlut(format, width, data);
    } else {
      converted = convert_texture(format, width, height, mips, data);
    }
  }
  if (!converted.data.empty()) data = converted.data;
  RgbaMipChain generated;
  // Colour formats, CMPR included: decoded to RGBA8 on the headset (no BC
  // there), and the boards' large textures, which shimmer at a distance
  // without mips. Intensity formats stay out: often masks or fonts, their
  // alpha is not a colour's coverage.
  const bool colorFormat = format == GX_TF_RGBA8 || format == GX_TF_RGB565 ||
      format == GX_TF_RGB5A3 || format == GX_TF_RGBA8_PC || format == GX_TF_CMPR;
  if (worldMips && !tlut && colorFormat && mips == 1 && width >= 16 && height >= 16 &&
      width <= 2048 && height <= 2048 && to_wgpu(format) == wgpu::TextureFormat::RGBA8Unorm) {
    CHECK(data.size() == UINT32_MAX || data.size() >= size_t(width) * height * 4, "RGBA mip source truncated");
    generated = rgba_mip_chain(data.data(), width, height);
    data = {generated.bytes.data(), generated.bytes.size()};
    mips = generated.levels;
  }
  auto handle = new_dynamic_texture_2d(width, height, mips, format, label);
  auto& ref = *handle;
  ref.hasArbitraryMips = converted.hasArbitraryMips;
  ref.worldMipsPrepared = worldMips;
  ref.generatedWorldMips = generated.levels > 1;

  uint32_t offset = 0;
  for (uint32_t mip = 0; mip < mips; ++mip) {
    const wgpu::Extent3D mipSize{
        .width = std::max(ref.size.width >> mip, 1u),
        .height = std::max(ref.size.height >> mip, 1u),
        .depthOrArrayLayers = ref.size.depthOrArrayLayers,
    };
    const auto info = format_info(ref.format);
    const auto physicalSize = physical_size(mipSize, info);
    const uint32_t widthBlocks = physicalSize.width / info.blockWidth;
    const uint32_t heightBlocks = physicalSize.height / info.blockHeight;
    const uint32_t bytesPerRow = widthBlocks * info.blockSize;
    const uint32_t dataSize = bytesPerRow * heightBlocks * mipSize.depthOrArrayLayers;
    CHECK(offset + dataSize <= data.size(), "new_static_texture_2d[{}]: expected at least {} bytes, got {}", label,
          offset + dataSize, data.size());
    const wgpu::TexelCopyTextureInfo dstView{
        .texture = ref.texture,
        .mipLevel = mip,
    };
    if constexpr (UseTextureBuffer) {
      queue_texture_upload_data(data.data() + offset, dataSize, bytesPerRow, heightBlocks, std::move(dstView),
                                physicalSize);
    } else {
      const wgpu::TexelCopyBufferLayout dataLayout{
          .bytesPerRow = bytesPerRow,
          .rowsPerImage = heightBlocks,
      };
      g_queue.WriteTexture(&dstView, data.data() + offset, dataSize, &dataLayout, &physicalSize);
    }
    offset += dataSize;
  }
  if (data.size() != UINT32_MAX && offset < data.size()) {
    Log.warn("new_static_texture_2d[{}]: texture used {} bytes, but given {} bytes", label, offset, data.size());
  }
  return handle;
}

TextureHandle new_dynamic_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat,
                                     const char* label) noexcept {
  ZoneScopedS(3);
  const auto wgpuFormat = to_wgpu(gxFormat);
  const wgpu::Extent3D size{
      .width = width,
      .height = height,
      .depthOrArrayLayers = 1,
  };
  const wgpu::TextureDescriptor textureDescriptor{
      .label = label,
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = wgpuFormat,
      .mipLevelCount = mips,
      .sampleCount = 1,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);
  const auto viewLabel = fmt::format("{} view", label);
  wgpu::TextureViewDescriptor textureViewDescriptor{
      .label = viewLabel.c_str(),
      .format = wgpuFormat,
      .dimension = wgpu::TextureViewDimension::e2D,
      .mipLevelCount = mips,
  };
  wgpu::TextureComponentSwizzleDescriptor swizzle;
  if (setup_swizzle(swizzle, gxFormat)) {
    textureViewDescriptor.nextInChain = &swizzle;
  }
  auto textureView = texture.CreateView(&textureViewDescriptor);
  return std::make_shared<TextureRef>(std::move(texture), std::move(textureView), wgpu::TextureView{}, size, wgpuFormat,
                                      mips, gxFormat);
}

TextureHandle new_render_texture(uint32_t width, uint32_t height, u32 gxFormat, const char* label) noexcept {
  ZoneScoped;

  const auto wgpuFormat = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  const wgpu::Extent3D size{
      .width = width,
      .height = height,
      .depthOrArrayLayers = 1,
  };
  const wgpu::TextureDescriptor textureDescriptor{
      .label = label,
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::RenderAttachment,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = wgpuFormat,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);

  // Create texture view for color attachments
  const auto viewLabel = fmt::format("{} view", label);
  wgpu::TextureViewDescriptor textureViewDescriptor{
      .label = viewLabel.c_str(),
      .format = wgpuFormat,
      .dimension = wgpu::TextureViewDimension::e2D,
  };
  auto attachmentTextureView = texture.CreateView(&textureViewDescriptor);
  wgpu::TextureView sampleTextureView = attachmentTextureView;
  return std::make_shared<TextureRef>(std::move(texture), std::move(sampleTextureView),
                                      std::move(attachmentTextureView), size, wgpuFormat, 1, gxFormat);
}

TextureHandle new_conv_texture(uint32_t width, uint32_t height, u32 gxFormat, const char* label) noexcept {
  ZoneScoped;

  const auto wgpuFormat = to_wgpu(gxFormat);
  const wgpu::Extent3D size{
      .width = width,
      .height = height,
      .depthOrArrayLayers = 1,
  };
  const wgpu::TextureDescriptor textureDescriptor{
      .label = label,
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::RenderAttachment,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = wgpuFormat,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);

  // Create texture view for color attachments
  const auto viewLabel = fmt::format("{} view", label);
  wgpu::TextureViewDescriptor textureViewDescriptor{
      .label = viewLabel.c_str(),
      .format = wgpuFormat,
      .dimension = wgpu::TextureViewDimension::e2D,
  };
  auto attachmentTextureView = texture.CreateView(&textureViewDescriptor);
  wgpu::TextureView sampleTextureView = attachmentTextureView;
  return std::make_shared<TextureRef>(std::move(texture), std::move(sampleTextureView),
                                      std::move(attachmentTextureView), size, wgpuFormat, 1, gxFormat);
}

void write_texture(TextureRef& ref, ArrayRef<uint8_t> data) noexcept {
  ZoneScoped;

  ConvertedTexture converted;
  if (ref.gxFormat != InvalidTextureFormat) {
    converted = convert_texture(ref.gxFormat, ref.size.width, ref.size.height,
                                ref.generatedWorldMips ? 1 : ref.mipCount, data);
    ref.hasArbitraryMips = converted.hasArbitraryMips;
    if (!converted.data.empty()) {
      data = converted.data;
    }
  }

  RgbaMipChain generated;
  if (ref.generatedWorldMips) {
    CHECK(data.size() == UINT32_MAX || data.size() >= size_t(ref.size.width) * ref.size.height * 4,
          "RGBA mip update truncated");
    generated = rgba_mip_chain(data.data(), ref.size.width, ref.size.height);
    data = {generated.bytes.data(), generated.bytes.size()};
  }

  uint32_t offset = 0;
  for (uint32_t mip = 0; mip < ref.mipCount; ++mip) {
    const wgpu::Extent3D mipSize{
        .width = std::max(ref.size.width >> mip, 1u),
        .height = std::max(ref.size.height >> mip, 1u),
        .depthOrArrayLayers = ref.size.depthOrArrayLayers,
    };
    const auto info = format_info(ref.format);
    const auto physicalSize = physical_size(mipSize, info);
    const uint32_t widthBlocks = physicalSize.width / info.blockWidth;
    const uint32_t heightBlocks = physicalSize.height / info.blockHeight;
    const uint32_t bytesPerRow = widthBlocks * info.blockSize;
    const uint32_t dataSize = bytesPerRow * heightBlocks * mipSize.depthOrArrayLayers;
    CHECK(offset + dataSize <= data.size(), "write_texture: expected at least {} bytes, got {}", offset + dataSize,
          data.size());
    const wgpu::TexelCopyTextureInfo dstView{
        .texture = ref.texture,
        .mipLevel = mip,
    };
    if constexpr (UseTextureBuffer) {
      queue_texture_upload_data(data.data() + offset, dataSize, bytesPerRow, heightBlocks, std::move(dstView),
                                physicalSize);
    } else {
      const wgpu::TexelCopyBufferLayout dataLayout{
          .bytesPerRow = bytesPerRow,
          .rowsPerImage = heightBlocks,
      };
      g_queue.WriteTexture(&dstView, data.data() + offset, dataSize, &dataLayout, &physicalSize);
    }
    offset += dataSize;
  }
  if (data.size() != UINT32_MAX && offset < data.size()) {
    Log.warn("write_texture: texture used {} bytes, but given {} bytes", offset, data.size());
  }
}
} // namespace aurora::gfx
