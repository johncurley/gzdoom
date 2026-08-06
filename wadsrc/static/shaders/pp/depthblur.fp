
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D AODepthTexture;

#define KERNEL_RADIUS 3.0

void AddSample(vec2 blurSample, float r, float centerDepth, inout float totalAO, inout float totalW)
{
	const float blurSigma = KERNEL_RADIUS * 0.5;
	const float blurFalloff = 1.0 / (2.0 * blurSigma * blurSigma);

	float ao = blurSample.x;
	float z = blurSample.y;

	float deltaZ = (z - centerDepth) * BlurSharpness;
	float w = exp2(-r * r * blurFalloff - deltaZ * deltaZ);

	totalAO += w * ao;
	totalW += w;
}

void main()
{
	vec2 centerSample = textureOffset(AODepthTexture, TexCoord, ivec2( 0, 0)).xy;
	float centerDepth = centerSample.y;
	float totalAO = centerSample.x;
	float totalW = 1.0;

#if defined(BLUR_HORIZONTAL)
	AddSample(textureOffset(AODepthTexture, TexCoord, ivec2(-3, 0)).xy, 3.0, centerDepth, totalAO, totalW);
	AddSample(textureOffset(AODepthTexture, TexCoord, ivec2(-2, 0)).xy, 2.0, centerDepth, totalAO, totalW);
	AddSample(textureOffset(AODepthTexture, TexCoord, ivec2(-1, 0)).xy, 1.0, centerDepth, totalAO, totalW);
	AddSample(textureOffset(AODepthTexture, TexCoord, ivec2( 1, 0)).xy, 1.0, centerDepth, totalAO, totalW);
	AddSample(textureOffset(AODepthTexture, TexCoord, ivec2( 2, 0)).xy, 2.0, centerDepth, totalAO, totalW);
	AddSample(textureOffset(AODepthTexture, TexCoord, ivec2( 3, 0)).xy, 3.0, centerDepth, totalAO, totalW);
#else
	AddSample(textureOffset(AODepthTexture, TexCoord, ivec2(0, -3)).xy, 3.0, centerDepth, totalAO, totalW);
	AddSample(textureOffset(AODepthTexture, TexCoord, ivec2(0, -2)).xy, 2.0, centerDepth, totalAO, totalW);
	AddSample(textureOffset(AODepthTexture, TexCoord, ivec2(0, -1)).xy, 1.0, centerDepth, totalAO, totalW);
	AddSample(textureOffset(AODepthTexture, TexCoord, ivec2(0,  1)).xy, 1.0, centerDepth, totalAO, totalW);
	AddSample(textureOffset(AODepthTexture, TexCoord, ivec2(0,  2)).xy, 2.0, centerDepth, totalAO, totalW);
	AddSample(textureOffset(AODepthTexture, TexCoord, ivec2(0,  3)).xy, 3.0, centerDepth, totalAO, totalW);
#endif

	float fragAO = totalAO / totalW;

#if defined(BLUR_HORIZONTAL)
	FragColor = vec4(fragAO, centerDepth, 0.0, 1.0);
#else
	// Preserve depth in .y here too. The vertical pass is the LAST thing to
	// touch Ambient0 before ssaocombine, and ssaocombine derives its entire
	// output alpha from .y:
	//
	//     depthSignal = 1.0 - exp2(-ssao.y * 0.01)
	//     alpha       = ssao.y > 2.0 ? (1.0 - attenutation) * depthMask : 0.0
	//
	// Writing 0.0 here made both terms zero, so AO was computed correctly and
	// then multiplied by nothing -- gl_ssao 0 and gl_ssao 3 produced
	// byte-identical frames on every backend. Measured 2026-08-06 with
	// mt_ao_probe: mean ssao.x = 0.867 (real occlusion present), max ssao.y =
	// 0.000, gate passing 0 of 278640 pixels.
	//
	// This hid behind gl_ssao_debug because hw_postprocess.cpp:910 skips both
	// blur passes when gl_ssao_debug >= 2. Every debug mode that displays depth
	// therefore reads ssao.fp's raw output, where .y is still intact -- the
	// only modes that show you the channel are the modes that bypass the pass
	// which destroys it.
	FragColor = vec4(pow(clamp(fragAO, 0.0, 1.0), PowExponent), centerDepth, 0.0, 1.0);
#endif
}
