/*
**  Metal backend - Render state
**  Copyright (c) 2025 GZDoom Contributors
*/

#include <Metal/Metal.hpp>
#include "mt_renderstate.h"
#include "metal/system/mt_renderdevice.h"
#include "metal/system/mt_commandbuffer.h"
#include "metal/system/mt_buffer.h"
#include "metal/system/mt_hwbuffer.h"
#include "metal/textures/mt_texture.h"
#include "flatvertices.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "hwrenderer/data/hw_cvars.h"
#include "printf.h"

MtRenderState::MtRenderState(MetalRenderDevice* fb) : fb(fb), mStreamBufferWriter(fb), mMatrixBufferWriter(fb) {}

void MtRenderState::ClearScreen()
{
	// Set 2D viewport and draw a fullscreen black quad
	screen->mViewpoints->Set2D(*this, SCREENWIDTH, SCREENHEIGHT);
	SetColor(0, 0, 0);
	Apply(DT_TriangleStrip);

	if (mEncoder)
	{
		auto encoder = (MTL::RenderCommandEncoder*)mEncoder;
		encoder->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, FFlatVertexBuffer::FULLSCREEN_INDEX, 4);
	}
}

void MtRenderState::Draw(int dt, int index, int count, bool apply)
{
	if (apply || mNeedApply)
		Apply(dt);

	if (!mEncoder)
	{
		static int warnCount = 0;
		if (warnCount++ < 5)
			Printf("Draw() called without encoder! dt=%d index=%d count=%d\n", dt, index, count);
		return;
	}

	// Convert draw type to Metal primitive type
	MTL::PrimitiveType primitiveType;
	switch (dt)
	{
		case DT_Points: primitiveType = MTL::PrimitiveTypePoint; break;
		case DT_Lines: primitiveType = MTL::PrimitiveTypeLine; break;
		case DT_Triangles: primitiveType = MTL::PrimitiveTypeTriangle; break;
		case DT_TriangleFan: primitiveType = MTL::PrimitiveTypeTriangle; break; // TODO: Emulate triangle fan
		case DT_TriangleStrip: primitiveType = MTL::PrimitiveTypeTriangleStrip; break;
		default: primitiveType = MTL::PrimitiveTypeTriangle; break;
	}

	static int drawCount = 0;
	if (drawCount++ < 10)
		Printf("Draw #%d: dt=%d index=%d count=%d\n", drawCount, dt, index, count);

	auto encoder = (MTL::RenderCommandEncoder*)mEncoder;
	encoder->drawPrimitives(primitiveType, index, count);
}

void MtRenderState::DrawIndexed(int dt, int index, int count, bool apply)
{
	static int drawIndexedCallCount = 0;
	if (++drawIndexedCallCount <= 10)
		Printf("Metal: DrawIndexed call #%d (dt=%d, index=%d, count=%d, apply=%d)\n",
			drawIndexedCallCount, dt, index, count, apply);

	if (apply || mNeedApply)
		Apply(dt);

	if (!mEncoder || !mIndexBuffer)
	{
		if (drawIndexedCallCount <= 10)
			Printf("Metal: Warning - DrawIndexed called but encoder=%p indexBuffer=%p\n",
				mEncoder, mIndexBuffer);
		return;
	}

	// Convert draw type to Metal primitive type
	MTL::PrimitiveType primitiveType;
	switch (dt)
	{
		case DT_Points: primitiveType = MTL::PrimitiveTypePoint; break;
		case DT_Lines: primitiveType = MTL::PrimitiveTypeLine; break;
		case DT_Triangles: primitiveType = MTL::PrimitiveTypeTriangle; break;
		case DT_TriangleFan: primitiveType = MTL::PrimitiveTypeTriangle; break; // TODO: Emulate triangle fan
		case DT_TriangleStrip: primitiveType = MTL::PrimitiveTypeTriangleStrip; break;
		default: primitiveType = MTL::PrimitiveTypeTriangle; break;
	}

	auto encoder = (MTL::RenderCommandEncoder*)mEncoder;
	auto mtIndexBuffer = static_cast<MtIndexBuffer*>(mIndexBuffer);
	auto indexBuffer = (MTL::Buffer*)mtIndexBuffer->GetBuffer();

	if (indexBuffer)
	{
		encoder->drawIndexedPrimitives(primitiveType, count, MTL::IndexTypeUInt32,
			indexBuffer, index * sizeof(uint32_t));
	}
}

