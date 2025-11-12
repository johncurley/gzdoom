/*
 * Simple test program for Metal shader compilation pipeline
 * Compiles a basic GLSL shader through GLSL → SPIR-V → MSL
 */

#include <iostream>
#include <vector>
#include <string>
#include <cstdint>

// glslang includes
#include <glslang/glslang/Public/ShaderLang.h>
#include <glslang/spirv/GlslangToSpv.h>

// SPIRV-Cross for MSL translation
#include <zvulkan/shadertranslator/shader_translator.h>

// Simple vertex shader (GLSL 4.50)
const char* vertexShaderSource = R"(
#version 450

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aColor;

layout(location = 0) out vec4 vColor;

layout(binding = 0) uniform MatrixUBO {
    mat4 modelMatrix;
    mat4 viewMatrix;
    mat4 projectionMatrix;
} matrices;

void main() {
    gl_Position = matrices.projectionMatrix * matrices.viewMatrix * matrices.modelMatrix * vec4(aPosition, 1.0);
    vColor = aColor;
}
)";

// Simple fragment shader (GLSL 4.50)
const char* fragmentShaderSource = R"(
#version 450

layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = vColor;
}
)";

// Helper function to get default glslang resources
static TBuiltInResource GetDefaultTBuiltInResource()
{
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

// Compile GLSL to SPIR-V
std::vector<uint32_t> CompileGLSLToSPIRV(const std::string& source, const std::string& name, bool isVertex)
{
    EShLanguage stage = isVertex ? EShLangVertex : EShLangFragment;

    const char* sourceStr = source.c_str();
    int sourceLength = static_cast<int>(source.length());
    const char* nameStr = name.c_str();

    TBuiltInResource resources = GetDefaultTBuiltInResource();
    glslang::TShader shader(stage);
    shader.setStringsWithLengthsAndNames(&sourceStr, &sourceLength, &nameStr, 1);

    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

    bool parseSuccess = shader.parse(&resources, 450, false, EShMsgDefault);
    if (!parseSuccess)
    {
        std::cerr << "ERROR: Shader parse failed for " << name << ":" << std::endl;
        std::cerr << shader.getInfoLog() << std::endl;
        return std::vector<uint32_t>();
    }

    glslang::TProgram program;
    program.addShader(&shader);
    bool linkSuccess = program.link(EShMsgDefault);
    if (!linkSuccess)
    {
        std::cerr << "ERROR: Shader link failed for " << name << ":" << std::endl;
        std::cerr << program.getInfoLog() << std::endl;
        return std::vector<uint32_t>();
    }

    glslang::TIntermediate* intermediate = program.getIntermediate(stage);
    if (!intermediate)
    {
        std::cerr << "ERROR: Failed to get intermediate representation for " << name << std::endl;
        return std::vector<uint32_t>();
    }

    std::vector<uint32_t> spirv;
    glslang::SpvOptions spvOptions;
    spvOptions.generateDebugInfo = false;
    spvOptions.disableOptimizer = false;
    spvOptions.optimizeSize = true;

    spv::SpvBuildLogger logger;
    glslang::GlslangToSpv(*intermediate, spirv, &logger, &spvOptions);

    std::string messages = logger.getAllMessages();
    if (!messages.empty())
    {
        std::cout << "SPIR-V generation messages for " << name << ":" << std::endl;
        std::cout << messages << std::endl;
    }

    return spirv;
}

// Translate SPIR-V to MSL
std::string TranslateSPIRVToMSL(const std::vector<uint32_t>& spirv)
{
    if (spirv.empty())
        return "";

    ShaderTranslator::SPIRVTranslator translator;
    auto result = translator.TranslateToMSL(spirv, 20);

    if (!result.success)
    {
        std::cerr << "ERROR: SPIR-V to MSL translation failed:" << std::endl;
        std::cerr << result.errorLog << std::endl;
        return "";
    }

    return result.source;
}

int main()
{
    std::cout << "Metal Shader Compilation Pipeline Test" << std::endl;
    std::cout << "=======================================" << std::endl;
    std::cout << std::endl;

    // Initialize glslang
    glslang::InitializeProcess();

    bool success = true;

    // Test vertex shader compilation
    std::cout << "Step 1: Compiling vertex shader (GLSL → SPIR-V)..." << std::endl;
    auto vertexSPIRV = CompileGLSLToSPIRV(vertexShaderSource, "test_vertex", true);
    if (vertexSPIRV.empty())
    {
        std::cerr << "FAILED: Vertex shader GLSL → SPIR-V compilation" << std::endl;
        success = false;
    }
    else
    {
        std::cout << "SUCCESS: Generated " << vertexSPIRV.size() << " SPIR-V words" << std::endl;
    }
    std::cout << std::endl;

    // Test fragment shader compilation
    std::cout << "Step 2: Compiling fragment shader (GLSL → SPIR-V)..." << std::endl;
    auto fragmentSPIRV = CompileGLSLToSPIRV(fragmentShaderSource, "test_fragment", false);
    if (fragmentSPIRV.empty())
    {
        std::cerr << "FAILED: Fragment shader GLSL → SPIR-V compilation" << std::endl;
        success = false;
    }
    else
    {
        std::cout << "SUCCESS: Generated " << fragmentSPIRV.size() << " SPIR-V words" << std::endl;
    }
    std::cout << std::endl;

    // Test SPIR-V to MSL translation (vertex)
    if (!vertexSPIRV.empty())
    {
        std::cout << "Step 3: Translating vertex shader (SPIR-V → MSL)..." << std::endl;
        auto vertexMSL = TranslateSPIRVToMSL(vertexSPIRV);
        if (vertexMSL.empty())
        {
            std::cerr << "FAILED: Vertex shader SPIR-V → MSL translation" << std::endl;
            success = false;
        }
        else
        {
            std::cout << "SUCCESS: Generated MSL shader (" << vertexMSL.length() << " bytes)" << std::endl;
            std::cout << std::endl;
            std::cout << "Vertex MSL Output:" << std::endl;
            std::cout << "==================" << std::endl;
            std::cout << vertexMSL << std::endl;
        }
        std::cout << std::endl;
    }

    // Test SPIR-V to MSL translation (fragment)
    if (!fragmentSPIRV.empty())
    {
        std::cout << "Step 4: Translating fragment shader (SPIR-V → MSL)..." << std::endl;
        auto fragmentMSL = TranslateSPIRVToMSL(fragmentSPIRV);
        if (fragmentMSL.empty())
        {
            std::cerr << "FAILED: Fragment shader SPIR-V → MSL translation" << std::endl;
            success = false;
        }
        else
        {
            std::cout << "SUCCESS: Generated MSL shader (" << fragmentMSL.length() << " bytes)" << std::endl;
            std::cout << std::endl;
            std::cout << "Fragment MSL Output:" << std::endl;
            std::cout << "====================" << std::endl;
            std::cout << fragmentMSL << std::endl;
        }
        std::cout << std::endl;
    }

    // Finalize glslang
    glslang::FinalizeProcess();

    std::cout << "=======================================" << std::endl;
    if (success)
    {
        std::cout << "✅ All tests PASSED!" << std::endl;
        return 0;
    }
    else
    {
        std::cout << "❌ Some tests FAILED" << std::endl;
        return 1;
    }
}
