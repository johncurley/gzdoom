void main()
{
	vec4 c = texture(InputTexture, TexCoord);
	FragColor = vec4(c.rgb * TestScale, c.a);
}
