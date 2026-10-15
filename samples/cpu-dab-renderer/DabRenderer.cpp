#include "DabRenderer.h"
#include "BrushTexture.h"
#include "core/BlendModes.h"
#include "util/Math.h"
#include "util/Logger.h"
#include <cmath>
#include <algorithm>
#include <atomic>
#include <execution>
#include <thread>

namespace DabRenderer {

// Falloff shared by the paint and smudge dabs: flat out to innerR, smoothstep
// to zero at the rim.
float DabCoverage(float dist, float innerR, float range)
{
    if (dist <= innerR) return 1.0f;
    float t = rmath::clamp01((dist - innerR) / (range + 0.001f));
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

// ---------------------------------------------------------------------------
//  The full stamp
// ---------------------------------------------------------------------------
namespace {

std::atomic<bool> g_parallel{ true };

// The pixels a dab can write: its bounding box, the buffer and its clip, as
// an inclusive range. False when that is empty or the dab lays nothing.
bool DabBox(const PixelBuffer& buf, const DabParams& d,
            int& x0, int& y0, int& x1, int& y1)
{
    if (d.radius < 0.5f || d.alpha <= 0.0f) return false;

    // The rotated ellipse always fits inside the major-radius circle, so bound
    // by that and let the per-pixel test carve the true shape.
    const float R = d.radius;
    x0 = std::max({ 0, d.clipL, (int)std::floor(d.cx - R) - 1 });
    y0 = std::max({ 0, d.clipT, (int)std::floor(d.cy - R) - 1 });
    x1 = std::min({ buf.Width()  - 1, d.clipR - 1, (int)std::ceil(d.cx + R) + 1 });
    y1 = std::min({ buf.Height() - 1, d.clipB - 1, (int)std::ceil(d.cy + R) + 1 });
    return x1 >= x0 && y1 >= y0;
}

// Stamps `d` into rows [rowLo, rowHi) only. Everything a write depends on is
// the pixel being written (except wet mixing, which RenderBatch keeps on one
// thread), so two calls on disjoint row ranges can run at the same time.
// Does not mark the buffer dirty; the caller does that once.
void StampRows(PixelBuffer& buf, const DabParams& d, int rowLo, int rowHi)
{
    int x0, y0, x1, y1;
    if (!DabBox(buf, d, x0, y0, x1, y1)) return;
    y0 = std::max(y0, rowLo);
    y1 = std::min(y1, rowHi - 1);
    if (y1 < y0) return;

    const float R = d.radius;
    const float roundness = std::max(0.02f, d.roundness);
    const float ca = std::cos(d.angleRad), sa = std::sin(d.angleRad);
    const float innerR = rmath::clamp01(d.hardness);   // in normalised [0,1]
    const float range  = 1.0f - innerR;
    const float invR   = 1.0f / R;
    const float invRk  = 1.0f / (R * roundness);

    // The ellipse test u*u + v*v < 1 is a quadratic in dx for a fixed row,
    // A dx^2 + B dx + C < 1, so each row's covered run can be solved for once
    // instead of discovered by testing every pixel of the bounding box. The
    // per-pixel test is kept as the authority; this only skips what it would
    // have rejected anyway.
    const float iR2 = invR * invR, iRk2 = invRk * invRk;
    const float qA = ca * ca * iR2 + sa * sa * iRk2;
    const float qB = 2.0f * ca * sa * (iR2 - iRk2);
    const float qC = sa * sa * iR2 + ca * ca * iRk2;

    const int nch = buf.Channels();
    const int ncc = nch - 1;

    // Loaded colour, straight 0-1.
    float src[4] = { 0,0,0,0 };
    for (int c = 0; c < ncc; ++c) src[c] = d.color.ch[c] * (1.0f / 65535.0f);

    // Decided once per stamp rather than once per pixel.
    const bool useTip   = d.tip && d.tip->Valid();
    const bool useGrain = d.grain && d.grain->Valid() && d.grainDepth > 0.0f;
    const bool useCov   = d.cov && d.glazeCap < 1.0f;
    const bool wet = d.wetDilution > 0.001f && d.mode == DabMode::Paint;

    const float gcr = std::cos(d.grainRot), gsr = std::sin(d.grainRot);
    const float invSpan = d.grainSpan > 0.0f ? 1.0f / d.grainSpan : 0.0f;

    // Reads the canvas under a point, optionally averaged over a neighbourhood,
    // as straight alpha. This is what wet mixing dilutes toward; blurring the
    // reading is what makes a wet brush smooth what it drags instead of
    // stencilling a copy of it.
    const int blurR = (int)(d.wetBlur * std::min(R, 24.0f));
    auto readCanvas = [&](int px, int py, float* col, float& outA)
    {
        if (blurR <= 0)
        {
            const uint16_t* q = buf.Row(py) + px * nch;
            outA = q[0] * (1.0f / 65535.0f);
            for (int c = 0; c < ncc; ++c) col[c] = q[c + 1] * (1.0f / 65535.0f);
            return;
        }
        float acc[4] = { 0,0,0,0 }, accA = 0.0f;
        int n = 0;
        const int step = std::max(1, blurR / 3);
        for (int yy = py - blurR; yy <= py + blurR; yy += step)
        {
            if (yy < 0 || yy >= buf.Height()) continue;
            const uint16_t* row2 = buf.Row(yy);
            for (int xx = px - blurR; xx <= px + blurR; xx += step)
            {
                if (xx < 0 || xx >= buf.Width()) continue;
                const uint16_t* q = row2 + xx * nch;
                accA += q[0] * (1.0f / 65535.0f);
                for (int c = 0; c < ncc; ++c) acc[c] += q[c + 1] * (1.0f / 65535.0f);
                ++n;
            }
        }
        if (n == 0) { outA = 0.0f; for (int c = 0; c < ncc; ++c) col[c] = 0.0f; return; }
        const float inv = 1.0f / n;
        outA = accA * inv;
        for (int c = 0; c < ncc; ++c) col[c] = acc[c] * inv;
    };

    // "Wet through the dab": one reading of the canvas for the whole stamp,
    // taken at its centre. The nib loads once and lays that mixture down, which
    // is why it smears; per-pixel reading is what glazes.
    float dabCol[4] = { 0,0,0,0 }; float dabA = 0.0f;
    if (wet && d.wetPerDab)
    {
        const int mx = std::min(buf.Width()  - 1, std::max(0, (int)d.cx));
        const int my = std::min(buf.Height() - 1, std::max(0, (int)d.cy));
        readCanvas(mx, my, dabCol, dabA);
    }

    for (int py = y0; py <= y1; ++py)
    {
        const float dy = py + 0.5f - d.cy;

        // This row's run, from the quadratic. One pixel of slack each side
        // absorbs float error; the exact test below still decides.
        const float b = qB * dy, c0 = qC * dy * dy - 1.0f;
        const float disc = b * b - 4.0f * qA * c0;
        if (disc <= 0.0f) continue;
        const float sq = std::sqrt(disc), inv2A = 0.5f / qA;
        const int rx0 = std::max(x0, (int)std::floor(d.cx + (-b - sq) * inv2A - 0.5f) - 1);
        const int rx1 = std::min(x1, (int)std::ceil (d.cx + (-b + sq) * inv2A - 0.5f) + 1);

        uint16_t* row = buf.Row(py);
        const float dyu = dy * sa * invR, dyv = dy * ca * invRk;

        for (int px = rx0; px <= rx1; ++px)
        {
            const float dx = px + 0.5f - d.cx;

            // Into unit tip space: rotate into the tip's frame, then squash the
            // minor axis so the ellipse becomes a unit circle.
            const float u =  dx * ca * invR  + dyu;
            const float v = -dx * sa * invRk + dyv;
            const float rr = u * u + v * v;
            if (rr >= 1.0f) continue;

            float coverage;
            if (useTip)
            {
                // Sample the silhouette. Tip space is [-1,1]; flip folds it.
                float su = d.flipX ? -u : u;
                float sv = d.flipY ? -v : v;
                coverage = d.tip->Sample(su * 0.5f + 0.5f, sv * 0.5f + 0.5f, false);
            }
            else
            {
                const float dist = std::sqrt(rr);          // 0..1
                coverage = DabCoverage(dist, innerR, range);
            }
            if (coverage <= 0.0f) continue;

            // Grain: pressed through as a reduction of coverage.
            if (useGrain)
            {
                float wx = (px + 0.5f - d.grainU) * invSpan;
                float wy = (py + 0.5f - d.grainV) * invSpan;
                float gu = wx * gcr - wy * gsr;
                float gv = wx * gsr + wy * gcr;
                float g = d.grain->Sample(gu, gv, true);
                if (d.grainInvert) g = 1.0f - g;
                coverage *= (1.0f - d.grainDepth) + d.grainDepth * g;
                if (coverage <= 0.0f) continue;
            }

            // Wet edges: paint driven out to the rim. The bump peaks in the
            // falloff band (coverage near a half) and vanishes at both the
            // solid core and the vanishing outside, so the mark keeps its
            // silhouette and only redistributes paint inside it.
            if (d.wetEdges > 0.0f)
            {
                const float rim = 4.0f * coverage * (1.0f - coverage);
                coverage = rmath::clamp01(coverage + d.wetEdges * rim * (1.0f - coverage));
            }

            float srcA = coverage * d.alpha;
            if (srcA <= 0.0f) continue;

            // Clip to the selection. Multiplied rather than tested, so a
            // feathered edge feathers the mark; a pixel outside the mask gets a
            // coverage of zero and is skipped like any other empty one.
            if (d.sel)
            {
                const int sx = px + d.selOffX;
                const int sy = py + d.selOffY;
                if (sx < 0 || sy < 0 || sx >= d.selW || sy >= d.selH) continue;
                const float cov = d.sel[(size_t)sy * d.selW + sx] * (1.0f / 255.0f);
                if (cov <= 0.0f) continue;
                srcA *= cov;
            }

            uint16_t* p = row + px * nch;

            if (d.alphaLock)
            {
                if (p[0] == 0) continue;
                srcA *= p[0] * (1.0f / 65535.0f);
            }

            // Glaze ceiling. The stroke's own coverage map records how much
            // alpha THIS pass has already laid on each pixel, and a stamp may
            // only close the gap up to the cap. Without it a soft brush that
            // crosses its own path doubles up, and the stroke reads as a chain
            // of blobs rather than one even mark.
            if (useCov && px < d.covW && py < d.covH)
            {
                uint16_t& acc = d.cov[(size_t)py * d.covW + px];
                const float have = acc * (1.0f / 65535.0f);
                if (have >= d.glazeCap) continue;

                // The map has to accumulate the way the layer does, or it does
                // not track the stroke it is capping: coverage compounds as
                // have + s(1-have), not as a running sum. Summing reached the
                // ceiling far too early and cut a low-flow brush short.
                // Solving that for the largest s that keeps the total at or
                // under the cap gives the room left for this stamp.
                const float room = (d.glazeCap - have) / std::max(1e-4f, 1.0f - have);
                if (srcA > room) srcA = room;
                acc = (uint16_t)(rmath::clamp01(have + srcA * (1.0f - have)) * 65535.0f);
            }

            if (d.mode == DabMode::Paint)
            {
                float lay[4];
                float layA = srcA;
                if (wet)
                {
                    // Blend the loaded colour toward the canvas underneath, and
                    // thin the deposit by the missing charge.
                    float dcol[4] = { 0,0,0,0 }; float wetA = 0.0f;
                    if (d.wetPerDab) { for (int c = 0; c < ncc; ++c) dcol[c] = dabCol[c]; wetA = dabA; }
                    else             readCanvas(px, py, dcol, wetA);

                    for (int c = 0; c < ncc; ++c)
                        lay[c] = src[c] + (dcol[c] - src[c]) * d.wetDilution;
                    layA = srcA * (d.wetCharge + (wetA * d.wetDilution) * (1.0f - d.wetCharge));
                }
                else
                {
                    for (int c = 0; c < ncc; ++c) lay[c] = src[c];
                }

                const float dstA = p[0] * (1.0f / 65535.0f);

                if (d.blendMode != 0)
                {
                    // Anything but Normal routes through the one shared blend
                    // definition, so a dab set to Multiply means exactly what a
                    // Multiply layer means.
                    float bc[4] = { 0,0,0,0 }, oc[4] = { 0,0,0,0 }, oa = 0.0f;
                    for (int c = 0; c < ncc; ++c) bc[c] = p[c + 1] * (1.0f / 65535.0f);
                    blend::SourceOverN((BlendMode)d.blendMode, ncc, bc, dstA, lay, layA, oc, oa);
                    for (int c = 0; c < ncc; ++c)
                        p[c + 1] = (uint16_t)(rmath::clamp01(oc[c]) * 65535.0f);
                    p[0] = (uint16_t)(rmath::clamp01(oa) * 65535.0f);
                }
                else
                {
                    const float outA = layA + dstA * (1.0f - layA);
                    if (outA > 0.0f)
                    {
                        const float ss = layA / outA;
                        const float ds = dstA * (1.0f - layA) / outA;
                        for (int c = 0; c < ncc; ++c)
                            p[c + 1] = (uint16_t)(rmath::clamp01(
                                lay[c] * ss + (p[c + 1] * (1.0f / 65535.0f)) * ds) * 65535.0f);
                    }
                    p[0] = (uint16_t)(rmath::clamp01(outA) * 65535.0f);
                }
            }
            else // Erase
            {
                float dstA = p[0] * (1.0f / 65535.0f);
                p[0] = (uint16_t)(rmath::clamp01(dstA * (1.0f - srcA)) * 65535.0f);
            }
        }
    }
}

} // namespace

void SetParallel(bool enabled) { g_parallel.store(enabled); }

void RenderDabEx(PixelBuffer& buf, const DabParams& d)
{
    if (!buf.IsValid()) return;
    int x0, y0, x1, y1;
    if (!DabBox(buf, d, x0, y0, x1, y1)) return;
    StampRows(buf, d, y0, y1 + 1);
    buf.MarkDirty(RectI{ x0, y0, x1 + 1, y1 + 1 });
}

void RenderBatch(PixelBuffer& buf, const DabParams* dabs, size_t n)
{
    if (!buf.IsValid() || n == 0) return;

    RectI box{ 0, 0, 0, 0 };
    bool  have = false, crossRow = false;
    double area = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        int x0, y0, x1, y1;
        if (!DabBox(buf, dabs[i], x0, y0, x1, y1)) continue;
        const RectI r{ x0, y0, x1 + 1, y1 + 1 };
        box  = have ? box.Union(r) : r;
        have = true;
        area += (double)r.Width() * r.Height();
        // A wet dab that reads only the pixel it writes is as band-safe as a
        // dry one. One that samples its centre once, or blurs what it reads,
        // reads pixels another band may be writing.
        const DabParams& d = dabs[i];
        if (d.wetDilution > 0.001f && d.mode == DabMode::Paint &&
            (d.wetPerDab || (int)(d.wetBlur * std::min(d.radius, 24.0f)) > 0))
            crossRow = true;
    }
    if (!have) return;

    // Below this many pixels of stamping the hand-off to the pool costs more
    // than it saves. A 60px soft dab is ~3,600; a batch is every dab one
    // pointer event placed, often only two or three. Measured with --bench on
    // a 6-core laptop: 16k left a pen-rate airbrush on one thread, 2k cut it
    // by two thirds and slowed nothing small.
    constexpr double kMinParallelArea = 2048.0;
    constexpr int    kMinBandRows     = 8;

    static const int hw = (int)std::max(1u, std::thread::hardware_concurrency());
    const int rows  = box.Height();
    const int bands = std::min({ hw * 2, rows / kMinBandRows, 64 });

    if (!g_parallel.load(std::memory_order_relaxed) || crossRow || hw < 2 ||
        bands < 2 || area < kMinParallelArea)
    {
        for (size_t i = 0; i < n; ++i)
            StampRows(buf, dabs[i], box.top, box.bottom);
    }
    else
    {
        int idx[64];
        for (int b = 0; b < bands; ++b) idx[b] = b;
        std::for_each(std::execution::par, idx, idx + bands, [&](int b)
        {
            const int lo = box.top + (int)((long long)rows * b / bands);
            const int hi = box.top + (int)((long long)rows * (b + 1) / bands);
            for (size_t i = 0; i < n; ++i)
                StampRows(buf, dabs[i], lo, hi);
        });
    }

    buf.MarkDirty(box);
}

// ---------------------------------------------------------------------------
//  Legacy round dab: thin wrapper so old callers and tests are unchanged.
// ---------------------------------------------------------------------------
void RenderDab(PixelBuffer& buf,
               float cx, float cy, float radius,
               float hardness, const ColorValue& color, float alpha,
               DabMode mode, bool alphaLock)
{
    DabParams d;
    d.cx = cx; d.cy = cy; d.radius = radius;
    d.hardness = hardness; d.color = color; d.alpha = alpha;
    d.mode = mode; d.alphaLock = alphaLock;
    RenderDabEx(buf, d);
}

// ---------------------------------------------------------------------------
//  Smudge.
// ---------------------------------------------------------------------------
void RenderSmudgeDab(PixelBuffer& buf,
                     float cx, float cy, float radius,
                     float hardness, float strength,
                     SmudgeState& state,
                     bool alphaLock)
{
    if (radius < 0.5f || strength <= 0.0f || !buf.IsValid())
        return;

    int x0 = std::max(0,                (int)(cx - radius - 1));
    int y0 = std::max(0,                (int)(cy - radius - 1));
    int x1 = std::min(buf.Width()  - 1, (int)(cx + radius + 1));
    int y1 = std::min(buf.Height() - 1, (int)(cy + radius + 1));
    if (x1 < x0 || y1 < y0) return;

    strength = rmath::clamp01(strength);

    const float innerR = radius * rmath::clamp01(hardness);
    const float range  = radius - innerR;

    const int nch = buf.Channels();
    const int ncc = nch - 1;

    float sumW = 0.0f, sumA = 0.0f, sum[4] = { 0, 0, 0, 0 };

    for (int py = y0; py <= y1; ++py)
    {
        const uint16_t* row = buf.Row(py);
        const float dy  = py + 0.5f - cy;
        const float dy2 = dy * dy;

        for (int px = x0; px <= x1; ++px)
        {
            const float dx   = px + 0.5f - cx;
            const float dist = sqrtf(dx * dx + dy2);
            if (dist >= radius) continue;

            const float w = DabCoverage(dist, innerR, range);
            if (w <= 0.0f) continue;

            const uint16_t* p = row + px * nch;
            const float a = p[0] * (1.0f / 65535.0f);

            sumW += w;
            sumA += w * a;
            for (int c = 0; c < ncc; ++c)
                sum[c] += w * a * (p[c + 1] * (1.0f / 65535.0f));
        }
    }

    if (sumW <= 0.0f) return;

    const float invW = 1.0f / sumW;
    const float smpA = sumA * invW;
    float smp[4] = { 0, 0, 0, 0 };
    for (int c = 0; c < ncc; ++c) smp[c] = sum[c] * invW;

    if (!state.primed)
    {
        for (int c = 0; c < ncc; ++c) state.p[c] = smp[c];
        state.a  = smpA; state.primed = true;
        return;
    }

    for (int py = y0; py <= y1; ++py)
    {
        uint16_t* row = buf.Row(py);
        const float dy  = py + 0.5f - cy;
        const float dy2 = dy * dy;

        for (int px = x0; px <= x1; ++px)
        {
            const float dx   = px + 0.5f - cx;
            const float dist = sqrtf(dx * dx + dy2);
            if (dist >= radius) continue;

            const float t = DabCoverage(dist, innerR, range) * strength;
            if (t <= 0.0f) continue;

            uint16_t* p = row + px * nch;
            const float dstA = p[0] * (1.0f / 65535.0f);

            if (alphaLock && dstA <= 0.0f) continue;

            const float na = alphaLock ? dstA : dstA + (state.a - dstA) * t;
            const float inv = (na > 0.0001f) ? 1.0f / na : 0.0f;

            for (int c = 0; c < ncc; ++c)
            {
                const float dc = dstA * (p[c + 1] * (1.0f / 65535.0f));
                const float ncv = dc + (state.p[c] - dc) * t;
                if (inv != 0.0f)
                    p[c + 1] = (uint16_t)(rmath::clamp01(ncv * inv) * 65535.0f);
            }
            p[0] = (uint16_t)(rmath::clamp01(na) * 65535.0f);
        }
    }

    const float pickup = std::max(0.06f, 1.0f - strength);
    for (int c = 0; c < ncc; ++c)
        state.p[c] += (smp[c] - state.p[c]) * pickup;
    state.a  += (smpA - state.a ) * pickup;

    buf.MarkDirty(RectI{x0, y0, x1 + 1, y1 + 1});
}

} // namespace DabRenderer
