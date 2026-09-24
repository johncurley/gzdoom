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
 * 1. Capture source pixels on the render thread
 * 2. Process the owned snapshot on a background GCD queue
 * 3. Upload to GPU on the render thread only
 */
struct TextureLoadTask {
  FTexturePixelSnapshot sourcePixels;
  FTexturePixelResult result;
  uint32_t taskId = 0;
  bool completed = false;
};

class MtTextureLoader {
public:
  MtTextureLoader(MetalRenderDevice *fb);
  ~MtTextureLoader();

  // Queue a texture for async loading
  uint32_t QueueTextureLoad(FTexture *tex, int translation, int flags);

  // Check if a task is complete and retrieve it
  bool TryGetCompletedTask(uint32_t taskId, TextureLoadTask &outTask);

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
  dispatch_group_t mLoadGroup;     // Group to track pending loads
  mutable std::mutex mCompletedMutex;
  std::vector<TextureLoadTask> mCompletedTasks;
  std::atomic<uint32_t> mNextTaskId = {1};
  mutable std::atomic<size_t> mPendingCount = {0};

  void ProcessTextureTask(TextureLoadTask &task);
};
