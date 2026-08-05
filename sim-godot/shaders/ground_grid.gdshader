shader_type spatial;

// Metric reference ground: a 1 m checker, thin grid lines every meter and
// thicker lines every 5 m. Distances are world-space meters, so the pattern
// stays true whatever the size of the mesh it is applied to. Lines are
// anti-aliased with fwidth so they do not shimmer at altitude.

uniform vec3 base_color: source_color = vec3(0.24, 0.27, 0.25);
uniform vec3 checker_color: source_color = vec3(0.21, 0.24, 0.22);
uniform vec3 minor_line_color: source_color = vec3(0.36, 0.39, 0.37);
uniform vec3 major_line_color: source_color = vec3(0.58, 0.60, 0.52);
uniform float minor_spacing_m = 1.0;
uniform float major_spacing_m = 5.0;
uniform float minor_width_m = 0.015;
uniform float major_width_m = 0.04;

varying vec3 world_pos;

void vertex() {
	world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

float line_mask(vec2 pos_m, float spacing_m, float width_m) {
	vec2 dist = abs(fract(pos_m / spacing_m - 0.5) - 0.5) * spacing_m;
	float nearest_m = min(dist.x, dist.y);
	float aa = fwidth(nearest_m);
	return 1.0 - smoothstep(width_m - aa, width_m + aa, nearest_m);
}

void fragment() {
	vec2 pos_m = world_pos.xz;
	vec2 cell = floor(pos_m / minor_spacing_m);
	float checker = mod(cell.x + cell.y, 2.0);
	vec3 color = mix(base_color, checker_color, checker);
	color = mix(color, minor_line_color, line_mask(pos_m, minor_spacing_m, minor_width_m));
	color = mix(color, major_line_color, line_mask(pos_m, major_spacing_m, major_width_m));
	ALBEDO = color;
	ROUGHNESS = 0.9;
	METALLIC = 0.0;
}
