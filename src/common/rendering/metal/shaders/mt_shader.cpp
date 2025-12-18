/*
**  Metal backend - Shader management
**  Copyright (c) 2025 GZDoom Contributors
*/

#include <Metal/Metal.hpp>
#include "mt_shader.h"
#include "metal/system/mt_renderdevice.h"
#include <shadertranslator/shader_translator.h>
#include "cmdlib.h"
#include "printf.h"
#include <sstream>

// glslang includes for GLSL → SPIR-V compilation
#include "glslang/glslang/Public/ShaderLang.h"
#include "glslang/spirv/GlslangToSpv.h"

// Helper function to get default glslang resources
static TBuiltInResource GetDefaultTBuiltInResource()
{
	TBuiltInResource resources = {};
	resources.maxLights = 32;
	resources.maxClipPlanes = 6;
	resources.maxTextureUnits = 32;
	resources.maxTextureCoords = 32;
	resources.maxVertexAttribs = 64;
	resources.maxVertexUniformComponents = 4096;
	resources.maxVaryingFloats = 64;
	resources.maxVertexTextureImageUnits = 32;
	resources.maxCombinedTextureImageUnits = 80;
	resources.maxTextureImageUnits = 32;
	resources.maxFragmentUniformComponents = 4096;
	resources.maxDrawBuffers = 32;
	resources.maxVertexUniformVectors = 128;
	resources.maxVaryingVectors = 8;
	resources.maxFragmentUniformVectors = 16;
	resources.maxVertexOutputVectors = 16;
	resources.maxFragmentInputVectors = 15;
	resources.minProgramTexelOffset = -8;
	resources.maxProgramTexelOffset = 7;
	resources.maxClipDistances = 8;
	resources.maxComputeWorkGroupCountX = 65535;
	resources.maxComputeWorkGroupCountY = 65535;
	resources.maxComputeWorkGroupCountZ = 65535;
	resources.maxComputeWorkGroupSizeX = 1024;
	resources.maxComputeWorkGroupSizeY = 1024;
	resources.maxComputeWorkGroupSizeZ = 64;
	resources.maxComputeUniformComponents = 1024;
	resources.maxComputeTextureImageUnits = 16;
	resources.maxComputeImageUniforms = 8;
	resources.maxComputeAtomicCounters = 8;
	resources.maxComputeAtomicCounterBuffers = 1;
	resources.maxVaryingComponents = 60;
	resources.maxVertexOutputComponents = 64;
	resources.maxGeometryInputComponents = 64;
	resources.maxGeometryOutputComponents = 128;
	resources.maxFragmentInputComponents = 128;
	resources.maxImageUnits = 8;
	resources.maxCombinedImageUnitsAndFragmentOutputs = 8;
	resources.maxCombinedShaderOutputResources = 8;
	resources.maxImageSamples = 0;
	resources.maxVertexImageUniforms = 0;
	resources.maxTessControlImageUniforms = 0;
	resources.maxTessEvaluationImageUniforms = 0;
	resources.maxGeometryImageUniforms = 0;
	resources.maxFragmentImageUniforms = 8;
	resources.maxCombinedImageUniforms = 8;
	resources.maxGeometryTextureImageUnits = 16;
	resources.maxGeometryOutputVertices = 256;
	resources.maxGeometryTotalOutputComponents = 1024;
	resources.maxGeometryUniformComponents = 1024;
	resources.maxGeometryVaryingComponents = 64;
	resources.maxTessControlInputComponents = 128;
	resources.maxTessControlOutputComponents = 128;
	resources.maxTessControlTextureImageUnits = 16;
	resources.maxTessControlUniformComponents = 1024;
	resources.maxTessControlTotalOutputComponents = 4096;
	resources.maxTessEvaluationInputComponents = 128;
	resources.maxTessEvaluationOutputComponents = 128;
	resources.maxTessEvaluationTextureImageUnits = 16;
	resources.maxTessEvaluationUniformComponents = 1024;
	resources.maxTessPatchComponents = 120;
	resources.maxPatchVertices = 32;
	resources.maxTessGenLevel = 64;
	resources.maxViewports = 16;
	resources.maxVertexAtomicCounters = 0;
	resources.maxTessControlAtomicCounters = 0;
	resources.maxTessEvaluationAtomicCounters = 0;
	resources.maxGeometryAtomicCounters = 0;
	resources.maxFragmentAtomicCounters = 8;
	resources.maxCombinedAtomicCounters = 8;
	resources.maxAtomicCounterBindings = 1;
	resources.maxVertexAtomicCounterBuffers = 0;
	resources.maxTessControlAtomicCounterBuffers = 0;
	resources.maxTessEvaluationAtomicCounterBuffers = 0;
	resources.maxGeometryAtomicCounterBuffers = 0;
	resources.maxFragmentAtomicCounterBuffers = 1;
	resources.maxCombinedAtomicCounterBuffers = 1;
	resources.maxAtomicCounterBufferSize = 16384;
	resources.maxTransformFeedbackBuffers = 4;
	resources.maxTransformFeedbackInterleavedComponents = 64;
	resources.maxCullDistances = 8;
	resources.maxCombinedClipAndCullDistances = 8;
	resources.maxSamples = 4;
	resources.maxMeshOutputVerticesNV = 256;
	resources.maxMeshOutputPrimitivesNV = 512;
	resources.maxMeshWorkGroupSizeX_NV = 32;
	resources.maxMeshWorkGroupSizeY_NV = 1;
	resources.maxMeshWorkGroupSizeZ_NV = 1;
	resources.maxTaskWorkGroupSizeX_NV = 32;
	resources.maxTaskWorkGroupSizeY_NV = 1;
	resources.maxTaskWorkGroupSizeZ_NV = 1;
	resources.maxMeshViewCountNV = 4;
	resources.maxDualSourceDrawBuffersEXT = 1;
	resources.limits.nonInductiveForLoops = true;
	resources.limits.whileLoops = true;
	resources.limits.doWhileLoops = true;
	resources.limits.generalUniformIndexing = true;
	resources.limits.generalAttributeMatrixVectorIndexing = true;
	resources.limits.generalVaryingIndexing = true;
	resources.limits.generalSamplerIndexing = true;
	resources.limits.generalVariableIndexing = true;
	resources.limits.generalConstantMatrixVectorIndexing = true;
	return resources;
}

