#include "i_time.h"
#include <Metal/Metal.hpp>

#include "cmdlib.h"
#include "common/textures/textures.h" // For usershaders array
#include "common/thirdparty/superfasthash.h"
#include "engineerrors.h"
#include "filesystem.h"
#include "hwrenderer/data/hw_shaderpatcher.h"
#include "i_specialpaths.h"
#include "metal/renderer/mt_pipelinestate.h"
#include "metal/system/mt_renderdevice.h"
#include "metal/system/mt_binaryarchive.h"
#include "gamestate.h"
#include "mt_shader.h"
#include "printf.h"
#include <fstream>
#include <regex>
#include <set>
#include <shadertranslator/shader_translator.h>
#include <sstream>

EXTERN_CVAR(Bool, mt_debug)

// glslang includes for GLSL → SPIR-V compilation
#include "glslang/glslang/Public/ShaderLang.h"
#include "glslang/spirv/GlslangToSpv.h"

// Helper function to get default glslang resources
static TBuiltInResource GetDefaultTBuiltInResource() {
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

static const char *shaderBindings = R"(

	layout(binding = 14) uniform sampler2D ShadowMap;
	layout(binding = 15) uniform sampler2DArray LightMap;

	// UBO/SSBO Bindings remapped to avoid texture slots (0-15)
	// Metal Argument Buffers or direct bindings

	// This must match the HWViewpointUniforms struct
	layout(binding = 17, std140) uniform readonly ViewpointUBO {
		mat4 ProjectionMatrix;
		mat4 ViewMatrix;
		mat4 NormalViewMatrix;

		vec4 uCameraPos;
		vec4 uClipLine;

		float uGlobVis;			// uGlobVis = R_GetGlobVis(r_visibility) / 32.0
		int uPalLightLevels;	
		int uViewHeight;		// Software fuzz scaling
		float uClipHeight;
		float uClipHeightDirection;
		int uShadowmapFilter;
		
		int uLightBlendMode;

		float uThickFogDistance;
		float uThickFogMultiplier;
	};

	layout(binding = 19, std140) uniform readonly MatricesUBO {
		mat4 ModelMatrix;
		mat4 NormalModelMatrix;
		mat4 TextureMatrix;
	};

	struct StreamData
	{
		vec4 uObjectColor;
		vec4 uObjectColor2;
		vec4 uDynLightColor;
		vec4 uAddColor;
		vec4 uTextureAddColor;
		vec4 uTextureModulateColor;
		vec4 uTextureBlendColor;
		vec4 uFogColor;
		float uDesaturationFactor;
		float uInterpolationFactor;
		float timer; // timer data for material shaders
		int useVertexData;
		vec4 uVertexColor;
		vec4 uVertexNormal;

		vec4 uGlowTopPlane;
		vec4 uGlowTopColor;
		vec4 uGlowBottomPlane;
		vec4 uGlowBottomColor;

		vec4 uGradientTopPlane;
		vec4 uGradientBottomPlane;

		vec4 uSplitTopPlane;
		vec4 uSplitBottomPlane;

		vec4 uDetailParms;
		vec4 uNpotEmulation;
		vec4 padding1, padding2, padding3;
	};

	layout(binding = 20, std140) uniform readonly StreamUBO {
		StreamData data[MAX_STREAM_DATA];
	};

	// light buffers
	layout(binding = 16, std430) buffer readonly LightBufferSSO
	{
	    vec4 lights[];
	};

	// bone matrix buffers
	layout(binding = 18, std430) buffer readonly BoneBufferSSO
	{
	    mat4 bones[];
	};

	// textures
	// In Metal, these map to texture slots 0-11
	layout(binding = 0) uniform sampler2D tex;
	layout(binding = 1) uniform sampler2D texture2;
	layout(binding = 2) uniform sampler2D texture3;
	layout(binding = 3) uniform sampler2D texture4;
	layout(binding = 4) uniform sampler2D texture5;
	layout(binding = 5) uniform sampler2D texture6;
	layout(binding = 6) uniform sampler2D texture7;
	layout(binding = 7) uniform sampler2D texture8;
	layout(binding = 8) uniform sampler2D texture9;
	layout(binding = 9) uniform sampler2D texture10;
	layout(binding = 10) uniform sampler2D texture11;
	layout(binding = 11) uniform sampler2D texture12;

	// This must match the PushConstants struct
	layout(binding = 21, std140) uniform PushConstants
	{
		vec4 group1; // xy: uClipSplit, zw: uSpecularMaterial
		vec4 group2; // x: uLightLevel, y: uFogDensity, z: uLightFactor, w: uLightDist
		vec4 group3; // x: uTextureMode, y: uAlphaThreshold, z: uFogEnabled, w: uLightIndex
		vec4 group4; // x: uBoneIndexBase, y: uDataIndex
	};

	#define uClipSplit group1.xy
	#define uSpecularMaterial group1.zw
	#define uLightLevel group2.x
	#define uFogDensity group2.y
	#define uLightFactor group2.z
	#define uLightDist group2.w
	#define uTextureMode int(group3.x)
	#define uAlphaThreshold group3.y
	#define uFogEnabled int(group3.z)
	#define uLightIndex int(group3.w)
	#define uBoneIndexBase int(group4.x)
	#define uDataIndex int(group4.y)

	// material types
	#if defined(SPECULAR)
	#define normaltexture texture2
	#define speculartexture texture3
	#define brighttexture texture4
	#define detailtexture texture5
	#define glowtexture texture6
	#elif defined(PBR)
	#define normaltexture texture2
	#define metallictexture texture3
	#define roughnesstexture texture4
	#define aotexture texture5
	#define brighttexture texture6
	#define detailtexture texture7
	#define glowtexture texture8
	#else
	#define brighttexture texture2
	#define detailtexture texture3
	#define glowtexture texture4
	#endif

	#define uObjectColor data[uDataIndex].uObjectColor
	#define uObjectColor2 data[uDataIndex].uObjectColor2
	#define uDynLightColor data[uDataIndex].uDynLightColor
	#define uAddColor data[uDataIndex].uAddColor
	#define uTextureBlendColor data[uDataIndex].uTextureBlendColor
	#define uTextureModulateColor data[uDataIndex].uTextureModulateColor
	#define uTextureAddColor data[uDataIndex].uTextureAddColor
	#define uFogColor data[uDataIndex].uFogColor
	#define uDesaturationFactor data[uDataIndex].uDesaturationFactor
	#define uInterpolationFactor data[uDataIndex].uInterpolationFactor
	#define timer data[uDataIndex].timer
	#define useVertexData data[uDataIndex].useVertexData
	#define uVertexColor data[uDataIndex].uVertexColor
	#define uVertexNormal data[uDataIndex].uVertexNormal
	#define uGlowTopPlane data[uDataIndex].uGlowTopPlane
	#define uGlowTopColor data[uDataIndex].uGlowTopColor
	#define uGlowBottomPlane data[uDataIndex].uGlowBottomPlane
	#define uGlowBottomColor data[uDataIndex].uGlowBottomColor
	#define uGradientTopPlane data[uDataIndex].uGradientTopPlane
	#define uGradientBottomPlane data[uDataIndex].uGradientBottomPlane
	#define uSplitTopPlane data[uDataIndex].uSplitTopPlane
	#define uSplitBottomPlane data[uDataIndex].uSplitBottomPlane
	#define uDetailParms data[uDataIndex].uDetailParms
	#define uNpotEmulation data[uDataIndex].uNpotEmulation

	#define SUPPORTS_SHADOWMAPS
	#define HAS_UNIFORM_VERTEX_DATA

	float noise1(float) { return 0; }
	vec2 noise2(vec2) { return vec2(0); }
	vec3 noise3(vec3) { return vec3(0); }
	vec4 noise4(vec4) { return vec4(0); }
)";

