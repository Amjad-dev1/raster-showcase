#pragma once
#include "core/PixelBuffer.h"
#include "core/Color.h"
#include "core/Rect.h"
#include <climits>
#include <cstddef>

struct BrushTexture;

enum class DabMode { Paint, Erase, Smudge };

// The colour a smudge stroke is currently carrying, in premultiplied form so a
// dab can be blended with transparent canvas without the straight-alpha divide
// blowing up. Lives for the length of one stroke; BrushEngine owns it.
//
// `p` holds as many channels as the buffer being smudged has colour channels:
// one for grey, three for RGB, four for ink. Nothing here interprets them, so
// dragging ink around works the same way dragging light does.
struct SmudgeState
{
    float p[4] = { 0.0f, 0.0f, 0.0f, 0.0f };   // premultiplied colour
    float a  = 0.0f;
    bool  primed = false;                    // false until the first dab sampled
};

// Everything one stamp needs. Built by BrushEngine from the preset and the
// pen state, so the renderer stays a pure "put this mark here" function and all
// the brush cleverness lives in one place upstream.
struct DabParams
{
    float cx = 0, cy = 0;         // centre, canvas px
    float radius = 1;             // major-axis radius, canvas px
    float roundness = 1.0f;       // minor/major, 1 = circle, ->0 = a line
    float angleRad = 0.0f;        // tip rotation
    float hardness = 0.85f;       // round-tip edge falloff, 0-1
    ColorValue color{};           // buffer's own channels; ignored when erasing
    float alpha = 1.0f;           // overall stamp opacity, 0-1
    DabMode mode = DabMode::Paint;
    bool  alphaLock = false;

    // Tip silhouette. Null = the analytic round/soft tip governed by hardness.
    const BrushTexture* tip = nullptr;
    bool  flipX = false, flipY = false;

    // Grain pressed through the tip. Null or depth<=0 = no grain.
    const BrushTexture* grain = nullptr;
    float grainDepth = 0.0f;      // 0 = ignore grain .. 1 = grain fully carves
    // Grain sample origin and span, already in canvas px: the texel at (cx,cy)
    // is (grainU, grainV) and the texture repeats every grainSpan px. BrushEngine
    // sets these so "moving" grain slides and "textured" grain pins to canvas.
    float grainU = 0.0f, grainV = 0.0f;
    float grainSpan = 128.0f;
    float grainRot = 0.0f;

    // Wet mix: the colour actually laid is lerped toward what is already on the
    // canvas under the stamp by `wetDilution`, and its alpha scaled by
    // `wetCharge`. 0 dilution / 1 charge is a dry brush.
    float wetDilution = 0.0f;
    float wetCharge   = 1.0f;
    // Softens what the dilution reads: the canvas colour is averaged over a
    // neighbourhood this many pixels across rather than taken from the pixel
    // under the nib, so a wet brush smooths what it drags instead of copying it.
    float wetBlur     = 0.0f;
    // Sample the canvas once at the stamp centre instead of per pixel. A dab
    // then deposits one mixed colour, which is how a loaded brush behaves;
    // per-pixel is how a glaze behaves.
    bool  wetPerDab   = false;

    // Grain sense. `invert` on the TextureRef flips the loaded map for every
    // user of it; this flips only how this brush reads depth, so one grain file
    // can be tooth on one brush and resist on another.
    bool  grainInvert = false;

    // Paint pools at the rim of the stamp: coverage in the falloff band is
    // pushed up, leaving a darker edge and a lighter middle. 0 is off.
    float wetEdges = 0.0f;

    // Index into the 18 composite blend modes. 0 (Normal) takes the plain
    // "over" path; anything else routes through blend::SourceOverN, the same
    // definition the layer compositor uses.
    int   blendMode = 0;

    // Glaze ceiling. When `cov` is non-null it is a layer-sized map of how much
    // alpha THIS STROKE has already laid on each pixel; a stamp may only bring
    // a pixel up to `glazeCap` and no further, however many times the stroke
    // crosses itself. Null means the stroke builds up without limit.
    uint16_t* cov      = nullptr;
    int       covW     = 0;
    int       covH     = 0;
    float     glazeCap = 1.0f;

    // The document's selection, as coverage. Null means no selection, which is
    // "paint everywhere" - the same convention Selection::CoverageAt uses, so
    // the two cannot disagree about what an absent mask means.
    //
    // It is in *document* space while the buffer being painted is a layer, so
    // the offset says where the layer's origin sits on the canvas. Multiplying
    // the stamp's coverage by it - rather than clipping to the selection's
    // bounding box - is what makes a feathered selection soften the mark
    // instead of cutting it off at a rectangle.
    const uint8_t* sel     = nullptr;
    int            selW    = 0;
    int            selH    = 0;
    int            selOffX = 0;      // layer origin on the canvas
    int            selOffY = 0;

    // Writes only land inside this rect, in buffer pixels. The default is
    // unbounded. Two users: the end taper re-renders a stroke's tail over the
    // pre-stroke pixels and must not stamp a second time into the part of a
    // dab that reaches outside what it restored; and RenderBatch hands each
    // thread one horizontal band.
    int clipL = INT_MIN, clipT = INT_MIN, clipR = INT_MAX, clipB = INT_MAX;
};

namespace DabRenderer {

// Legacy round-dab entry, kept for the GPU-ineligible simple paths and tests.
void RenderDab(PixelBuffer& buf,
               float cx, float cy, float radius,
               float hardness, const ColorValue& color, float alpha,
               DabMode mode = DabMode::Paint,
               bool alphaLock = false);

// The full stamp: elliptical/rotated, optional tip silhouette and grain, wet
// mixing. One "over" composite per colour channel, so grey, RGB and ink all
// go through the same loop.
void RenderDabEx(PixelBuffer& buf, const DabParams& d);

// Stamps `n` dabs in order and marks what they touched dirty once.
//
// Two dabs of a stroke overlap, so they cannot be drawn at the same time, but
// two *rows* never interact: every write a dab makes depends only on the pixel
// it is writing. So a large batch is cut into horizontal bands, and each thread
// stamps every dab, in order, clipped to its own band. Each pixel sees exactly
// the sequence of writes it would have seen on one thread, so the result is
// bit-identical, and a big soft brush uses every core rather than one.
//
// The exception is a wet dab that reads beyond its own pixel (one that blurs
// what it mixes with, or samples its centre once for the whole stamp), since
// those read pixels another band may be writing; a batch containing one is
// drawn on one thread. Callers do not need to know: it is decided here.
void RenderBatch(PixelBuffer& buf, const DabParams* dabs, size_t n);

// Test hook: false forces RenderBatch onto one thread, so a test can prove the
// banded result is identical to the serial one.
void SetParallel(bool enabled);

// Coverage of the round/soft tip at a pixel distance: the single definition
// the GPU brush_dab shader must match.
float DabCoverage(float dist, float innerR, float range);

void RenderSmudgeDab(PixelBuffer& buf,
                     float cx, float cy, float radius,
                     float hardness, float strength,
                     SmudgeState& state,
                     bool alphaLock = false);

} // namespace DabRenderer
