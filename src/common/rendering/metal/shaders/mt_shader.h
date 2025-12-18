#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include "matrix.h"
#include "hw_renderstate.h"

#ifdef __OBJC__
#import <Metal/Metal.h>
#else
#endif

class MetalRenderDevice;

// Shader uniform structures (matching Vulkan)
struct MatricesUBO
{
	VSMatrix ModelMatrix;
	VSMatrix NormalModelMatrix;
	VSMatrix TextureMatrix;
};

// Push constants structure (small frequently-updated uniforms)
//
// Terminology note:
// - Vulkan: "Push Constants" (vkCmdPushConstants)
// - Metal: "Inline Buffers" via setVertexBytes/setFragmentBytes
// - NOT to be confused with Metal "Function Constants" which are compile-time specialization
//
// Metal implementation: passed via setVertexBytes/setFragmentBytes (< 4KB inline data)
struct PushConstants
{
	int uTextureMode;
	float uAlphaThreshold;
	FVector2 uClipSplit;

	// Lighting + Fog
	float uLightLevel;
	float uFogDensity;
	float uLightFactor;
	float uLightDist;
	int uFogEnabled;

	// Dynamic lights
	int uLightIndex;

	// Blinn glossiness and specular level
	FVector2 uSpecularMaterial;

	// Bone animation
	int uBoneIndexBase;

	// Stream data index
	int uDataIndex;

	// Padding to align to 16 bytes
	int padding[2];
};

#define MAX_STREAM_DATA ((int)(65536 / sizeof(StreamData)))

struct StreamUBO
{
	StreamData data[MAX_STREAM_DATA];
};

// Shader compilation result
struct MtShaderModule
{
#ifdef __OBJC__
	id<MTLLibrary> library = nil;
	id<MTLFunction> function = nil;
	id<MTLFunction> fragmentFunction = nil;
	id<MTLLibrary> fragmentLibrary = nil;
#else
	void* library = nullptr;
	void* function = nullptr;
	void* fragmentFunction = nullptr;
	void* fragmentLibrary = nullptr;
#endif
	std::string name;
	std::string entryPoint;
};

// Shader manager - handles GLSL → SPIR-V → MSL compilation
class MtShaderManager
{
public:
	MtShaderManager(MetalRenderDevice* fb);
	~MtShaderManager();

	// Compile shader from GLSL source
	// Pipeline: GLSL → glslang → SPIR-V → shader-translator → MSL → MTLLibrary
	std::shared_ptr<MtShaderModule> CompileShader(
		const std::string& name,
		const std::string& vertexSource,
		const std::string& fragmentSource,
		const std::vector<std::string>& defines);

	// Get compiled shader
	std::shared_ptr<MtShaderModule> GetShader(const std::string& name, int variant);

	// Compile next shader in queue (for incremental compilation)
	bool CompileNextShader();

	// Clear cache
	void ClearCache();

private:
	// Compile GLSL to SPIR-V (reuse Vulkan logic)
	std::vector<uint32_t> CompileGLSLToSPIRV(
		const std::string& source,
		const std::string& name,
		bool isVertex,
		const std::vector<std::string>& defines);

	// Translate SPIR-V to MSL (using shader-translator)
	std::string TranslateSPIRVToMSL(const std::vector<uint32_t>& spirv, bool isVertex);

	// Compile MSL to MTLLibrary
#ifdef __OBJC__
	id<MTLLibrary> CompileMSLToLibrary(const std::string& msl, const std::string& name);
#else
	void* CompileMSLToLibrary(const std::string& msl, const std::string& name);
#endif

	MetalRenderDevice* fb = nullptr;
	std::unordered_map<std::string, std::shared_ptr<MtShaderModule>> mShaderCache;
};