MtShaderManager::MtShaderManager(MetalRenderDevice *fb) : fb(fb) {
  // Initialize glslang (once per program)
  glslang::InitializeProcess();
}

MtShaderManager::~MtShaderManager() {
  ClearCache();
  if (mNativeLibrary) {
    mNativeLibrary->release();
    mNativeLibrary = nullptr;
  }
  // Finalize glslang
  glslang::FinalizeProcess();
}

std::shared_ptr<MtShaderModule>
MtShaderManager::CompileShader(const std::string &name,
                               const std::string &vertexSource,
                               const std::string &fragmentSource,
                               const std::vector<std::string> &defines) {
  // Key the cache on the SOURCE, not just the name. Several PPShaders share a
  // fragment filename and differ only in their Defines, which are baked into
  // the source text but were absent from this key -- so the second variant to
  // be requested silently received the first one's compiled module.
  //
  // This was not hypothetical. `shaders/pp/blur.fp` is both BlurHorizontal and
  // BlurVertical; PPBloom::RenderBloom runs the horizontal step first, so every
  // "vertical" blur in the Metal PP path was a second horizontal blur. Measured
  // 2026-08-05: the reference bloom blob's bbox was identical before and after
  // its vertical pass, and the pyramid elongated it 1.37 -> 1.90 -> 2.50:1.
  // `shaders/pp/tonemap.fp` has five variants and was affected the same way.
  //
  // The on-disk cache below was always keyed correctly, by a hash of the
  // source; only this in-memory map was wrong, which is why a cache wipe never
  // helped.
  uint32_t sourceHash = 0;
  if (!vertexSource.empty())
    sourceHash = SuperFastHash(vertexSource.c_str(), vertexSource.length());
  if (!fragmentSource.empty())
    sourceHash = SuperFastHash(fragmentSource.c_str(), fragmentSource.length()) ^
                 (sourceHash << 1);
  for (auto &d : defines)
    sourceHash = SuperFastHash(d.c_str(), d.length()) ^ (sourceHash << 1);

  char keyBuf[64];
  snprintf(keyBuf, sizeof(keyBuf), "#%08x", sourceHash);
  const std::string cacheKey = name + keyBuf;

  // Check cache first
  auto it = mShaderCache.find(cacheKey);
  if (it != mShaderCache.end())
    return it->second;

  // Create shader module
  auto module = std::make_shared<MtShaderModule>();
  module->name = name;

  // Compile vertex shader: GLSL → SPIR-V → MSL → MTLLibrary
  if (!vertexSource.empty()) {
    uint32_t hash = SuperFastHash(vertexSource.c_str(), vertexSource.length());
    for (auto &d : defines)
      hash = SuperFastHash(d.c_str(), d.length()) ^ (hash << 1);

    char cacheKey[128];
    snprintf(cacheKey, sizeof(cacheKey), "%s_%08x_vert", name.c_str(), hash);
    std::string cachePath = GetCachePath(cacheKey);
    std::string vertexMSL;

    std::ifstream vfile(cachePath);
    if (vfile.is_open()) {
      std::stringstream ss;
      ss << vfile.rdbuf();
      vertexMSL = ss.str();
    } else {
      // Step 1: GLSL → SPIR-V
      auto vertexSPIRV =
          CompileGLSLToSPIRV(vertexSource, name + "_vert", true, defines);
      if (vertexSPIRV.empty()) {
        return nullptr;
      }

      // Step 2: SPIR-V → MSL
      vertexMSL =
          TranslateSPIRVToMSL(vertexSPIRV, true, name + "_vert"); // Pass name
      if (vertexMSL.empty()) {
        if (module->function)
          ((MTL::Function *)module->function)->release();
        if (module->library)
          ((MTL::Library *)module->library)->release();
        return nullptr;
      }

      // Save to cache
      std::ofstream outfile(cachePath);
      if (outfile.is_open()) {
        outfile << vertexMSL;
      }
    }

    // Step 3: MSL → MTLLibrary
    module->library = CompileMSLToLibrary(vertexMSL, name + "_vert");
    if (!module->library) {
      Printf(PRINT_LOG,
             "Metal: Failed to compile vertex shader MSL to library: %s\n",
             name.c_str());
      return nullptr;
    }

    // Get main function
    NS::String *funcName = NS::String::string("main0", NS::UTF8StringEncoding);
    module->function = module->library->newFunction(funcName);
    funcName->release();

    if (!module->function) {
      Printf(PRINT_LOG,
             "Metal: Failed to find main0 function in vertex shader: %s\n",
             name.c_str());
      module->library->release();
      return nullptr;
    }

    module->entryPoint = "main0";
    module->isReady = true;
  }

  // Compile fragment shader: GLSL → SPIR-V → MSL → MTLLibrary
  if (!fragmentSource.empty()) {
    uint32_t hash =
        SuperFastHash(fragmentSource.c_str(), fragmentSource.length());
    for (auto &d : defines)
      hash = SuperFastHash(d.c_str(), d.length()) ^ (hash << 1);

    char cacheKey[128];
    snprintf(cacheKey, sizeof(cacheKey), "%s_%08x_frag", name.c_str(), hash);
    std::string cachePath = GetCachePath(cacheKey);
    std::string fragmentMSL;

    std::ifstream ffile(cachePath);
    if (ffile.is_open()) {
      std::stringstream ss;
      ss << ffile.rdbuf();
      fragmentMSL = ss.str();
    } else {
      // Step 1: GLSL → SPIR-V
      auto fragmentSPIRV =
          CompileGLSLToSPIRV(fragmentSource, name + "_frag", false, defines);
      if (fragmentSPIRV.empty()) {
        if (module->function)
          ((MTL::Function *)module->function)->release();
        if (module->library)
          ((MTL::Library *)module->library)->release();
        return nullptr;
      }

      // Step 2: SPIR-V → MSL
      fragmentMSL = TranslateSPIRVToMSL(fragmentSPIRV, false,
                                        name + "_frag"); // Pass name
      if (fragmentMSL.empty()) {
        if (module->function)
          ((MTL::Function *)module->function)->release();
        if (module->library)
          ((MTL::Library *)module->library)->release();
        return nullptr;
      }

      // Save to cache
      std::ofstream outfile(cachePath);
      if (outfile.is_open()) {
        outfile << fragmentMSL;
      }
    }

    // Step 3: MSL → MTLLibrary
    module->fragmentLibrary = CompileMSLToLibrary(fragmentMSL, name + "_frag");
    if (!module->fragmentLibrary) {
      Printf(PRINT_LOG,
             "Metal: Failed to compile fragment shader MSL to library: %s\n",
             name.c_str());
      if (module->function)
        ((MTL::Function *)module->function)->release();
      if (module->library)
        ((MTL::Library *)module->library)->release();
      return nullptr;
    }

    // Get main function for fragment shader
    NS::String *fragFuncName =
        NS::String::string("main0", NS::UTF8StringEncoding);
    module->fragmentFunction =
        module->fragmentLibrary->newFunction(fragFuncName);
    fragFuncName->release();

    if (!module->fragmentFunction) {
      Printf(PRINT_LOG,
             "Metal: Failed to find main0 function in fragment shader: %s\n",
             name.c_str());
      module->fragmentLibrary->release();
      if (module->function)
        module->function->release();
      if (module->library)
        module->library->release();
      return nullptr;
    }
  }

  // Cache the compiled shader
  module->isReady = true;
  mShaderCache[cacheKey] = module;

  return module;
}

