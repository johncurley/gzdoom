#include "i_time.h"
#define TimeScale TimeScale_GZDOOM
#include <Metal/Metal.hpp>
#undef TimeScale

#include "mt_commandbuffer.h"
#include "mt_renderdevice.h"

MtCommandBufferManager::MtCommandBufferManager(MetalRenderDevice *fb)
    : fb(fb) {}

MtCommandBufferManager::~MtCommandBufferManager() {}

MTL::CommandBuffer *MtCommandBufferManager::GetRenderCommandBuffer() {
  if (!mCurrentCommandBuffer) {
    MTL::CommandBuffer *cmdBuf = fb->device->commandQueue->commandBuffer();
    if (!cmdBuf) {
      throw CMetalError("Failed to create command buffer");
    }
    cmdBuf->retain(); // Keep alive
    mCurrentCommandBuffer = cmdBuf;
  }
  return mCurrentCommandBuffer;
}

MTL::CommandBuffer *MtCommandBufferManager::GetBlitCommandBuffer() {
  // For now, reuse the same command buffer
  // In the future, we could optimize by using separate blit command encoders
  return GetRenderCommandBuffer();
}

void MtCommandBufferManager::FlushCommands() {
  if (mCurrentCommandBuffer) {
    mCurrentCommandBuffer->commit();
    mCurrentCommandBuffer->release();
    mCurrentCommandBuffer = nullptr;
  }
}

void MtCommandBufferManager::WaitForCommands(bool finish) {
  if (mCurrentCommandBuffer) {
    if (finish) {
      mCurrentCommandBuffer->waitUntilCompleted();
    }
    mCurrentCommandBuffer->release();
    mCurrentCommandBuffer = nullptr;
  }
}

void MtCommandBufferManager::BeginFrame() {
  mFrameIndex++;

  // Create new command buffer for this frame
  if (mCurrentCommandBuffer) {
    mCurrentCommandBuffer->release();
  }
  MTL::CommandBuffer *cmdBuf = fb->device->commandQueue->commandBuffer();
  if (cmdBuf) {
    cmdBuf->retain();
    mCurrentCommandBuffer = cmdBuf;
  } else {
    mCurrentCommandBuffer = nullptr;
  }
}

void MtCommandBufferManager::EndFrame() {
  // Commit the frame's command buffer
  if (mCurrentCommandBuffer) {
    mCurrentCommandBuffer->commit();
    // Don't release yet - let the next frame or wait handle it
  }
}

void MtCommandBufferManager::AddCompletedHandler(
    MTL::CommandBuffer *cmdBuffer, std::function<void()> handler) {
  if (!cmdBuffer || !handler)
    return;

  // Store handler and attach completion block
  auto handlerPtr = new std::function<void()>(handler);
  cmdBuffer->addCompletedHandler(^(MTL::CommandBuffer *buffer) {
    (*handlerPtr)();
    delete handlerPtr;
  });
}
