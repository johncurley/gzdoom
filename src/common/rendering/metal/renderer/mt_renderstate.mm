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
#include "metal/renderer/mt_pipelinestate.h"
#include "metal/renderer/mt_renderbuffers.h"
#include "metal/system/mt_buffer.h"
#include "metal/system/mt_commandbuffer.h"
#include "metal/system/mt_renderdevice.h"
#include "metal/textures/mt_sampler.h"
#include "metal/textures/mt_texture.h"

#include "flatvertices.h"
#include "hw_clock.h"
#include "hw_cvars.h"
#include "hw_lightbuffer.h"
#include "hw_skydome.h"
#include "hw_viewpointuniforms.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "hwrenderer/data/shaderuniforms.h"
#include "v_text.h"

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

CVAR(Int, mt_submit_size, 1000, 0);
EXTERN_CVAR(Bool, r_skipmats)
EXTERN_CVAR(Bool, mt_debug)

MtRenderState::MtRenderState(MetalRenderDevice *fb)
    : fb(fb), mStreamBufferWriter(fb), mMatrixBufferWriter(fb) {
  Reset();
}

void MtRenderState::ClearScreen() {
  if (mt_debug) {
    Printf("Metal: ClearScreen\n");
  }
  screen->mViewpoints->Set2D(*this, SCREENWIDTH, SCREENHEIGHT);
  SetColor(0, 0, 0);
  SetVertexBuffer(screen->mVertexData->GetBufferObjects().first, 0, 0);
  Apply(DT_TriangleStrip);

  if (mEncoder) {
    mEncoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangleStrip,
                             FFlatVertexBuffer::FULLSCREEN_INDEX, 4, 1);
  }
}

void MtRenderState::Draw(int dt, int index, int count, bool apply) {
  if (mt_debug) {
    Printf("Metal: Draw dt=%d index=%d count=%d apply=%d vbuf=%p stride=%zu\n", 
           dt, index, count, (int)apply, mVertexBuffer, 
           mVertexBuffer ? static_cast<MtVertexBuffer*>(mVertexBuffer)->GetStride() : 0);
  }
  if (dt == DT_TriangleFan) {
    IIndexBuffer *oldIndexBuffer = mIndexBuffer;
    mIndexBuffer = fb->GetBufferManager()->FanToTrisIndexBuffer.get();

    if (apply || mNeedApply)
      Apply(DT_Triangles);
    else
      ApplyVertexBuffers();

    if (mEncoder) {
      mEncoder->drawIndexedPrimitives(
          MTL::PrimitiveType::PrimitiveTypeTriangle, (count - 2) * 3,
          MTL::IndexType::IndexTypeUInt32,
          static_cast<MtIndexBuffer *>(mIndexBuffer)->GetBuffer(), 0, 1, index,
          0);
    }

    mIndexBuffer = oldIndexBuffer;
  } else {
    if (apply || mNeedApply)
      Apply(dt);

    if (mEncoder) {
      MTL::PrimitiveType type;
      switch (dt) {
      case DT_Points:
        type = MTL::PrimitiveType::PrimitiveTypePoint;
        break;
      case DT_Lines:
        type = MTL::PrimitiveType::PrimitiveTypeLine;
        break;
      case DT_Triangles:
        type = MTL::PrimitiveType::PrimitiveTypeTriangle;
        break;
      case DT_TriangleStrip:
        type = MTL::PrimitiveType::PrimitiveTypeTriangleStrip;
        break;
      default:
        type = MTL::PrimitiveType::PrimitiveTypeTriangle;
        break;
      }
      mEncoder->drawPrimitives(type, index, count, 1);
    }
  }
}

