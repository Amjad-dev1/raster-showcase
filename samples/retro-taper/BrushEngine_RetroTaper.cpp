// Excerpt from src/engine/BrushEngine.cpp: how a tapered stroke is drawn
// without the mark trailing the pen. Dabs land at full width as the pen
// moves; when the stroke ends, its tail is restored from the undo snapshot
// and drawn again with the taper it has actually earned.

void BrushEngine::Drain(bool final)
{
    if (m_taperWin <= 0.0f)
    {
        // No end taper: stamp everything now.
        while (!m_pending.empty())
        {
            PathPt pt = m_pending.front(); m_pending.pop_front();
            EmitStamp(pt, StartTaperFactor(pt.dist), 1.0f);
        }
        FlushBatch();
        return;
    }

    // A tail taper is a statement about where the stroke *ends*, so the honest
    // implementation holds dabs back until they are far enough behind the nib
    // to be safe, which is exactly a delay line, and it is felt as the mark
    // trailing the pen by the width of the taper window. For the inking presets
    // that is tens of pixels, which at drawing speed is tens of milliseconds of
    // visible lag on the one tool where the line most needs to feel attached to
    // the nib.
    //
    // So when the stroke is replayable, nothing is held back: every dab is
    // stamped the moment it is placed, at full width, and the tail is
    // re-rendered from the pre-stroke pixels once the stroke really ends. The
    // result is identical, and nothing lags.
    if (m_retro)
    {
        while (!m_pending.empty())
        {
            PathPt pt = m_pending.front(); m_pending.pop_front();
            pt.rng = m_rng;
            m_stamped.push_back(pt);
            EmitStamp(pt, StartTaperFactor(pt.dist), 1.0f);
        }
        FlushBatch();
        if (final) RetroTaper();
        return;
    }

    if (final)
    {
        for (const PathPt& pt : m_pending)
        {
            float endT = rmath::clamp01((m_strokeLen - pt.dist) / m_taperWin);
            EmitStamp(pt, StartTaperFactor(pt.dist), endT);
        }
        m_pending.clear();
        FlushBatch();
        return;
    }

    // Stamp only dabs that are safely past the trailing taper window.
    while (!m_pending.empty() &&
           m_pending.front().dist <= m_strokeLen - m_taperWin)
    {
        PathPt pt = m_pending.front(); m_pending.pop_front();
        EmitStamp(pt, StartTaperFactor(pt.dist), 1.0f);
    }
    FlushBatch();
}

// A stroke can be re-rendered from its point list alone only if no dab depends
// on a die roll or on what the canvas already held. Anything else replays to a
// different picture than the one on screen.
bool BrushEngine::ReplaySafe() const
{
    // Only what reads the canvas disqualifies a stroke. Smudge and wet mixing
    // take colour from under the nib, which re-rendering over restored pixels
    // would read differently. Randomness does not: every recorded point keeps
    // the generator state it was first stamped with. Glazed does not either:
    // the glaze map is reset over the restored tail along with the pixels.
    //
    // This used to test wet.pull, which only acts through dilution and
    // defaults to 0.5, so no brush was ever replayable and every tapered
    // stroke paid the delay line this exists to avoid.
    return m_mode != DabMode::Smudge && m_preset.wet.dilution <= 0.0f;
}

// Put the tail back the way it was before the stroke touched it, then stamp
// every dab that reaches into it again, this time with the end taper each one
// has actually earned, now that the stroke's real length is known.
void BrushEngine::RetroTaper()
{
    if (!m_layer || m_taperWin <= 0.0f || m_stamped.empty()) return;

    const auto endTaper = [&](const PathPt& pt) {
        return rmath::clamp01((m_strokeLen - pt.dist) / m_taperWin);
    };

    // On the GPU the stroke lives in a buffer of its own, so there is nothing
    // to restore: clear it and lay every dab again with its real taper. It is
    // one draw call however long the stroke was.
    if (m_gpuActive)
    {
        m_gpu->Restart();
        for (const PathPt& pt : m_stamped)
        {
            m_rng = pt.rng;
            EmitStamp(pt, StartTaperFactor(pt.dist), endTaper(pt));
        }
        return;
    }

    if (!m_snap) return;

    PixelBuffer& buf = m_layer->Pixels();
    const RectI clip{ 0, 0, buf.Width(), buf.Height() };
    const float tailStart = m_strokeLen - m_taperWin;
    const float rad = m_maxReach + 2.0f;
    auto dabRect = [&](const PathPt& pt) {
        return RectI{ (int)std::floor(pt.x - rad) - 1, (int)std::floor(pt.y - rad) - 1,
                      (int)std::ceil (pt.x + rad) + 1, (int)std::ceil (pt.y + rad) + 1 };
    };

    // The region the taper actually changes: everything a tail dab covers.
    RectI tail{0,0,0,0};
    bool  have = false;
    for (const PathPt& pt : m_stamped)
    {
        if (pt.dist <= tailStart) continue;
        const RectI r = dabRect(pt).Intersect(clip);
        if (r.IsEmpty()) continue;
        tail = have ? tail.Union(r) : r;
        have = true;
    }
    if (!have) return;

    // A tile under the tail that no dab has written to yet still holds its
    // pre-stroke pixels (every write is preserved one dab ahead of it), so
    // preserving it now is as good as having done it earlier. Without this the
    // restore refused whenever the tail rect reached a tile the stroke had not,
    // and the taper silently never happened.
    m_snap->Preserve(buf, tail);
    if (!m_snap->Restore(buf, tail)) return;

    // The glaze map has to go back with the pixels, or the re-stamped dabs
    // would find their own coverage already there and lay nothing.
    if (m_cov)
        for (int y = tail.top; y < tail.bottom; ++y)
            std::memset(m_cov.Data() + (size_t)y * m_cov.Width() + tail.left, 0,
                        (size_t)tail.Width() * sizeof(uint16_t));

    // Everything that reaches into the tail has to go down again, in order, or
    // the part of the stroke that merely passed through it would be missing;
    // and clipped to the tail, or the part of such a dab lying outside it,
    // which was never restored, would be laid twice.
    m_clip   = tail;
    m_clipOn = true;
    for (const PathPt& pt : m_stamped)
    {
        if (dabRect(pt).Intersect(tail).IsEmpty()) continue;
        m_rng = pt.rng;
        EmitStamp(pt, StartTaperFactor(pt.dist), endTaper(pt));
    }
    FlushBatch();
    m_clipOn = false;
}

