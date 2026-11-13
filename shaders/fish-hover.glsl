// Native-cell fish overlay; the vendor media cell remains untouched outside its silhouette.
#define FISH_SPINE_COUNT 20
uniform vec2 fishSpine[FISH_SPINE_COUNT];
uniform vec2 fishHeading;
uniform vec4 fishBounds;
uniform float fishLength;
uniform float fishVisible;
// Screen-right light, lifted toward the viewer to reveal the top surface.
const vec3 FISH_LIGHT = vec3(0.8, 0.0, 0.6);
// Coverage keeps the silhouette connected; it must not drive surface tone.
// Its cell-expanded distance plateaus at zero and would flatten the ramp to x/X.

// Radius in body-length units: broad shoulders taper into a fine, articulated tail.
float fishRadius(float t) {
    if (t < 0.2) return mix(0.034, 0.047, t / 0.2);
    if (t < 0.45) return mix(0.047, 0.031, (t - 0.2) / 0.25);
    if (t < 0.75) return mix(0.031, 0.013, (t - 0.45) / 0.3);
    return mix(0.013, 0.004, (t - 0.75) / 0.25);
}

// Test each cell's footprint, not just its centre: the fine tail must remain
// connected in horizontal/diagonal poses on the anisotropic ASCII grid.
// This is finite silhouette coverage, not an antialias halo or exterior dots.
float fishSegment(vec2 p, vec2 a, vec2 b, float ra, float rb,
                  out float t, out vec2 delta) {
    vec2 axis = b - a;
    float squared = dot(axis, axis);
    t = 0.0;
    delta = vec2(0.0);
    if (!(squared > 1e-14)) return -1.0;
    t = clamp(dot(p - a, axis) / squared, 0.0, 1.0);
    delta = p - mix(a, b, t);
    float radius = mix(ra, rb, t);
    vec2 halfCell = 0.5 * vec2(u_aspect, 1.0) / u_grid;
    float radial = length(max(abs(delta) - halfCell, vec2(0.0))) / radius;
    if (!(radial < 1.0)) return -1.0;
    return radial;
}

// Material normals use the actual cross-section, not the expanded coverage
// distance. Filter by the projected cell width to keep sub-cell tails stable.
float fishSurface(vec2 delta, vec2 tangent, float radius, float anatomy) {
    vec2 across = vec2(-tangent.y, tangent.x);
    vec2 halfCell = 0.5 * vec2(u_aspect, 1.0) / u_grid;
    float footprint = dot(abs(across), halfCell);
    float crossSection = clamp(dot(delta, across) /
        sqrt(radius * radius + footprint * footprint), -0.98, 0.98);
    float crown = sqrt(1.0 - crossSection * crossSection);
    // Smooth shoulder-to-tail slope: no per-capsule highlight or radius kink.
    float slope = mix(0.065, -0.05, smoothstep(0.08, 0.35, anatomy));
    vec3 surface = normalize(vec3(across * crossSection - tangent * slope * crown, crown));
    float diffuse = max(dot(surface, FISH_LIGHT), 0.0);
    float shoulder = 1.0 - smoothstep(0.18, 0.53, anatomy);
    float tail = smoothstep(0.58, 0.92, anatomy);
    float tone = 0.25 + 0.82 * diffuse + 0.07 * shoulder - 0.09 * tail;
    // The peduncle stays legible even when only a sliver intersects a cell.
    return clamp(tone, mix(0.25, 0.43, tail), 0.98);
}

// Filled swept fin. Test triangle/cell overlap so thin tips do not disappear
// between cell centres; only cells touching the finite fin receive ink.
float fishFin(vec2 p, vec2 a, vec2 b, vec2 c) {
    vec2 v = b - a, w = c - a, q = p - a;
    float determinant = v.x * w.y - v.y * w.x;
    if (!(abs(determinant) > 1e-14)) return 0.0;
    vec2 halfCell = 0.5 * vec2(u_aspect, 1.0) / u_grid;
    if (any(lessThan(p, min(a, min(b, c)) - halfCell)) ||
        any(greaterThan(p, max(a, max(b, c)) + halfCell))) return 0.0;
    float u = (q.x * w.y - q.y * w.x) / determinant;
    float t = (v.x * q.y - v.y * q.x) / determinant;
    float inverseArea = 1.0 / abs(determinant);
    float reachU = dot(abs(vec2(w.y, -w.x)), halfCell) * inverseArea;
    float reachT = dot(abs(vec2(-v.y, v.x)), halfCell) * inverseArea;
    float reachSum = dot(abs(vec2(w.y - v.y, v.x - w.x)), halfCell) * inverseArea;
    if (u < -reachU || t < -reachT || u + t > 1.0 + reachSum) return 0.0;
    // A shallow, curved sheet shares the body's fixed screen-right light.
    // Root-to-tip curvature provides broad tonal structure, not stippling.
    vec2 slopeNormal = vec2(w.y, -w.x) * sign(determinant);
    slopeNormal /= max(length(slopeNormal), 1e-7);
    float span = clamp(u, 0.0, 1.0);
    vec3 surface = normalize(vec3(slopeNormal * mix(0.25, 0.95, span), 0.8));
    float diffuse = max(dot(surface, FISH_LIGHT), 0.0);
    return clamp(0.28 + 0.65 * diffuse - 0.08 * span, 0.34, 0.94);
}

