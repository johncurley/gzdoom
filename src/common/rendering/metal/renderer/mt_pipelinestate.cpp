#include "mt_pipelinestate.h"
#include "hwrenderer/data/hw_renderstate.h"
#include "hwrenderer/data/flatvertices.h" // New include for FFlatVertex
#include "metal/shaders/mt_shader.h"
#include "metal/system/mt_hwbuffer.h"
#include "metal/system/mt_renderdevice.h"
#include "printf.h"
#include "renderstyle.h"
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

EXTERN_CVAR(Bool, mt_debug)

bool MtPipelineKey::operator==(const MtPipelineKey &other) const {
  return VertexFormat == other.VertexFormat &&
         SpecialEffect == other.SpecialEffect &&
         EffectState == other.EffectState && AlphaTest == other.AlphaTest &&
         BlendMode == other.BlendMode && DepthFunc == other.DepthFunc &&
         StencilOp == other.StencilOp && ColorMask == other.ColorMask &&
         CullMode == other.CullMode && DepthClampMode == other.DepthClampMode &&
         DepthWrite == other.DepthWrite && StencilTest == other.StencilTest &&
         SampleCount == other.SampleCount &&
         DrawBufferCount == other.DrawBufferCount &&
         PixelFormat == other.PixelFormat &&
         DepthStencilFormat == other.DepthStencilFormat &&
         ClipDistanceMask == other.ClipDistanceMask;
}

size_t std::hash<MtPipelineKey>::operator()(const MtPipelineKey &key) const {
  size_t hash = 0;
  hash ^= std::hash<int>()(key.VertexFormat);
  hash ^= std::hash<int>()(key.SpecialEffect) << 1;
  hash ^= std::hash<int>()(key.EffectState) << 2;
  hash ^= std::hash<int>()(key.AlphaTest) << 3;
  hash ^= std::hash<int>()(key.BlendMode) << 4;
  hash ^= std::hash<int>()(key.DepthFunc) << 5;
  hash ^= std::hash<int>()(key.StencilOp) << 6;
  hash ^= std::hash<int>()(key.ColorMask) << 7;
  hash ^= std::hash<int>()(key.CullMode) << 8;
  hash ^= std::hash<int>()(key.DepthClampMode) << 9;
  hash ^= std::hash<int>()(key.DepthWrite) << 10;
  hash ^= std::hash<int>()(key.StencilTest) << 11;
  hash ^= std::hash<int>()(key.SampleCount) << 12;
  hash ^= std::hash<int>()(key.DrawBufferCount) << 13;
  hash ^= std::hash<int>()(key.PixelFormat) << 14;
  hash ^= std::hash<int>()(key.DepthStencilFormat) << 15;
  hash ^= std::hash<int>()(key.ClipDistanceMask) << 16;
  return hash;
}

MtPipelineStateManager::MtPipelineStateManager(MetalRenderDevice *fb)
    : fb(fb) {}
MtPipelineStateManager::~MtPipelineStateManager() { ClearCache(); }

MtPipelineState *
MtPipelineStateManager::GetPipelineState(const MtPipelineKey &key,
                                         MtVertexBuffer *vertexBuffer) {
  auto it = mPipelineCache.find(key);
  if (it != mPipelineCache.end())
    return it->second.get();

  // Create new pipeline state
  auto state = std::make_unique<MtPipelineState>();
  state->Key = key;

  // Create render pipeline state
  state->pipelineState = CreateRenderPipelineState(key, vertexBuffer);
  if (!state->pipelineState)
    return nullptr;

  // Create depth/stencil state
  state->depthStencilState = CreateDepthStencilState(key);
  if (!state->depthStencilState) {
    state->pipelineState->release();
    return nullptr;
  }

  // Cache and return
  auto ptr = state.get();
  mPipelineCache[key] = std::move(state);
  return ptr;
}

