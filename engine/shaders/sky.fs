#version 330

// Procedural sky: vertical gradient + sun glow + volumetric-shaded cumulus clouds.
// Uses a light-space density offset sample to calculate self-shadowing (Beer's Law)
// and forward scattering (silver lining) at near-zero performance cost.

in vec3 fragDir;

uniform float uTime;
uniform vec3 uSunDir;     // normalized, points toward the sun
uniform vec2 uWindDir;    // cloud drift direction (world XZ)
uniform vec3 uSunColor;    // Sun light color
uniform float uExposure;   // single global scene exposure
uniform vec3 uMoonDir;     // normalized, points toward the moon

out vec4 finalColor;

// --- hash / value noise / fbm (texture-free) ---
float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float valueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash(i + vec2(0.0, 0.0));
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Full 4-octave FBM for primary shapes (optimized from 5)
float fbm(vec2 p) {
    float v = 0.0;
    float amp = 0.5;
    mat2 rot = mat2(1.6, 1.2, -1.2, 1.6);
    for (int i = 0; i < 4; i++) {
        v += amp * valueNoise(p);
        p = rot * p;
        amp *= 0.5;
    }
    return v;
}

// Fast 3-octave FBM for details and light-shadow evaluation (saves GPU math)
float fbmShadow(vec2 p) {
    float v = 0.0;
    float amp = 0.5;
    mat2 rot = mat2(1.6, 1.2, -1.2, 1.6);
    for (int i = 0; i < 3; i++) {
        v += amp * valueNoise(p);
        p = rot * p;
        amp *= 0.5;
    }
    return v;
}

// --- direction-based stable twinkling star field ---
float starHash(vec3 p) {
    p = fract(p * vec3(123.34, 456.21, 789.43));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y * p.z);
}

float getStars(vec3 d) {
    vec3 grid = floor(d * 180.0);
    float h = starHash(grid);
    if (h > 0.985) { // increased star density
        float twinkle = sin(uTime * 3.0 + h * 20.0) * 0.4 + 0.6;
        vec3 local = fract(d * 180.0) - 0.5;
        float size = 0.08 + 0.12 * fract(h * 100.0); // larger sizes for high-res visibility
        return smoothstep(size, 0.0, length(local)) * twinkle * 1.5; // boost brightness
    }
    return 0.0;
}

// --- Physically Based Atmospheric Scattering (Rayleigh/Mie) ---
const float PI = 3.141592653589793;
const float e = 2.718281828459045;

uniform float uTurbidity;        // Haze / dust factor (exposed)
const float rayleigh = 3.5;         // Rayleigh scattering coefficient
const float mieCoefficient = 0.005;  // Mie scattering coefficient
const float mieDirectionalG = 0.85;  // Mie forward anisotropy

const vec3 totalRayleigh = vec3(5.80454E-6, 1.35629E-5, 3.02659E-5);
const vec3 MieConst = vec3(1.83999E14, 2.77980E14, 4.07905E14);

const float rayleighZenithLength = 8.4E3;
const float mieZenithLength = 1.25E3;
const vec3 up = vec3(0.0, 1.0, 0.0);
const float sunAngularDiameterCos = 0.99995667;

float sunIntensity(float zenithAngleCos) {
    zenithAngleCos = clamp(zenithAngleCos, -1.0, 1.0);
    return 100.0 * max(0.0, 1.0 - pow(e, -((1.6110731 - acos(zenithAngleCos)) / 1.5)));
}

