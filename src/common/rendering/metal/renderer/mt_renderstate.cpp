/*
**  Metal backend
**  Copyright (c) 2020-2025 Magnus Norddahl
**
**  This software is provided 'as-is', without any express or implied
**  warranty.  In no event will the authors be held liable for any damages
**  arising from the use of this software.
**
**  Permission is granted to anyone to use this software for any purpose,
**  including commercial applications, and to alter it and redistribute it
**  freely, subject to the following restrictions:
**
**  1. The origin of this software must not be misrepresented; you must not
**     claim that you wrote the original software. If you use this software
**     in a product, an acknowledgment in the product documentation would be
**     appreciated but is not required.
**  2. Altered source versions must be plainly marked as such, and must not be
**     misrepresented as being the original software.
**  3. This notice may not be removed or altered from any source distribution.
**
*/

#include "mt_renderstate.h"
#include "v_video.h"
#include "doomdef.h"
#include "metal/renderer/mt_pipelinestate.h"
#include "metal/renderer/mt_renderbuffers.h"
#include "metal/renderer/mt_resourcebinding.h"
#include "metal/renderer/mt_debug.h"
#include "metal/system/mt_buffer.h"
#include "metal/system/mt_commandbuffer.h"
#include "metal/system/mt_hwbuffer.h" // Needed for MtHardwareDataBuffer definition
#include "metal/system/mt_renderdevice.h"
#include "metal/textures/mt_sampler.h"
#include "metal/textures/mt_texture.h"

#include "flatvertices.h"
#include "gamestate.h"
#include "hw_clock.h"
#include "hw_cvars.h"
#include "hw_lightbuffer.h"
#include "hw_skydome.h"
#include "hw_viewpointuniforms.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "hwrenderer/data/shaderuniforms.h"
#include "v_text.h"

#include <chrono>

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

CVAR(Int, mt_submit_size, 3000, 0);
EXTERN_CVAR(Bool, r_skipmats)
EXTERN_CVAR(Bool, mt_debug)
MtRenderState::MtRenderState(MetalRenderDevice *fb)
    : fb(fb), mStreamBufferWriter(fb), mMatrixBufferWriter(fb) {
  mStencilFunc = 2; // DF_Always (Index 2 in GZDoom)
  mStencilRef = 0;
  mVirtualWidth = max(fb->GetWidth(), 1);
  mVirtualHeight = max(fb->GetHeight(), 1);
  Reset();
}

MtRenderState::~MtRenderState() {
  if (mPassDescriptor) {
    mPassDescriptor->release();
    mPassDescriptor = nullptr;
  }
}

void MtRenderState::ClearScreen() {
  if (mt_debug) {
    Printf(PRINT_LOG, "Metal: ClearScreen\n");
  }
  screen->mViewpoints->Set2D(*this, SCREENWIDTH, SCREENHEIGHT);
  SetColor(0, 0, 0);
  SetVertexBuffer(screen->mVertexData->GetBufferObjects().first, 0, 0);
  Apply(DT_TriangleStrip);

  if (mEncoder) {
    fb->GetDebugManager()->RecordDrawCallCategory(4, 4, "ui");
    mEncoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangleStrip,
                             FFlatVertexBuffer::FULLSCREEN_INDEX, 4, 1);
  }
}

void MtRenderState::Draw(int dt, int index, int count, bool apply) {
  // If an indexed batch is pending, flush it NOW — while the indexed pipeline is
  // still the current encoder state.  apply() below will switch to the triangle
  // pipeline; flushing after that would use the wrong pipeline for those draws.
  if (mPendingIndexedBatch.active) {
    FlushIndexedBatch();
    // Force a full Apply() so the triangle pipeline is correctly (re-)bound.
    mNeedApply = true;
  }

  if (apply || mNeedApply) {
    if (mPendingBatch.active) {
      // If state changed or primitive type changed, flush the batch
      if (mPendingBatch.dt != dt || mNeedApply) {
        FlushBatch();
      }
    }
    Apply(dt);
  }

  if (!mEncoder || !mPipelineBound || count <= 0)
    return;

  // Check if we can start or continue a batch
  // For simplicity, we only batch DT_TriangleFan, DT_Triangles, and DT_TriangleStrip
  // and only if the current PushConstants and PipelineKey match the batch.
  // uDataIndex is part of PushConstants, so it's naturally handled.
  bool canBatch = (dt == DT_TriangleFan || dt == DT_Triangles || dt == DT_TriangleStrip);

  if (canBatch) {
    if (!mPendingBatch.active) {
      // For batching, we always convert to a triangle list
      mPendingBatch.dt = DT_Triangles;
      mPendingBatch.pipelineKey = mPipelineKey;
      mPendingBatch.vertexBuffer = mVertexBuffer;
      mPendingBatch.vertexOffsets[0] = mVertexOffsets[0];
      mPendingBatch.vertexOffsets[1] = mVertexOffsets[1];
      mPendingBatch.cullMode = mCullMode;
      mPendingBatch.material = mMaterial.mMaterial;
      mPendingBatch.translation = mMaterial.mTranslation;
      mPendingBatch.clampMode = mMaterial.mClampMode;
      mPendingBatch.overrideShader = mMaterial.mOverrideShader;
      mPendingBatch.indexCount = 0;
      mPendingBatch.indexStart = mBatchIndexOffset;
      mPendingBatch.active = true;

      // Initialise first sub-draw with the current per-draw state
      mPendingBatch.subDrawCount = 0;
      mPendingBatch.currentPushConstants = mPushConstants;
      mPendingBatch.currentMatrixBuffer = mMatrixBufferWriter.GetBuffer();
      mPendingBatch.currentMatrixOffset = mMatrixBufferWriter.Offset();
      mPendingBatch.currentStreamBuffer = mStreamBufferWriter.GetBuffer();
      mPendingBatch.currentStreamOffset = mStreamBufferWriter.StreamDataOffset();
      mPendingBatch.currentSubDrawIndexStart = mBatchIndexOffset;
    }

    // Accumulate indices into BatchIndexBuffer
    if (mBatchIBPointer) {
      uint32_t *dest = mBatchIBPointer + mBatchIndexOffset;
      int addedIndices = 0;
      if (dt == DT_TriangleFan) {
        // Convert Fan to Triangles
        for (int i = 2; i < count; i++) {
          dest[addedIndices++] = index + 0;
          dest[addedIndices++] = index + i - 1;
          dest[addedIndices++] = index + i;
        }
      } else if (dt == DT_TriangleStrip) {
        // Convert Strip to Triangles
        for (int i = 2; i < count; i++) {
          if (i & 1) {
            dest[addedIndices++] = index + i - 1;
            dest[addedIndices++] = index + i - 2;
            dest[addedIndices++] = index + i;
          } else {
            dest[addedIndices++] = index + i - 2;
            dest[addedIndices++] = index + i - 1;
            dest[addedIndices++] = index + i;
          }
        }
      } else if (dt == DT_Triangles) {
        for (int i = 0; i < count; i++) {
          dest[addedIndices++] = index + i;
        }
      }

      mBatchIndexOffset += addedIndices;
      mPendingBatch.indexCount += addedIndices;

      // If buffer is getting full, flush it
      if (mBatchIndexOffset > 1000000) {
        FlushBatch();
      }
    }
  } else {
    // Non-batchable primitive, just draw immediately
    FlushBatch();
    MTL::PrimitiveType type;
    switch (dt) {
    case DT_Points:
      type = MTL::PrimitiveType::PrimitiveTypePoint;
      break;
    case DT_Lines:
      type = MTL::PrimitiveType::PrimitiveTypeLine;
      break;
    default:
      type = MTL::PrimitiveType::PrimitiveTypeTriangle;
      break;
    }
    fb->GetDebugManager()->RecordDrawCallCategory(count, count, mCurrentDrawCategory);
    mEncoder->drawPrimitives(type, index, count, 1);
    mApplyCount++;
  }
}