MTL::DepthStencilState *MtPipelineStateManager::GetDisabledDepthStencilState() {
  if (mDisabledDepthStencilState)
    return mDisabledDepthStencilState;

  auto desc = MTL::DepthStencilDescriptor::alloc()->init();
  desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
  desc->setDepthWriteEnabled(false);

  mDisabledDepthStencilState = fb->device->device->newDepthStencilState(desc);
  desc->release();
  return mDisabledDepthStencilState;
}

void MtPipelineStateManager::ClearCache() {
  for (auto &pair : mPipelineCache) {
    auto &state = pair.second;
    if (state) {
      if (state->pipelineState)
        state->pipelineState->release();
      if (state->depthStencilState)
        state->depthStencilState->release();
    }
  }
  mPipelineCache.clear();

  for (auto &pair : mPPPipelineCache) {
    if (pair.second)
      pair.second->release();
  }
  mPPPipelineCache.clear();

  if (mDisabledDepthStencilState) {
    mDisabledDepthStencilState->release();
    mDisabledDepthStencilState = nullptr;
  }
}

MTL::RenderPipelineState *
MtPipelineStateManager::GetPPPipelineState(MtShaderProgram *program,
                                           MTL::PixelFormat colorFormat,
                                           FRenderStyle blendMode) {
  PPKey key = {program, colorFormat, blendMode};
  auto it = mPPPipelineCache.find(key);
  if (it != mPPPipelineCache.end())
    return it->second;

  auto desc = MTL::RenderPipelineDescriptor::alloc()->init();
  desc->setVertexFunction(program->vert->function);
  desc->setFragmentFunction(program->frag->fragmentFunction);

  auto vertexDesc = MTL::VertexDescriptor::alloc()->init();
  // Pos (x, z, y) - FFlatVertex layout.
  // This maps FFlatVertex.x to Position.x, FFlatVertex.z to Position.y, and FFlatVertex.y to Position.z.
  // For the screen quad, FFlatVertex.y is typically 0, and x/z are used for NDC.
  auto attr0 = vertexDesc->attributes()->object(0);
  attr0->setFormat(MTL::VertexFormatFloat3);
  attr0->setOffset(offsetof(FFlatVertex, x));
  attr0->setBufferIndex(0);
  
  // TexCoord (u, v)
  auto attr1 = vertexDesc->attributes()->object(1);
  attr1->setFormat(MTL::VertexFormatFloat2); // vec2
  attr1->setOffset(offsetof(FFlatVertex, u));
  attr1->setBufferIndex(0);

  vertexDesc->layouts()->object(0)->setStride(sizeof(FFlatVertex)); // FFlatVertex size
  desc->setVertexDescriptor(vertexDesc);

  auto colorAttachment = desc->colorAttachments()->object(0);
  colorAttachment->setPixelFormat(colorFormat);
  colorAttachment->setWriteMask(MTL::ColorWriteMaskAll);

  // Configure blend mode
  // Opaque (STYLEOP_Add, STYLEALPHA_One, STYLEALPHA_Zero)
  if (blendMode.SrcAlpha == (uint8_t)STYLEALPHA_One &&
      blendMode.DestAlpha == (uint8_t)STYLEALPHA_Zero) {
    colorAttachment->setBlendingEnabled(false); // Disable blending for opaque overwrite
  } else if (blendMode.SrcAlpha == (uint8_t)STYLEALPHA_One &&
             blendMode.DestAlpha == (uint8_t)STYLEALPHA_One) { // Additive
    colorAttachment->setBlendingEnabled(true);
    colorAttachment->setSourceRGBBlendFactor(MTL::BlendFactorOne);
    colorAttachment->setDestinationRGBBlendFactor(MTL::BlendFactorOne);
  } else if (blendMode.SrcAlpha == (uint8_t)STYLEALPHA_Src &&
             blendMode.DestAlpha == (uint8_t)STYLEALPHA_InvSrc) { // Translucent
    colorAttachment->setBlendingEnabled(true);
    colorAttachment->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
    colorAttachment->setDestinationRGBBlendFactor(
        MTL::BlendFactorOneMinusSourceAlpha);
  }

  NS::Error *error = nullptr;
  MTL::RenderPipelineState *pipeline =
      fb->device->device->newRenderPipelineState(desc, &error);

  if (!pipeline && error) {
    Printf("Metal: Failed to create PP pipeline: %s\n",
           error->localizedDescription()->utf8String());
  }

  vertexDesc->release();
  desc->release();

  mPPPipelineCache[key] = pipeline;
  return pipeline;
}

