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
#include "printf.h"

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

	auto shaderManager = fb->GetShaderManager();
	auto module = shaderManager->CompileShader("default", 
		R"(
			#version 450
			// GLSL Vertex Shader

			layout(location = 0) in vec3 position;
			layout(location = 1) in vec2 texCoord;
			layout(location = 2) in vec4 color0; // Vertex color

			layout(location = 0) out vec4 out_position;
			layout(location = 1) out vec2 out_texCoord;
			layout(location = 2) out vec4 out_color0; // Pass vertex color to fragment shader

			layout(binding = 2, std140) uniform ViewpointUBO {
				mat4 ProjectionMatrix;
				mat4 ViewMatrix;
				mat4 NormalViewMatrix;
			} viewpoint;

			layout(binding = 3, std140) uniform MatricesUBO {
				mat4 ModelMatrix;
				mat4 NormalModelMatrix;
				mat4 TextureMatrix;
			} matrices;

			void main() {
				// Transform: Projection * View * Model * position
				vec4 worldPos = matrices.ModelMatrix * vec4(position, 1.0);
				vec4 eyePos = viewpoint.ViewMatrix * worldPos;
				gl_Position = viewpoint.ProjectionMatrix * eyePos; // Use gl_Position for clip-space output
				out_texCoord = texCoord;
				out_color0 = color0; // Pass vertex color
			}
		)",
		R"(
			#version 450
			// GLSL Fragment Shader

			layout(location = 1) in vec2 in_texCoord; // Matches out_texCoord from vertex shader
			layout(location = 2) in vec4 in_color0; // Input vertex color from vertex shader

			// StreamData uniform buffer (from MtStreamBufferWriter)
			layout(binding = 0, std140) uniform StreamDataUBO {
				vec4 uObjectColor;
				vec4 uObjectColor2;
				vec4 uDynLightColor;
				vec4 uAddColor;
				vec4 uTextureAddColor;
				vec4 uTextureModulateColor;
				vec4 uTextureBlendColor;
				vec4 uFogColor;
				float uDesaturationFactor;
				float uInterpolationFactor;
				float timer;
				int useVertexData;
				vec4 uVertexColor;
				vec4 uVertexNormal;

				vec4 uGlowTopPlane;
				vec4 uGlowTopColor;
				vec4 uGlowBottomPlane;
				vec4 uGlowBottomColor;

				vec4 uGradientTopPlane;
				vec4 uGradientBottomPlane;

				vec4 uSplitTopPlane;
				vec4 uSplitBottomPlane;

				vec4 uDetailParms;
				vec4 uNpotEmulation;
				vec4 padding1;
				vec4 padding2;
				vec4 padding3;
			} streamData;

			// PushConstants uniform buffer (from MtRenderState::ApplyPushConstants)
			layout(binding = 1, std140) uniform PushConstantsUBO {
				int uTextureMode;
				float uAlphaThreshold;
				vec2 uClipSplit;

				// Lighting + Fog
				float uLightLevel;
				float uFogDensity;
				float uLightFactor;
				float uLightDist;
				int uFogEnabled;

				// Dynamic lights
			int uLightIndex;

				// Blinn glossiness and specular level
				vec2 uSpecularMaterial;

				// Bone animation
			int uBoneIndexBase;

				// Stream data index
			int uDataIndex;

				// Padding to align to 16 bytes
			int padding[2];
			} pushConstants;


			layout(binding = 0) uniform sampler2D diffuseTexture; // Texture binding, separate space from buffer bindings

			layout(location = 0) out vec4 fragColor; // Output fragment color

			void main() {
				// Sample texture
				vec4 color = texture(diffuseTexture, in_texCoord);

				// If texture is mostly black (unbound), show magenta for debugging
				if (color.r < 0.01 && color.g < 0.01 && color.b < 0.01) {
					fragColor = vec4(1.0, 0.0, 1.0, 1.0);  // Magenta = no texture
				} else {
					// Apply object color and add color from streamData and pushConstants
					vec4 finalColor = color * streamData.uObjectColor + streamData.uAddColor;

					// Multiply with vertex color (for tinting) - Swizzle to correct BGRA to RGBA
					finalColor *= in_color0.bgra;

					// Apply push constants (e.g., alpha threshold)
					// if (finalColor.a < pushConstants.uAlphaThreshold) {
					// 	discard; // Discard fragment if below alpha threshold
					// }
					fragColor = finalColor;
				}
			}
		)",
		{});

	if (!module)
	{
		Printf("Metal: Failed to get default shader\n");
		desc->release();
		return nullptr;
	}
	
	auto vertexFunction = (MTL::Function*)module->function;
	auto fragmentFunction = (MTL::Function*)module->fragmentFunction;

	if (!vertexFunction || !fragmentFunction)
	{
		Printf("Metal: Failed to load shader functions from default library\n");
		desc->release();
		return nullptr;
	}

	desc->setVertexFunction(vertexFunction);
	desc->setFragmentFunction(fragmentFunction);

	// Configure vertex descriptor to match F2DDrawer::TwoDVertex structure
	auto vertexDesc = MTL::VertexDescriptor::alloc()->init();

	// Attribute 0: Position (float3) - x, y, z (z is depth)
	vertexDesc->attributes()->object(0)->setFormat(MTL::VertexFormatFloat3);
	vertexDesc->attributes()->object(0)->setOffset(0);
	vertexDesc->attributes()->object(0)->setBufferIndex(0);

	// Attribute 1: TexCoord (float2) - u, v
	vertexDesc->attributes()->object(1)->setFormat(MTL::VertexFormatFloat2);
	vertexDesc->attributes()->object(1)->setOffset(12);
	vertexDesc->attributes()->object(1)->setBufferIndex(0);

	// Attribute 2: Color (uchar4 normalized) - color0
	vertexDesc->attributes()->object(2)->setFormat(MTL::VertexFormatUChar4Normalized);
	vertexDesc->attributes()->object(2)->setOffset(20);
	vertexDesc->attributes()->object(2)->setBufferIndex(0);

	// Buffer layout - F2DDrawer::TwoDVertex is 24 bytes
	vertexDesc->layouts()->object(0)->setStride(24);
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
		Printf("Metal: Failed to create render pipeline state: %s\n",
			error->localizedDescription()->utf8String());
		error->release();
	}
	else if (state)
	{
		Printf("Metal: Pipeline state created successfully (shader key=%d, vertex format=%d)\n",
			key.ShaderKey, key.VertexFormat);
	}

	desc->release();
	return state;
}

