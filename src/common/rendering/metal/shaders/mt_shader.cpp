#include "i_time.h"
#define TimeScale TimeScale_GZDOOM
#include <Metal/Metal.hpp>
#undef TimeScale

#include "cmdlib.h"
#include "engineerrors.h"
#include "filesystem.h"
#include "hwrenderer/data/hw_shaderpatcher.h"
#include "i_specialpaths.h"
#include "metal/system/mt_renderdevice.h"
#include "mt_shader.h"
#include "printf.h"
#include <fstream>
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
		int uTextureMode;
		float uAlphaThreshold;
		vec2 uClipSplit;

		// Lighting + Fog
		float uLightLevel;
		float uFogDensity;
		float uLightFactor;
		float uLightDist;
		int uFogEnabled;

		// dynamic lights
		int uLightIndex;

		// Blinn glossiness and specular level
		vec2 uSpecularMaterial;

		// bone animation
		int uBoneIndexBase;

		int uDataIndex;
		int padding2, padding3;
	};

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
	//#define HAS_UNIFORM_VERTEX_DATA

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
  // Finalize glslang
  glslang::FinalizeProcess();
}


std::shared_ptr<MtShaderModule>
MtShaderManager::CompileShader(const std::string &name,
                               const std::string &vertexSource,
                               const std::string &fragmentSource,
                               const std::vector<std::string> &defines) {
  // Check cache first
  auto it = mShaderCache.find(name);
  if (it != mShaderCache.end())
    return it->second;

  // Create shader module
  auto module = std::make_shared<MtShaderModule>();
  module->name = name;

  // Compile vertex shader: GLSL → SPIR-V → MSL → MTLLibrary
  if (!vertexSource.empty()) {
    std::string cacheKey = name + "_vert";
    for (auto &d : defines)
      cacheKey += d;
    std::string cachePath = GetCachePath(cacheKey);
    std::string vertexMSL;

    std::ifstream vfile(cachePath);
    if (vfile.is_open()) {
      if (mt_debug) Printf("Metal: Loading vertex shader %s from cache.\n", name.c_str());
      std::stringstream ss;
      ss << vfile.rdbuf();
      vertexMSL = ss.str();
    } else {
      if (mt_debug) Printf("Metal: Recompiling vertex shader %s from scratch.\n", name.c_str());
      // Step 1: GLSL → SPIR-V
      auto vertexSPIRV =
          CompileGLSLToSPIRV(vertexSource, name + "_vert", true, defines);
      if (vertexSPIRV.empty()) {
        return nullptr;
      }

      // Step 2: SPIR-V → MSL
      vertexMSL = TranslateSPIRVToMSL(vertexSPIRV, true, name + "_vert"); // Pass name
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
      Printf("Metal: Failed to compile vertex shader MSL to library: %s\n",
             name.c_str());
      return nullptr;
    }

    // Get main function
    NS::String *funcName = NS::String::string("main0", NS::UTF8StringEncoding);
    module->function = module->library->newFunction(funcName);
    funcName->release();

    if (!module->function) {
      Printf("Metal: Failed to find main0 function in vertex shader: %s\n",
             name.c_str());
      module->library->release();
      return nullptr;
    }

    module->entryPoint = "main0";
  }

  // Compile fragment shader: GLSL → SPIR-V → MSL → MTLLibrary
  if (!fragmentSource.empty()) {
    std::string cacheKey = name + "_frag";
    for (auto &d : defines)
      cacheKey += d;
    std::string cachePath = GetCachePath(cacheKey);
    std::string fragmentMSL;

    std::ifstream ffile(cachePath);
    if (ffile.is_open()) {
      if (mt_debug) Printf("Metal: Loading fragment shader %s from cache.\n", name.c_str());
      std::stringstream ss;
      ss << ffile.rdbuf();
      fragmentMSL = ss.str();
    } else {
      if (mt_debug) Printf("Metal: Recompiling fragment shader %s from scratch.\n", name.c_str());
      // Step 1: GLSL → SPIR-V
      if (mt_debug && fragmentSource.find("present") != std::string::npos) {
          Printf("Metal: Compiling fragment shader %s. Source preview: %.256s\n", name.c_str(), fragmentSource.c_str());
      }
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
      fragmentMSL = TranslateSPIRVToMSL(fragmentSPIRV, false, name + "_frag"); // Pass name
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
      Printf("Metal: Failed to compile fragment shader MSL to library: %s\n",
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
      Printf("Metal: Failed to find main0 function in fragment shader: %s\n",
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
  mShaderCache[name] = module;

  return module;
}

MtShaderProgram *MtShaderManager::GetEffect(int effect, EPassType passType) {
  if (mt_debug) Printf("Metal: MtShaderManager::GetEffect %d pass=%d\n", effect, (int)passType);
  if (compileIndex == -1 && effect >= 0 && effect < MAX_EFFECTS &&
      mEffectShaders[passType][effect].frag) {
    return &mEffectShaders[passType][effect];
  }
  return nullptr;
}

MtShaderProgram *MtShaderManager::Get(unsigned int eff, bool alphateston,
                                      EPassType passType) {
  if (mt_debug) Printf("Metal: MtShaderManager::Get %u alpha=%d pass=%d\n", eff, (int)alphateston, (int)passType);
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
    if (compileIndex == 4) // SHADER_NoTexture
    {
      compileIndex = 0;
      compileState = 3; // Skip user shaders for now
    }
  } else if (compileState == 3) {
    // Effect shaders
    MtShaderProgram prog;
    prog.vert = LoadVertShader(effectshaders[i].ShaderName, effectshaders[i].vp,
                               effectshaders[i].defines);
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
        compileIndex = -1; // we're done.
        return true;
      }
      compileState = 0;
    }
  }
  return false; // Not done yet
}

std::shared_ptr<MtShaderModule>
MtShaderManager::LoadVertShader(const std::string &shadername,
                                const char *vert_lump, const char *defines) {
  std::string code = "#version 450\n";
  code += "#extension GL_GOOGLE_include_directive : enable\n";
  
  std::string definesStr = defines;
  size_t vpos = definesStr.find("#define VULKAN_COORDINATE_SYSTEM");
  if (vpos != std::string::npos) {
      if (mt_debug) Printf("Metal: Stripping VULKAN_COORDINATE_SYSTEM from %s to use manual Metal patch.\n", shadername.c_str());
      definesStr.replace(vpos, strlen("#define VULKAN_COORDINATE_SYSTEM"), "//efine VULKAN_COORDINATE_SYSTEM");
  }
  
  code += definesStr;
  code += "\n#define MAX_STREAM_DATA " + std::to_string(MAX_STREAM_DATA) + "\n";
  code += shaderBindings;
  code += "\n#line 1\n";
  std::string source = LoadPrivateShaderLump(vert_lump);

  // Patch source to handle Metal coordinate system (Y-flip and Z range 0..1)
  // GZDoom shaders expect OpenGL NDC (-1..1 for all axes, Y up)
  // Metal expects 0..1 for Z, and we want to Y-flip 3D only to match GZDoom's RenderTextureIsFlipped(true)
  size_t pos = source.find("gl_Position = ProjectionMatrix * eyeCoordPos;");
  if (pos != std::string::npos) {
      if (mt_debug) Printf("Metal: Patching vertex shader %s for coordinate system.\n", shadername.c_str());
      source.replace(pos, strlen("gl_Position = ProjectionMatrix * eyeCoordPos;"),
          "gl_Position = ProjectionMatrix * eyeCoordPos;\n"
          "if (ProjectionMatrix[3][3] < 0.5) gl_Position.y = -gl_Position.y;\n" // Flip Y for 3D (perspective) only
          "gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n" // Map -1..1 to 0..1
      );
  } else {
      if (mt_debug) Printf("Metal: WARNING - Failed to patch vertex shader %s! Coordinate system may be wrong.\n", shadername.c_str());
  }

  code += source;

  return CompileShader(shadername + "_vert", code, "", {});
}

std::shared_ptr<MtShaderModule> MtShaderManager::LoadFragShader(
    const std::string &shadername, const char *frag_lump,
    const char *material_lump, const char *light_lump, const char *defines,
    bool alphatest, bool gbufferpass) {
  std::string code = "#version 450\n";
  code += "#extension GL_GOOGLE_include_directive : enable\n";
  code += defines;
  code += "\n#define MAX_STREAM_DATA " + std::to_string(MAX_STREAM_DATA) + "\n";
  code += shaderBindings;

  if (!alphatest)
    code += "#define NO_ALPHATEST\n";
  if (gbufferpass)
    code += "#define GBUFFER_PASS\n";

  code += "\n#line 1\n";
  code += LoadPrivateShaderLump(frag_lump);

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
      if (mt_debug) Printf("Metal: WARNING - Public shader lump not found: %s\n", lumpname);
      return "";
  }
  auto data = fileSystem.ReadFile(lump);
  return std::string((const char *)data.data(), data.size());
}

