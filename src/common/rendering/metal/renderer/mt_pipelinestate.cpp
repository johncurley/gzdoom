/*
**  Metal backend - Pipeline state management
**  Copyright (c) 2025 GZDoom Contributors
*/

#include <Metal/Metal.hpp>
#include "mt_pipelinestate.h"
#include "metal/system/mt_renderdevice.h"
#include "metal/shaders/mt_shader.h"
#include "hwrenderer/data/hw_renderstate.h"
#include "renderstyle.h"

bool MtPipelineKey::operator==(const MtPipelineKey& other) const
{
	return VertexFormat == other.VertexFormat &&
		ShaderKey == other.ShaderKey &&
		BlendMode == other.BlendMode &&
		DepthFunc == other.DepthFunc &&
		StencilOp == other.StencilOp &&
		ColorMask == other.ColorMask &&
		CullMode == other.CullMode &&
		DepthClampMode == other.DepthClampMode &&
		SampleCount == other.SampleCount &&
		DrawBufferCount == other.DrawBufferCount &&
		PixelFormat == other.PixelFormat &&
		DepthStencilFormat == other.DepthStencilFormat;
}

size_t std::hash<MtPipelineKey>::operator()(const MtPipelineKey& key) const
{
	size_t hash = 0;
	hash ^= std::hash<int>()(key.VertexFormat);
	hash ^= std::hash<int>()(key.ShaderKey) << 1;
	hash ^= std::hash<int>()(key.BlendMode) << 2;
	hash ^= std::hash<int>()(key.DepthFunc) << 3;
	hash ^= std::hash<int>()(key.StencilOp) << 4;
	hash ^= std::hash<int>()(key.ColorMask) << 5;
	hash ^= std::hash<int>()(key.CullMode) << 6;
	hash ^= std::hash<int>()(key.DepthClampMode) << 7;
	hash ^= std::hash<int>()(key.SampleCount) << 8;
	hash ^= std::hash<int>()(key.DrawBufferCount) << 9;
	hash ^= std::hash<int>()(key.PixelFormat) << 10;
	hash ^= std::hash<int>()(key.DepthStencilFormat) << 11;
	return hash;
}

MtPipelineStateManager::MtPipelineStateManager(MetalRenderDevice* fb) : fb(fb) {}
MtPipelineStateManager::~MtPipelineStateManager() { ClearCache(); }

MtPipelineState* MtPipelineStateManager::GetPipelineState(const MtPipelineKey& key)
{
	auto it = mPipelineCache.find(key);
	if (it != mPipelineCache.end())
		return it->second.get();

	// Create new pipeline state
	auto state = std::make_unique<MtPipelineState>();
	state->Key = key;

	// Create render pipeline state
	state->pipelineState = CreateRenderPipelineState(key);
	if (!state->pipelineState)
		return nullptr;

	// Create depth/stencil state
	state->depthStencilState = CreateDepthStencilState(key);
	if (!state->depthStencilState)
	{
		((MTL::RenderPipelineState*)state->pipelineState)->release();
		return nullptr;
	}

	// Cache and return
	auto ptr = state.get();
	mPipelineCache[key] = std::move(state);
	return ptr;
}

void MtPipelineStateManager::ClearCache()
{
	for (auto& pair : mPipelineCache)
	{
		auto& state = pair.second;
		if (state)
		{
			if (state->pipelineState) ((MTL::RenderPipelineState*)state->pipelineState)->release();
			if (state->depthStencilState) ((MTL::DepthStencilState*)state->depthStencilState)->release();
		}
	}
	mPipelineCache.clear();
}

// ============================================================================
// Pipeline State Creation
// ============================================================================

void* MtPipelineStateManager::CreateDepthStencilState(const MtPipelineKey& key)
{
	auto desc = MTL::DepthStencilDescriptor::alloc()->init();

	// Map depth function enum to Metal
	static const MTL::CompareFunction depthFuncs[] = {
		MTL::CompareFunctionLess,       // DF_Less
		MTL::CompareFunctionLessEqual,  // DF_LEqual
		MTL::CompareFunctionAlways      // DF_Always
	};

	// Configure depth test
	if (key.DepthFunc >= 0 && key.DepthFunc < 3)
	{
		desc->setDepthCompareFunction(depthFuncs[key.DepthFunc]);
		desc->setDepthWriteEnabled((key.ColorMask & 8) != 0); // Bit 3 is depth write
	}
	else
	{
		desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
		desc->setDepthWriteEnabled(false);
	}

	// Configure stencil operations
	static const MTL::StencilOperation stencilOps[] = {
		MTL::StencilOperationKeep,              // SOP_Keep
		MTL::StencilOperationIncrementClamp,    // SOP_Increment
		MTL::StencilOperationDecrementClamp     // SOP_Decrement
	};

	if (key.StencilOp >= 0 && key.StencilOp < 3)
	{
		auto stencilDesc = MTL::StencilDescriptor::alloc()->init();
		stencilDesc->setStencilCompareFunction(MTL::CompareFunctionEqual);
		stencilDesc->setStencilFailureOperation(MTL::StencilOperationKeep);
		stencilDesc->setDepthFailureOperation(MTL::StencilOperationKeep);
		stencilDesc->setDepthStencilPassOperation(stencilOps[key.StencilOp]);
		stencilDesc->setReadMask(0xFFFFFFFF);
		stencilDesc->setWriteMask(0xFFFFFFFF);

		desc->setFrontFaceStencil(stencilDesc);
		desc->setBackFaceStencil(stencilDesc);
		stencilDesc->release();
	}

	// Create the state
	auto device = (MTL::Device*)fb->device->device;
	auto state = device->newDepthStencilState(desc);
	desc->release();

	return state;
}

