#include "i_time.h"
#include <Metal/Metal.hpp>
#include <cstdio>

#include "mt_commandbuffer.h"
#include "mt_renderdevice.h"
#include "mt_version.h"
#include "metal/renderer/mt_debug.h"
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
  // Every call here is a separate command buffer submitted outside the main
  // per-frame render command buffer (world texture uploads, lightmap
  // uploads, mipmap regeneration) -- centralized counting here catches all
  // of them uniformly without touching each call site.
  if (fb->GetDebugManager())
    fb->GetDebugManager()->RecordExtraCommandBuffer();
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
  MetalRenderDevice *device = fb;
  cb->addCompletedHandler([=](MTL::CommandBuffer* buffer) {
      if (sem) {
          dispatch_semaphore_signal(sem);
          if (device)
            device->RecordSemSignal();
      }
  });

  // Real GPU execution time for the whole frame (this is normally the only
  // command buffer per frame -- see GetBlitCommandBuffer, unused by the
  // frame path). Runs on Metal's internal completion-handler thread, not
  // the render thread, so it only does a thread-safe atomic handoff; the
  // render thread drains it once per frame in MtDebugManager::EndFrame.
  if (fb->mVersionManager.supportsGPUTimestamps) {
    MtDebugManager *debugManager = fb->GetDebugManager();
    cb->addCompletedHandler([debugManager](MTL::CommandBuffer *buffer) {
      if (!debugManager || buffer->status() != MTL::CommandBufferStatusCompleted)
        return;
      double gpuSeconds = buffer->GPUEndTime() - buffer->GPUStartTime();
      if (gpuSeconds > 0.0)
        debugManager->RecordGPUFrameTimeAsync((float)(gpuSeconds * 1000.0));
    });
  }

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