bool MtRenderState::SetDepthClamp(bool on)
{
	bool lastValue = mDepthClamp;
	mDepthClamp = on;
	mNeedApply = true;
	return lastValue;
}

void MtRenderState::SetDepthMask(bool on)
{
	mDepthWrite = on;
	mNeedApply = true;
}

void MtRenderState::SetDepthFunc(int func)
{
	mDepthFunc = func;
	mNeedApply = true;
}

void MtRenderState::SetDepthRange(float min, float max)
{
	mViewportDepthMin = min;
	mViewportDepthMax = max;
	mViewportChanged = true;
	mNeedApply = true;
}

void MtRenderState::SetColorMask(bool r, bool g, bool b, bool a)
{
	mColorMask = (r?1:0)|(g?2:0)|(b?4:0)|(a?8:0);
	mNeedApply = true;
}

void MtRenderState::SetStencil(int offs, int op, int flags)
{
	mStencilRef = offs;
	mStencilOp = op;
	mStencilRefChanged = true;
	mNeedApply = true;
}

void MtRenderState::SetCulling(int mode)
{
	mCullMode = mode;
	mNeedApply = true;
}

void MtRenderState::EnableClipDistance(int num, bool state)
{
	// TODO: Implement clip distance
}

void MtRenderState::Clear(int targets)
{
	mClearTargets = targets;
}

void MtRenderState::EnableStencil(bool on)
{
	mStencilTest = on;
	mNeedApply = true;
}

void MtRenderState::SetScissor(int x, int y, int w, int h)
{
	mScissorX = x;
	mScissorY = y;
	mScissorWidth = w;
	mScissorHeight = h;
	mScissorChanged = true;
}

void MtRenderState::SetViewport(int x, int y, int w, int h)
{
	mViewportX = x;
	mViewportY = y;
	mViewportWidth = w;
	mViewportHeight = h;
	mViewportChanged = true;
}

void MtRenderState::EnableDepthTest(bool on)
{
	mDepthTest = on;
	mNeedApply = true;
}

void MtRenderState::EnableMultisampling(bool on)
{
	// Metal handles MSAA through render pass configuration
	// No per-draw state needed (controlled via MTLRenderPipelineDescriptor at pipeline creation)
}

void MtRenderState::EnableLineSmooth(bool on)
{
	// Metal does not have explicit line smoothing state
	// Line rendering quality is controlled by the pipeline configuration
}

void MtRenderState::EnableDrawBuffers(int count, bool apply)
{
	// Handle Multiple Render Targets (MRT)
	// If draw buffer count changes, we need to restart the render pass
	if (mRenderTarget.DrawBuffers != count)
	{
		EndRenderPass();
		mRenderTarget.DrawBuffers = count;
	}
}

void MtRenderState::BeginFrame()
{
	mMaterial.Reset();
	mApplyCount = 0;
}

void MtRenderState::SetRenderTarget(void* image, void* depthStencilView, int width, int height, int format, int samples)
{
	mRenderTarget.Image = image;
	mRenderTarget.DepthStencil = depthStencilView;
	mRenderTarget.Width = width;
	mRenderTarget.Height = height;
	mRenderTarget.Format = format;
	mRenderTarget.Samples = samples;
}

void MtRenderState::Bind(int bindingpoint, uint32_t offset)
{
	// TODO: Implement viewpoint binding
	mViewpointOffset = offset;
	mNeedApply = true;
}

void MtRenderState::EndRenderPass()
{
	if (mEncoder)
	{
		((MTL::RenderCommandEncoder*)mEncoder)->endEncoding();
		mEncoder = nullptr;
	}
}

void MtRenderState::EndFrame()
{
	// Reset stream buffer writers for next frame
	mMatrixBufferWriter.Reset();
	mStreamBufferWriter.Reset();
}

// ============================================================================
// Lazy State Evaluation - Core Apply Methods
// ============================================================================

void MtRenderState::Apply(int dt)
{
	mApplyCount++;

	// TODO: Implement command buffer flushing like Vulkan
	// if (mApplyCount >= mt_submit_size)
	// {
	//     fb->GetCommands()->FlushCommands();
	//     mApplyCount = 0;
	// }

	// Apply all state changes in order (following Vulkan pattern)
	ApplyStreamData();
	ApplyMatrices();
	ApplyRenderPass(dt);
	ApplyScissor();
	ApplyViewport();
	ApplyStencilRef();
	ApplyDepthBias();
	ApplyPushConstants();
	ApplyVertexBuffers();
	ApplyHWBufferSet();
	ApplyMaterial();

	mNeedApply = false;
}

