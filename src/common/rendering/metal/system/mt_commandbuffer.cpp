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

  // Add error logging to every buffer
  cmdBuf->addCompletedHandler([=](MTL::CommandBuffer* buffer) {
    if (buffer->status() == MTL::CommandBufferStatusError && buffer->error()) {
        const char *errStr = buffer->error()->localizedDescription()->utf8String();
        Printf(PRINT_LOG, "Metal: CommandBuffer %p Error: %s\n", buffer, errStr);
    }
  });

  return cmdBuf;
}

MTL::CommandBuffer *MtCommandBufferManager::GetBlitCommandBuffer() {
  return CreateNewCommandBuffer();
}

void MtCommandBufferManager::FlushCommands(bool wait) {
  if (mCurrentCommandBuffer) {
    // Safety check: ensure any active render pass is ended before commit
    auto renderState = dynamic_cast<MtRenderState*>(fb->RenderState());
    if (renderState) {
        renderState->EndRenderPass();
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
  if (finish) FlushCommands(false);

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
  // Ensure we have at least one command buffer
  MTL::CommandBuffer *cb = GetRenderCommandBuffer();
  dispatch_semaphore_t sem = fb->GetInflightSemaphore();

  // Add the signal handler BEFORE commit
  cb->addCompletedHandler([=](MTL::CommandBuffer* buffer) {
      if (sem) {
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
