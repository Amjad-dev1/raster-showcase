#pragma once

// Smooths the pointer path before dabs are placed on it. All positions are in
// canvas-pixel space.
//
// This used to be a box average over the last N samples, N scaled by strength
// up to 20. Two things were wrong with that. It lags by half the window
// unconditionally (the drawn point never reaches the pen until the stroke
// ends) and, worse, the window is counted in *samples*, so the same setting
// behaves completely differently on different hardware: a 133 Hz tablet and a
// 1000 Hz pen fill the same window in eight times the difference in time, and
// the mouse fills it slower still. A brush tuned on one machine felt sluggish
// or unsmoothed on the next.
//
// What replaces it is a one-pole filter whose coefficient is corrected for the
// interval between samples, so the amount of smoothing is a property of the
// hand movement rather than of the device's report rate; plus a hard leash, so
// that however heavy the setting, the mark is never further behind the nib
// than a known number of pixels. The lag is bounded and small by construction
// instead of being whatever the sample rate makes it.
class StrokeStabilizer
{
public:
    // strength 0 = passthrough, 1 = maximum smoothing
    void SetStrength(float strength) { m_strength = strength < 0.0f ? 0.0f : (strength > 1.0f ? 1.0f : strength); }
    void Reset();

    // Smooth raw position in-place.
    void Stabilize(float& x, float& y, float& pressure);

    // On pen-up, walk the smoothed point the rest of the way to where the pen
    // actually lifted. Call repeatedly; returns false once it has arrived.
    bool Flush(float finalX, float finalY, float finalP,
               float& outX, float& outY, float& outP);

private:
    // How far the mark may fall behind the nib at full strength, in canvas px.
    static constexpr float kMaxLagPx = 18.0f;

    float  m_strength = 0.0f;
    float  m_x = 0.0f, m_y = 0.0f, m_p = 0.0f;
    bool   m_have    = false;
    double m_lastMs  = 0.0;
};
