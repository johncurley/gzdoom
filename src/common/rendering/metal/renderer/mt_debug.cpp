/*
**  Metal backend
**  Copyright (c) 2020-2025 John Curley and contributors
**
**  This software is provided 'as-is', without any express or implied
**  warranty.  In no event will the authors be held liable for any damages
**  arising from the use of this software.
**
**  Permission is granted to anyone to use this software for any purpose,
**  including commercial applications, and to alter and redistribute it
**  freely, subject to the following restrictions:
**
**  1. The origin of this software must not be misrepresented; you must not
**     claim that you wrote the original software. If you use this software
**     in a product, an acknowledgment in the product documentation would be
**     appreciated but is not required.
**  2. Altered source versions must not be misrepresented; you must not claim
**     that you wrote the original software.
**  3. This notice may not be removed or altered from any source distribution.
**
*/

#include "mt_debug.h"
#include "c_dispatch.h"
#include "metal/renderer/mt_renderbuffers.h"
#include "metal/system/mt_renderdevice.h"
#include "metal/system/mt_version.h"
#include "printf.h"
#include "v_video.h"
#include <algorithm>
#include <ctime>
#include <cstring>

EXTERN_CVAR(Bool, mt_debug)
EXTERN_CVAR(Bool, mt_compute_ao)
EXTERN_CVAR(Bool, mt_compute_bloom)
EXTERN_CVAR(Int, mt_compute_bloom_composite)
EXTERN_CVAR(Int, mt_compute_ao_worldpos_debug)
EXTERN_CVAR(Int, mt_compute_ao_directions)
EXTERN_CVAR(Int, mt_compute_ao_steps)
EXTERN_CVAR(Bool, gl_bloom)
EXTERN_CVAR(Int, gl_ssao)
EXTERN_CVAR(Int, gl_ssao_debug)


namespace {
MetalRenderDevice *GetActiveMetalRenderDevice() {
  if (!screen || !screen->IsMetal())
    return nullptr;

  return static_cast<MetalRenderDevice *>(screen);
}

MtDebugManager *GetActiveMetalDebugManager() {
  auto fb = GetActiveMetalRenderDevice();
  return fb ? fb->GetDebugManager() : nullptr;
}

void PrintMetricSummary(MtDebugManager *debug, MtMetric metric) {
  const auto &info = MtMetricHistory::GetInfo(metric);
  const auto stats = debug->GetMetricStats(metric);
  if (stats.samples == 0)
    return;

  Printf(PRINT_HIGH, "  %-7s %-5s current=%6.3fms active_avg=%6.3fms min=%6.3fms max=%6.3fms samples=%zu\n",
         info.group, info.label, stats.current, stats.average,
         stats.minimum, stats.maximum, stats.samples);
}

void PrintFrameSummary(MtDebugManager *debug) {
  const auto stats = debug->GetFrameTimeStats();
  if (stats.maximum <= 0.0f)
    return;

  const float fps = stats.average > 0.0f ? 1000.0f / stats.average : 0.0f;
  Printf(PRINT_HIGH, "  Frame          current=%6.3fms avg=%6.3fms min=%6.3fms max=%6.3fms fps=%5.1f\n",
         stats.current, stats.average, stats.minimum, stats.maximum, fps);
}
}

MtDebugManager::MtDebugManager(MetalRenderDevice *renderDevice)
    : fb(renderDevice), mMetricHistory(120) {
  mFrameTimeHistory.reserve(120);
}

MtDebugManager::~MtDebugManager() {
  StopLogging();
}

void MtDebugManager::BeginFrame() {
  // Reset current frame stats
  mCurrentFrameStats = {};

  // Record start time
  mFrameStartTime = std::chrono::high_resolution_clock::now();
}