void MtRenderState::ApplyRenderPass(int dt)
{
	// Create render encoder if needed
	bool inRenderPass = (mEncoder != nullptr);

	if (!inRenderPass)
	{
		// Get command buffer
		// TODO: Implement proper command buffer management
		// auto cmdBuffer = fb->GetCommands()->GetRenderCommandBuffer();

		// Mark state as needing updates
		mScissorChanged = true;
		mViewportChanged = true;
		mStencilRefChanged = true;
		mBias.mChanged = true;

		// Create render encoder
		BeginRenderPass();
	}

	// Build pipeline key from current state
	MtPipelineKey pipelineKey;
	// Get vertex format from current vertex buffer
	pipelineKey.VertexFormat = mVertexBuffer ? static_cast<MtVertexBuffer*>(mVertexBuffer)->VertexFormat : -1;

	// Build shader key from effect state (following Vulkan pattern)
	int effectState = mMaterial.mOverrideShader >= 0 ? mMaterial.mOverrideShader : (mMaterial.mMaterial ? mMaterial.mMaterial->GetShaderIndex() : 0);
	if (!mTextureEnabled) effectState = SHADER_NoTexture;
	int specialEffect = mSpecialEffect;
	bool alphaTest = (mAlphaThreshold >= 0.f);
	// Combine effect state, special effect, and alpha test into a single shader key
	pipelineKey.ShaderKey = effectState | (specialEffect << 16) | (alphaTest ? (1 << 24) : 0);

	pipelineKey.DepthFunc = mDepthFunc;
	pipelineKey.DepthClampMode = mDepthClamp ? 1 : 0;
	pipelineKey.ColorMask = mColorMask;
	pipelineKey.CullMode = mCullMode;
	pipelineKey.StencilOp = mStencilOp;
	pipelineKey.BlendMode = mRenderStyle.BlendOp;  // Use render style blend op
	pipelineKey.SampleCount = mRenderTarget.Samples;
	pipelineKey.DrawBufferCount = mRenderTarget.DrawBuffers;
	pipelineKey.PixelFormat = mRenderTarget.Format;
	pipelineKey.DepthStencilFormat = mRenderTarget.DepthStencil ? (int)MTL::PixelFormatDepth32Float_Stencil8 : 0;

	// Only update pipeline state if key changed
	if (pipelineKey != mPipelineKey)
	{
		auto pipelineState = fb->GetPipelineStateManager()->GetPipelineState(pipelineKey);
		if (!pipelineState)
		{
			Printf("Warning: Failed to get pipeline state\n");
			return;
		}
		if (!pipelineState->pipelineState)
		{
			Printf("Warning: Pipeline state is null\n");
			return;
		}
		if (mEncoder)
		{
			auto encoder = (MTL::RenderCommandEncoder*)mEncoder;
			encoder->setRenderPipelineState((MTL::RenderPipelineState*)pipelineState->pipelineState);
			encoder->setDepthStencilState((MTL::DepthStencilState*)pipelineState->depthStencilState);
		}
		else
		{
			Printf("Warning: No render encoder in ApplyRenderPass\n");
		}
		mPipelineKey = pipelineKey;
	}
}

void MtRenderState::ApplyStencilRef()
{
	if (mStencilRefChanged && mEncoder)
	{
		auto encoder = (MTL::RenderCommandEncoder*)mEncoder;
		encoder->setStencilReferenceValue(mStencilRef);
		mStencilRefChanged = false;
	}
}

void MtRenderState::ApplyDepthBias()
{
	if (mBias.mChanged && mEncoder)
	{
		auto encoder = (MTL::RenderCommandEncoder*)mEncoder;
		encoder->setDepthBias(mBias.mUnits, 0.0f, mBias.mFactor);
		mBias.mChanged = false;
	}
}

