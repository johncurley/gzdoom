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
#include "../mt_system_wrapper.h"
#include "c_dispatch.h"
#include "metal/renderer/mt_renderbuffers.h"
#include "metal/system/mt_commandbuffer.h"
#include "metal/textures/mt_texture.h"
#include "metal/system/mt_renderdevice.h"
#include "metal/system/mt_version.h"
#include "printf.h"
#include "v_video.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstring>

// Frame-interval trace for ACTUAL GAMEPLAY, in seconds between reports (0 = off).
//
// This exists because the benchmark harness is blind to hitching. Measured
// 2026-08-16: a compute-AO configuration that froze constantly in play reported
// avg 5.5ms / max 90.6ms / 4 stalls from the matrix suite -- indistinguishable
// from the reference path, which was smooth. A settled viewpoint in MAP06 does
// not exercise what the renderer does badly while the camera moves, so
// `Frame avg` cannot see the defect that actually makes the game unplayable.
//
// Percentiles rather than mean and max, because that is what hitching looks
// like: p50 stays healthy while p99 blows out. A mean hides exactly the frames
// the player feels, and a lone max cannot distinguish one startup stall from
// continuous stutter -- which is why the counts below are reported too.
//
// stderr, not Printf: this runs during play, where console output covers the
// screen it is measuring, and PRINT_LOG lands in the console rather than
// anywhere a harness can read. Same reason in_keytrace uses stderr.
CVAR(Int, mt_frametrace, 0, 0)

EXTERN_CVAR(Bool, mt_debug)
EXTERN_CVAR(Bool, mt_compute_ao)
EXTERN_CVAR(Bool, mt_compute_ao_intel)
EXTERN_CVAR(Bool, mt_compute_bloom)
EXTERN_CVAR(Bool, mt_compute_bloom_intel)
EXTERN_CVAR(Int, mt_compute_bloom_composite)
EXTERN_CVAR(Int, mt_compute_ao_worldpos_debug)
EXTERN_CVAR(Int, mt_compute_ao_algorithm)
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
  // Last frame's probe blit has completed by now, so the readback is free.
  ReportHdrProbe();

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

  TraceFrameInterval(frameTimeMs);

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