void MtRenderState::DrawIndexed(int dt, int index, int count, bool apply) {
  if (count <= 0)
    return;

  // Triangle and indexed batches are mutually exclusive.
  if (mPendingBatch.active)
    FlushBatch();

  auto mtIB = dynamic_cast<MtIndexBuffer *>(mIndexBuffer);
  if (!mtIB || !mtIB->GetBuffer())
    return;
  MTL::Buffer *rawIB = mtIB->GetBuffer();

  MTL::PrimitiveType type;
  switch (dt) {
  case DT_Points:
    type = MTL::PrimitiveTypePoint;
    break;
  case DT_Lines:
    type = MTL::PrimitiveTypeLine;
    break;
  case DT_TriangleStrip:
    type = MTL::PrimitiveTypeTriangleStrip;
    break;
  default:
    type = MTL::PrimitiveTypeTriangle;
    break;
  }

  // Fast path: if the draw shares the same IB/VB/material/pipeline as the current
  // indexed batch and no pipeline-breaking state has changed, skip the full Apply()
  // and only advance the per-draw stream/matrix buffers.
  bool canExtend = mPendingIndexedBatch.active && !mNeedApply &&
                   rawIB == mPendingIndexedBatch.indexBuffer &&
                   type == mPendingIndexedBatch.primitiveType &&
                   mVertexBuffer == mPendingIndexedBatch.vertexBuffer &&
                   mVertexOffsets[0] == mPendingIndexedBatch.vertexOffsets[0] &&
                   mVertexOffsets[1] == mPendingIndexedBatch.vertexOffsets[1] &&
                   mCullMode == mPendingIndexedBatch.cullMode &&
                   mMaterial.mMaterial == mPendingIndexedBatch.material &&
                   mMaterial.mTranslation == mPendingIndexedBatch.translation &&
                   mMaterial.mClampMode == mPendingIndexedBatch.clampMode &&
                   mMaterial.mOverrideShader == mPendingIndexedBatch.overrideShader;

  if (canExtend) {
    // Advance per-draw ring buffer writers and rebuild push constants.
    ApplyStreamData();
    ApplyMatrices();

    // If WaitForStreamBuffers() fired inside ApplyStreamData/ApplyMatrices, the
    // indexed batch was flushed by EndRenderPass() and active is now false.
    // Fall through to the full Apply path to re-establish state.
    if (!mPendingIndexedBatch.active) {
      canExtend = false;
      mNeedApply = true;
    } else {
      BuildPushConstants();
    }
  }

  if (!canExtend) {
    // Full state apply: flush the old indexed batch, rebind pipeline/material.
    FlushIndexedBatch();
    if (apply || mNeedApply)
      Apply(dt);

    if (!mEncoder || !mPipelineBound)
      return;

    // Start a new indexed batch with the current state as the key.
    mPendingIndexedBatch.primitiveType = type;
    mPendingIndexedBatch.vertexBuffer = mVertexBuffer;
    mPendingIndexedBatch.vertexOffsets[0] = mVertexOffsets[0];
    mPendingIndexedBatch.vertexOffsets[1] = mVertexOffsets[1];
    mPendingIndexedBatch.indexBuffer = rawIB;
    mPendingIndexedBatch.pipelineKey = mPipelineKey;
    mPendingIndexedBatch.cullMode = mCullMode;
    mPendingIndexedBatch.material = mMaterial.mMaterial;
    mPendingIndexedBatch.translation = mMaterial.mTranslation;
    mPendingIndexedBatch.clampMode = mMaterial.mClampMode;
    mPendingIndexedBatch.overrideShader = mMaterial.mOverrideShader;
    mPendingIndexedBatch.subDrawCount = 0;
    mPendingIndexedBatch.active = true;
  }

  if (!mEncoder || !mPipelineBound)
    return;

  // Flush if the sub-draw array is full.
  if (mPendingIndexedBatch.subDrawCount >= PendingIndexedBatch::kMaxSubDraws) {
    FlushIndexedBatch();
    // Re-open the batch after flush (state is still bound).
    mPendingIndexedBatch.active = true;
    mPendingIndexedBatch.subDrawCount = 0;
  }

  auto &sub =
      mPendingIndexedBatch.subDraws[mPendingIndexedBatch.subDrawCount++];
  sub.pushConstants = mPushConstants;
  sub.matrixBuffer = mMatrixBufferWriter.GetBuffer();
  sub.matrixOffset = mMatrixBufferWriter.Offset();
  sub.streamBuffer = mStreamBufferWriter.GetBuffer();
  sub.streamOffset = mStreamBufferWriter.StreamDataOffset();
  sub.indexStart = index;
  sub.indexCount = count;
}


void MtRenderState::FlushBatch() {
  // NOTE: Do NOT call FlushIndexedBatch() here.  The two batch types use different
  // pipelines; flushing the indexed batch from inside FlushBatch() would emit
  // indexed draws with whatever pipeline is currently bound (likely the triangle
  // pipeline), producing incorrect rendering.  Each call site is responsible for
  // flushing the opposite batch type before switching pipelines.

  if (!mPendingBatch.active || mPendingBatch.indexCount == 0) {
    mPendingBatch.active = false;
    mPendingBatch.subDrawCount = 0;
    return;
  }

  // Finalise the still-accumulating sub-draw (may have indices that haven't been
  // committed to a SubDraw entry yet).
  unsigned int lastSubDrawIndexCount =
      mBatchIndexOffset - mPendingBatch.currentSubDrawIndexStart;
  if (lastSubDrawIndexCount > 0 &&
      mPendingBatch.subDrawCount < PendingBatch::kMaxSubDraws) {
    auto &sub = mPendingBatch.subDraws[mPendingBatch.subDrawCount++];
    sub.pushConstants = mPendingBatch.currentPushConstants;
    sub.matrixBuffer = mPendingBatch.currentMatrixBuffer;
    sub.matrixOffset = mPendingBatch.currentMatrixOffset;
    sub.streamBuffer = mPendingBatch.currentStreamBuffer;
    sub.streamOffset = mPendingBatch.currentStreamOffset;
    sub.indexStart = mPendingBatch.currentSubDrawIndexStart;
    sub.indexCount = lastSubDrawIndexCount;
  }

  if (mPendingBatch.subDrawCount == 0) {
    mPendingBatch.active = false;
    mPendingBatch.indexCount = 0;
    return;
  }

  if (mEncoder && mPipelineBound) {
    auto batchIB = dynamic_cast<MtIndexBuffer *>(
        fb->GetBufferManager()->BatchIndexBuffer.get());
    if (batchIB) {
      // Upload the entire accumulated index range once.
      batchIB->Upload(mPendingBatch.indexStart * 4,
                      mPendingBatch.indexCount * 4);
      auto mtlIB = batchIB->GetBuffer();
      if (mtlIB) {
        for (int i = 0; i < mPendingBatch.subDrawCount; i++) {
          const auto &sub = mPendingBatch.subDraws[i];
          if (sub.indexCount == 0)
            continue;

          // Push constants change per sub-draw (DataIndex, fog, lighting…).
          // setVertexBytes is cheap inline data — no separate buffer allocation.
          mEncoder->setVertexBytes(&sub.pushConstants, sizeof(PushConstants),
                                   21);
          mEncoder->setFragmentBytes(&sub.pushConstants, sizeof(PushConstants),
                                     21);

          // Re-bind matrix buffer only if it changed from the previous sub-draw.
          if (sub.matrixBuffer &&
              (sub.matrixBuffer != mLastBoundBuffers[19] ||
               sub.matrixOffset != mLastBoundOffsets[19])) {
            mEncoder->setVertexBuffer(sub.matrixBuffer, sub.matrixOffset, 19);
            mEncoder->setFragmentBuffer(sub.matrixBuffer, sub.matrixOffset, 19);
            mLastBoundBuffers[19] = sub.matrixBuffer;
            mLastBoundOffsets[19] = sub.matrixOffset;
            mLastBoundFragmentBuffers[19] = sub.matrixBuffer;
            mLastBoundFragmentOffsets[19] = sub.matrixOffset;
          }

          // Re-bind stream buffer only if the block rolled over (rare within a
          // frame, but must be handled correctly).
          if (sub.streamBuffer &&
              (sub.streamBuffer != mLastBoundBuffers[20] ||
               sub.streamOffset != mLastBoundOffsets[20])) {
            mEncoder->setVertexBuffer(sub.streamBuffer, sub.streamOffset, 20);
            mEncoder->setFragmentBuffer(sub.streamBuffer, sub.streamOffset, 20);
            mLastBoundBuffers[20] = sub.streamBuffer;
            mLastBoundOffsets[20] = sub.streamOffset;
            mLastBoundFragmentBuffers[20] = sub.streamBuffer;
            mLastBoundFragmentOffsets[20] = sub.streamOffset;
          }

          fb->GetDebugManager()->RecordDrawCallCategory(
              sub.indexCount, sub.indexCount, mCurrentDrawCategory);
          mEncoder->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle,
                                          sub.indexCount, MTL::IndexTypeUInt32,
                                          mtlIB, sub.indexStart * 4);
          mApplyCount++;
        }
      }
    }
  }

  mPendingBatch.active = false;
  mPendingBatch.indexCount = 0;
  mPendingBatch.subDrawCount = 0;
  // Note: mBatchIndexOffset is only reset at BeginFrame or when the buffer fills.
}

