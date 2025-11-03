/*
**  Metal backend - Shader management
**  Copyright (c) 2025 GZDoom Contributors
*/

#include <Metal/Metal.hpp>
#include "mt_shader.h"
#include "mt_renderdevice.h"

MtShaderManager::MtShaderManager(MetalRenderDevice* fb) : fb(fb) {}
MtShaderManager::~MtShaderManager() { ClearCache(); }

std::shared_ptr<MtShaderModule> MtShaderManager::CompileShader(
	const std::string& name,
	const std::string& vertexSource,
	const std::string& fragmentSource,
	const std::vector<std::string>& defines)
{
	// TODO: Implement GLSL → SPIR-V → MSL pipeline using shader-translator
	// For now, return nullptr
	return nullptr;
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
			if (module->function) module->function->release();
			if (module->library) module->library->release();
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
	// TODO: Reuse Vulkan's GLSL → SPIR-V compilation
	return std::vector<uint32_t>();
}

std::string MtShaderManager::TranslateSPIRVToMSL(const std::vector<uint32_t>& spirv, bool isVertex)
{
	// TODO: Use shader-translator library
	return "";
}

MTL::Library* MtShaderManager::CompileMSLToLibrary(const std::string& msl, const std::string& name)
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
