/*
**  Vulkan backend
**  Copyright (c) 2016-2020 Magnus Norddahl
**
**  This software is provided 'as-is', without any express or implied
**  warranty.  In no event will the authors be held liable for any damages
**  arising from the use of this software.
**
**  Permission is granted to anyone to use this software for any purpose,
**  including commercial applications, and to alter it and redistribute it
**  freely, subject to the following restrictions:
**
**  1. The origin of this software must not be misrepresented; you must not
**     claim that you wrote the original software. If you use this software
**     in a product, an acknowledgment in the product documentation would be
**     appreciated but is not required.
**  2. Altered source versions must be plainly marked as such, and must not be
**     misrepresented as being the original software.
**  3. This notice may not be removed or altered from any source distribution.
**
*/

#include "vk_postprocess.h"
#include "vulkan/shaders/vk_shader.h"
#include <zvulkan/vulkanswapchain.h>
#include <zvulkan/vulkanbuilders.h>
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/system/vk_hwbuffer.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "vulkan/renderer/vk_renderstate.h"
#include "vulkan/renderer/vk_pprenderstate.h"
#include "vulkan/renderer/vk_descriptorset.h"
#include "vulkan/shaders/vk_ppshader.h"
#include "vulkan/textures/vk_pptexture.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/textures/vk_samplers.h"
#include "vulkan/textures/vk_imagetransition.h"
#include "vulkan/textures/vk_texture.h"
#include "vulkan/textures/vk_framebuffer.h"
#include "filesystem.h"
#include "hw_cvars.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "hwrenderer/postprocessing/hw_postprocess_cvars.h"
#include "hw_vrmodes.h"
#include "flatvertices.h"
#include "r_videoscale.h"
#include "cmdlib.h"

EXTERN_CVAR(Int, gl_dither_bpc)

