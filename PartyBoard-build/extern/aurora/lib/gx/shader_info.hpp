#pragma once

#include "gx.hpp"

#include <array>

namespace aurora::gx {
ShaderInfo build_shader_info(const ShaderConfig& config) noexcept;
gfx::Range build_uniform(const ShaderInfo& info, uint32_t vtxStart, const BindGroupRanges& ranges) noexcept;
// Both eyes' blocks from one build, each with its own projection (gfx/stereo.cpp).
std::array<gfx::Range, 2> build_uniform_stereo(const ShaderInfo& info, uint32_t vtxStart, const BindGroupRanges& ranges,
                                               const Mat4x4<float>& left, const Mat4x4<float>& right) noexcept;
// Both eyes in one instanced draw (gfx/stereo.cpp): one block holding both
// projections and where each eye goes in the image. stereo_instanced_fits
// says whether the extra members fit the uniform binding.
constexpr u32 kStereoInstancedUniformSize = sizeof(Mat4x4<float>) + 2 * sizeof(Vec4<float>);
bool stereo_instanced_fits(const ShaderInfo& info) noexcept;
gfx::Range build_uniform_stereo_instanced(const ShaderInfo& info, uint32_t vtxStart, const BindGroupRanges& ranges,
                                          const Mat4x4<float>& left, const Mat4x4<float>& right,
                                          const Vec4<float>& eyeX, const Vec4<float>& eyeClip) noexcept;
u8 color_channel(GXChannelID id) noexcept;
}; // namespace aurora::gx
