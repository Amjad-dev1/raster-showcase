#include "StrokeStabilizer.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

double NowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// The report interval the strength is calibrated against. A device that reports
// twice as often gets a coefficient that smooths half as much per sample, which
// leaves the smoothing over any given stretch of time the same.
constexpr float kRefMs = 8.0f;

} // namespace

void StrokeStabilizer::Reset()
{
    m_have   = false;
    m_lastMs = 0.0;
}

void StrokeStabilizer::Stabilize(float& x, float& y, float& pressure)
{
    if (m_strength <= 0.001f) { m_have = false; return; }

    const double now = NowMs();

    if (!m_have)
    {
        m_x = x; m_y = y; m_p = pressure;
        m_have = true;
        m_lastMs = now;
        return;
    }

    // Clamped: a stall between samples must not let one sample snap the mark
    // to the pen, and a burst of samples in the same millisecond must not
    // freeze it.
    const float dt = (float)std::clamp(now - m_lastMs, 0.5, 32.0);
    m_lastMs = now;

    const float perRef = 1.0f - m_strength * 0.88f;      // blend at the reference rate
    const float a = 1.0f - std::pow(1.0f - perRef, dt / kRefMs);

    m_x += (x - m_x) * a;
    m_y += (y - m_y) * a;
    m_p += (pressure - m_p) * a;

    // The leash. Exponential smoothing alone lags by an amount that grows with
    // how fast the hand is moving, which is precisely when the lag is most
    // visible. Capping the distance means a fast stroke tracks the nib almost
    // exactly and only a slow, deliberate one is smoothed, which is the way
    // round a person wants it.
    const float dx = x - m_x, dy = y - m_y;
    const float d  = std::sqrt(dx * dx + dy * dy);
    const float leash = m_strength * kMaxLagPx;
    if (d > leash && d > 1e-6f)
    {
        const float k = (d - leash) / d;
        m_x += dx * k;
        m_y += dy * k;
    }

    x = m_x; y = m_y; pressure = m_p;
}

bool StrokeStabilizer::Flush(float finalX, float finalY, float finalP,
                             float& outX, float& outY, float& outP)
{
    if (!m_have || m_strength <= 0.001f) return false;

    const float dx = finalX - m_x, dy = finalY - m_y;
    if (std::sqrt(dx * dx + dy * dy) < 0.35f)
    {
        m_have = false;
        return false;
    }

    // A fixed fraction of what is left, so the loop closes geometrically and
    // the last stretch of the stroke is drawn rather than jumped.
    constexpr float kStep = 0.34f;
    m_x += dx * kStep;
    m_y += dy * kStep;
    m_p += (finalP - m_p) * kStep;

    outX = m_x; outY = m_y; outP = m_p;
    return true;
}
