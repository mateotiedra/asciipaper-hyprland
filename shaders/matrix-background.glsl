// defaults: {"matrixBrightness": 0.5, "matrixDensity": 1.35, "matrixSpeed": 2, "matrixTrailLength": 5, "matrixChangeInterval": 2.4}
// Sparse native-cell rain behind the video and the unchanged pointer fish.
// Alpha selects a glyph; fade its ink toward the wallpaper color instead of
// cycling through density levels. Stable columns and slow glyph epochs avoid
// the unrelated flashing speckles of time-hashed particle noise.
// The media loader supplies this existing setting to both CPU playback and
// shader uniforms; the vendor media shader itself does not declare it.
uniform float speed;
// Live-tunable in the wallpaper JSON's "uniforms"; the header supplies defaults.
uniform float matrixBrightness;     // Ink/background blend, 0..1; 0 hides rain.
uniform float matrixDensity;        // Stream-count multiplier; 1 = original sparse rain, 0 = off.
uniform float matrixSpeed;          // Rows per video-clock second; 0 stops descent.
uniform float matrixTrailLength;    // Typical trail length in cells, with +/-1 cell variation.
uniform float matrixChangeInterval; // Typical video-clock seconds between mutations; 0 disables them.
// The current Terminal Pink wallpaper's #101014 background. This is only the
// fade destination, not a new canvas/background fill.
const vec3 MATRIX_BACKGROUND = vec3(16.0, 16.0, 20.0) / 255.0;

float matrixSeed(float n) {
    return fract(sin(n * 127.1 + 311.7) * 43758.5453);
}

vec4 matrixRainCell(vec2 uv) {
    float brightness = clamp(matrixBrightness, 0.0, 1.0);
    if (!(matrixDensity > 0.0) || !(brightness > 0.0)) return vec4(0.0);
    float spacing = max(4.0, floor(24.0 / matrixDensity + 0.5));
    vec2 at = floor(uv * u_grid);
    float lane = floor(at.x / spacing);
    float seed = matrixSeed(lane + 0.5);
    // Scale the lane margins with density, keeping trails thin and separated.
    float margin = max(1.0, floor(spacing / 6.0));
    float column = lane * spacing + margin + floor(seed * (spacing - 2.0 * margin));
    if (at.x != column) return vec4(0.0);

    float trail = max(1.0, floor(matrixTrailLength + 0.5) - 1.0 + floor(matrixSeed(lane + 19.0) * 3.0));
    float gap = 12.0 + floor(matrixSeed(lane + 31.0) * 12.0);
    float rate = max(matrixSpeed, 0.0) * (0.85 + 0.3 * matrixSeed(lane + 47.0));
    // spec.c samples media at scene time * speed. Reusing that exact clock
    // preserves the relative pace at both active and idle render rates.
    float clock = u_time * speed;
    float period = u_grid.y + trail + gap;
    float travel = clock * rate + matrixSeed(lane + 61.0) * period;
    float cycle = floor(travel / period);
    float head = mod(travel, period) - 1.0;
    float age = head - (at.y + 0.5);
    if (age <= -0.55 || age >= trail) return vec4(0.0);

    float entering = smoothstep(-0.55, 0.15, age);
    float history = pow(max(0.0, 1.0 - max(age, 0.0) / trail), 1.6);
    float strength = entering * history;
    if (strength < 0.015) return vec4(0.0);

    // Existing charset only: ': ; i + = x'. Each row keeps its glyph in the
    // trail, with staggered, infrequent mutations rather than per-frame noise.
    float identity = lane * 97.0 + at.y * 7.0 + cycle * 433.0;
    float epoch = 0.0;
    if (matrixChangeInterval > 0.0) {
        float changePeriod = matrixChangeInterval * (0.75 + 0.5 * matrixSeed(identity + 13.0));
        epoch = floor(clock / changePeriod + matrixSeed(identity + 29.0));
    }
    float glyph = 3.0 + floor(matrixSeed(identity + epoch * 53.0) * 6.0);
    // The approved 12-character atlas is unchanged: index / (12 - 1).
    float density = glyph / 11.0;
    vec3 color = mix(tint, tint2, 0.2 + 0.65 * seed);
    vec3 ink = mix(MATRIX_BACKGROUND, color, brightness * strength);
    return vec4(ink, density);
}

vec4 sourceMediaCell(vec2 uv) {
    vec4 video = rawMediaCell(uv);
    // Preserve every visible shark cell exactly. The approved fish wrapper
    // is composed last and independently covers either the video or the rain.
    if (video.a > 0.0) return video;
    vec4 rain = matrixRainCell(uv);
    return rain.a > 0.0 ? rain : video;
}