void MtDebugManager::EndFrame() {
  // Calculate frame time
  auto endTime = std::chrono::high_resolution_clock::now();
  float frameTimeMs =
      std::chrono::duration<float, std::milli>(endTime - mFrameStartTime)
          .count();

  mCurrentFrameStats.frameTimeMs = frameTimeMs;

  // Drain any real GPU frame time reported asynchronously by a just-
  // completed command buffer's completion handler. Due to pipelining
  // (maxDrawableCount) this value typically corresponds to a frame 1-2
  // iterations behind the current one -- tagging it onto "now" is fine for
  // a rolling-average display, same tradeoff every async GPU timer makes.
  float gpuFrameTimeMs =
      mPendingGPUFrameTimeMs.exchange(-1.0f, std::memory_order_relaxed);
  if (gpuFrameTimeMs >= 0.0f) {
    RecordMetric(MtMetric::FrameGPU, gpuFrameTimeMs);
  }

  // Store in history
  mFrameTimeHistory.push_back(frameTimeMs);
  if (mFrameTimeHistory.size() > 120) {
    mFrameTimeHistory.erase(mFrameTimeHistory.begin());
  }

  mFrameIndex++;

  // Save to last frame stats
  mLastFrameStats = mCurrentFrameStats;
  mMetricHistory.RecordFrame(mCurrentFrameStats.metrics);

  // Log if enabled
  if (IsLogging()) {
    WriteLogEntry();
  }

  // Print every frame if debug enabled
  if (mt_debug) {
    PrintDebugStats();
  }
}

void MtDebugManager::RecordDrawCallCategory(int vertexCount, int indexCount, const char *category) {
  mCurrentFrameStats.drawCallCount++;
  mCurrentFrameStats.indexCount += indexCount;

  // Skip category bucketing when not logging — avoids strstr overhead on
  // every draw call during normal gameplay.
  if (!IsLogging())
    return;

  if (!category) {
    mCurrentFrameStats.other_draws++;
    return;
  }

  // Category strings are compile-time constants set by us; compare by first
  // character(s) to avoid strstr scanning the whole string on every draw.
  switch (category[0]) {
    case 'g': mCurrentFrameStats.geometry_draws++; break; // "geometry"
    case 'h': mCurrentFrameStats.ui_draws++;       break; // "hud"
    case 'u': mCurrentFrameStats.ui_draws++;       break; // "ui"
    case 's': mCurrentFrameStats.sky_draws++;      break; // "sky"
    case 'p': mCurrentFrameStats.sky_draws++;      break; // "portal"
    case 'l': mCurrentFrameStats.light_draws++;    break; // "light"
    default:  mCurrentFrameStats.other_draws++;    break;
  }
}

void MtDebugManager::RecordStateChange(const char *stateName) {
  mCurrentFrameStats.stateChanges++;
}

void MtDebugManager::RecordTextureAllocation(size_t bytes, const char *name) {
  mCurrentFrameStats.textureAllocations++;
  mCurrentFrameStats.gpuMemoryAllocated += bytes;
  mTotalAllocated += bytes;
  mCurrentFrameStats.gpuMemoryCurrent = mTotalAllocated;

  if (mt_debug) {
    Printf(PRINT_LOG,
           "Metal Debug: Texture allocation '%s' - %zu bytes (Total: %zu MB)\n",
           name, bytes, mTotalAllocated / (1024 * 1024));
  }
}

void MtDebugManager::RecordBufferAllocation(size_t bytes, const char *name) {
  mCurrentFrameStats.bufferAllocations++;
  mCurrentFrameStats.gpuMemoryAllocated += bytes;
  mTotalAllocated += bytes;
  mCurrentFrameStats.gpuMemoryCurrent = mTotalAllocated;

  if (mt_debug) {
    Printf(PRINT_LOG,
           "Metal Debug: Buffer allocation '%s' - %zu bytes (Total: %zu MB)\n",
           name, bytes, mTotalAllocated / (1024 * 1024));
  }
}

void MtDebugManager::RecordTextureMipmap(const char *name, int levels) {
  mCurrentFrameStats.textureMipmaps++;

  if (mt_debug) {
    Printf(PRINT_LOG, "Metal Debug: Generated %d mipmap levels for '%s'\n",
           levels, name);
  }
}

void MtDebugManager::RecordMetric(MtMetric metric, float durationMs) {
  mCurrentFrameStats.metrics.Record(metric, durationMs);
}

void MtDebugManager::RecordAOTiming(bool computePath, float durationMs) {
  RecordMetric(computePath ? MtMetric::ComputeAO : MtMetric::PPAO,
               durationMs);
}

void MtDebugManager::RecordBloomTiming(float durationMs) {
  RecordMetric(MtMetric::ComputeBloom, durationMs);
}

void MtDebugManager::RecordGPUFrameTimeAsync(float durationMs) {
  mPendingGPUFrameTimeMs.store(durationMs, std::memory_order_relaxed);
}

void MtDebugManager::RecordStall(const char *type, float durationMs) {
  mCurrentFrameStats.stallCount++;
  mCurrentFrameStats.stallTotalMs += durationMs;
  if (durationMs > mCurrentFrameStats.stallMaxMs)
    mCurrentFrameStats.stallMaxMs = durationMs;
}

