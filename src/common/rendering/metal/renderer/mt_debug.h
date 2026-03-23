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
  void RecordDrawCall(int vertexCount, int indexCount);
  void RecordStateChange(const char *stateName);
  void RecordTextureAllocation(size_t bytes, const char *name);
  void RecordBufferAllocation(size_t bytes, const char *name);
  void RecordTextureMipmap(const char *name, int levels);

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
