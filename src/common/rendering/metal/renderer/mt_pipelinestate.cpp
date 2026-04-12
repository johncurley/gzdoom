#include "mt_pipelinestate.h"
#include "hwrenderer/data/hw_renderstate.h"
#include "hwrenderer/data/flatvertices.h" // New include for FFlatVertex
#include "metal/shaders/mt_shader.h"
#include "metal/system/mt_hwbuffer.h"
#include "metal/system/mt_renderdevice.h"
#include "metal/system/mt_binaryarchive.h"
#include "metal/renderer/mt_debug.h"
#include "printf.h"
#include "renderstyle.h"
#include <Foundation/Foundation.hpp>
#include <chrono>
#include <algorithm>
#include <Metal/Metal.hpp>

EXTERN_CVAR(Bool, mt_debug)

bool MtPipelineKey::operator==(const MtPipelineKey &other) const {
  return VertexFormat == other.VertexFormat &&
         SpecialEffect == other.SpecialEffect &&
         EffectState == other.EffectState && AlphaTest == other.AlphaTest &&
         BlendMode == other.BlendMode && DepthFunc == other.DepthFunc &&
         StencilOp == other.StencilOp && StencilFunc == other.StencilFunc &&
         ColorMask == other.ColorMask &&
         CullMode == other.CullMode && DepthClampMode == other.DepthClampMode &&
         DepthWrite == other.DepthWrite && StencilTest == other.StencilTest &&
         SampleCount == other.SampleCount &&
         DrawBufferCount == other.DrawBufferCount &&
         PixelFormat == other.PixelFormat &&
         DepthStencilFormat == other.DepthStencilFormat &&
         ClipDistanceMask == other.ClipDistanceMask &&
         IsShadowPass == other.IsShadowPass;
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
  hash ^= std::hash<int>()(key.StencilFunc) << 7;
  hash ^= std::hash<int>()(key.ColorMask) << 8;
  hash ^= std::hash<int>()(key.CullMode) << 9;
  hash ^= std::hash<int>()(key.DepthClampMode) << 10;
  hash ^= std::hash<int>()(key.DepthWrite) << 11;
  hash ^= std::hash<int>()(key.StencilTest) << 12;
  hash ^= std::hash<int>()(key.SampleCount) << 13;
  hash ^= std::hash<int>()(key.DrawBufferCount) << 14;
  hash ^= std::hash<int>()(key.PixelFormat) << 15;
  hash ^= std::hash<int>()(key.DepthStencilFormat) << 16;
  hash ^= std::hash<int>()(key.ClipDistanceMask) << 17;
  hash ^= std::hash<bool>()(key.IsShadowPass) << 18;
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

  // Cache miss — compile a new PSO synchronously on the main thread.
  // This can stall for 10-100ms on first use; record it so freeze spikes
  // show up clearly in the debug log even when mt_debug is off.
  auto compileStart = std::chrono::high_resolution_clock::now();

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

  float compileMs = std::chrono::duration<float, std::milli>(
      std::chrono::high_resolution_clock::now() - compileStart).count();

  if (fb->GetDebugManager())
    fb->GetDebugManager()->RecordStall("pso_compile", compileMs);

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

MTL::DepthStencilState *MtPipelineStateManager::GetPPStencilState() {
  if (mPPStencilState)
    return mPPStencilState;

  auto desc = MTL::DepthStencilDescriptor::alloc()->init();
  desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
  desc->setDepthWriteEnabled(false);

  auto stencilDesc = MTL::StencilDescriptor::alloc()->init();
  stencilDesc->setStencilCompareFunction(MTL::CompareFunctionEqual);
  stencilDesc->setStencilFailureOperation(MTL::StencilOperationKeep);
  stencilDesc->setDepthFailureOperation(MTL::StencilOperationKeep);
  stencilDesc->setDepthStencilPassOperation(MTL::StencilOperationKeep);
  stencilDesc->setReadMask(0xFFFFFFFF);
  stencilDesc->setWriteMask(0xFFFFFFFF);

  desc->setFrontFaceStencil(stencilDesc);
  desc->setBackFaceStencil(stencilDesc);
  stencilDesc->release();

  mPPStencilState = fb->device->device->newDepthStencilState(desc);
  desc->release();
  return mPPStencilState;
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

  if (mPPStencilState) {
    mPPStencilState->release();
    mPPStencilState = nullptr;
  }
}

MTL::RenderPipelineState *
MtPipelineStateManager::GetPPPipelineState(MtShaderProgram *program,
                                           MTL::PixelFormat colorFormat,
                                           FRenderStyle blendMode,
                                           MTL::PixelFormat depthStencilFormat,
                                           bool stencilTest) {
  PPKey key = {program, colorFormat, blendMode, depthStencilFormat, stencilTest};
  auto it = mPPPipelineCache.find(key);
  if (it != mPPPipelineCache.end())
    return it->second;

  auto desc = MTL::RenderPipelineDescriptor::alloc()->init();
  desc->setVertexFunction(program->vert->function);
  desc->setFragmentFunction(program->frag->fragmentFunction);

  auto vertexDesc = MTL::VertexDescriptor::alloc()->init();
  
  // Define standard attributes for PP
  auto attr0 = vertexDesc->attributes()->object(0);
  attr0->setFormat(MTL::VertexFormatFloat2); 
  attr0->setOffset(offsetof(FFlatVertex, x));
  attr0->setBufferIndex(0);
  
  auto attr1 = vertexDesc->attributes()->object(1);
  attr1->setFormat(MTL::VertexFormatFloat2); 
  attr1->setOffset(offsetof(FFlatVertex, u));
  attr1->setBufferIndex(0);

  // Robustly define all remaining attributes to satisfy Metal driver validation
  for (int i = 2; i < 16; i++) {
      auto attr = vertexDesc->attributes()->object(i);
      attr->setFormat(MTL::VertexFormatFloat4); // Default to something safe
      attr->setOffset(0);
      attr->setBufferIndex(0);
  }

  vertexDesc->layouts()->object(0)->setStride(sizeof(FFlatVertex)); // 32 bytes
  desc->setVertexDescriptor(vertexDesc);

  auto colorAttachment = desc->colorAttachments()->object(0);
  colorAttachment->setPixelFormat(colorFormat);
  colorAttachment->setWriteMask(MTL::ColorWriteMaskAll);

  // Configure depth/stencil format for PP
  if (depthStencilFormat != MTL::PixelFormatInvalid) {
      desc->setDepthAttachmentPixelFormat(depthStencilFormat);
      desc->setStencilAttachmentPixelFormat(depthStencilFormat);
  }

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

  // Use Binary Archive for caching if available
  auto archive = fb->GetBinaryArchive() ? fb->GetBinaryArchive()->GetArchive() : nullptr;
  if (archive) {
      auto archives = NS::Array::array((NS::Object* const *)&archive, 1);
      desc->setBinaryArchives(archives);
  }

  NS::Error *error = nullptr;
  MTL::RenderPipelineState *pipeline =
      fb->device->device->newRenderPipelineState(desc, &error);

  if (!pipeline && error) {
    Printf(PRINT_LOG, "Metal: Failed to create PP pipeline: %s\n",
           error->localizedDescription()->utf8String());
  } else if (pipeline) {
      // Add to archive for persistence
      if (fb->GetBinaryArchive()) {
          fb->GetBinaryArchive()->AddRenderPipeline(desc);
      }
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

  // Map depth functions to Metal (GZDoom EDepthFunc has only 3 values)
  // REVERSE-Z: Near is 1.0, Far is 0.0. Mapping must be inverted for scene pass.
  // SHADOW PASS: Use standard mapping (Near is 0.0, Far is 1.0).
  static const MTL::CompareFunction depthFuncsReverse[] = {
      MTL::CompareFunctionGreater,       // 0: DF_Less (becomes Greater)
      MTL::CompareFunctionGreaterEqual,  // 1: DF_LEqual (becomes GreaterEqual)
      MTL::CompareFunctionAlways         // 2: DF_Always
  };
  static const MTL::CompareFunction depthFuncsStandard[] = {
      MTL::CompareFunctionLess,          // 0: DF_Less
      MTL::CompareFunctionLessEqual,     // 1: DF_LEqual
      MTL::CompareFunctionAlways         // 2: DF_Always
  };

  const MTL::CompareFunction *funcs =
      key.IsShadowPass ? depthFuncsStandard : depthFuncsReverse;

  if (key.DepthFunc >= 0 && key.DepthFunc < 3) {
    desc->setDepthCompareFunction(funcs[key.DepthFunc]);
  } else {
    desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
  }
  desc->setDepthWriteEnabled(key.DepthWrite != 0);

  // Map GZDoom stencil operations to Metal (Correct 3-value mapping)
  static const MTL::StencilOperation stencilOps[] = {
      MTL::StencilOperationKeep,           // SOP_Keep (0)
      MTL::StencilOperationIncrementClamp, // SOP_Increment (1)
      MTL::StencilOperationDecrementClamp  // SOP_Decrement (2)
  };

  if (key.StencilTest != 0) {
    auto stencilDesc = MTL::StencilDescriptor::alloc()->init();
    
    // Stencil functions often use a wider range (e.g. SetStencil uses index 3 for Equal)
    static const MTL::CompareFunction stencilFuncs[] = {
        MTL::CompareFunctionLess,          // 0
        MTL::CompareFunctionLessEqual,     // 1
        MTL::CompareFunctionAlways,        // 2
        MTL::CompareFunctionEqual,         // 3: Used by SetStencil
        MTL::CompareFunctionNotEqual,      // 4
        MTL::CompareFunctionGreater,       // 5
        MTL::CompareFunctionGreaterEqual,  // 6
        MTL::CompareFunctionNever          // 7
    };

    if (key.StencilFunc >= 0 && key.StencilFunc < 8) {
        stencilDesc->setStencilCompareFunction(stencilFuncs[key.StencilFunc]);
    } else {
        stencilDesc->setStencilCompareFunction(MTL::CompareFunctionAlways);
    }

    stencilDesc->setStencilFailureOperation(MTL::StencilOperationKeep);
    stencilDesc->setDepthFailureOperation(MTL::StencilOperationKeep);
    
    if (key.StencilOp >= 0 && key.StencilOp < 3) {
        stencilDesc->setDepthStencilPassOperation(stencilOps[key.StencilOp]);
    } else {
        stencilDesc->setDepthStencilPassOperation(MTL::StencilOperationKeep);
    }

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
    desc->release();
    return nullptr;
  }

  auto module = program->vert; // Start with Vertex Module
  auto vertexFunction = module->function;
  auto fragmentFunction = program->frag->fragmentFunction;

  if (!vertexFunction || !fragmentFunction) {
    desc->release();
    return nullptr;
  }

  desc->setVertexFunction(vertexFunction);
  desc->setFragmentFunction(fragmentFunction);

  size_t stride = vertexBuffer ? vertexBuffer->GetStride() : ((key.VertexFormat >> 8) & 0xFF);
  if (stride == 0) stride = sizeof(FFlatVertex);

  auto vertexDesc = MTL::VertexDescriptor::alloc()->init();

  // Configure vertex descriptor
  if (vertexBuffer && vertexBuffer->GetNumAttributes() > 0) {
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

    // Robustly define attributes 0-11 to satisfy Metal validation for standard shaders
    for (int i = 0; i < 12; i++) {
        auto attr = vertexDesc->attributes()->object(i);
        if (attr->format() == MTL::VertexFormatInvalid) {
            // Map unknown attributes to a safe dummy slot (30)
            if (i == 8)
                attr->setFormat(MTL::VertexFormatUInt4);
            else
                attr->setFormat(MTL::VertexFormatFloat4);
            attr->setOffset(0);
            attr->setBufferIndex(30); 
        }
    }

    // Configure buffer layouts
    int numBindings = vertexBuffer->GetBindingPoints();
    for (int i = 0; i < numBindings; i++) {
      auto layoutDesc = vertexDesc->layouts()->object(i);
      layoutDesc->setStride(stride);
      layoutDesc->setStepFunction(MTL::VertexStepFunctionPerVertex);
    }
    
    // CRITICAL: Must also define layout for our dummy index 30
    auto dummyLayout = vertexDesc->layouts()->object(30);
    dummyLayout->setStride(16); // Non-zero stride to satisfy Intel driver validation
    dummyLayout->setStepFunction(MTL::VertexStepFunctionConstant);
    dummyLayout->setStepRate(0); // MUST be 0 for Constant step function

    desc->setVertexDescriptor(vertexDesc);
    vertexDesc->release();
  } else {
    // Fallback: Create a basic vertex descriptor for internal draws
    auto vertexDesc = MTL::VertexDescriptor::alloc()->init();
    vertexDesc->layouts()->object(0)->setStride(stride);
    vertexDesc->layouts()->object(0)->setStepFunction(MTL::VertexStepFunctionPerVertex);

    // Attribute 0: Position. 
    vertexDesc->attributes()->object(0)->setFormat(MTL::VertexFormatFloat3);
    vertexDesc->attributes()->object(0)->setOffset(0); 
    vertexDesc->attributes()->object(0)->setBufferIndex(0);

    // Attribute 1: TexCoord.
    vertexDesc->attributes()->object(1)->setFormat(MTL::VertexFormatFloat2);
    vertexDesc->attributes()->object(1)->setOffset(12);
    vertexDesc->attributes()->object(1)->setBufferIndex(0);

    // Attribute 2: Color (for 2D drawer fallback)
    auto attr2 = vertexDesc->attributes()->object(2);
    attr2->setFormat(MTL::VertexFormatUChar4Normalized);
    if (stride == 24) {
        attr2->setOffset(20); 
    } else {
        // For FFlatVertex (32), there is no color in the struct. 
        // We set it to offset 0 and hope the shader uses uVertexColor instead.
        attr2->setOffset(0);
    }
    attr2->setBufferIndex(0);

    // Robustly define attributes 0-11 to satisfy Metal validation for ANY shader
    for (int i = 0; i < 12; i++) {
        auto attr = vertexDesc->attributes()->object(i);
        if (attr->format() == MTL::VertexFormatInvalid) {
            if (i == 8)
                attr->setFormat(MTL::VertexFormatUInt4);
            else
                attr->setFormat(MTL::VertexFormatFloat4);
            attr->setOffset(0);
            attr->setBufferIndex(30);
        }
    }

    // Define layout for dummy index 30
    auto dummyLayout = vertexDesc->layouts()->object(30);
    dummyLayout->setStride(16);
    dummyLayout->setStepFunction(MTL::VertexStepFunctionConstant);
    dummyLayout->setStepRate(0);

    desc->setVertexDescriptor(vertexDesc);
    vertexDesc->release();
  }

  // Configure color attachments
  int numColorAttachments = key.DrawBufferCount;
  for (int i = 0; i < numColorAttachments; i++) {
    auto colorAttachment = desc->colorAttachments()->object(i);

    // Set pixel format
    MTL::PixelFormat format = MTL::PixelFormatBGRA8Unorm;
    if (i == 0) {
        format = (key.PixelFormat != 0) ? (MTL::PixelFormat)key.PixelFormat : MTL::PixelFormatBGRA8Unorm;
    } else if (i == 1) {
        format = MTL::PixelFormatBGRA8Unorm; // Fog
    } else if (i == 2) {
        // Normal
        format = fb->mVersionManager.supportsRGB10A2 ? MTL::PixelFormatRGB10A2Unorm : MTL::PixelFormatBGRA8Unorm;
    }
    colorAttachment->setPixelFormat(format);

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

  // Use Binary Archive for caching if available
  auto archive = fb->GetBinaryArchive() ? fb->GetBinaryArchive()->GetArchive() : nullptr;
  if (archive) {
      auto archives = NS::Array::array((NS::Object* const *)&archive, 1);
      desc->setBinaryArchives(archives);
  }

  // Create the pipeline state
  NS::Error *error = nullptr;
  auto state = device->newRenderPipelineState(desc, &error);

  if (!state && error) {
    Printf(PRINT_LOG, "Metal: Failed to create render pipeline state: %s\n",
           error->localizedDescription()->utf8String());
    error->release();
  } else if (state) {
    // Add to archive for persistence
    if (fb->GetBinaryArchive()) {
        fb->GetBinaryArchive()->AddRenderPipeline(desc);
    }
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
      MTL::BlendFactorZero,
      MTL::BlendFactorOne,
      MTL::BlendFactorSourceAlpha,
      MTL::BlendFactorOneMinusSourceAlpha,
      MTL::BlendFactorSourceColor,
      MTL::BlendFactorOneMinusSourceColor,
      MTL::BlendFactorDestinationColor,
      MTL::BlendFactorOneMinusDestinationColor,
      MTL::BlendFactorDestinationAlpha,
      MTL::BlendFactorOneMinusDestinationAlpha
  };

  // Metal Blend Operations mapping
  static const MTL::BlendOperation blendOps[] = {
      MTL::BlendOperationAdd, // Default/Placeholder
      MTL::BlendOperationAdd,
      MTL::BlendOperationSubtract,
      MTL::BlendOperationReverseSubtract,
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
    // Opaque check: STYLEOP_Add with Src=1, Dest=0 is opaque overwrite (NO BLENDING).
    // HOWEVER, for GZDoom, we must only disable blending if BOTH alpha and RGB are opaque.
    // Translucent style (SrcAlpha, InvSrcAlpha) MUST have blending enabled.
    if (style.SrcAlpha == STYLEALPHA_One && style.DestAlpha == STYLEALPHA_Zero && 
        style.BlendOp == STYLEOP_Add) {
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
    Printf(PRINT_LOG, "Metal: Warning - Unhandled blendMode: %d. Disabling blending.\n", style.BlendOp);
    attachment->setBlendingEnabled(false);
    return;
  }
}