#pragma once

#include <vector>
#include <queue>
#include <memory>
#include <dispatch/dispatch.h>

class FTexture;
class MetalRenderDevice;

/**
 * Async texture loader for Metal renderer using Grand Central Dispatch
 * 
 * Loads texture data on background GCD queues, uploads to GPU on main thread.
 * Metal requires GPU operations on the main thread, so we:
 * 1. Process texture data (CreateTexBuffer) on background GCD queues
 * 2. Upload to GPU (replaceRegion) on render thread only
 */
struct TextureLoadTask {
  FTexture *texSource = nullptr;
  int translation = 0;
  int flags = 0;
  std::vector<uint8_t> pixelData;
  int width = 0;
  int height = 0;
  bool indexed = false;
  int bytesPerPixel = 0;
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
  std::vector<TextureLoadTask> mCompletedTasks;
  std::atomic<uint32_t> mNextTaskId = {1};
  mutable std::atomic<size_t> mPendingCount = {0};

  void ProcessTextureTask(TextureLoadTask &task);
};