MtShaderProgram *MtShaderManager::GetEffect(int effect, EPassType passType) {
  if (compileIndex == -1 && effect >= 0 && effect < MAX_EFFECTS &&
      mEffectShaders[passType][effect].frag) {
    return &mEffectShaders[passType][effect];
  }
  return nullptr;
}

MtShaderProgram *MtShaderManager::Get(unsigned int eff, bool alphateston,
                                      EPassType passType) {
  if (compileIndex != -1) {
    if (mMaterialShaders[0].size() > 0)
      return &mMaterialShaders[0][0];
    return nullptr;
  }

  if (!alphateston && eff < 4) // SHADER_NoTexture is usually around here
  {
    if (eff < mMaterialShadersNAT[passType].size())
      return &mMaterialShadersNAT[passType][eff];
  } else if (eff < (unsigned int)mMaterialShaders[passType].size()) {
    return &mMaterialShaders[passType][eff];
  }
  return nullptr;
}

MtShaderProgram *MtShaderManager::GetPPShader(PPShader *shader) {
  auto it = mPPShaders.find(shader);
  if (it != mPPShaders.end())
    return &it->second;

  if (shader->Backend == nullptr) {
    shader->Backend = std::make_unique<MtPPShader>(fb, shader);
  }
  mPPShaders[shader] =
      static_cast<MtPPShader *>(shader->Backend.get())->mProgram;
  return &mPPShaders[shader];
}

bool MtShaderManager::CompileNextShader() {
  const char *mainvp = "shaders/glsl/main.vp";
  const char *mainfp = "shaders/glsl/main.fp";

  // Metal shader compilation is slow (GLSL->SPIRV->MSL->Lib).
  // Process more shaders per frame during startup to speed up initialization.
  uint64_t startTime = I_msTime();
  uint64_t budget = (gamestate == GS_STARTUP) ? 100 : 15;

  while (true) {
    int i = compileIndex;

    if (compileIndex == -1)
      return true;

    if (compileState == 0) {
      // regular material shaders
      MtShaderProgram prog;
      prog.vert = LoadVertShader(defaultshaders[i].ShaderName, mainvp,
                                 defaultshaders[i].Defines);
      prog.frag = LoadFragShader(
          defaultshaders[i].ShaderName, mainfp, defaultshaders[i].gettexelfunc,
          defaultshaders[i].lightfunc, defaultshaders[i].Defines, true,
          compilePass == GBUFFER_PASS);
      mMaterialShaders[compilePass].push_back(std::move(prog));

      compileIndex++;
      if (defaultshaders[compileIndex].ShaderName == nullptr) {
        compileIndex = 0;
        compileState++;
      }
    } else if (compileState == 1) {
      // NAT material shaders
      MtShaderProgram natprog;
      natprog.vert = LoadVertShader(defaultshaders[i].ShaderName, mainvp,
                                    defaultshaders[i].Defines);
      natprog.frag = LoadFragShader(
          defaultshaders[i].ShaderName, mainfp, defaultshaders[i].gettexelfunc,
          defaultshaders[i].lightfunc, defaultshaders[i].Defines, false,
          compilePass == GBUFFER_PASS);
      mMaterialShadersNAT[compilePass].push_back(std::move(natprog));

      compileIndex++;
      if (defaultshaders[compileIndex].ShaderName == nullptr) {
        compileIndex = 0;
        compileState = 2; // Move to user shaders
      }
    } else if (compileState == 2) {
      // User shaders (Mod shaders)
      if (usershaders.Size() > 0) {
        FString name = ExtractFileBase(usershaders[i].shader.GetChars());
        // Combine default defines with user defines
        FString defines = defaultshaders[usershaders[i].shaderType].Defines +
                          usershaders[i].defines;

        MtShaderProgram prog;
        prog.vert = LoadVertShader(name.GetChars(), mainvp, defines.GetChars());
        prog.frag = LoadFragShader(
            name.GetChars(), mainfp, usershaders[i].shader.GetChars(),
            defaultshaders[usershaders[i].shaderType].lightfunc,
            defines.GetChars(), true, compilePass == GBUFFER_PASS);

        mMaterialShaders[compilePass].push_back(std::move(prog));

        compileIndex++;
        if (compileIndex >= (int)usershaders.Size()) {
          compileIndex = 0;
          compileState = 3; // Move to effect shaders
        }
      } else {
        compileIndex = 0;
        compileState = 3; // Skip if no user shaders
      }
    } else if (compileState == 3) {
      // Effect shaders
      MtShaderProgram prog;
      prog.vert = LoadVertShader(effectshaders[i].ShaderName,
                                 effectshaders[i].vp, effectshaders[i].defines);
      prog.frag = LoadFragShader(effectshaders[i].ShaderName,
                                 effectshaders[i].fp1, effectshaders[i].fp2,
                                 effectshaders[i].fp3, effectshaders[i].defines,
                                 true, compilePass == GBUFFER_PASS);
      mEffectShaders[compilePass].push_back(std::move(prog));

      compileIndex++;
      if (compileIndex >= MAX_EFFECTS) {
        compileIndex = 0;
        compilePass++;
        if (compilePass == MAX_PASS_TYPES) {
          compileIndex = 0;
          compilePass = 0;
          compileState = 4; // Move to global pipeline pre-warming
        } else {
          compileState = 0; // Next pass
        }
      }
    } else if (compileState == 4) {
      // Pipeline Pre-warming phase
      // Iterate through all compiled material shaders and create common
      // pipeline variants
      if (fb->GetPipelineStateManager()) {
        auto *psm = fb->GetPipelineStateManager();

        std::vector<MtShaderProgram> *target = nullptr;
        size_t pass = compilePass;

        if (compileIndex < (int)mMaterialShaders[pass].size()) {
          target = &mMaterialShaders[pass];
        } else if (compileIndex < (int)(mMaterialShaders[pass].size() +
                                        mMaterialShadersNAT[pass].size())) {
          target = &mMaterialShadersNAT[pass];
        }

        if (target) {
          size_t idx = (target == &mMaterialShaders[pass])
                           ? compileIndex
                           : (compileIndex - mMaterialShaders[pass].size());
          MtShaderProgram &prog = (*target)[idx];

          if (prog.vert && prog.vert->isReady && prog.frag &&
              prog.frag->isReady) {
            // Pre-warm a few common keys to prime the Binary Archive and driver
            // cache
            MtPipelineKey key;
            memset(&key, 0, sizeof(key));
            key.VertexFormat = 0;
            key.PixelFormat = (uint32_t)MTL::PixelFormatRGBA16Float;
            key.DepthStencilFormat =
                (uint32_t)MTL::PixelFormatDepth32Float_Stencil8;
            key.SampleCount = 1;
            key.DrawBufferCount = 1;
            key.ColorMask = 0xF;
            key.DepthWrite = 1;
            key.DepthFunc = 1; // DF_LEqual

            // 1. Opaque
            key.BlendMode = STYLE_Normal;
            psm->GetPipelineState(key, nullptr);

            // 2. Translucent
            key.BlendMode = STYLE_Translucent;
            psm->GetPipelineState(key, nullptr);
          }

          compileIndex++;
        } else {
          // Finished this pass
          compileIndex = 0;
          compilePass++;
          if (compilePass >= MAX_PASS_TYPES) {
            compileIndex = -1;
            compileState = 5; // All done
            Printf(PRINT_LOG,
                   "Metal: Shader and Pipeline pre-warming complete.\n");
            
            if (fb->GetBinaryArchive()) {
                fb->GetBinaryArchive()->Save();
            }
            return true;
          }
        }
      } else {
        compileIndex = -1;
        compileState = 5;
        return true;
      }
    }

    // Check time budget
    if (I_msTime() - startTime > budget) {
      return false; // Yield to update loop
    }
  }
}