void* MtPipelineStateManager::CreateRenderPipelineState(const MtPipelineKey& key)
{
	auto desc = MTL::RenderPipelineDescriptor::alloc()->init();

	// Get or create default shader
	// TODO: Implement proper shader selection based on key.ShaderKey
	auto device = (MTL::Device*)fb->device->device;

	// Create a simple default shader if not cached
	static MTL::Library* defaultLibrary = nullptr;
	static MTL::Function* vertexFunction = nullptr;
	static MTL::Function* fragmentFunction = nullptr;

	if (!defaultLibrary)
	{
		// Simple shader matching FFlatVertex structure with matrix transformations
		const char* shaderSource = R"(
			#include <metal_stdlib>
			using namespace metal;

			struct VertexIn {
				float3 position [[attribute(0)]];      // x, z, y
				float2 texCoord [[attribute(1)]];      // u, v
				float2 lightmapCoord [[attribute(2)]]; // lu, lv
				float lightmapIndex [[attribute(3)]];  // lindex
			};

			struct VertexOut {
				float4 position [[position]];
				float2 texCoord;
			};

			// Match HWViewpointUniforms structure
			struct Viewpoint {
				float4x4 ProjectionMatrix;
				float4x4 ViewMatrix;
				float4x4 NormalViewMatrix;
				// Additional fields omitted for now
			};

			// Match MatricesUBO structure
			struct Matrices {
				float4x4 ModelMatrix;
				float4x4 NormalModelMatrix;
				float4x4 TextureMatrix;
			};

			vertex VertexOut vertex_main(
				VertexIn in [[stage_in]],
				constant Viewpoint& viewpoint [[buffer(2)]],
				constant Matrices& matrices [[buffer(3)]]
			) {
				VertexOut out;
				// Transform: Projection * View * Model * position
				float4 worldPos = matrices.ModelMatrix * float4(in.position, 1.0);
				float4 eyePos = viewpoint.ViewMatrix * worldPos;
				out.position = viewpoint.ProjectionMatrix * eyePos;
				out.texCoord = in.texCoord;
				return out;
			}

			fragment float4 fragment_main(VertexOut in [[stage_in]]) {
				// Draw bright magenta for testing - should be VERY visible!
				return float4(1.0, 0.0, 1.0, 1.0);
			}
		)";

		NS::Error* error = nullptr;
		auto sourceStr = NS::String::string(shaderSource, NS::UTF8StringEncoding);
		defaultLibrary = device->newLibrary(sourceStr, nullptr, &error);

		if (!defaultLibrary)
		{
			if (error)
			{
				printf("Failed to compile default shader: %s\n",
					error->localizedDescription()->utf8String());
				error->release();
			}
			desc->release();
			return nullptr;
		}

		auto vertFuncName = NS::String::string("vertex_main", NS::UTF8StringEncoding);
		auto fragFuncName = NS::String::string("fragment_main", NS::UTF8StringEncoding);

		vertexFunction = defaultLibrary->newFunction(vertFuncName);
		fragmentFunction = defaultLibrary->newFunction(fragFuncName);

		if (!vertexFunction || !fragmentFunction)
		{
			printf("Failed to load shader functions from default library\n");
			desc->release();
			return nullptr;
		}
	}

	desc->setVertexFunction(vertexFunction);
	desc->setFragmentFunction(fragmentFunction);

	// Configure vertex descriptor to match FFlatVertex structure
	auto vertexDesc = MTL::VertexDescriptor::alloc()->init();

	// Attribute 0: Position (float3) - x, z, y
	vertexDesc->attributes()->object(0)->setFormat(MTL::VertexFormatFloat3);
	vertexDesc->attributes()->object(0)->setOffset(0);
	vertexDesc->attributes()->object(0)->setBufferIndex(0);

	// Attribute 1: TexCoord (float2) - u, v
	vertexDesc->attributes()->object(1)->setFormat(MTL::VertexFormatFloat2);
	vertexDesc->attributes()->object(1)->setOffset(12);
	vertexDesc->attributes()->object(1)->setBufferIndex(0);

	// Attribute 2: Lightmap TexCoord (float2) - lu, lv
	vertexDesc->attributes()->object(2)->setFormat(MTL::VertexFormatFloat2);
	vertexDesc->attributes()->object(2)->setOffset(20);
	vertexDesc->attributes()->object(2)->setBufferIndex(0);

	// Attribute 3: Lightmap Index (float) - lindex
	vertexDesc->attributes()->object(3)->setFormat(MTL::VertexFormatFloat);
	vertexDesc->attributes()->object(3)->setOffset(28);
	vertexDesc->attributes()->object(3)->setBufferIndex(0);

	// Buffer layout - FFlatVertex is 32 bytes
	vertexDesc->layouts()->object(0)->setStride(32);
	vertexDesc->layouts()->object(0)->setStepFunction(MTL::VertexStepFunctionPerVertex);

	desc->setVertexDescriptor(vertexDesc);
	vertexDesc->release();

	// Configure color attachments
	int numColorAttachments = key.DrawBufferCount;
	for (int i = 0; i < numColorAttachments; i++)
	{
		auto colorAttachment = desc->colorAttachments()->object(i);

		// Set pixel format
		if (key.PixelFormat != 0)
			colorAttachment->setPixelFormat((MTL::PixelFormat)key.PixelFormat);
		else
			colorAttachment->setPixelFormat(MTL::PixelFormatBGRA8Unorm); // Default

		// Configure blend mode
		ConfigureBlendMode(colorAttachment, key.BlendMode);

		// Set color write mask
		MTL::ColorWriteMask writeMask = MTL::ColorWriteMaskNone;
		if (key.ColorMask & 1) writeMask |= MTL::ColorWriteMaskRed;
		if (key.ColorMask & 2) writeMask |= MTL::ColorWriteMaskGreen;
		if (key.ColorMask & 4) writeMask |= MTL::ColorWriteMaskBlue;
		if (key.ColorMask & 8) writeMask |= MTL::ColorWriteMaskAlpha;
		colorAttachment->setWriteMask(writeMask);
	}

	// Configure depth/stencil format
	if (key.DepthStencilFormat != 0)
	{
		desc->setDepthAttachmentPixelFormat((MTL::PixelFormat)key.DepthStencilFormat);
		desc->setStencilAttachmentPixelFormat((MTL::PixelFormat)key.DepthStencilFormat);
	}

	// Configure sample count (MSAA)
	desc->setRasterSampleCount(key.SampleCount > 0 ? key.SampleCount : 1);

	// Culling is set dynamically via render command encoder, not in pipeline state
	// Metal doesn't support depth clamp in pipeline descriptor (requires feature check)

	// Create the pipeline state
	NS::Error* error = nullptr;
	auto state = device->newRenderPipelineState(desc, &error);

	if (!state && error)
	{
		printf("Failed to create render pipeline state: %s\n",
			error->localizedDescription()->utf8String());
		error->release();
	}

	desc->release();
	return state;
}