// Called at the end of Apply() to detect per-draw state changes (push constants,
// matrix/stream buffer offsets) and break the current sub-draw without flushing
// the entire batch.
void MtRenderState::UpdateSubDrawState() {
  if (!mPendingBatch.active)
    return;

  bool changed =
      (mPushConstants != mPendingBatch.currentPushConstants) ||
      (mMatrixBufferWriter.GetBuffer() != mPendingBatch.currentMatrixBuffer) ||
      (mMatrixBufferWriter.Offset() != mPendingBatch.currentMatrixOffset) ||
      (mStreamBufferWriter.GetBuffer() != mPendingBatch.currentStreamBuffer) ||
      (mStreamBufferWriter.StreamDataOffset() !=
       mPendingBatch.currentStreamOffset);

  if (!changed)
    return;

  // Finalise the sub-draw that just ended, recording the indices it accumulated.
  unsigned int subDrawIndexCount =
      mBatchIndexOffset - mPendingBatch.currentSubDrawIndexStart;
  if (subDrawIndexCount > 0 &&
      mPendingBatch.subDrawCount < PendingBatch::kMaxSubDraws) {
    auto &sub = mPendingBatch.subDraws[mPendingBatch.subDrawCount++];
    sub.pushConstants = mPendingBatch.currentPushConstants;
    sub.matrixBuffer = mPendingBatch.currentMatrixBuffer;
    sub.matrixOffset = mPendingBatch.currentMatrixOffset;
    sub.streamBuffer = mPendingBatch.currentStreamBuffer;
    sub.streamOffset = mPendingBatch.currentStreamOffset;
    sub.indexStart = mPendingBatch.currentSubDrawIndexStart;
    sub.indexCount = subDrawIndexCount;
  }

  // If we've hit the sub-draw limit, flush the whole batch now.
  if (mPendingBatch.subDrawCount >= PendingBatch::kMaxSubDraws) {
    FlushBatch();
    return;
  }

  // Start the next sub-draw with the updated state.
  mPendingBatch.currentSubDrawIndexStart = mBatchIndexOffset;
  mPendingBatch.currentPushConstants = mPushConstants;
  mPendingBatch.currentMatrixBuffer = mMatrixBufferWriter.GetBuffer();
  mPendingBatch.currentMatrixOffset = mMatrixBufferWriter.Offset();
  mPendingBatch.currentStreamBuffer = mStreamBufferWriter.GetBuffer();
  mPendingBatch.currentStreamOffset = mStreamBufferWriter.StreamDataOffset();
}

// Emit all deferred indexed sub-draws, reusing the pipeline/material/texture state
// that was bound when the batch was started.  Only push constants and per-draw
// buffer offsets are re-issued per sub-draw — expensive state is shared.
void MtRenderState::FlushIndexedBatch() {
  if (!mPendingIndexedBatch.active || mPendingIndexedBatch.subDrawCount == 0) {
    mPendingIndexedBatch.active = false;
    return;
  }

  if (mEncoder && mPipelineBound && mPendingIndexedBatch.indexBuffer) {
    for (int i = 0; i < mPendingIndexedBatch.subDrawCount; i++) {
      const auto &sub = mPendingIndexedBatch.subDraws[i];
      if (sub.indexCount == 0)
        continue;

      mEncoder->setVertexBytes(&sub.pushConstants, sizeof(PushConstants), 21);
      mEncoder->setFragmentBytes(&sub.pushConstants, sizeof(PushConstants), 21);

      if (sub.matrixBuffer &&
          (sub.matrixBuffer != mLastBoundBuffers[19] ||
           sub.matrixOffset != mLastBoundOffsets[19])) {
        mEncoder->setVertexBuffer(sub.matrixBuffer, sub.matrixOffset, 19);
        mEncoder->setFragmentBuffer(sub.matrixBuffer, sub.matrixOffset, 19);
        mLastBoundBuffers[19] = sub.matrixBuffer;
        mLastBoundOffsets[19] = sub.matrixOffset;
        mLastBoundFragmentBuffers[19] = sub.matrixBuffer;
        mLastBoundFragmentOffsets[19] = sub.matrixOffset;
      }

      if (sub.streamBuffer &&
          (sub.streamBuffer != mLastBoundBuffers[20] ||
           sub.streamOffset != mLastBoundOffsets[20])) {
        mEncoder->setVertexBuffer(sub.streamBuffer, sub.streamOffset, 20);
        mEncoder->setFragmentBuffer(sub.streamBuffer, sub.streamOffset, 20);
        mLastBoundBuffers[20] = sub.streamBuffer;
        mLastBoundOffsets[20] = sub.streamOffset;
        mLastBoundFragmentBuffers[20] = sub.streamBuffer;
        mLastBoundFragmentOffsets[20] = sub.streamOffset;
      }

      fb->GetDebugManager()->RecordDrawCallCategory(sub.indexCount, sub.indexCount,
                                                    mCurrentDrawCategory);
      mEncoder->drawIndexedPrimitives(mPendingIndexedBatch.primitiveType,
                                      sub.indexCount, MTL::IndexTypeUInt32,
                                      mPendingIndexedBatch.indexBuffer,
                                      sub.indexStart * 4);
      mApplyCount++;
    }
  }

  mPendingIndexedBatch.active = false;
  mPendingIndexedBatch.subDrawCount = 0;
}

// Build mPushConstants from current render state without issuing any Metal API calls.
// Called in the DrawIndexed fast path to capture per-sub-draw data cheaply.
void MtRenderState::BuildPushConstants() {
  int fogset = 0;
  if (mFogEnabled) {
    if (mFogEnabled == 2)
      fogset = -3;
    else if ((GetFogColor() & 0xffffff) == 0)
      fogset = gl_fogmode;
    else
      fogset = -gl_fogmode;
  }

  int tempTM = TM_NORMAL;
  if (mMaterial.mMaterial && mMaterial.mMaterial->Source()->isHardwareCanvas())
    tempTM = TM_OPAQUE;

  mPushConstants.group1[0] = mClipSplit[0];
  mPushConstants.group1[1] = mClipSplit[1];
  mPushConstants.group1[2] = mGlossiness;
  mPushConstants.group1[3] = mSpecularLevel;
  if (mPushConstants.group1[2] == 0.0f && mPushConstants.group1[3] == 0.0f &&
      mMaterial.mMaterial) {
    auto source = mMaterial.mMaterial->Source();
    mPushConstants.group1[2] = source->GetGlossiness();
    mPushConstants.group1[3] = source->GetSpecularLevel();
  }

  mPushConstants.group2[0] = mLightParms[3];
  mPushConstants.group2[1] = mLightParms[2];
  mPushConstants.group2[2] = mLightParms[1];
  mPushConstants.group2[3] = mLightParms[0];

  mPushConstants.group3[0] = (float)GetTextureModeAndFlags(tempTM);
  mPushConstants.group3[1] = mAlphaThreshold;
  mPushConstants.group3[2] = (float)fogset;
  mPushConstants.group3[3] = (float)mLightIndex;

  mPushConstants.group4[0] = (float)mBoneIndexBase;
  mPushConstants.group4[1] = (float)mStreamBufferWriter.DataIndex();
  mPushConstants.group4[2] = (float)mClipDistanceMask;
  mPushConstants.group4[3] = 0.0f;
}

bool MtRenderState::SetDepthClamp(bool on) {
  bool lastValue = mDepthClamp;
  mDepthClamp = on;
  mNeedApply = true;
  return lastValue;
}

void MtRenderState::SetDepthMask(bool on) {
  mDepthWrite = on;
  mNeedApply = true;
}

void MtRenderState::SetDepthFunc(int func) {
  mDepthFunc = func;
  mNeedApply = true;
}

void MtRenderState::SetDepthRange(float min, float max) {
  mViewportDepthMin = min;
  mViewportDepthMax = max;
  mViewportChanged = true;
  mNeedApply = true;
}

void MtRenderState::SetColorMask(bool r, bool g, bool b, bool a) {
  int rr = r, gg = g, bb = b, aa = a;
  mColorMask = (aa << 3) | (bb << 2) | (gg << 1) | rr;
  mNeedApply = true;
}

void MtRenderState::SetStencil(int offs, int op, int flags) {
  mStencilRef = screen->stencilValue + offs;
  mStencilRefChanged = true;
  mStencilOp = op;

  // GZDoom's SetStencil expects an EQUAL test against the reference value.
  // We use our internal index 3 which maps to MTL::CompareFunctionEqual in
  // stencilFuncs.
  mStencilFunc = 3;

  if (flags != -1) {
    bool cmon = !(flags & SF_ColorMaskOff);
    SetColorMask(cmon, cmon, cmon, cmon);
    SetDepthMask(!(flags & SF_DepthMaskOff));
  }
  mNeedApply = true;
}

void MtRenderState::SetCulling(int mode) {
  mCullMode = mode;
  mCullModeChanged = true;
  mNeedApply = true;
}

void MtRenderState::EnableClipDistance(int num, bool state) {
  if (state)
    mClipDistanceMask |= (1 << num);
  else
    mClipDistanceMask &= ~(1 << num);
  mNeedApply = true;
}

void MtRenderState::Clear(int targets) {
  mClearTargets = targets;
  EndRenderPass();
}

void MtRenderState::EnableStencil(bool on) {
  mStencilTest = on;
  mNeedApply = true;
}

