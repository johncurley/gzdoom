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
#include "metal/system/mt_hwbuffer.h" // Needed for MtHardwareDataBuffer definition
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
#include "gamestate.h"

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
 
CVAR(Int, mt_submit_size, 4096, 0);
EXTERN_CVAR(Bool, r_skipmats)
EXTERN_CVAR(Bool, mt_debug)
MtRenderState::MtRenderState(MetalRenderDevice *fb)
    : fb(fb), mStreamBufferWriter(fb), mMatrixBufferWriter(fb) {
  mStencilFunc = 2; // DF_Always (Index 2 in GZDoom)
  mStencilRef = 0;
  Reset();
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
    mEncoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangleStrip,
                             FFlatVertexBuffer::FULLSCREEN_INDEX, 4, 1);
  }
}

void MtRenderState::Draw(int dt, int index, int count, bool apply) {
  if (mt_debug) {
    auto mtVBuf = dynamic_cast<MtVertexBuffer*>(mVertexBuffer);
    Printf(PRINT_LOG, "Metal: Draw dt=%d index=%d count=%d apply=%d vbuf=%p stride=%zu\n", 
           dt, index, count, (int)apply, mVertexBuffer, 
           mtVBuf ? mtVBuf->GetStride() : 0);
    
    if (mtVBuf && mtVBuf->GetStride() == 24 && count > 0) {
        float* v = (float*)((uint8_t*)mtVBuf->GetBuffer()->contents() + index * mtVBuf->GetStride());
        Printf(PRINT_LOG, "  2D Vert 0: Pos=(%f, %f, %f) UV=(%f, %f) Color=%08x\n", v[0], v[1], v[2], v[3], v[4], ((uint32_t*)v)[5]);
    }
  }
  if (dt == DT_TriangleFan) {
    IIndexBuffer *oldIndexBuffer = mIndexBuffer;
    mIndexBuffer = fb->GetBufferManager()->FanToTrisIndexBuffer.get();

    if (apply || mNeedApply)
      Apply(DT_Triangles);
    else
      ApplyVertexBuffers();

    auto mtIB = dynamic_cast<MtIndexBuffer *>(mIndexBuffer);
    if (mEncoder && mPipelineBound && mtIB && count >= 3) {
      auto mtlIB = mtIB->GetBuffer();
      if (mtlIB) {
          mEncoder->drawIndexedPrimitives(
              MTL::PrimitiveType::PrimitiveTypeTriangle, (NS::UInteger)((count - 2) * 3),
              MTL::IndexType::IndexTypeUInt32,
              mtlIB, 0, 1, (NS::Integer)index,
              0);
      }
    }

    mIndexBuffer = oldIndexBuffer;
  } else {
    if (apply || mNeedApply)
      Apply(dt);

    if (mEncoder && mPipelineBound) {
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
      if (count > 0) {
        mEncoder->drawPrimitives(type, index, count, 1);
        mApplyCount++;
      }
    }
  }
}

void MtRenderState::DrawIndexed(int dt, int index, int count, bool apply) {
  if (mt_debug) {
    auto mtVBuf = dynamic_cast<MtVertexBuffer*>(mVertexBuffer);
    Printf(PRINT_LOG, "Metal: DrawIndexed dt=%d index=%d count=%d apply=%d vbuf=%p stride=%zu\n", 
           dt, index, count, (int)apply, mVertexBuffer, 
           mtVBuf ? mtVBuf->GetStride() : 0);
    
    if (mtVBuf && mtVBuf->GetStride() == 24 && count > 0) {
        float* v = (float*)((uint8_t*)mtVBuf->GetBuffer()->contents() + mVertexOffsets[0] * mtVBuf->GetStride());
        Printf(PRINT_LOG, "  2D Vert 0: Pos=(%f, %f, %f) UV=(%f, %f) Color=%08x\n", v[0], v[1], v[2], v[3], v[4], ((uint32_t*)v)[5]);
    }
  }
  if (apply || mNeedApply)
    Apply(dt);

  if (!mEncoder || !mPipelineBound || !mIndexBuffer || count <= 0)
    return;

  auto mtIB = dynamic_cast<MtIndexBuffer *>(mIndexBuffer);
  if (!mtIB) return;
  auto mtlIB = mtIB->GetBuffer();
  if (!mtlIB)
    return;

  mEncoder->drawIndexedPrimitives(
      MTL::PrimitiveTypeTriangle, count, MTL::IndexTypeUInt32,
      mtlIB,
      index * 4); // Each index is 4 bytes (UInt32)
  mApplyCount++;
}

