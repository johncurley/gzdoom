/*
** test_spirv_to_msl.cpp
**
** Simple test program to verify SPIR-V to MSL translation
**
*/

#include <shadertranslator/shader_translator.h>
#include <iostream>
#include <fstream>
#include <vector>

// Simple SPIR-V module (precompiled fragment shader that outputs red)
// This is a minimal valid SPIR-V module:
// #version 450
// layout(location = 0) out vec4 FragColor;
// void main() { FragColor = vec4(1.0, 0.0, 0.0, 1.0); }
static const uint32_t simpleFragmentShaderSPIRV[] = {
	0x07230203, 0x00010000, 0x00080001, 0x0000000d, 0x00000000, 0x00020011,
	0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
	0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0007000f, 0x00000004,
	0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00000000, 0x00030010,
	0x00000004, 0x00000007, 0x00040047, 0x00000009, 0x0000001e, 0x00000000,
	0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016,
	0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004,
	0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008,
	0x00000009, 0x00000003, 0x0004002b, 0x00000006, 0x0000000a, 0x3f800000,
	0x0004002b, 0x00000006, 0x0000000b, 0x00000000, 0x0007002c, 0x00000007,
	0x0000000c, 0x0000000a, 0x0000000b, 0x0000000b, 0x0000000a, 0x00050036,
	0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005,
	0x0003003e, 0x00000009, 0x0000000c, 0x000100fd, 0x00010038
};

int main(int argc, char** argv)
{
	std::cout << "=== SPIR-V to MSL Translation Test ===\n\n";

	// Convert array to vector
	std::vector<uint32_t> spirv(
		simpleFragmentShaderSPIRV,
		simpleFragmentShaderSPIRV + (sizeof(simpleFragmentShaderSPIRV) / sizeof(uint32_t))
	);

	std::cout << "Input SPIR-V size: " << spirv.size() << " words ("
		      << (spirv.size() * 4) << " bytes)\n\n";

	// Create translator
	ShaderTranslator::SPIRVTranslator translator;

	// Translate to MSL (Metal 2.0)
	std::cout << "Translating to Metal Shading Language (Metal 2.0)...\n";
	auto result = translator.TranslateToMSL(spirv, 20);

	if (result.success)
	{
		std::cout << "\n✅ Translation successful!\n\n";
		std::cout << "=== Generated MSL Source ===\n";
		std::cout << result.source << "\n";
		std::cout << "=== End of MSL Source ===\n\n";

		// Print resource bindings
		if (!result.textures.empty())
		{
			std::cout << "Texture bindings:\n";
			for (const auto& tex : result.textures)
			{
				std::cout << "  " << tex.name
					      << " (set=" << tex.set
					      << ", binding=" << tex.binding
					      << ", msl=" << tex.mslBinding << ")\n";
			}
		}

		if (!result.buffers.empty())
		{
			std::cout << "Buffer bindings:\n";
			for (const auto& buf : result.buffers)
			{
				std::cout << "  " << buf.name
					      << " (set=" << buf.set
					      << ", binding=" << buf.binding
					      << ", msl=" << buf.mslBinding << ")\n";
			}
		}

		// Save to file
		std::ofstream outFile("output_test.metal");
		if (outFile)
		{
			outFile << result.source;
			std::cout << "\nMSL source saved to: output_test.metal\n";
		}

		return 0;
	}
	else
	{
		std::cerr << "\n❌ Translation failed!\n";
		std::cerr << "Error: " << result.errorLog << "\n";
		return 1;
	}
}