void MtRenderState::ApplyScissor()
{
	if (mScissorChanged && mEncoder)
	{
		auto encoder = (MTL::RenderCommandEncoder*)mEncoder;

		MTL::ScissorRect scissor;
		if (mScissorWidth >= 0)
		{
			int x0 = std::max(0, std::min(mScissorX, mRenderTarget.Width));
			int y0 = std::max(0, std::min(mScissorY, mRenderTarget.Height));
			int x1 = std::max(0, std::min(mScissorX + mScissorWidth, mRenderTarget.Width));
			int y1 = std::max(0, std::min(mScissorY + mScissorHeight, mRenderTarget.Height));

			scissor.x = x0;
			scissor.y = y0;
			scissor.width = x1 - x0;
			scissor.height = y1 - y0;
		}
		else
		{
			scissor.x = 0;
			scissor.y = 0;
			scissor.width = mRenderTarget.Width;
			scissor.height = mRenderTarget.Height;
		}

		encoder->setScissorRect(scissor);
		mScissorChanged = false;
	}
}

void MtRenderState::ApplyViewport()
{
	if (mViewportChanged && mEncoder)
	{
		auto encoder = (MTL::RenderCommandEncoder*)mEncoder;

		MTL::Viewport viewport;
		if (mViewportWidth >= 0)
		{
			viewport.originX = (double)mViewportX;
			viewport.originY = (double)mViewportY;
			viewport.width = (double)mViewportWidth;
			viewport.height = (double)mViewportHeight;
		}
		else
		{
			viewport.originX = 0.0;
			viewport.originY = 0.0;
			viewport.width = (double)mRenderTarget.Width;
			viewport.height = (double)mRenderTarget.Height;
		}
		viewport.znear = mViewportDepthMin;
		viewport.zfar = mViewportDepthMax;

		encoder->setViewport(viewport);
		mViewportChanged = false;
	}
}

void MtRenderState::ApplyStreamData()
{
	// TODO: Set useVertexData from vertex format
	// mStreamData.useVertexData = ...;

	// Update timer based on material
	if (mMaterial.mMaterial && mMaterial.mMaterial->Source())
		mStreamData.timer = static_cast<float>((double)(screen->FrameTime - firstFrame) * (double)mMaterial.mMaterial->Source()->GetShaderSpeed() / 1000.);
	else
		mStreamData.timer = 0.0f;

	// Write stream data to buffer
	if (!mStreamBufferWriter.Write(mStreamData))
	{
		WaitForStreamBuffers();
		mStreamBufferWriter.Write(mStreamData);
	}
}

void MtRenderState::ApplyMatrices()
{
	// Write matrices to buffer
	if (!mMatrixBufferWriter.Write(mModelMatrix, mModelMatrixEnabled, mTextureMatrix, mTextureMatrixEnabled))
	{
		WaitForStreamBuffers();
		mMatrixBufferWriter.Write(mModelMatrix, mModelMatrixEnabled, mTextureMatrix, mTextureMatrixEnabled);
	}
}

void MtRenderState::ApplyPushConstants()
{
	if (!mEncoder) return;

	// Calculate fog settings
	int fogset = 0;
	if (mFogEnabled)
	{
		if (mFogEnabled == 2)
		{
			fogset = -3;	// 2D rendering with 'foggy' overlay.
		}
		else if ((GetFogColor() & 0xffffff) == 0)
		{
			fogset = gl_fogmode;
		}
		else
		{
			fogset = -gl_fogmode;
		}
	}

	// Handle hardware canvas textures
	int tempTM = TM_NORMAL;
	if (mMaterial.mMaterial && mMaterial.mMaterial->Source()->isHardwareCanvas())
		tempTM = TM_OPAQUE;

	// Populate push constants structure
	mPushConstants.uFogEnabled = fogset;
	mPushConstants.uTextureMode = GetTextureModeAndFlags(tempTM);
	mPushConstants.uLightDist = mLightParms[0];
	mPushConstants.uLightFactor = mLightParms[1];
	mPushConstants.uFogDensity = mLightParms[2];
	mPushConstants.uLightLevel = mLightParms[3];
	mPushConstants.uAlphaThreshold = mAlphaThreshold;
	mPushConstants.uClipSplit = { mClipSplit[0], mClipSplit[1] };

	// Material specular properties
	if (mMaterial.mMaterial)
	{
		auto source = mMaterial.mMaterial->Source();
		mPushConstants.uSpecularMaterial = { source->GetGlossiness(), source->GetSpecularLevel() };
	}

	// Light and bone indices
	mPushConstants.uLightIndex = mLightIndex;
	mPushConstants.uBoneIndexBase = mBoneIndexBase;
	mPushConstants.uDataIndex = mStreamBufferWriter.DataIndex();

	// Metal implementation: Push constants are passed as inline buffers
	// Using setVertexBytes/setFragmentBytes (Metal's equivalent to Vulkan push constants)
	// Note: NOT Metal "function constants" (which are compile-time specialization)
	//
	// Buffer binding indices:
	// - Vertex shader: index 5 (after viewpoint=2, matrices=3, streamdata=4)
	// - Fragment shader: index 1 (after streamdata=0)
	auto encoder = (MTL::RenderCommandEncoder*)mEncoder;
	encoder->setVertexBytes(&mPushConstants, sizeof(PushConstants), 5);
	encoder->setFragmentBytes(&mPushConstants, sizeof(PushConstants), 1);
}