void MtDebugManager::RecordTextureUpload(float durationMs) {
  mCurrentFrameStats.textureUploads++;
  RecordMetric(MtMetric::TextureUploadCPU, durationMs);
}

void MtDebugManager::RecordExtraCommandBuffer() {
  mCurrentFrameStats.extraCommandBuffers++;
}

void MtDebugManager::PrintDebugStats() {
  if (!fb || !mt_debug)
    return;

  float avgFrameTime = GetAverageFrameTime();
  float fps = (avgFrameTime > 0) ? 1000.0f / avgFrameTime : 0.0f;

  Printf(PRINT_HIGH,
         "Metal: FPS: %.1f | Frame: %.2fms | Draws: %d | Verts: %d | State: %d",
         fps, avgFrameTime, mCurrentFrameStats.drawCallCount,
         mCurrentFrameStats.indexCount, mCurrentFrameStats.stateChanges);

  if (mCurrentFrameStats.stallCount > 0)
    Printf(PRINT_HIGH, " | Stalls: %d (%.2fms, max %.2fms)",
           mCurrentFrameStats.stallCount,
           mCurrentFrameStats.stallTotalMs,
           mCurrentFrameStats.stallMaxMs);

  const float textureUploadMs = mCurrentFrameStats.GetMetric(MtMetric::TextureUploadCPU);
  if (mCurrentFrameStats.extraCommandBuffers > 0 || textureUploadMs > 0.0f)
    Printf(PRINT_HIGH, " | TexUpload: %d (%.2fms, extra CBs: %d)",
           mCurrentFrameStats.textureUploads,
           textureUploadMs,
           mCurrentFrameStats.extraCommandBuffers);

  const float computeAO = mCurrentFrameStats.GetMetric(MtMetric::ComputeAO);
  const float computeBloom = mCurrentFrameStats.GetMetric(MtMetric::ComputeBloom);
  if (computeAO > 0.0f || computeBloom > 0.0f) {
    Printf(PRINT_HIGH, " | Compute:");
    if (computeAO > 0.0f)
      Printf(PRINT_HIGH, " AO=%.2fms", computeAO);
    if (computeBloom > 0.0f)
      Printf(PRINT_HIGH, " Bloom=%.2fms", computeBloom);
  }

  const float ppAO = mCurrentFrameStats.GetMetric(MtMetric::PPAO);
  const float ppBloom = mCurrentFrameStats.GetMetric(MtMetric::PPBloom);
  if (ppAO > 0.0f || ppBloom > 0.0f) {
    Printf(PRINT_HIGH, " | PP:");
    if (ppAO > 0.0f)
      Printf(PRINT_HIGH, " AO=%.2fms", ppAO);
    if (ppBloom > 0.0f)
      Printf(PRINT_HIGH, " Bloom=%.2fms", ppBloom);
  }

  const float gpuFrame = mCurrentFrameStats.GetMetric(MtMetric::FrameGPU);
  if (gpuFrame > 0.0f)
    Printf(PRINT_HIGH, " | GPU: %.2fms", gpuFrame);

  Printf(PRINT_HIGH, "\n");
  
  // Show categorized draw calls if significant
  int total = mCurrentFrameStats.drawCallCount;
  if (total > 0) {
    Printf(PRINT_HIGH, 
           "  Categories: Geo=%d UI=%d Sky=%d Light=%d Other=%d\n",
           mCurrentFrameStats.geometry_draws,
           mCurrentFrameStats.ui_draws,
           mCurrentFrameStats.sky_draws,
           mCurrentFrameStats.light_draws,
           mCurrentFrameStats.other_draws);
  }
}

void MtDebugManager::PrintArchitectureInfo() {
  if (!fb)
    return;

  const auto &version = fb->mVersionManager;

  Printf(PRINT_HIGH,
         "\n=== Metal Renderer Architecture Info ===\n"
         "GPU Architecture: %s\n"
         "Metal Version: %d.%d\n"
         "OS Version: %d.%d.%d\n"
         "TBDR: %s | Memoryless: %s\n"
         "Managed Storage: %s | Binary Archives: %s\n"
         "Max Drawable Count: %d\n"
         "=========================================\n",
         version.GetArchName(), version.metalVersion / 10,
         version.metalVersion % 10, version.osMajor, version.osMinor,
         version.osPatch, version.isTBDR ? "Yes" : "No",
         version.supportsMemoryless ? "Yes" : "No",
         version.useManagedStorage ? "Yes" : "No",
         version.supportsBinaryArchives ? "Yes" : "No",
         version.maxDrawableCount);
}

