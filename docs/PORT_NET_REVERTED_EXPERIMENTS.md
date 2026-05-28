# port-net-predict — Reverted Experiments

Approaches that were implemented on `port-net-predict`, failed, and were rolled back. Recorded so they aren't re-attempted blindly. Failure descriptions are quoted verbatim from the original design notes.

---

## CSP "input replay" history-shift + variable smoothing window

**Reverted on `port-net-predict`**: an "input replay" history-shift and a magnitude-based variable smoothing window. The shift modified all history entries with `tick >= ack_tick` by the error delta; the window scaled smoothing to 2–5 frames for big errors. Both caused exponential teleporting at ≥100 ms ping: shifted history desyncs subsequent ack comparisons (each new ack reports a fresh huge error against the now-wrong history, schedules another shift, etc.), and the variable window snapped large errors aggressively which looked like teleports instead of smooth corrections.

Do not retry without addressing: the feedback loop where a shifted history desyncs subsequent ack comparisons — each new ack reports a fresh huge error against the now-wrong history and schedules another shift — and the variable window's aggressive snapping of large errors, both of which produce exponential teleporting at ≥100 ms ping.

---

## Full-array bone-matrix translation for lag compensation

**A full-array bone-matrix translation was attempted on `port-net-predict` and reverted** — `chr->model->matrices` is allocated each frame from `gfxAllocate` (a per-frame heap reset by `gfxSwapBuffers`), so the pointer may be stale or already reused for vertex buffers by the time `shotCalculateHits` runs. Writing past matrix[0] crashed the host on disconnect (access violation, `0xc0000005`). Doing this safely would require either re-deriving the matrices on demand or hooking into the model render path.

Do not retry without addressing: `chr->model->matrices` lives in the per-frame `gfxAllocate` heap (reset by `gfxSwapBuffers`) and may be stale or reused by the time `shotCalculateHits` runs — a safe attempt must re-derive the matrices on demand or hook into the model render path instead of writing into that buffer.