void MtRenderState::ApplyHWBufferSet()
{
	if (!mEncoder) return;

	auto encoder = (MTL::RenderCommandEncoder*)mEncoder;

	// Get current offsets from buffer writers
	uint32_t matrixOffset = mMatrixBufferWriter.Offset();
	uint32_t streamDataOffset = mStreamBufferWriter.StreamDataOffset();

	// Only rebind if offsets have changed
	if (mViewpointOffset != mLastViewpointOffset ||
	    matrixOffset != mLastMatricesOffset ||
	    streamDataOffset != mLastStreamDataOffset)
	{
		// Metal Buffer Binding Strategy (following Vulkan pattern):
		//
		// Vertex Shader Buffers:
		//   - Index 0-1: Vertex buffers (bound in ApplyVertexBuffers)
		//   - Index 2: Viewpoint buffer (per-frame camera/projection data)
		//   - Index 3: Matrices buffer (per-draw model/texture matrices)
		//   - Index 4: Stream data buffer (per-draw stream data)
		//   - Index 5: Push constants (inline buffer, bound in ApplyPushConstants)
		//
		// Fragment Shader Buffers:
		//   - Index 0: Stream data buffer (per-draw stream data)
		//   - Index 1: Push constants (inline buffer, bound in ApplyPushConstants)
		//   - Index 2+: Material textures (bound in ApplyMaterial)

		// Bind viewpoint buffer (camera/projection matrices)
		auto bufferManager = fb->GetBufferManager();
		if (bufferManager->ViewpointUBO)
		{
			auto viewpointBuffer = (MTL::Buffer*)bufferManager->ViewpointUBO->GetBuffer();
			if (viewpointBuffer)
			{
				encoder->setVertexBuffer(viewpointBuffer, mViewpointOffset, 2);
			}
		}

		// Bind matrices buffer (model/texture matrices)
		auto matrixBuffer = (MTL::Buffer*)mMatrixBufferWriter.GetBuffer();
		if (matrixBuffer)
		{
			encoder->setVertexBuffer(matrixBuffer, matrixOffset, 3);
		}

		// Bind stream data buffer (per-draw uniforms)
		auto streamBuffer = (MTL::Buffer*)mStreamBufferWriter.GetBuffer();
		if (streamBuffer)
		{
			encoder->setVertexBuffer(streamBuffer, streamDataOffset, 4);
			encoder->setFragmentBuffer(streamBuffer, streamDataOffset, 0);
		}

		// Track last bound offsets
		mLastViewpointOffset = mViewpointOffset;
		mLastMatricesOffset = matrixOffset;
		mLastStreamDataOffset = streamDataOffset;
	}
}

void MtRenderState::ApplyVertexBuffers()
{
	if (!mEncoder) return;

	auto encoder = (MTL::RenderCommandEncoder*)mEncoder;

	// Bind vertex buffer if changed
	if ((mVertexBuffer != mLastVertexBuffer ||
	     mVertexOffsets[0] != mLastVertexOffsets[0] ||
	     mVertexOffsets[1] != mLastVertexOffsets[1]) &&
	     mVertexBuffer)
	{
		auto mtBuffer = static_cast<MtVertexBuffer*>(mVertexBuffer);
		size_t stride = mtBuffer->GetStride();
		auto buffer = (MTL::Buffer*)mtBuffer->GetBuffer();

		if (buffer)
		{
			// Metal binds vertex buffers at indices 0 and 1
			// Calculate byte offsets from vertex offsets
			NS::UInteger offset0 = mVertexOffsets[0] * stride;
			NS::UInteger offset1 = mVertexOffsets[1] * stride;

			encoder->setVertexBuffer(buffer, offset0, 0);
			encoder->setVertexBuffer(buffer, offset1, 1);

			mLastVertexBuffer = mVertexBuffer;
			mLastVertexOffsets[0] = mVertexOffsets[0];
			mLastVertexOffsets[1] = mVertexOffsets[1];
		}
	}

	// Track index buffer changes (actual binding happens in drawIndexedPrimitives)
	if (mIndexBuffer != mLastIndexBuffer && mIndexBuffer)
	{
		mLastIndexBuffer = mIndexBuffer;
	}
}