void MtRenderState::SetScissor(int x, int y, int w, int h) {
  mScissorX = x;
  mScissorY = y;
  mScissorWidth = w;
  mScissorHeight = h;
  mScissorChanged = true;
  mNeedApply = true;
}

void MtRenderState::SetViewport(int x, int y, int w, int h) {
  mViewportX = x;
  mViewportY = y;
  mViewportWidth = w;
  mViewportHeight = h;
  mViewportChanged = true;
  mNeedApply = true;
}

void MtRenderState::EnableDepthTest(bool on) {
  mDepthTest = on;
  mNeedApply = true;
}

void MtRenderState::EnableMultisampling(bool on) {}

void MtRenderState::EnableLineSmooth(bool on) {}

void MtRenderState::Apply(int dt) {
  // Handle command buffer flushing like Vulkan
  if (mApplyCount >= mt_submit_size) {
    EndRenderPass();
    fb->GetCommands()->FlushCommands();
    mApplyCount = 0;
  }

  ApplyStreamData();
  ApplyMatrices();

  // If we have an active batch, flush it only when geometry/material/pipeline-level
  // state changes.  Push constant and matrix/stream offset changes are handled as
  // sub-draw breaks inside UpdateSubDrawState() after the full Apply() run.
  if (mPendingBatch.active) {
    if (mPendingBatch.dt != dt ||
        mVertexBuffer != mPendingBatch.vertexBuffer ||
        mVertexOffsets[0] != mPendingBatch.vertexOffsets[0] ||
        mVertexOffsets[1] != mPendingBatch.vertexOffsets[1] ||
        mCullMode != mPendingBatch.cullMode ||
        mMaterial.mMaterial != mPendingBatch.material ||
        mMaterial.mTranslation != mPendingBatch.translation ||
        mMaterial.mClampMode != mPendingBatch.clampMode ||
        mMaterial.mOverrideShader != mPendingBatch.overrideShader) {
      FlushBatch();
    }
  }

  ApplyRenderPass(dt);

  if (!mPipelineBound)
    return;

  // If pipeline key changed, it might also break the batch
  if (mPendingBatch.active && mPipelineKey != mPendingBatch.pipelineKey) {
    FlushBatch();
  }

  ApplyScissor();
  ApplyCulling();

  // Implement Depth Clamping (Depth Clip Mode)
  if (mEncoder) {
    mEncoder->setDepthClipMode(mDepthClamp ? MTL::DepthClipModeClamp
                                           : MTL::DepthClipModeClip);
  }

  ApplyViewport();
  ApplyStencilRef();
  ApplyDepthBias();
  ApplyPushConstants();
  ApplyVertexBuffers();
  ApplyHWBufferSet();
  ApplyMaterial();
  ApplyFixedTextures();
  mNeedApply = false;

  // After all state is committed, check whether per-draw data (push constants,
  // matrix/stream buffer offsets) has changed since the last sub-draw.  If so,
  // finalise the previous sub-draw and start a new one — without flushing the
  // entire batch.
  UpdateSubDrawState();
}

void MtRenderState::ApplyFixedTextures() {
  if (!mEncoder)
    return;

  auto buffers = fb->GetBuffers();
  if (buffers->ShadowMap) {
    MTL::Texture *shadowTex = buffers->ShadowMap->GetTexture();
    if (shadowTex && shadowTex != mLastShadowMapTex) {
      mEncoder->setFragmentTexture(shadowTex, 14);
      mEncoder->setVertexTexture(shadowTex, 14);

      MtSamplerKey sk;
      sk.MinFilter = 0;
      sk.MagFilter = 0;
      sk.MipFilter = 0;
      sk.AddressU = 3;
      sk.AddressV = 3;
      sk.AddressW = 3;
      sk.MaxAnisotropy = 1.0f;

      auto sampler = fb->GetSamplerManager()->GetSamplerState(sk);
      if (sampler) {
        mEncoder->setFragmentSamplerState(sampler, 14);
        mEncoder->setVertexSamplerState(sampler, 14);
      }
      mLastShadowMapTex = shadowTex;
    }
  }

  MTL::Texture *lmTex = fb->GetTextureManager()->GetLightmap();
  if (lmTex && lmTex != mLastLightmapTex) {
    mEncoder->setFragmentTexture(lmTex, 15);
    mEncoder->setVertexTexture(lmTex, 15);

    MtSamplerKey sk;
    sk.MinFilter = 1;
    sk.MagFilter = 1;
    sk.MipFilter = 0;
    sk.AddressU = 3;
    sk.AddressV = 3;
    sk.AddressW = 3;
    sk.MaxAnisotropy = 1.0f;

    MTL::SamplerState *sampler = fb->GetSamplerManager()->GetSamplerState(sk);
    if (sampler) {
      mEncoder->setFragmentSamplerState(sampler, 15);
      mEncoder->setVertexSamplerState(sampler, 15);
    }
    mLastLightmapTex = lmTex;
  }
}

void MtRenderState::ApplyRenderPass(int dt) {
  // Create render encoder if needed
  bool inRenderPass = (mEncoder != nullptr);

  // CRITICAL: If the engine requested a clear, we MUST end the current pass
  // to allow the next BeginRenderPass to set the LoadAction to Clear.
  if (inRenderPass && mClearTargets != 0) {
    EndRenderPass();
    inRenderPass = false;
  }

  if (!inRenderPass) {
    BeginRenderPass();
    mScissorChanged = true;
    mViewportChanged = true;
    mStencilRefChanged = true;
    mCullModeChanged = true;
    mBias.mChanged = true;
  }

  // Build pipeline key from current state
  MtPipelineKey pipelineKey;
  // Get vertex format and stride from current vertex buffer
  auto mtVBuf = dynamic_cast<MtVertexBuffer *>(mVertexBuffer);
  int stride = mtVBuf ? (int)mtVBuf->GetStride() : 0;
  pipelineKey.VertexFormat = mtVBuf ? mtVBuf->VertexFormat : -1;
  // Add stride to the key to differentiate between FFlatVertex (32) and
  // TwoDVertex (24)
  pipelineKey.VertexFormat |= (stride << 8);

  // Build shader key from effect state (following Vulkan pattern)
  pipelineKey.EffectState =
      mMaterial.mOverrideShader >= 0
          ? mMaterial.mOverrideShader
          : (mMaterial.mMaterial ? mMaterial.mMaterial->GetShaderIndex() : 0);
  if (!mTextureEnabled)
    pipelineKey.EffectState = SHADER_NoTexture;
  pipelineKey.SpecialEffect = mSpecialEffect;
  pipelineKey.AlphaTest = (mAlphaThreshold >= 0.f) ? 1 : 0;

  pipelineKey.DepthFunc = mDepthTest ? mDepthFunc : DF_Always;
  pipelineKey.DepthWrite = mDepthWrite ? 1 : 0;
  pipelineKey.DepthClampMode = mDepthClamp ? 1 : 0;
  pipelineKey.ColorMask = mColorMask;
  pipelineKey.CullMode = mCullMode;
  pipelineKey.StencilTest = mStencilTest ? 1 : 0;
  pipelineKey.StencilOp = mStencilOp;
  pipelineKey.StencilFunc =
      mStencilTest ? mStencilFunc : 2; // Force Always if test is off
  pipelineKey.BlendMode =
      mRenderStyle.AsDWORD; // Use full render style for pipeline key
  pipelineKey.SampleCount = mRenderTarget.Samples;
  pipelineKey.DrawBufferCount = mRenderTarget.DrawBuffers;
  pipelineKey.PixelFormat = mRenderTarget.Format;
  pipelineKey.DepthStencilFormat =
      mRenderTarget.DepthStencil
          ? (int)mRenderTarget.DepthStencil->pixelFormat()
          : 0;
  pipelineKey.ClipDistanceMask = mClipDistanceMask;

  // Only update pipeline state if key changed or not bound
  if (pipelineKey != mPipelineKey || !mPipelineBound) {
    fb->GetDebugManager()->RecordStateChange("pipeline");
    auto pipelineState =
        fb->GetPipelineStateManager()->GetPipelineState(pipelineKey, mtVBuf);
    if (pipelineState && pipelineState->pipelineState) {
      if (mEncoder) {
        mEncoder->setRenderPipelineState(pipelineState->pipelineState);
        mEncoder->setDepthStencilState(pipelineState->depthStencilState);
        mPipelineBound = true;
      }
      mPipelineKey = pipelineKey;
    }
  }
}

void MtRenderState::ApplyStencilRef() {
  if (mStencilRefChanged && mEncoder) {
    mEncoder->setStencilReferenceValue(mStencilRef);
    mStencilRefChanged = false;
  }
}

