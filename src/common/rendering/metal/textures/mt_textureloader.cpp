#include "mt_textureloader.h"
#include "c_cvars.h"
#include "printf.h"

CVAR(Bool, mt_async_png, true, 0)
static constexpr size_t PngBudget = 16 * 1024 * 1024;

MtTextureLoader::MtTextureLoader(MetalRenderDevice *fb)
    : fb(fb) {
  // Create a background queue for texture processing
  mTextureQueue = dispatch_queue_create("com.gzdoom.metal.textureloader",
                                        DISPATCH_QUEUE_CONCURRENT);
  mPngQueue = dispatch_queue_create("com.gzdoom.metal.pngdecode",
                                    DISPATCH_QUEUE_SERIAL);
  mLoadGroup = dispatch_group_create();
}

MtTextureLoader::~MtTextureLoader() {
  Shutdown();
  dispatch_release(mPngQueue);
  if (mTextureQueue) {
    dispatch_release(mTextureQueue);
  }
  if (mLoadGroup) {
    dispatch_release(mLoadGroup);
  }
}

uint32_t MtTextureLoader::QueueTextureLoad(FTexture *tex, int translation,
                                          int flags, bool &queuedPng) {
  queuedPng = false;
  if (!tex) return 0;

  flags |= CTF_ProcessData;
  TextureLoadTask initialTask;
  if (mt_async_png) {
    if (auto image = dynamic_cast<FImageTexture *>(tex)) {
      initialTask.pngBytes = image->ReadPngSnapshot(
          translation, flags, PngBudget - mPngBytes, initialTask.sourcePixels,
          initialTask.pngIDAT, initialTask.pngChannels);
    }
  }
  queuedPng = initialTask.pngBytes != 0;
  if (queuedPng) {
    mPngBytes += initialTask.pngBytes;
    mPngPeakBytes = std::max(mPngPeakBytes, mPngBytes);
    ++mPngQueued;
  } else if (!tex->CreatePixelSnapshot(translation, flags, initialTask.sourcePixels)) {
    return 0;
  }

  uint32_t taskId = mNextTaskId++;
  mPendingCount++;

  auto task = std::make_shared<TextureLoadTask>(std::move(initialTask));
  task->taskId = taskId;

  // The source pixels are now owned by this task. The worker never needs the
  // FTexture object or its image/cache state.
  dispatch_group_async(mLoadGroup, queuedPng ? mPngQueue : mTextureQueue, ^{
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

void MtTextureLoader::ReleaseCompletedTasks(std::vector<TextureLoadTask> &tasks) {
  size_t released = 0;
  for (const auto &task : tasks)
    released += task.pngBytes;
  tasks.clear();
  mPngBytes -= released;
}

void MtTextureLoader::RecordPngCompletion(bool uploaded) {
  mPngUploaded += uploaded;
  mPngFailed += !uploaded;
}

void MtTextureLoader::PrintPngStats() const {
  Printf("Metal PNG precache: queued=%u uploaded=%u failed=%u first-use-ready=%u "
         "first-use-fallback=%u reserved=%zu peak=%zu budget=%zu bytes\n",
         mPngQueued, mPngUploaded, mPngFailed, mPngReady, mPngFirstUseFallback,
         mPngBytes, mPngPeakBytes, PngBudget);
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
  auto &snapshot = task.sourcePixels;
  if (task.pngBytes) {
    const bool decoded = FImageTexture::DecodePngSnapshot(
        task.pngIDAT, task.pngChannels, snapshot);
    task.pngIDAT = std::vector<uint8_t>();
    if (!decoded) return;
  }
  task.completed = FTexture::ProcessPixelSnapshot(snapshot, task.result);
  task.sourcePixels = {};
}
