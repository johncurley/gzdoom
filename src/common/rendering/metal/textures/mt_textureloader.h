#pragma once

#include <vector>
#include <queue>
#include <memory>
#include <mutex>
#include <dispatch/dispatch.h>
#include "textures.h"

class MetalRenderDevice;

/**
 * Async texture loader for Metal renderer using Grand Central Dispatch
 * 
 * Loads texture data on background GCD queues, uploads to GPU on the render thread.
 * Metal requires GPU operations on the main thread, so we:
 * 1. Capture pixels or eligible PNG input on the render thread
 * 2. Decode PNG input and process the owned snapshot on background GCD queues
 * 3. Upload to GPU on the render thread only
 */
struct TextureLoadTask {
  FTexturePixelSnapshot sourcePixels;
  std::vector<uint8_t> pngIDAT;
  size_t pngBytes = 0;
  int pngChannels = 0;
  FTexturePixelResult result;
  uint32_t taskId = 0;
  bool completed = false;
};

class MtTextureLoader {
public:
  MtTextureLoader(MetalRenderDevice *fb);
  ~MtTextureLoader();

  // Queue a texture for async loading
  uint32_t QueueTextureLoad(FTexture *tex, int translation, int flags,
                            bool &queuedPng);
  void ReleaseCompletedTasks(std::vector<TextureLoadTask> &tasks);
  void PrintPngStats() const;
  void RecordPngFirstUseFallback() { ++mPngFirstUseFallback; }
  void RecordPngReady() { ++mPngReady; }
  void RecordPngCompletion(bool uploaded);

  // Get all completed tasks
  std::vector<TextureLoadTask> GetCompletedTasks();

  // Process pending completions (call from render thread)
  void ProcessCompletions();

  // Shutdown workers
  void Shutdown();

  // Debug: get queue size
  size_t GetQueueSize() const;
  size_t GetCompletedCount() const;

private:
  MetalRenderDevice *fb;
  dispatch_queue_t mTextureQueue;  // Background queue for texture processing
  dispatch_queue_t mPngQueue;
  dispatch_group_t mLoadGroup;     // Group to track pending loads
  mutable std::mutex mCompletedMutex;
  std::vector<TextureLoadTask> mCompletedTasks;
  std::atomic<uint32_t> mNextTaskId = {1};
  mutable std::atomic<size_t> mPendingCount = {0};

  // Accessed only by the render thread. Reservations include completed
  // jobs until their results have been uploaded or discarded.
  size_t mPngBytes = 0, mPngPeakBytes = 0;
  unsigned mPngQueued = 0, mPngUploaded = 0, mPngFailed = 0;
  unsigned mPngReady = 0, mPngFirstUseFallback = 0;

  void ProcessTextureTask(TextureLoadTask &task);
};
