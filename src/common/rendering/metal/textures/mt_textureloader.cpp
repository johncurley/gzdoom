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

  uint32_t taskId = mNextTaskId++;
  mPendingCount++;

  // Capture the task data for the block
  dispatch_group_async(mLoadGroup, mTextureQueue, ^{
    TextureLoadTask task;
    task.texSource = tex;
    task.translation = translation;
    task.flags = flags;
    task.taskId = taskId;
    task.completed = false;

    ProcessTextureTask(task);

    // Add to completed tasks on main thread via a completion handler
    dispatch_async(dispatch_get_main_queue(), ^{
      mCompletedTasks.push_back(task);
    });
  });

  return taskId;
}

bool MtTextureLoader::TryGetCompletedTask(uint32_t taskId,
                                         TextureLoadTask &outTask) {
  for (size_t i = 0; i < mCompletedTasks.size(); ++i) {
    if (mCompletedTasks[i].taskId == taskId) {
      outTask = mCompletedTasks[i];
      mCompletedTasks.erase(mCompletedTasks.begin() + i);
      mPendingCount--;
      return true;
    }
  }
  return false;
}

std::vector<TextureLoadTask> MtTextureLoader::GetCompletedTasks() {
  std::vector<TextureLoadTask> result = mCompletedTasks;
  mCompletedTasks.clear();
  mPendingCount -= result.size();
  return result;
}

void MtTextureLoader::ProcessCompletions() {
  // Called from render thread to process any pending GCD callbacks
  // GCD will handle this automatically via dispatch_async to main_queue
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
  return mCompletedTasks.size();
}

void MtTextureLoader::ProcessTextureTask(TextureLoadTask &task) {
  if (!task.texSource) return;

  // This runs on background GCD queue - CPU intensive work
  FTextureBuffer texbuffer = task.texSource->CreateTexBuffer(
      task.translation, task.flags | CTF_ProcessData);

  if (!texbuffer.mBuffer) {
    return;
  }

  task.width = texbuffer.mWidth;
  task.height = texbuffer.mHeight;
  task.indexed = (task.flags & CTF_Indexed) != 0;
  task.bytesPerPixel = task.indexed ? 1 : 4;

  // Copy pixel data
  size_t bufferSize = task.width * task.height * task.bytesPerPixel;
  task.pixelData.resize(bufferSize);
  memcpy(task.pixelData.data(), texbuffer.mBuffer, bufferSize);

  task.completed = true;
}
