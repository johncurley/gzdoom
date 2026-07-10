
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D DepthTexture;

#if defined(MULTISAMPLE)
layout(binding=1) uniform sampler2DMS NormalTexture;
#else
layout(binding=1) uniform sampler2D NormalTexture;
#endif

#if defined(USE_RANDOM_TEXTURE)
layout(binding=2) uniform sampler2D RandomTexture;
#endif

#define PI 3.14159265358979323846
#define TAU 6.28318530717958647692
#ifndef AO_VISIBILITY_STRENGTH
#define AO_VISIBILITY_STRENGTH 2.75
#endif

float InterleavedGradientNoise(vec2 p)
{
	return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// Calculate eye space position for the specified texture coordinate
vec3 FetchViewPos(vec2 uv)
{
	float z = texture(DepthTexture, uv).x;
    return vec3((UVToViewA * uv + UVToViewB) * z, z);
}

#if defined(MULTISAMPLE)
vec3 SampleNormal(vec2 uv)
{
	ivec2 texSize = textureSize(NormalTexture);
	ivec2 ipos = ivec2(uv * vec2(texSize));
	return texelFetch(NormalTexture, ipos, SampleIndex).xyz * 2.0 - 1.0;
}
#else
vec3 SampleNormal(vec2 uv)
{
	ivec2 texSize = textureSize(NormalTexture, 0);
	ivec2 ipos = ivec2(uv * vec2(texSize));
	return texelFetch(NormalTexture, ipos, 0).xyz * 2.0 - 1.0;
}
#endif

// Look up the eye space normal for the specified texture coordinate
vec3 FetchNormal(vec2 uv)
{
	vec3 normal = SampleNormal(Offset + uv * Scale);
	if (length(normal) > 0.1)
	{
		normal = normalize(normal);
		normal.z = -normal.z;
		return normal;
	}
	else
	{
		return vec3(0.0);
	}
}

// Compute normalized 2D direction
vec2 RotateDirection(vec2 dir, vec2 cossin)
{
    return vec2(dir.x * cossin.x - dir.y * cossin.y, dir.x * cossin.y + dir.y * cossin.x);
}

vec4 GetJitter()
{
	float ign = InterleavedGradientNoise(gl_FragCoord.xy);
#if !defined(USE_RANDOM_TEXTURE)
	float angle = ign * TAU;
	return vec4(cos(angle), sin(angle), fract(ign * 1.61803398875), fract(ign * 2.2360679775));
#else
	vec4 noise = texture(RandomTexture, gl_FragCoord.xy / RANDOM_TEXTURE_WIDTH);
	return vec4(noise.xy, fract(noise.z + ign * 0.754877666), fract(noise.w + ign * 0.569840296));
#endif
}

float ComputeSampleHorizon(vec3 viewPosition, vec3 viewNormal, vec2 sampleUV, inout float horizon)
{
	vec3 samplePos = FetchViewPos(clamp(sampleUV, InvFullResolution * 0.5, vec2(1.0) - InvFullResolution * 0.5));
	vec3 viewDelta = samplePos - viewPosition;

	// Skybox/portal guard: reject samples from incompatible camera views
	// (different cameras write incompatible depth ranges, creating seams)
	float depthRatio = max(viewPosition.z, samplePos.z) / max(min(viewPosition.z, samplePos.z), 1e-5);
	if (depthRatio > 100.0) return 0.0;

	float distanceSquare = max(dot(viewDelta, viewDelta), 1e-6);
	float invDistance = inversesqrt(distanceSquare);
	float normalAngle = dot(viewNormal, viewDelta) * invDistance;
	float sampleHorizon = max(normalAngle - NDotVBias, 0.0);
	float falloff = clamp(distanceSquare * NegInvR2 + 1.0, 0.0, 1.0);
	float contribution = max(sampleHorizon - horizon, 0.0) * falloff;
	horizon = max(horizon, sampleHorizon);
	return contribution;
}

// Calculates the total ambient occlusion for the entire fragment
float ComputeAO(vec3 viewPosition, vec3 viewNormal)
{
	vec4 rand = GetJitter();

	float radiusPixels = RadiusToScreen / viewPosition.z;
	float stepSizePixels = radiusPixels / (NUM_STEPS + 1.0);
	float minStepPixels = max(stepSizePixels, 1.0);

	const float directionAngleStep = TAU / NUM_DIRECTIONS;
	float ao = 0.0;

	for (float directionIndex = 0.0; directionIndex < NUM_DIRECTIONS; ++directionIndex)
	{
		float angle = directionAngleStep * (directionIndex + rand.w * 0.25);
		vec2 direction = RotateDirection(vec2(cos(angle), sin(angle)), rand.xy);
		float horizon = 0.0;

		for (float stepIndex = 0.0; stepIndex < NUM_STEPS; ++stepIndex)
		{
			float rayPixels = (stepIndex + rand.z + 1.0) * minStepPixels;
			vec2 sampleUV = TexCoord + direction * rayPixels * InvFullResolution;
			ao += ComputeSampleHorizon(viewPosition, viewNormal, sampleUV, horizon);
		}
	}

	ao *= AOMultiplier / (NUM_DIRECTIONS * NUM_STEPS);
	return clamp(1.0 - ao * AO_VISIBILITY_STRENGTH, 0.0, 1.0);
}

void main()
{
    vec3 viewPosition = FetchViewPos(TexCoord);
	vec3 viewNormal = FetchNormal(TexCoord);

	// Distance fade: screen-space AO is both numerically unreliable and
	// perceptually unwanted at long range. In particular a sky camera room
	// (real physical geometry portaled in to stand in for the sky) is
	// genuine geometry with genuine normals and will otherwise pick up real
	// occlusion from its own creases, even though it's meant to read as
	// open, unoccluded sky. Fade strength back to 0 between FadeStart and
	// FadeEnd (view-space Z, world units) using the same blend-toward-no-AO
	// idiom already used for AOStrength itself.
	float distanceFade = 1.0 - smoothstep(FadeStart, FadeEnd, viewPosition.z);
	float effectiveStrength = AOStrength * distanceFade;

	float occlusion = viewNormal != vec3(0.0) ? ComputeAO(viewPosition, viewNormal) * effectiveStrength + (1.0 - effectiveStrength) : 1.0;

	FragColor = vec4(occlusion, viewPosition.z, 0.0, 1.0);
}
