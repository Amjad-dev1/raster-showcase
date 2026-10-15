// Excerpt from tests/io_selftest.cpp: the checks that pin the brush engine.
// Helpers (MakeLayer, Premul, ColumnHeight, Check) are defined earlier in
// the full test file.

        // ---- the retro taper lands exactly what the delay line would ----
        //
        // Two routes to the same tail: hold dabs back until the end is known,
        // or stamp them at once and re-render the tail from the snapshot. With
        // no snapshot to restore from the engine has to use the delay line,
        // which is the reference. The brush is translucent so a dab laid twice
        // would show. This caught the re-render stamping, unclipped, dabs that
        // straddled the tail's edge. The third pass is a random brush, which
        // replays exactly only because each point keeps its generator state.
        for (int variant = 0; variant < 3; ++variant)
        {
            const bool glazed = variant >= 1, random = variant == 2;
            BrushPreset p;
            p.size = 30.0f; p.shape.hardness = 0.4f; p.opacity = 0.6f;
            p.path.spacing = 0.1f;
            p.taper.endSize = 1.0f; p.taper.endOpacity = 0.5f;
            p.taper.length = 0.4f; p.taper.pressureTip = false;
            p.rendering.mode = glazed ? RenderMode::Glazed : RenderMode::Blended;
            if (random)
            {
                p.shape.scatter = 0.4f; p.shape.rotation = 1.0f; p.shape.count = 3;
                p.shape.countJitter = 0.5f; p.shape.flipX = true; p.shape.roundness = 0.5f;
                p.dynamics.jitterSize = 0.4f; p.dynamics.jitterOpacity = 0.4f;
                // No path jitter: it rolls when a point is queued, and the
                // delay line queues and stamps in a different interleaving,
                // so the two routes would legitimately draw different dice.
            }

            // One layer for both runs: a stroke seeds its dice from the layer it
            // paints, so two layers would roll two different random brushes.
            // The random brush is tried on several layers (several seeds)
            // because one roll of the dice can miss the case that breaks: a
            // stamp scattered into the tail from a point outside it.
            std::vector<std::unique_ptr<Layer>> keep;   // distinct addresses
            int worst = 0;
            RectI delayBounds{};
            PixelBuffer b;
            for (int trial = 0; trial < (random ? 12 : 1); ++trial)
            {
            keep.push_back(MakeLayer(520, 220));
            Layer* L = keep.back().get();
            auto run = [&](bool delayLine, RectI* boundsOut) {
                L->Pixels().Clear();
                StrokeSnapshot snap;
                snap.Begin(L->Pixels());
                BrushPreset q = p;
                BrushEngine e;
                if (!delayLine) e.SetSnapshot(&snap);
                ColorValue ink{ 65535, { 10000, 30000, 50000, 0 } };
                e.BeginStroke(q, ink, DabMode::Paint, L);
                for (int i = 0; i <= 150; ++i)
                    e.AddPoint(40.0f + i * 2.8f, 110.0f + 50.0f * std::sin(i * 0.05f), 1.0f);
                e.EndStroke();
                if (boundsOut) *boundsOut = e.StrokeBounds();
                return PixelBuffer(L->Pixels());
            };

            const PixelBuffer a = run(false, nullptr);
            b = run(true, &delayBounds);

            for (int y = 0; y < a.Height(); ++y)
                for (int x = 0; x < a.Width() * a.Channels(); ++x)
                    worst = std::max(worst, std::abs((int)a.Row(y)[x] - (int)b.Row(y)[x]));
            }
            Check(worst == 0, random ? "random brush: the retro taper matches the delay line exactly"
                            : glazed ? "glazed: the retro taper matches the delay line exactly"
                                     : "blended: the retro taper matches the delay line exactly");

            // The tail is laid when the stroke ends; the bounds read after
            // that must cover it, or undo leaves the tail behind.
            bool outside = false;
            for (int y = 0; y < b.Height() && !outside; ++y)
                for (int x = 0; x < b.Width(); ++x)
                    if (b.At(x, y)[0] != 0 &&
                        !(x >= delayBounds.left && x < delayBounds.right &&
                          y >= delayBounds.top  && y < delayBounds.bottom))
                    { outside = true; break; }
            Check(!outside, "every pixel a tapered stroke paints is inside its reported bounds");
        }

        // ---- the retro taper works whatever the brush's size ceiling ----
        //
        // The tail rect used to be sized by props.maxSize (500px by default)
        // rather than by the dabs actually laid, which reached tiles the
        // snapshot had never saved; the restore then refused and the tail
        // stayed full width. Glazed off so the stroke is replayable.
        {
            auto L = MakeLayer(600, 600);
            StrokeSnapshot snap;
            snap.Begin(L->Pixels());
            BrushPreset p;
            p.size = 24.0f; p.shape.hardness = 1.0f;
            p.rendering.mode = RenderMode::Blended;
            p.taper.endSize = 1.0f; p.taper.length = 0.4f; p.taper.pressureTip = false;
            BrushEngine e;
            e.SetSnapshot(&snap);
            ColorValue black{ 65535, { 0, 0, 0, 0 } };
            e.BeginStroke(p, black, DabMode::Paint, L.get());
            for (int i = 0; i <= 160; ++i) e.AddPoint(30.0f + i * 2.0f, 60.0f, 1.0f);
            e.EndStroke();
            const int mid  = ColumnHeight(L->Pixels(), 170);
            const int tail = ColumnHeight(L->Pixels(), 344);
            Check(mid > 20 && tail * 2 < mid, "a replayable stroke near the canvas edge still tapers");
        }

        // ---- banded rendering is bit-identical to one thread ----
        //
        // RenderBatch cuts a big batch into row bands drawn concurrently. Each
        // band replays every dab in order, so nothing may differ: not on a
        // glazed stroke, not through a non-Normal blend, not when erasing.
        {
            auto run = [&](bool parallel, RenderMode rm, int blendMode, DabMode mode,
                           float dilution = 0.0f) {
                DabRenderer::SetParallel(parallel);
                auto L = MakeLayer(900, 700);
                FillPattern(L->Pixels(), 3);
                BrushPreset p;
                p.size = 160.0f; p.shape.hardness = 0.0f; p.rendering.flow = 0.1f;
                p.path.spacing = 0.03f; p.rendering.mode = rm;
                p.rendering.blendMode = blendMode;
                p.wet.dilution = dilution; p.wet.charge = 0.6f;
                BrushEngine e;
                ColorValue ink{ 65535, { 50000, 20000, 9000, 0 } };
                e.BeginStroke(p, ink, mode, L.get());
                for (int i = 0; i <= 60; ++i)
                    e.AddPoint(100.0f + i * 12.0f, 350.0f + 200.0f * std::sin(i * 0.1f), 0.8f);
                e.EndStroke();
                DabRenderer::SetParallel(true);
                return L;
            };
            const bool a = SamePixels(run(true,  RenderMode::Glazed,  0, DabMode::Paint)->Pixels(),
                                      run(false, RenderMode::Glazed,  0, DabMode::Paint)->Pixels());
            const bool b = SamePixels(run(true,  RenderMode::Blended, 2, DabMode::Paint)->Pixels(),
                                      run(false, RenderMode::Blended, 2, DabMode::Paint)->Pixels());
            const bool c = SamePixels(run(true,  RenderMode::Blended, 0, DabMode::Erase)->Pixels(),
                                      run(false, RenderMode::Blended, 0, DabMode::Erase)->Pixels());
            Check(a, "a glazed stroke drawn in bands matches one drawn on one thread");
            Check(b, "a blend-mode stroke drawn in bands matches one drawn on one thread");
            const bool w = SamePixels(run(true,  RenderMode::Blended, 0, DabMode::Paint, 0.6f)->Pixels(),
                                      run(false, RenderMode::Blended, 0, DabMode::Paint, 0.6f)->Pixels());
            Check(c, "an erase stroke drawn in bands matches one drawn on one thread");
            Check(w, "a wet stroke drawn in bands matches one drawn on one thread");
        }

        // ---- the GPU paints what the CPU paints ----
        //
        // brush_dab.hlsl claims to be StampRows step for step. That claim is
        // what lets a textured, angled, grained, scattered brush take the GPU
        // path at all, so it is checked against the real thing: the same
        // shipped presets, drawn both ways, compared pixel by pixel. The two
        // quantise differently (the CPU rounds to 16 bits after every dab,
        // the GPU accumulates in a 16-bit target and composites once), so the
        // tolerance is a tenth of a percent, far below what an eye or a
        // wrong formula would produce.
        {
            HWND hw = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 64, 64,
                                      nullptr, nullptr, nullptr, nullptr);
            RenderDevice dev;
            StrokeBuffer sb;
            const bool gpu = hw && dev.Init(hw, 64, 64) && sb.Init(&dev);
            Check(gpu, "a D3D11 device and the dab shaders are available for the GPU comparison");
            Check(UseRepoResources(), "the shipped brush textures are found");

            brushlib::Library lib;
            brushlib::BuildDefaults(lib);
            const wchar_t* names[] = { L"Round Soft", L"6B Pencil", L"Graphite Stick",
                                       L"Charcoal", L"Bristle Scatter", L"Noise Spray",
                                       L"Soft Airbrush", L"Brush Pen" };
            for (const wchar_t* name : names)
            {
                const BrushPreset* preset = nullptr;
                for (const auto& col : lib.Collections())
                    for (const auto& b : col.brushes)
                        if (b.preset.name == name) preset = &b.preset;
                if (!preset || !gpu) { Check(false, "preset found and GPU available"); continue; }

                bool wasGpu = false;
                auto L = MakeLayer(420, 260);       // one layer: same seed both ways
                auto run = [&](bool onGpu) {
                    L->Pixels().Clear();
                    StrokeSnapshot snap;
                    snap.Begin(L->Pixels());
                    BrushEngine e;
                    e.SetSnapshot(&snap);
                    if (onGpu) e.SetGpuSink(&sb);
                    ColorValue ink{ 65535, { 12000, 30000, 52000, 0 } };
                    e.BeginStroke(*preset, ink, DabMode::Paint, L.get());
                    if (onGpu) wasGpu = e.IsGpuStroke();
                    for (int i = 0; i <= 110; ++i)
                        e.AddPoint(50.0f + i * 3.0f, 130.0f + 70.0f * std::sin(i * 0.06f),
                                   0.5f + 0.5f * std::sin(i * 0.03f));
                    e.EndStroke();
                    return PixelBuffer(L->Pixels());
                };
                const PixelBuffer cpu  = run(false);
                const PixelBuffer gpuB = run(true);
                const Diff df = Premul(cpu, gpuB);
                const int worst = df.worst;
                const double meanDiff = df.mean;
                char label[160], nm[64];
                WideCharToMultiByte(CP_UTF8, 0, name, -1, nm, sizeof nm, nullptr, nullptr);
                snprintf(label, sizeof label, "%s takes the GPU path", nm);
                Check(wasGpu, label);
                snprintf(label, sizeof label, "%s: GPU matches CPU (worst %d, mean %.2f of 65535)",
                         nm, worst, meanDiff);
                Check(worst <= 64 && meanDiff < 2.0, label);
            }
            sb.Shutdown();
            if (hw) DestroyWindow(hw);
        }