void MtRenderState::ApplyScissor() {
  if (mScissorChanged && mEncoder) {
    MTL::ScissorRect scissor;
    if (mScissorWidth >= 0) {
      // CONTEXT-AWARE SCALING: Use logical dimensions as divider
      double scaleX = (double)mRenderTarget.Width / (double)mVirtualWidth;
      double scaleY = (double)mRenderTarget.Height / (double)mVirtualHeight;

      int x0 = (int)clamp((double)mScissorX * scaleX, 0.0, (double)mRenderTarget.Width);
      int y0 = (int)clamp((double)mScissorY * scaleY, 0.0, (double)mRenderTarget.Height);
      int w0 = (int)((double)mScissorWidth * scaleX);
      int h0 = (int)((double)mScissorHeight * scaleY);

      scissor.x = (NS::UInteger)x0;
      scissor.y = (NS::UInteger)y0;
      scissor.width = (NS::UInteger)clamp(w0, 0, (int)mRenderTarget.Width - x0);
      scissor.height = (NS::UInteger)clamp(h0, 0, (int)mRenderTarget.Height - y0);
    } else {
      // Negative width/height means disable scissor (full screen)
      scissor.x = 0;
      scissor.y = 0;
      scissor.width = (NS::UInteger)mRenderTarget.Width;
      scissor.height = (NS::UInteger)mRenderTarget.Height;
    }
    mEncoder->setScissorRect(scissor);
    mScissorChanged = false;
  }
}

void MtRenderState::ApplyDepthBias() {
  if (mBias.mChanged && mEncoder) {
    // REVERSE-Z: Near is 1.0, Far is 0.0.
    // GZDoom sends negative bias values (OpenGL style) to pull closer.
    // We negate these to push towards 1.0 in Metal.
    // For Depth32Float, the constant bias 'units' must be scaled by r (min
    // resolvable difference). We use 1/2^24 as a reasonable epsilon for 32-bit
    // float buffers to simulate 24-bit precision.
    float unitsScale = 1.0f / 16777216.0f;
    mCurrentBiasUnits = -mBias.mUnits * unitsScale * 128.0f; // Scale up for Reverse-Z precision
    mCurrentBiasFactor = -mBias.mFactor;
    mEncoder->setDepthBias(mCurrentBiasUnits, mCurrentBiasFactor, 0.0f);
    mBias.mChanged = false;
  }
}

void MtRenderState::ApplyViewport() {
  if (mViewportChanged && mEncoder) {
    MTL::Viewport viewport;
    if (mViewportWidth >= 0) {
      // CONTEXT-AWARE SCALING: Use logical dimensions as divider
      double scaleX = (double)mRenderTarget.Width / (double)mVirtualWidth;
      double scaleY = (double)mRenderTarget.Height / (double)mVirtualHeight;

      viewport.originX = (double)mViewportX * scaleX;
      viewport.originY = (double)mViewportY * scaleY;
      viewport.width = (double)mViewportWidth * scaleX;
      viewport.height = (double)mViewportHeight * scaleY;
    } else {
      viewport.originX = 0.0;
      viewport.originY = 0.0;
      viewport.width = (double)mRenderTarget.Width;
      viewport.height = (double)mRenderTarget.Height;
    }
    // REVERSE-Z: Map engine range [min, max] to [1-max, 1-min]
    viewport.znear = 1.0 - mViewportDepthMax;
    viewport.zfar = 1.0 - mViewportDepthMin;
    mEncoder->setViewport(viewport);
    mViewportChanged = false;
  }
}

void MtRenderState::ApplyStreamData() {
  auto passManager = fb->GetPipelineStateManager();

  // Set useVertexData from vertex format
  mStreamData.useVertexData =
      (mVertexBuffer &&
       static_cast<MtVertexBuffer *>(mVertexBuffer)->HasColor())
          ? 1
          : 0;

  if (mMaterial.mMaterial && mMaterial.mMaterial->Source())
  {
    double time = I_msTimeFS();
    mStreamData.timer = static_cast<float>(
        time * (double)mMaterial.mMaterial->Source()->GetShaderSpeed() / 1000.);
  }
  else
    mStreamData.timer = 0.0f;

  if (mStreamBufferWriter.Write(mStreamData)) {
    // Write was successful (either skipped due to same data or updated)
  } else {
    WaitForStreamBuffers();
    mStreamBufferWriter.Write(mStreamData);
  }
}

void MtRenderState::ApplyPushConstants() {
  if (!mEncoder)
    return;

  BuildPushConstants();

  if (mPushConstants != mLastPushConstants) {
    mEncoder->setVertexBytes(&mPushConstants, sizeof(PushConstants), 21);
    mEncoder->setFragmentBytes(&mPushConstants, sizeof(PushConstants), 21);
    mLastPushConstants = mPushConstants;
  }
}

void MtRenderState::ApplyMatrices() {
  if (mMatrixBufferWriter.Write(mModelMatrix, mModelMatrixEnabled,
                                 mTextureMatrix, mTextureMatrixEnabled)) {
    // Write was successful
  } else {
    WaitForStreamBuffers();
    mMatrixBufferWriter.Write(mModelMatrix, mModelMatrixEnabled, mTextureMatrix,
                              mTextureMatrixEnabled);
  }
}

void MtRenderState::ApplyVertexBuffers() {
  if (!mEncoder)
    return;

  if ((mVertexBuffer != mLastVertexBuffer ||
       mVertexOffsets[0] != mLastVertexOffsets[0] ||
       mVertexOffsets[1] != mLastVertexOffsets[1]) &&
      mVertexBuffer) {
    auto mtbuf = dynamic_cast<MtVertexBuffer *>(mVertexBuffer);
    if (mtbuf) {
      size_t stride = mtbuf->GetStride();
      MTL::Buffer *buffer = mtbuf->GetBuffer();

      if (buffer) {
        mEncoder->setVertexBuffer(buffer, mVertexOffsets[0] * stride, 0);
        mEncoder->setVertexBuffer(buffer, mVertexOffsets[1] * stride, 1);
      }
    }

    mLastVertexBuffer = mVertexBuffer;
    mLastVertexOffsets[0] = mVertexOffsets[0];
    mLastVertexOffsets[1] = mVertexOffsets[1];
  }

  if (mIndexBuffer != mLastIndexBuffer && mIndexBuffer) {
    mLastIndexBuffer = mIndexBuffer;
  }
}

