#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

#ifdef __OBJC__
#import <Metal/Metal.h>
#else
typedef void* id;
#endif

class MetalRenderDevice;

// Shader compilation result
struct MtShaderModule
{
#ifdef __OBJC__
	id<MTLLibrary> library = nil;
	id<MTLFunction> function = nil;
#else
	void* library = nullptr;
	void* function = nullptr;
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
