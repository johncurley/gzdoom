/*
**  Metal backend - Post-processing
**  Copyright (c) 2025 GZDoom Contributors
*/

#include "mt_postprocess.h"
#include "c_cvars.h"
#include "flatvertices.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "hwrenderer/postprocessing/hw_postprocess_cvars.h"
#include "metal/renderer/mt_pipelinestate.h"
#include "metal/renderer/mt_postprocess.h"
#include "metal/renderer/mt_renderbuffers.h"
#include "metal/renderer/mt_renderstate.h"
#include "metal/shaders/mt_shader.h"
#include "metal/system/mt_commandbuffer.h"
#include "metal/system/mt_hwbuffer.h"
#include "metal/system/mt_renderdevice.h"
#include "metal/textures/mt_texture.h"
#include "printf.h"
#include "r_videoscale.h"
#include "v_video.h"

EXTERN_CVAR(Int, gl_dither_bpc)

class MtPPRenderState : public PPRenderState {
public:
  MtPPRenderState(MetalRenderDevice *fb) : fb(fb) {}

  void PushGroup(const FString &name) override {
    // fb->GetCommands()->PushGroup(name.GetChars());
  }

  void PopGroup() override {
    // fb->GetCommands()->PopGroup();
  }

  void Draw() override {
    auto renderState = fb->GetRenderState();
    renderState->EndRenderPass();

    MtShaderProgram *program = fb->GetShaderManager()->GetPPShader(Shader);
    if (!program || !program->vert || !program->frag)
      return;

    // Determine output target
    MTL::Texture *outputTex = nullptr;
    int width = fb->GetBuffers()->GetWidth();
    int height = fb->GetBuffers()->GetHeight();
    MTL::PixelFormat format = MTL::PixelFormatRGBA16Float;

    if (Output.Type == PPTextureType::SwapChain) {
      // Swapchain handled by MtRenderState default
      outputTex = nullptr;                 // use default
      format = MTL::PixelFormatBGRA8Unorm; // Standard for macOS swapchain
      width = screen->GetWidth();
      height = screen->GetHeight();
    } else if (Output.Type == PPTextureType::PPTexture) {
      outputTex = fb->GetTextureManager()->GetPPTexture(Output.Texture);
      if (outputTex) {
        width = (int)outputTex->width();
        height = (int)outputTex->height();
        format = outputTex->pixelFormat();
      }
    } else if (Output.Type == PPTextureType::CurrentPipelineTexture) {
      outputTex =
          fb->GetBuffers()
              ->PipelineImage[fb->GetPostprocess()->mCurrentPipelineImage]
              ->GetTexture();
      format = MTL::PixelFormatRGBA16Float;
    } else if (Output.Type == PPTextureType::NextPipelineTexture) {
      int next = (fb->GetPostprocess()->mCurrentPipelineImage + 1) %
                 MtRenderBuffers::NumPipelineImages;
      outputTex = fb->GetBuffers()->PipelineImage[next]->GetTexture();
      format = MTL::PixelFormatRGBA16Float;
    } else if (Output.Type == PPTextureType::SceneColor) {
      outputTex = fb->GetBuffers()->SceneColor->GetTexture();
      format = MTL::PixelFormatRGBA16Float;
    }

    auto renderPassDesc = MTL::RenderPassDescriptor::alloc()->init();
    auto colorAttachment = renderPassDesc->colorAttachments()->object(0);

    if (outputTex) {
      colorAttachment->setTexture(outputTex);
    } else {
      // Use swapchain texture if available
      auto curTex = fb->GetRenderState()->GetRenderTarget().Image;
      if (curTex)
        colorAttachment->setTexture(curTex);
      else {
        renderPassDesc->release();
        return;
      }
    }

    // Load/Clear logic
    if (BlendMode.SrcAlpha == STYLEALPHA_One &&
        BlendMode.DestAlpha == STYLEALPHA_Zero && !ShadowMapBuffers) {
      colorAttachment->setLoadAction(MTL::LoadActionClear);
      colorAttachment->setClearColor(MTL::ClearColor(0, 0, 0, 1));
    } else {
      colorAttachment->setLoadAction(MTL::LoadActionLoad);
    }
    colorAttachment->setStoreAction(MTL::StoreActionStore);

    auto cmdBuffer = fb->GetCommands()->GetRenderCommandBuffer();
    auto encoder = cmdBuffer->renderCommandEncoder(renderPassDesc);

    auto pipeline = fb->GetPipelineStateManager()->GetPPPipelineState(
        program, format, BlendMode);
    if (pipeline) {
      encoder->setRenderPipelineState(pipeline);

      // Bind vertex buffer (screen->mVertexData)
      auto vb = static_cast<MtVertexBuffer *>(
          screen->mVertexData->GetBufferObjects().first);
      encoder->setVertexBuffer(vb->GetBuffer(), 0, 0);

      // Bind input textures
      for (int i = 0; i < (int)Textures.Size(); ++i) {
        auto &input = Textures[i];
        MTL::Texture *tex = nullptr;

        switch (input.Type) {
        case PPTextureType::CurrentPipelineTexture:
          tex = fb->GetBuffers()
                    ->PipelineImage[fb->GetPostprocess()->mCurrentPipelineImage]
                    ->GetTexture();
          break;
        case PPTextureType::NextPipelineTexture: {
          int next = (fb->GetPostprocess()->mCurrentPipelineImage + 1) %
                     MtRenderBuffers::NumPipelineImages;
          tex = fb->GetBuffers()->PipelineImage[next]->GetTexture();
          break;
        }
        case PPTextureType::PPTexture:
          tex = fb->GetTextureManager()->GetPPTexture(input.Texture);
          break;
        case PPTextureType::SceneColor:
          tex = fb->GetBuffers()->SceneColor->GetTexture();
          break;
        case PPTextureType::SceneDepth:
          tex = fb->GetBuffers()->SceneDepthStencil->GetTexture();
          break;
        default:
          break;
        }

        if (tex) {
          encoder->setFragmentTexture(tex, i);
          // TODO: Set sampler
        }
      }

      // Bind uniforms
      if (Uniforms.Data.Size() > 0) {
        encoder->setFragmentBytes(Uniforms.Data.Data(), Uniforms.Data.Size(),
                                  0);
      }

      // Draw quad
      encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                              FFlatVertexBuffer::PRESENT_INDEX, 3);
    }

