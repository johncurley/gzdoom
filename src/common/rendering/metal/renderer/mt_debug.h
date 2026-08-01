#pragma once

#include "mt_metrics.h"

#include <atomic>
#include <chrono>
#include <vector>

class MetalRenderDevice;

namespace MTL {
class Buffer;
class CommandBuffer;
} // namespace MTL

// Debug metrics and diagnostics for Metal renderer
class MtDebugManager {
public:
  MtDebugManager(MetalRenderDevice *fb);
  ~MtDebugManager();

  // Frame lifecycle
  void BeginFrame();
  void EndFrame();

  // Scene colour HDR probe (mt_hdr_probe). Answers whether the scene shader
  // actually emits above 1.0, which no screenshot can show because the
  // swapchain is 8-bit regardless of the pipeline format.
  //
  // Deliberately split across two frames rather than read back inside the
  // CCMD. A CCMD runs between frames, when SceneColor holds a *finished*
  // frame -- including the 2D pass, which MetalRenderDevice::
  // SetActiveRenderTarget points at SceneColor. That made the first version
  // of this probe report the HUD's white pixels (exactly 1.0) rather than
  // scene lighting. Capture instead happens at PostProcessScene entry, which
  // is after the scene draw and before hw_postprocess.Pass1 moves SceneColor
  // into the pipeline images, so it sees raw scene output and no HUD.
  void ArmHdrProbe();
  void CaptureHdrProbe();   // PostProcessScene entry; GPU blit only, no stall
  void ReportHdrProbe();    // next BeginFrame; readback is already complete

  // Metric collection
  void RecordDrawCallCategory(int vertexCount, int indexCount, const char *category);
  void RecordStateChange(const char *stateName);
  void RecordTextureAllocation(size_t bytes, const char *name);
  void RecordBufferAllocation(size_t bytes, const char *name);
  void RecordTextureMipmap(const char *name, int levels);
  void RecordMetric(MtMetric metric, float durationMs);
  void RecordAOTiming(bool computePath, float durationMs);
  void RecordBloomTiming(float durationMs);

  // Real GPU execution time for a completed frame's command buffer, via
  // MTLCommandBuffer::GPUStartTime()/GPUEndTime(). Thread-safe entry point:
  // called from Metal's internal completion-handler thread, NOT the render
  // thread, so it only does an atomic store -- EndFrame() (render thread)
  // drains it into the regular metric history.
  void RecordGPUFrameTimeAsync(float durationMs);

  // Stall tracking — call from render thread when a synchronous GPU wait occurs
  // type: "drawable", "semaphore", "streambuffer"
  void RecordStall(const char *type, float durationMs);

  // CPU-side cost (staging memcpy + blit encode + commit) of the "world
  // texture" upload path in mt_texture.cpp -- not a blocking stall (the
  // upload's command buffer commits async, no waitUntilCompleted), but a
  // real render-thread CPU cost worth tracking separately from stalls.
  void RecordTextureUpload(float durationMs);

  // Centralized count of every MtCommandBufferManager::GetBlitCommandBuffer()
  // call this frame (world texture uploads, lightmap uploads, mipmap
  // regeneration) -- each is a separate command buffer submitted to the GPU
  // queue outside the main per-frame render command buffer. A burst here
  // (e.g. during a level transition) is a plausible submission-overhead
  // cost on GPUs with weaker per-submission throughput (see Intel compute
  // AO findings above for the same class of cost in a different subsystem).
  void RecordExtraCommandBuffer();

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

    // Texture upload tracking (see RecordTextureUpload/RecordExtraCommandBuffer)
    int textureUploads = 0;       // world-texture content uploads, CPU-timed
    int extraCommandBuffers = 0;  // all GetBlitCommandBuffer() calls this frame

    // CPU encode/composite timing for comparing native compute modules with
    // engine postprocess reference paths.
    MtMetricFrame metrics;

    float GetMetric(MtMetric metric) const {
      return metrics.Get(metric);
    }
  };

  FrameStats GetLastFrameStats() const { return mLastFrameStats; }
  MtMetricStats GetFrameTimeStats() const;
  MtMetricStats GetMetricStats(MtMetric metric) const {
    return mMetricHistory.GetStats(metric);
  }
  void ClearMetricHistory() { mMetricHistory.Clear(); }
  void ClearBenchmarkHistory() {
    mMetricHistory.Clear();
    mFrameTimeHistory.clear();
  }

  // Architecture info
  void PrintArchitectureInfo();

private:
  MetalRenderDevice *fb = nullptr;

  // mt_hdr_probe state; see the ArmHdrProbe/CaptureHdrProbe pair above.
  bool mHdrProbeArmed = false;
  bool mHdrProbePending = false;
  MTL::Buffer *mHdrProbeBuffer = nullptr;
  MTL::CommandBuffer *mHdrProbeCmd = nullptr;
  int mHdrProbeW = 0;
  int mHdrProbeH = 0;
  bool mHdrProbeHalfFloat = false;

  // Metrics for current frame
  FrameStats mCurrentFrameStats = {};
  FrameStats mLastFrameStats = {};
  MtMetricHistory mMetricHistory;

  // Timing
  std::chrono::high_resolution_clock::time_point mFrameStartTime;
  std::vector<float> mFrameTimeHistory;  // Rolling window (120 frames)
  int mFrameIndex = 0;

  // Cross-thread handoff for async GPU frame timing: written (relaxed
  // store) from a Metal completion-handler thread, drained (exchange) once
  // per frame on the render thread in EndFrame(). -1.0f means "no new value
  // since last drain".
  std::atomic<float> mPendingGPUFrameTimeMs{-1.0f};

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
