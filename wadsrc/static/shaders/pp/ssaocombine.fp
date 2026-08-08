
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D AODepthTexture;

#if defined(MULTISAMPLE)
layout(binding=1) uniform sampler2DMS SceneFogTexture;
#else
layout(binding=1) uniform sampler2D SceneFogTexture;
#endif

void main()
{
	vec2 uv = Offset + TexCoord * Scale;

#if defined(MULTISAMPLE)
	ivec2 texSize = textureSize(SceneFogTexture);
#else
	ivec2 texSize = textureSize(SceneFogTexture, 0);
#endif
	ivec2 ipos = ivec2(uv * vec2(texSize));

#if defined(MULTISAMPLE)
	vec4 fogSample = texelFetch(SceneFogTexture, ipos, 0);
#else
	vec4 fogSample = texelFetch(SceneFogTexture, ipos, 0);
#endif
	vec3 fogColor = fogSample.rgb;

	vec4 ssao = texture(AODepthTexture, TexCoord);
	float attenutation = ssao.x;
	float depthSignal = 1.0 - exp2(-ssao.y * 0.01);
	float depthMask = clamp(depthSignal, 0.0, 1.0);

	if (DebugMode == 0)
		FragColor = vec4(fogColor, ssao.y > 2.0 ? (1.0 - attenutation) * depthMask : 0.0);
	else if (DebugMode < 3)
		FragColor = vec4(attenutation, attenutation, attenutation, 1.0);
	else if (DebugMode == 3)
		FragColor = vec4(ssao.yyy / 1000.0, 1.0);
	else if (DebugMode == 5)
		FragColor = vec4(vec3(ssao.x), 1.0);
	else if (DebugMode == 6)
		FragColor = vec4(vec3(depthSignal), 1.0);
	else if (DebugMode == 7)
		FragColor = vec4(vec3(step(1e-5, ssao.y)), 1.0);
	else if (DebugMode == 8)
		FragColor = vec4(vec3(depthMask), 1.0);
	else if (DebugMode == 10)
		FragColor = vec4(fogColor, 1.0); // SceneFog (the G-buffer fog colour the composite tints with)
	else
		FragColor = vec4(ssao.xyz, 1.0);
}