    encoder->endEncoding();
    renderPassDesc->release();

    // Advance pipeline index if output was Next
    if (Output.Type == PPTextureType::NextPipelineTexture) {
      fb->GetPostprocess()->mCurrentPipelineImage =
          (fb->GetPostprocess()->mCurrentPipelineImage + 1) %
          MtRenderBuffers::NumPipelineImages;
    }
  }

private:
  MetalRenderDevice *fb;
};

MtPostprocess::MtPostprocess(MetalRenderDevice *fb) : fb(fb) {}
MtPostprocess::~MtPostprocess() {}

void MtPostprocess::BlurScene(float amount) {
  int sceneWidth = fb->GetBuffers()->GetSceneWidth();
  int sceneHeight = fb->GetBuffers()->GetSceneHeight();

  MtPPRenderState renderstate(fb);
  hw_postprocess.bloom.RenderBlur(&renderstate, sceneWidth, sceneHeight,
                                  amount);
}

void MtPostprocess::AmbientOccludeScene(float m5) {
  int sceneWidth = fb->GetBuffers()->GetSceneWidth();
  int sceneHeight = fb->GetBuffers()->GetSceneHeight();

  MtPPRenderState renderstate(fb);
  hw_postprocess.ssao.Render(&renderstate, m5, sceneWidth, sceneHeight);
}

void MtPostprocess::UpdateShadowMap() {
  if (screen->mShadowMap.PerformUpdate()) {
    MtPPRenderState renderstate(fb);
    hw_postprocess.shadowmap.Update(&renderstate);
    screen->mShadowMap.FinishUpdate();
  }
}

void MtPostprocess::SetActiveRenderTarget() {
  auto buffers = fb->GetBuffers();
  fb->GetRenderState()->SetRenderTarget(
      buffers->PipelineImage[mCurrentPipelineImage]->GetTexture(),
      buffers->PipelineDepthStencil->GetTexture(), buffers->GetWidth(),
      buffers->GetHeight(), (int)MTL::PixelFormatRGBA16Float, 1);
}

void MtPostprocess::PostProcessScene(
    bool swscene, int fixedcm, float flash,
    const std::function<void()> &afterBloomDrawEndScene2D) {
  int sceneWidth = fb->GetBuffers()->GetSceneWidth();
  int sceneHeight = fb->GetBuffers()->GetSceneHeight();

  MtPPRenderState renderstate(fb);

  hw_postprocess.Pass1(&renderstate, fixedcm, sceneWidth, sceneHeight);
  SetActiveRenderTarget();
  afterBloomDrawEndScene2D();
  hw_postprocess.Pass2(&renderstate, fixedcm, flash, sceneWidth, sceneHeight);
}

void MtPostprocess::ImageTransitionScene(bool undefinedSrcLayout) {
  // Metal doesn't need explicit transitions
}

