// Brush dab shader: stamps dabs into the stroke accumulation buffer.
//
// This is the GPU twin of DabRenderer's StampRows, and it has to be the *same*
// arithmetic, not merely a similar one: a brush can land on either path (an
// active selection or an alpha-locked layer sends it to the CPU), and a mark
// whose edge, grain or silhouette changed with the path would read as a bug.
// So every step below is StampRows' step, in the same order, in the same
// units: normalised tip space, the +0.001 falloff guard, and texture taps
// done by hand with Load rather than by the sampler, because hardware bilinear
// keeps only ~8 bits of sub-texel position and BrushTexture::Sample keeps a
// float.
//
// Every dab of a batch carries its whole description in its vertices rather
// than in a constant buffer, so a stroke segment goes down in one draw call.
// The tip and grain textures are per stroke, bound once.
//
// Output is premultiplied. Dabs accumulate with ONE / INV_SRC_ALPHA, which is
// "over", and because "over" is associative, stamping every dab into a
// transparent buffer and compositing that once is exactly equal to compositing
// each dab onto the layer in turn.

Texture2D<float> g_tip   : register(t0);   // R8 coverage, clamped
Texture2D<float> g_grain : register(t1);   // R8 coverage, tiled

struct VSIn
{
    float2 pos    : POSITION;     // clip space
    float4 shape  : TEXCOORD0;    // cx, cy, radius, roundness   (layer px)
    float4 look   : TEXCOORD1;    // innerR (normalised), alpha, wetEdges, tipOn
    float4 orient : TEXCOORD2;    // cos, sin of the tip angle; flipX, flipY (+-1)
    float4 color  : TEXCOORD3;    // straight rgb; w unused
    float4 grainA : TEXCOORD4;    // grainU, grainV, 1/span, depth
    float4 grainB : TEXCOORD5;    // cos, sin of the grain angle; invert; unused
};

struct VSOut
{
    float4 pos    : SV_POSITION;
    nointerpolation float4 shape  : TEXCOORD0;
    nointerpolation float4 look   : TEXCOORD1;
    nointerpolation float4 orient : TEXCOORD2;
    nointerpolation float4 color  : TEXCOORD3;
    nointerpolation float4 grainA : TEXCOORD4;
    nointerpolation float4 grainB : TEXCOORD5;
};

VSOut BrushDabVS(VSIn v)
{
    VSOut o;
    o.pos    = float4(v.pos, 0.0f, 1.0f);
    o.shape  = v.shape;
    o.look   = v.look;
    o.orient = v.orient;
    o.color  = v.color;
    o.grainA = v.grainA;
    o.grainB = v.grainB;
    return o;
}

// BrushTexture::Sample, tap for tap. `tile` wraps (grain); otherwise the
// coordinate clamps to the edge (a tip, which must fade to nothing off it).
float SampleCoverage(Texture2D<float> tex, float u, float v, bool tile)
{
    uint w, h;
    tex.GetDimensions(w, h);
    if (w == 0 || h == 0) return 1.0f;

    if (tile) { u -= floor(u); v -= floor(v); }
    else      { u = saturate(u); v = saturate(v); }

    const float fx = u * (float)(w - 1);
    const float fy = v * (float)(h - 1);
    const int x0 = (int)fx, y0 = (int)fy;
    const int x1 = tile ? (int)((uint)(x0 + 1) % w) : min(x0 + 1, (int)w - 1);
    const int y1 = tile ? (int)((uint)(y0 + 1) % h) : min(y0 + 1, (int)h - 1);
    const float tx = fx - x0, ty = fy - y0;

    const float top = tex.Load(int3(x0, y0, 0)) * (1 - tx) + tex.Load(int3(x1, y0, 0)) * tx;
    const float bot = tex.Load(int3(x0, y1, 0)) * (1 - tx) + tex.Load(int3(x1, y1, 0)) * tx;
    return top * (1 - ty) + bot * ty;
}

float4 BrushDabPS(VSOut p) : SV_Target
{
    const float cx = p.shape.x, cy = p.shape.y;
    const float R  = p.shape.z, roundness = p.shape.w;
    const float innerR = p.look.x;
    const float range  = 1.0f - innerR;
    const float ca = p.orient.x, sa = p.orient.y;

    // SV_Position is the pixel centre, which is StampRows' px + 0.5.
    const float dx = p.pos.x - cx;
    const float dy = p.pos.y - cy;

    // Into unit tip space: rotate into the tip's frame, then squash the minor
    // axis so the ellipse becomes a unit circle.
    const float invR  = 1.0f / R;
    const float invRk = 1.0f / (R * roundness);
    const float u =  dx * ca * invR  + dy * sa * invR;
    const float v = -dx * sa * invRk + dy * ca * invRk;
    const float rr = u * u + v * v;
    if (rr >= 1.0f) discard;

    float coverage;
    if (p.look.w > 0.5f)
    {
        const float su = u * p.orient.z;
        const float sv = v * p.orient.w;
        coverage = SampleCoverage(g_tip, su * 0.5f + 0.5f, sv * 0.5f + 0.5f, false);
    }
    else
    {
        const float dist = sqrt(rr);
        if (dist <= innerR) coverage = 1.0f;
        else
        {
            const float t = saturate((dist - innerR) / (range + 0.001f));
            coverage = 1.0f - t * t * (3.0f - 2.0f * t);
        }
    }
    if (coverage <= 0.0f) discard;

    // Grain: pressed through as a reduction of coverage.
    const float depth = p.grainA.w;
    if (depth > 0.0f)
    {
        const float wx = (p.pos.x - p.grainA.x) * p.grainA.z;
        const float wy = (p.pos.y - p.grainA.y) * p.grainA.z;
        const float gu = wx * p.grainB.x - wy * p.grainB.y;
        const float gv = wx * p.grainB.y + wy * p.grainB.x;
        float g = SampleCoverage(g_grain, gu, gv, true);
        if (p.grainB.z > 0.5f) g = 1.0f - g;
        coverage *= (1.0f - depth) + depth * g;
        if (coverage <= 0.0f) discard;
    }

    // Wet edges: paint driven out to the rim of the stamp.
    const float wetEdges = p.look.z;
    if (wetEdges > 0.0f)
    {
        const float rim = 4.0f * coverage * (1.0f - coverage);
        coverage = saturate(coverage + wetEdges * rim * (1.0f - coverage));
    }

    const float a = coverage * p.look.y;
    if (a <= 0.0f) discard;

    return float4(p.color.rgb * a, a);
}