void MtRenderState::ApplyMaterial()
{
	if (!mEncoder) return;

	if (mMaterial.mChanged)
	{
		auto encoder = (MTL::RenderCommandEncoder*)mEncoder;

		// Update canvas textures if needed (same as Vulkan)
		if (mMaterial.mMaterial && mMaterial.mMaterial->Source()->isHardwareCanvas())
		{
			static_cast<FCanvasTexture*>(mMaterial.mMaterial->Source()->GetTexture())->NeedUpdate();
		}

		// Get Metal texture from material
		static int texBindCount = 0;
		if (mMaterial.mMaterial)
		{
			auto hwTexture = mMaterial.mMaterial->GetLayer(0, mMaterial.mTranslation);
			if (hwTexture)
			{
				// Cast to Metal hardware texture and get the image
				auto mtHwTexture = static_cast<MtHardwareTexture*>(hwTexture);
				auto image = mtHwTexture->GetImage();

				// If texture hasn't been created yet, create it now from game texture data
				if (!image->GetTexture())
				{
					auto tex = mMaterial.mMaterial->Source();
					if (tex)
					{
						if (texBindCount < 5)
							Printf("Metal: Texture not yet created, calling CreateImage()\n");
						mtHwTexture->CreateImage(tex, mMaterial.mTranslation, mMaterial.mScaleFlags);
					}
				}

				// Upload staging buffer data to texture if needed
				// This happens after MapBuffer() was used to fill the staging buffer
				if (mtHwTexture->GetStagingBufferSize() > 0 && image->GetTexture())
				{
					auto mtlTexture = (MTL::Texture*)image->GetTexture();
					int w = image->GetWidth();
					int h = image->GetHeight();
					int texelsize = mtHwTexture->GetStagingBufferSize() / (w * h);

					// Upload staging buffer to texture
					MTL::Region region = MTL::Region(0, 0, w, h);
					mtlTexture->replaceRegion(region, 0, mtHwTexture->GetStagingBuffer(), w * texelsize);

					if (texBindCount < 5)
						Printf("Metal: Uploaded %d bytes to texture %p (%dx%d)\n",
							(int)mtHwTexture->GetStagingBufferSize(), mtlTexture, w, h);
				}

				// Now bind the texture
				if (image)
				{
					auto mtlTexture = image->GetTexture();
					if (mtlTexture)
					{
						if (texBindCount++ < 5)
							Printf("Metal: Binding texture %p at index 0\n", mtlTexture);

						encoder->setFragmentTexture((MTL::Texture*)mtlTexture, 0);

						// TODO: Get proper sampler from mMaterial.mClampMode
						// For now, create a simple sampler
						MTL::SamplerDescriptor* samplerDesc = MTL::SamplerDescriptor::alloc()->init();
						samplerDesc->setMinFilter(MTL::SamplerMinMagFilterLinear);
						samplerDesc->setMagFilter(MTL::SamplerMinMagFilterLinear);
						samplerDesc->setSAddressMode(MTL::SamplerAddressModeRepeat);
						samplerDesc->setTAddressMode(MTL::SamplerAddressModeRepeat);

						MTL::SamplerState* sampler = fb->device->device->newSamplerState(samplerDesc);
						encoder->setFragmentSamplerState(sampler, 0);
						samplerDesc->release();
						sampler->release();
					}
					else if (texBindCount < 5)
					{
						Printf("Metal: Warning - hardware texture image exists but GetTexture() returned null\n");
					}
				}
				else if (texBindCount < 5)
				{
					Printf("Metal: Warning - hardware texture exists but GetImage() returned null\n");
				}
			}
			else if (texBindCount < 5)
			{
				Printf("Metal: Warning - no hardware texture for material layer 0\n");
			}
		}
		else if (texBindCount < 5)
		{
			Printf("Metal: Warning - no material set\n");
		}

		mMaterial.mChanged = false;
	}
}