vec3 getAtmosphericScattering(vec3 d, vec3 betaR, vec3 betaM, float sunIntensityVal, float sunfade, bool drawSun) {
    float viewZenithAngle = acos(max(0.001, d.y));
    float inverse = 1.0 / (cos(viewZenithAngle) + 0.15 * pow(93.885 - ((viewZenithAngle * 180.0) / PI), -1.253));
    float sR = rayleighZenithLength * inverse;
    float sM = mieZenithLength * inverse;
    
    vec3 Fex = exp(-(betaR * sR + betaM * sM));
    
    float cosTheta = dot(d, uSunDir);
    
    float rPhase = 0.0596831 * (1.0 + pow(cosTheta, 2.0));
    vec3 betaRTheta = betaR * rPhase;
    
    float g2 = pow(mieDirectionalG, 2.0);
    float hgDenom = 1.0 / pow(1.0 - 2.0 * mieDirectionalG * cosTheta + g2, 1.5);
    float mPhase = 0.0795775 * ((1.0 - g2) * hgDenom);
    vec3 betaMTheta = betaM * mPhase;
    
    vec3 Lin = pow(sunIntensityVal * ((betaRTheta + betaMTheta) / (betaR + betaM)) * (1.0 - Fex), vec3(1.5));
    Lin *= mix(vec3(1.0), pow(sunIntensityVal * ((betaRTheta + betaMTheta) / (betaR + betaM)) * Fex, vec3(0.5)), clamp(pow(1.0 - dot(up, uSunDir), 5.0), 0.0, 1.0));
    
    vec3 L0 = vec3(0.1) * Fex;
    if (drawSun) {
        float sundisk = smoothstep(sunAngularDiameterCos, sunAngularDiameterCos + 0.00002, cosTheta);
        L0 += (sunIntensityVal * 19000.0 * Fex) * sundisk;
    }
    L0 = pow(L0, vec3(1.0 / (1.0 + (1.0 * sunfade))));
    
    return (Lin + L0) * 0.15; // Scale factor for scene HDR range
}

