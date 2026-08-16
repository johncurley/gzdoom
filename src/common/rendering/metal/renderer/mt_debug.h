#pragma once

#include "mt_metrics.h"

#include <atomic>
#include <chrono>
#include <map>
#include <string>
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
  void ArmHdrProbe(int frames);
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

  // Stall tracking — call from the render thread whenever the frame is held up
  // by synchronous work, whether that is a GPU wait ("drawable", "semaphore",
  // "streambuffer") or a compilation ("msl_translate", "msl_tolib",
  // "pso_compile", "pp_pso", "compute_pso").
  //
  // `type` must be a stable short label; `detail` is optional and names the
  // specific thing compiled or waited on (a shader name, a pipeline key).
  // Both are copied only when a trace is actually being kept.
  //
  // Attribution is the point: a >100ms frame from mt_frametrace says a stall
  // happened, not what it was. Every cold path that can run inside a frame
  // reports here, so a hitch during play can be pinned on shader translation,
  // Metal library compilation, PSO creation or a GPU wait — which are four
  // different fixes.
  void RecordStall(const char *type, float durationMs,
                   const char *detail = nullptr);

  // Per-type session totals for mt_stalltrace. `inFrame` counts stalls that
  // landed between BeginFrame and EndFrame, so they are inside the interval
  // mt_frametrace measures.
  //
  // A "between" stall is NOT free: it still holds the render thread and still
  // delays the next frame — measured 2026-08-17, the startup precompile batch
  // reports entirely as "between" while the same window's frametrace shows
  // p95=73.87ms. Read the flag as "is this inside the number frametrace
  // reports", not as "does the player feel this".
  struct StallTotals {
    int count = 0;
    float totalMs = 0.0f;
    float maxMs = 0.0f;
    int inFrameCount = 0;
    float inFrameTotalMs = 0.0f;
    int frameOfMax = -1;
    std::string worstDetail;
  };

  // Writes the per-type table to stderr. Called automatically at each
  // mt_frametrace window boundary (so the two instruments line up) and by the
  // mt_stalls CCMD.
  void PrintStallSummary(const char *reason);
  void ResetStallSummary();

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
  // Per-frame hook for mt_frametrace. No-op unless the cvar is set.
  void TraceFrameInterval(float frameTimeMs);
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
  int mHdrProbeFramesLeft = 0;
  int mHdrProbeFramesDone = 0;
  float mHdrProbeRunMax = 0.0f;
  int mHdrProbeRunMaxFrame = 0;
  size_t mHdrProbeRunOver1 = 0;
  size_t mHdrProbeRunOver1_2 = 0;
  double mHdrProbeRunMeanSum = 0.0;
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

  // mt_frametrace: an UNBOUNDED window per report period, deliberately. The
  // 120-frame ring above is a display average; hitching needs every frame in
  // the window or the percentiles are computed over a sample that has already
  // discarded the bad ones.
  std::vector<float> mTraceSamples;
  std::chrono::steady_clock::time_point mTraceWindowStart;
  bool mTraceStarted = false;
  int mFrameIndex = 0;

  // Cross-thread handoff for async GPU frame timing: written (relaxed
  // store) from a Metal completion-handler thread, drained (exchange) once
  // per frame on the render thread in EndFrame(). -1.0f means "no new value
  // since last drain".
  std::atomic<float> mPendingGPUFrameTimeMs{-1.0f};

  // mt_stalltrace: per-type session totals, and whether a frame is currently
  // open. std::map rather than unordered_map so the summary prints in a
  // stable order without sorting it at report time.
  std::map<std::string, StallTotals> mStallTotals;
  bool mInFrame = false;

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