// Source patching helpers that FAIL LOUDLY.
//
// Every one of these patches is a literal or a regex matched against shader
// text that upstream (and this branch) edits freely. When a pattern stops
// matching, the patch silently becomes a no-op and the renderer quietly loses
// a fix. That has now happened at least three times here, and one instance --
// the ssaocombine alpha patch -- sat dead next to an AO bug for months while
// four debugging sessions looked elsewhere. A warning naming the shader and the
// patch costs nothing and turns an invisible regression into a log line.
//
// Audited 2026-08-06; every surviving patch below was re-derived against the
// current shader sources and its match count measured.
// Warn once per patch per run. These patches target main.vp, which every scene
// shader compiles against, so an unguarded warning would repeat for each of the
// ~20 stock shaders and train the reader to ignore it.
static bool WarnPatchMissedOnce(const char *what, const std::string &shadername) {
  static std::set<std::string> warned;
  if (!warned.insert(what).second)
    return false;
  Printf(PRINT_LOG,
         TEXTCOLOR_YELLOW
         "Metal: shader patch MISSED -- '%s' (first seen in %s). The pattern no "
         "longer matches; this fix is NOT being applied.\n" TEXTCOLOR_NORMAL,
         what, shadername.c_str());
  return true;
}

static bool PatchLiteral(std::string &source, const char *find,
                         const char *replace, const char *what,
                         const std::string &shadername) {
  size_t pos = source.find(find);
  if (pos == std::string::npos) {
    WarnPatchMissedOnce(what, shadername);
    return false;
  }
  source.replace(pos, strlen(find), replace);
  return true;
}

static bool PatchRegex(std::string &source, const std::regex &re,
                       const char *replace, const char *what,
                       const std::string &shadername, int expectedMatches) {
  const int found = (int)std::distance(
      std::sregex_iterator(source.begin(), source.end(), re),
      std::sregex_iterator());
  if (found == 0) {
    WarnPatchMissedOnce(what, shadername);
    return false;
  }
  if (expectedMatches > 0 && found != expectedMatches) {
    // Partial application is worse than none: half a coordinated set of edits
    // leaves the shader in a state neither the author nor the reader expects.
    Printf(PRINT_LOG,
           TEXTCOLOR_YELLOW
           "Metal: shader patch '%s' in %s matched %d times, expected %d.\n"
                   TEXTCOLOR_NORMAL,
           what, shadername.c_str(), found, expectedMatches);
  }
  source = std::regex_replace(source, re, replace);
  return true;
}

static void PatchVertexShader(std::string &source,
                              const std::string &shadername) {
  // GZDoom shaders expect OpenGL NDC (-1..1 for all axes, Y up)
  // Metal expects 0..1 for Z. 
  // We negate Y here to align Metal with OpenGL's Y-up convention natively.
  
  std::regex glPosRegex(R"(gl_Position\s*=\s*([^;]+);)");

  // Don't flip screen-space postprocess/present shaders (present/pp)
  if (shadername.find("present") != std::string::npos || shadername.find("pp/") != std::string::npos) {
      std::string patch = "gl_Position = $1;";
      source = std::regex_replace(source, glPosRegex, patch);
      return;
  }

  if (shadername.find("shadowmap") == std::string::npos) {
      // Scene: Flip Y and apply Reverse-Z [1..0]
      std::string patch =
          "gl_Position = $1;\n"
          "    gl_Position.y = -gl_Position.y;\n"
          "    gl_Position.z = 0.5 * (gl_Position.w - gl_Position.z);";
      source = std::regex_replace(source, glPosRegex, patch);
  } else {
      // Shadows: Flip Y and apply Standard-Z [0..1]
      std::string patch =
          "gl_Position = $1;\n"
          "    gl_Position.y = -gl_Position.y;\n"
          "    gl_Position.z = 0.5 * (gl_Position.w + gl_Position.z);";
      source = std::regex_replace(source, glPosRegex, patch);
  }

  // Guard against normalize(vec3(0)) which is undefined on Metal 2.0 (Intel HD 6000).
  // When bones.Normal is zero (sky dome missing normal attribute), the matrix
  // transform with w=1 picks up the translation column, producing a non-zero
  // direction instead of zero. Must guard the entire normal assignment chain
  // BEFORE the normalize stabilization patch below, which would otherwise
  // rewrite the lines before this guard can match the original patterns.
  PatchRegex(
      source,
      std::regex(
          R"(vWorldNormal\s*=\s*vec4\(normalize\(\(NormalModelMatrix\s*\*\s*vec4\(normalize\(bones\.Normal\),\s*1\.0\)\)\.xyz\),\s*1\.0\);)"),
      "vWorldNormal = length(bones.Normal) > 0.0001 ? vec4(normalize((NormalModelMatrix * vec4(normalize(bones.Normal), 1.0)).xyz), 1.0) : vec4(0.0);",
      "zero world-normal guard", shadername, 1);

  PatchRegex(
      source,
      std::regex(
          R"(vEyeNormal\s*=\s*vec4\(normalize\(\(NormalViewMatrix\s*\*\s*vec4\(normalize\(vWorldNormal\.xyz\),\s*1\.0\)\)\.xyz\),\s*1\.0\);)"),
      "vEyeNormal = length(vWorldNormal.xyz) > 0.0001 ? vec4(normalize((NormalViewMatrix * vec4(normalize(vWorldNormal.xyz), 1.0)).xyz), 1.0) : vec4(0.0);",
      "zero eye-normal guard", shadername, 1);

  // The vNormal normalize-stabilization patch that stood here is DELETED.
  // `vNormal\s*=` matches ZERO times across every shader under
  // wadsrc/static/shaders -- the variable it targeted no longer exists, and
  // vWorldNormal / vEyeNormal are covered by the two guards above. It was doing
  // nothing, and unlike those it had no warning to say so.
}

