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
    mCurrentCommandBuffer = CreateNewCommandBuffer();
  }
  return mCurrentCommandBuffer;
}

MTL::CommandBuffer *MtCommandBufferManager::CreateNewCommandBuffer() {
  MTL::CommandBuffer *cmdBuf = fb->device->commandQueue->commandBuffer();
  if (!cmdBuf) {
    throw CMetalError("Failed to create command buffer");
  }
  cmdBuf->retain(); // Keep alive for our manual management
  
  // Add completed handler to signal semaphore for inflight frame management
  dispatch_semaphore_t sem = fb->GetInflightSemaphore();
  [(__bridge id<MTLCommandBuffer>)cmdBuf addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
      if (buffer.status == MTLCommandBufferStatusError && buffer.error) {
          NSLog(@"Metal: CommandBuffer Error: %@", buffer.error.localizedDescription);
      }
      dispatch_semaphore_signal(sem);
  }];
  return cmdBuf;
}

MTL::CommandBuffer *MtCommandBufferManager::GetBlitCommandBuffer() {
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

  if (mCurrentCommandBuffer) {
    mCurrentCommandBuffer->release();
  }
  
  // Proactively create command buffer for the frame
  mCurrentCommandBuffer = CreateNewCommandBuffer();
}

void MtCommandBufferManager::EndFrame() {
  if (mCurrentCommandBuffer) {
    mCurrentCommandBuffer->release();
    mCurrentCommandBuffer = nullptr;
  }
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