void MtDebugManager::StartLogging(const char *filepath) {
  if (IsLogging())
    StopLogging();

  // Generate default filename if not provided
  std::string filename;
  if (!filepath) {
    time_t now = time(nullptr);
    struct tm *timeinfo = localtime(&now);
    char buffer[256];
    strftime(buffer, sizeof(buffer), "metal_debug_%Y%m%d_%H%M%S.csv",
             timeinfo);
    filename = buffer;
  } else {
    filename = filepath;
  }

  mLogFile = fopen(filename.c_str(), "w");
  if (mLogFile) {
    // Write header
    fprintf(mLogFile,
            "Frame,FPS,DrawCalls,FrameTimeMS,FrameGPUms,GPUMemoryMB,StateChanges,"
            "TextureAllocs,BufferAllocs,ComputeAOCPUms,ComputeBloomCPUms,"
            "PPAOCPUms,PPBloomCPUms,TextureUploads,TextureUploadCPUms,ExtraCommandBuffers\n");
    mLogFrameCount = 0;
    mLoggingStartTime = std::chrono::high_resolution_clock::now();
    Printf(PRINT_HIGH, "Metal Debug: Logging started to %s\n", filename.c_str());
  } else {
    Printf(PRINT_HIGH, "Metal Debug: Failed to open log file %s\n",
           filename.c_str());
  }
}

void MtDebugManager::StopLogging() {
  if (mLogFile) {
    fclose(mLogFile);
    mLogFile = nullptr;
    Printf(PRINT_HIGH, "Metal Debug: Logging stopped (%d frames written)\n",
           mLogFrameCount);
  }
}

void MtDebugManager::WriteLogEntry() {
  if (!mLogFile)
    return;

  float avgFrameTime = GetAverageFrameTime();
  float fps = (avgFrameTime > 0) ? 1000.0f / avgFrameTime : 0.0f;

  fprintf(mLogFile, "%d,%.1f,%d,%.2f,%.3f,%.1f,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%d,%.3f,%d\n", mLogFrameCount, fps,
          mCurrentFrameStats.drawCallCount, mCurrentFrameStats.frameTimeMs,
          mCurrentFrameStats.GetMetric(MtMetric::FrameGPU),
          mCurrentFrameStats.gpuMemoryCurrent / (1024.0f * 1024.0f),
          mCurrentFrameStats.stateChanges,
          mCurrentFrameStats.textureAllocations,
          mCurrentFrameStats.bufferAllocations,
          mCurrentFrameStats.GetMetric(MtMetric::ComputeAO),
          mCurrentFrameStats.GetMetric(MtMetric::ComputeBloom),
          mCurrentFrameStats.GetMetric(MtMetric::PPAO),
          mCurrentFrameStats.GetMetric(MtMetric::PPBloom),
          mCurrentFrameStats.textureUploads,
          mCurrentFrameStats.GetMetric(MtMetric::TextureUploadCPU),
          mCurrentFrameStats.extraCommandBuffers);

  mLogFrameCount++;

  // Flush every 300 frames to avoid data loss
  if ((mLogFrameCount % 300) == 0) {
    fflush(mLogFile);
  }
}

float MtDebugManager::GetAverageFrameTime() const {
  if (mFrameTimeHistory.empty())
    return 0.0f;

  float sum = 0.0f;
  for (float t : mFrameTimeHistory) {
    sum += t;
  }
  return sum / mFrameTimeHistory.size();
}

MtMetricStats MtDebugManager::GetFrameTimeStats() const {
  MtMetricStats stats;
  if (mFrameTimeHistory.empty())
    return stats;

  stats.current = mFrameTimeHistory.back();
  stats.minimum = stats.current;
  stats.maximum = stats.current;

  float total = 0.0f;
  for (float value : mFrameTimeHistory) {
    total += value;
    stats.minimum = std::min(stats.minimum, value);
    stats.maximum = std::max(stats.maximum, value);
  }
  stats.average = total / (float)mFrameTimeHistory.size();
  return stats;
}