void MtRenderState::BeginRenderPass()
{
	if (!mRenderTarget.Image)
	{
		static int warnCount = 0;
		if (warnCount++ < 3)
			Printf("BeginRenderPass: No render target image!\n");
		return;
	}

	// Get command buffer from command buffer manager
	auto cmdManager = fb->GetCommands();
	auto cmdBuffer = (MTL::CommandBuffer*)cmdManager->GetRenderCommandBuffer();
	if (!cmdBuffer)
	{
		static int warnCount = 0;
		if (warnCount++ < 3)
			Printf("BeginRenderPass: No command buffer!\n");
		return;
	}

	// Create render pass descriptor
	auto passDescriptor = MTL::RenderPassDescriptor::alloc()->init();

	// Set color attachment 0 (main scene color)
	if (mRenderTarget.Image)
	{
		auto colorAttachment = passDescriptor->colorAttachments()->object(0);
		colorAttachment->setTexture((MTL::Texture*)mRenderTarget.Image);
		colorAttachment->setLoadAction((mClearTargets & CT_Color) ? MTL::LoadActionClear : MTL::LoadActionLoad);
		colorAttachment->setStoreAction(MTL::StoreActionStore);

		if (mClearTargets & CT_Color)
		{
			colorAttachment->setClearColor(MTL::ClearColor::Make(
				screen->mSceneClearColor[0],
				screen->mSceneClearColor[1],
				screen->mSceneClearColor[2],
				screen->mSceneClearColor[3]
			));
		}
	}

	// Set color attachment 1 (fog) if using multiple draw buffers
	if (mRenderTarget.DrawBuffers > 1)
	{
		// TODO: Get fog buffer from render buffers once implemented
		// auto buffers = fb->GetBuffers();
		// auto colorAttachment1 = passDescriptor->colorAttachments()->object(1);
		// colorAttachment1->setTexture(buffers->SceneFog);
		// colorAttachment1->setLoadAction(MTL::LoadActionClear);
		// colorAttachment1->setStoreAction(MTL::StoreActionStore);
		// colorAttachment1->setClearColor(MTL::ClearColor::Make(0.0, 0.0, 0.0, 0.0));
	}

	// Set color attachment 2 (normals) if using 3+ draw buffers
	if (mRenderTarget.DrawBuffers > 2)
	{
		// TODO: Get normal buffer from render buffers once implemented
		// auto buffers = fb->GetBuffers();
		// auto colorAttachment2 = passDescriptor->colorAttachments()->object(2);
		// colorAttachment2->setTexture(buffers->SceneNormal);
		// colorAttachment2->setLoadAction(MTL::LoadActionClear);
		// colorAttachment2->setStoreAction(MTL::StoreActionStore);
		// colorAttachment2->setClearColor(MTL::ClearColor::Make(0.0, 0.0, 0.0, 0.0));
	}

	// Set depth/stencil attachment if present
	if (mRenderTarget.DepthStencil)
	{
		auto depthAttachment = passDescriptor->depthAttachment();
		depthAttachment->setTexture((MTL::Texture*)mRenderTarget.DepthStencil);
		depthAttachment->setLoadAction((mClearTargets & CT_Depth) ? MTL::LoadActionClear : MTL::LoadActionLoad);
		depthAttachment->setStoreAction(MTL::StoreActionStore);
		depthAttachment->setClearDepth(1.0);

		auto stencilAttachment = passDescriptor->stencilAttachment();
		stencilAttachment->setTexture((MTL::Texture*)mRenderTarget.DepthStencil);
		stencilAttachment->setLoadAction((mClearTargets & CT_Stencil) ? MTL::LoadActionClear : MTL::LoadActionLoad);
		stencilAttachment->setStoreAction(MTL::StoreActionStore);
		stencilAttachment->setClearStencil(0);
	}

	// Create render command encoder
	mEncoder = cmdBuffer->renderCommandEncoder(passDescriptor);

	// Clean up pass descriptor
	passDescriptor->release();

	static int encCount = 0;
	if (encCount++ < 3)
		Printf("BeginRenderPass: Created encoder %p (width=%d height=%d)\n",
			mEncoder, mRenderTarget.Width, mRenderTarget.Height);

	// Mark material as changed to force rebinding
	mMaterial.mChanged = true;

	// Clear targets have been applied
	mClearTargets = 0;

	// Mark all state as needing reapplication
	mScissorChanged = true;
	mViewportChanged = true;
	mStencilRefChanged = true;
	mBias.mChanged = true;

	// Invalidate pipeline key to force pipeline state rebinding
	mPipelineKey = MtPipelineKey(); // Reset to default/invalid state
}