// PatchFragmentShader is GONE, deliberately -- see git history for the body.
//
// It was called only from LoadFragShader, which compiles SCENE shaders, and it
// dispatched on shadername containing "shadowmap", "main", "lineardepth" or
// "ssao". The names LoadFragShader actually receives are the fixed set in
// hw_shaderpatcher.cpp's defaultshaders/effectshaders tables -- "Default",
// "Warp 1", "Specular", "PBR", "Paletted", "Basic Fuzz", "fogboundary",
// "spheremap", "burn", "stencil", "dithertrans" and so on. NONE of them
// contains any of those four substrings, so every branch was unreachable: not
// merely mis-routed to the wrong patch function, but dead by construction for
// every stock shader.
//
// The postprocess shaders it appeared to target (shadowmap.fp, lineardepth.fp,
// ssao.fp) are compiled through MtPPShader and PatchPostprocessFragmentShader
// instead, so they never saw these edits either.
//
// Deleting rather than rerouting, on evidence:
//   - AO generation demonstrably works without them. mt_ao_probe measured mean
//     ssao.x = 0.867, i.e. real occlusion, and gl_ssao_debug 1 shows a buffer
//     correctly aligned to geometry.
//   - Three of the block's six ssao sub-patterns ALSO no longer match current
//     source (the jitter, angle and AO-return rewrites). Rerouting would have
//     applied three of six -- a half-patched shader, which is harder to
//     diagnose than an unpatched one.
//   - Its lineardepth reverse-Z rewrite would now compound with the live sky
//     patch in PatchPostprocessFragmentShader, which handles the same concern.
//   - The only way any branch could ever fire was a USER shader whose filename
//     contains one of those substrings (LoadFragShader is called with
//     ExtractFileBase of a mod's shader path). A mod shader called main.fp
//     would have had its `float bias = ...;` silently rewritten to 16.0. That
//     is a latent bug against modders, not a feature worth preserving.

static void PatchPostprocessFragmentShader(std::string &source,
                                           const std::string &shadername) {
  // Fix Metal Reverse-Z: lineardepth.fp forces sky pixels (alpha=0) to depth 1.0,
  // but with Metal's Reverse-Z params, that maps to zNear instead of zFar.
  // Change forced depth to 0.0 (Reverse-Z far plane) so sky pixels get zFar.
  if (shadername.find("lineardepth") != std::string::npos) {
    // Two matches expected: the MSAA and non-MSAA branches of the same
    // expression. One match means the shader grew or lost a branch and only
    // half the sky handling is corrected.
    std::regex skyRegex(R"(: 1\.0\);)");
    PatchRegex(source, skyRegex, ": 0.0);", "lineardepth sky far-plane",
               shadername, 2);
  }

  // Sky-dome guard: Metal may produce non-zero normals for the sky dome's
  // missing vertex attribute (GPU-dependent behavior of normalize(vec3(0))).
  // Skip AO computation on far-plane pixels by checking linear depth > 50000.
  if (shadername.find("ssao.fp") != std::string::npos &&
      shadername.find("ssaocombine") == std::string::npos) {
    // RE-DERIVED 2026-08-06. The previous pattern spelled out the whole
    // occlusion expression including `* AOStrength + (1.0 - AOStrength)`, and
    // ac0fec5db -- our own AO distance-fade commit -- rewrote that to use
    // `effectiveStrength`. The patch had matched zero times ever since and this
    // sky-dome guard was silently absent.
    //
    // Now anchored on the smallest thing that must be true: the ternary's
    // condition. It no longer cares what the strength term is called or how the
    // branches are written, so the next edit to that line does not disarm it.
    // This is the general lesson from the audit -- match the minimum, not the
    // sentence.
    // REVERTED 2026-08-06, same day it was re-derived. Re-enabling this guard
    // was a mistake and it is measured, not suspected.
    //
    // The patch had matched zero times since ac0fec5db, so "restoring" it was a
    // BEHAVIOUR CHANGE dressed up as a repair. mt_ao_probe, one run either side:
    //
    //     guard off:  mean ssao.x = 0.86671   (~13% occlusion)
    //     guard on:   mean ssao.x = 0.99924   (~0.08% occlusion)
    //
    // Nothing else touching the SSAO pass changed between those runs, so the
    // guard is what removed the occlusion.
    //
    // The MECHANISM is not understood, and that is recorded honestly rather
    // than guessed at. With the guard enabled the probe also reported ssao.y
    // saturating at 65504 (the half-float maximum) with a mean of 15627; with
    // it reverted, the same scene reports mean 64.861 and max 101.375 -- sane
    // world units for a compact interior. The guard edits only the occlusion
    // expression, which writes FragColor.x, and cannot explain a change in
    // FragColor.y. So enabling it did something beyond its apparent effect --
    // most likely the patched shader failed to translate and something
    // degenerate was used, though no compile error appeared in the log.
    //
    // An earlier reading of this took the 65504 saturation as evidence of a
    // SECOND defect in the depth scale, and inferred that ac0fec5db's distance
    // fade must be fully faded out. Both were WRONG: at the measured 65-101
    // world units the fade barely acts, and the depth chain is fine. That
    // inference was drawn from a run in which two things had changed at once.
    //
    // Do not reinstate this patch. If someone wants the sky-dome guard back,
    // establish first what enabling it actually does to the compiled shader,
    // and measure ssao.x AND ssao.y either side. The sky-dome problem it
    // targeted is separately handled by ac0fec5db's distance fade and by the
    // zero-normal guards in PatchVertexShader.
  }

  if (shadername.find("ssaocombine") != std::string::npos) {
    // NOTE: a local V-flip was applied to the AODepthTexture fetch here until
    // 2026-08-05. It was a workaround for the general Metal PP orientation
    // defect -- every postprocess pass mirrored V because the shared
    // fullscreen triangle's UVs assume OpenGL's bottom-left texture origin.
    // That is now fixed at the source, in the PP vertex shader (see MtPPShader
    // below), so this compensation would be a second flip and would misalign
    // AO against the scene. Removed deliberately; do not reinstate it without
    // first checking whether the general fix is still in place.

    // The alpha patch that lived here is REMOVED, not repaired. It rewrote
    //     FragColor = vec4(fogColor, (1.0 - attenutation) * depthMask);
    // into
    //     FragColor = vec4(vec3(0.0), clamp((1.0 - attenutation) * depthMask * 1.85, 0.0, 1.0));
    // and had matched zero times since ac0fec5db added the `ssao.y > 2.0`
    // ternary to that line.
    //
    // Deliberately not re-derived against the new text, for two reasons:
    //
    //  - Forcing RGB to vec3(0.0) discards fogColor. On this test map that is
    //    invisible -- mt_ao_probe measured SceneFog as black across the entire
    //    frame -- but in a fogged map it would make Metal's AO darken toward
    //    black where GL and Vulkan darken toward the fog colour. A silent
    //    backend divergence, against the parity goal of this branch.
    //  - The * 1.85 is an unexplained strength multiplier. Nothing records what
    //    it compensated for, and it was written while the AO composite was
    //    contributing NOTHING at all (the depth channel was zeroed by
    //    depthblur.fp until 2026-08-06), so it cannot have been tuned against a
    //    working composite. Reinstating a magic constant fitted to a broken
    //    pipeline would be the wrong default.
    //
    // If Metal's AO turns out to read weak against GL or Vulkan once the depth
    // fix is verified, that is a real comparison to make on measured output --
    // not a reason to restore this.
    (void)source;
  }
}