CVAR(Bool, vk_compute_ssao, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

VkPostprocess::VkPostprocess(VulkanRenderDevice* fb) : fb(fb)
{
}

VkPostprocess::~VkPostprocess()
{
}

void VkPostprocess::SetActiveRenderTarget()
{
	auto buffers = fb->GetBuffers();

	VkImageTransition()
		.AddImage(&buffers->PipelineImage[mCurrentPipelineImage], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false)
		.AddImage(&buffers->PipelineDepthStencil, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, false)
		.Execute(fb->GetCommands()->GetDrawCommands());

	fb->GetRenderState()->SetRenderTarget(&buffers->PipelineImage[mCurrentPipelineImage], buffers->PipelineDepthStencil.View.get(), buffers->GetWidth(), buffers->GetHeight(), VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT);
}

void VkPostprocess::PostProcessScene(int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D)
{
	int sceneWidth = fb->GetBuffers()->GetSceneWidth();
	int sceneHeight = fb->GetBuffers()->GetSceneHeight();

	VkPPRenderState renderstate(fb);

	hw_postprocess.Pass1(&renderstate, fixedcm, sceneWidth, sceneHeight);
	SetActiveRenderTarget();
	afterBloomDrawEndScene2D();
	hw_postprocess.Pass2(&renderstate, fixedcm, flash, sceneWidth, sceneHeight);
}

void VkPostprocess::BlitSceneToPostprocess()
{
	fb->GetRenderState()->EndRenderPass();

	auto buffers = fb->GetBuffers();
	auto cmdbuffer = fb->GetCommands()->GetDrawCommands();

	mCurrentPipelineImage = 0;

	PassDesc desc;
	desc.name = "scene.resolve";
	desc.owner = "VkPostprocess";
	desc.reads = { "SceneColor" };
	desc.writes = { "PipelineImage[0]" };
	desc.uses.Push({ "SceneColor", FrameGraphAccess::Read, FrameGraphUsage::TransferSource });
	desc.uses.Push({ "PipelineImage[0]", FrameGraphAccess::Write, FrameGraphUsage::TransferDestination });
	int graphPass = fb->Graph().AddPass(desc);
	fb->Graph().BeginBackendPass(graphPass);
	fb->Resources().Touch("SceneColor", false);
	fb->Graph().ObserveBackendUse("SceneColor", FrameGraphAccess::Read, FrameGraphUsage::TransferSource);
	fb->Resources().Touch("PipelineImage[0]", true);
	fb->Graph().ObserveBackendUse("PipelineImage[0]", FrameGraphAccess::Write, FrameGraphUsage::TransferDestination);

	VkImageTransition()
		.AddImage(&buffers->SceneColor, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
		.AddImage(&buffers->PipelineImage[mCurrentPipelineImage], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true)
		.Execute(fb->GetCommands()->GetDrawCommands());

	if (buffers->GetSceneSamples() != VK_SAMPLE_COUNT_1_BIT)
	{
		auto sceneColor = buffers->SceneColor.Image.get();
		VkImageResolve resolve = {};
		resolve.srcOffset = { 0, 0, 0 };
		resolve.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		resolve.srcSubresource.mipLevel = 0;
		resolve.srcSubresource.baseArrayLayer = 0;
		resolve.srcSubresource.layerCount = 1;
		resolve.dstOffset = { 0, 0, 0 };
		resolve.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		resolve.dstSubresource.mipLevel = 0;
		resolve.dstSubresource.baseArrayLayer = 0;
		resolve.dstSubresource.layerCount = 1;
		resolve.extent = { (uint32_t)sceneColor->width, (uint32_t)sceneColor->height, 1 };
		cmdbuffer->resolveImage(
			sceneColor->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			buffers->PipelineImage[mCurrentPipelineImage].Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &resolve);
	}
	else
	{
		auto sceneColor = buffers->SceneColor.Image.get();
		VkImageBlit blit = {};
		blit.srcOffsets[0] = { 0, 0, 0 };
		blit.srcOffsets[1] = { sceneColor->width, sceneColor->height, 1 };
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.mipLevel = 0;
		blit.srcSubresource.baseArrayLayer = 0;
		blit.srcSubresource.layerCount = 1;
		blit.dstOffsets[0] = { 0, 0, 0 };
		blit.dstOffsets[1] = { sceneColor->width, sceneColor->height, 1 };
		blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.dstSubresource.mipLevel = 0;
		blit.dstSubresource.baseArrayLayer = 0;
		blit.dstSubresource.layerCount = 1;
		cmdbuffer->blitImage(
			sceneColor->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			buffers->PipelineImage[mCurrentPipelineImage].Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &blit, VK_FILTER_NEAREST);
	}
	fb->Graph().EndBackendPass();
}

void VkPostprocess::ImageTransitionScene(bool undefinedSrcLayout)
{
	auto buffers = fb->GetBuffers();

	VkImageTransition()
		.AddImage(&buffers->SceneColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, undefinedSrcLayout)
		.AddImage(&buffers->SceneFog, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, undefinedSrcLayout)
		.AddImage(&buffers->SceneNormal, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, undefinedSrcLayout)
		.AddImage(&buffers->SceneDepthStencil, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, undefinedSrcLayout)
		.Execute(fb->GetCommands()->GetDrawCommands());
}

void VkPostprocess::BlitCurrentToImage(VkTextureImage *dstimage, VkImageLayout finallayout, const char *destinationName)
{
	fb->GetRenderState()->EndRenderPass();

	auto srcimage = &fb->GetBuffers()->PipelineImage[mCurrentPipelineImage];
	auto cmdbuffer = fb->GetCommands()->GetDrawCommands();
	const char *sourceName = fb->GetTextureManager()->GetTextureResourceName(PPTextureType::CurrentPipelineTexture);
	int graphPass = -1;
	if (destinationName && sourceName)
	{
		PassDesc desc;
		desc.name = "wipe.copy";
		desc.owner = "VkPostprocess";
		desc.reads = { sourceName };
		desc.writes = { destinationName };
		desc.uses.Push({ sourceName, FrameGraphAccess::Read, FrameGraphUsage::TransferSource });
		desc.uses.Push({ destinationName, FrameGraphAccess::Write, FrameGraphUsage::TransferDestination });
		graphPass = fb->Graph().AddPass(desc);
		fb->Graph().BeginBackendPass(graphPass);
		fb->Resources().Touch(sourceName, false);
		fb->Graph().ObserveBackendUse(sourceName, FrameGraphAccess::Read, FrameGraphUsage::TransferSource);
		fb->Graph().ObserveBackendUse(destinationName, FrameGraphAccess::Write, FrameGraphUsage::TransferDestination);
	}

	VkImageTransition()
		.AddImage(srcimage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
		.AddImage(dstimage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true)
		.Execute(cmdbuffer);

	VkImageBlit blit = {};
	blit.srcOffsets[0] = { 0, 0, 0 };
	blit.srcOffsets[1] = { srcimage->Image->width, srcimage->Image->height, 1 };
	blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.srcSubresource.mipLevel = 0;
	blit.srcSubresource.baseArrayLayer = 0;
	blit.srcSubresource.layerCount = 1;
	blit.dstOffsets[0] = { 0, 0, 0 };
	blit.dstOffsets[1] = { dstimage->Image->width, dstimage->Image->height, 1 };
	blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.dstSubresource.mipLevel = 0;
	blit.dstSubresource.baseArrayLayer = 0;
	blit.dstSubresource.layerCount = 1;

	cmdbuffer->blitImage(
		srcimage->Image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		dstimage->Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &blit, VK_FILTER_NEAREST);

	VkImageTransition()
		.AddImage(dstimage, finallayout, false)
		.Execute(cmdbuffer);

	if (graphPass >= 0)
		fb->Graph().EndBackendPass();
}

void VkPostprocess::DrawPresentTexture(const IntRect &box, bool applyGamma, bool screenshot)
{
	VkPPRenderState renderstate(fb);

	if (!screenshot) // Already applied as we are actually copying the last frame here (GetScreenshotBuffer is called after swap)
		hw_postprocess.customShaders.Run(&renderstate, "screen");

	PresentUniforms uniforms;
	if (!applyGamma)
	{
		uniforms.InvGamma = 1.0f;
		uniforms.Contrast = 1.0f;
		uniforms.Brightness = 0.0f;
		uniforms.Saturation = 1.0f;
	}
	else
	{
		uniforms.InvGamma = 1.0f / clamp<float>(vid_gamma, 0.1f, 4.f);
		uniforms.Contrast = clamp<float>(vid_contrast, 0.1f, 3.f);
		uniforms.Brightness = clamp<float>(vid_brightness, -0.8f, 0.8f);
		uniforms.Saturation = clamp<float>(vid_saturation, -15.0f, 15.f);
		uniforms.GrayFormula = static_cast<int>(gl_satformula);
	}
	uniforms.ColorScale = (gl_dither_bpc == -1) ? 255.0f : (float)((1 << gl_dither_bpc) - 1);

	if (screenshot)
	{
		uniforms.Scale = { screen->mScreenViewport.width / (float)fb->GetBuffers()->GetWidth(), screen->mScreenViewport.height / (float)fb->GetBuffers()->GetHeight() };
		uniforms.Offset = { 0.0f, 0.0f };
	}
	else
	{
		uniforms.Scale = { screen->mScreenViewport.width / (float)fb->GetBuffers()->GetWidth(), -screen->mScreenViewport.height / (float)fb->GetBuffers()->GetHeight() };
		uniforms.Offset = { 0.0f, 1.0f };
	}

	if (applyGamma && fb->GetFramebufferManager()->SwapChain->Format().colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT && !screenshot)
	{
		uniforms.HdrMode = 1;
	}
	else
	{
		uniforms.HdrMode = 0;
	}

	renderstate.Clear();
	if (screenshot)
		renderstate.SetPassName("screenshot.present");
	renderstate.Shader = &hw_postprocess.present.Present;
	renderstate.Uniforms.Set(uniforms);
	renderstate.Viewport = box;
	renderstate.SetInputCurrent(0, ViewportLinearScale() ? PPFilterMode::Linear : PPFilterMode::Nearest);
	renderstate.SetInputTexture(1, &hw_postprocess.present.Dither, PPFilterMode::Nearest, PPWrapMode::Repeat);
	if (screenshot)
		renderstate.SetOutputNext();
	else
		renderstate.SetOutputSwapChain();
	renderstate.SetNoBlend();
	renderstate.Draw();
}

void VkPostprocess::AmbientOccludeScene(float m5)
{
	int sceneWidth = fb->GetBuffers()->GetSceneWidth();
	int sceneHeight = fb->GetBuffers()->GetSceneHeight();

	VkPPRenderState renderstate(fb);
	bool computeLinearDepth = false;
	if (vk_compute_ssao && fb->GetBuffers()->GetSceneSamples() == VK_SAMPLE_COUNT_1_BIT)
		computeLinearDepth = ComputeLinearDepth(sceneWidth, sceneHeight);
	hw_postprocess.ssao.Render(&renderstate, m5, sceneWidth, sceneHeight, computeLinearDepth);

	ImageTransitionScene(false);
}

void VkPostprocess::CreateComputeLinearDepthPipeline()
{
	if (ComputeLinearDepthPipeline)
		return;

	int lump = fileSystem.CheckNumForFullName("shaders/pp/lineardepth.comp");
	if (lump == -1)
		I_FatalError("Unable to load 'shaders/pp/lineardepth.comp'");

	FString code;
	code.AppendFormat("#version 450\n#line 1\n%s", GetStringFromLump(lump).GetChars());

	ComputeLinearDepthShader = ShaderBuilder()
		.Type(ShaderType::Compute)
		.AddSource("shaders/pp/lineardepth.comp", code.GetChars())
		.DebugName("VkPostprocess.LinearDepthComputeShader")
		.Create("shaders/pp/lineardepth.comp", fb->device.get());

	ComputeLinearDepthDescriptorLayout = DescriptorSetLayoutBuilder()
		.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.DebugName("VkPostprocess.LinearDepthComputeDescriptorLayout")
		.Create(fb->device.get());

	ComputeLinearDepthPipelineLayout = PipelineLayoutBuilder()
		.AddSetLayout(ComputeLinearDepthDescriptorLayout.get())
		.AddPushConstantRange(VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(LinearDepthUniforms))
		.DebugName("VkPostprocess.LinearDepthComputePipelineLayout")
		.Create(fb->device.get());

	ComputeLinearDepthPipeline = ComputePipelineBuilder()
		.Layout(ComputeLinearDepthPipelineLayout.get())
		.ComputeShader(ComputeLinearDepthShader.get())
		.DebugName("VkPostprocess.LinearDepthComputePipeline")
		.Create(fb->device.get());
}

bool VkPostprocess::ComputeLinearDepth(int sceneWidth, int sceneHeight)
{
	if (!hw_postprocess.ssao.PrepareLinearDepth(sceneWidth, sceneHeight))
		return false;

	// AmbientOccludeScene is called from the middle of scene rendering. The
	// scene render pass is still open there, but compute dispatch and the
	// image barriers below must be recorded outside any render-pass instance.
	fb->GetRenderState()->EndRenderPass();

	CreateComputeLinearDepthPipeline();

	auto buffers = fb->GetBuffers();
	auto linearDepth = fb->GetTextureManager()->GetTexture(PPTextureType::PPTexture,
		hw_postprocess.ssao.GetLinearDepthTexture());
	if (!linearDepth || !linearDepth->View || !buffers->SceneColor.View || !buffers->SceneDepthStencil.DepthOnlyView)
		return false;

	PassDesc desc;
	desc.name = "ssao.lineardepth.compute";
	desc.owner = "VkPostprocess";
	desc.reads = { "SceneDepthStencil", "SceneColor" };
	desc.writes = { "AO.LinearDepth" };
	desc.uses.Push({ "SceneDepthStencil", FrameGraphAccess::Read, FrameGraphUsage::Sampled });
	desc.uses.Push({ "SceneColor", FrameGraphAccess::Read, FrameGraphUsage::Sampled });
	desc.uses.Push({ "AO.LinearDepth", FrameGraphAccess::Write, FrameGraphUsage::Storage });
	int graphPass = fb->Graph().AddPass(desc);
	fb->Graph().BeginBackendPass(graphPass);
	fb->Resources().Touch("SceneDepthStencil", false);
	fb->Graph().ObserveBackendUse("SceneDepthStencil", FrameGraphAccess::Read, FrameGraphUsage::Sampled);
	fb->Resources().Touch("SceneColor", false);
	fb->Graph().ObserveBackendUse("SceneColor", FrameGraphAccess::Read, FrameGraphUsage::Sampled);
	fb->Resources().Touch("AO.LinearDepth", true);
	fb->Graph().ObserveBackendUse("AO.LinearDepth", FrameGraphAccess::Write, FrameGraphUsage::Storage);

	VkImageTransition()
		.AddComputeSampledImage(&buffers->SceneDepthStencil)
		.AddComputeSampledImage(&buffers->SceneColor)
		.AddComputeStorageImage(linearDepth)
		.Execute(fb->GetCommands()->GetDrawCommands());

	auto descriptors = fb->GetDescriptorSetManager()->AllocateComputeDescriptorSet(ComputeLinearDepthDescriptorLayout.get());
	VulkanSampler *sampler = fb->GetSamplerManager()->Get(PPFilterMode::Nearest, PPWrapMode::Clamp);
	WriteDescriptors write;
	write.AddCombinedImageSampler(descriptors.get(), 0, buffers->SceneDepthStencil.DepthOnlyView.get(), sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	write.AddCombinedImageSampler(descriptors.get(), 1, buffers->SceneColor.View.get(), sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	write.AddStorageImage(descriptors.get(), 2, linearDepth->View.get(), VK_IMAGE_LAYOUT_GENERAL);
	write.Execute(fb->device.get());

	LinearDepthUniforms uniforms;
	hw_postprocess.ssao.GetLinearDepthUniforms(uniforms);

	fb->GetCommands()->PushGroup("ssao.lineardepth.compute");
	auto cmdbuffer = fb->GetCommands()->GetDrawCommands();
	cmdbuffer->bindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, ComputeLinearDepthPipeline.get());
	cmdbuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, ComputeLinearDepthPipelineLayout.get(), 0, descriptors.get());
	cmdbuffer->pushConstants(ComputeLinearDepthPipelineLayout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uniforms), &uniforms);
	cmdbuffer->dispatch((linearDepth->Image->width + 7) / 8, (linearDepth->Image->height + 7) / 8, 1);
	fb->GetCommands()->PopGroup();

	// Make the compute write visible to the existing fragment AO pass. The
	// layout transition also changes the tracked image state used by the
	// ordinary postprocess descriptor path.
	VkImageTransition()
		.AddImage(linearDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
		.Execute(cmdbuffer);

	fb->GetCommands()->DrawDeleteList->Add(std::move(descriptors));
	fb->Graph().EndBackendPass();
	return true;
}

void VkPostprocess::BlurScene(float gameinfobluramount)
{
	int sceneWidth = fb->GetBuffers()->GetSceneWidth();
	int sceneHeight = fb->GetBuffers()->GetSceneHeight();

	VkPPRenderState renderstate(fb);

	auto vrmode = VRMode::GetVRMode(true);
	int eyeCount = vrmode->mEyeCount;
	for (int i = 0; i < eyeCount; ++i)
	{
		hw_postprocess.bloom.RenderBlur(&renderstate, sceneWidth, sceneHeight, gameinfobluramount);
		if (eyeCount - i > 1) NextEye(eyeCount);
	}
}

void VkPostprocess::ClearTonemapPalette()
{
	hw_postprocess.tonemap.ClearTonemapPalette();
}

void VkPostprocess::UpdateShadowMap()
{
	if (screen->mShadowMap.PerformUpdate())
	{
		VkPPRenderState renderstate(fb);
		hw_postprocess.shadowmap.Update(&renderstate);

		VkImageTransition()
			.AddImage(&fb->GetTextureManager()->Shadowmap, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
			.Execute(fb->GetCommands()->GetDrawCommands());

		screen->mShadowMap.FinishUpdate();
	}
}

void VkPostprocess::NextEye(int eyeCount)
{
}