void MtRenderState::WaitForStreamBuffers()
{
	// Wait for GPU to finish with buffers
	fb->WaitForCommands(false);

	// Reset buffer writers for new frame
	mApplyCount = 0;
	mStreamBufferWriter.Reset();
	mMatrixBufferWriter.Reset();
}

// ============================================================================
// Stream Buffer Writers
// ============================================================================

MtStreamBufferWriter::MtStreamBufferWriter(MetalRenderDevice* fb)
	: mBuffer(std::make_unique<MtStreamBuffer>(fb, sizeof(StreamData) * MAX_STREAM_DATA))
{
}

bool MtStreamBufferWriter::Write(const StreamData& data)
{
	mDataIndex++;
	if (mDataIndex == MAX_STREAM_DATA)
	{
		mDataIndex = 0;
		mStreamDataOffset = mBuffer->NextStreamDataBlock();
		if (mStreamDataOffset == 0xffffffff)
			return false;
	}

	// Write to Metal buffer's CPU-accessible memory
	uint8_t* ptr = mBuffer->GetBufferPointer();
	if (ptr)
	{
		memcpy(ptr + mStreamDataOffset + sizeof(StreamData) * mDataIndex, &data, sizeof(StreamData));
	}

	return true;
}

void MtStreamBufferWriter::Reset()
{
	mDataIndex = MAX_STREAM_DATA - 1;
	mStreamDataOffset = 0;
	mBuffer->Reset();
}

uint32_t MtStreamBufferWriter::DataIndex() const
{
	return mDataIndex;
}

uint32_t MtStreamBufferWriter::StreamDataOffset() const
{
	return mStreamDataOffset;
}

/////////////////////////////////////////////////////////////////////////////

MtMatrixBufferWriter::MtMatrixBufferWriter(MetalRenderDevice* fb)
	: mBuffer(std::make_unique<MtStreamBuffer>(fb, sizeof(MatricesUBO)))
{
	mIdentityMatrix.loadIdentity();
}

template<typename T>
static void BufferedSet(bool& modified, T& dst, const T& src)
{
	if (dst == src)
		return;
	dst = src;
	modified = true;
}

static void BufferedSet(bool& modified, VSMatrix& dst, const VSMatrix& src)
{
	if (memcmp(dst.get(), src.get(), sizeof(FLOATTYPE) * 16) == 0)
		return;
	dst = src;
	modified = true;
}

bool MtMatrixBufferWriter::Write(const VSMatrix& modelMatrix, bool modelMatrixEnabled, const VSMatrix& textureMatrix, bool textureMatrixEnabled)
{
	bool modified = (mOffset == 0); // always modified first call

	if (modelMatrixEnabled)
	{
		BufferedSet(modified, mMatrices.ModelMatrix, modelMatrix);
		if (modified)
			mMatrices.NormalModelMatrix.computeNormalMatrix(modelMatrix);
	}
	else
	{
		BufferedSet(modified, mMatrices.ModelMatrix, mIdentityMatrix);
		BufferedSet(modified, mMatrices.NormalModelMatrix, mIdentityMatrix);
	}

	if (textureMatrixEnabled)
	{
		BufferedSet(modified, mMatrices.TextureMatrix, textureMatrix);
	}
	else
	{
		BufferedSet(modified, mMatrices.TextureMatrix, mIdentityMatrix);
	}

	if (modified)
	{
		mOffset = mBuffer->NextStreamDataBlock();
		if (mOffset == 0xffffffff)
			return false;

		// Write to Metal buffer's CPU-accessible memory
		uint8_t* ptr = mBuffer->GetBufferPointer();
		if (ptr)
		{
			memcpy(ptr + mOffset, &mMatrices, sizeof(MatricesUBO));
		}
	}

	return true;
}

void MtMatrixBufferWriter::Reset()
{
	mOffset = 0;
	mBuffer->Reset();
}

uint32_t MtMatrixBufferWriter::Offset() const
{
	return mOffset;
}