CCMD(mt_metrics)
{
  auto debug = GetActiveMetalDebugManager();
  if (!debug) {
    Printf(PRINT_HIGH, "Metal metrics are not available.\n");
    return;
  }

  Printf(PRINT_HIGH, "Metal metrics over the rolling debug window:\n");
  PrintFrameSummary(debug);
  PrintMetricSummary(debug, MtMetric::FrameGPU);
  if (auto fb = GetActiveMetalRenderDevice()) {
    Printf(PRINT_HIGH, "  Per-pass GPU timing capability (stage boundary counter sampling): %s\n",
           fb->mVersionManager.supportsStageCounterSampling ? "supported" : "NOT supported on this GPU/driver");
  }
  PrintMetricSummary(debug, MtMetric::ComputeAO);
  PrintMetricSummary(debug, MtMetric::ComputeBloom);
  PrintMetricSummary(debug, MtMetric::PPAO);
  PrintMetricSummary(debug, MtMetric::PPBloom);
  PrintMetricSummary(debug, MtMetric::TextureUploadCPU);
  Printf(PRINT_HIGH, "  Extra command buffers (last frame, world/lightmap texture uploads + mipmap regen): %d\n",
         debug->GetLastFrameStats().extraCommandBuffers);
}

// Dumps the capabilities MtVersionManager probed at device creation. None of
// these were reachable from in-game before, which made it impossible to tell
// which of several capability-gated code paths a given machine actually
// takes -- the bloom composite tier and the AO resolution gate both branch on
// values printed here, and both were being reasoned about from assumptions
// about the hardware rather than from the detected values.
CCMD(mt_caps)
{
  auto fb = GetActiveMetalRenderDevice();
  if (!fb) {
    Printf(PRINT_HIGH, "Metal capabilities are not available.\n");
    return;
  }
  const auto &v = fb->mVersionManager;
  Printf(PRINT_HIGH, "Metal device capabilities:\n");
  Printf(PRINT_HIGH, "  Architecture:            %s\n", v.GetArchName());
  Printf(PRINT_HIGH, "  Metal version (approx):  %d.%d\n", v.metalVersion / 10, v.metalVersion % 10);
  Printf(PRINT_HIGH, "  macOS:                   %d.%d.%d\n", v.osMajor, v.osMinor, v.osPatch);
  Printf(PRINT_HIGH, "  TBDR / memoryless:       %s / %s\n",
         v.isTBDR ? "yes" : "no", v.supportsMemoryless ? "yes" : "no");
  Printf(PRINT_HIGH, "  ReadWrite BGRA8 (Tier2): %s   <- bloom Tier 2 direct-composite gate\n",
         v.supportsReadWriteBGRA8 ? "yes" : "NO (Tier 1 path)");
  // Read the format off the live texture, not off the CVAR -- the point is to
  // prove what the buffers actually are, which is the question a colour A/B
  // cannot answer from screenshots alone.
  if (auto buffers = fb->GetBuffers()) {
    const int fmt = buffers->GetSceneColorFormat();
    const char *name = fmt == (int)MTL::PixelFormatRGBA16Float ? "RGBA16Float (HDR)"
                     : fmt == (int)MTL::PixelFormatBGRA8Unorm  ? "BGRA8Unorm (LDR, clamps at 1.0)"
                                                               : "<unexpected>";
    Printf(PRINT_HIGH, "  Scene colour format:     %s   <- mt_hdr_pipeline\n", name);
  }
  Printf(PRINT_HIGH, "  Argument buffers:        %s (tier %d)\n",
         v.supportsArgumentBuffers ? "yes" : "no", v.argumentBufferTier);
  Printf(PRINT_HIGH, "  RGB10A2:                 %s\n", v.supportsRGB10A2 ? "yes" : "no");
  Printf(PRINT_HIGH, "  SIMD-group / non-uniform threadgroups: %s / %s\n",
         v.supportsSIMDGroup ? "yes" : "no", v.supportsNonUniformThreadgroups ? "yes" : "no");
  Printf(PRINT_HIGH, "  Binary archives:         %s\n", v.supportsBinaryArchives ? "yes" : "no");
  Printf(PRINT_HIGH, "  GPU timestamps:          %s\n", v.supportsGPUTimestamps ? "yes" : "no");
  Printf(PRINT_HIGH, "  Stage counter sampling:  %s   <- per-pass GPU timing gate\n",
         v.supportsStageCounterSampling ? "yes" : "no");
  Printf(PRINT_HIGH, "  Managed storage:         %s\n", v.useManagedStorage ? "yes" : "no");
  Printf(PRINT_HIGH, "  Max drawables:           %d\n", v.maxDrawableCount);
  if (v.architecture == MtGPUArchitecture::Intel) {
    Printf(PRINT_HIGH, TEXTCOLOR_YELLOW
           "  NOTE: Intel detected -- AO is force-clamped to quarter-res "
           "(mt_ao.cpp aoScale>=4) regardless of mt_compute_ao_scale.\n");
  }

  // Effective postprocess configuration. Every one of these is CVAR_ARCHIVE,
  // so a value set during one debugging session silently persists into the
  // next -- which has now invalidated three separate in-game A/B tests
  // (mt_compute_ao_worldpos_debug left on, AO directions/steps left at
  // experiment values, and mt_compute_bloom left off, which made the bloom
  // composite modes inert because the compute path never ran at all).
  // Printing the resolved path removes the guesswork.
  {
    const bool computeBloomOn = mt_compute_bloom && fb->mBloomModule != nullptr;
    const bool tier2 = v.supportsReadWriteBGRA8;
    Printf(PRINT_HIGH, "Effective postprocess configuration:\n");
    Printf(PRINT_HIGH, "  gl_bloom:                %s\n", gl_bloom ? "on" : "OFF (no bloom at all)");
    Printf(PRINT_HIGH, "  mt_compute_bloom:        %s\n",
           computeBloomOn ? "on" : "OFF -> falls back to hw_postprocess bloom");
    if (computeBloomOn) {
      const int mode = mt_compute_bloom_composite;
      const char *path = (mode == 2 && !tier2) ? "hw_postprocess fallback (Tier 2 required but unsupported)"
                       : (tier2 && mode != 1)  ? "Tier 2 direct read-write composite"
                                               : "Tier 1 compute + raster composite";
      Printf(PRINT_HIGH, "  bloom composite path:    %s\n", path);
    }
    Printf(PRINT_HIGH, "  mt_compute_ao:           %s\n", mt_compute_ao ? "on" : "OFF");
    Printf(PRINT_HIGH, "  gl_ssao / debug:         %d / %d%s\n", (int)gl_ssao, (int)gl_ssao_debug,
           gl_ssao_debug != 0 ? "  <- debug view active, not the shaded result" : "");
    if (mt_compute_ao_worldpos_debug != 0)
      Printf(PRINT_HIGH, TEXTCOLOR_YELLOW "  mt_compute_ao_worldpos_debug = %d -- AO buffer shows a "
             "world-position grid, NOT occlusion.\n", (int)mt_compute_ao_worldpos_debug);
    if (mt_compute_ao_directions != 0 || mt_compute_ao_steps != 0)
      Printf(PRINT_HIGH, TEXTCOLOR_YELLOW "  AO sample counts overridden: directions=%d steps=%d "
             "(0 = use gl_ssao tier default)\n",
             (int)mt_compute_ao_directions, (int)mt_compute_ao_steps);
  }

  // Raw GPU family probes. MtVersionManager derives four separate flags
  // (supportsRGB10A2, metalVersion, supportsSIMDGroup,
  // supportsNonUniformThreadgroups) from supportsFamily(GPUFamilyMac2)
  // alone, so a single wrong probe silently disables all four. Printing the
  // raw answers distinguishes "this GPU genuinely lacks the family" from
  // "the probe is broken" -- worth knowing before trusting any of the
  // derived flags above.
  if (auto *dev = fb->device ? fb->device->device : nullptr) {
    struct { const char *name; MTL::GPUFamily fam; } families[] = {
      {"Common1",       MTL::GPUFamilyCommon1},
      {"Common2",       MTL::GPUFamilyCommon2},
      {"Common3",       MTL::GPUFamilyCommon3},
      {"Mac2",          MTL::GPUFamilyMac2},
      {"MacCatalyst2",  MTL::GPUFamilyMacCatalyst2},
      {"Apple1",        MTL::GPUFamilyApple1},
      {"Apple4",        MTL::GPUFamilyApple4},
      {"Apple7",        MTL::GPUFamilyApple7},
    };
    Printf(PRINT_HIGH, "  Raw supportsFamily() probes:\n");
    for (const auto &f : families)
      Printf(PRINT_HIGH, "    %-14s %s\n", f.name, dev->supportsFamily(f.fam) ? "yes" : "no");
  }
}

CCMD(mt_metrics_reset)
{
  auto debug = GetActiveMetalDebugManager();
  if (!debug) {
    Printf(PRINT_HIGH, "Metal metrics are not available.\n");
    return;
  }

  debug->ClearBenchmarkHistory();
  Printf(PRINT_HIGH, "Metal benchmark history reset.\n");
}
