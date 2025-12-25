#include "i_time.h"
#define TimeScale TimeScale_GZDOOM
#include <Metal/Metal.hpp>
#import <Metal/Metal.h>
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
    
    // Add completed handler to signal semaphore for inflight frame management
    // This is especially important on Intel GPUs to maintain steady frame pacing.
    dispatch_semaphore_t sem = fb->GetInflightSemaphore();
    [(__bridge id<MTLCommandBuffer>)cmdBuf addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
        if (buffer.status == MTLCommandBufferStatusError) {
            NSLog(@"Metal: CommandBuffer Error: %@", buffer.error.localizedDescription);
        }
        dispatch_semaphore_signal(sem);
    }];
    
    mCurrentCommandBuffer = cmdBuf;
  }
  return mCurrentCommandBuffer;
}

MTL::CommandBuffer *MtCommandBufferManager::GetBlitCommandBuffer() {
  // For now, reuse the same command buffer
  // In the future, we could optimize by using separate blit command encoders
  return GetRenderCommandBuffer();
}

void MtCommandBufferManager::FlushCommands(bool wait) {
  if (mCurrentCommandBuffer) {
    mCurrentCommandBuffer->commit();
    if (wait) {
      mCurrentCommandBuffer->waitUntilCompleted();
    }
    mCurrentCommandBuffer->release();
    mCurrentCommandBuffer = nullptr;
  }
}

void MtCommandBufferManager::WaitForCommands(bool finish) {
  if (mCurrentCommandBuffer) {
    if (finish) {
      if (mCurrentCommandBuffer->status() < 2) // MTLCommandBufferStatusCommitted
        mCurrentCommandBuffer->commit();
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
  // Command buffer is committed and released in PresentFrame
  // Just clear our reference here
  mCurrentCommandBuffer = nullptr;
}

void MtCommandBufferManager::AddCompletedHandler(
    MTL::CommandBuffer *cmdBuffer, std::function<void()> handler) {
  if (!cmdBuffer || !handler)
    return;

  // Use the Objective-C version of CommandBuffer to add the handler
  id<MTLCommandBuffer> objcCmdBuf = (__bridge id<MTLCommandBuffer>)cmdBuffer;
  [objcCmdBuf addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
    handler();
  }];
}
