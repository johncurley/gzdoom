#include "mt_textureloader.h"
#include "filesystem.h"
#include "image.h"
#include "hw_material.h"
#include "textures.h"

MtTextureLoader::MtTextureLoader(MetalRenderDevice *fb)
    : fb(fb) {
  // Create a background queue for texture processing
  mTextureQueue = dispatch_queue_create("com.gzdoom.metal.textureloader",
                                        DISPATCH_QUEUE_CONCURRENT);
  mLoadGroup = dispatch_group_create();
}

MtTextureLoader::~MtTextureLoader() {
  Shutdown();
  if (mTextureQueue) {
    dispatch_release(mTextureQueue);
  }
  if (mLoadGroup) {
    dispatch_release(mLoadGroup);
  }
}

uint32_t MtTextureLoader::QueueTextureLoad(FTexture *tex, int translation,
                                          int flags) {
  if (!tex) return 0;

  TextureLoadTask initialTask;
  if (!tex->CreatePixelSnapshot(translation, flags | CTF_ProcessData,
                                initialTask.sourcePixels))
    return 0;

  uint32_t taskId = mNextTaskId++;
  mPendingCount++;

  auto task = std::make_shared<TextureLoadTask>(std::move(initialTask));
  task->taskId = taskId;

  // The source pixels are now owned by this task. The worker never needs the
  // FTexture object or its image/cache state.
  dispatch_group_async(mLoadGroup, mTextureQueue, ^{
    ProcessTextureTask(*task);

    // Do not dispatch completion to the main queue: the Cocoa entry point
    // runs DoMain() inside a long-lived main-queue block, so such callbacks
    // cannot execute until the game exits. The render thread drains this
    // thread-safe queue at BeginFrame().
    std::lock_guard<std::mutex> lock(mCompletedMutex);
    mCompletedTasks.push_back(std::move(*task));
  });

  return taskId;
}

bool MtTextureLoader::TryGetCompletedTask(uint32_t taskId,
                                         TextureLoadTask &outTask) {
  std::lock_guard<std::mutex> lock(mCompletedMutex);
  for (size_t i = 0; i < mCompletedTasks.size(); ++i) {
    if (mCompletedTasks[i].taskId == taskId) {
      outTask = std::move(mCompletedTasks[i]);
      mCompletedTasks.erase(mCompletedTasks.begin() + i);
      mPendingCount--;
      return true;
    }
  }
  return false;
}

std::vector<TextureLoadTask> MtTextureLoader::GetCompletedTasks() {
  std::vector<TextureLoadTask> result;
  {
    std::lock_guard<std::mutex> lock(mCompletedMutex);
    result.swap(mCompletedTasks);
  }
  mPendingCount -= result.size();
  return result;
}

void MtTextureLoader::ProcessCompletions() {
  // Compatibility no-op: completed tasks are collected by GetCompletedTasks()
  // on the render thread after workers append them under mCompletedMutex.
}

void MtTextureLoader::Shutdown() {
  if (mLoadGroup) {
    // Wait for all pending tasks to complete
    dispatch_group_wait(mLoadGroup, DISPATCH_TIME_FOREVER);
  }
}

size_t MtTextureLoader::GetQueueSize() const {
  return mPendingCount.load();
}

size_t MtTextureLoader::GetCompletedCount() const {
  std::lock_guard<std::mutex> lock(mCompletedMutex);
  return mCompletedTasks.size();
}

void MtTextureLoader::ProcessTextureTask(TextureLoadTask &task) {
  // CPU-intensive conversion runs on GCD, using only task-owned values. Any
  // FTexture metadata and ImageArena work is returned to the render thread.
  task.completed = FTexture::ProcessPixelSnapshot(task.sourcePixels, task.result);
  task.sourcePixels = {};
}
