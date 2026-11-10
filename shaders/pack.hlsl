// SCCT Unofficial Remaster - shader pack: replacements for the game's Shader Model 1.1 specular shaders.
//
// The originals (ps_1_1, hand-written, embedded in splintercell3.exe) look the light and eye directions up in
// 64x64 normalization cube maps (8 bits per axis) and fake pow() with a chain of saturating x4 multiplies:
//     s = sat(0.5 - 4k(1 - x));  s = sat(4 s^2);  s = s^2;  out = 2 * s * colour       (k = gloss, c1.z)
// - a steep window with a hard edge that turns normal-map noise (DXT1 normal maps, some without mipmaps) into
// glitter. These versions take the vertex shader's raw tangent-space vectors (the cube lookup coordinates),
// normalize them in full precision, use a real power with an exponent matched to the original's lobe width,
// and (ps_2_a) widen and dim the lobe where the normal varies a lot from pixel to pixel (specular anti-aliasing),
// which removes the sparkle on bumpy and distant surfaces. Inputs, blending and alpha are the same as the
// originals'. Compiled by tools/build_shaders.py (fxc) into src/shaderpack_data.h.
//
// Registers (from the originals and their vertex shaders):
//   t0/s0  program A: the light's projected pattern (4D projective coordinates); B: a second, world-projected
//          normal map; C: a ripple normal map (render target)
//   t1/s1  the surface normal map (rgb), its alpha scales the gloss with c1.w
//   t2     A/B: tangent-space vector to the light (unnormalized); C: tangent-space half vector
//   s2     A/B: normalization cube (B uses its alpha, a light term)
//   t3     A/B: tangent-space vector to the camera
//   v0     colour (light colour x attenuation); v0.a: B/C normal-map blend weight
//   c0.w   B: colour offset;  c1.z (+ t1.a * c1.w): gloss k

sampler2D   s0 : register(s0);
sampler2D   s1 : register(s1);
samplerCUBE s2 : register(s2);
float4 c0 : register(c0);
float4 c1 : register(c1);

// Phong exponent with about the same visible width as the original window: it reaches 0 at 1 - x = 0.125 / k;
// the power falls to 2% there.
float SpecExponent(float k)
{
    float w = clamp(0.125 / max(k, 0.001), 0.02, 0.9);
    return log(0.02) / log(1.0 - w);
}

// Specular anti-aliasing: the normal's change across neighbouring pixels widens the lobe (as roughness), and the
// peak drops to keep the energy, so a noisy or minified normal map shades as a soft sheen instead of glittering.
float SpecAA(float3 N, inout float n)
{
#ifdef SPEC_AA
    float3 dx = ddx(N), dy = ddy(N);
    float variance = saturate(dot(dx, dx) + dot(dy, dy));
    float a2 = 2.0 / (n + 2.0) + variance;
    float n2 = max(2.0 / a2 - 2.0, 1.0);
    float scale = (n2 + 2.0) / (n + 2.0);
    n = n2;
    return scale;
#else
    return 1.0;
#endif
}

float3 DecodeNormal(float3 t) { return normalize(t * 2.0 - 1.0); }

// Calms a noisy (DXT1-compressed) normal map: fades it towards the flat surface by 'base', plus wherever it
// changes faster than the pixels (screen-space derivatives) or its mip average has gone short - detail finer
// than a pixel shimmers and glints as the camera moves. Input and output are unnormalized ([-1,1] decoded).
float3 CalmNormal(float3 n, float base)
{
    float rough = saturate(1.0 - length(n)) * 2.0;
#ifdef SPEC_AA
    float3 dx = ddx(n), dy = ddy(n);
    rough += (dot(dx, dx) + dot(dy, dy)) * 2.0;
#endif
    return lerp(n, float3(0.0, 0.0, 1.0), saturate(base + rough));
}

// Tangent-space self-shadowing: no highlight from a light behind the surface.
float Facing(float3 l) { return saturate(l.z * 4.0); }

// A: Phong, one normal map, colour = v0 x projected light pattern.
float4 PS_A(float4 uv0 : TEXCOORD0, float2 uv1 : TEXCOORD1, float3 L : TEXCOORD2, float3 E : TEXCOORD3,
            float4 col : COLOR0) : COLOR
{
    float4 nm = tex2D(s1, uv1);
    float3 N = normalize(CalmNormal(nm.rgb * 2.0 - 1.0, 0.25));
    float3 l = normalize(L), e = normalize(E);
    float3 R = 2.0 * dot(N, e) * N - e;
    float x = saturate(dot(R, l));
    float n = SpecExponent(nm.a * c1.w + c1.z);
    float scale = SpecAA(N, n);
    float s = pow(x, n) * scale * Facing(l);
    float3 colour = col.rgb * tex2Dproj(s0, uv0).rgb;
    return float4(2.0 * s * colour, s);
}

// B: Phong, two normal maps blended by v0.a, colour = sat(2 (light-cube alpha x v0 + c0.w)).
float4 PS_B(float2 uv0 : TEXCOORD0, float2 uv1 : TEXCOORD1, float3 L : TEXCOORD2, float3 E : TEXCOORD3,
            float4 col : COLOR0) : COLOR
{
    float4 n0 = tex2D(s0, uv0), n1 = tex2D(s1, uv1);
    float3 N = normalize(CalmNormal(lerp(n1, n0, col.a).rgb * 2.0 - 1.0, 0.25));
    float3 l = normalize(L), e = normalize(E);
    float3 R = 2.0 * dot(N, e) * N - e;
    float x = saturate(dot(R, l));
    float n = SpecExponent(n1.a * c1.w + c1.z);
    float scale = SpecAA(N, n);
    float s = pow(x, n) * scale;
    float3 colour = saturate(2.0 * (texCUBE(s2, L).a * col.rgb + c0.w));
    return float4(2.0 * s * colour, s);
}