vec4 cell(vec2 uv) {
    vec4 base = sourceMediaCell(uv);
    if (!(fishVisible > 0.0) || !(fishLength > 0.0)) return base;
    vec2 p = vec2(uv.x * u_aspect, uv.y);
    vec2 halfCell = 0.5 * vec2(u_aspect, 1.0) / u_grid;
    // Bounds include the cells intersecting the finite anatomical silhouette.
    if (!all(greaterThanEqual(p, fishBounds.xy - halfCell)) ||
        !all(lessThanEqual(p, fishBounds.zw + halfCell))) return base;
    // The nose pivots independently; trailing world-space joints must not be
    // rotated or mirrored just to manufacture a new head-facing direction.
    vec2 heading = -fishHeading;
    float headingSquared = dot(heading, heading);
    if (!(headingSquared > 1e-14)) return base;
    heading *= inversesqrt(headingSquared);
    vec2 normal = vec2(-heading.y, heading.x);
    float scale = fishLength;
    float density = 0.0;

    // Keep the exact union of footprint tests, but shade one nearest anatomical
    // sample. Interpolated joint tangents prevent a string of capsule highlights.
    float nearest = 1e20;
    vec2 bodyDelta = vec2(0.0), bodyTangent = heading;
    float bodyRadius = scale * fishRadius(0.0), bodyAnatomy = 0.0;
    for (int i = 0; i < FISH_SPINE_COUNT - 1; ++i) {
        float a = float(i) / float(FISH_SPINE_COUNT - 1);
        float b = float(i + 1) / float(FISH_SPINE_COUNT - 1);
        float ra = scale * fishRadius(a), rb = scale * fishRadius(b);
        vec2 start = fishSpine[i], end = fishSpine[i + 1];
        float radius = max(ra, rb);
        if (any(lessThan(p, min(start, end) - vec2(radius) - halfCell)) ||
            any(greaterThan(p, max(start, end) + vec2(radius) + halfCell))) continue;
        float t;
        vec2 delta;
        if (fishSegment(p, start, end, ra, rb, t, delta) < 0.0) continue;
        float sampledRadius = mix(ra, rb, t);
        float distanceSquared = dot(delta, delta) / (sampledRadius * sampledRadius);
        if (distanceSquared < nearest) {
            nearest = distanceSquared;
            bodyDelta = delta;
            bodyRadius = sampledRadius;
            bodyAnatomy = mix(a, b, t);
            vec2 before = fishSpine[i > 0 ? i - 1 : 0];
            vec2 after = fishSpine[i + 2 < FISH_SPINE_COUNT ? i + 2 : FISH_SPINE_COUNT - 1];
            vec2 tangentA = end - before, tangentB = after - start;
            tangentA /= max(length(tangentA), 1e-7);
            tangentB /= max(length(tangentB), 1e-7);
            vec2 tangent = mix(tangentA, tangentB, t);
            bodyTangent = tangent / max(length(tangent), 1e-7);
        }
    }
    if (nearest < 1e20)
        density = fishSurface(bodyDelta, bodyTangent, bodyRadius, bodyAnatomy);

    // Gently rear-swept hammer: two wings join the nose, not a circular pointer brush.
    vec2 head = fishSpine[0];
    for (int side = -1; side <= 1; side += 2) {
        vec2 tip = head + scale * (heading * 0.028 + normal * (float(side) * 0.16));
        float t;
        vec2 delta;
        if (fishSegment(p, head, tip, scale * 0.03, scale * 0.022, t, delta) >= 0.0) {
            float radius = scale * mix(0.03, 0.022, t);
            // Rounded hammer tips retain their end-cap normals as well.
            vec2 halfWidth = sqrt(vec2(radius * radius) + halfCell * halfCell);
            vec2 xy = delta / halfWidth;
            xy /= max(1.0, length(xy) / 0.98);
            vec3 surface = vec3(xy, sqrt(max(0.0, 1.0 - dot(xy, xy))));
            float diffuse = max(dot(surface, FISH_LIGHT), 0.0);
            density = max(density, clamp(0.25 + 0.76 * diffuse, 0.25, 0.98));
        }
    }

    // Broad swept pectoral fins remain readable in the rectangular ASCII grid.
    // Their tips fit within the existing 0.035 screen-height pose bounds.
    vec2 shoulder = fishSpine[5];
    vec2 finHeading = fishSpine[6] - fishSpine[4];
    float finSquared = dot(finHeading, finHeading);
    if (finSquared > 1e-14) {
        finHeading *= inversesqrt(finSquared);
        vec2 finNormal = vec2(-finHeading.y, finHeading.x);
        for (int side = -1; side <= 1; side += 2) {
            vec2 outward = finNormal * float(side);
            vec2 root = shoulder + scale * (outward * 0.018 - finHeading * 0.045);
            vec2 tip = shoulder + scale * (outward * 0.17 + finHeading * 0.09);
            vec2 rear = shoulder + scale * (outward * 0.018 + finHeading * 0.14);
            density = max(density, fishFin(p, root, tip, rear));
        }
    }
    if (!(density > 0.0)) return base;
    // Alpha is the engine's native charset density, not opacity or a second glyph pass.
    vec3 ink = mix(tint, tint2, clamp((density - 0.24) / 0.72, 0.0, 1.0));
    return vec4(ink, density);
}