MtShaderManager::MtShaderManager(MetalRenderDevice* fb) : fb(fb)
{
	// Initialize glslang (once per program)
	glslang::InitializeProcess();
}

MtShaderManager::~MtShaderManager()
{
	ClearCache();
	// Finalize glslang
	glslang::FinalizeProcess();
}

std::shared_ptr<MtShaderModule> MtShaderManager::CompileShader(
	const std::string& name,
	const std::string& vertexSource,
	const std::string& fragmentSource,
	const std::vector<std::string>& defines)
{
	// Check cache first
	auto it = mShaderCache.find(name);
	if (it != mShaderCache.end())
		return it->second;

	// Create shader module
	auto module = std::make_shared<MtShaderModule>();
	module->name = name;

	// Compile vertex shader: GLSL → SPIR-V → MSL → MTLLibrary
	if (!vertexSource.empty())
	{
		// Step 1: GLSL → SPIR-V
		auto vertexSPIRV = CompileGLSLToSPIRV(vertexSource, name + "_vert", true, defines);
		if (vertexSPIRV.empty())
		{
			Printf("Metal: Failed to compile vertex shader GLSL to SPIR-V: %s\n", name.c_str());
			return nullptr;
		}

		// Step 2: SPIR-V → MSL
		std::string vertexMSL = TranslateSPIRVToMSL(vertexSPIRV, true);
		if (vertexMSL.empty())
		{
			Printf("Metal: Failed to translate vertex shader SPIR-V to MSL: %s\n", name.c_str());
			return nullptr;
		}

		// Step 3: MSL → MTLLibrary
		module->library = CompileMSLToLibrary(vertexMSL, name + "_vert");
		if (!module->library)
		{
			Printf("Metal: Failed to compile vertex shader MSL to library: %s\n", name.c_str());
			return nullptr;
		}

		// Get main function
		NS::String* funcName = NS::String::string("main0", NS::UTF8StringEncoding);
		MTL::Library* lib = (MTL::Library*)module->library;
		module->function = lib->newFunction(funcName);
		funcName->release();

		if (!module->function)
		{
			Printf("Metal: Failed to find main0 function in vertex shader: %s\n", name.c_str());
			((MTL::Library*)module->library)->release();
			return nullptr;
		}

		module->entryPoint = "main0";
	}

	// Compile fragment shader: GLSL → SPIR-V → MSL → MTLLibrary
	if (!fragmentSource.empty())
	{
		// Step 1: GLSL → SPIR-V
		auto fragmentSPIRV = CompileGLSLToSPIRV(fragmentSource, name + "_frag", false, defines);
		if (fragmentSPIRV.empty())
		{
			Printf("Metal: Failed to compile fragment shader GLSL to SPIR-V: %s\n", name.c_str());
			if (module->function) ((MTL::Function*)module->function)->release();
			if (module->library) ((MTL::Library*)module->library)->release();
			return nullptr;
		}

		// Step 2: SPIR-V → MSL
		std::string fragmentMSL = TranslateSPIRVToMSL(fragmentSPIRV, false);
		if (fragmentMSL.empty())
		{
			Printf("Metal: Failed to translate fragment shader SPIR-V to MSL: %s\n", name.c_str());
			if (module->function) ((MTL::Function*)module->function)->release();
			if (module->library) ((MTL::Library*)module->library)->release();
			return nullptr;
		}

		// Step 3: MSL → MTLLibrary
		module->fragmentLibrary = CompileMSLToLibrary(fragmentMSL, name + "_frag");
		if (!module->fragmentLibrary)
		{
			Printf("Metal: Failed to compile fragment shader MSL to library: %s\n", name.c_str());
			if (module->function) ((MTL::Function*)module->function)->release();
			if (module->library) ((MTL::Library*)module->library)->release();
			return nullptr;
		}

		// Get main function for fragment shader
		NS::String* fragFuncName = NS::String::string("main0", NS::UTF8StringEncoding);
		MTL::Library* fragLib = (MTL::Library*)module->fragmentLibrary;
		module->fragmentFunction = fragLib->newFunction(fragFuncName);
		fragFuncName->release();

		if (!module->fragmentFunction)
		{
			Printf("Metal: Failed to find main0 function in fragment shader: %s\n", name.c_str());
			((MTL::Library*)module->fragmentLibrary)->release();
			if (module->function) ((MTL::Function*)module->function)->release();
			if (module->library) ((MTL::Library*)module->library)->release();
			return nullptr;
		}
	}

	// Cache the compiled shader
	mShaderCache[name] = module;

	return module;
}

