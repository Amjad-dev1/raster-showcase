#pragma once
#include "Color.h"
#include <cstdint>

// Colour theory, as a painter uses it rather than as a colour scientist states
// it. Everything here is hue geometry on a wheel plus the three ways a hue is
// modified with white, black and grey; none of it is colour management, and
// none of it needs a profile. Conversions into the document's mode happen
// afterwards, in the picker, through the usual bridge.
//
// The one non-obvious thing in this file is that *there are two wheels*, and
// which one is in force changes every answer.
namespace harmony {

// ---------------------------------------------------------------- the wheels
//
// RGB/HSV is the wheel every digital tool draws: hue is an angle around the
// additive primaries, and the complement of a colour is the hue 180 degrees
// away in that space. Red's complement is cyan.
//
// RYB is Itten's wheel, the one taught in every painting class and printed on
// every colour-mixing chart. Its primaries are red, yellow and blue, because
// those are the pigments you can mix from. Red's complement is green, blue's is
// orange, yellow's is purple.
//
// These disagree, and neither is wrong; they answer different questions. A
// painter reaching for "the complement of this blue" means orange. A screen
// designer means yellow. Offering only the HSV wheel, which is what almost
// every digital app does, quietly tells painters their training is wrong.
enum class Wheel : int
{
    HSV = 0,   // additive / digital: red-green-blue
    RYB,       // subtractive / artist's: red-yellow-blue
    Count
};

const wchar_t* WheelName(Wheel w);
const wchar_t* WheelBlurb(Wheel w);

// Angle on the chosen wheel -> HSV hue angle, and back. Both take and return
// degrees in [0,360). For Wheel::HSV both are the identity.
//
// The RYB mapping is the standard 24-point piecewise-linear remap: the artist's
// wheel spends a third of its circumference on the red-orange-yellow arc, where
// HSV spends a sixth, so the two run at different rates and a fixed offset
// cannot express it.
float ToHsvHue  (float wheelHue, Wheel w);
float FromHsvHue(float hsvHue,   Wheel w);

// ------------------------------------------------------------------ schemes
enum class Scheme : int
{
    None = 0,
    Monochromatic,
    Analogous,
    Complementary,
    SplitComplementary,
    Triadic,
    Tetradic,          // rectangle: two complementary pairs, unequal spacing
    Square,            // four hues at 90 degrees
    AccentedAnalogous, // an analogous run plus the complement of its centre
    Count
};

const wchar_t* SchemeName (Scheme s);   // for a chip
const wchar_t* SchemeShort(Scheme s);   // 3-5 characters, for a narrow chip

// One sentence on what the scheme is for and how it goes wrong. Shown under the
// chips, because a name alone teaches nothing and the whole point of this
// section is that it should teach.
const wchar_t* SchemeAdvice(Scheme s);

// Whether `spread` means anything for this scheme, and the range it may take.
bool  UsesSpread(Scheme s);
float SpreadMin (Scheme s);
float SpreadMax (Scheme s);
float SpreadDefault(Scheme s);

// ------------------------------------------------------------------- results
struct Swatch
{
    float h = 0.0f;   // HSV hue, degrees
    float s = 0.0f;
    float v = 0.0f;
    bool  isBase = false;
};

struct Set
{
    // Twelve because a value scale wants nine or eleven steps. Hue schemes
    // never exceed six.
    static constexpr int kMax = 12;
    Swatch c[kMax];
    int    count = 0;
};

// Build the scheme's colours from a base. `baseHue` is an *HSV* hue (the
// caller keeps the fore colour, not a wheel position) and the geometry is done
// on `w`'s wheel and mapped back.
//
// Saturation and value are carried through unchanged except where the scheme is
// defined in terms of them: Monochromatic is exactly that case, and is the only
// one that moves them.
Set Build(Scheme s, float baseHue, float sat, float val, Wheel w, float spreadDeg);

// ---------------------------------------------------- tints, tones and shades
//
// The other half of colour theory, and the half digital pickers usually drop.
// A hue on its own is not a palette; what makes one is what you do to it with
// white, black and grey.
//
//   Tint:  hue + white. Lighter and *less* saturated. Pastels, air, distance.
//   Shade: hue + black. Darker and, on a real pigment, duller.
//   Tone:  hue + grey. Same lightness, less chroma. The most useful and the
//           least used: almost nothing in observed life is at full chroma, and
//           toning is what stops a painting looking like a cartoon.
enum class Variation : int { None = 0, Tints, Tones, Shades, Count };

const wchar_t* VariationName(Variation v);
const wchar_t* VariationAdvice(Variation v);

// `steps` samples of the variation, from the base colour outward. The base
// itself is index 0, so a row always begins with what you already had.
Set BuildVariation(Variation v, float hue, float sat, float val, int steps);

// ------------------------------------------------------------ value study
//
// A grayscale document has no hue, so none of the above applies to it: a triad
// of greys is three greys, and asking a monochrome painter to choose a wheel
// model is asking a question with no answer. What a monochrome painter actually
// works with is *value structure*, and that has its own body of practice.
//
// Two ideas carry most of it:
//
//   A value scale: a limited, evenly spaced set of tones, chosen in advance
//   and stuck to. Working from a scale rather than from the whole continuum is
//   what keeps a monochrome study from turning to mud, and it is why every
//   drawing course hands out a printed one.
//
//   Notan: the Japanese term for massing a composition into a small number of
//   flat values, usually two or three. It is a compositional test, not a
//   rendering technique: if the two-value version does not read, no amount of
//   modelling will save it.
//
// The keys are the third: which band of the scale a picture lives in. High key
// is light and airy, low key is dark and dramatic, mid key is neither and has
// to earn its interest elsewhere.
// A plain evenly spaced scale is not in this list, because it is the Steps
// *picker*; offering it here as well would put the same control on screen
// twice under two names.
enum class ValueScheme : int
{
    None = 0,
    Notan2,     // two masses: the compositional test
    Notan3,
    Notan4,
    HighKey,
    MidKey,
    LowKey,
    Count
};

// The evenly spaced scale itself, which the Steps picker is made of.
Set BuildValueScale(int steps);

const wchar_t* ValueSchemeName (ValueScheme s);
const wchar_t* ValueSchemeShort(ValueScheme s);
const wchar_t* ValueSchemeAdvice(ValueScheme s);

// Whether `steps` means anything for this scheme. The notans carry their own
// count in their name; the scale and the keys take one.
bool UsesSteps(ValueScheme s);

// The scheme's values, as greys (hue 0, saturation 0). `steps` is used only
// where UsesSteps says so.
//
// These are deliberately *absolute* rather than relative to a current colour: a
// value scale is a fixed reference you snap to, not a set of neighbours around
// where you happen to be. The caller marks whichever step is nearest what the
// user is holding.
Set BuildValueSet(ValueScheme s, int steps);

// Index of the entry nearest `val`, or -1 for an empty set. This is what the
// panel marks, since the set itself does not depend on the current colour.
int NearestIndex(const Set& set, float val);

// ------------------------------------------------------------- temperature
//
// Warm colours advance and cool ones recede: the single most load-bearing fact
// in representational painting, and one a hue number does not tell you. The
// axis runs through the wheel: the warm pole is around 30 degrees HSV (orange)
// and the cool pole around 210 (cyan-blue).
//
// Returns -1 at the coolest, +1 at the warmest.
float Warmth(float hsvHue);
const wchar_t* WarmthName(float warmth);   // "WARM" / "COOL" / "NEUTRAL"

} // namespace harmony
