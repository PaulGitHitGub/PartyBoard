#include "pipeline.hpp"

#include "../gfx/stereo.hpp"
#include "../webgpu/gpu.hpp"
#include "gx_fmt.hpp"
#include "shader_info.hpp"
#include "tracy/Tracy.hpp"

namespace aurora::gx {
static Module Log("aurora::gx");

wgpu::RenderPipeline create_pipeline(const PipelineConfig& config) {
  ZoneScoped;
  if (config.shaderConfig.stereo) {
    // A headset's eyes in one instanced draw. Any uncaptured WebGPU error is
    // fatal, so this is built in error scopes: should it fail, there is no
    // pipeline, and stereo.cpp goes back to drawing each eye on its own. A
    // driver's own failure (Adreno's VK_ERROR_UNKNOWN) is an Internal error,
    // not a Validation one, so every filter gets a scope.
    constexpr wgpu::ErrorFilter kFilters[]{wgpu::ErrorFilter::Validation, wgpu::ErrorFilter::OutOfMemory,
                                           wgpu::ErrorFilter::Internal};
    for (const auto filter : kFilters) webgpu::g_device.PushErrorScope(filter);
    auto pipeline = build_pipeline(config, {}, build_shader(config.shaderConfig), "GX Stereo Pipeline");
    bool failed = false;
    std::string message;
    for (size_t i = 0; i < std::size(kFilters); ++i) {
      bool scopeFailed = true;
      std::string scopeMessage = "no answer";
      const auto future = webgpu::g_device.PopErrorScope(
          wgpu::CallbackMode::WaitAnyOnly,
          [&](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView text) {
            scopeFailed = status != wgpu::PopErrorScopeStatus::Success || type != wgpu::ErrorType::NoError;
            scopeMessage = std::string{text.data != nullptr ? std::string_view{text} : std::string_view{}};
          });
      if (webgpu::g_instance.WaitAny(future, UINT64_MAX) != wgpu::WaitStatus::Success || scopeFailed) {
        failed = true;
        if (message.empty()) message = scopeMessage;
      }
    }
    if (failed) {
      gfx::stereo::instanced_failed(message);
      return {};
    }
    return pipeline;
  }  const auto shader = build_shader(config.shaderConfig);
  return build_pipeline(config, {}, shader, "GX Pipeline");
}

void render(const DrawData& data, const wgpu::RenderPassEncoder& pass) {
  if (!gfx::bind_pipeline(data.pipeline, pass)) {
    return;
  }

  const std::array offsets{data.uniformRange.offset};
  pass.SetBindGroup(1, gfx::g_uniformBindGroup, offsets.size(), offsets.data());
  if (data.bindGroups.textureBindGroup) {
    pass.SetBindGroup(2, gfx::find_bind_group(data.bindGroups.textureBindGroup));
  }
  pass.SetIndexBuffer(gfx::g_indexBuffer, wgpu::IndexFormat::Uint16, data.idxRange.offset, data.idxRange.size);
  if (data.dstAlpha != UINT32_MAX) {
    const wgpu::Color color{0.f, 0.f, 0.f, data.dstAlpha / 255.f};
    pass.SetBlendConstant(&color);
  }
  if (data.indexCount == 0) {
    pass.Draw(data.vtxCount, data.instanceCount);
  } else {
    pass.DrawIndexed(data.indexCount, data.instanceCount);
  }
}
} // namespace aurora::gx