std::shared_ptr<MtShaderModule> MtShaderManager::GetShader(const std::string& name, int variant)
{
	auto it = mShaderCache.find(name);
	if (it != mShaderCache.end())
		return it->second;
	return nullptr;
}

bool MtShaderManager::CompileNextShader()
{
	// TODO: Implement incremental shader compilation
	return false;
}

void MtShaderManager::ClearCache()
{
	for (auto& pair : mShaderCache)
	{
		auto& module = pair.second;
		if (module)
		{
			if (module->function) ((MTL::Function*)module->function)->release();
			if (module->fragmentFunction) ((MTL::Function*)module->fragmentFunction)->release();
			if (module->library) ((MTL::Library*)module->library)->release();
			if (module->fragmentLibrary) ((MTL::Library*)module->fragmentLibrary)->release();
		}
	}
	mShaderCache.clear();
}

std::vector<uint32_t> MtShaderManager::CompileGLSLToSPIRV(
	const std::string& source,
	const std::string& name,
	bool isVertex,
	const std::vector<std::string>& defines)
{
	// Determine shader stage
	EShLanguage stage = isVertex ? EShLangVertex : EShLangFragment;

	// Prepare source:
	// 1. Remove Metal-specific includes that glslang cannot handle.
	// 2. Prepend the GL_GOOGLE_include_directive extension.
	// 3. Add defines.
	std::string processedSource;
	std::stringstream ss(source);
	std::string line;
	while (std::getline(ss, line)) {
		// Remove lines that explicitly include metal_stdlib or use namespace metal as glslang cannot process them
		if (line.find("#include <metal_stdlib>") == std::string::npos &&
			line.find("using namespace metal;") == std::string::npos) {
			processedSource += line + "\n";
		}
	}

		std::string finalSource = processedSource;		
			const char* sourceStr = finalSource.c_str();	int sourceLength = static_cast<int>(finalSource.length());
	const char* nameStr = name.c_str();

	// Create glslang shader
	TBuiltInResource resources = GetDefaultTBuiltInResource();
	glslang::TShader shader(stage);
	shader.setStringsWithLengthsAndNames(&sourceStr, &sourceLength, &nameStr, 1);

	// Configure for Metal/MSL target (similar to Vulkan)
	shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
	shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
	shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

	// Parse shader
	// EShMsgVulkanRules helps enforce Vulkan GLSL best practices for SPIR-V output.
	bool parseSuccess = shader.parse(&resources, 450, false, EShMsgVulkanRules); // Use EShMsgVulkanRules
	if (!parseSuccess)
	{
		Printf("Metal: Shader parse failed for %s:\n%s\n", name.c_str(), shader.getInfoLog());
		return std::vector<uint32_t>();
	}

	// Link into program
	glslang::TProgram program;
	program.addShader(&shader);
	bool linkSuccess = program.link(EShMsgDefault);
	if (!linkSuccess)
	{
		Printf("Metal: Shader link failed for %s:\n%s\n", name.c_str(), program.getInfoLog());
		return std::vector<uint32_t>();
	}

	// Get intermediate representation
	glslang::TIntermediate* intermediate = program.getIntermediate(stage);
	if (!intermediate)
	{
		Printf("Metal: Failed to get intermediate representation for %s\n", name.c_str());
		return std::vector<uint32_t>();
	}

	// Convert to SPIR-V
	std::vector<uint32_t> spirv;
	glslang::SpvOptions spvOptions;
	spvOptions.generateDebugInfo = false;
	spvOptions.disableOptimizer = false;
	spvOptions.optimizeSize = true;

	spv::SpvBuildLogger logger;
	glslang::GlslangToSpv(*intermediate, spirv, &logger, &spvOptions);

	// Log any messages
	std::string messages = logger.getAllMessages();
	if (!messages.empty())
	{
		Printf("Metal: SPIR-V generation messages for %s:\n%s\n", name.c_str(), messages.c_str());
	}

	return spirv;
}

std::string MtShaderManager::TranslateSPIRVToMSL(const std::vector<uint32_t>& spirv, bool isVertex)
{
	if (spirv.empty())
		return "";

	// Create SPIR-V translator
	ShaderTranslator::SPIRVTranslator translator;

	// Translate SPIR-V to MSL (Metal 2.0 for macOS 10.13+)
	auto result = translator.TranslateToMSL(spirv, 20);

	if (!result.success)
	{
		Printf("Metal SPIR-V translation error: %s\n", result.errorLog.c_str());
		return "";
	}

	return result.source;
}

void* MtShaderManager::CompileMSLToLibrary(const std::string& msl, const std::string& name)
{
	if (msl.empty()) return nullptr;

	NS::String* source = NS::String::string(msl.c_str(), NS::UTF8StringEncoding);
	NS::Error* error = nullptr;

	MTL::Library* library = fb->device->device->newLibrary(source, nullptr, &error);

	if (!library && error)
	{
		const char* errorMsg = error->localizedDescription()->utf8String();
		Printf("Metal shader compilation error: %s\n", errorMsg);
	}

	return library;
}
