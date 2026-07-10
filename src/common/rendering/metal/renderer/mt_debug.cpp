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
#include "metal/system/mt_renderdevice.h"
#include "metal/system/mt_version.h"
#include "printf.h"
#include "v_video.h"
#include <algorithm>
#include <ctime>
#include <cstring>

EXTERN_CVAR(Bool, mt_debug)

namespace {
MtDebugManager *GetActiveMetalDebugManager() {
  if (!screen || !screen->IsMetal())
    return nullptr;

  auto fb = static_cast<MetalRenderDevice *>(screen);
  return fb->GetDebugManager();
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

void MtDebugManager::RecordStall(const char *type, float durationMs) {
  mCurrentFrameStats.stallCount++;
  mCurrentFrameStats.stallTotalMs += durationMs;
  if (durationMs > mCurrentFrameStats.stallMaxMs)
    mCurrentFrameStats.stallMaxMs = durationMs;
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
            "Frame,FPS,DrawCalls,FrameTimeMS,GPUMemoryMB,StateChanges,"
            "TextureAllocs,BufferAllocs,ComputeAOCPUms,ComputeBloomCPUms,"
            "PPAOCPUms,PPBloomCPUms\n");
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

  fprintf(mLogFile, "%d,%.1f,%d,%.2f,%.1f,%d,%d,%d,%.3f,%.3f,%.3f,%.3f\n", mLogFrameCount, fps,
          mCurrentFrameStats.drawCallCount, mCurrentFrameStats.frameTimeMs,
          mCurrentFrameStats.gpuMemoryCurrent / (1024.0f * 1024.0f),
          mCurrentFrameStats.stateChanges,
          mCurrentFrameStats.textureAllocations,
          mCurrentFrameStats.bufferAllocations,
          mCurrentFrameStats.GetMetric(MtMetric::ComputeAO),
          mCurrentFrameStats.GetMetric(MtMetric::ComputeBloom),
          mCurrentFrameStats.GetMetric(MtMetric::PPAO),
          mCurrentFrameStats.GetMetric(MtMetric::PPBloom));

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
  PrintMetricSummary(debug, MtMetric::ComputeAO);
  PrintMetricSummary(debug, MtMetric::ComputeBloom);
  PrintMetricSummary(debug, MtMetric::PPAO);
  PrintMetricSummary(debug, MtMetric::PPBloom);
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