// ============================================================================
// Pipeline State Creation
// ============================================================================

MTL::DepthStencilState *
MtPipelineStateManager::CreateDepthStencilState(const MtPipelineKey &key) {
  auto desc = MTL::DepthStencilDescriptor::alloc()->init();

  // Map depth function enum to Metal
  static const MTL::CompareFunction depthFuncs[] = {
      MTL::CompareFunctionLess,      // DF_Less
      MTL::CompareFunctionLessEqual, // DF_LEqual
      MTL::CompareFunctionAlways     // DF_Always
  };

  // Configure depth test
  if (key.DepthFunc >= 0 && key.DepthFunc < 3) {
    desc->setDepthCompareFunction(depthFuncs[key.DepthFunc]);
  } else {
    desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
  }
  desc->setDepthWriteEnabled(key.DepthWrite != 0);

  // Configure stencil operations
  static const MTL::StencilOperation stencilOps[] = {
      MTL::StencilOperationKeep,           // SOP_Keep
      MTL::StencilOperationIncrementClamp, // SOP_Increment
      MTL::StencilOperationDecrementClamp  // SOP_Decrement
  };

  if (key.StencilTest != 0 && key.StencilOp >= 0 && key.StencilOp < 3) {
    auto stencilDesc = MTL::StencilDescriptor::alloc()->init();
    stencilDesc->setStencilCompareFunction(MTL::CompareFunctionEqual);
    stencilDesc->setStencilFailureOperation(MTL::StencilOperationKeep);
    stencilDesc->setDepthFailureOperation(MTL::StencilOperationKeep);
    stencilDesc->setDepthStencilPassOperation(stencilOps[key.StencilOp]);
    stencilDesc->setReadMask(0xFFFFFFFF);
    stencilDesc->setWriteMask(0xFFFFFFFF);

    desc->setFrontFaceStencil(stencilDesc);
    desc->setBackFaceStencil(stencilDesc);
    stencilDesc->release();
  }

  // Create the state
  auto device = (MTL::Device *)fb->device->device;
  auto state = device->newDepthStencilState(desc);
  desc->release();

  return state;
}

