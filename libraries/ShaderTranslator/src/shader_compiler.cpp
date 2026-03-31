/*
** shader_compiler.cpp
**
**---------------------------------------------------------------------------
** Copyright 2025 GZDoom Maintainers and Contributors
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#include "shadertranslator/shader_translator.h"
#include <spirv_msl.hpp>
#ifdef SHADER_TRANSLATOR_HLSL_ENABLED
#include <spirv_hlsl.hpp>
#endif
#include <spirv_glsl.hpp>
#include <stdexcept>

// glslang includes for GLSL → SPIR-V compilation
#include "glslang/Public/ShaderLang.h"
#include "SPIRV/GlslangToSpv.h"

#ifdef SHADER_TRANSLATOR_HLSL_INPUT_ENABLED
// DXC includes for HLSL → SPIR-V compilation
#include <dxc/dxcapi.h>
#endif

namespace ShaderTranslator
{

// Helper: Get glslang shader stage
static EShLanguage GetGlslangStage(ShaderStage stage)
{
	switch (stage)
	{
	case ShaderStage::Vertex:   return EShLangVertex;
	case ShaderStage::Fragment: return EShLangFragment;
	case ShaderStage::Compute:  return EShLangCompute;
	default: return EShLangVertex;
	}
}

// Helper: Get default glslang resources
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

// ShaderCompiler implementation
class ShaderCompiler::Impl
{
public:
	SPIRVTranslator spirvTranslator;
	std::vector<uint32_t> lastSPIRV;
	static bool glslangInitialized;

	Impl()
	{
		if (!glslangInitialized)
		{
			glslang::InitializeProcess();
			glslangInitialized = true;
		}
	}

	~Impl()
	{
		// Note: We don't call FinalizeProcess() because there might be other instances
	}

	// Compile GLSL to SPIR-V
	std::vector<uint32_t> CompileGLSLToSPIRV(
		const std::string& source,
		ShaderStage stage,
		const std::vector<std::string>& defines)
	{
		EShLanguage glslangStage = GetGlslangStage(stage);

		// Prepare source with defines
		std::string fullSource;
		fullSource += "#version 450\n";
		for (const auto& define : defines)
		{
			fullSource += "#define " + define + "\n";
		}
		fullSource += source;

		const char* sourceStr = fullSource.c_str();
		int sourceLength = static_cast<int>(fullSource.length());
		const char* nameStr = "shader";

		// Create glslang shader
		TBuiltInResource resources = GetDefaultTBuiltInResource();
		glslang::TShader shader(glslangStage);
		shader.setStringsWithLengthsAndNames(&sourceStr, &sourceLength, &nameStr, 1);

		// Configure for Vulkan SPIR-V target
		shader.setEnvInput(glslang::EShSourceGlsl, glslangStage, glslang::EShClientVulkan, 100);
		shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
		shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

		// Parse shader
		if (!shader.parse(&resources, 450, false, EShMsgDefault))
		{
			throw std::runtime_error(std::string("GLSL parse failed: ") + shader.getInfoLog());
		}

		// Link into program
		glslang::TProgram program;
		program.addShader(&shader);
		if (!program.link(EShMsgDefault))
		{
			throw std::runtime_error(std::string("GLSL link failed: ") + program.getInfoLog());
		}

		// Get intermediate representation
		glslang::TIntermediate* intermediate = program.getIntermediate(glslangStage);
		if (!intermediate)
		{
			throw std::runtime_error("Failed to get intermediate representation");
		}

		// Convert to SPIR-V
		std::vector<uint32_t> spirv;
		glslang::SpvOptions spvOptions;
		spvOptions.generateDebugInfo = false;
		spvOptions.disableOptimizer = false;
		spvOptions.optimizeSize = true;

		spv::SpvBuildLogger logger;
		glslang::GlslangToSpv(*intermediate, spirv, &logger, &spvOptions);

		return spirv;
	}

#ifdef SHADER_TRANSLATOR_HLSL_INPUT_ENABLED
	// Compile HLSL to SPIR-V using DXC
	std::vector<uint32_t> CompileHLSLToSPIRV(
		const std::string& source,
		ShaderStage stage,
		const std::vector<std::string>& defines)
	{
		// DXC instance and compiler
		IDxcUtils* utils = nullptr;
		IDxcCompiler3* compiler = nullptr;

		HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
		if (FAILED(hr))
		{
			throw std::runtime_error("Failed to create DXC utils instance");
		}

		hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
		if (FAILED(hr))
		{
			utils->Release();
			throw std::runtime_error("Failed to create DXC compiler instance");
		}

		// Determine shader profile and entry point
		const wchar_t* profile = nullptr;
		const wchar_t* entryPoint = L"main";

		switch (stage)
		{
		case ShaderStage::Vertex:   profile = L"vs_6_0"; break;
		case ShaderStage::Fragment: profile = L"ps_6_0"; break;
		case ShaderStage::Compute:  profile = L"cs_6_0"; break;
		}

		// Create source blob
		IDxcBlobEncoding* sourceBlob = nullptr;
		hr = utils->CreateBlob(source.c_str(), source.length(), CP_UTF8, &sourceBlob);
		if (FAILED(hr))
		{
			compiler->Release();
			utils->Release();
			throw std::runtime_error("Failed to create source blob");
		}

		// Build arguments for SPIR-V generation
		std::vector<LPCWSTR> arguments;
		arguments.push_back(L"-spirv");                  // Generate SPIR-V
		arguments.push_back(L"-fspv-target-env=vulkan1.0");
		arguments.push_back(L"-E");
		arguments.push_back(entryPoint);
		arguments.push_back(L"-T");
		arguments.push_back(profile);

		// Add defines
		std::vector<std::wstring> wideDefines;
		for (const auto& define : defines)
		{
			std::wstring wideDefine = L"-D";
			wideDefine += std::wstring(define.begin(), define.end());
			wideDefines.push_back(wideDefine);
			arguments.push_back(wideDefines.back().c_str());
		}

		// Compile
		DxcBuffer sourceBuffer;
		sourceBuffer.Ptr = sourceBlob->GetBufferPointer();
		sourceBuffer.Size = sourceBlob->GetBufferSize();
		sourceBuffer.Encoding = 0;

		IDxcResult* result = nullptr;
		hr = compiler->Compile(&sourceBuffer, arguments.data(), arguments.size(),
		                       nullptr, IID_PPV_ARGS(&result));

		sourceBlob->Release();

		if (FAILED(hr) || result == nullptr)
		{
			compiler->Release();
			utils->Release();
			throw std::runtime_error("HLSL compilation failed");
		}

		// Check compilation status
		HRESULT compileStatus;
		result->GetStatus(&compileStatus);

		if (FAILED(compileStatus))
		{
			IDxcBlobEncoding* errorBlob = nullptr;
			result->GetErrorBuffer(&errorBlob);

			std::string errorMsg = "HLSL compilation error: ";
			if (errorBlob)
			{
				errorMsg += std::string((char*)errorBlob->GetBufferPointer(),
				                       errorBlob->GetBufferSize());
				errorBlob->Release();
			}

			result->Release();
			compiler->Release();
			utils->Release();
			throw std::runtime_error(errorMsg);
		}

		// Get SPIR-V bytecode
		IDxcBlob* spirvBlob = nullptr;
		hr = result->GetResult(&spirvBlob);
		if (FAILED(hr) || spirvBlob == nullptr)
		{
			result->Release();
			compiler->Release();
			utils->Release();
			throw std::runtime_error("Failed to get SPIR-V bytecode");
		}

		// Copy SPIR-V to vector
		const uint32_t* spirvData = (const uint32_t*)spirvBlob->GetBufferPointer();
		size_t spirvSize = spirvBlob->GetBufferSize() / sizeof(uint32_t);
		std::vector<uint32_t> spirv(spirvData, spirvData + spirvSize);

		// Cleanup
		spirvBlob->Release();
		result->Release();
		compiler->Release();
		utils->Release();

		return spirv;
	}
#endif
};

bool ShaderCompiler::Impl::glslangInitialized = false;

// ShaderCompiler public methods
ShaderCompiler::ShaderCompiler()
	: impl(new Impl())
{
}

ShaderCompiler::~ShaderCompiler()
{
	delete impl;
}

TranslationResult ShaderCompiler::Compile(
	const std::string& sourceCode,
	SourceLanguage sourceLanguage,
	ShaderStage stage,
	TargetLanguage targetLanguage,
	const PlatformVersion& platformVersion,
	const std::vector<std::string>& defines)
{
	TranslationResult result;

	try
	{
		// Step 1: Compile source to SPIR-V
		std::vector<uint32_t> spirv;

		if (sourceLanguage == SourceLanguage::GLSL)
		{
			spirv = impl->CompileGLSLToSPIRV(sourceCode, stage, defines);
		}
#ifdef SHADER_TRANSLATOR_HLSL_INPUT_ENABLED
		else if (sourceLanguage == SourceLanguage::HLSL)
		{
			spirv = impl->CompileHLSLToSPIRV(sourceCode, stage, defines);
		}
#endif
		else if (sourceLanguage == SourceLanguage::SPIRV)
		{
			result.success = false;
			result.errorLog = "SPIRV source language requires using SPIRVTranslator directly";
			return result;
		}
		else
		{
			result.success = false;
			result.errorLog = "Unsupported source language";
			return result;
		}

		impl->lastSPIRV = spirv;

		// Step 2: Translate SPIR-V to target language
		if (targetLanguage == TargetLanguage::SPIRV)
		{
			result.success = true;
			result.source = ""; // SPIR-V bytecode stored in lastSPIRV
			return result;
		}

		result = impl->spirvTranslator.Translate(spirv, targetLanguage, platformVersion);
	}
	catch (const std::exception& e)
	{
		result.success = false;
		result.errorLog = std::string("Compilation failed: ") + e.what();
	}

	return result;
}

TranslationResult ShaderCompiler::CompileGLSL(
	const std::string& glslSource,
	ShaderStage stage,
	TargetLanguage targetLanguage,
	const PlatformVersion& platformVersion,
	const std::vector<std::string>& defines)
{
	return Compile(glslSource, SourceLanguage::GLSL, stage, targetLanguage, platformVersion, defines);
}

#ifdef SHADER_TRANSLATOR_HLSL_INPUT_ENABLED
TranslationResult ShaderCompiler::CompileHLSL(
	const std::string& hlslSource,
	ShaderStage stage,
	TargetLanguage targetLanguage,
	const PlatformVersion& platformVersion,
	const std::vector<std::string>& defines)
{
	return Compile(hlslSource, SourceLanguage::HLSL, stage, targetLanguage, platformVersion, defines);
}
#endif

std::vector<uint32_t> ShaderCompiler::GetLastSPIRV() const
{
	return impl->lastSPIRV;
}

} // namespace ShaderTranslator
