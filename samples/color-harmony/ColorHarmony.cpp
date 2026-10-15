#include "ColorHarmony.h"
#include <algorithm>
#include <cmath>

namespace harmony {

namespace {

inline float Wrap360(float d)
{
    d = std::fmod(d, 360.0f);
    return d < 0.0f ? d + 360.0f : d;
}

// The artist's wheel against the HSV one, sampled every 15 degrees of RYB.
//
// Read it as: standing at 15 degrees on a painter's colour wheel, the hue you
// are looking at is 8 degrees of HSV hue. The two agree at red and at the
// far side of blue and diverge everywhere else, most sharply across the
// yellow-to-green arc: RYB gives green a quarter of the wheel where HSV gives
// it a sixth, which is why a "triad" means different colours on each.
constexpr float kRybToHsv[25] = {
      0.0f,   8.0f,  17.0f,  26.0f,  34.0f,  41.0f,  48.0f,  54.0f,
     60.0f,  81.0f, 103.0f, 123.0f, 138.0f, 155.0f, 171.0f, 186.0f,
    240.0f, 254.0f, 269.0f, 284.0f, 299.0f, 315.0f, 330.0f, 345.0f,
    360.0f
};

} // namespace

const wchar_t* WheelName(Wheel w)
{
    return w == Wheel::RYB ? L"ARTIST (RYB)" : L"DIGITAL (RGB)";
}

const wchar_t* WheelBlurb(Wheel w)
{
    return w == Wheel::RYB
        ? L"Itten's pigment wheel. Blue opposes orange, red opposes green."
        : L"The additive wheel. Blue opposes yellow, red opposes cyan.";
}

float ToHsvHue(float wheelHue, Wheel w)
{
    if (w != Wheel::RYB) return Wrap360(wheelHue);

    const float d = Wrap360(wheelHue) / 15.0f;
    const int   i = std::min((int)d, 23);
    const float f = d - (float)i;

    // The table crosses 186 -> 240 in one step. That jump is the mapping, not a
    // seam: RYB has no hues between green and blue that HSV spreads over 54
    // degrees, so a linear walk across the gap is the honest interpolation.
    return Wrap360(kRybToHsv[i] + (kRybToHsv[i + 1] - kRybToHsv[i]) * f);
}

float FromHsvHue(float hsvHue, Wheel w)
{
    if (w != Wheel::RYB) return Wrap360(hsvHue);

    const float h = Wrap360(hsvHue);
    for (int i = 0; i < 24; ++i)
    {
        const float a = kRybToHsv[i], b = kRybToHsv[i + 1];
        if (h >= a && h <= b)
        {
            const float t = (b - a) > 1e-6f ? (h - a) / (b - a) : 0.0f;
            return Wrap360((i + t) * 15.0f);
        }
    }
    return h;
}

// ------------------------------------------------------------------ schemes

const wchar_t* SchemeName(Scheme s)
{
    switch (s)
    {
        case Scheme::Monochromatic:      return L"MONOCHROMATIC";
        case Scheme::Analogous:          return L"ANALOGOUS";
        case Scheme::Complementary:      return L"COMPLEMENTARY";
        case Scheme::SplitComplementary: return L"SPLIT COMPLEMENT";
        case Scheme::Triadic:            return L"TRIADIC";
        case Scheme::Tetradic:           return L"TETRADIC";
        case Scheme::Square:             return L"SQUARE";
        case Scheme::AccentedAnalogous:  return L"ACCENTED ANALOGOUS";
        default:                         return L"NONE";
    }
}

const wchar_t* SchemeShort(Scheme s)
{
    switch (s)
    {
        case Scheme::Monochromatic:      return L"MONO";
        case Scheme::Analogous:          return L"ANLG";
        case Scheme::Complementary:      return L"COMP";
        case Scheme::SplitComplementary: return L"SPLIT";
        case Scheme::Triadic:            return L"TRIAD";
        case Scheme::Tetradic:           return L"TETRA";
        case Scheme::Square:             return L"SQUARE";
        case Scheme::AccentedAnalogous:  return L"ACCENT";
        default:                         return L"OFF";
    }
}

const wchar_t* SchemeAdvice(Scheme s)
{
    switch (s)
    {
        case Scheme::Monochromatic:
            return L"One hue, many values. Cannot clash. Carry it with value "
                   L"contrast, or it goes flat.";
        case Scheme::Analogous:
            return L"Neighbours on the wheel. Calm and natural \x2014 most skies "
                   L"and most forests. Pick one to lead.";
        case Scheme::Complementary:
            return L"Opposites. Maximum vibrance, and maximum risk: at equal "
                   L"area and chroma they fight. Let one dominate.";
        case Scheme::SplitComplementary:
            return L"The opposite's two neighbours instead of the opposite "
                   L"itself. Most of the contrast, far less tension.";
        case Scheme::Triadic:
            return L"Three, evenly spaced. Vivid and balanced even when "
                   L"unsaturated. Rarely wants all three at full strength.";
        case Scheme::Tetradic:
            return L"Two complementary pairs. The richest scheme and the "
                   L"hardest \x2014 keep warm and cool unequal in area.";
        case Scheme::Square:
            return L"Four at even spacing. Like tetradic but symmetrical; "
                   L"balance it by value rather than by area.";
        case Scheme::AccentedAnalogous:
            return L"An analogous run plus the complement of its centre. A "
                   L"quiet field with one place for the eye to land.";
        default:
            return L"No scheme. The wheel shows only the current hue.";
    }
}

bool  UsesSpread(Scheme s)
{
    return s == Scheme::Analogous || s == Scheme::SplitComplementary ||
           s == Scheme::Tetradic  || s == Scheme::AccentedAnalogous;
}

float SpreadMin(Scheme s)     { return s == Scheme::Tetradic ? 15.0f : 10.0f; }
float SpreadMax(Scheme s)     { return s == Scheme::Tetradic ? 80.0f : 60.0f; }
float SpreadDefault(Scheme s) { return s == Scheme::Tetradic ? 60.0f : 30.0f; }

Set Build(Scheme s, float baseHue, float sat, float val, Wheel w, float spreadDeg)
{
    Set out;
    sat = std::clamp(sat, 0.0f, 1.0f);
    val = std::clamp(val, 0.0f, 1.0f);

    if (UsesSpread(s))
        spreadDeg = std::clamp(spreadDeg, SpreadMin(s), SpreadMax(s));

    // Geometry is done in wheel space and mapped back per result, so that on
    // the artist's wheel "120 degrees away" means 120 degrees of *that* wheel.
    const float base = FromHsvHue(baseHue, w);

    auto push = [&](float wheelHue, float ss, float vv, bool isBase)
    {
        if (out.count >= Set::kMax) return;
        Swatch& c = out.c[out.count++];
        c.h = ToHsvHue(wheelHue, w);
        c.s = std::clamp(ss, 0.0f, 1.0f);
        c.v = std::clamp(vv, 0.0f, 1.0f);
        c.isBase = isBase;
    };

    switch (s)
    {
        case Scheme::Monochromatic:
        {
            // The one scheme that is about value rather than hue. Five steps
            // spanning a usable range on both axes, with the base kept exactly
            // where it was rather than snapped into the ramp.
            push(base, sat, val, true);
            push(base, sat * 0.35f, std::min(1.0f, val * 0.55f + 0.45f), false);
            push(base, sat * 0.70f, std::min(1.0f, val * 0.78f + 0.22f), false);
            push(base, std::min(1.0f, sat * 1.15f), val * 0.68f, false);
            push(base, std::min(1.0f, sat * 1.25f), val * 0.40f, false);
            break;
        }
        case Scheme::Analogous:
            push(base - spreadDeg * 2.0f, sat, val, false);
            push(base - spreadDeg,        sat, val, false);
            push(base,                    sat, val, true);
            push(base + spreadDeg,        sat, val, false);
            push(base + spreadDeg * 2.0f, sat, val, false);
            break;

        case Scheme::Complementary:
            push(base,          sat, val, true);
            push(base + 180.0f, sat, val, false);
            break;

        case Scheme::SplitComplementary:
            push(base,                     sat, val, true);
            push(base + 180.0f - spreadDeg, sat, val, false);
            push(base + 180.0f + spreadDeg, sat, val, false);
            break;

        case Scheme::Triadic:
            push(base,          sat, val, true);
            push(base + 120.0f, sat, val, false);
            push(base + 240.0f, sat, val, false);
            break;

        case Scheme::Tetradic:
            push(base,                     sat, val, true);
            push(base + spreadDeg,          sat, val, false);
            push(base + 180.0f,             sat, val, false);
            push(base + 180.0f + spreadDeg, sat, val, false);
            break;

        case Scheme::Square:
            push(base,          sat, val, true);
            push(base + 90.0f,  sat, val, false);
            push(base + 180.0f, sat, val, false);
            push(base + 270.0f, sat, val, false);
            break;

        case Scheme::AccentedAnalogous:
            push(base - spreadDeg, sat, val, false);
            push(base,             sat, val, true);
            push(base + spreadDeg, sat, val, false);
            // The accent is deliberately left at full strength while the
            // analogous run keeps the base's saturation: an accent that matches
            // its field in chroma is not an accent.
            push(base + 180.0f, std::min(1.0f, sat * 1.1f + 0.1f), val, false);
            break;

        default:
            push(base, sat, val, true);
            break;
    }
    return out;
}

// ---------------------------------------------------- tints, tones and shades

const wchar_t* VariationName(Variation v)
{
    switch (v)
    {
        case Variation::Tints:  return L"TINTS";
        case Variation::Tones:  return L"TONES";
        case Variation::Shades: return L"SHADES";
        default:                return L"OFF";
    }
}

const wchar_t* VariationAdvice(Variation v)
{
    switch (v)
    {
        case Variation::Tints:
            return L"Hue plus white. Lighter and less saturated \x2014 haze, "
                   L"distance, light falling on a surface.";
        case Variation::Tones:
            return L"Hue plus grey. Same lightness, less chroma. Almost "
                   L"nothing observed is at full chroma; this is why.";
        case Variation::Shades:
            return L"Hue plus black. Darker, and on real pigment duller. "
                   L"Shadow is usually a tone, not a shade.";
        default:
            return L"";
    }
}

Set BuildVariation(Variation v, float hue, float sat, float val, int steps)
{
    Set out;
    steps = std::clamp(steps, 1, Set::kMax);
    sat = std::clamp(sat, 0.0f, 1.0f);
    val = std::clamp(val, 0.0f, 1.0f);

    for (int i = 0; i < steps; ++i)
    {
        // Index 0 is the colour you already have, so a row always starts from
        // where you are rather than from an endpoint you did not choose.
        const float t = (steps <= 1) ? 0.0f : (float)i / (float)(steps - 1);

        float ss = sat, vv = val;
        switch (v)
        {
            case Variation::Tints:
                // Toward white: value up, saturation down. Both, because adding
                // white to a pigment does both, and moving only one gives the
                // washed-out "reduce saturation" look that is not a tint.
                ss = sat * (1.0f - t * 0.85f);
                vv = val + (1.0f - val) * t * 0.95f;
                break;
            case Variation::Tones:
                // Toward grey at constant lightness: chroma out, value held.
                ss = sat * (1.0f - t * 0.9f);
                vv = val;
                break;
            case Variation::Shades:
                vv = val * (1.0f - t * 0.85f);
                break;
            default:
                break;
        }

        Swatch& c = out.c[out.count++];
        c.h = Wrap360(hue);
        c.s = std::clamp(ss, 0.0f, 1.0f);
        c.v = std::clamp(vv, 0.0f, 1.0f);
        c.isBase = (i == 0);
    }
    return out;
}

// ------------------------------------------------------------ value study

const wchar_t* ValueSchemeName(ValueScheme s)
{
    switch (s)
    {
        case ValueScheme::Notan2:  return L"NOTAN  14 TWO VALUES";
        case ValueScheme::Notan3:  return L"NOTAN  14 THREE VALUES";
        case ValueScheme::Notan4:  return L"NOTAN  14 FOUR VALUES";
        case ValueScheme::HighKey: return L"HIGH KEY";
        case ValueScheme::MidKey:  return L"MID KEY";
        case ValueScheme::LowKey:  return L"LOW KEY";
        default:                   return L"NONE";
    }
}

const wchar_t* ValueSchemeShort(ValueScheme s)
{
    switch (s)
    {
        case ValueScheme::Notan2:  return L"NOTAN 2";
        case ValueScheme::Notan3:  return L"NOTAN 3";
        case ValueScheme::Notan4:  return L"NOTAN 4";
        case ValueScheme::HighKey: return L"HIGH";
        case ValueScheme::MidKey:  return L"MID";
        case ValueScheme::LowKey:  return L"LOW";
        default:                   return L"OFF";
    }
}

const wchar_t* ValueSchemeAdvice(ValueScheme s)
{
    switch (s)
    {
        case ValueScheme::Notan2:
            return L"The compositional test: two masses, nothing between. If "
                   L"it does not read at two values, no amount of modelling "
                   L"will save it.";
        case ValueScheme::Notan3:
            return L"Light, mid and dark. Enough to describe form and still "
                   L"few enough to force a decision about what belongs to "
                   L"which mass.";
        case ValueScheme::Notan4:
            return L"Four masses. About the most you can hold as a plan; past "
                   L"this you are rendering rather than composing.";
        case ValueScheme::HighKey:
            return L"The light band. Airy and open, and it lives or dies on "
                   L"small differences  14 keep the darks out entirely or they "
                   L"take over.";
        case ValueScheme::MidKey:
            return L"The middle band. The hardest to carry: with no extremes "
                   L"to lean on, the interest has to come from edges, shape or "
                   L"texture.";
        case ValueScheme::LowKey:
            return L"The dark band. Dramatic, and it needs one genuinely light "
                   L"accent to stop reading as underexposed rather than dark.";
        default:
            return L"No study. The picker shows the whole range.";
    }
}

bool UsesSteps(ValueScheme s)
{
    // The notans carry their count in their name; only the keys take one.
    return s == ValueScheme::HighKey || s == ValueScheme::MidKey ||
           s == ValueScheme::LowKey;
}

Set BuildValueSet(ValueScheme s, int steps)
{
    Set out;
    if (s == ValueScheme::None) return out;

    steps = std::clamp(steps, 2, Set::kMax);

    // Endpoints are pulled in from 0 and 1 on purpose. Paper is not white and
    // ink is not black; a scale whose ends nothing can reach wastes two of its
    // steps and teaches a range the medium does not have.
    auto span = [&](float lo, float hi, int n)
    {
        n = std::clamp(n, 2, Set::kMax);
        for (int i = 0; i < n; ++i)
        {
            Swatch& c = out.c[out.count++];
            c.h = 0.0f;
            c.s = 0.0f;
            c.v = lo + (hi - lo) * (float)i / (float)(n - 1);
            c.isBase = false;
        }
    };

    switch (s)
    {
        // The notans span the full usable range whatever their count: massing a
        // picture into three values means three *separated* values, and packing
        // them into a band would be a key, not a notan.
        case ValueScheme::Notan2:  span(0.10f, 0.90f, 2); break;
        case ValueScheme::Notan3:  span(0.10f, 0.90f, 3); break;
        case ValueScheme::Notan4:  span(0.08f, 0.92f, 4); break;

        // The three keys tile the range and touch at their edges rather than
        // partitioning it cleanly. That overlap is the practice, not sloppiness:
        // a tone on the boundary genuinely belongs to both bands, and a picture
        // in low key still owns the darkest of the mid ones. It is kept small,
        // so the three chips mean visibly different things.
        case ValueScheme::HighKey: span(0.62f, 0.97f, steps); break;
        case ValueScheme::MidKey:  span(0.33f, 0.67f, steps); break;
        case ValueScheme::LowKey:  span(0.03f, 0.38f, steps); break;
        default: break;
    }
    return out;
}

Set BuildValueScale(int steps)
{
    // The same pulled-in endpoints as the schemes above, for the same reason:
    // paper is not white and ink is not black, and a scale whose ends nothing
    // can reach wastes two of its steps.
    Set out;
    steps = std::clamp(steps, 2, Set::kMax);
    for (int i = 0; i < steps; ++i)
    {
        Swatch& c = out.c[out.count++];
        c.h = 0.0f;
        c.s = 0.0f;
        c.v = 0.04f + 0.92f * (float)i / (float)(steps - 1);
    }
    return out;
}

int NearestIndex(const Set& set, float val)
{
    int best = -1;
    float bestD = 1e9f;
    for (int i = 0; i < set.count; ++i)
    {
        const float d = std::fabs(set.c[i].v - val);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

// ------------------------------------------------------------- temperature

float Warmth(float hsvHue)
{
    // Cosine about the warm pole. A hard warm/cool split at two hues would put
    // a step in the middle of yellow-green and violet, where the judgement is
    // genuinely gradual, and would report the same answer for a hue just inside
    // the boundary as for the pole itself.
    constexpr float kWarmPole = 30.0f;   // orange
    const float d = (Wrap360(hsvHue) - kWarmPole) * 0.0174532925f;
    return std::cos(d);
}

const wchar_t* WarmthName(float warmth)
{
    if (warmth >  0.35f) return L"WARM";
    if (warmth < -0.35f) return L"COOL";
    return L"NEUTRAL";
}

} // namespace harmony
