/*
** spirv_translator.cpp
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
#ifdef SHADERTRANSLATOR_HLSL_ENABLED
#include <spirv_hlsl.hpp>
#endif
#include <spirv_glsl.hpp>
#include <stdexcept>

namespace ShaderTranslator
{

class SPIRVTranslator::Impl
{
public:
	TranslationResult TranslateToMSL(const std::vector<uint32_t>& spirv, int metalVersion)
	{
		TranslationResult result;

		try
		{
			// Create SPIRV-Cross MSL compiler
			spirv_cross::CompilerMSL msl(spirv);

			// Configure MSL options
			spirv_cross::CompilerMSL::Options options;
			options.platform = spirv_cross::CompilerMSL::Options::macOS;

			// Set Metal language version
			if (metalVersion >= 24)
				options.msl_version = spirv_cross::CompilerMSL::Options::make_msl_version(2, 4);
			else if (metalVersion >= 23)
				options.msl_version = spirv_cross::CompilerMSL::Options::make_msl_version(2, 3);
			else if (metalVersion >= 22)
				options.msl_version = spirv_cross::CompilerMSL::Options::make_msl_version(2, 2);
			else if (metalVersion >= 21)
				options.msl_version = spirv_cross::CompilerMSL::Options::make_msl_version(2, 1);
			else
				options.msl_version = spirv_cross::CompilerMSL::Options::make_msl_version(2, 0);

			options.enable_decoration_binding = true;
			msl.set_msl_options(options);

			// Map all bindings to their original binding points in MSL
			spirv_cross::ShaderResources resources = msl.get_shader_resources();
			
			for (auto &resource : resources.uniform_buffers) {
				uint32_t binding = msl.get_decoration(resource.id, spv::DecorationBinding);
				uint32_t set = msl.get_decoration(resource.id, spv::DecorationDescriptorSet);
				
				for (int stage = 0; stage < 3; stage++) { // Vertex, Fragment, Compute
					spirv_cross::MSLResourceBinding b;
					b.stage = (spv::ExecutionModel)stage;
					b.desc_set = set;
					b.binding = binding;
					b.msl_buffer = binding;
					msl.add_msl_resource_binding(b);
				}
			}

			for (auto &resource : resources.storage_buffers) {
				uint32_t binding = msl.get_decoration(resource.id, spv::DecorationBinding);
				uint32_t set = msl.get_decoration(resource.id, spv::DecorationDescriptorSet);
				
				for (int stage = 0; stage < 3; stage++) {
					spirv_cross::MSLResourceBinding b;
					b.stage = (spv::ExecutionModel)stage;
					b.desc_set = set;
					b.binding = binding;
					b.msl_buffer = binding;
					msl.add_msl_resource_binding(b);
				}
			}

			for (auto &resource : resources.sampled_images) {
				uint32_t binding = msl.get_decoration(resource.id, spv::DecorationBinding);
				uint32_t set = msl.get_decoration(resource.id, spv::DecorationDescriptorSet);
				
				for (int stage = 0; stage < 3; stage++) {
					spirv_cross::MSLResourceBinding b;
					b.stage = (spv::ExecutionModel)stage;
					b.desc_set = set;
					b.binding = binding;
					b.msl_texture = binding;
					b.msl_sampler = binding;
					msl.add_msl_resource_binding(b);
				}
			}

			// Compile SPIR-V to MSL
			result.source = msl.compile();

			// Extract resource bindings for reflection
			msl.get_shader_resources();

			// Extract texture bindings
			for (const auto& resource : resources.sampled_images)
			{
				TranslationResult::Resource r;
				r.name = resource.name;
				r.set = msl.get_decoration(resource.id, spv::DecorationDescriptorSet);
				r.binding = msl.get_decoration(resource.id, spv::DecorationBinding);
				r.mslBinding = msl.get_automatic_msl_resource_binding(resource.id);
				result.textures.push_back(r);
			}

			// Extract uniform buffer bindings
			for (const auto& resource : resources.uniform_buffers)
			{
				TranslationResult::Resource r;
				r.name = resource.name;
				r.set = msl.get_decoration(resource.id, spv::DecorationDescriptorSet);
				r.binding = msl.get_decoration(resource.id, spv::DecorationBinding);
				r.mslBinding = msl.get_automatic_msl_resource_binding(resource.id);
				result.buffers.push_back(r);
			}

			// Extract storage buffer bindings
			for (const auto& resource : resources.storage_buffers)
			{
				TranslationResult::Resource r;
				r.name = resource.name;
				r.set = msl.get_decoration(resource.id, spv::DecorationDescriptorSet);
				r.binding = msl.get_decoration(resource.id, spv::DecorationBinding);
				r.mslBinding = msl.get_automatic_msl_resource_binding(resource.id);
				result.buffers.push_back(r);
			}

			result.success = true;
		}
		catch (const std::exception& e)
		{
			result.success = false;
			result.errorLog = std::string("MSL translation failed: ") + e.what();
		}

		return result;
	}

#ifdef SHADERTRANSLATOR_HLSL_ENABLED
	TranslationResult TranslateToHLSL(const std::vector<uint32_t>& spirv, int shaderModel)
	{
		TranslationResult result;

		try
		{
			// Create SPIRV-Cross HLSL compiler
			spirv_cross::CompilerHLSL hlsl(spirv);

			// Configure HLSL options
			spirv_cross::CompilerHLSL::Options options;
			options.shader_model = shaderModel;
			hlsl.set_hlsl_options(options);

			// Compile SPIR-V to HLSL
			result.source = hlsl.compile();

			// Extract resource bindings
			spirv_cross::ShaderResources resources = hlsl.get_shader_resources();

			// Extract texture bindings
			for (const auto& resource : resources.sampled_images)
			{
				TranslationResult::Resource r;
				r.name = resource.name;
				r.set = hlsl.get_decoration(resource.id, spv::DecorationDescriptorSet);
				r.binding = hlsl.get_decoration(resource.id, spv::DecorationBinding);
				// HLSL uses register() syntax - would need to parse output
				result.textures.push_back(r);
			}

			result.success = true;
		}
		catch (const std::exception& e)
		{
			result.success = false;
			result.errorLog = std::string("HLSL translation failed: ") + e.what();
		}

		return result;
	}
#endif
};

// SPIRVTranslator implementation
SPIRVTranslator::SPIRVTranslator()
	: impl(new Impl())
{
}

SPIRVTranslator::~SPIRVTranslator()
{
	delete impl;
}

TranslationResult SPIRVTranslator::Translate(
	const std::vector<uint32_t>& spirv,
	TargetLanguage targetLanguage,
	const PlatformVersion& platformVersion)
{
	switch (targetLanguage)
	{
	case TargetLanguage::MSL:
		return impl->TranslateToMSL(spirv, platformVersion.metalVersion);

#ifdef SHADERTRANSLATOR_HLSL_ENABLED
	case TargetLanguage::HLSL:
		return impl->TranslateToHLSL(spirv, platformVersion.hlslShaderModel);
#endif

	case TargetLanguage::GLSL_Vulkan:
		// Future: could use spirv_cross::CompilerGLSL for reference output
		break;

	case TargetLanguage::SPIRV:
		// Passthrough: caller already has SPIRV, no translation needed
		break;

	default:
		break;
	}

	TranslationResult result;
	result.success = false;
	result.errorLog = "Unsupported target language";
	return result;
}

TranslationResult SPIRVTranslator::TranslateToMSL(
	const std::vector<uint32_t>& spirv,
	int metalVersion)
{
	return impl->TranslateToMSL(spirv, metalVersion);
}

#ifdef SHADERTRANSLATOR_HLSL_ENABLED
TranslationResult SPIRVTranslator::TranslateToHLSL(
	const std::vector<uint32_t>& spirv,
	int shaderModel)
{
	return impl->TranslateToHLSL(spirv, shaderModel);
}
#endif

} // namespace ShaderTranslator