std::shared_ptr<MtShaderModule>
MtShaderManager::LoadVertShader(const std::string &shadername,
                                const char *vert_lump, const char *defines) {
  std::string code = "#version 450\n";
  code += "#extension GL_GOOGLE_include_directive : enable\n";

  std::string definesStr = defines;

  // Strip VULKAN_COORDINATE_SYSTEM if present to avoid conflict with our manual
  // patch
  size_t vpos = definesStr.find("#define VULKAN_COORDINATE_SYSTEM");
  if (vpos != std::string::npos) {
    definesStr.erase(vpos, strlen("#define VULKAN_COORDINATE_SYSTEM"));
  }

  code += definesStr;
  code += "\n#define MAX_STREAM_DATA " + std::to_string(MAX_STREAM_DATA) + "\n";
  code += shaderBindings;
  code += "\n#line 1\n";
  std::string source = LoadPrivateShaderLump(vert_lump);

  PatchVertexShader(source, shadername);

  code += source;

  return CompileShader(shadername + "_vert", code, "", {});
}

std::shared_ptr<MtShaderModule> MtShaderManager::LoadFragShader(
    const std::string &shadername, const char *frag_lump,
    const char *material_lump, const char *light_lump, const char *defines,
    bool alphatest, bool gbufferpass) {
  std::string code = "#version 450\n";
  code += "#extension GL_GOOGLE_include_directive : enable\n";
  std::string definesStr = defines;
  size_t vpos = definesStr.find("#define VULKAN_COORDINATE_SYSTEM");
  if (vpos != std::string::npos) {
    definesStr.erase(vpos, strlen("#define VULKAN_COORDINATE_SYSTEM"));
  }
  code += definesStr;
  code += "\n#define MAX_STREAM_DATA " + std::to_string(MAX_STREAM_DATA) + "\n";
  code += shaderBindings;

  if (!alphatest)
    code += "#define NO_ALPHATEST\n";
  if (gbufferpass)
    code += "#define GBUFFER_PASS\n";

  code += "\n#line 1\n";
  std::string fragSource = LoadPrivateShaderLump(frag_lump);
  code += fragSource;

  if (material_lump) {
    if (material_lump[0] != '#') {
      std::string pp_code = LoadPublicShaderLump(material_lump);

      if (pp_code.find("ProcessMaterial") == std::string::npos &&
          pp_code.find("SetupMaterial") == std::string::npos) {
        if (pp_code.find("GetTexCoord") != std::string::npos) {
          code += "\n" +
                  LoadPrivateShaderLump("shaders/glsl/func_defaultmat2.fp") +
                  "\n";
        } else {
          code += "\n" +
                  LoadPrivateShaderLump("shaders/glsl/func_defaultmat.fp") +
                  "\n";
        }

        if (pp_code.find("ProcessLight") != std::string::npos) {
          code += "\nvec4 ProcessLight(vec4 color);\n";
          code += "\nvec4 ProcessLight(Material material, vec4 color) { return "
                  "ProcessLight(color); }\n";
        }
      }

      code += "\n#line 1\n";
      code += pp_code;

      if (pp_code.find("ProcessLight") == std::string::npos) {
        code += "\n" +
                LoadPrivateShaderLump("shaders/glsl/func_defaultlight.fp") +
                "\n";
      }
    } else {
      code += (material_lump + 1);
    }
  }

  if (light_lump) {
    code += "\n#line 1\n";
    code += LoadPrivateShaderLump(light_lump);
  }

  return CompileShader(shadername + "_frag", "", code, {});
}

std::string MtShaderManager::LoadPublicShaderLump(const char *lumpname) {
  int lump = fileSystem.CheckNumForFullName(lumpname, 0);
  if (lump == -1)
    lump = fileSystem.CheckNumForFullName(lumpname);
  if (lump == -1) {
    if (mt_debug)
      Printf(PRINT_LOG, "Metal: WARNING - Public shader lump not found: %s\n",
             lumpname);
    return "";
  }
  auto data = fileSystem.ReadFile(lump);
  return std::string((const char *)data.data(), data.size());
}

std::string MtShaderManager::LoadPrivateShaderLump(const char *lumpname) {
  // Unrestricted lookup, matching Vulkan (vk_ppshader.cpp:74). This used to
  // pass the `int wadfile` overload with 0, which searches ONLY the engine's
  // own gzdoom.pk3 -- so a shader lump belonging to a MOD was never found, and
  // every custom postprocess shader got an empty fragment source. The name is
  // now a misnomer; the lookup is no more "private" than the reference's.
  //
  // Restricting it to file 0 would also be a Metal-only behaviour difference:
  // on GL and Vulkan a mod CAN override a stock shader lump, and the whole
  // point of this audit is to not have quiet per-backend divergences.
  int lump = fileSystem.CheckNumForFullName(lumpname);
  if (lump == -1) {
    // Deliberately PRINT_HIGH and not gated on mt_debug. An empty source here
    // always becomes a compile failure downstream, and this exact message
    // being invisible is what hid the broken custom-shader path.
    Printf(PRINT_HIGH, TEXTCOLOR_RED
           "Metal: shader lump not found: %s -- this pass will not run.\n"
           TEXTCOLOR_NORMAL, lumpname);
    return "";
  }
  auto data = fileSystem.ReadFile(lump);
  return std::string((const char *)data.data(), data.size());
}

void MtShaderManager::ClearCache() {
  for (auto &pair : mShaderCache) {
    auto &module = pair.second;
    if (module) {
      if (module->function)
        module->function->release();
      if (module->fragmentFunction)
        module->fragmentFunction->release();
      if (module->library)
        module->library->release();
      if (module->fragmentLibrary)
        module->fragmentLibrary->release();
    }
  }
  mShaderCache.clear();
  mPPShaders.clear();
}