void main()
{
    vec3 dir = normalize(fragDir);
    float y = dir.y;

    // Initialize scattering coefficients once per frame
    float zenithAngleCos = clamp(dot(uSunDir, up), -1.0, 1.0);
    float sunIntensityVal = sunIntensity(zenithAngleCos);
    float sunfade = 1.0 - clamp(1.0 - exp((uSunDir.y * 400000.0 / 450000.0)), 0.0, 1.0);
    
    float rayleighCoefficient = rayleigh - (1.0 * (1.0 - sunfade));
    vec3 betaR = totalRayleigh * rayleighCoefficient;
    
    float c = (0.2 * uTurbidity) * 10E-18;
    vec3 betaM = 0.434 * c * MieConst * mieCoefficient;

    // Compute sky scattering
    vec3 sky = getAtmosphericScattering(dir, betaR, betaM, sunIntensityVal, sunfade, true);

    // Below the horizon: dusky warm haze (by day)
    sky = mix(sky, vec3(0.40, 0.32, 0.25) * 0.3, clamp(-y * 4.0, 0.0, 1.0));

    // Night sky base gradient (darker zenith for a truer pitch-black feel)
    vec3 nightSkyColor = vec3(0.001, 0.002, 0.005) * 0.1; // zenith (near pitch black)
    vec3 nightHorizonColor = vec3(0.005, 0.008, 0.02) * 0.15; // horizon
    vec3 nightSky = mix(nightHorizonColor, nightSkyColor, clamp(y, 0.0, 1.0));
    
    // Smooth transition from day/sunset to night gradient
    float dayWeight = smoothstep(-0.25, 0.05, uSunDir.y);
    sky = mix(nightSky, sky, dayWeight);

    // 1. The 3D Moon is now drawn in renderer.c (we removed the fake 2D moon disc from the shader).
    
    // Moon glow / halo (still rendered procedurally to cast a wide glow across the sky)
    // Moon glow removed so we can clearly see the 3D moon model.

    // 2. Draw stars (fade in at dusk, mask out below horizon and inside clouds)
    // Fix: smoothstep requires edge0 < edge1. So we use smoothstep(-0.20, 0.0, uSunDir.y) and invert.
    float starMask = (1.0 - smoothstep(-0.20, 0.0, uSunDir.y)) * smoothstep(0.0, 0.05, y);
    float stars = getStars(dir) * starMask;
    sky += vec3(1.0, 0.96, 0.90) * stars;

    // For clouds and effects
    float sd = max(dot(dir, uSunDir), 0.0);
    float glow = 0.0;
    if (sd > 0.5) {
        glow = pow(sd, 14.0) * 0.30 + pow(sd, 150.0) * 1.3 + pow(sd, 4000.0) * 8.0;
    }

    // --- Procedural clouds (upper hemisphere) ---
    if (y > 0.005) {
        // Project onto flat cloud layer
        vec2 base = dir.xz / max(y, 0.10);

        // Coordinates for base and detail layers
        vec2 uv1 = base * 0.45 + uWindDir * uTime * 0.004;
        vec2 uv2 = base * 1.10 + uWindDir * uTime * 0.013;

        // 1. SOTA Domain Warping (Recursive coordinate distortion using fast valueNoise vector)
        vec2 q = vec2(valueNoise(uv1), valueNoise(uv1 + vec2(2.3, 4.5)));
        vec2 warpedUV1 = uv1 + q * 0.22;
        vec2 warpedUV2 = uv2 + q * 0.35;

        // Slow cumulus mass layer (5 octaves) on warped domain
        float n1 = fbm(warpedUV1);
        
        // Fast detail layer (3 octaves) on warped domain
        float n2 = fbmShadow(warpedUV2);
        
        // Combined base density
        float n = n1 * 0.65 + n2 * 0.35;

        // 2. Dramatic Cumulus Shaping: Billow top creation
        float billow = 1.0 - abs(n - 0.5) * 2.0;
        float nDramatic = mix(n, billow, 0.45);

        float density = smoothstep(0.48, 0.65, nDramatic);
        density = pow(density, 0.70);

        // Fade towards horizon (push clouds higher up to clear the horizon/sun)
        float atmo = smoothstep(0.18, 0.45, y);
        density *= atmo;

        // 3. Volumetric Self-Shadowing: offset sample towards the sun azimuth direction
        vec2 sunDirXZ = normalize(uSunDir.xz + vec2(1e-5));
        vec2 uvLight = warpedUV1 + sunDirXZ * 0.05;
        float nLight = fbmShadow(uvLight);
        
        // Beer-Lambert light attenuation + SOTA Powder Effect (Horizon Zero Dawn multiple scattering)
        float thickness = max(n1 - nLight, 0.0);
        float beer = exp(-thickness * 13.0);
        float powder = 1.0 - exp(-density * 4.0);
        float selfShadow = clamp(beer * powder * 2.2, 0.08, 1.0);

        // Volumetric cloud shading with dynamic colors sampled from the atmospheric scattering
        vec3 zenithColorScat = getAtmosphericScattering(vec3(0.0, 1.0, 0.0), betaR, betaM, sunIntensityVal, sunfade, false);
        vec3 horizonColorScat = getAtmosphericScattering(normalize(vec3(uSunDir.x, 0.01, uSunDir.z)), betaR, betaM, sunIntensityVal, sunfade, false);

        vec3 cloudBase = mix(vec3(0.02, 0.02, 0.05), zenithColorScat * 0.8, 0.6); 
        vec3 cloudTop  = mix(vec3(0.95, 0.65, 0.30), horizonColorScat * 2.5, 0.5);
        
        // Moonlight cloud shading at night (scaled by moon elevation and toned down)
        float moonElev = max(uMoonDir.y, 0.0);
        vec3 moonCloudBase = vec3(0.01, 0.012, 0.025) * 0.15 * (0.1 + 0.9 * moonElev);
        vec3 moonCloudTop = vec3(0.18, 0.22, 0.32) * (0.3 + 0.7 * selfShadow) * 0.15 * moonElev;
        
        // Blend day and night cloud colors
        vec3 currentCloudBase = mix(moonCloudBase, cloudBase, dayWeight);
        vec3 currentCloudTop  = mix(moonCloudTop, cloudTop, dayWeight);
        
        // Apply volumetric self-shadowing to the base-top color mix
        vec3 cloudCol = mix(currentCloudBase, currentCloudTop, smoothstep(0.18, 0.90, nDramatic) * selfShadow);
        
        // Silver Lining (Forward scattering edge glow)
        float edge = density * (1.0 - density);
        float silverLining = 0.0;
        if (sd > 0.5) {
            silverLining = pow(sd, 16.0) * edge * 6.0;
        }
        cloudCol += vec3(1.0, 0.94, 0.82) * silverLining * dayWeight;
        
        // Ambient washes
        float sunAz = max(dot(normalize(vec3(dir.x, 0.0, dir.z) + 1e-5),
                              normalize(vec3(uSunDir.x, 0.0, uSunDir.z))), 0.0);
        float horizonBand = 1.0 - smoothstep(0.0, 0.45, y);
        cloudCol += vec3(0.85, 0.55, 0.20) * pow(sunAz, 1.8) * horizonBand * selfShadow * dayWeight;
        cloudCol += vec3(0.25, 0.14, 0.0) * glow * dayWeight;
        
        // Blend sky edge with horizon scattering color
        cloudCol = mix(horizonColorScat, cloudCol, atmo);

        sky = mix(sky, cloudCol, density);
    }

    // Linear HDR out — tone mapping happens ONCE, in the post composite.
    finalColor = vec4(sky * uExposure, 1.0);
}
