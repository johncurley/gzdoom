#pragma once

#include <chrono>
#include <vector>
#include <cstdint>

class MetalRenderDevice;

// Debug metrics and diagnostics for Metal renderer
class MtDebugManager {
public:
  MtDebugManager(MetalRenderDevice *fb);
  ~MtDebugManager();

  // Frame lifecycle
  void BeginFrame();
  void EndFrame();

  // Metric collection
  void RecordDrawCallCategory(int vertexCount, int indexCount, const char *category);
  void RecordStateChange(const char *stateName);
  void RecordTextureAllocation(size_t bytes, const char *name);
  void RecordBufferAllocation(size_t bytes, const char *name);
  void RecordTextureMipmap(const char *name, int levels);

  // Stall tracking — call from render thread when a synchronous GPU wait occurs
  // type: "drawable", "semaphore", "streambuffer", "texture_upload"
  void RecordStall(const char *type, float durationMs);

  // Display (console-based)
  void PrintDebugStats();

  // Logging
  void StartLogging(const char *filepath = nullptr);
  void StopLogging();
  bool IsLogging() const { return mLogFile != nullptr; }

  // Statistics query
  struct FrameStats {
    float frameTimeMs;
    int drawCallCount;
    int indexCount;
    size_t gpuMemoryAllocated;
    size_t gpuMemoryCurrent;
    int stateChanges;
    int textureAllocations;
    int bufferAllocations;
    int textureMipmaps;
    
    // Draw call categories
    int geometry_draws = 0;
    int ui_draws = 0;
    int sky_draws = 0;
    int light_draws = 0;
    int other_draws = 0;

    // Stall tracking
    int stallCount = 0;
    float stallTotalMs = 0.0f;
    float stallMaxMs = 0.0f;
  };

  FrameStats GetLastFrameStats() const { return mLastFrameStats; }

  // Architecture info
  void PrintArchitectureInfo();

private:
  MetalRenderDevice *fb = nullptr;

  // Metrics for current frame
  FrameStats mCurrentFrameStats = {};
  FrameStats mLastFrameStats = {};

  // Timing
  std::chrono::high_resolution_clock::time_point mFrameStartTime;
  std::vector<float> mFrameTimeHistory;  // Rolling window (120 frames)
  int mFrameIndex = 0;

  // Memory tracking
  size_t mTotalAllocated = 0;

  // Logging
  FILE *mLogFile = nullptr;
  int mLogFrameCount = 0;
  std::chrono::high_resolution_clock::time_point mLoggingStartTime;

  // Helper methods
  float GetAverageFrameTime() const;
  void WriteLogEntry();
};
