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
#include "metal/system/mt_renderdevice.h"
#include "metal/system/mt_version.h"
#include "printf.h"
#include <algorithm>
#include <ctime>
#include <cstring>

EXTERN_CVAR(Bool, mt_debug)

MtDebugManager::MtDebugManager(MetalRenderDevice *renderDevice)
    : fb(renderDevice) {
  mFrameTimeHistory.reserve(120);
  Printf(PRINT_HIGH, "Metal Debug Manager initialized (enable with: mt_debug 1)\n");
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
  
  if (!category) {
    mCurrentFrameStats.other_draws++;
    return;
  }
  
  // Categorize by name patterns
  if (strstr(category, "geometry") || strstr(category, "wall") || strstr(category, "floor")) {
    mCurrentFrameStats.geometry_draws++;
  } else if (strstr(category, "ui") || strstr(category, "hud") || strstr(category, "text")) {
    mCurrentFrameStats.ui_draws++;
  } else if (strstr(category, "sky") || strstr(category, "portal")) {
    mCurrentFrameStats.sky_draws++;
  } else if (strstr(category, "light") || strstr(category, "shadow")) {
    mCurrentFrameStats.light_draws++;
  } else {
    mCurrentFrameStats.other_draws++;
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

void MtDebugManager::RecordStall(const char *type, float durationMs) {
  mCurrentFrameStats.stallCount++;
  mCurrentFrameStats.stallTotalMs += durationMs;
  if (durationMs > mCurrentFrameStats.stallMaxMs)
    mCurrentFrameStats.stallMaxMs = durationMs;

  // Always log stalls >= 1ms — these are the micro-hitches the user sees.
  if (durationMs >= 1.0f) {
    Printf(PRINT_HIGH,
           "Metal: GPU stall (%s) %.2fms\n", type, durationMs);
  }
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
            "TextureAllocs,BufferAllocs\n");
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

  fprintf(mLogFile, "%d,%.1f,%d,%.2f,%.1f,%d,%d,%d\n", mLogFrameCount, fps,
          mCurrentFrameStats.drawCallCount, mCurrentFrameStats.frameTimeMs,
          mCurrentFrameStats.gpuMemoryCurrent / (1024.0f * 1024.0f),
          mCurrentFrameStats.stateChanges,
          mCurrentFrameStats.textureAllocations,
          mCurrentFrameStats.bufferAllocations);

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
