#pragma once

#include "common.hpp"

#include <functional>

namespace aurora::gfx::clear {
struct PipelineConfig;
} // namespace aurora::gfx::clear

namespace aurora::gx {
struct PipelineConfig;
} // namespace aurora::gx

namespace aurora::rmlui {
struct PipelineConfig;
} // namespace aurora::rmlui

namespace aurora::gfx {

using NewPipelineCallback = std::function<wgpu::RenderPipeline()>;

/* How many queued pipelines have not been created yet. Non-blocking, so a
 * caller can drive its own frame loop and show progress instead of freezing. */
size_t pipelines_remaining();

using PipelineCacheProgress = std::function<void(size_t created, size_t total)>;

/* Blocks until every pipeline queued from the cache has been created, reporting
 * progress. Returns how many were built. Safe to call once after
 * initialize_pipeline_cache(); gives up after a timeout rather than hanging. */
size_t wait_for_pipeline_cache(const PipelineCacheProgress& progress = {});

void initialize_pipeline_cache();
void shutdown_pipeline_cache();
void begin_pipeline_frame();
void end_pipeline_frame();

template <typename Config>
PipelineRef find_pipeline(ShaderType type, const Config& config, NewPipelineCallback&& cb);

bool get_pipeline(PipelineRef ref, wgpu::RenderPipeline& pipeline);

} // namespace aurora::gfx