MTL::RenderPipelineState *MtPipelineStateManager::CreateRenderPipelineState(
    const MtPipelineKey &key, MtVertexBuffer *vertexBuffer) {
  if (mt_debug) {
      Printf("Metal: CreateRenderPipelineState. FFlatVertex size = %zu\n", sizeof(FFlatVertex));
  }
  auto desc = MTL::RenderPipelineDescriptor::alloc()->init();

  // Get or create shader based on key
  auto device = (MTL::Device *)fb->device->device;
  auto shaderManager = fb->GetShaderManager();
  MtShaderProgram *program = nullptr;

  if (key.SpecialEffect != EFF_NONE) {
    program = shaderManager->GetEffect(key.SpecialEffect,
                                       key.DrawBufferCount > 1 ? GBUFFER_PASS
                                                               : NORMAL_PASS);
  } else {
    program = shaderManager->Get(key.EffectState, key.AlphaTest != 0,
                                 key.DrawBufferCount > 1 ? GBUFFER_PASS
                                                         : NORMAL_PASS);
  }

  if (!program || !program->vert || !program->frag) {
    static int warnCount = 0;
    if (warnCount++ < 5)
      Printf(
          "Metal: Failed to get shader for effect=%d state=%d alphaTest=%d\n",
          key.SpecialEffect, key.EffectState, key.AlphaTest);
    desc->release();
    return nullptr;
  }

  auto module = program->vert; // Start with Vertex Module
  auto vertexFunction = module->function;
  auto fragmentFunction = program->frag->fragmentFunction;

  if (!vertexFunction || !fragmentFunction) {
    Printf("Metal: Failed to load shader functions from default library\n");
    desc->release();
    return nullptr;
  }

  desc->setVertexFunction(vertexFunction);
  desc->setFragmentFunction(fragmentFunction);

  size_t stride = vertexBuffer ? vertexBuffer->GetStride() : ((key.VertexFormat >> 8) & 0xFF);
  if (stride == 0) stride = sizeof(FFlatVertex);

  // Configure vertex descriptor
  if (vertexBuffer && vertexBuffer->GetNumAttributes() > 0) {
    auto vertexDesc = MTL::VertexDescriptor::alloc()->init();

    int numAttrs = vertexBuffer->GetNumAttributes();
    const FVertexBufferAttribute *attrs = vertexBuffer->GetAttributes();

    for (int i = 0; i < numAttrs; i++) {
      // Map GZDoom vertex format to Metal vertex format
      MTL::VertexFormat mtlFormat;
      switch (attrs[i].format) {
      case VFmt_Float4:
        mtlFormat = MTL::VertexFormatFloat4;
        break;
      case VFmt_Float3:
        mtlFormat = MTL::VertexFormatFloat3;
        break;
      case VFmt_Float2:
        mtlFormat = MTL::VertexFormatFloat2;
        break;
      case VFmt_Float:
        mtlFormat = MTL::VertexFormatFloat;
        break;
      case VFmt_Byte4:
        mtlFormat = MTL::VertexFormatUChar4Normalized;
        break;
      case VFmt_Packed_A2R10G10B10:
        mtlFormat = MTL::VertexFormatUInt1010102Normalized;
        break;
      case VFmt_Byte4_UInt:
        mtlFormat = MTL::VertexFormatUChar4;
        break;
      default:
        mtlFormat = MTL::VertexFormatFloat4;
        break;
      }

      auto attrDesc = vertexDesc->attributes()->object(attrs[i].location);
      attrDesc->setFormat(mtlFormat);
      attrDesc->setOffset(attrs[i].offset);
      attrDesc->setBufferIndex(attrs[i].binding);
    }

    // Fix: Ensure all potentially required attributes (2-8) are defined if the shader expects them but the buffer doesn't provide them.
    // This aliases them to existing attributes to satisfy Metal validation.
    auto attr2 = vertexDesc->attributes()->object(2); // aColor
    if (attr2->format() == MTL::VertexFormatInvalid) {
        attr2->setFormat(MTL::VertexFormatUChar4Normalized);
        attr2->setOffset(0); // Alias to Position (reads first 4 bytes as color)
        attr2->setBufferIndex(0);
    }
    // Attribute 3: aVertex2
    auto attr3 = vertexDesc->attributes()->object(3);
    if (attr3->format() == MTL::VertexFormatInvalid) {
        if (stride == 24) {
             // Alias to TexCoord (offset 12) for 2D vertex buffer to avoid reading out of bounds
             // FFlatVertex 24: Pos(0-11), UV(12-19), Color(20-23)
             attr3->setFormat(MTL::VertexFormatFloat2);
             attr3->setOffset(offsetof(FFlatVertex, u)); 
             attr3->setBufferIndex(0);
        } else {
             // Alias to Position for 32-byte buffer
             attr3->setFormat(MTL::VertexFormatFloat2);
             attr3->setOffset(0); 
             attr3->setBufferIndex(0);
        }
    }
    auto attr4 = vertexDesc->attributes()->object(4); // aNormal
    if (attr4->format() == MTL::VertexFormatInvalid) {
        attr4->setFormat(MTL::VertexFormatFloat3);
        attr4->setOffset(0); // Alias to Position
        attr4->setBufferIndex(0);
    }
    auto attr5 = vertexDesc->attributes()->object(5); // aNormal2
    if (attr5->format() == MTL::VertexFormatInvalid) {
        attr5->setFormat(MTL::VertexFormatFloat3);
        attr5->setOffset(0); // Alias to Position
        attr5->setBufferIndex(0);
    }
    auto attr6 = vertexDesc->attributes()->object(6); // aLightmap
    if (attr6->format() == MTL::VertexFormatInvalid) {
        if (stride == 24) {
             // Alias to TexCoord (offset 12) for 2D
             attr6->setFormat(MTL::VertexFormatFloat2);
             attr6->setOffset(offsetof(FFlatVertex, u));
             attr6->setBufferIndex(0);
        } else {
             // Alias to Position for 32-byte
             attr6->setFormat(MTL::VertexFormatFloat2);
             attr6->setOffset(0);
             attr6->setBufferIndex(0);
        }
    }
    auto attr7 = vertexDesc->attributes()->object(7); // aBoneWeight
    if (attr7->format() == MTL::VertexFormatInvalid) {
        attr7->setFormat(MTL::VertexFormatFloat4);
        attr7->setOffset(0); // Alias to Position
        attr7->setBufferIndex(0);
    }
    auto attr8 = vertexDesc->attributes()->object(8); // aBoneSelector
    if (attr8->format() == MTL::VertexFormatInvalid) {
        attr8->setFormat(MTL::VertexFormatUInt4);
        attr8->setOffset(0); // Alias to Position
        attr8->setBufferIndex(0);
    }

    // Configure buffer layouts
    int numBindings = vertexBuffer->GetBindingPoints();
    for (int i = 0; i < numBindings; i++) {
      auto layoutDesc = vertexDesc->layouts()->object(i);
      layoutDesc->setStride(stride);
      layoutDesc->setStepFunction(MTL::VertexStepFunctionPerVertex);
    }

    desc->setVertexDescriptor(vertexDesc);
    vertexDesc->release();
  } else {
    // Fallback: Create a basic vertex descriptor for 2D rendering if no buffer
    // provided This is useful for ClearScreen or other internal draws
    auto vertexDesc = MTL::VertexDescriptor::alloc()->init();

    vertexDesc->layouts()->object(0)->setStride(stride);
    vertexDesc->layouts()->object(0)->setStepFunction(
        MTL::VertexStepFunctionPerVertex);

    // Attribute 0: Position (float3)
    vertexDesc->attributes()->object(0)->setFormat(MTL::VertexFormatFloat3);
    vertexDesc->attributes()->object(0)->setOffset(offsetof(FFlatVertex, x));
    vertexDesc->attributes()->object(0)->setBufferIndex(0);

    // Attribute 1: TexCoord (float2)
    vertexDesc->attributes()->object(1)->setFormat(MTL::VertexFormatFloat2);
    vertexDesc->attributes()->object(1)->setOffset(offsetof(FFlatVertex, u));
    vertexDesc->attributes()->object(1)->setBufferIndex(0);

    // Attribute 2: Color (uchar4 normalized) - Alias to Position if color missing
    vertexDesc->attributes()->object(2)->setFormat(MTL::VertexFormatUChar4Normalized);
    vertexDesc->attributes()->object(2)->setOffset(offsetof(FFlatVertex, x));
    vertexDesc->attributes()->object(2)->setBufferIndex(0);

    // Attribute 3: aVertex2 (float2) - Points to TexCoord if stride is 24, or lindex if 32
    if (stride == 32) {
        vertexDesc->attributes()->object(3)->setFormat(MTL::VertexFormatFloat);
        vertexDesc->attributes()->object(3)->setOffset(offsetof(FFlatVertex, lindex));
        vertexDesc->attributes()->object(3)->setBufferIndex(0);
    } else {
        // Fallback for 24-byte FFlatVertex or TwoDVertex
        vertexDesc->attributes()->object(3)->setFormat(MTL::VertexFormatFloat2);
        vertexDesc->attributes()->object(3)->setOffset(offsetof(FFlatVertex, u));
        vertexDesc->attributes()->object(3)->setBufferIndex(0);
    }

    // Attribute 6: aLightmap (float2) - Points to lu, lv if 32 bytes
    if (stride == 32) {
        vertexDesc->attributes()->object(6)->setFormat(MTL::VertexFormatFloat2);
        vertexDesc->attributes()->object(6)->setOffset(offsetof(FFlatVertex, lu));
        vertexDesc->attributes()->object(6)->setBufferIndex(0);
    } else {
        // Fallback for 24-byte
        vertexDesc->attributes()->object(6)->setFormat(MTL::VertexFormatFloat2);
        vertexDesc->attributes()->object(6)->setOffset(offsetof(FFlatVertex, u));
        vertexDesc->attributes()->object(6)->setBufferIndex(0);
    }

    desc->setVertexDescriptor(vertexDesc);
    vertexDesc->release();
  }

  // Configure color attachments
  int numColorAttachments = key.DrawBufferCount;
  for (int i = 0; i < numColorAttachments; i++) {
    auto colorAttachment = desc->colorAttachments()->object(i);

    // Set pixel format
    if (key.PixelFormat != 0)
      colorAttachment->setPixelFormat((MTL::PixelFormat)key.PixelFormat);
    else
      colorAttachment->setPixelFormat(MTL::PixelFormatBGRA8Unorm); // Default

    // Configure blend mode
    ConfigureBlendMode(colorAttachment, key.BlendMode);

    // Set color write mask
    MTL::ColorWriteMask writeMask = MTL::ColorWriteMaskNone;
    if (key.ColorMask & 1)
      writeMask |= MTL::ColorWriteMaskRed;
    if (key.ColorMask & 2)
      writeMask |= MTL::ColorWriteMaskGreen;
    if (key.ColorMask & 4)
      writeMask |= MTL::ColorWriteMaskBlue;
    if (key.ColorMask & 8)
      writeMask |= MTL::ColorWriteMaskAlpha;
    colorAttachment->setWriteMask(writeMask);
  }

  // Configure depth/stencil format
  if (key.DepthStencilFormat != 0) {
    desc->setDepthAttachmentPixelFormat(
        (MTL::PixelFormat)key.DepthStencilFormat);
    desc->setStencilAttachmentPixelFormat(
        (MTL::PixelFormat)key.DepthStencilFormat);
  }

  // Configure sample count (MSAA)
  desc->setRasterSampleCount(key.SampleCount > 0 ? key.SampleCount : 1);

  // Culling is set dynamically via render command encoder, not in pipeline
  // state Metal doesn't support depth clamp in pipeline descriptor (requires
  // feature check)

  // Create the pipeline state
  NS::Error *error = nullptr;
  auto state = device->newRenderPipelineState(desc, &error);

  if (!state && error) {
    Printf("Metal: Failed to create render pipeline state: %s\n",
           error->localizedDescription()->utf8String());
    error->release();
  } else if (state) {
    Printf("Metal: Pipeline state created successfully (effect=%d, state=%d, "
           "alpha=%d, vfmt=%d)\n",
           key.SpecialEffect, key.EffectState, key.AlphaTest, key.VertexFormat);
  }

  desc->release();
  return state;
}