void MtRenderState::ApplyMaterial() {
  if (!mEncoder)
    return;

  if (mMaterial.mChanged) {
    if (mMaterial.mMaterial) {
      if (mMaterial.mMaterial->Source()->isHardwareCanvas())
        static_cast<FCanvasTexture *>(
            mMaterial.mMaterial->Source()->GetTexture())
            ->NeedUpdate();

      int numLayers = mMaterial.mMaterial->NumLayers();
      int scaleFlags = mMaterial.mMaterial->GetScaleFlags();
      bool isIndexed = (scaleFlags & CTF_Indexed) != 0;

      // For indexed textures, we must bind the palette layers (slots 1 and 2)
      if (isIndexed)
        numLayers = 3;

      for (int i = 0; i < numLayers; i++) {
        // Use the correct translation for palette layers
        int translation = (isIndexed && i > 0)
                              ? mMaterial.mTranslation
                              : ((i == 0) ? mMaterial.mTranslation : 0);
        MaterialLayerInfo *layerInfo = nullptr;
        auto hwTexture = mMaterial.mMaterial->GetLayer(i, translation, &layerInfo);
        if (hwTexture) {
          auto mtHwTexture = static_cast<MtHardwareTexture *>(hwTexture);
          auto image = mtHwTexture->GetImage();

          if (!image->GetTexture()) {
            FTexture *tex = (layerInfo && layerInfo->layerTexture)
                                ? layerInfo->layerTexture
                                : nullptr;
            if (!tex && i == 0)
              tex = mMaterial.mMaterial->Source()->GetTexture();

            if (tex) {
              mtHwTexture->CreateImage(
                  tex, translation,
                  layerInfo ? layerInfo->scaleFlags : scaleFlags);
            }
          }

          // Upload staging if needed
          if (mtHwTexture->NeedsUpload() &&
              mtHwTexture->GetStagingBufferSize() > 0 && image->GetTexture()) {
            MTL::Texture *mtlTexture = image->GetTexture();
            int w = image->GetWidth();
            int h = image->GetHeight();
            int pitch = mtHwTexture->GetBufferPitch();
            int texelsize = isIndexed ? 1 : (mtHwTexture->GetNumChannels());

            if (mtlTexture->storageMode() == MTL::StorageModePrivate) {
              // If we have an active render pass, we MUST end it before doing a blit
              if (mEncoder) {
                EndRenderPass();
              }

              size_t alignedPitch = (pitch + 1023) & ~1023;
              size_t totalSize = alignedPitch * h;
              MTL::Buffer *staging = fb->device->device->newBuffer(
                  totalSize, MTL::StorageModeShared);
              if (staging) {
                uint8_t *dst = (uint8_t *)staging->contents();
                const uint8_t *src = mtHwTexture->GetStagingBuffer();
                
                if (texelsize == 3) {
                    // RGB to BGRA conversion during upload to Private texture
                    for (int y = 0; y < h; y++) {
                        uint8_t* rowDst = dst + y * alignedPitch;
                        const uint8_t* rowSrc = src + y * pitch;
                        for (int x = 0; x < w; x++) {
                            rowDst[x*4 + 0] = rowSrc[x*3 + 2]; // B
                            rowDst[x*4 + 1] = rowSrc[x*3 + 1]; // G
                            rowDst[x*4 + 2] = rowSrc[x*3 + 0]; // R
                            rowDst[x*4 + 3] = 255;             // A
                        }
                    }
                } else {
                    for (int y = 0; y < h; y++) {
                      memcpy(dst + y * alignedPitch, src + y * pitch, pitch);
                    }
                }

                auto cmdBuf = fb->GetCommands()->GetBlitCommandBuffer();
                if (cmdBuf) {
                  auto blit = cmdBuf->blitCommandEncoder();
                  blit->copyFromBuffer(staging, 0, alignedPitch, 0,
                                       MTL::Size::Make(w, h, 1), mtlTexture, 0,
                                       0, MTL::Origin::Make(0, 0, 0));
                  if (mtlTexture->mipmapLevelCount() > 1) {
                    blit->generateMipmaps(mtlTexture);
                  }
                  blit->endEncoding();

                  // CRITICAL: Force synchronization for textures during startup or first frames
                  if (gamestate == GS_STARTUP || fb->GetFrameCount() < 100) {
                      cmdBuf->commit();
                      cmdBuf->waitUntilCompleted();
                  }
                }
                fb->RecycleBuffer(staging);
              }
            } else {
              // Managed/Shared: Direct upload via replaceRegion. 
              // No need to end render pass!
              MTL::Region region = MTL::Region::Make2D(0, 0, w, h);
              
              if (texelsize == 3) {
                  std::vector<uint8_t> temp(w * h * 4);
                  const uint8_t* src = mtHwTexture->GetStagingBuffer();
                  for (int j = 0; j < w * h; j++) {
                      temp[j*4 + 0] = src[j*3 + 2]; // B
                      temp[j*4 + 1] = src[j*3 + 1]; // G
                      temp[j*4 + 2] = src[j*3 + 0]; // R
                      temp[j*4 + 3] = 255;          // A
                  }
                  mtlTexture->replaceRegion(region, 0, temp.data(), w * 4);
              } else {
                  mtlTexture->replaceRegion(region, 0,
                                            mtHwTexture->GetStagingBuffer(), pitch);
              }

              if (mtlTexture->mipmapLevelCount() > 1) {
                // Mipmap generation DOES require a blit encoder, so check that
                if (mEncoder) {
                   EndRenderPass();
                }
                fb->GetTextureManager()->GenerateMipmaps(mtlTexture);
              }
            }

            // Ensure the renderer knows this texture is now filled
            MarkAsFilled(mtlTexture);

            // Clear staging buffer so we don't upload again
            mtHwTexture->ResetStagingBuffer();
          }

          MTL::Texture *mtlTexture = image->GetTexture();
          if (mtlTexture) {
            mEncoder->setFragmentTexture(mtlTexture, i);
            mEncoder->setVertexTexture(mtlTexture, i);

            // Sampler
            MtSamplerKey samplerKey;

            // Use the current draw category to determine sampler mode.
            // UI/HUD passes use nearest filtering; world geometry uses the
            // user's gl_texture_filter setting.
            bool isUI = (mCurrentDrawCategory[0] == 'h' || // "hud"
                         mCurrentDrawCategory[0] == 'u');  // "ui"

            int filter = isUI ? 0 : gl_texture_filter;
            // Vulkan-parity filter table: 0=Nearest, 1=Linear. Mip: 0=None,
            // 1=Nearest, 2=Linear
            static const int minFilters[] = {0, 0, 1, 1, 1, 0, 1};
            static const int magFilters[] = {0, 0, 1, 1, 1, 0, 0};
            static const int mipFilters[] = {0, 1, 0, 1, 2, 2, 2};

            int f = (filter >= 0 && filter <= 6) ? filter : 0;
            samplerKey.MinFilter = minFilters[f];
            samplerKey.MagFilter = magFilters[f];
            samplerKey.MipFilter = mipFilters[f];

            // CRITICAL: If texture has no mips, force sampler to NotMipmapped
            if (mtlTexture->mipmapLevelCount() == 1) {
              samplerKey.MipFilter = 0;
            }

            samplerKey.AddressU = mMaterial.mClampMode;
            samplerKey.AddressV = mMaterial.mClampMode;
            samplerKey.AddressW = mMaterial.mClampMode;
            samplerKey.MaxAnisotropy =
                isUI ? 1.0f : (float)gl_texture_filter_anisotropic;

            MTL::SamplerState *sampler =
                fb->GetSamplerManager()->GetSamplerState(samplerKey);
            if (sampler) {
              mEncoder->setFragmentSamplerState(sampler, i);
              mEncoder->setVertexSamplerState(sampler, i);
            }
          }
        }
      }
    }

    mMaterial.mChanged = false;
  }
}

void MtRenderState::SetInRenderTextureView(bool on) {
  if (mInRenderTextureView != on) {
    mInRenderTextureView = on;
    mCullModeChanged = true;
    mNeedApply = true;
  }
}

void MtRenderState::ApplyCulling() {
  if (mCullModeChanged && mEncoder) {
    if (mCullMode == Cull_None)
      mEncoder->setCullMode(MTL::CullModeNone);
    else if (mCullMode == Cull_CW)
      mEncoder->setCullMode(MTL::CullModeFront);
    else // Cull_CCW
      mEncoder->setCullMode(MTL::CullModeBack);

    mCullModeChanged = false;
  }
}

void MtRenderState::ApplyHWBufferSet() {
  if (!mEncoder)
    return;

  // Binding Indices (Matching shaderBindings in mt_shader.cpp)
  // Viewpoint = 17, Light = 16, Bone = 18, Matrix = 19, Stream = 20

  auto dummyBuf = fb->GetBufferManager()->DummyBuffer;

  // 1. Viewpoint (17)
  MTL::Buffer *vpBuf = mBoundBuffers[VIEWPOINT_BINDINGPOINT];
  if (!vpBuf && fb->GetBufferManager()->ViewpointUBO) {
    vpBuf = fb->GetBufferManager()->ViewpointUBO->GetBuffer();
  }
  uint32_t vpOff = mBoundOffsets[VIEWPOINT_BINDINGPOINT];
  if (!vpBuf) {
    vpBuf = dummyBuf;
    vpOff = 0;
  }

  if (vpBuf &&
      (vpBuf != mLastBoundBuffers[17] || vpOff != mLastBoundOffsets[17])) {
    mEncoder->setVertexBuffer(vpBuf, vpOff, 17);
    mEncoder->setFragmentBuffer(vpBuf, vpOff, 17);
    mLastBoundBuffers[17] = vpBuf;
    mLastBoundOffsets[17] = vpOff;
    mLastBoundFragmentBuffers[17] = vpBuf;
    mLastBoundFragmentOffsets[17] = vpOff;
  }

  // 2. Light (16)
  MTL::Buffer *ltBuf = mBoundBuffers[LIGHTBUF_BINDINGPOINT];
  if (!ltBuf && fb->GetBufferManager()->LightBufferSSO) {
    ltBuf = fb->GetBufferManager()->LightBufferSSO->GetBuffer();
  }
  uint32_t ltOff = mBoundOffsets[LIGHTBUF_BINDINGPOINT];
  if (!ltBuf) {
    ltBuf = dummyBuf;
    ltOff = 0;
  }

  if (ltBuf &&
      (ltBuf != mLastBoundBuffers[16] || ltOff != mLastBoundOffsets[16])) {
    mEncoder->setVertexBuffer(ltBuf, ltOff, 16);
    mEncoder->setFragmentBuffer(ltBuf, ltOff, 16);
    mLastBoundBuffers[16] = ltBuf;
    mLastBoundOffsets[16] = ltOff;
    mLastBoundFragmentBuffers[16] = ltBuf;
    mLastBoundFragmentOffsets[16] = ltOff;
  }

  // 3. Bone (18)
  MTL::Buffer *bnBuf = mBoundBuffers[BONEBUF_BINDINGPOINT];
  if (!bnBuf && fb->GetBufferManager()->BoneBufferSSO) {
    bnBuf = fb->GetBufferManager()->BoneBufferSSO->GetBuffer();
  }
  uint32_t bnOff = mBoundOffsets[BONEBUF_BINDINGPOINT];
  if (!bnBuf) {
    bnBuf = dummyBuf;
    bnOff = 0;
  }

  if (bnBuf &&
      (bnBuf != mLastBoundBuffers[18] || bnOff != mLastBoundOffsets[18])) {
    mEncoder->setVertexBuffer(bnBuf, bnOff, 18);
    mEncoder->setFragmentBuffer(bnBuf, bnOff, 18);
    mLastBoundBuffers[18] = bnBuf;
    mLastBoundOffsets[18] = bnOff;
    mLastBoundFragmentBuffers[18] = bnBuf;
    mLastBoundFragmentOffsets[18] = bnOff;
  }

  // 4. Matrix (Model/Texture) (19)
  uint32_t matrixOffset = mMatrixBufferWriter.Offset();
  MTL::Buffer *matrixBuffer = mMatrixBufferWriter.GetBuffer();
  if (matrixOffset == 0xffffffff) {
    matrixBuffer = dummyBuf;
    matrixOffset = 0;
  }

  if (matrixBuffer && (matrixBuffer != mLastBoundBuffers[19] ||
                       matrixOffset != mLastBoundOffsets[19])) {
    mEncoder->setVertexBuffer(matrixBuffer, matrixOffset, 19);
    mEncoder->setFragmentBuffer(matrixBuffer, matrixOffset, 19);
    mLastBoundBuffers[19] = matrixBuffer;
    mLastBoundOffsets[19] = matrixOffset;
    mLastBoundFragmentBuffers[19] = matrixBuffer;
    mLastBoundFragmentOffsets[19] = matrixOffset;
  }

  // 5. Stream (Per-draw uniforms) (20)
  uint32_t streamDataOffset = mStreamBufferWriter.StreamDataOffset();
  MTL::Buffer *streamBuffer = mStreamBufferWriter.GetBuffer();
  if (streamDataOffset == 0xffffffff) {
    streamBuffer = dummyBuf;
    streamDataOffset = 0;
  }

  if (streamBuffer && (streamBuffer != mLastBoundBuffers[20] ||
                       streamDataOffset != mLastBoundOffsets[20])) {
    mEncoder->setVertexBuffer(streamBuffer, streamDataOffset, 20);
    mEncoder->setFragmentBuffer(streamBuffer, streamDataOffset, 20);
    mLastBoundBuffers[20] = streamBuffer;
    mLastBoundOffsets[20] = streamDataOffset;
    mLastBoundFragmentBuffers[20] = streamBuffer;
    mLastBoundFragmentOffsets[20] = streamDataOffset;
  }
}