// Helper method to configure blend mode
void MtPipelineStateManager::ConfigureBlendMode(void* colorAttachment, int blendMode)
{
	auto attachment = (MTL::RenderPipelineColorAttachmentDescriptor*)colorAttachment;

	switch (blendMode)
	{
		case STYLEOP_Add: // This is the blend mode used by STYLE_Normal
			attachment->setBlendingEnabled(true);
			attachment->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
			attachment->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
			attachment->setRgbBlendOperation(MTL::BlendOperationAdd);
			attachment->setSourceAlphaBlendFactor(MTL::BlendFactorSourceAlpha);
			attachment->setDestinationAlphaBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
			attachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
			break;

		case STYLEOP_RevSub: // This is the blend mode used by STYLE_Subtract
			attachment->setBlendingEnabled(true);
			attachment->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha); // Typical for subtractive, can be adjusted
			attachment->setDestinationRGBBlendFactor(MTL::BlendFactorOne);
			attachment->setRgbBlendOperation(MTL::BlendOperationReverseSubtract);
			attachment->setSourceAlphaBlendFactor(MTL::BlendFactorZero); // Typical for subtractive, can be adjusted
			attachment->setDestinationAlphaBlendFactor(MTL::BlendFactorOne);
			attachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
			break;

		case STYLEOP_None:
			attachment->setBlendingEnabled(false);
			break;
		
		case STYLEOP_Shadow: // Shadow blend mode
			attachment->setBlendingEnabled(true);
			attachment->setSourceRGBBlendFactor(MTL::BlendFactorZero);
			attachment->setDestinationRGBBlendFactor(MTL::BlendFactorSourceAlpha); // Use source alpha to dim background
			attachment->setRgbBlendOperation(MTL::BlendOperationAdd);
			attachment->setSourceAlphaBlendFactor(MTL::BlendFactorZero);
			attachment->setDestinationAlphaBlendFactor(MTL::BlendFactorOne);
			attachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
			break;


		default: // For any other unhandled blend mode, disable blending for now and warn
			Printf("Metal: Warning - Unhandled blendMode: %d. Disabling blending.\n", blendMode);
			attachment->setBlendingEnabled(false);
			break;
	}
}