bool MtRenderState::SetDepthClamp(bool on) {
  if (mt_debug) fprintf(stderr, "Metal: SetDepthClamp %d\n", (int)on);
  bool lastValue = mDepthClamp;
  mDepthClamp = on;
  mNeedApply = true;
  return lastValue;
}

void MtRenderState::SetDepthMask(bool on) {
  if (mt_debug) fprintf(stderr, "Metal: SetDepthMask %d\n", (int)on);
  mDepthWrite = on;
  mNeedApply = true;
}

void MtRenderState::SetDepthFunc(int func) {
  if (mt_debug) fprintf(stderr, "Metal: SetDepthFunc %d\n", func);
  mDepthFunc = func;
  mNeedApply = true;
}

void MtRenderState::SetDepthRange(float min, float max) {
  if (mt_debug) fprintf(stderr, "Metal: SetDepthRange %f..%f\n", min, max);
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
  // We use our internal index 3 which maps to MTL::CompareFunctionEqual in stencilFuncs.
  mStencilFunc = 3; 

  if (mt_debug) fprintf(stderr, "Metal: SetStencil offset=%d op=%d ref=%d\n", offs, op, mStencilRef);

  if (flags != -1) {
    bool cmon = !(flags & SF_ColorMaskOff);
    SetColorMask(cmon, cmon, cmon, cmon);
    SetDepthMask(!(flags & SF_DepthMaskOff));
  }
  mNeedApply = true;
}

void MtRenderState::SetCulling(int mode) {
  if (mt_debug) fprintf(stderr, "Metal: SetCulling %d\n", mode);
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
  if (gamestate == GS_STARTUP) {
      fprintf(stderr, "Metal: Startup SetScissor %d,%d %dx%d\n", x, y, w, h);
  }
  mScissorX = x;
  mScissorY = y;
  mScissorWidth = w;
  mScissorHeight = h;
  mScissorChanged = true;
  mNeedApply = true;
}

void MtRenderState::SetViewport(int x, int y, int w, int h) {
  if (mt_debug) {
      fprintf(stderr, "Metal: SetViewport %d,%d %dx%d (gamestate=%d)\n", x, y, w, h, (int)gamestate);
  }
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
    if (mt_debug) Printf(PRINT_LOG, "Metal: mt_submit_size (%d) reached, flushing.\n", *mt_submit_size);
    EndRenderPass();
    fb->GetCommands()->FlushCommands();
    mApplyCount = 0;
  }

  ApplyStreamData();
    ApplyMatrices();
    ApplyRenderPass(dt);
    
    if (!mPipelineBound) return;
  
    ApplyScissor();
  ApplyCulling();
  
  // Implement Depth Clamping (Depth Clip Mode)
  if (mEncoder) {
      mEncoder->setDepthClipMode(mDepthClamp ? MTL::DepthClipModeClamp : MTL::DepthClipModeClip);
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

  // drawcalls.Unclock();
}