// Helper method to configure blend mode
void MtPipelineStateManager::ConfigureBlendMode(void* colorAttachment, int blendMode)
{
	auto attachment = (MTL::RenderPipelineColorAttachmentDescriptor*)colorAttachment;

	// TODO: Implement proper blend mode mapping from FRenderStyle
	// For now, use basic blend modes
	switch (blendMode)
	{
		case 0: // Normal (alpha blend)
			attachment->setBlendingEnabled(true);
			attachment->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
			attachment->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
			attachment->setRgbBlendOperation(MTL::BlendOperationAdd);
			attachment->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
			attachment->setDestinationAlphaBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
			attachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
			break;

		case 1: // Additive
			attachment->setBlendingEnabled(true);
			attachment->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
			attachment->setDestinationRGBBlendFactor(MTL::BlendFactorOne);
			attachment->setRgbBlendOperation(MTL::BlendOperationAdd);
			attachment->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
			attachment->setDestinationAlphaBlendFactor(MTL::BlendFactorOne);
			attachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
			break;

		case 2: // Subtractive
			attachment->setBlendingEnabled(true);
			attachment->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
			attachment->setDestinationRGBBlendFactor(MTL::BlendFactorOne);
			attachment->setRgbBlendOperation(MTL::BlendOperationReverseSubtract);
			attachment->setSourceAlphaBlendFactor(MTL::BlendFactorZero);
			attachment->setDestinationAlphaBlendFactor(MTL::BlendFactorOne);
			attachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
			break;

		default: // No blending
			attachment->setBlendingEnabled(false);
			break;
	}
}
