#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout (push_constant) uniform PushConsts
{
	float gamma;
	float contrast;
	float linearize_output;
}
push_constants;

layout (input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput color_input;

layout (location = 0) out vec4 out_frag_color;

void main ()
{
	vec3 frag = subpassLoad (color_input).rgb;
	frag.rgb = frag.rgb * push_constants.contrast;
	frag = pow (frag, vec3 (push_constants.gamma));
	if (push_constants.linearize_output > 0.5)
	{
		// Cancel the sRGB attachment's hardware encoding. The stored values
		// then match Quake's existing gamma-corrected UNORM output.
		frag = mix (pow ((frag + vec3 (0.055)) / 1.055, vec3 (2.4)), frag / 12.92, lessThanEqual (frag, vec3 (0.04045)));
	}
	out_frag_color = vec4 (frag, 1.0);
}