void MtRenderState::WaitForStreamBuffers() {
  auto stallStart = std::chrono::high_resolution_clock::now();

  EndRenderPass();
  fb->GetCommands()->FlushCommands(true);

  mApplyCount = 0;
  mStreamBufferWriter.BeginFrame();
  mMatrixBufferWriter.BeginFrame();

  BeginRenderPass();

  float stallMs = std::chrono::duration<float, std::milli>(
                      std::chrono::high_resolution_clock::now() - stallStart)
                      .count();
  if (fb->GetDebugManager())
    fb->GetDebugManager()->RecordStall("streambuffer", stallMs);
}

void MtRenderState::Bind(int bindingpoint, uint32_t offset) {
  BindBuffer(bindingpoint, nullptr, offset);
}

void MtRenderState::BindBuffer(int bindingpoint, MTL::Buffer *buffer,
                               uint32_t offset) {
  if (bindingpoint >= 0 && bindingpoint < 32) {
    mBoundBuffers[bindingpoint] = buffer;
    mBoundOffsets[bindingpoint] = offset;
    mNeedApply = true;

    // Legacy support for viewpoint member
    if (bindingpoint == VIEWPOINT_BINDINGPOINT) {
      mViewpointOffset = offset;
    }
  }
}

void MtRenderState::MarkAsFilled(MTL::Texture *tex) {
  if (tex) {
    mClearedTargets.insert(tex);
  }
}

void MtRenderState::BeginFrame() {
  mMaterial.Reset();
  mApplyCount = 0;
  mCurrentDrawCategory = "geometry";
  mIsFirstPass = true;
  mStencilFunc = 2; // DF_Always
  mStencilRef = 0;
  mStencilRefChanged = true;
  mViewportWidth = -1;
  mViewportHeight = -1;
  mScissorWidth = -1;
  mScissorHeight = -1;
  mClearedTargets.clear();
  mPipelineBound = false;

  mVirtualWidth = fb->GetWidth();
  mVirtualHeight = fb->GetHeight();

  mStreamBufferWriter.BeginFrame();
  mMatrixBufferWriter.BeginFrame();

  mBatchIndexOffset = 0;
  mBatchIBPointer = (uint32_t *)fb->GetBufferManager()->BatchIndexBuffer->Lock(1024 * 1024 * sizeof(uint32_t));
  mPendingBatch.active = false;

  mBias.mUnits = 0.0f;
  mBias.mFactor = 0.0f;
  mBias.mChanged = true;
  mCurrentBiasUnits = 0.0f;
  mCurrentBiasFactor = 0.0f;
  mCurrentWinding = MTL::WindingCounterClockwise;

  for (int i = 0; i < 32; i++) {
    mBoundBuffers[i] = nullptr;
    mBoundOffsets[i] = 0;
  }

  if (gamestate == GS_STARTUP) {
    mClearTargets |= CT_Color;
  }
}

void MtRenderState::EndRenderPass() {
  if (mEncoder) {
    FlushIndexedBatch(); // must come first — uses indexed pipeline while it's still bound
    FlushBatch();
    mEncoder->endEncoding();
    mEncoder = nullptr;
  }
}

void MtRenderState::EndFrame() {
  FlushIndexedBatch();
  FlushBatch();
  if (mBatchIBPointer) {
    fb->GetBufferManager()->BatchIndexBuffer->Unlock();
    mBatchIBPointer = nullptr;
  }
  mMatrixBufferWriter.Reset();
  mStreamBufferWriter.Reset();
  mRenderTarget = {};
  mVertexBuffer = nullptr;
  mIndexBuffer = nullptr;
  mLastVertexBuffer = nullptr;
  mLastIndexBuffer = nullptr;
}

void MtRenderState::EnableDrawBuffers(int count, bool apply) {
  if (mRenderTarget.DrawBuffers != count) {
    EndRenderPass();
    mRenderTarget.DrawBuffers = count;
  }
}

void MtRenderState::SetRenderTarget(MTL::Texture *image,
                                    MTL::Texture *depthStencilView, int width,
                                    int height, int format, int samples) {
  // End current pass, but DON'T flush. Let Apply() or EndFrame handle it.
  EndRenderPass();

  bool isSwapChain = (image == nullptr);
  if (isSwapChain && fb->mCurrentDrawable) {
    image = (MTL::Texture *)fb->mCurrentDrawable->texture();
  }

  mRenderTarget.Image = image;
  mRenderTarget.IsSwapChain = isSwapChain;
  mRenderTarget.DepthStencil = depthStencilView;
  mRenderTarget.Width = width;
  mRenderTarget.Height = height;

  if (isSwapChain) {
    mVirtualWidth = max(fb->GetWidth(), 1);
    mVirtualHeight = max(fb->GetHeight(), 1);
  } else {
    mVirtualWidth = max(width, 1);
    mVirtualHeight = max(height, 1);
  }

  mRenderTarget.Format = image ? (int)image->pixelFormat() : format;
  mRenderTarget.Samples = samples;
}

