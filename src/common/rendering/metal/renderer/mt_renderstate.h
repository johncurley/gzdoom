#pragma once

#include "metal/shaders/mt_shader.h"
#include "metal/system/mt_hwbuffer.h"
#include "mt_pipelinestate.h"
#include "mt_streambuffer.h"

#include "hw_material.h"
#include "hw_renderstate.h"
#include "name.h"

namespace MTL {
class RenderCommandEncoder;
class RenderPassDescriptor;
class Texture;
} // namespace MTL

class MetalRenderDevice;
class MtPipelineState;
class MtTextureImage;

class MtRenderState : public FRenderState {
public:
  MtRenderState(MetalRenderDevice *fb);
  ~MtRenderState() = default;

  // Draw commands
  void ClearScreen() override;
  void Draw(int dt, int index, int count, bool apply = true) override;
  void DrawIndexed(int dt, int index, int count, bool apply = true) override;

  // Immediate render state change commands. These only change infrequently and
  // should not clutter the render state.
  bool SetDepthClamp(bool on) override;
  void SetDepthMask(bool on) override;
  void SetDepthFunc(int func) override;
  void SetDepthRange(float min, float max) override;
  void SetColorMask(bool r, bool g, bool b, bool a) override;
  void SetStencil(int offs, int op, int flags = -1) override;
  void SetCulling(int mode) override;
  void EnableClipDistance(int num, bool state) override;
  void Clear(int targets) override;
  void EnableStencil(bool on) override;
  void SetScissor(int x, int y, int w, int h) override;
  void SetViewport(int x, int y, int w, int h) override;
  void EnableDepthTest(bool on) override;
  void EnableMultisampling(bool on) override;
  void EnableLineSmooth(bool on) override;
  void EnableDrawBuffers(int count, bool apply) override;

  void BeginFrame();
  void SetRenderTarget(MTL::Texture *image, MTL::Texture *depthStencilView,
                       int width, int height, int format, int samples);
  void Bind(int bindingpoint, uint32_t offset);
  void EndRenderPass();
  void EndFrame();

  MTL::RenderCommandEncoder *GetEncoder() { return mEncoder; }
  const auto &GetRenderTarget() const { return mRenderTarget; }

protected:
  // Lazy state evaluation pattern (from Vulkan)
  void Apply(int dt);
  void ApplyRenderPass(int dt);
  void ApplyStencilRef();
  void ApplyDepthBias();
  void ApplyScissor();
  void ApplyViewport();
  void ApplyStreamData();
  void ApplyMatrices();
  void ApplyPushConstants();
  void ApplyHWBufferSet();
  void ApplyVertexBuffers();
  void ApplyMaterial();
  void ApplyCulling();

  void BeginRenderPass();
  void WaitForStreamBuffers();

  MetalRenderDevice *fb = nullptr;

  bool mDepthClamp = true;
  MTL::RenderCommandEncoder *mEncoder = nullptr;
  MtPipelineKey mPipelineKey = {};
  MTL::RenderPassDescriptor *mPassDescriptor = nullptr;
  int mClearTargets = 0;
  bool mNeedApply = true;

  // State tracking (same as Vulkan)
  int mScissorX = 0, mScissorY = 0, mScissorWidth = -1, mScissorHeight = -1;
  int mViewportX = 0, mViewportY = 0, mViewportWidth = -1, mViewportHeight = -1;
  float mViewportDepthMin = 0.0f, mViewportDepthMax = 1.0f;
  bool mScissorChanged = true;
  bool mViewportChanged = true;

  bool mDepthTest = false;
  bool mDepthWrite = false;
  bool mStencilTest = false;
  int mClipDistanceMask = 0;

  bool mStencilRefChanged = false;
  int mStencilRef = 0;
  int mStencilOp = 0;
  int mDepthFunc = 0;
  int mColorMask = 15;
  int mCullMode = 0;
  bool mCullModeChanged = true;

  PushConstants mPushConstants = {};

  uint32_t mLastViewpointOffset = 0xffffffff;
  uint32_t mLastMatricesOffset = 0xffffffff;
  uint32_t mLastStreamDataOffset = 0xffffffff;
  uint32_t mViewpointOffset = 0;

  MtStreamBufferWriter mStreamBufferWriter;
  MtMatrixBufferWriter mMatrixBufferWriter;

  int mLastVertexOffsets[2] = {0, 0};
  IVertexBuffer *mLastVertexBuffer = nullptr;
  IIndexBuffer *mLastIndexBuffer = nullptr;

  bool mLastModelMatrixEnabled = true;
  bool mLastTextureMatrixEnabled = true;

  int mApplyCount = 0;

  struct RenderTarget {
    MTL::Texture *Image = nullptr;
    MTL::Texture *DepthStencil = nullptr;
    int Width = 0;
    int Height = 0;
    int Format = 0;  // MTLPixelFormat
    int Samples = 1; // Sample count
    int DrawBuffers = 1;
  } mRenderTarget;
};
