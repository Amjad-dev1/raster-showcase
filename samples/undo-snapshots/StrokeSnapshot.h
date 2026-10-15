#pragma once
#include "core/PixelBuffer.h"
#include "core/Rect.h"
#include <vector>
#include <cstring>
#include <cstdint>

// The "before" image an undoable stroke needs, captured lazily in tiles.
//
// The obvious implementation (copy the whole layer at mouse-down and trim it
// to the stroke's bounding box at mouse-up) costs one allocation and one
// memcpy of the entire layer per stroke. On a 4000x4000 RGBA16 document that is
// 128 MB of memcpy before the first dab is drawn, which is felt as a hitch at
// the start of every mark and gets worse the larger the canvas is.
//
// A stroke only ever touches the pixels under it, and it knows which those are
// one dab before it writes them. So nothing is copied up front: the layer is
// divided into a grid of tiles, and a tile is copied the first time a dab is
// about to write into it. The cost becomes proportional to the area the stroke
// actually covers rather than to the size of the document.
class StrokeSnapshot
{
public:
    static constexpr int kTile = 128;

    void Begin(const PixelBuffer& pb)
    {
        Clear();
        m_w  = pb.Width();
        m_h  = pb.Height();
        m_ch = pb.Channels();
        m_tx = (m_w + kTile - 1) / kTile;
        m_ty = (m_h + kTile - 1) / kTile;
        m_tiles.assign((size_t)m_tx * m_ty, {});
        m_saved.assign((size_t)m_tx * m_ty, 0);
        m_active = (m_w > 0 && m_h > 0 && m_ch > 0);
    }

    bool Active() const { return m_active; }

    void Clear()
    {
        m_tiles.clear();  m_tiles.shrink_to_fit();
        m_saved.clear();  m_saved.shrink_to_fit();
        m_w = m_h = m_ch = m_tx = m_ty = 0;
        m_active = false;
    }

    // Copy every tile `r` touches that has not been copied yet. Must be called
    // before the dab writes, and with a rect already clipped to the buffer.
    void Preserve(const PixelBuffer& pb, const RectI& r)
    {
        if (!m_active || r.IsEmpty()) return;
        if (pb.Width() != m_w || pb.Height() != m_h || pb.Channels() != m_ch) return;

        const int t0x = std::max(0, r.left   / kTile);
        const int t0y = std::max(0, r.top    / kTile);
        const int t1x = std::min(m_tx - 1, (r.right  - 1) / kTile);
        const int t1y = std::min(m_ty - 1, (r.bottom - 1) / kTile);

        for (int ty = t0y; ty <= t1y; ++ty)
        for (int tx = t0x; tx <= t1x; ++tx)
        {
            const size_t ti = (size_t)ty * m_tx + tx;
            if (m_saved[ti]) continue;

            const int x0 = tx * kTile, y0 = ty * kTile;
            const int tw = std::min(kTile, m_w - x0);
            const int th = std::min(kTile, m_h - y0);
            const size_t rowU16 = (size_t)tw * m_ch;

            std::vector<uint16_t> tile(rowU16 * th);
            for (int y = 0; y < th; ++y)
                std::memcpy(tile.data() + (size_t)y * rowU16,
                            pb.At(x0, y0 + y), rowU16 * sizeof(uint16_t));

            m_tiles[ti] = std::move(tile);
            m_saved[ti] = 1;
        }
    }

    // Assemble the pre-stroke image of `bounds`. A tile that was never touched
    // cannot appear inside the stroke's own bounding box, but if one somehow
    // does it is filled from `now`; an undo that leaves those pixels alone is
    // wrong in a way nobody sees, where reading uninitialised memory is not.
    std::vector<uint16_t> Extract(const PixelBuffer& now, const RectI& bounds) const
    {
        std::vector<uint16_t> out;
        if (!m_active || bounds.IsEmpty()) return out;

        const int bw = bounds.Width(), bh = bounds.Height();
        const size_t outRow = (size_t)bw * m_ch;
        out.resize(outRow * bh);

        for (int y = 0; y < bh; ++y)
        {
            const int gy = bounds.top + y;
            const int ty = gy / kTile;
            uint16_t* dst = out.data() + (size_t)y * outRow;

            int x = 0;
            while (x < bw)
            {
                const int gx = bounds.left + x;
                const int tx = gx / kTile;
                const int x0 = tx * kTile;
                const int tw = std::min(kTile, m_w - x0);
                const int run = std::min(bw - x, x0 + tw - gx);
                if (run <= 0) break;

                const size_t ti = (size_t)ty * m_tx + tx;
                if (ti < m_saved.size() && m_saved[ti])
                {
                    const size_t tileRow = (size_t)tw * m_ch;
                    const uint16_t* src = m_tiles[ti].data()
                                        + (size_t)(gy - ty * kTile) * tileRow
                                        + (size_t)(gx - x0) * m_ch;
                    std::memcpy(dst + (size_t)x * m_ch, src,
                                (size_t)run * m_ch * sizeof(uint16_t));
                }
                else
                {
                    std::memcpy(dst + (size_t)x * m_ch, now.At(gx, gy),
                                (size_t)run * m_ch * sizeof(uint16_t));
                }
                x += run;
            }
        }
        return out;
    }

    // Put `r` back the way it was before the stroke started. Used by the end
    // taper, which stamps its tail at full width so the mark keeps up with the
    // nib and then re-renders that tail properly once the real end is known.
    // Returns false if any tile under `r` was never preserved, in which case
    // nothing is written, since a partial restore would punch a hole in the stroke.
    bool Restore(PixelBuffer& pb, const RectI& r) const
    {
        if (!m_active || r.IsEmpty()) return false;
        if (pb.Width() != m_w || pb.Height() != m_h || pb.Channels() != m_ch) return false;

        const int t0x = std::max(0, r.left   / kTile);
        const int t0y = std::max(0, r.top    / kTile);
        const int t1x = std::min(m_tx - 1, (r.right  - 1) / kTile);
        const int t1y = std::min(m_ty - 1, (r.bottom - 1) / kTile);

        for (int ty = t0y; ty <= t1y; ++ty)
        for (int tx = t0x; tx <= t1x; ++tx)
            if (!m_saved[(size_t)ty * m_tx + tx]) return false;

        for (int y = r.top; y < r.bottom; ++y)
        {
            const int ty = y / kTile;
            int x = r.left;
            while (x < r.right)
            {
                const int tx = x / kTile;
                const int x0 = tx * kTile;
                const int tw = std::min(kTile, m_w - x0);
                const int run = std::min(r.right - x, x0 + tw - x);
                if (run <= 0) break;

                const size_t ti = (size_t)ty * m_tx + tx;
                const size_t tileRow = (size_t)tw * m_ch;
                const uint16_t* src = m_tiles[ti].data()
                                    + (size_t)(y - ty * kTile) * tileRow
                                    + (size_t)(x - x0) * m_ch;
                std::memcpy(pb.At(x, y), src, (size_t)run * m_ch * sizeof(uint16_t));
                x += run;
            }
        }
        pb.MarkDirty(r);
        return true;
    }

private:
    std::vector<std::vector<uint16_t>> m_tiles;
    std::vector<uint8_t>               m_saved;
    int  m_w = 0, m_h = 0, m_ch = 0, m_tx = 0, m_ty = 0;
    bool m_active = false;
};