MTL::Library *MtShaderManager::LoadNativeLibrary() {
  if (mNativeLibrary)
    return mNativeLibrary;

  FString base = progdir;
  if (base.IsNotEmpty() && base.Back() != '/')
    base += "/";

  std::vector<std::string> candidates;
  if (base.IsNotEmpty()) {
    candidates.emplace_back(std::string(base.GetChars()) + "../Resources/native_shaders.metallib");
    candidates.emplace_back(std::string(base.GetChars()) + "native_shaders.metallib");
  }
  candidates.emplace_back("native_shaders.metallib");

  for (const auto &path : candidates) {
    std::ifstream test(path, std::ios::binary);
    if (!test.good())
      continue;

    NS::String *libraryPath =
        NS::String::string(path.c_str(), NS::UTF8StringEncoding);
    NS::Error *error = nullptr;
    mNativeLibrary = fb->device->device->newLibrary(libraryPath, &error);
    if (mNativeLibrary) {
      if (mt_debug)
        Printf(PRINT_LOG, "Metal: Loaded native shader library: %s\n",
               path.c_str());
      return mNativeLibrary;
    }

    if (error) {
      Printf(PRINT_LOG, "Metal: Failed to load native shader library %s: %s\n",
             path.c_str(), error->localizedDescription()->utf8String());
      error->release();
    }
  }

  if (mt_debug)
    Printf(PRINT_LOG,
           "Metal: native_shaders.metallib not found; using runtime shader compilation fallback.\n");
  return nullptr;
}

MTL::ComputePipelineState *
MtShaderManager::CreateComputePipeline(const char *functionName,
                                       const char *fallbackSource,
                                       const char *debugName) {
  auto deviceObj = fb->device->device;

  auto createFromLibrary = [&](MTL::Library *library) -> MTL::ComputePipelineState * {
    if (!library)
      return nullptr;

    auto function =
        library->newFunction(NS::String::string(functionName, NS::UTF8StringEncoding));
    if (!function)
      return nullptr;

    auto desc = MTL::ComputePipelineDescriptor::alloc()->init();
    desc->setComputeFunction(function);

    // Use Binary Archive for caching if available
    auto archive = fb->GetBinaryArchive() ? fb->GetBinaryArchive()->GetArchive() : nullptr;
    if (archive) {
        auto archives = NS::Array::array((NS::Object* const *)&archive, 1);
        desc->setBinaryArchives(archives);
    }

    NS::Error *error = nullptr;
    auto pso = deviceObj->newComputePipelineState(desc, MTL::PipelineOptionNone, nullptr, &error);
    
    if (!pso && error) {
      Printf(PRINT_LOG, "Metal: Failed to create compute pipeline %s: %s\n",
             debugName, error->localizedDescription()->utf8String());
    } else if (pso) {
        // Add to archive for persistence
        if (fb->GetBinaryArchive()) {
            fb->GetBinaryArchive()->AddComputePipeline(desc);
        }
    }

    desc->release();
    function->release();
    return pso;
  };

  if (auto pso = createFromLibrary(LoadNativeLibrary()))
    return pso;

  if (!fallbackSource)
    return nullptr;

  auto sourceString =
      NS::String::string(fallbackSource, NS::UTF8StringEncoding);
  auto compileOptions = MTL::CompileOptions::alloc()->init();
  compileOptions->setLanguageVersion(MTL::LanguageVersion2_0);

  NS::Error *error = nullptr;
  auto library = deviceObj->newLibrary(sourceString, compileOptions, &error);
  compileOptions->release();
  if (!library) {
    if (error) {
      Printf(PRINT_LOG, "Metal: %s shader compilation failed: %s\n", debugName,
             error->localizedDescription()->utf8String());
      error->release();
    }
    return nullptr;
  }

  auto pso = createFromLibrary(library);
  library->release();
  return pso;
}

std::string MtShaderManager::GetCachePath(const std::string &key) {
  FString path = M_GetCachePath(true);
  path += "/mt_";
  // Filter key for filesystem safety
  for (char c : key) {
    if (isalnum(c) || c == '_' || c == '-')
      path += c;
  }
  path += ".msl";
  return path.GetChars();
}

std::vector<uint32_t>
MtShaderManager::CompileGLSLToSPIRV(const std::string &source,
                                    const std::string &name, bool isVertex,
                                    const std::vector<std::string> &defines) {
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
    // Remove lines that explicitly include metal_stdlib or use namespace metal
    // as glslang cannot process them
    if (line.find("#include <metal_stdlib>") == std::string::npos &&
        line.find("using namespace metal;") == std::string::npos) {
      processedSource += line + "\n";
    }
  }

  std::string finalSource = processedSource;
  const char *sourceStr = finalSource.c_str();
  int sourceLength = static_cast<int>(finalSource.length());
  const char *nameStr = name.c_str();

  // Create glslang shader
  TBuiltInResource resources = GetDefaultTBuiltInResource();
  glslang::TShader shader(stage);
  shader.setStringsWithLengthsAndNames(&sourceStr, &sourceLength, &nameStr, 1);

  // Configure for Metal/MSL target (similar to Vulkan)
  shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan,
                     100);
  shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
  shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

  // Parse shader
  // EShMsgVulkanRules helps enforce Vulkan GLSL best practices for SPIR-V
  // output.
  bool parseSuccess = shader.parse(&resources, 450, false,
                                   EShMsgVulkanRules); // Use EShMsgVulkanRules
  if (!parseSuccess) {
    // PRINT_HIGH, not PRINT_LOG. PRINT_LOG never reaches stdout, so a redirected
    // launch log showed nothing at all while every custom postprocess shader
    // silently failed to compile. A shader that does not build is a failure the
    // operator needs to see, not a debug detail.
    Printf(PRINT_HIGH, TEXTCOLOR_RED "Metal: shader parse FAILED for %s:\n%s\n"
           TEXTCOLOR_NORMAL, name.c_str(), shader.getInfoLog());
    return std::vector<uint32_t>();
  }

  // Link into program
  glslang::TProgram program;
  program.addShader(&shader);
  bool linkSuccess = program.link(EShMsgDefault);
  if (!linkSuccess) {
    Printf(PRINT_HIGH, TEXTCOLOR_RED "Metal: shader link FAILED for %s:\n%s\n"
           TEXTCOLOR_NORMAL, name.c_str(), program.getInfoLog());
    return std::vector<uint32_t>();
  }

  // Get intermediate representation
  glslang::TIntermediate *intermediate = program.getIntermediate(stage);
  if (!intermediate) {
    Printf(PRINT_LOG,
           "Metal: Failed to get intermediate representation for %s\n",
           name.c_str());
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
  if (!messages.empty()) {
    Printf(PRINT_LOG, "Metal: SPIR-V generation messages for %s:\n%s\n",
           name.c_str(), messages.c_str());
  }

  return spirv;
}

