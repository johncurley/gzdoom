/*
** shader_translator.h
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
** Cross-platform shader translation library for GZDoom
** Translates SPIR-V bytecode to Metal Shading Language (MSL) and HLSL
**
*/

#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace ShaderTranslator
{

// Target shader languages
enum class TargetLanguage
{
	MSL,            // Metal Shading Language (macOS/iOS)
	HLSL,           // High-Level Shading Language (D3D12)
	GLSL_Vulkan     // GLSL with Vulkan semantics (for reference)
};

// Metal/HLSL platform versions
struct PlatformVersion
{
	int metalVersion = 20;      // Metal 2.0 (macOS 10.13+)
	int hlslShaderModel = 60;   // Shader Model 6.0 (D3D12)
};

// Compilation result
struct TranslationResult
{
	bool success = false;
	std::string errorLog;
	std::string source;         // Translated shader source code (MSL/HLSL)

	// Reflection data for resource binding
	struct Resource
	{
		std::string name;
		uint32_t set;           // Vulkan descriptor set
		uint32_t binding;       // Vulkan binding point
		uint32_t mslBinding;    // Translated Metal binding
		uint32_t hlslRegister;  // Translated HLSL register
	};

	std::vector<Resource> textures;
	std::vector<Resource> buffers;
	std::vector<Resource> samplers;
};

// SPIR-V to MSL/HLSL translator
class SPIRVTranslator
{
public:
	SPIRVTranslator();
	~SPIRVTranslator();

	// Translate SPIR-V bytecode to target language
	TranslationResult Translate(
		const std::vector<uint32_t>& spirv,
		TargetLanguage targetLanguage,
		const PlatformVersion& platformVersion = PlatformVersion()
	);

	// Translate SPIR-V bytecode to MSL (convenience method)
	TranslationResult TranslateToMSL(
		const std::vector<uint32_t>& spirv,
		int metalVersion = 20
	);

	// Translate SPIR-V bytecode to HLSL (convenience method)
	TranslationResult TranslateToHLSL(
		const std::vector<uint32_t>& spirv,
		int shaderModel = 60
	);

private:
	class Impl;
	Impl* impl;
};

} // namespace ShaderTranslator