// C: Blinn (half vector in t2), two normal maps blended by v0.a, colour = v0; alpha = the gloss value, as before.
float4 PS_C(float2 uv0 : TEXCOORD0, float2 uv1 : TEXCOORD1, float3 H : TEXCOORD2, float4 col : COLOR0) : COLOR
{
    float4 n0 = tex2D(s0, uv0), n1 = tex2D(s1, uv1);
    float3 N = normalize(CalmNormal(lerp(n1, n0, col.a).rgb * 2.0 - 1.0, 0.25));
    float x = saturate(dot(N, normalize(H)));
    float k = n1.a * c1.w + c1.z;
    float n = SpecExponent(k);
    float scale = SpecAA(N, n);
    float s = pow(x, n) * scale;
    return float4(2.0 * s * col.rgb, k);
}

// D: bump base lighting (the light's first pass on a surface, e.g. floors; exe 111277F8, hash AB78AFEC): writes
// sat(2 (N.L x light colour c0 x attenuation + ambient c1)) + lightmap t3 x c2. The original looked L up in the
// 64x64 8-bit normalization cube: on surfaces lit at a shallow angle the brightness hangs on tiny direction errors,
// so the light patch came out blotchy and stepped. Here L is the exact vector. N stays unnormalized like the
// original (a mip-averaged normal is shorter, which tones down distant bump detail), sampled half a mip softer to
// smooth the DXT1 compression blocks of the normal maps.
//   t1/s1 normal map, t2 tangent-space light vector (s2: normalization cube, its alpha is part of the
//   attenuation), t3/s3 lightmap, v0.a distance attenuation, c0 light colour (w: attenuation offset),
//   c1 ambient (w: output alpha), c2 lightmap scale.
sampler2D s3 : register(s3);
float4 c2 : register(c2);
float4 PS_D(float2 uv1 : TEXCOORD1, float3 L : TEXCOORD2, float2 uv3 : TEXCOORD3, float4 col : COLOR0) : COLOR
{
    float3 N = CalmNormal(tex2Dbias(s1, float4(uv1, 0.0, 0.5)).rgb * 2.0 - 1.0, 0.2);
    float ndl = saturate(dot(N, normalize(L)));
    float att = saturate(2.0 * (col.a * texCUBE(s2, L).a + c0.w));
    float3 lit = saturate(2.0 * (ndl * c0.rgb * att + c1.rgb));
    lit += tex2D(s3, uv3).rgb * c2.rgb;
    return float4(lit, c1.w);
}

// E: bump-mapped environment reflection (exe 111270D0, hash 3884117E): the original's texm3x3vspec turns the
// normal map into a per-pixel reflection direction and adds the environment cube there, x v0 - independent of
// any light, so on floors in the dark its glitter is all there is. With compressed normal maps, at a distance or
// a shallow angle, that direction jumps from pixel to pixel and bright spots of the cube flicker on and off.
// Here the bump is faded towards the flat surface and the cube is read from a blurrier mip wherever the normal
// changes faster than the pixels (screen-space derivatives) or the mip-averaged normal has gone short: close up
// the reflection stays bumpy, further away it becomes a smooth sheen.
//   t0/s0 normal map; t1..t3 xyz: rows of the tangent -> environment matrix, w: eye vector; s3: environment
//   cube; v0 reflection strength. Output as the original: cube x v0.
samplerCUBE envCube : register(s3);
float4 PS_E(float2 uv0 : TEXCOORD0, float4 m1 : TEXCOORD1, float4 m2 : TEXCOORD2, float4 m3 : TEXCOORD3,
            float4 col : COLOR0) : COLOR
{
    float3 nt = tex2D(s0, uv0).rgb * 2.0 - 1.0;
    float rough = saturate(1.0 - length(nt)) * 2.0;                 // mip-averaged normal gone short
#ifdef SPEC_AA
    float3 dx = ddx(nt), dy = ddy(nt);
    rough += dot(dx, dx) + dot(dy, dy);                              // normal changing faster than the pixels
#endif
    rough = saturate(rough);
    nt = normalize(lerp(nt, float3(0.0, 0.0, 1.0), saturate(0.75 + rough * 2.0)));   // keep a quarter of the bump
    float3 N = float3(dot(m1.xyz, nt), dot(m2.xyz, nt), dot(m3.xyz, nt));
    float3 E = float3(m1.w, m2.w, m3.w);
    float3 R = 2.0 * dot(N, E) / max(dot(N, N), 1e-4) * N - E;   // texm3x3vspec's reflection
    return texCUBEbias(envCube, float4(R, 1.5 + rough * 4.0)) * col * 0.5;   // blurrier, half as strong
}

// Debug: flat magenta, bound by the F11 highlight tool in place of the game shaders being highlighted.
float4 PS_Magenta() : COLOR
{
    return float4(1.0, 0.0, 1.0, 1.0);
}