void MtRenderState::DrawIndexed(int dt, int index, int count, bool apply) {
  if (mt_debug) {
    Printf("Metal: DrawIndexed dt=%d index=%d count=%d apply=%d vbuf=%p stride=%zu\n", 
           dt, index, count, (int)apply, mVertexBuffer, 
           mVertexBuffer ? static_cast<MtVertexBuffer*>(mVertexBuffer)->GetStride() : 0);
  }
  if (apply || mNeedApply)
    Apply(dt);

  if (!mEncoder || !mIndexBuffer)
    return;

  auto mtlIB = static_cast<MtIndexBuffer *>(mIndexBuffer)->GetBuffer();
  if (!mtlIB)
    return;

  mEncoder->drawIndexedPrimitives(
      MTL::PrimitiveTypeTriangle, count, MTL::IndexTypeUInt32,
      mtlIB,
      index * 4); // Each index is 4 bytes (UInt32)
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

  if (flags != -1) {
    bool cmon = !(flags & SF_ColorMaskOff);
    SetColorMask(cmon, cmon, cmon, cmon); // don't write to the graphics buffer
    mDepthWrite = !(flags & SF_DepthMaskOff);
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
  ApplyRenderPass(dt);
  ApplyScissor();
  ApplyCulling();
  ApplyViewport();
  ApplyStencilRef();
  ApplyDepthBias();
  ApplyPushConstants();
  ApplyVertexBuffers();
  ApplyHWBufferSet();
  ApplyMaterial();
  mNeedApply = false;

  // drawcalls.Unclock();
}

void MtRenderState::ApplyRenderPass(int dt) {
  // Create render encoder if needed
  bool inRenderPass = (mEncoder != nullptr);

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
  int stride = mVertexBuffer ? static_cast<MtVertexBuffer *>(mVertexBuffer)->GetStride() : 0;
  pipelineKey.VertexFormat =
      mVertexBuffer ? static_cast<MtVertexBuffer *>(mVertexBuffer)->VertexFormat
                    : -1;
  // Add stride to the key to differentiate between FFlatVertex (32) and TwoDVertex (24)
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
  pipelineKey.ColorMask = mColorMask;
  pipelineKey.CullMode = mCullMode;
  pipelineKey.StencilTest = mStencilTest ? 1 : 0;
  pipelineKey.StencilOp = mStencilOp;
  pipelineKey.BlendMode = mRenderStyle.AsDWORD; // Use full render style for pipeline key
  pipelineKey.SampleCount = mRenderTarget.Samples;
  pipelineKey.DrawBufferCount = mRenderTarget.DrawBuffers;
  pipelineKey.PixelFormat = mRenderTarget.Format;
  pipelineKey.DepthStencilFormat =
      mRenderTarget.DepthStencil ? (int)mRenderTarget.DepthStencil->pixelFormat()
                                 : 0;
  pipelineKey.ClipDistanceMask = mClipDistanceMask;

  // Only update pipeline state if key changed
  if (pipelineKey != mPipelineKey) {
    auto pipelineState = fb->GetPipelineStateManager()->GetPipelineState(
        pipelineKey, static_cast<MtVertexBuffer *>(mVertexBuffer));
    if (pipelineState && pipelineState->pipelineState) {
      if (mEncoder) {
        if (mt_debug) {
            Printf("Metal: Changed PipelineState effect=%d state=%d vfmt=%d\n", 
                   pipelineKey.SpecialEffect, pipelineKey.EffectState, pipelineKey.VertexFormat);
        }
        mEncoder->setRenderPipelineState(pipelineState->pipelineState);
        mEncoder->setDepthStencilState(pipelineState->depthStencilState);
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
      int x0 = clamp(mScissorX, 0, mRenderTarget.Width);
      int y0 = clamp(mScissorY, 0, mRenderTarget.Height);
      int x1 = clamp(mScissorX + mScissorWidth, 0, mRenderTarget.Width);
      int y1 = clamp(mScissorY + mScissorHeight, 0, mRenderTarget.Height);

      scissor.x = (NS::UInteger)x0;
      scissor.y = (NS::UInteger)y0;
      scissor.width = (NS::UInteger)max(0, x1 - x0);
      scissor.height = (NS::UInteger)max(0, y1 - y0);
    } else {
      scissor.x = 0;
      scissor.y = 0;
      scissor.width = (NS::UInteger)mRenderTarget.Width;
      scissor.height = (NS::UInteger)mRenderTarget.Height;
    }
    if (mt_debug) {
        Printf("Metal: ApplyScissor %lu,%lu %lu x %lu\n", (unsigned long)scissor.x, (unsigned long)scissor.y, (unsigned long)scissor.width, (unsigned long)scissor.height);
    }
    mEncoder->setScissorRect(scissor);
    mScissorChanged = false;
  }
}

void MtRenderState::ApplyDepthBias() {
  if (mBias.mChanged && mEncoder) {
    mEncoder->setDepthBias(mBias.mUnits, 0.0f, mBias.mFactor);
    mBias.mChanged = false;
  }
}

void MtRenderState::ApplyViewport() {
  if (mViewportChanged && mEncoder) {
    MTL::Viewport viewport;
    if (mViewportWidth >= 0) {
                      viewport.originX = (double)mViewportX;
                      viewport.originY = (double)mViewportY;
                      viewport.width = (double)mViewportWidth;
                      viewport.height = (double)mViewportHeight;
                      } else {
                        viewport.originX = 0.0;
                        viewport.originY = 0.0;
                        viewport.width = (double)mRenderTarget.Width;
                        viewport.height = (double)mRenderTarget.Height;
                      }    viewport.znear = mViewportDepthMin;
    viewport.zfar = mViewportDepthMax;
    if (mt_debug) {
        Printf("Metal: ApplyViewport %f,%f %f x %f range %f..%f\n", viewport.originX, viewport.originY, viewport.width, viewport.height, viewport.znear, viewport.zfar);
    }
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
    mStreamData.timer = static_cast<float>(
        (double)(screen->FrameTime - firstFrame) *
        (double)mMaterial.mMaterial->Source()->GetShaderSpeed() / 1000.);
  else
    mStreamData.timer = 0.0f;

  if (!mStreamBufferWriter.Write(mStreamData)) {
    WaitForStreamBuffers();
    mStreamBufferWriter.Write(mStreamData);
  }
}

void MtRenderState::ApplyPushConstants() {
  if (!mEncoder)
    return;

  int fogset = 0;
  if (mFogEnabled) {
    if (mFogEnabled == 2) {
      fogset = -3; // 2D rendering with 'foggy' overlay.
    } else if ((GetFogColor() & 0xffffff) == 0) {
      fogset = gl_fogmode;
    } else {
      fogset = -gl_fogmode;
    }
  }

  int tempTM = TM_NORMAL;
  if (mMaterial.mMaterial && mMaterial.mMaterial->Source()->isHardwareCanvas())
    tempTM = TM_OPAQUE;

  mPushConstants.uFogEnabled = fogset;
  mPushConstants.uTextureMode = GetTextureModeAndFlags(tempTM);
  mPushConstants.uLightDist = mLightParms[0];
  mPushConstants.uLightFactor = mLightParms[1];
  mPushConstants.uFogDensity = mLightParms[2];
  mPushConstants.uLightLevel = mLightParms[3];
  mPushConstants.uAlphaThreshold = mAlphaThreshold;
  mPushConstants.uClipSplit = {mClipSplit[0], mClipSplit[1]};

  if (mMaterial.mMaterial) {
    auto source = mMaterial.mMaterial->Source();
    mPushConstants.uSpecularMaterial = {source->GetGlossiness(),
                                        source->GetSpecularLevel()};
  }

  mPushConstants.uLightIndex = mLightIndex;
  mPushConstants.uBoneIndexBase = mBoneIndexBase;
  mPushConstants.uDataIndex = mStreamBufferWriter.DataIndex();

  if (mt_debug) {
      Printf("Metal: ApplyPushConstants (21) texMode=%d alpha=%f fog=%d lightLvl=%f dataIndex=%d\n", 
             mPushConstants.uTextureMode, mPushConstants.uAlphaThreshold, mPushConstants.uFogEnabled, mPushConstants.uLightLevel, mPushConstants.uDataIndex);
  }

  mEncoder->setVertexBytes(&mPushConstants, sizeof(PushConstants), 21);
  mEncoder->setFragmentBytes(&mPushConstants, sizeof(PushConstants), 21);
}

void MtRenderState::ApplyMatrices() {
  if (!mMatrixBufferWriter.Write(mModelMatrix, mModelMatrixEnabled,
                                 mTextureMatrix, mTextureMatrixEnabled)) {
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
    auto mtbuf = static_cast<MtVertexBuffer *>(mVertexBuffer);
    size_t stride = mtbuf->GetStride();
    MTL::Buffer *buffer = mtbuf->GetBuffer();

    if (buffer) {
        mEncoder->setVertexBuffer(buffer, mVertexOffsets[0] * stride, 0);
        mEncoder->setVertexBuffer(buffer, mVertexOffsets[1] * stride, 1);
        if (mt_debug) {
            Printf("Metal: Bound VertexBuffer %p at offsets %d,%d (stride %zu)\n", 
                   buffer, mVertexOffsets[0], mVertexOffsets[1], stride);
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
      for (int i = 0; i < numLayers; i++) {
        auto hwTexture =
            mMaterial.mMaterial->GetLayer(i, mMaterial.mTranslation);
        if (hwTexture) {
          auto mtHwTexture = static_cast<MtHardwareTexture *>(hwTexture);
          auto image = mtHwTexture->GetImage();

          if (!image->GetTexture()) {
            auto tex = mMaterial.mMaterial->Source();
            if (tex) {
              mtHwTexture->CreateImage(tex->GetTexture(),
                                       mMaterial.mTranslation,
                                       mMaterial.mMaterial->GetScaleFlags());
            }
          }

          // Upload staging if needed
          if (mtHwTexture->GetStagingBufferSize() > 0 && image->GetTexture()) {
            MTL::Texture *mtlTexture = image->GetTexture();
            int w = image->GetWidth();
            int h = image->GetHeight();
            int texelsize =
                (int)(mtHwTexture->GetStagingBufferSize() / (w * h));

            MTL::Region region = MTL::Region::Make2D(0, 0, w, h);
            mtlTexture->replaceRegion(
                region, 0, mtHwTexture->GetStagingBuffer(), w * texelsize);
          }

          MTL::Texture *mtlTexture = image->GetTexture();
          if (mtlTexture) {
            mEncoder->setFragmentTexture(mtlTexture, i);
            mEncoder->setVertexTexture(mtlTexture,
                                       i); // Bind to vertex too just in case

            if (mt_debug) {
                Printf("Metal: ApplyMaterial - Bound texture %p to slot %d (W:%lu H:%lu)\n", mtlTexture, i, mtlTexture->width(), mtlTexture->height());
            }

            // Sampler
            MtSamplerKey samplerKey;
            int filter = gl_texture_filter;
            static const int minFilters[] = {0, 1, 0, 1, 1};
            static const int magFilters[] = {0, 1, 0, 1, 1};
            static const int mipFilters[] = {0, 0, 0, 1, 1};

            int f = (filter >= 0 && filter <= 4) ? filter : 4;
            samplerKey.MinFilter = minFilters[f];
            samplerKey.MagFilter = magFilters[f];
            samplerKey.MipFilter = mipFilters[f];
            samplerKey.AddressU = mMaterial.mClampMode;
            samplerKey.AddressV = mMaterial.mClampMode;
            samplerKey.AddressW = mMaterial.mClampMode;
            samplerKey.MaxAnisotropy = gl_texture_filter_anisotropic;

            MTL::SamplerState *sampler =
                fb->GetSamplerManager()->GetSamplerState(samplerKey);
            if (sampler) {
              mEncoder->setFragmentSamplerState(sampler, i);
              mEncoder->setVertexSamplerState(sampler, i);
              if (mt_debug) {
                  Printf("Metal: ApplyMaterial - Bound sampler %p to slot %d\n", sampler, i);
              }
            }
          }
        }
      }
    }

    mMaterial.mChanged = false;
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

  if (mt_debug) Printf("Metal: ApplyHWBufferSet\n");

  // Binding Indices (Matching shaderBindings in mt_shader.cpp)
  // Viewpoint = 17, Light = 16, Bone = 18, Matrix = 19, Stream = 20, PushConstants = 21
  
  // 1. Viewpoint
  MTL::Buffer* vpBuf = mBoundBuffers[VIEWPOINT_BINDINGPOINT];
  uint32_t vpOff = mBoundOffsets[VIEWPOINT_BINDINGPOINT];
  if (vpBuf && (vpBuf != mLastBoundBuffers[VIEWPOINT_BINDINGPOINT] || vpOff != mLastBoundOffsets[VIEWPOINT_BINDINGPOINT])) {
      mEncoder->setVertexBuffer(vpBuf, vpOff, 17);
      mEncoder->setFragmentBuffer(vpBuf, vpOff, 17);
      mLastBoundBuffers[VIEWPOINT_BINDINGPOINT] = vpBuf;
      mLastBoundOffsets[VIEWPOINT_BINDINGPOINT] = vpOff;
      if (mt_debug) {
          float* m = (float*)((uint8_t*)vpBuf->contents() + vpOff);
          Printf("Metal: Bound Viewpoint (17) buf=%p off=%u. Matrix[0]=%.6f %.6f %.6f %.6f\n", vpBuf, vpOff, m[0], m[1], m[2], m[3]);
      }
  }

  // 2. Light
  MTL::Buffer* ltBuf = mBoundBuffers[LIGHTBUF_BINDINGPOINT];
  uint32_t ltOff = mBoundOffsets[LIGHTBUF_BINDINGPOINT];
  if (ltBuf && (ltBuf != mLastBoundBuffers[LIGHTBUF_BINDINGPOINT] || ltOff != mLastBoundOffsets[LIGHTBUF_BINDINGPOINT])) {
      mEncoder->setVertexBuffer(ltBuf, ltOff, 16);
      mEncoder->setFragmentBuffer(ltBuf, ltOff, 16);
      mLastBoundBuffers[LIGHTBUF_BINDINGPOINT] = ltBuf;
      mLastBoundOffsets[LIGHTBUF_BINDINGPOINT] = ltOff;
      if (mt_debug) Printf("Metal: Bound Light (16) buf=%p off=%u\n", ltBuf, ltOff);
  }

  // 3. Bone
  MTL::Buffer* bnBuf = mBoundBuffers[BONEBUF_BINDINGPOINT];
  uint32_t bnOff = mBoundOffsets[BONEBUF_BINDINGPOINT];
  if (bnBuf && (bnBuf != mLastBoundBuffers[BONEBUF_BINDINGPOINT] || bnOff != mLastBoundOffsets[BONEBUF_BINDINGPOINT])) {
      mEncoder->setVertexBuffer(bnBuf, bnOff, 18);
      mEncoder->setFragmentBuffer(bnBuf, bnOff, 18);
      mLastBoundBuffers[BONEBUF_BINDINGPOINT] = bnBuf;
      mLastBoundOffsets[BONEBUF_BINDINGPOINT] = bnOff;
      if (mt_debug) Printf("Metal: Bound Bone (18) buf=%p off=%u\n", bnBuf, bnOff);
  }

  // 4. Matrix (Model/Texture)
  uint32_t matrixOffset = mMatrixBufferWriter.Offset();
  MTL::Buffer *matrixBuffer = mMatrixBufferWriter.GetBuffer();
  if (matrixBuffer && (matrixBuffer != mLastBoundBuffers[14] || matrixOffset != mLastBoundOffsets[14])) {
    mEncoder->setVertexBuffer(matrixBuffer, matrixOffset, 19);
    mEncoder->setFragmentBuffer(matrixBuffer, matrixOffset, 19);
    mLastBoundBuffers[14] = matrixBuffer;
    mLastBoundOffsets[14] = matrixOffset;
    if (mt_debug) {
        float* m = (float*)((uint8_t*)matrixBuffer->contents() + matrixOffset);
        Printf("Metal: Bound Matrix (19) buf=%p off=%u. ModelMatrix[0]=%.6f %.6f %.6f %.6f\n", matrixBuffer, matrixOffset, m[0], m[1], m[2], m[3]);
    }
  }

  // 5. Stream (Per-draw uniforms)
  uint32_t streamDataOffset = mStreamBufferWriter.StreamDataOffset();
  MTL::Buffer *streamBuffer = mStreamBufferWriter.GetBuffer();
  if (streamBuffer && (streamBuffer != mLastBoundBuffers[15] || streamDataOffset != mLastBoundOffsets[15])) {
    mEncoder->setVertexBuffer(streamBuffer, streamDataOffset, 20);
    mEncoder->setFragmentBuffer(streamBuffer, streamDataOffset, 20);
    mLastBoundBuffers[15] = streamBuffer;
    mLastBoundOffsets[15] = streamDataOffset;
    if (mt_debug) {
        float* m = (float*)((uint8_t*)streamBuffer->contents() + streamDataOffset);
        Printf("Metal: Bound Stream (20) buf=%p off=%u. uObjectColor=%.6f %.6f %.6f %.6f\n", streamBuffer, streamDataOffset, m[0], m[1], m[2], m[3]);
    }
  }
}

void MtRenderState::WaitForStreamBuffers() {
  EndRenderPass();
  fb->GetCommands()->FlushCommands(true);
  
  mApplyCount = 0;
  mStreamBufferWriter.Reset();
  mMatrixBufferWriter.Reset();
  
  BeginRenderPass();
}

void MtRenderState::Bind(int bindingpoint, uint32_t offset) {
  BindBuffer(bindingpoint, nullptr, offset);
}

void MtRenderState::BindBuffer(int bindingpoint, MTL::Buffer *buffer,
                               uint32_t offset) {
  if (mt_debug) {
    Printf("Metal: BindBuffer bp=%d buffer=%p offset=%u\n", bindingpoint, buffer, offset);
  }
  if (bindingpoint >= 0 && bindingpoint < 16) {
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
    if (mt_debug) Printf("Metal: MarkAsFilled tex=%p\n", tex);
    mClearedTargets.insert(tex);
  }
}

void MtRenderState::BeginFrame() {
  mMaterial.Reset();
  mApplyCount = 0;
  mIsFirstPass = true;
  if (mt_debug) Printf("Metal: BeginFrame - Resetting mClearedTargets\n");
  mClearedTargets.clear();
}

void MtRenderState::EndRenderPass() {
  if (mEncoder) {
    if (mt_debug) {
      Printf("Metal: EndRenderPass encoder=%p\n", mEncoder);
    }
    mEncoder->endEncoding();
    mEncoder = nullptr;
    mApplyCount++;
  }
}

void MtRenderState::EndFrame() {
  mMatrixBufferWriter.Reset();
  mStreamBufferWriter.Reset();
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
  if (mt_debug) {
    Printf("Metal: SetRenderTarget image=%p ds=%p %dx%d fmt=%d samples=%d\n",
           image, depthStencilView, width, height, format, samples);
  }
  EndRenderPass();

  if (!image && fb->mCurrentDrawable) {
      image = (MTL::Texture*)fb->mCurrentDrawable->texture();
  }

  mRenderTarget.Image = image;
  mRenderTarget.DepthStencil = depthStencilView;
  mRenderTarget.Width = width;
  mRenderTarget.Height = height;
  mRenderTarget.Format = image ? (int)image->pixelFormat() : format;
  mRenderTarget.Samples = samples;
}

// FORCE RECOMPILE: December 25 V8 Final Audit Build
void MtRenderState::BeginRenderPass() {
  if (!mRenderTarget.Image) {
    if (mt_debug) Printf("Metal: BeginRenderPass - No render target image! width=%d height=%d\n", mRenderTarget.Width, mRenderTarget.Height);
    return;
  }
    
  if (mRenderTarget.Width <= 0 || mRenderTarget.Height <= 0) {
      if (mt_debug) Printf("Metal: BeginRenderPass - Invalid render target size %dx%d\n", mRenderTarget.Width, mRenderTarget.Height);
      return;
  }

  // Use autoreleased descriptor for maximum compatibility
  MTL::RenderPassDescriptor *pRPD = MTL::RenderPassDescriptor::renderPassDescriptor();
  if (!pRPD) return;

  if (mt_debug) {
    Printf("Metal: BeginRenderPass target=%p clearTargets=%d applyCount=%d desc=%p\n",
           mRenderTarget.Image, mClearTargets, mApplyCount, pRPD);
  }

  // Color Attachment
  auto colorAttachment = pRPD->colorAttachments()->object(0);
  colorAttachment->setTexture(mRenderTarget.Image);
  bool colorFilled = mClearedTargets.find(mRenderTarget.Image) != mClearedTargets.end();
  bool clearColor = (mClearTargets & CT_Color) || !colorFilled;
  
  colorAttachment->setLoadAction(clearColor ? MTL::LoadActionClear : MTL::LoadActionLoad);
  colorAttachment->setStoreAction(MTL::StoreActionStore);
  
  if (clearColor) {
      if (mt_debug) Printf("Metal: BeginRenderPass - Clearing color target %p\n", mRenderTarget.Image);
      mClearedTargets.insert(mRenderTarget.Image);
      colorAttachment->setClearColor(MTL::ClearColor::Make(
          screen->mSceneClearColor[0], screen->mSceneClearColor[1],
          screen->mSceneClearColor[2], screen->mSceneClearColor[3]));
  }


  // Depth/Stencil Attachment
  if (mRenderTarget.DepthStencil) {
    bool dsFilled = mClearedTargets.find(mRenderTarget.DepthStencil) != mClearedTargets.end();
    bool clearDS = !dsFilled; // Force clear on first use
    
    bool clearDepth = (mClearTargets & CT_Depth) || clearDS;
    bool clearStencil = (mClearTargets & CT_Stencil) || clearDS;

    auto depthAttachment = pRPD->depthAttachment();
    depthAttachment->setTexture(mRenderTarget.DepthStencil);
    depthAttachment->setLoadAction(clearDepth ? MTL::LoadActionClear : MTL::LoadActionLoad);
    depthAttachment->setStoreAction(MTL::StoreActionStore);
    if (clearDepth) {
        depthAttachment->setClearDepth(1.0);
        mClearedTargets.insert(mRenderTarget.DepthStencil);
    }

    auto stencilAttachment = pRPD->stencilAttachment();
    stencilAttachment->setTexture(mRenderTarget.DepthStencil);
    stencilAttachment->setLoadAction(clearStencil ? MTL::LoadActionClear : MTL::LoadActionLoad);
    stencilAttachment->setStoreAction(MTL::StoreActionStore);
    if (clearStencil) {
        stencilAttachment->setClearStencil(0);
        mClearedTargets.insert(mRenderTarget.DepthStencil);
    }
  }

  mIsFirstPass = false;

  MTL::CommandBuffer *cmdBuffer = fb->GetCommands()->GetRenderCommandBuffer();
  if (cmdBuffer) {
    if (mt_debug) Printf("Metal: renderCommandEncoder status=%d\n", (int)cmdBuffer->status());
    mEncoder = cmdBuffer->renderCommandEncoder(pRPD);
    if (mEncoder) {
        mEncoder->setFrontFacingWinding(MTL::WindingCounterClockwise);
        
        MTL::Viewport viewport;
        if (mViewportWidth >= 0) {
            viewport.originX = (double)mViewportX;
            viewport.originY = (double)mViewportY;
            viewport.width = (double)mViewportWidth;
            viewport.height = (double)mViewportHeight;
        } else {
            viewport.originX = 0;
            viewport.originY = 0;
            viewport.width = (double)mRenderTarget.Width;
            viewport.height = (double)mRenderTarget.Height;
        }
        viewport.znear = mViewportDepthMin;
        viewport.zfar = mViewportDepthMax;
        mEncoder->setViewport(viewport);
        
        MTL::ScissorRect scissor;
        if (mScissorWidth >= 0) {
            scissor.x = (NS::UInteger)clamp(mScissorX, 0, mRenderTarget.Width);
            scissor.y = (NS::UInteger)clamp(mScissorY, 0, mRenderTarget.Height);
            scissor.width = (NS::UInteger)clamp(mScissorWidth, 0, mRenderTarget.Width - (int)scissor.x);
            scissor.height = (NS::UInteger)clamp(mScissorHeight, 0, mRenderTarget.Height - (int)scissor.y);
        } else {
            scissor.x = 0;
            scissor.y = 0;
            scissor.width = (NS::UInteger)mRenderTarget.Width;
            scissor.height = (NS::UInteger)mRenderTarget.Height;
        }
        mEncoder->setScissorRect(scissor);
    }
  }

  mMaterial.mChanged = true;
  mClearTargets = 0;
  mScissorChanged = true;
  mViewportChanged = true;
  mStencilRefChanged = true;
  mCullModeChanged = true;
  mBias.mChanged = true;
  mPipelineKey = {};
  
  mLastVertexBuffer = nullptr;
  mLastIndexBuffer = nullptr;
  mLastViewpointOffset = 0xffffffff;
  mLastMatricesOffset = 0xffffffff;
  mLastStreamDataOffset = 0xffffffff;
  
  for (int i = 0; i < 16; i++) {
      mLastBoundBuffers[i] = nullptr;
      mLastBoundOffsets[i] = 0xffffffff;
  }
}
