#pragma once

#include "hwrenderer/data/hw_renderstate.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "matrix.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace MTL {
class Library;
class Function;
} // namespace MTL

class MetalRenderDevice;

// Shader uniform structures (matching Vulkan)
struct MatricesUBO {
  VSMatrix ModelMatrix;
  VSMatrix NormalModelMatrix;
  VSMatrix TextureMatrix;
};

// Push constants structure (small frequently-updated uniforms)
//
// Terminology note:
// - Vulkan: "Push Constants" (vkCmdPushConstants)
// - Metal: "Inline Buffers" via setVertexBytes/setFragmentBytes
// - NOT to be confused with Metal "Function Constants" which are compile-time
// specialization
//
// Metal implementation: passed via setVertexBytes/setFragmentBytes (< 4KB
// inline data)
struct PushConstants {
  float group1[4]; // 0: uClipSplit.x, 1: uClipSplit.y, 2: uSpecularMaterial.x,
                   // 3: uSpecularMaterial.y
  float group2[4]; // 0: uLightLevel, 1: uFogDensity, 2: uLightFactor, 3:
                   // uLightDist
  float group3[4]; // 0: uTextureMode (int), 1: uAlphaThreshold, 2: uFogEnabled
                   // (int), 3: uLightIndex (int)
  float group4[4]; // 0: uBoneIndexBase (int), 1: uDataIndex (int), 2: padding,
                   // 3: padding

  bool operator==(const PushConstants &other) const {
    return memcmp(this, &other, sizeof(PushConstants)) == 0;
  }
  bool operator!=(const PushConstants &other) const {
    return !(*this == other);
  }
};

#define MAX_STREAM_DATA ((int)(65536 / sizeof(StreamData)))

struct StreamUBO {
  StreamData data[MAX_STREAM_DATA];
};

// Shader compilation result
struct MtShaderModule {
  MTL::Library *library = nullptr;
  MTL::Function *function = nullptr;
  MTL::Function *fragmentFunction = nullptr;
  MTL::Library *fragmentLibrary = nullptr;
  std::string name;
  std::string entryPoint;

  bool isCompiling = false;
  bool isReady = false;
  uint64_t compileStartTime = 0;
};

class MtShaderProgram {
public:
  std::shared_ptr<MtShaderModule> vert;
  std::shared_ptr<MtShaderModule> frag;
};

// Shader manager - handles GLSL → SPIR-V → MSL compilation
class MtShaderManager {
public:
  MtShaderManager(class MetalRenderDevice *fb);
  ~MtShaderManager();

  // Compile GLSL to Metal (via SPIR-V and MSL)
  // Pipeline: GLSL → glslang → SPIR-V → shader-translator → MSL → MTLLibrary
  std::shared_ptr<MtShaderModule>
  CompileShader(const std::string &name, const std::string &vertexSource,
                const std::string &fragmentSource,
                const std::vector<std::string> &defines);

  // Get compiled shader
  MtShaderProgram *GetEffect(int effect, EPassType passType);
  MtShaderProgram *Get(unsigned int eff, bool alphateston, EPassType passType);

  // Compile next shader in queue (for incremental compilation)
  bool CompileNextShader();

  // PP Shader support
  MtShaderProgram *GetPPShader(PPShader *shader);

  // Clear cache
  void ClearCache();

  std::string LoadPublicShaderLump(const char *lumpname);
  std::string LoadPrivateShaderLump(const char *lumpname);

private:
  std::string GetCachePath(const std::string &key);
  // Compile GLSL to SPIR-V (reuse Vulkan logic)
  std::vector<uint32_t>
  CompileGLSLToSPIRV(const std::string &source, const std::string &name,
                     bool isVertex, const std::vector<std::string> &defines);

  // Translate SPIR-V to MSL (using shader-translator)
  std::string TranslateSPIRVToMSL(const std::vector<uint32_t> &spirv,
                                  bool isVertex,
                                  const std::string &name); // Added name

  // Compile MSL to MTLLibrary
  MTL::Library *CompileMSLToLibrary(const std::string &msl,
                                    const std::string &name);

  std::shared_ptr<MtShaderModule> LoadVertShader(const std::string &shadername,
                                                 const char *vert_lump,
                                                 const char *defines);
  std::shared_ptr<MtShaderModule>
  LoadFragShader(const std::string &shadername, const char *frag_lump,
                 const char *material_lump, const char *light_lump,
                 const char *defines, bool alphatest, bool gbufferpass);

  MetalRenderDevice *fb = nullptr;

private:
  std::unordered_map<std::string, std::shared_ptr<MtShaderModule>> mShaderCache;

  std::vector<MtShaderProgram> mMaterialShaders[MAX_PASS_TYPES];
  std::vector<MtShaderProgram> mMaterialShadersNAT[MAX_PASS_TYPES];
  std::vector<MtShaderProgram> mEffectShaders[MAX_PASS_TYPES];

  uint8_t compilePass = 0, compileState = 0;
  int compileIndex = 0;
  std::unordered_map<PPShader *, MtShaderProgram> mPPShaders;
};

class MtPPShader : public PPShaderBackend {
public:
  MtPPShader(MetalRenderDevice *fb, PPShader *shader);
  ~MtPPShader();

  MetalRenderDevice *fb = nullptr;
  MtShaderProgram mProgram;
};
