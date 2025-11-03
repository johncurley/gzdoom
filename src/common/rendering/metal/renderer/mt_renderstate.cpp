/*
**  Metal backend - Render state
**  Copyright (c) 2025 GZDoom Contributors
*/

#include <Metal/Metal.hpp>
#include "mt_renderstate.h"
#include "mt_renderdevice.h"

MtRenderState::MtRenderState(MetalRenderDevice* fb) : fb(fb), mStreamBufferWriter(fb), mMatrixBufferWriter(fb) {}

void MtRenderState::ClearScreen() {}
void MtRenderState::Draw(int dt, int index, int count, bool apply) { if (apply) Apply(dt); }
void MtRenderState::DrawIndexed(int dt, int index, int count, bool apply) { if (apply) Apply(dt); }
bool MtRenderState::SetDepthClamp(bool on) { mDepthClamp = on; return true; }
void MtRenderState::SetDepthMask(bool on) { mDepthWrite = on; }
void MtRenderState::SetDepthFunc(int func) { mDepthFunc = func; }
void MtRenderState::SetDepthRange(float min, float max) { mViewportDepthMin = min; mViewportDepthMax = max; }
void MtRenderState::SetColorMask(bool r, bool g, bool b, bool a) { mColorMask = (r?1:0)|(g?2:0)|(b?4:0)|(a?8:0); }
void MtRenderState::SetStencil(int offs, int op, int flags) { mStencilRef = offs; mStencilOp = op; }
void MtRenderState::SetCulling(int mode) { mCullMode = mode; }
void MtRenderState::EnableClipDistance(int num, bool state) {}
void MtRenderState::Clear(int targets) { mClearTargets = targets; }
void MtRenderState::EnableStencil(bool on) { mStencilTest = on; }
void MtRenderState::SetScissor(int x, int y, int w, int h) { mScissorX = x; mScissorY = y; mScissorWidth = w; mScissorHeight = h; mScissorChanged = true; }
void MtRenderState::SetViewport(int x, int y, int w, int h) { mViewportX = x; mViewportY = y; mViewportWidth = w; mViewportHeight = h; mViewportChanged = true; }
void MtRenderState::EnableDepthTest(bool on) { mDepthTest = on; }
void MtRenderState::EnableMultisampling(bool on) {}
void MtRenderState::EnableLineSmooth(bool on) {}
void MtRenderState::EnableDrawBuffers(int count, bool apply) {}
void MtRenderState::BeginFrame() {}
void MtRenderState::SetRenderTarget(MtTextureImage* image, void* depthStencilView, int width, int height, int format, int samples) {}
void MtRenderState::Bind(int bindingpoint, uint32_t offset) {}
void MtRenderState::EndRenderPass() { if (mEncoder) { ((MTL::RenderCommandEncoder*)mEncoder)->endEncoding(); mEncoder = nullptr; } }
void MtRenderState::EndFrame() {}
void MtRenderState::Apply(int dt) { mNeedApply = false; mApplyCount++; }
void MtRenderState::ApplyRenderPass(int dt) {}
void MtRenderState::ApplyStencilRef() {}
void MtRenderState::ApplyDepthBias() {}
void MtRenderState::ApplyScissor() {}
void MtRenderState::ApplyViewport() {}
void MtRenderState::ApplyStreamData() {}
void MtRenderState::ApplyMatrices() {}
void MtRenderState::ApplyPushConstants() {}
void MtRenderState::ApplyHWBufferSet() {}
void MtRenderState::ApplyVertexBuffers() {}
void MtRenderState::ApplyMaterial() {}
void MtRenderState::BeginRenderPass() {}
void MtRenderState::WaitForStreamBuffers() {}

MtStreamBufferWriter::MtStreamBufferWriter(MetalRenderDevice* fb) {}
uint32_t MtStreamBufferWriter::Write(const void* data, size_t size) { return 0; }
void MtStreamBufferWriter::Reset() {}

MtMatrixBufferWriter::MtMatrixBufferWriter(MetalRenderDevice* fb) {}
uint32_t MtMatrixBufferWriter::Write(const void* data, size_t size) { return 0; }
void MtMatrixBufferWriter::Reset() {}