// FORCE RECOMPILE: December 26 V10 Diagnostic Build
void MtRenderState::BeginRenderPass() {
  EndRenderPass(); // Ensure previous encoder is ended

  MTL::Texture *targetTex = mRenderTarget.Image;
  if (mRenderTarget.IsSwapChain && fb->mCurrentDrawable) {
    targetTex = (MTL::Texture *)fb->mCurrentDrawable->texture();
  }

  if (!targetTex) {
    return;
  }

  if (mRenderTarget.Width <= 0 || mRenderTarget.Height <= 0) {
    return;
  }

  // Always allocate a new descriptor to avoid stale state issues
  MTL::RenderPassDescriptor *pRPD = MTL::RenderPassDescriptor::renderPassDescriptor();
  
  // Clear previous attachments (not needed for new RPD, but good practice if we reverted)
  // ...
  for (int i = 0; i < 8; i++) {
      pRPD->colorAttachments()->object(i)->setTexture(nullptr);
  }
  pRPD->depthAttachment()->setTexture(nullptr);
  pRPD->stencilAttachment()->setTexture(nullptr);

  auto buffers = fb->GetBuffers();
  int numAttachments = mRenderTarget.DrawBuffers;
  // GZDoom extra attachments (Fog, Normal) only exist for the scene target.
  if (numAttachments > 1) {
      // Relaxed check: Only ensure the auxiliary buffers actually exist.
      // We trust mRenderTarget.DrawBuffers if the buffers are valid.
      if (!buffers || !buffers->SceneFog || !buffers->SceneFog->GetTexture() ||
          !buffers->SceneNormal || !buffers->SceneNormal->GetTexture()) {
          numAttachments = 1;
      }
  }

  for (int i = 0; i < numAttachments; i++) {
    auto colorAtt = pRPD->colorAttachments()->object(i);
    MTL::Texture *tex = (i == 0)              ? targetTex
                        : (i == 1 && buffers) ? buffers->SceneFog->GetTexture()
                        : (i == 2 && buffers)
                            ? buffers->SceneNormal->GetTexture()
                            : nullptr;

    if (!tex)
      continue;
    colorAtt->setTexture(tex);

    bool filled = mClearedTargets.find(tex) != mClearedTargets.end();
    bool clear = (mClearTargets & CT_Color) || !filled;

    colorAtt->setLoadAction(clear ? MTL::LoadActionClear : MTL::LoadActionLoad);
    colorAtt->setStoreAction(MTL::StoreActionStore);

    if (clear) {
      MTL::ClearColor cc;
      if (i == 0) {
        if (targetTex == fb->GetBuffers()->ShadowMap->GetTexture()) {
          cc = MTL::ClearColor::Make(1e6, 0.0, 0.0, 1.0);
        } else {
          cc = MTL::ClearColor::Make(
              screen->mSceneClearColor[0], screen->mSceneClearColor[1],
              screen->mSceneClearColor[2], screen->mSceneClearColor[3]);
        }
      } else {
        cc = MTL::ClearColor::Make(0, 0, 0, 0); // Fog and Normal clear to zero
      }
      colorAtt->setClearColor(cc);
      mClearedTargets.insert(tex);
    }
  }

  // Depth/Stencil Attachment
  if (mRenderTarget.DepthStencil) {
    bool dsFilled = mClearedTargets.find(mRenderTarget.DepthStencil) !=
                    mClearedTargets.end();
    bool clearDS = !dsFilled;

    bool clearDepth = (mClearTargets & CT_Depth) || clearDS;
    bool clearStencil = (mClearTargets & CT_Stencil) || clearDS;

    auto depthAttachment = pRPD->depthAttachment();
    depthAttachment->setTexture(mRenderTarget.DepthStencil);
    depthAttachment->setLoadAction(clearDepth ? MTL::LoadActionClear
                                              : MTL::LoadActionLoad);

    // Depth store action:
    // - Memoryless textures (Apple Silicon PipelineDepthStencil) MUST use
    //   DontCare — there is no backing store to write to.
    // - SceneDepthStencil is sampled by SSAO/PostProcess so must be Store.
    // Use the texture's storage mode to decide automatically.
    bool depthIsMemoryless = (mRenderTarget.DepthStencil->storageMode() ==
                              MTL::StorageModeMemoryless);
    depthAttachment->setStoreAction(depthIsMemoryless ? MTL::StoreActionDontCare
                                                      : MTL::StoreActionStore);

    bool isShadowPass = false;
    auto shadowMap = fb->GetBuffers()->ShadowMap.get();
    if (shadowMap && mRenderTarget.Image == shadowMap->GetTexture()) {
        isShadowPass = true;
    }

    if (clearDepth) {
      // REVERSE-Z: Scene clears to 0.0 (Far). 
      // SHADOW PASS: Clears to 1.0 (Far).
      depthAttachment->setClearDepth(isShadowPass ? 1.0 : 0.0);
      mClearedTargets.insert(mRenderTarget.DepthStencil);
    }

    auto stencilAttachment = pRPD->stencilAttachment();
    stencilAttachment->setTexture(mRenderTarget.DepthStencil);
    stencilAttachment->setLoadAction(clearStencil ? MTL::LoadActionClear
                                                  : MTL::LoadActionLoad);

    // Stencil store action:
    // - If depth is memoryless the stencil component is too — DontCare.
    // - On TBDR (Apple Silicon), stencil is cleared at the start of every
    //   frame and never sampled across frames, so DontCare avoids a writeback.
    // - On Intel/AMD/NVIDIA, stencil shares the packed Depth32Stencil8 texture
    //   and must be Stored to protect the depth component.
    bool stencilDontCare = depthIsMemoryless || fb->mVersionManager.isTBDR;
    stencilAttachment->setStoreAction(stencilDontCare ? MTL::StoreActionDontCare
                                                      : MTL::StoreActionStore);

    if (clearStencil) {
      stencilAttachment->setClearStencil(0);
      mClearedTargets.insert(mRenderTarget.DepthStencil);
    }
  }

  // Consume clear flags now that we've set up the pass
  mClearTargets = 0;
  mIsFirstPass = false;

  MTL::CommandBuffer *cmdBuffer = fb->GetCommands()->GetRenderCommandBuffer();
  if (cmdBuffer) {
    mEncoder = cmdBuffer->renderCommandEncoder(pRPD);
    fb->GetResourceBindingManager()->ResetEncoderState();
    mLastResidentIndexBuffer = nullptr;
    mPassCount++;
    mPipelineBound = false;
    mLastShadowMapTex = nullptr;
    mLastLightmapTex = nullptr;
    if (mEncoder) {
      // Vertex-Flip Fix: Because we negated gl_Position.y in the shader,
      // the front-facing winding is now Clockwise.
      mCurrentWinding = mMirror ? MTL::WindingCounterClockwise : MTL::WindingClockwise;
      mEncoder->setFrontFacingWinding(mCurrentWinding);

      // Satisfy vertex descriptor aliases
      if (fb->GetBufferManager()->DummyBuffer) {
        mEncoder->setVertexBuffer(fb->GetBufferManager()->DummyBuffer, 0, 30);
        mEncoder->useResource(fb->GetBufferManager()->DummyBuffer, MTL::ResourceUsageRead, MTL::RenderStageVertex);
      }

      // Use current state if set, otherwise default to full target
      MTL::Viewport viewport;
      if (mViewportWidth > 0) {
        double scaleX = (double)mRenderTarget.Width / (double)mVirtualWidth;
        double scaleY = (double)mRenderTarget.Height / (double)mVirtualHeight;

        viewport.originX = (double)mViewportX * scaleX;
        viewport.originY = (double)mViewportY * scaleY;
        viewport.width = (double)mViewportWidth * scaleX;
        viewport.height = (double)mViewportHeight * scaleY;
      } else {
        viewport.originX = 0;
        viewport.originY = 0;
        viewport.width = (double)mRenderTarget.Width;
        viewport.height = (double)mRenderTarget.Height;
      }
      // REVERSE-Z: Map engine range [min, max] directly to [min, max]
      // Our shader already inverts Z to [1, 0]. Viewport range [0, 1] preserves this.
      viewport.znear = mViewportDepthMin;
      viewport.zfar = mViewportDepthMax;
      mEncoder->setViewport(viewport);

      MTL::ScissorRect scissor;
      if (mScissorWidth > 0) {
        double scaleX = (double)mRenderTarget.Width / (double)mVirtualWidth;
        double scaleY = (double)mRenderTarget.Height / (double)mVirtualHeight;

        scissor.x = (NS::UInteger)clamp((double)mScissorX * scaleX, 0.0, (double)mRenderTarget.Width);
        scissor.y = (NS::UInteger)clamp((double)mScissorY * scaleY, 0.0, (double)mRenderTarget.Height);
        scissor.width = (NS::UInteger)clamp((double)mScissorWidth * scaleX, 0.0, (double)mRenderTarget.Width - (double)scissor.x);
        scissor.height = (NS::UInteger)clamp((double)mScissorHeight * scaleY, 0.0, (double)mRenderTarget.Height - (double)scissor.y);
      } else {
        scissor.x = 0;
        scissor.y = 0;
        scissor.width = (NS::UInteger)mRenderTarget.Width;
        scissor.height = (NS::UInteger)mRenderTarget.Height;
      }
      mEncoder->setScissorRect(scissor);

      // Force re-apply of vertex buffer if we have one
      if (mVertexBuffer) {
        auto mtbuf = dynamic_cast<MtVertexBuffer *>(mVertexBuffer);
        if (mtbuf) {
          size_t stride = mtbuf->GetStride();
          MTL::Buffer *buffer = mtbuf->GetBuffer();
          if (buffer) {
            mEncoder->setVertexBuffer(buffer, mVertexOffsets[0] * stride, 0);
            mEncoder->setVertexBuffer(buffer, mVertexOffsets[1] * stride, 1);
          }
        }
      }
    }
  }

  mMaterial.mChanged = true;
  mClearTargets = 0;
  mScissorChanged = false;
  mViewportChanged = false;
  mStencilRefChanged = true;
  mCullModeChanged = true;
  mBias.mChanged = true; // Ensure depth bias is re-applied to the new encoder
  mLastPushConstants = {};
  mPipelineKey = {};
  mPipelineBound = false;

  mLastVertexBuffer = nullptr;
  mLastIndexBuffer = nullptr;
  mLastViewpointOffset = 0xffffffff;
  mLastMatricesOffset = 0xffffffff;
  mLastStreamDataOffset = 0xffffffff;

  for (int i = 0; i < 32; i++) {
    mLastBoundBuffers[i] = nullptr;
    mLastBoundOffsets[i] = 0xffffffff;
    mLastBoundFragmentBuffers[i] = nullptr;
    mLastBoundFragmentOffsets[i] = 0xffffffff;
  }
}