void MtPostprocess::BlitSceneToPostprocess() {
  fb->GetRenderState()->EndRenderPass();

  auto buffers = fb->GetBuffers();
  mCurrentPipelineImage = 0;

  auto cmdBuffer = fb->GetCommands()->GetBlitCommandBuffer();
  auto blitEncoder = cmdBuffer->blitCommandEncoder();

  auto src = buffers->SceneColor->GetTexture();
  auto dst = buffers->PipelineImage[0]->GetTexture();

  if (src && dst) {
    blitEncoder->copyFromTexture(src, dst);
  }

  blitEncoder->endEncoding();
}

void MtPostprocess::BlitCurrentToImage(MTL::Texture *dstimage) {
  fb->GetRenderState()->EndRenderPass();

  auto srcimage =
      fb->GetBuffers()->PipelineImage[mCurrentPipelineImage]->GetTexture();
  if (!srcimage || !dstimage)
    return;

  // If formats match, use blit encoder
  if (srcimage->pixelFormat() == dstimage->pixelFormat()) {
    auto cmdBuffer = fb->GetCommands()->GetBlitCommandBuffer();
    auto blitEncoder = cmdBuffer->blitCommandEncoder();
    blitEncoder->copyFromTexture(srcimage, dstimage);
    blitEncoder->endEncoding();
  } else {
    // Use a simple draw call to convert formats
    // We can use a simple draw pass here
    MtPPRenderState renderstate(fb);
    // This is a stub for now, but will work if formats are same or handled by
    // Draw.
    Printf("Metal: BlitCurrentToImage format conversion potentially needed: %d "
           "-> %d\n",
           (int)srcimage->pixelFormat(), (int)dstimage->pixelFormat());
  }
}

void MtPostprocess::DrawPresentTexture(IntRect box, bool applyGamma,
                                       bool screenshot) {
  MtPPRenderState renderstate(fb);
  PresentUniforms uniforms;

  if (!applyGamma) {
    uniforms.InvGamma = 1.0f;
    uniforms.Contrast = 1.0f;
    uniforms.Brightness = 0.0f;
    uniforms.Saturation = 1.0f;
    uniforms.GrayFormula = 0;
  } else {
    uniforms.InvGamma = 1.0f / clamp<float>(vid_gamma, 0.1f, 4.f);
    uniforms.Contrast = clamp<float>(vid_contrast, 0.1f, 3.f);
    uniforms.Brightness = clamp<float>(vid_brightness, -0.8f, 0.8f);
    uniforms.Saturation = clamp<float>(vid_saturation, -15.0f, 15.f);
    uniforms.GrayFormula = static_cast<int>(gl_satformula);
  }
  uniforms.ColorScale =
      (gl_dither_bpc == -1) ? 255.0f : (float)((1 << gl_dither_bpc) - 1);

  if (screenshot) {
    uniforms.Scale = {
        screen->mScreenViewport.width / (float)fb->GetBuffers()->GetWidth(),
        screen->mScreenViewport.height / (float)fb->GetBuffers()->GetHeight()};
    uniforms.Offset = {0.0f, 0.0f};
  } else {
    uniforms.Scale = {
        screen->mScreenViewport.width / (float)fb->GetBuffers()->GetWidth(),
        -screen->mScreenViewport.height / (float)fb->GetBuffers()->GetHeight()};
    uniforms.Offset = {0.0f, 1.0f};
  }

  uniforms.HdrMode = 0;

  renderstate.Clear();
  renderstate.Shader = &hw_postprocess.present.Present;
  renderstate.Uniforms.Set(uniforms);
  renderstate.Viewport = box;
  renderstate.SetInputCurrent(0, PPFilterMode::Linear);
  renderstate.SetInputTexture(1, &hw_postprocess.present.Dither,
                              PPFilterMode::Nearest, PPWrapMode::Repeat);

  if (screenshot)
    renderstate.SetOutputNext();
  else {
    // For Metal, we might need a specific way to set the swapchain output
    // For now, assume it's the current target
  }

  renderstate.SetNoBlend();
  renderstate.Draw();
}

MTL::Texture *MtPostprocess::GetCurrentTexture() {
  auto buffers = fb->GetBuffers();
  if (!buffers || !buffers->PipelineImage[mCurrentPipelineImage])
    return nullptr;
  return buffers->PipelineImage[mCurrentPipelineImage]->GetTexture();
}

void MtPostprocess::SetSceneRenderTarget(bool useSSAO) {
  fb->GetRenderState()->SetRenderTarget(
      fb->GetBuffers()->SceneColor->GetTexture(),
      fb->GetBuffers()->SceneDepthStencil->GetTexture(),
      fb->GetBuffers()->GetWidth(), fb->GetBuffers()->GetHeight(),
      (int)MTL::PixelFormatRGBA16Float, fb->GetBuffers()->GetSceneSamples());
}
