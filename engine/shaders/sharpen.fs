#version 330

// Contrast Adaptive Sharpening (AMD FidelityFX CAS), ported from the technique in
// ReShade's AdaptiveSharpen. Runs as the FINAL upscale blit: the scene is rendered
// at the render scale and bilinear-upscaled to the retina backbuffer, which
// softens everything — this recovers the crispness essentially for free.
//
// The trick vs a naive unsharp mask: the sharpen amount is scaled by LOCAL CONTRAST
// (so flat sky/haze isn't amplified into noise) and the result is bounded by the
// local min/max (so edges can't overshoot into halos/ringing). Operates on the
// final LDR sRGB image, which is the range CAS expects.

in vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D texture0; // present: final LDR sRGB scene (pre-upscale)
uniform vec2 uTexel;        // 1 / present resolution (neighbour tap size)
uniform float uSharpen;     // 0 = off .. 1 = strong

void main() {
    vec2 uv = fragTexCoord;
    // 3x3 neighbourhood (e = centre):  a b c / d e f / g h i
    vec3 a = texture(texture0, uv + vec2(-uTexel.x, -uTexel.y)).rgb;
    vec3 b = texture(texture0, uv + vec2( 0.0,      -uTexel.y)).rgb;
    vec3 c = texture(texture0, uv + vec2( uTexel.x, -uTexel.y)).rgb;
    vec3 d = texture(texture0, uv + vec2(-uTexel.x,  0.0)).rgb;
    vec3 e = texture(texture0, uv).rgb;
    vec3 f = texture(texture0, uv + vec2( uTexel.x,  0.0)).rgb;
    vec3 g = texture(texture0, uv + vec2(-uTexel.x,  uTexel.y)).rgb;
    vec3 h = texture(texture0, uv + vec2( 0.0,       uTexel.y)).rgb;
    vec3 i = texture(texture0, uv + vec2( uTexel.x,  uTexel.y)).rgb;

    // Local min/max across the neighbourhood (per channel).
    vec3 mn = min(min(min(d, e), min(f, b)), min(h, min(min(a, c), min(g, i))));
    vec3 mx = max(max(max(d, e), max(f, b)), max(h, max(max(a, c), max(g, i))));

    // Sharpening amplitude: high where there's headroom, low near clipping / high
    // contrast. sqrt shapes the falloff. Then a negative weight forms the CAS kernel.
    vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, 1e-4), 0.0, 1.0));
    vec3 w = amp * (uSharpen * mix(-0.125, -0.2, clamp(uSharpen, 0.0, 1.0))); // CAS peak range scaled by strength

    // Contrast gate: below grain-level local contrast (flat sky/haze) do nothing,
    // so CAS doesn't amplify the film grain into visible noise. Real edges (gravel,
    // roofs, foliage) clear the threshold and get the full sharpen.
    float lumaContrast = dot(mx - mn, vec3(0.299, 0.587, 0.114));
    w *= smoothstep(0.04, 0.08, lumaContrast);
    // Weighted blend of the 4-neighbours with the centre, normalised — bounded so
    // it can't ring beyond the local signal.
    vec3 res = ((b + d + f + h) * w + e) / (1.0 + 4.0 * w);

    finalColor = vec4(clamp(res, 0.0, 1.0), 1.0);
}