std::string
MtShaderManager::TranslateSPIRVToMSL(const std::vector<uint32_t> &spirv,
                                     bool isVertex,
                                     const std::string &name) { // Added name
  if (spirv.empty())
    return "";

  // Create SPIR-V translator
  ShaderTranslator::SPIRVTranslator translator;

  // Translate SPIR-V to MSL (Metal 2.0 for macOS 10.13+)
  auto result = translator.TranslateToMSL(spirv, 20);

  if (!result.success) {
    Printf(PRINT_LOG, "Metal SPIR-V translation error: %s\n",
           result.errorLog.c_str());
    return "";
  }

  // FORCE ALPHA 1.0 for Present pass to avoid transparency issues on some
  // hardware
  if (name.find("present") != std::string::npos && !isVertex) {
    size_t fpos = result.source.find("out.FragColor = ");
    if (fpos != std::string::npos) {
      size_t epos = result.source.find(";", fpos);
      if (epos != std::string::npos) {
        std::string expr = result.source.substr(fpos + 16, epos - (fpos + 16));
        result.source.replace(fpos, epos - fpos + 1,
                              "out.FragColor = float4((" + expr +
                                  ").xyz, 1.0);");
      }
    }
  }

  return result.source;
}

MTL::Library *MtShaderManager::CompileMSLToLibrary(const std::string &msl,
                                                   const std::string &name) {
  if (msl.empty())
    return nullptr;

  NS::String *source = NS::String::string(msl.c_str(), NS::UTF8StringEncoding);
  NS::Error *error = nullptr;

  MTL::Library *library =
      fb->device->device->newLibrary(source, nullptr, &error);

  if (!library) { // Always log if library creation failed
    if (error) {
      const char *errorMsg = error->localizedDescription()->utf8String();
      Printf(PRINT_LOG,
             "Metal: MSL to MTLLibrary compilation FAILED for %s:\n%s\n",
             name.c_str(), errorMsg);
      error->release(); // Release error object
    } else {
      Printf(PRINT_LOG,
             "Metal: MSL to MTLLibrary compilation FAILED for %s: Unknown "
             "error (NS::Error was null).\n",
             name.c_str());
    }
  } else if (error) { // Log warnings if library created but error present
    const char *errorMsg = error->localizedDescription()->utf8String();
    Printf(PRINT_LOG,
           "Metal: MSL to MTLLibrary compilation WARNING for %s:\n%s\n",
           name.c_str(), errorMsg);
    error->release(); // Release error object
  }

  return library;
}

MtPPShader::MtPPShader(MetalRenderDevice *fb, PPShader *shader) : fb(fb) {
  FString prolog;
  if (shader->Uniforms.size() > 0) {
    prolog = UniformBlockDecl::Create("Uniforms", shader->Uniforms, -1);
  }
  prolog += shader->Defines;

  // Compile vertex shader.
  //
  // The prolog is deliberately NOT prepended here. Vulkan passes "" as the
  // vertex stage's prolog and the real one only to the fragment stage
  // (vk_ppshader.cpp:39 vs :48), and Metal must match: for a CUSTOM
  // postprocess shader the prolog carries the sampler and in/out declarations
  // that PPCustomShaderInstance builds, and injecting
  // `layout(location=0) in vec2 TexCoord;` into screenquad.vp collides with its
  // own `layout(location = 0) in vec4 PositionInProjection`, which glslang
  // rejects as "overlapping use of location 0".
  //
  // This never showed on the stock PP shaders because their prolog is only a
  // uniform block plus defines, and an unused uniform block in the vertex stage
  // is harmless. It is fatal exactly and only on the custom path.
  std::string vertCode = "#version 450\n";
  vertCode += "#extension GL_GOOGLE_include_directive : enable\n";
  vertCode += "\n#line 1\n";

  std::string vertSource = fb->GetShaderManager()->LoadPrivateShaderLump(
      shader->VertexShader.GetChars());
  PatchVertexShader(vertSource, shader->VertexShader.GetChars());

  // Make each postprocess pass orientation-preserving on Metal.
  //
  // The shared fullscreen triangle (flatvertices.cpp:66-68) is
  //   (-1,-1) UV(0,0)   (3,-1) UV(2,0)   (-1,3) UV(0,2)
  // so UV.y increases with NDC.y. That is correct only under OpenGL's
  // bottom-left texture origin. Metal's texture origin is top-left, so UV.y=0
  // is the TOP of the source while NDC.y=-1 is the BOTTOM of the target, and
  // every pass that samples a texture and writes it out mirrors V.
  //
  // The chain used to survive this by accident, because the flips cancelled:
  // BlitSceneToPostprocess flipped once and the final Present flipped again,
  // so an EVEN pass count came out upright. Any effect adding an ODD number of
  // passes inverted the whole frame. MEASURED 2026-08-05: gl_lens 1 (one draw)
  // renders upside down; gl_fxaa 1 (two draws) renders upright; gl_tonemap
  // 1-5 (one draw) all render upside down, including mode 4, which is a
  // mathematical identity -- proving the defect is in the plumbing and not in
  // any shader's maths.
  //
  // The present* shaders are deliberately excluded. They are the only ones
  // that apply their own UVScale/UVOffset (present.fp:56), which is how both
  // the scene-viewport mapping and the final orientation correction are
  // expressed, and they are the two call sites that are correct today. Leaving
  // them untouched keeps every existing uniform value valid and confines this
  // change to the passes that are actually broken.
  //
  // Only possible because b017e7c92 keys the shader cache on source rather
  // than name: this produces two variants of screenquad.vp, which would
  // previously have collided on a single cache entry.
  if (strstr(shader->FragmentShader.GetChars(), "present") == nullptr) {
    const char *kTexCoordAssign = "TexCoord = UV;";
    size_t pos = vertSource.find(kTexCoordAssign);
    if (pos != std::string::npos) {
      vertSource.replace(pos, strlen(kTexCoordAssign),
                         "TexCoord = vec2(UV.x, 1.0 - UV.y);");
    } else {
      Printf(PRINT_LOG,
             "Metal: PP V-flip patch did not match in %s (vertex shader %s) -- "
             "this pass will render inverted.\n",
             shader->FragmentShader.GetChars(),
             shader->VertexShader.GetChars());
    }
  }

  vertCode += vertSource;

  // Compile fragment shader
  std::string fragCode = "#version 450\n";
  fragCode += "#extension GL_GOOGLE_include_directive : enable\n";
  fragCode += prolog.GetChars();

  // Add standard PP samplers if not already there
  // hw_postprocess shaders expect 'InputTexture' at binding 0, etc.
  // We'll let the GZDoom GLSL shaders handle their own bindings but we can add
  // helpers.
  fragCode += "\n#line 1\n";
  std::string fragSource = fb->GetShaderManager()->LoadPrivateShaderLump(
      shader->FragmentShader.GetChars());
  PatchPostprocessFragmentShader(fragSource, shader->FragmentShader.GetChars());
  fragCode += fragSource;

  mProgram.vert = fb->GetShaderManager()->CompileShader(
      shader->VertexShader.GetChars(), vertCode, "", {});
  mProgram.frag = fb->GetShaderManager()->CompileShader(
      shader->FragmentShader.GetChars(), "", fragCode, {});
}

MtPPShader::~MtPPShader() {
  // Map entry will be cleared when it's re-requested and found null,
  // or we could explicitly remove it here if we pass the manager
}