// Helper method to configure blend mode
void MtPipelineStateManager::ConfigureBlendMode(
    MTL::RenderPipelineColorAttachmentDescriptor *attachment, int blendMode) {

  // Default to no blending
  attachment->setBlendingEnabled(false);

  // Unpack FRenderStyle from the int
  FRenderStyle style;
  style.AsDWORD = blendMode;

  // Metal Blend Factors mapping
  static const MTL::BlendFactor blendFactors[] = {
      MTL::BlendFactorZero,                     // STYLEALPHA_Zero
      MTL::BlendFactorOne,                      // STYLEALPHA_One
      MTL::BlendFactorSourceAlpha,              // STYLEALPHA_Src
      MTL::BlendFactorOneMinusSourceAlpha,      // STYLEALPHA_InvSrc
      MTL::BlendFactorSourceColor,              // STYLEALPHA_SrcCol
      MTL::BlendFactorOneMinusSourceColor,      // STYLEALPHA_InvSrcCol
      MTL::BlendFactorDestinationColor,         // STYLEALPHA_DstCol
      MTL::BlendFactorOneMinusDestinationColor, // STYLEALPHA_InvDstCol
      MTL::BlendFactorDestinationAlpha,         // STYLEALPHA_Dst
      MTL::BlendFactorOneMinusDestinationAlpha  // STYLEALPHA_InvDst
  };

  // Metal Blend Operations mapping
  static const MTL::BlendOperation blendOps[] = {
      MTL::BlendOperationAdd, // Default/Placeholder
      MTL::BlendOperationAdd,             // STYLEOP_Add
      MTL::BlendOperationSubtract,        // STYLEOP_Sub
      MTL::BlendOperationReverseSubtract, // STYLEOP_RevSub
  };

  MTL::BlendFactor srcRGBFactor = blendFactors[style.SrcAlpha % STYLEALPHA_MAX];
  MTL::BlendFactor dstRGBFactor = blendFactors[style.DestAlpha % STYLEALPHA_MAX];
  MTL::BlendOperation rgbBlendOp = MTL::BlendOperationAdd; // Default to Add

  if (style.BlendOp >= STYLEOP_Add && style.BlendOp <= STYLEOP_RevSub) {
      rgbBlendOp = blendOps[style.BlendOp];
  }

  // Alpha blending typically follows RGB, or is additive
  MTL::BlendFactor srcAlphaFactor = blendFactors[style.SrcAlpha % STYLEALPHA_MAX];
  MTL::BlendFactor dstAlphaFactor = blendFactors[style.DestAlpha % STYLEALPHA_MAX];
  MTL::BlendOperation alphaBlendOp = MTL::BlendOperationAdd;

  switch (style.BlendOp) {
  case STYLEOP_None:
    attachment->setBlendingEnabled(false);
    return;

  case STYLEOP_Shadow:
    // Special blend mode for shadows (similar to Opaque but dims destination)
    attachment->setBlendingEnabled(true);
    attachment->setSourceRGBBlendFactor(MTL::BlendFactorZero);
    attachment->setDestinationRGBBlendFactor(MTL::BlendFactorSourceAlpha); // Use source alpha to dim background
    attachment->setRgbBlendOperation(MTL::BlendOperationAdd);
    attachment->setSourceAlphaBlendFactor(MTL::BlendFactorZero);
    attachment->setDestinationAlphaBlendFactor(MTL::BlendFactorOne);
    attachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
    return;

  case STYLEOP_Add:
  case STYLEOP_Sub:
  case STYLEOP_RevSub:
    if (style.AsDWORD == STYLE_Normal) {
        attachment->setBlendingEnabled(false);
        return;
    }
    attachment->setBlendingEnabled(true);
    attachment->setSourceRGBBlendFactor(srcRGBFactor);
    attachment->setDestinationRGBBlendFactor(dstRGBFactor);
    attachment->setRgbBlendOperation(rgbBlendOp);
    attachment->setSourceAlphaBlendFactor(srcAlphaFactor);
    attachment->setDestinationAlphaBlendFactor(dstAlphaFactor);
    attachment->setAlphaBlendOperation(alphaBlendOp);
    return;

  default:
    // Handle Fuzzy styles or any unmapped complex styles
    // For now, default to additive blend if unhandled, or disable if unsure
    if (style.BlendOp == STYLEOP_Fuzz || style.BlendOp == STYLEOP_FuzzOrAdd || 
        style.BlendOp == STYLEOP_FuzzOrSub || style.BlendOp == STYLEOP_FuzzOrRevSub)
    {
        // Default fuzz to translucent style if not handled more specifically
        attachment->setBlendingEnabled(true);
        attachment->setSourceRGBBlendFactor(MTL::BlendFactorDestinationColor); // Similar to Vulkan fuzz
        attachment->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
        attachment->setRgbBlendOperation(MTL::BlendOperationAdd);
        attachment->setSourceAlphaBlendFactor(MTL::BlendFactorZero);
        attachment->setDestinationAlphaBlendFactor(MTL::BlendFactorOne);
        attachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
        return;
    }
    Printf("Metal: Warning - Unhandled blendMode: %d. Disabling blending.\n", style.BlendOp);
    attachment->setBlendingEnabled(false);
    return;
  }
}