void MtRenderState::ApplyFixedTextures() {
  if (!mEncoder)
    return;

  auto buffers = fb->GetBuffers();
  if (buffers->ShadowMap && buffers->ShadowMap->GetTexture()) {
    mEncoder->setFragmentTexture(buffers->ShadowMap->GetTexture(), 14);
    mEncoder->setVertexTexture(buffers->ShadowMap->GetTexture(), 14);

    // Shadow map sampler: Nearest, Clamp
    MtSamplerKey sk;
    sk.MinFilter = 0;
    sk.MagFilter = 0;
    sk.MipFilter = 0;
    sk.AddressU = 2; // Clamp
    sk.AddressV = 2;
    sk.AddressW = 2;
    sk.MaxAnisotropy = 1.0f;

    auto sampler = fb->GetSamplerManager()->GetSamplerState(sk);
    if (sampler) {
      mEncoder->setFragmentSamplerState(sampler, 14);
      mEncoder->setVertexSamplerState(sampler, 14);
    }
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
  pipelineKey.VertexFormat =
      mtVBuf ? mtVBuf->VertexFormat
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
  pipelineKey.StencilFunc = mStencilTest ? mStencilFunc : 2; // Force Always if test is off
  pipelineKey.BlendMode = mRenderStyle.AsDWORD; // Use full render style for pipeline key
  pipelineKey.SampleCount = mRenderTarget.Samples;
  pipelineKey.DrawBufferCount = mRenderTarget.DrawBuffers;
  pipelineKey.PixelFormat = mRenderTarget.Format;
  pipelineKey.DepthStencilFormat =
      mRenderTarget.DepthStencil ? (int)mRenderTarget.DepthStencil->pixelFormat()
                                 : 0;
  pipelineKey.ClipDistanceMask = mClipDistanceMask;

  // Only update pipeline state if key changed or not bound
  if (pipelineKey != mPipelineKey || !mPipelineBound) {
    auto pipelineState = fb->GetPipelineStateManager()->GetPipelineState(
        pipelineKey, mtVBuf);
    if (pipelineState && pipelineState->pipelineState) {
      if (mEncoder) {
        if (mt_debug) {
            Printf(PRINT_LOG, "Metal: Changed PipelineState effect=%d state=%d vfmt=%d\n", 
                   pipelineKey.SpecialEffect, pipelineKey.EffectState, pipelineKey.VertexFormat);
        }
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
      int x0 = clamp(mScissorX, 0, mRenderTarget.Width);
      int y0 = clamp(mScissorY, 0, mRenderTarget.Height);
      int x1 = clamp(mScissorX + mScissorWidth, 0, mRenderTarget.Width);
      int y1 = clamp(mScissorY + mScissorHeight, 0, mRenderTarget.Height);

      scissor.x = (NS::UInteger)x0;
      scissor.y = (NS::UInteger)y0;
      scissor.width = (NS::UInteger)max(0, x1 - x0);
      scissor.height = (NS::UInteger)max(0, y1 - y0);
    } else {
      // Negative width/height means disable scissor (full screen)
      scissor.x = 0;
      scissor.y = 0;
      scissor.width = (NS::UInteger)mRenderTarget.Width;
      scissor.height = (NS::UInteger)mRenderTarget.Height;
    }
    if (mt_debug) {
        Printf(PRINT_LOG, "Metal: ApplyScissor %lu,%lu %lu x %lu\n", (unsigned long)scissor.x, (unsigned long)scissor.y, (unsigned long)scissor.width, (unsigned long)scissor.height);
    }
    mEncoder->setScissorRect(scissor);
    mScissorChanged = false;
  }
}

void MtRenderState::ApplyDepthBias() {
  if (mBias.mChanged && mEncoder) {
    // REVERSE-Z: Near is 1.0, Far is 0.0. 
    // GZDoom sends negative bias values (OpenGL style) to pull closer.
    // We negate these to push towards 1.0 in Metal, and scale for Depth32Float.
    float scale = 32.0f; 
    float units = -mBias.mUnits * scale;
    float factor = -mBias.mFactor * scale;
    if (mt_debug) {
        fprintf(stderr, "Metal: ApplyDepthBias units=%f (raw %f) factor=%f (raw %f) [Reverse-Z]\n", 
               units, mBias.mUnits, factor, mBias.mFactor);
    }
    mEncoder->setDepthBias(units, factor, 0.0f);
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
                      }    
    // REVERSE-Z: Map engine range [min, max] to [1-max, 1-min]
    viewport.znear = 1.0 - mViewportDepthMax;
    viewport.zfar = 1.0 - mViewportDepthMin;
    if (mt_debug) {
        fprintf(stderr, "Metal: ApplyViewport %f,%f %f x %f range %f..%f\n", viewport.originX, viewport.originY, viewport.width, viewport.height, viewport.znear, viewport.zfar);
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

  mPushConstants.uClipSplit = {mClipSplit[0], mClipSplit[1]};
  mPushConstants.uSpecularMaterial = {0.0f, 0.0f}; // Default
  if (mMaterial.mMaterial) {
    auto source = mMaterial.mMaterial->Source();
    mPushConstants.uSpecularMaterial = {source->GetGlossiness(),
                                        source->GetSpecularLevel()};
  }

  mPushConstants.uLightLevel = mLightParms[3];
  mPushConstants.uFogDensity = mLightParms[2];
  mPushConstants.uLightFactor = mLightParms[1];
  mPushConstants.uLightDist = mLightParms[0];

  mPushConstants.uTextureMode = GetTextureModeAndFlags(tempTM);
  mPushConstants.uAlphaThreshold = mAlphaThreshold;
  mPushConstants.uFogEnabled = fogset;
  mPushConstants.uLightIndex = mLightIndex;

  mPushConstants.uBoneIndexBase = mBoneIndexBase;
  mPushConstants.uDataIndex = mStreamBufferWriter.DataIndex();

  if (mt_debug) {
      auto mat = mMaterial.mMaterial;
      const char* matName = (mat && mat->Source()) ? mat->Source()->GetName().GetChars() : "none";
      Printf(PRINT_LOG, "Metal: ApplyPushConstants (21) texMode=%d alpha=%f fog=%d lightLvl=%f dataIndex=%d material=%s\n", 
             mPushConstants.uTextureMode, mPushConstants.uAlphaThreshold, mPushConstants.uFogEnabled, mPushConstants.uLightLevel, mPushConstants.uDataIndex, matName);
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
    auto mtbuf = dynamic_cast<MtVertexBuffer *>(mVertexBuffer);
    if (mtbuf) {
        size_t stride = mtbuf->GetStride();
        MTL::Buffer *buffer = mtbuf->GetBuffer();

        if (buffer) {
            mEncoder->setVertexBuffer(buffer, mVertexOffsets[0] * stride, 0);
            mEncoder->setVertexBuffer(buffer, mVertexOffsets[1] * stride, 1);
            if (mt_debug) {
                Printf(PRINT_LOG, "Metal: Bound VertexBuffer %p at offsets %d,%d (stride %zu)\n", 
                       buffer, mVertexOffsets[0], mVertexOffsets[1], stride);
            }
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
      if (isIndexed) numLayers = 3;

      for (int i = 0; i < numLayers; i++) {
        // Use the correct translation for palette layers
        int translation = (isIndexed && i > 0) ? mMaterial.mTranslation : ((i == 0) ? mMaterial.mTranslation : 0);
        auto hwTexture = mMaterial.mMaterial->GetLayer(i, translation);
        if (hwTexture) {
          auto mtHwTexture = static_cast<MtHardwareTexture *>(hwTexture);
          auto image = mtHwTexture->GetImage();

          if (!image->GetTexture()) {
            auto tex = mMaterial.mMaterial->Source();
            if (tex) {
              // Note: GetLayer for palettes handles its own creation usually, 
              // but we ensure the base layer has an image.
              if (i == 0) {
                  mtHwTexture->CreateImage(tex->GetTexture(),
                                           mMaterial.mTranslation,
                                           scaleFlags);
              }
            }
          }

          // Upload staging if needed
          if (mtHwTexture->NeedsUpload() && mtHwTexture->GetStagingBufferSize() > 0 && image->GetTexture()) {
            MTL::Texture *mtlTexture = image->GetTexture();
            int w = image->GetWidth();
            int h = image->GetHeight();
            int pitch = mtHwTexture->GetBufferPitch();

            MTL::Region region = MTL::Region::Make2D(0, 0, w, h);
            mtlTexture->replaceRegion(region, 0, mtHwTexture->GetStagingBuffer(), pitch);
            
            if (mtlTexture->mipmapLevelCount() > 1) {
                fb->GetTextureManager()->GenerateMipmaps(mtlTexture);
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

            // 2. Identify UI textures for special sampler handling
            const char* texName = mtHwTexture->GetDebugName().c_str();
            bool isUI = (strstr(texName, "STARTUP") || strstr(texName, "BOOTLOGO") || 
                         strstr(texName, "M_") || strstr(texName, "ST_") || strstr(texName, "WI_") ||
                         strstr(texName, "TITLE") || strstr(texName, "INTER") || 
                         strstr(texName, "HELP") || strstr(texName, "CREDIT") || strstr(texName, "CONBACK") ||
                         strstr(texName, "Font") || strstr(texName, "FONT") ||
                         mtlTexture->width() == 640); // 640 is common for old UI graphics

            // Sampler
            MtSamplerKey samplerKey;
            int filter = isUI ? 0 : gl_texture_filter; // Force Nearest for UI
            static const int minFilters[] = {0, 1, 0, 1, 1};
            static const int magFilters[] = {0, 1, 0, 1, 1};
            static const int mipFilters[] = {0, 0, 0, 2, 2}; // 0 = NotMipmapped, 2 = Linear

            int f = (filter >= 0 && filter <= 4) ? filter : 4;
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
            samplerKey.MaxAnisotropy = isUI ? 1.0f : gl_texture_filter_anisotropic;

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
    
    if (mt_debug) {
        fprintf(stderr, "Metal: ApplyCulling mode=%d\n", mCullMode);
    }
    mCullModeChanged = false;
  }
}

void MtRenderState::ApplyHWBufferSet() {
  if (!mEncoder)
    return;

  if (mt_debug) Printf(PRINT_LOG, "Metal: ApplyHWBufferSet\n");

  // Binding Indices (Matching shaderBindings in mt_shader.cpp)
  // Viewpoint = 17, Light = 16, Bone = 18, Matrix = 19, Stream = 20, PushConstants = 21
  
  // 1. Viewpoint
  MTL::Buffer* vpBuf = mBoundBuffers[VIEWPOINT_BINDINGPOINT];
  if (!vpBuf && fb->GetBufferManager()->ViewpointUBO) {
      vpBuf = fb->GetBufferManager()->ViewpointUBO->GetBuffer();
  }
  uint32_t vpOff = mBoundOffsets[VIEWPOINT_BINDINGPOINT];
  if (vpBuf && (vpBuf != mLastBoundBuffers[17] || vpOff != mLastBoundOffsets[17])) {
      mEncoder->setVertexBuffer(vpBuf, vpOff, 17);
      mEncoder->setFragmentBuffer(vpBuf, vpOff, 17);
      mLastBoundBuffers[17] = vpBuf;
      mLastBoundOffsets[17] = vpOff;
      if (mt_debug) {
          float* m = (float*)((uint8_t*)vpBuf->contents() + vpOff);
          Printf(PRINT_LOG, "Metal: Bound Viewpoint (17) buf=%p off=%u. Matrix[0]=%.6f %.6f %.6f %.6f\n", vpBuf, vpOff, m[0], m[1], m[2], m[3]);
      }
  }

  // 2. Light
  MTL::Buffer* ltBuf = mBoundBuffers[LIGHTBUF_BINDINGPOINT];
  if (!ltBuf && fb->GetBufferManager()->LightBufferSSO) {
      ltBuf = fb->GetBufferManager()->LightBufferSSO->GetBuffer();
  }
  uint32_t ltOff = mBoundOffsets[LIGHTBUF_BINDINGPOINT];
  if (ltBuf && (ltBuf != mLastBoundBuffers[16] || ltOff != mLastBoundOffsets[16])) {
      mEncoder->setVertexBuffer(ltBuf, ltOff, 16);
      mEncoder->setFragmentBuffer(ltBuf, ltOff, 16);
      mLastBoundBuffers[16] = ltBuf;
      mLastBoundOffsets[16] = ltOff;
      if (mt_debug) Printf(PRINT_LOG, "Metal: Bound Light (16) buf=%p off=%u\n", ltBuf, ltOff);
  }

  // 3. Bone
  MTL::Buffer* bnBuf = mBoundBuffers[BONEBUF_BINDINGPOINT];
  if (!bnBuf && fb->GetBufferManager()->BoneBufferSSO) {
      bnBuf = fb->GetBufferManager()->BoneBufferSSO->GetBuffer();
  }
  uint32_t bnOff = mBoundOffsets[BONEBUF_BINDINGPOINT];
  if (bnBuf && (bnBuf != mLastBoundBuffers[18] || bnOff != mLastBoundOffsets[18])) {
      mEncoder->setVertexBuffer(bnBuf, bnOff, 18);
      mEncoder->setFragmentBuffer(bnBuf, bnOff, 18);
      mLastBoundBuffers[18] = bnBuf;
      mLastBoundOffsets[18] = bnOff;
      if (mt_debug) Printf(PRINT_LOG, "Metal: Bound Bone (18) buf=%p off=%u\n", bnBuf, bnOff);
  }

  // 4. Matrix (Model/Texture)
  uint32_t matrixOffset = mMatrixBufferWriter.Offset();
  MTL::Buffer *matrixBuffer = mMatrixBufferWriter.GetBuffer();
  if (matrixBuffer && (matrixBuffer != mLastBoundBuffers[19] || matrixOffset != mLastBoundOffsets[19])) {
    mEncoder->setVertexBuffer(matrixBuffer, matrixOffset, 19);
    mEncoder->setFragmentBuffer(matrixBuffer, matrixOffset, 19);
    mLastBoundBuffers[19] = matrixBuffer;
    mLastBoundOffsets[19] = matrixOffset;
    if (mt_debug) {
        float* m = (float*)((uint8_t*)matrixBuffer->contents() + matrixOffset);
        Printf(PRINT_LOG, "Metal: Bound Matrix (19) buf=%p off=%u. ModelMatrix[0]=%.6f %.6f %.6f %.6f\n", matrixBuffer, matrixOffset, m[0], m[1], m[2], m[3]);
    }
  }

  // 5. Stream (Per-draw uniforms)
  uint32_t streamDataOffset = mStreamBufferWriter.StreamDataOffset();
  MTL::Buffer *streamBuffer = mStreamBufferWriter.GetBuffer();
  if (streamBuffer && (streamBuffer != mLastBoundBuffers[20] || streamDataOffset != mLastBoundOffsets[20])) {
    mEncoder->setVertexBuffer(streamBuffer, streamDataOffset, 20);
    mEncoder->setFragmentBuffer(streamBuffer, streamDataOffset, 20);
    mLastBoundBuffers[20] = streamBuffer;
    mLastBoundOffsets[20] = streamDataOffset;
    if (mt_debug) {
        float* m = (float*)((uint8_t*)streamBuffer->contents() + streamDataOffset);
        Printf(PRINT_LOG, "Metal: Bound Stream (20) buf=%p off=%u. uObjectColor=%.6f %.6f %.6f %.6f\n", streamBuffer, streamDataOffset, m[0], m[1], m[2], m[3]);
    }
  }
}

void MtRenderState::WaitForStreamBuffers() {
  EndRenderPass();
  fb->GetCommands()->FlushCommands(true);
  
  mApplyCount = 0;
  mStreamBufferWriter.BeginFrame();
  mMatrixBufferWriter.BeginFrame();
  
  BeginRenderPass();
}

void MtRenderState::Bind(int bindingpoint, uint32_t offset) {
  BindBuffer(bindingpoint, nullptr, offset);
}

void MtRenderState::BindBuffer(int bindingpoint, MTL::Buffer *buffer,
                               uint32_t offset) {
  if (mt_debug) {
    Printf(PRINT_LOG, "Metal: BindBuffer bp=%d buffer=%p offset=%u\n", bindingpoint, buffer, offset);
  }
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
    if (mt_debug) Printf(PRINT_LOG, "Metal: MarkAsFilled tex=%p (W:%lu H:%lu)\n", tex, tex->width(), tex->height());
    mClearedTargets.insert(tex);
  }
}

void MtRenderState::BeginFrame() {
  mMaterial.Reset();
  mApplyCount = 0;
  mIsFirstPass = true;
  mStencilFunc = 2; // DF_Always
  mStencilRef = 0;
  mStencilRefChanged = true;
  mViewportWidth = -1;
  mViewportHeight = -1;
  mScissorWidth = -1;
  mScissorHeight = -1;
  if (mt_debug) Printf(PRINT_LOG, "Metal: BeginFrame - Clearing mClearedTargets\n");
  mClearedTargets.clear();
  mPipelineBound = false;
  
  mStreamBufferWriter.BeginFrame();
  mMatrixBufferWriter.BeginFrame();
  
  mBias.mUnits = 0.0f;
  mBias.mFactor = 0.0f;
  mBias.mChanged = true;

  for (int i = 0; i < 32; i++) {
      mBoundBuffers[i] = nullptr;
      mBoundOffsets[i] = 0;
  }
}

void MtRenderState::EndRenderPass() {
  if (mEncoder) {
    if (mt_debug) {
      Printf(PRINT_LOG, "Metal: EndRenderPass encoder=%p (applyCount was %d)\n", mEncoder, mApplyCount);
    }
    mEncoder->endEncoding();
    mEncoder = nullptr;
    mApplyCount++; // Increment when a pass is completed
  }
}

void MtRenderState::EndFrame() {
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
  if (mt_debug) {
    Printf(PRINT_LOG, "Metal: SetRenderTarget image=%p ds=%p %dx%d fmt=%d samples=%d\n",
           image, depthStencilView, width, height, format, samples);
  }
  
  // End current pass, but DON'T flush. Let Apply() or EndFrame handle it.
  EndRenderPass();

  bool isSwapChain = (image == nullptr);
  if (isSwapChain && fb->mCurrentDrawable) {
      image = (MTL::Texture*)fb->mCurrentDrawable->texture();
  }

  mRenderTarget.Image = image;
  mRenderTarget.IsSwapChain = isSwapChain;
  mRenderTarget.DepthStencil = depthStencilView;
  mRenderTarget.Width = width;
  mRenderTarget.Height = height;
  mRenderTarget.Format = image ? (int)image->pixelFormat() : format;
  mRenderTarget.Samples = samples;
}

// FORCE RECOMPILE: December 26 V10 Diagnostic Build
void MtRenderState::BeginRenderPass() {
  EndRenderPass(); // Ensure previous encoder is ended
  
  MTL::Texture *targetTex = mRenderTarget.Image;
  if (mRenderTarget.IsSwapChain && fb->mCurrentDrawable) {
      targetTex = (MTL::Texture*)fb->mCurrentDrawable->texture();
  }

  if (!targetTex) {
    if (mt_debug) Printf(PRINT_LOG, "Metal: BeginRenderPass - No render target image! width=%d height=%d\n", mRenderTarget.Width, mRenderTarget.Height);
    return;
  }
    
  if (mRenderTarget.Width <= 0 || mRenderTarget.Height <= 0) {
      if (mt_debug) Printf(PRINT_LOG, "Metal: BeginRenderPass - Invalid render target size %dx%d\n", mRenderTarget.Width, mRenderTarget.Height);
      return;
  }

  // Use manually allocated descriptor for maximum control and stability
  MTL::RenderPassDescriptor *pRPD = MTL::RenderPassDescriptor::alloc()->init();
  if (!pRPD) return;

  if (mt_debug) {
    fprintf(stderr, "Metal: BeginRenderPass target=%p (%dx%d fmt=%llu) clearTargets=%d applyCount=%d isSwap=%d\n",
           targetTex, mRenderTarget.Width, mRenderTarget.Height, (unsigned long long)targetTex->pixelFormat(), mClearTargets, mApplyCount, (int)mRenderTarget.IsSwapChain);
  }

  // Color Attachment
  auto colorAttachment = pRPD->colorAttachments()->object(0);
  colorAttachment->setTexture(targetTex);
  
  bool colorFilled = mClearedTargets.find(targetTex) != mClearedTargets.end();
  bool clearColor = (mClearTargets & CT_Color) || !colorFilled;
  
  if (mt_debug) {
      fprintf(stderr, "Metal: BeginRenderPass Color target %p, Filled: %d, ClearRequest: %d -> LoadAction: %s\n", 
             targetTex, (int)colorFilled, (int)(mClearTargets & CT_Color), 
             clearColor ? "Clear" : "Load");
  }

  colorAttachment->setLoadAction(clearColor ? MTL::LoadActionClear : MTL::LoadActionLoad);
  colorAttachment->setStoreAction(MTL::StoreActionStore);
  
  if (clearColor) {
      MTL::ClearColor cc;
      // Shadow maps must be cleared to a large value (max distance) to avoid artifacts
      if (targetTex == fb->GetBuffers()->ShadowMap->GetTexture()) {
          cc = MTL::ClearColor::Make(1e20, 0.0, 0.0, 1.0);
      } else {
          cc = MTL::ClearColor::Make(
              screen->mSceneClearColor[0], screen->mSceneClearColor[1],
              screen->mSceneClearColor[2], screen->mSceneClearColor[3]);
      }

      if (mt_debug) {
          fprintf(stderr, "Metal: BeginRenderPass - Clearing color target %p with (%f, %f, %f, %f)\n", 
                 targetTex, cc.red, cc.green, cc.blue, cc.alpha);
      }
      colorAttachment->setClearColor(cc);
      mClearedTargets.insert(targetTex);
  }


  // Depth/Stencil Attachment
  if (mRenderTarget.DepthStencil) {
    bool dsFilled = mClearedTargets.find(mRenderTarget.DepthStencil) != mClearedTargets.end();
    bool clearDS = !dsFilled;
    
    bool clearDepth = (mClearTargets & CT_Depth) || clearDS;
    bool clearStencil = (mClearTargets & CT_Stencil) || clearDS;

    if (mt_debug) {
        fprintf(stderr, "Metal: BeginRenderPass Depth target %p, Filled: %d, ClearRequest: %d -> LoadAction: %s\n", 
               mRenderTarget.DepthStencil, (int)dsFilled, (int)(mClearTargets & (CT_Depth | CT_Stencil)), 
               clearDS ? "Clear" : "Load");
    }

    auto depthAttachment = pRPD->depthAttachment();
    depthAttachment->setTexture(mRenderTarget.DepthStencil);
    depthAttachment->setLoadAction(clearDepth ? MTL::LoadActionClear : MTL::LoadActionLoad);
    
    // TBDR Optimization (Apple Silicon): If we are in the final swapchain pass, 
    // we don't need to store the depth/stencil results back to main memory.
    if (mRenderTarget.IsSwapChain) {
        depthAttachment->setStoreAction(MTL::StoreActionDontCare);
    } else {
        depthAttachment->setStoreAction(MTL::StoreActionStore);
    }

    if (mt_debug) {
        fprintf(stderr, "Metal:   Depth Load: %s, Store: %s, Write: %d, Func: %d\n", 
               depthAttachment->loadAction() == MTL::LoadActionClear ? "Clear" : "Load",
               depthAttachment->storeAction() == MTL::StoreActionStore ? "Store" : "DontCare",
               (int)mDepthWrite, mDepthFunc);
    }

    if (clearDepth) {
        depthAttachment->setClearDepth(0.0); // Reverse-Z: Clear to 0.0 (Far)
        mClearedTargets.insert(mRenderTarget.DepthStencil);
    }

    auto stencilAttachment = pRPD->stencilAttachment();
    stencilAttachment->setTexture(mRenderTarget.DepthStencil);
    stencilAttachment->setLoadAction(clearStencil ? MTL::LoadActionClear : MTL::LoadActionLoad);
    
    if (mRenderTarget.IsSwapChain) {
        stencilAttachment->setStoreAction(MTL::StoreActionDontCare);
    } else {
        stencilAttachment->setStoreAction(MTL::StoreActionStore);
    }

    if (mt_debug) {
        fprintf(stderr, "Metal:   Stencil Load: %s, Store: %s, Ref: %d, Func: %d, Op: %d\n", 
               stencilAttachment->loadAction() == MTL::LoadActionClear ? "Clear" : "Load",
               stencilAttachment->storeAction() == MTL::StoreActionStore ? "Store" : "DontCare",
               mStencilRef, mStencilFunc, mStencilOp);
    }

    if (clearStencil) {
        stencilAttachment->setClearStencil(0);
        mClearedTargets.insert(mRenderTarget.DepthStencil);
    }
  }

  mIsFirstPass = false;

  MTL::CommandBuffer *cmdBuffer = fb->GetCommands()->GetRenderCommandBuffer();
  if (cmdBuffer) {
    if (mt_debug) Printf(PRINT_LOG, "Metal: renderCommandEncoder status=%d\n", (int)cmdBuffer->status());
    mEncoder = cmdBuffer->renderCommandEncoder(pRPD);
    mPipelineBound = false;
    if (mEncoder) {
        mEncoder->setFrontFacingWinding(MTL::WindingCounterClockwise);
        
        // Use current state if set, otherwise default to full target
        MTL::Viewport viewport;
        if (mViewportWidth > 0) {
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
        if (mScissorWidth > 0) {
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

  pRPD->release();

  mMaterial.mChanged = true;
  mClearTargets = 0;
  mScissorChanged = true;
  mViewportChanged = true;
  mStencilRefChanged = true;
  mCullModeChanged = true;
  mBias.mUnits = 0.0f;
  mBias.mFactor = 0.0f;
  mBias.mChanged = true;
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
  }
}
