#pragma once

#include <functional>
#include <memory>

#define TimeScale TimeScale_GZDOOM
namespace MTL {
class CommandBuffer;
} // namespace MTL
#undef TimeScale

class MetalRenderDevice;

// Command buffer manager
class MtCommandBufferManager {
public:
  MtCommandBufferManager(MetalRenderDevice *fb);
  ~MtCommandBufferManager();

  // Get command buffers for different operations
  MTL::CommandBuffer *GetRenderCommandBuffer();
  MTL::CommandBuffer *GetBlitCommandBuffer();
  MTL::CommandBuffer *CreateNewCommandBuffer();
  MTL::CommandBuffer *GetCurrentCommandBuffer() {
    return mCurrentCommandBuffer;
  }

  // Submit and wait
  void FlushCommands(bool wait = false);
  void WaitForCommands(bool finish);

  // Frame synchronization
  void BeginFrame();
  void EndFrame();

  // Add completion handler
  void AddCompletedHandler(MTL::CommandBuffer *cmdBuffer,
                           std::function<void()> handler);

private:
  MetalRenderDevice *fb = nullptr;

  MTL::CommandBuffer *mCurrentCommandBuffer = nullptr;

  int mFrameIndex = 0;
};
