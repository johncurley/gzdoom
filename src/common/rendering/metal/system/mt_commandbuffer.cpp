#include "i_time.h"
#define TimeScale TimeScale_GZDOOM
#include <Metal/Metal.hpp>
#include <cstdio>
#undef TimeScale

#include "mt_commandbuffer.h"
#include "mt_renderdevice.h"
#include "metal/renderer/mt_renderstate.h"
#include "c_cvars.h"
#include "printf.h"

EXTERN_CVAR(Bool, mt_debug)
void MetalPrintLog(const char *typestr, const std::string &msg);

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

  if (mt_debug) Printf("Metal: Created CommandBuffer %p\n", cmdBuf);

  // Add error logging to every buffer
  cmdBuf->addCompletedHandler([=](MTL::CommandBuffer* buffer) {
      if (buffer->status() == MTL::CommandBufferStatusError && buffer->error()) {
          NS::String* desc = buffer->error()->localizedDescription();
          const char* errStr = desc ? desc->utf8String() : "Unknown Error";
          fprintf(stderr, "Metal: CommandBuffer %p Error: %s\n", buffer, errStr);
          MetalPrintLog("Error", errStr);
      } else if (mt_debug) {
          fprintf(stderr, "Metal: CommandBuffer %p completed successfully\n", buffer);
      }
  });

  return cmdBuf;
}

MTL::CommandBuffer *MtCommandBufferManager::GetBlitCommandBuffer() {
  return CreateNewCommandBuffer();
}

void MtCommandBufferManager::FlushCommands(bool wait) {
  if (mCurrentCommandBuffer) {
    if (mt_debug) Printf("Metal: Flushing CommandBuffer %p (wait=%d)\n", mCurrentCommandBuffer, (int)wait);
    
    // Reset apply count in render state since we are starting a new command buffer
    auto renderState = dynamic_cast<MtRenderState*>(fb->RenderState());
    if (renderState) {
        renderState->ResetApplyCount();
    }

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
      if (mCurrentCommandBuffer->status() < MTL::CommandBufferStatusCommitted)
        mCurrentCommandBuffer->commit();
      mCurrentCommandBuffer->waitUntilCompleted();
    }
    mCurrentCommandBuffer->release();
    mCurrentCommandBuffer = nullptr;
  }

  if (finish) {
    // Absolute GPU sync: submit a dummy buffer and wait for it.
    // This ensures all work submitted BEFORE this point is finished.
    auto waitBuf = fb->device->commandQueue->commandBuffer();
    if (waitBuf) {
        waitBuf->commit();
        waitBuf->waitUntilCompleted();
    }
  }
}

void MtCommandBufferManager::BeginFrame() {
  mFrameIndex++;
  // mCurrentCommandBuffer should be null here because EndFrame clears it
}

void MtCommandBufferManager::EndFrame() {
  // Ensure we have at least one command buffer to signal the semaphore
  MTL::CommandBuffer *cb = GetRenderCommandBuffer();
  dispatch_semaphore_t sem = fb->GetInflightSemaphore();

  if (mt_debug) Printf("Metal: EndFrame committing CommandBuffer %p with semaphore signal\n", cb);

  // Add the signal handler BEFORE commit
  cb->addCompletedHandler([=](MTL::CommandBuffer* buffer) {
      if (sem) {
          // if (mt_debug) Printf("Metal: CommandBuffer %p completed, signaling semaphore\n", buffer); // Can't print easily from bg thread
          dispatch_semaphore_signal(sem);
      }
  });

  cb->commit();
  cb->release();
  mCurrentCommandBuffer = nullptr;
}

void MtCommandBufferManager::AddCompletedHandler(
    MTL::CommandBuffer *cmdBuffer, std::function<void()> handler) {
  if (!cmdBuffer || !handler)
    return;

  cmdBuffer->addCompletedHandler([handler](MTL::CommandBuffer* buffer) {
    handler();
  });
}