// One frame's wall-clock interval. Accumulates until mt_frametrace seconds have
// passed, then reports the distribution and starts a fresh window -- so a long
// session reads as a series of independent windows rather than an average that
// slowly buries a bad patch.
void MtDebugManager::TraceFrameInterval(float frameTimeMs) {
  const int period = mt_frametrace;
  if (period <= 0) {
    if (!mTraceSamples.empty())
      mTraceSamples.clear();
    mTraceStarted = false;
    return;
  }

  using clock = std::chrono::steady_clock;
  const auto now = clock::now();

  if (!mTraceStarted) {
    mTraceStarted = true;
    mTraceWindowStart = now;
    mTraceSamples.clear();
    fprintf(stderr,
            "mt_frametrace: reporting every %ds. p99 and the >100ms count are "
            "the hitching signal; avg is not.\n",
            period);
  }

  mTraceSamples.push_back(frameTimeMs);

  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - mTraceWindowStart)
          .count();
  if (elapsed < (long long)period * 1000 || mTraceSamples.size() < 2)
    return;

  std::vector<float> sorted = mTraceSamples;
  std::sort(sorted.begin(), sorted.end());
  auto pct = [&sorted](double p) {
    const size_t i = (size_t)(p * (double)(sorted.size() - 1) + 0.5);
    return sorted[i < sorted.size() ? i : sorted.size() - 1];
  };

  double total = 0.0;
  size_t over33 = 0, over100 = 0;
  for (float v : mTraceSamples) {
    total += v;
    if (v > 33.3f) ++over33;
    if (v > 100.0f) ++over100;
  }
  const double mean = total / (double)mTraceSamples.size();

  fprintf(stderr,
          "mt_frametrace  n=%zu  avg=%6.2fms (%5.1f fps)  p50=%6.2f  p95=%6.2f  "
          "p99=%6.2f  max=%7.2f  >33ms=%zu (%.1f%%)  >100ms=%zu\n",
          mTraceSamples.size(), mean, mean > 0.0 ? 1000.0 / mean : 0.0,
          pct(0.50), pct(0.95), pct(0.99), sorted.back(), over33,
          100.0 * (double)over33 / (double)mTraceSamples.size(), over100);

  mTraceSamples.clear();
  mTraceWindowStart = now;
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
  // The scene viewport is the coordinate frame every postprocess and compute
  // pass works in, and it is NOT the window size (screenblocks, letterboxing
  // and scaling all move it). Printing it makes a capture's reflection axis
  // -- top + height/2 -- derivable from geometry instead of fitted to the
  // measurement, which is the difference between a prediction and a circular
  // check. See the bloom V-flip predictions in AGENTS.md.
  Printf(PRINT_HIGH, "  Scene viewport:          %d,%d %dx%d  (centre y = %.1f)\n",
         screen->mSceneViewport.left, screen->mSceneViewport.top,
         screen->mSceneViewport.width, screen->mSceneViewport.height,
         screen->mSceneViewport.top + screen->mSceneViewport.height * 0.5f);
  Printf(PRINT_HIGH, "  Screen viewport:         %d,%d %dx%d\n",
         screen->mScreenViewport.left, screen->mScreenViewport.top,
         screen->mScreenViewport.width, screen->mScreenViewport.height);
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

  // Effective postprocess configuration. MOST of these are CVAR_ARCHIVE, so a
  // value set during one debugging session silently persists into the next --
  // which has now invalidated three separate in-game A/B tests
  // (mt_compute_ao_worldpos_debug left on, AO directions/steps left at
  // experiment values, and mt_compute_bloom left off, which made the bloom
  // composite modes inert because the compute path never ran at all).
  // Printing the resolved path removes the guesswork.
  //
  // Two are NOT archived (declared with flags 0) and reset to their defaults
  // on restart: mt_compute_bloom_composite (mt_postprocess.cpp) and
  // gl_ssao_debug (hw_postprocess_cvars.cpp). This is the opposite trap and
  // just as costly -- set one, restart to pick up an unrelated change, and
  // the A/B silently reverts to the default path. They are tagged
  // "not archived" below; do not assume a restart preserved them.
  {
    // Same reasoning as the AO gate below: on Intel the cvar can read "on"
    // while mt_postprocess.cpp's architecture gate routes the frame to the
    // reference PP bloom, so report the RESOLVED path, not the cvar.
    const bool bloomIntelGated = mt_compute_bloom && !mt_compute_bloom_intel &&
                                 v.architecture == MtGPUArchitecture::Intel;
    const bool computeBloomOn =
        mt_compute_bloom && !bloomIntelGated && fb->mBloomModule != nullptr;
    const bool tier2 = v.supportsReadWriteBGRA8;
    Printf(PRINT_HIGH, "Effective postprocess configuration:\n");
    Printf(PRINT_HIGH, "  gl_bloom:                %s\n", gl_bloom ? "on" : "OFF (no bloom at all)");
    Printf(PRINT_HIGH, "  mt_compute_bloom:        %s\n", mt_compute_bloom ? "on" : "OFF");
    Printf(PRINT_HIGH, "  bloom path in use:       %s\n",
           !mt_compute_bloom ? "hw_postprocess bloom -- compute bloom off"
           : bloomIntelGated ? "hw_postprocess bloom <- Intel gate, compute bloom "
                               "overridden (set mt_compute_bloom_intel 1 to force compute)"
                             : "Metal compute (MtBloomModule)");
    if (computeBloomOn) {
      const int mode = mt_compute_bloom_composite;
      const char *path = (mode == 2 && !tier2) ? "hw_postprocess fallback (Tier 2 required but unsupported)"
                       : (tier2 && mode != 1)  ? "Tier 2 direct read-write composite"
                                               : "Tier 1 compute + raster composite";
      Printf(PRINT_HIGH, "  bloom composite path:    %s\n", path);
      Printf(PRINT_HIGH, "    (mt_compute_bloom_composite = %d, not archived -- resets on restart)\n",
             mode);
    }
    // Print the RESOLVED AO path, not just the cvar. On Intel the cvar can
    // read "on" while mt_postprocess.cpp's architecture gate silently routes
    // the frame to the reference PP path -- so the cvar alone tells you
    // nothing about which code actually ran.
    const bool intelGated = mt_compute_ao && !mt_compute_ao_intel &&
                            v.architecture == MtGPUArchitecture::Intel;
    Printf(PRINT_HIGH, "  mt_compute_ao:           %s\n", mt_compute_ao ? "on" : "OFF");
    Printf(PRINT_HIGH, "  AO path in use:          %s\n",
           !mt_compute_ao   ? "reference PP (hw_postprocess.ssao) -- compute AO off"
           : intelGated     ? "reference PP (hw_postprocess.ssao) <- Intel gate, "
                              "compute AO overridden (set mt_compute_ao_intel 1 to force compute)"
                            : "Metal compute (MtAOModule)");
    // The ALGORITHM is archived and was missing from this dump, which is exactly
    // the trap this whole block exists to close: a compute-vs-reference AO
    // measurement was nearly published against algorithm 0 while the recorded
    // claim being checked was about algorithm 1. The resolved cost differs by
    // ~10% between them.
    if (mt_compute_ao) {
      const int alg = clamp((int)mt_compute_ao_algorithm, 0, 2);
      Printf(PRINT_HIGH, "  AO algorithm:            %d (%s)   <- ARCHIVED, persists across runs\n",
             alg,
             alg == 0 ? "GTAO horizon search"
             : alg == 1 ? "AlchemyAO/SAO"
                        : "depth-mip-pyramid");
    }
    Printf(PRINT_HIGH, "  gl_ssao / debug:         %d / %d%s\n", (int)gl_ssao, (int)gl_ssao_debug,
           gl_ssao_debug != 0 ? "  <- debug view active, not the shaded result (not archived)" : "");
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


//===========================================================================
//
// mt_hdr_probe -- does the scene shader actually emit above 1.0?
//
// The mt_hdr_pipeline case rests on ProcessMaterialLight clamping to 1.4
// rather than 1.0 (material_normal.fp), making 1.4 the designed headroom and
// 1.0 the bloom extract threshold. "The shader can emit 1.4" is not "this
// scene does", and screenshots cannot close that gap: the swapchain is 8-bit
// whatever the pipeline format is.
//
// Capture is deferred by one frame rather than done inside the CCMD. A CCMD
// runs between frames, when SceneColor holds a finished frame *including the
// 2D pass* -- MetalRenderDevice::SetActiveRenderTarget points 2D at
// SceneColor. The first version of this probe therefore reported the HUD's
// white pixels, which sit at exactly 1.0, and not scene lighting at all.
//
//===========================================================================

void MtDebugManager::ArmHdrProbe(int frames) {
  mHdrProbeFramesLeft = std::max(frames, 1);
  mHdrProbeFramesDone = 0;
  mHdrProbeRunMax = 0.0f;
  mHdrProbeRunMaxFrame = 0;
  mHdrProbeRunOver1 = 0;
  mHdrProbeRunOver1_2 = 0;
  mHdrProbeRunMeanSum = 0.0;
  mHdrProbeArmed = true;
}

void MtDebugManager::CaptureHdrProbe() {
  if (!mHdrProbeArmed || mHdrProbePending)
    return;
  mHdrProbeArmed = false;

  if (!fb || !fb->GetBuffers() || !fb->GetBuffers()->SceneColor)
    return;
  MTL::Texture *tex = fb->GetBuffers()->SceneColor->GetTexture();
  if (!tex)
    return;

  const bool halfFloat = tex->pixelFormat() == MTL::PixelFormatRGBA16Float;
  if (!halfFloat && tex->pixelFormat() != MTL::PixelFormatBGRA8Unorm) {
    Printf(PRINT_HIGH, "mt_hdr_probe: unexpected scene colour format %d.\n",
           (int)tex->pixelFormat());
    return;
  }
  if (tex->sampleCount() > 1) {
    Printf(PRINT_HIGH, "mt_hdr_probe: multisampled scene colour is not supported.\n");
    return;
  }

  const int w = (int)tex->width();
  const int h = (int)tex->height();
  const size_t bytesPerPixel = halfFloat ? 8 : 4;
  const size_t bytesPerRow = (size_t)w * bytesPerPixel;
  const size_t dataSize = bytesPerRow * (size_t)h;

  if (mHdrProbeBuffer) {
    mHdrProbeBuffer->release();
    mHdrProbeBuffer = nullptr;
  }
  mHdrProbeBuffer =
      fb->device->device->newBuffer(dataSize, MTL::ResourceStorageModeShared);
  if (!mHdrProbeBuffer) {
    Printf(PRINT_HIGH, "mt_hdr_probe: could not allocate a %zu byte staging buffer.\n",
           dataSize);
    return;
  }

  auto cmdBuf = fb->GetCommands()->GetBlitCommandBuffer();
  if (!cmdBuf) {
    mHdrProbeBuffer->release();
    mHdrProbeBuffer = nullptr;
    Printf(PRINT_HIGH, "mt_hdr_probe: no blit command buffer.\n");
    return;
  }
  auto blit = cmdBuf->blitCommandEncoder();
  blit->copyFromTexture(tex, 0, 0, MTL::Origin(0, 0, 0), MTL::Size(w, h, 1),
                        mHdrProbeBuffer, 0, bytesPerRow, dataSize);
  blit->endEncoding();
  // Committed but deliberately not waited on -- ReportHdrProbe picks it up at
  // the start of the next frame, by which point it has long since completed.
  cmdBuf->commit();
  mHdrProbeCmd = cmdBuf;

  mHdrProbeW = w;
  mHdrProbeH = h;
  mHdrProbeHalfFloat = halfFloat;
  mHdrProbePending = true;
}

void MtDebugManager::ReportHdrProbe() {
  if (!mHdrProbePending)
    return;
  mHdrProbePending = false;

  if (mHdrProbeCmd) {
    mHdrProbeCmd->waitUntilCompleted();
    mHdrProbeCmd->release();
    mHdrProbeCmd = nullptr;
  }
  if (!mHdrProbeBuffer)
    return;

  const size_t pixels = (size_t)mHdrProbeW * mHdrProbeH;
  float maxChannel = 0.0f;
  double sum = 0.0;
  size_t over1 = 0, over1_05 = 0, over1_2 = 0, pinned = 0;

  if (mHdrProbeHalfFloat) {
    const __fp16 *px = (const __fp16 *)mHdrProbeBuffer->contents();
    for (size_t i = 0; i < pixels; i++) {
      float r = (float)px[i * 4 + 0];
      float g = (float)px[i * 4 + 1];
      float b = (float)px[i * 4 + 2];
      sum += r + g + b;
      float m = std::max({r, g, b});
      if (m > maxChannel) maxChannel = m;
      if (m > 1.0f)  over1++;
      if (m > 1.05f) over1_05++;
      if (m > 1.2f)  over1_2++;
    }
  } else {
    const uint8_t *px = (const uint8_t *)mHdrProbeBuffer->contents();
    for (size_t i = 0; i < pixels; i++) {
      uint8_t m = 0;
      for (int c = 0; c < 3; c++) {
        uint8_t v = px[i * 4 + c];
        sum += v / 255.0;
        if (v > m) m = v;
      }
      if (m / 255.0f > maxChannel) maxChannel = m / 255.0f;
      if (m == 255) pinned++;
    }
  }

  mHdrProbeBuffer->release();
  mHdrProbeBuffer = nullptr;

  // Accumulate; a single frame in a dark map cannot answer the question, so
  // the interesting statistic is the peak over a window that can contain a
  // muzzle flash or an explosion.
  mHdrProbeFramesDone++;
  if (maxChannel > mHdrProbeRunMax) {
    mHdrProbeRunMax = maxChannel;
    mHdrProbeRunMaxFrame = mHdrProbeFramesDone;
  }
  mHdrProbeRunOver1 = std::max(mHdrProbeRunOver1, over1);
  mHdrProbeRunOver1_2 = std::max(mHdrProbeRunOver1_2, over1_2);
  mHdrProbeRunMeanSum += sum / (pixels * 3);

  if (mHdrProbeFramesLeft > 1) {
    mHdrProbeFramesLeft--;
    mHdrProbeArmed = true; // sample again next frame
    return;
  }
  mHdrProbeFramesLeft = 0;

  maxChannel = mHdrProbeRunMax;
  over1 = mHdrProbeRunOver1;
  over1_2 = mHdrProbeRunOver1_2;

  Printf(PRINT_HIGH, "Metal scene colour probe (%dx%d, %s, pre-postprocess, no HUD):\n",
         mHdrProbeW, mHdrProbeH, mHdrProbeHalfFloat ? "RGBA16Float" : "BGRA8Unorm");
  Printf(PRINT_HIGH, "  frames sampled:     %d\n", mHdrProbeFramesDone);
  Printf(PRINT_HIGH, "  peak channel:       %.4f (frame %d of %d)\n",
         maxChannel, mHdrProbeRunMaxFrame, mHdrProbeFramesDone);
  Printf(PRINT_HIGH, "  mean channel:       %.4f (averaged over the window)\n",
         (float)(mHdrProbeRunMeanSum / mHdrProbeFramesDone));
  if (mHdrProbeHalfFloat) {
    Printf(PRINT_HIGH, "  peak pixels > 1.00: %zu (%.4f%%)\n", over1, 100.0 * over1 / pixels);
    Printf(PRINT_HIGH, "  peak pixels > 1.20: %zu (%.4f%%)\n", over1_2, 100.0 * over1_2 / pixels);
    if (maxChannel <= 1.0f) {
      Printf(PRINT_HIGH, "  -> Nothing exceeded 1.0 across the window. The half-float buffer\n");
      Printf(PRINT_HIGH, "     buys precision only here, and compute bloom cannot fire at\n");
      Printf(PRINT_HIGH, "     exposureAdjustment <= 1 regardless of format.\n");
      Printf(PRINT_HIGH, "     Note that brightmaps and fullbright sprites are clamped to\n");
      Printf(PRINT_HIGH, "     exactly 1.0 by main.fp:745 in the reference, so a peak of\n");
      Printf(PRINT_HIGH, "     1.0000 means a lamp or sprite, not headroom in use.\n");
    } else {
      Printf(PRINT_HIGH, "  -> Headroom is in use. BGRA8 clips this; tonemapping has something\n");
      Printf(PRINT_HIGH, "     to roll off and bloom has something to extract.\n");
    }
    Printf(PRINT_HIGH, "  The only path above 1.0 is Base.rgb * clamp(color + dynlight, 0, 1.4),\n");
    Printf(PRINT_HIGH, "  so it needs a strong dynamic light on a pale surface at close range --\n");
    Printf(PRINT_HIGH, "  a rocket blast or muzzle flash against a near wall. Arm a window and\n");
    Printf(PRINT_HIGH, "  fire: mt_hdr_probe 120\n");
  } else {
    Printf(PRINT_HIGH, "  pixels pinned at 255: %zu (%.4f%%)\n", pinned, 100.0 * pinned / pixels);
    Printf(PRINT_HIGH, "  -> BGRA8 clamps at 1.0 by construction and cannot show headroom.\n");
    Printf(PRINT_HIGH, "     The pinned figure is what a half-float buffer would recover.\n");
    Printf(PRINT_HIGH, "     Set mt_hdr_pipeline 1 and probe again to compare.\n");
  }
}

CCMD(mt_hdr_probe)
{
  auto debug = GetActiveMetalDebugManager();
  if (!debug) {
    Printf(PRINT_HIGH, "Metal diagnostics are not available.\n");
    return;
  }
  int frames = argv.argc() > 1 ? atoi(argv[1]) : 1;
  frames = clamp(frames, 1, 600);
  debug->ArmHdrProbe(frames);
  if (frames == 1)
    Printf(PRINT_HIGH, "mt_hdr_probe armed; sampling the next rendered frame.\n");
  else
    Printf(PRINT_HIGH, "mt_hdr_probe armed for %d frames; reporting the peak. Go make some light.\n",
           frames);
}
