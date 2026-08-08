
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

#if defined(MULTISAMPLE)
layout(binding=0) uniform sampler2DMS DepthTexture;
layout(binding=1) uniform sampler2DMS ColorTexture;
#else
layout(binding=0) uniform sampler2D DepthTexture;
layout(binding=1) uniform sampler2D ColorTexture;
#endif

// Raw device depth -> normalized depth, 0 at the near plane and 1 at the far
// plane. Metal renders reverse-Z, so InverseDepthRange flips the range back
// (A=-1, B=1); OpenGL and Vulkan pass it straight through (A=1, B=0).
float normalizeDepth(float depth)
{
	return clamp(InverseDepthRangeA * depth + InverseDepthRangeB, 0.0, 1.0);
}

// Normalized depth -> view space distance in world units.
float linearizeDepth(float normalizedDepth)
{
	return 1.0 / (normalizedDepth * LinearizeDepthA + LinearizeDepthB);
}

void main()
{
	vec2 uv = Offset + TexCoord * Scale;

#if defined(MULTISAMPLE)
	ivec2 texSize = textureSize(DepthTexture);
#else
	ivec2 texSize = textureSize(DepthTexture, 0);
#endif

	ivec2 ipos = ivec2(max(uv * vec2(texSize), vec2(0.0)));

	// Pixels the scene never wrote colour to (sky, and anything else left at
	// alpha 0) have no meaningful depth, so they are pushed to the far plane
	// and pick up no occlusion. That sentinel is a NORMALIZED depth: it must
	// bypass the reverse-Z inversion above, not be fed through it. Passing raw
	// 1.0 into the inversion is correct only where A=1/B=0, so on Metal it
	// silently became 0.0 -- the NEAR plane -- and every alpha-0 pixel rendered
	// as fully occluded black instead of unoccluded sky.
#if defined(MULTISAMPLE)
	float alpha = texelFetch(ColorTexture, ipos, SampleIndex).a;
	float normalizedDepth = alpha != 0.0 ? normalizeDepth(texelFetch(DepthTexture, ipos, SampleIndex).x) : 1.0;
#else
	float alpha = texelFetch(ColorTexture, ipos, 0).a;
	float normalizedDepth = alpha != 0.0 ? normalizeDepth(texelFetch(DepthTexture, ipos, 0).x) : 1.0;
#endif

	float depth = linearizeDepth(normalizedDepth);

	FragColor = vec4(depth, 0.0, 0.0, 1.0);
}