std::string MtShaderManager::LoadPrivateShaderLump(const char *lumpname) {
  int lump = fileSystem.CheckNumForFullName(lumpname, 0);
  if (lump == -1) {
      if (mt_debug) Printf("Metal: WARNING - Private shader lump not found: %s\n", lumpname);
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

std::string MtShaderManager::GetCachePath(const std::string &key) {
  FString path = M_GetCachePath(true);
  path += "/metal_sh_v8_";
  if (mt_debug) Printf("Metal: Cache path for key %s: %s\n", key.c_str(), path.GetChars());
  // Simple alphanumeric key
  for (char c : key) {
    if (isalnum(c) || c == '_')
      path += c;
    else {
      char hex[8];
      snprintf(hex, sizeof(hex), "_%02x", (unsigned char)c);
      path += hex;
    }
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

  if (mt_debug) {
      Printf("Metal: Compiling GLSL shader %s. Source length: %d\n", name.c_str(), sourceLength);
  }

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
    Printf("Metal: Shader parse failed for %s:\n%s\n", name.c_str(),
           shader.getInfoLog());
    return std::vector<uint32_t>();
  }

  // Link into program
  glslang::TProgram program;
  program.addShader(&shader);
  bool linkSuccess = program.link(EShMsgDefault);
  if (!linkSuccess) {
    Printf("Metal: Shader link failed for %s:\n%s\n", name.c_str(),
           program.getInfoLog());
    return std::vector<uint32_t>();
  }

  // Get intermediate representation
  glslang::TIntermediate *intermediate = program.getIntermediate(stage);
  if (!intermediate) {
    Printf("Metal: Failed to get intermediate representation for %s\n",
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
    Printf("Metal: SPIR-V generation messages for %s:\n%s\n", name.c_str(),
           messages.c_str());
  }

  return spirv;
}

std::string
MtShaderManager::TranslateSPIRVToMSL(const std::vector<uint32_t> &spirv,
                                     bool isVertex, const std::string &name) { // Added name
  if (spirv.empty())
    return "";

  // Create SPIR-V translator
  ShaderTranslator::SPIRVTranslator translator;

  // Translate SPIR-V to MSL (Metal 2.0 for macOS 10.13+)
  auto result = translator.TranslateToMSL(spirv, 20);

  if (!result.success) {
    Printf("Metal SPIR-V translation error: %s\n", result.errorLog.c_str());
    return "";
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
      Printf("Metal: MSL to MTLLibrary compilation FAILED for %s:\n%s\n", name.c_str(), errorMsg);
      error->release(); // Release error object
    } else {
      Printf("Metal: MSL to MTLLibrary compilation FAILED for %s: Unknown error (NS::Error was null).\n", name.c_str());
    }
  } else if (error) { // Log warnings if library created but error present
      const char *errorMsg = error->localizedDescription()->utf8String();
      Printf("Metal: MSL to MTLLibrary compilation WARNING for %s:\n%s\n", name.c_str(), errorMsg);
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

  // Compile vertex shader
  std::string vertCode = "#version 450\n";
  vertCode += "#extension GL_GOOGLE_include_directive : enable\n";
  // For PP shaders, we don't necessarily need all the scene bindings,
  // but we do need the prolog.
  vertCode += prolog.GetChars();
  vertCode += "\n#line 1\n";
  vertCode += fb->GetShaderManager()->LoadPrivateShaderLump(
      shader->VertexShader.GetChars());

  // Compile fragment shader
  std::string fragCode = "#version 450\n";
  fragCode += "#extension GL_GOOGLE_include_directive : enable\n";
  fragCode += prolog.GetChars();

  // Add standard PP samplers if not already there
  // hw_postprocess shaders expect 'InputTexture' at binding 0, etc.
  // We'll let the GZDoom GLSL shaders handle their own bindings but we can add
  // helpers.
  fragCode += "\n#line 1\n";
  fragCode += fb->GetShaderManager()->LoadPrivateShaderLump(
      shader->FragmentShader.GetChars());

  mProgram.vert = fb->GetShaderManager()->CompileShader(
      shader->VertexShader.GetChars(), vertCode, "", {});
  mProgram.frag = fb->GetShaderManager()->CompileShader(
      shader->FragmentShader.GetChars(), "", fragCode, {});
}

MtPPShader::~MtPPShader() {
  // Map entry will be cleared when it's re-requested and found null,
  // or we could explicitly remove it here if we pass the manager
}
