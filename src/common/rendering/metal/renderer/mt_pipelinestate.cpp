/*
**  Metal backend - Pipeline state management
**  Copyright (c) 2025 GZDoom Contributors
*/

#include <Metal/Metal.hpp>
#include "mt_pipelinestate.h"
#include "mt_renderdevice.h"

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

	// TODO: Create pipeline state
	return nullptr;
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
