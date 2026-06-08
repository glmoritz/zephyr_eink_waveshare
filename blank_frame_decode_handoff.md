# Blank server frame — diagnosis & fix handoff

**Status:** root cause confirmed; fix approach chosen (grayscale decode); implementation
not started. This doc has everything needed to resume cold.

Device under test: `dev_362034071742` (800×480, **display_bit_depth=4**).
Backend: `/home/guilherme/00_tmp/eink_llss` (running in container `dazzling_blackwell`,
uvicorn on port 8008, packages live under the **`vscode`** user, not root).
Firmware build container: `Zephyr-Introduction`; LVGL at
`/opt/toolchains/modules/lib/gui/lvgl/`.

---

## TL;DR

The frame **downloads correctly**. It renders blank because **LVGL's PNG decode runs out
of heap**: lodepng decodes to **ARGB8888 = 800×480×4 = 1.46 MiB** but the LVGL pool
(`.lvgl_heap`) is only **128 KiB**. `lodepng` alloc fails → `decode_png_data()` returns
NULL → image object has no data → blank.

**Chosen fix:** custom **L8 (8-bit grayscale)** image decoder so the decoded buffer is
~384 KiB instead of 1.46 MiB, fitting the existing PSRAM budget. **Triple buffer is fine,
do NOT extend it** (it holds the *compressed* PNG, 8.5 KB; that part works).

---

## What was ruled OUT (don't re-investigate)

1. **Download / triple buffer.** Backend serves a valid 8-bit grayscale PNG, 800×480,
   **8474 bytes, full of content** (verified by running the real
   `convert_png_to_quantized_png` against live DB frame `frame_39ce7ae2ea98`). Fits the
   64 KB slots easily. `display_thread.c` triple-buffer logic is correct.
2. **`frame_dsc.header.cf = LV_COLOR_FORMAT_L8` in `display_png_frame_locked()`**
   (`src/display_thread.c:223`). Harmless: `lv_lodepng_init()` inserts its decoder at the
   list **head** (`lv_ll_ins_head`), so lodepng's `info_cb` runs first, matches the PNG
   magic, and overwrites the header to ARGB8888. The L8 value is never used. (Leaving it
   wrong is sloppy though — see step 4 below.)
3. **`recv_buf`/`ctx.buf` aliasing in `llss_client.c do_request()`** (same pointer used as
   both HTTP parse buffer and accumulator). Theoretically risky but NOT the cause here —
   the bytes arrive intact (download verified end to end on the backend side).

## What is CONFIRMED the bug

`/opt/toolchains/modules/lib/gui/lvgl/src/libs/lodepng/lv_lodepng.c` →
`decode_png_data()` calls `lodepng_decode32()` → always ARGB8888 (4 B/px) → `lv_malloc`.
Pool too small → returns NULL → blank. This path **never worked**; it only surfaced now
that the download succeeds.

---

## PSRAM budget (from `build/zephyr/zephyr.map`, 8 MiB region 0x3c000000–0x3c800000)

```
0x3c17b450  .ext_ram_noinit.llss_frames   0x30000  192 KiB  (PNG triple buffer, 3×64K)
0x3c1ab450  .ext_ram.bss (ssd16xx panel)  0x23280  143 KiB  (panel old+new framebuffers)
0x3c1ce6d0  .lvgl_buf (VDB, L8 full scr)  0x5dc00  384 KiB
0x3c22c2d0  .lvgl_heap                    0x20000  128 KiB  ← lv_malloc pool, TOO SMALL
0x3c24c2d0  .mbedtls_heap                 0x19000  100 KiB
0x3c2652d0  ext k_malloc heap            0x400000  4.00 MiB (ESP_SPIRAM_HEAP_SIZE, WiFi/TLS)
0x3c6652d0  ── FREE ──                             1.60 MiB
0x3c800000  (top)
```

Config knobs:
- `prj.conf:29  CONFIG_LV_Z_MEM_POOL_SIZE=131072`  → `.lvgl_heap` (the 128 KiB).
- `prj.conf:31  CONFIG_LV_Z_VDB_CUSTOM_SECTION=y`, `:32 CONFIG_LV_Z_MEMORY_POOL_CUSTOM_SECTION=y`
  → these are what put `.lvgl_buf`/`.lvgl_heap` in PSRAM.
- `boards/eink_llss_esp32_procpu.conf:18  CONFIG_ESP_SPIRAM_HEAP_SIZE=4194304` → the 4 MiB.

L8 decode math: resident 800×480×1 = **384 KiB**; transient peak (grayscale scanlines
~376 KiB + draw buf 384 KiB) ≈ **760 KiB**. Comfortably inside the 1.60 MiB free, so a
**modest** `CONFIG_LV_Z_MEM_POOL_SIZE` bump (e.g. 128K → 1 MiB = `1048576`) is enough.
Do NOT need to shrink the WiFi heap with this approach.

---

## CRITICAL GOTCHA discovered (must solve before coding)

LVGL **patched lodepng.c itself**. `decodeGeneric` hardcodes the output draw buf to
ARGB8888 (`lodepng.c:~5798`):

```c
lv_draw_buf_t * decoded = lv_draw_buf_create_ex(image_cache_draw_buf_handlers,
                                                *w, *h, LV_COLOR_FORMAT_ARGB8888, 4 * *w);
*out = (unsigned char*)decoded;   // *out is an lv_draw_buf_t, not raw pixels
```

So `*out` from `lodepng_decode*` is already an `lv_draw_buf_t`, and its `->data` holds the
pixels. The color-convert branch in `lodepng_decode()` (`lodepng.c:~5839`) **also**
allocates ARGB8888 as the destination. So:

> Calling `lodepng_decode_memory(..., LCT_GREY, 8)` does NOT give an L8 draw buf — the
> patched code still creates an ARGB8888 draw buf. We cannot get an L8 buffer for free
> from this patched API.

There's also a relevant guard at `lodepng.c:~5814` (`return 56` unsupported conversion)
limiting which `info_raw` conversions are allowed.

### Implication for the implementation

Two viable routes — **decide first thing next session**:

- **Route A (recommended): own decoder, bypass the patched draw-buf path.**
  Write a small app-side image decoder (register AFTER `lv_lodepng_init()` so it's at the
  list head and wins). In its `open_cb`:
  1. Allocate our own L8 draw buf: `lv_draw_buf_create(800, 480, LV_COLOR_FORMAT_L8, 0)`.
  2. Decode PNG → grayscale into a temp raw buffer using the **low-level** lodepng state
     API with `color_convert` set so it writes plain bytes (NOT the patched
     `lv_draw_buf_create_ex` path), OR decode then `lodepng_convert` into our L8 buffer.
     The patched `decodeGeneric` is the obstacle — confirm whether ANY entry point yields
     raw bytes, or whether we must call the still-unpatched `zlib_decompress` +
     `postProcessScanlines` ourselves (they exist in lodepng.c). Need to re-read
     `lodepng.c` 5740–5860 to pick the cleanest seam.
  3. Set `dsc->decoded = our_buf`, header cf=L8, w=800, h=480.
  This keeps RGBA decode out of the heap entirely (~384 KiB resident).

- **Route B (fallback): keep stock ARGB8888 decode, just grow the pool.**
  This was the *other* option the user explicitly did NOT pick (they want grayscale), but
  if Route A proves too invasive, bumping `CONFIG_LV_Z_MEM_POOL_SIZE` to ~2.5 MiB +
  shrinking `ESP_SPIRAM_HEAP_SIZE` to 2 MiB is the quick unblock. Documented only as a
  safety net.

**Open question to resolve next session:** find the cleanest lodepng entry point (or
direct `zlib_decompress`+`postProcessScanlines` call) that yields raw grayscale bytes
without triggering the patched `lv_draw_buf_create_ex(...ARGB8888...)`. Re-read
`/opt/toolchains/modules/lib/gui/lvgl/src/libs/lodepng/lodepng.c` lines ~5740–5870.

---

## Step-by-step plan for next session

1. **Fix the cosmetic cf bug** in `src/display_thread.c:223`: the manual descriptor setup
   in `display_png_frame_locked()` should reflect reality (L8 once our decoder outputs L8;
   or `LV_COLOR_FORMAT_RAW` if relying on a decoder to fill the header). Currently it sets
   L8 on PNG bytes which only works by luck.
2. **Resolve the open question** above (which lodepng seam to use for grayscale).
3. **Implement Route A**: new file e.g. `src/png_l8_decoder.c` + register it from
   `ui_init()` in `display_thread.c` (replace or follow `lv_lodepng_init()`; ensure ours
   is tried first). Decode → L8 draw buf.
4. **Bump pool modestly**: `prj.conf:29 CONFIG_LV_Z_MEM_POOL_SIZE` 131072 → 1048576
   (1 MiB) to give the L8 draw buf + transient scanlines room. Re-check map after build:
   `.lvgl_heap` grows by ~0.9 MiB, must stay under the 1.60 MiB free gap (it will).
5. **Build in container** (`Zephyr-Introduction`; west/ninja are container-only — never
   build on host). Flash, fetch a frame, confirm it renders.
6. If decode still fails, add a log in the decoder's error path and check the serial log
   for the lodepng error code / alloc failure.

## Useful repro commands

Backend — dump what the device actually receives (run as vscode user):
```
docker exec -u vscode dazzling_blackwell bash -lc 'cd /workspaces/eink_llss/app && \
  /usr/local/bin/python - <<PY
import io; from PIL import Image
from database import SessionLocal; import db_models as M; import frame_converter as fc
db=SessionLocal(); dev=db.query(M.Device).first()
fr=db.query(M.Frame).order_by(M.Frame.id.desc()).first()
out=fc.convert_png_to_quantized_png(png_data=fr.data, target_bit_depth=dev.display_bit_depth,
      expected_width=dev.display_width, expected_height=dev.display_height)
im=Image.open(io.BytesIO(out)); print(im.format, im.mode, im.size, len(out), "bytes")
PY'
```

Inspect LVGL decode internals:
```
docker exec Zephyr-Introduction bash -lc 'sed -n "5740,5870p" \
  /opt/toolchains/modules/lib/gui/lvgl/src/libs/lodepng/lodepng.c'
```

## Key source locations

- Device decode/render: `src/display_thread.c`
  - `display_png_frame_locked()` ~210 (sets frame_dsc, cf=L8 cosmetic bug)
  - `ui_init()` ~292 (`lv_lodepng_init()` call — register custom decoder here)
  - triple buffer slots/submit ~64–80, ~338–368 (correct, leave alone)
- Device fetch: `src/llss_thread.c do_fetch_frame()` ~712; `src/llss_client.c`
  `llss_fetch_frame()` ~1119, `http_response_cb()` ~483, `do_request()` ~540.
- Backend frame endpoint: `eink_llss/app/routers/devices.py get_frame()` ~244
  (default path = quantized PNG; `?raw=true` = packed framebuffer — unused by device now).
- Backend converter: `eink_llss/app/frame_converter.py convert_to_quantized_png()` ~248.
- LVGL (container): `.../lvgl/src/libs/lodepng/lv_lodepng.c`,
  `.../lodepng/lodepng.c` (patched), `.../draw/lv_image_decoder.c` (decoder list/order),
  `.../draw/lv_draw_buf.h` (`lv_draw_buf_create` / `_ex`).

---

# ALTERNATIVE ROUTE C: raw packed framebuffer (no PNG decode at all)

Strongly worth considering — likely the **cleanest** fix. Skip PNG/LVGL decode entirely:
ask the backend for the already-packed panel framebuffer and `memcpy` it into the panel's
RAM plane. Decode memory drops to **~0** (no lodepng, no ARGB8888, no pool bump).

## Why this is compelling: the byte formats already match exactly

The backend `?raw=true` output was **designed for this exact panel**. Confirmed:

| | Backend `?raw=true` (`frame_converter.py`) | Device driver (`display_ssd16xx_800x480.c`) |
|---|---|---|
| 1bpp (bit_depth=1) | `convert_to_1bit_packed` → `(800*480+7)//8` = **48000 B**, MSB-first | `bw_plane` = `CUSTOM_SSD16XX_BUF_BYTES` = **48000 B** (100 B/row × 480), MSB = col 0 |
| 2bpp (bit_depth=2) | `convert_to_2bit_planes` → MSB plane (→reg 0x24) ‖ LSB plane (→reg 0x26), 2×6000... = 2×48000 | `bw_plane` (→0x24) + `red_plane` (→0x26), each 48000 B |

So for **MONO/1bpp** the device just does: receive 48000 bytes → `memcpy` into
`data->bw_plane` → refresh. Byte-for-byte identical, no transform. The backend's own
docstring even says "First plane (MSB): Written to EPD register 0x24" — it was written
against this driver.

> **Plane-order caveat (2bpp only):** double-check bit mapping. Backend uses
> `msb = level>>1`, `lsb = level&1`; driver packs `red = (level>>1)&1` (→0x26),
> `bw = level&1` (→0x24). So backend "MSB plane" ↔ driver `red_plane` (0x26), backend
> "LSB plane" ↔ driver `bw_plane` (0x24). For 1bpp this caveat doesn't apply.

## Backend: already supports it, zero changes needed

`eink_llss/app/routers/devices.py get_frame()` ~244: `GET /devices/{id}/frames/{fid}?raw=true`
→ `convert_png_to_framebuffer(..., target_bit_depth=device.display_bit_depth)` →
`application/octet-stream`. The device currently fetches **without** `?raw`, getting a PNG.

**Implication:** the raw route only makes sense at **bit_depth 1 or 2** (the panel's native
modes: MONO 1bpp partial-capable, GRAY2 2bpp full-only). `bit_depth=4` raw returns 4-bit
packed (192000 B) the panel can't consume directly. The device is currently registered
**bit_depth=4** → must re-register as 1 (recommended, partial-refresh) or 2.

## Device changes

1. **`llss_fetch_frame`** (`src/llss_client.c:1119`): append `?raw=true` to the path. Buffer
   stays the same triple-buffer slot (48000 B ≪ 64 KB `CONFIG_LLSS_FRAME_BUF_SIZE`).
2. **New driver entry point** in `display_ssd16xx_800x480.c` + `custom_ssd16xx.h`, e.g.
   `int custom_ssd16xx_load_packed(const struct device *dev, const uint8_t *bw,
   const uint8_t *red /*NULL for mono*/, size_t len)`: validate `len == 48000`, `memcpy`
   into `bw_plane` (and `red_plane`), then call the existing
   `custom_ssd16xx_refresh_partial/full`. (The existing `custom_ssd16xx_write()` at ~530 is
   the LVGL flush path that converts L8→planes with dithering — bypass it; we already have
   packed bits.)
3. **`display_thread.c`**: replace the lodepng image path for server frames with a call to
   the new driver entry point. The server frame is **full-screen** (no LVGL compositing
   needed on it), so this is clean for the main screen.

## The real tension to resolve (LVGL coexistence)

LVGL owns the display and its VDB; device-local UI screens (log/network/clock in
`device_ui.c`) render *through* LVGL → `custom_ssd16xx_write()` → planes. If we also blit
raw server frames straight into `bw_plane` behind LVGL's back, the two writers fight over
the same plane + the driver's `prev_bw_plane` partial-refresh history.

Workable model (matches the existing architecture — server frame and device UI are already
*separate screens*, never composited):
- **Server-frame mode:** raw blit → `bw_plane` → refresh. Don't run `lv_task_handler`
  flush for the main screen.
- **Device-UI mode:** LVGL drives as today.
- On mode switch, force a full refresh (the partial-refresh `prev_bw_plane` baseline is
  invalid across a writer change — already handled by `UI_CTX_SWITCH`).
- Make sure LVGL doesn't repaint the main screen over the raw frame: e.g. keep the LVGL
  main screen empty/invalidated-suppressed while in server-frame mode, or hand the panel
  to the raw path and only re-init LVGL flush when entering device UI. **This is the part
  to design carefully** — biggest risk of the route.

## Route C vs Route A (L8 decoder) — decision aid

| | A: L8 decoder | C: raw framebuffer |
|---|---|---|
| Decode memory | ~384 KiB resident + ~760 KiB peak; pool bump to ~1 MiB | ~0 (48 KB packed, already in triple buf) |
| lodepng / LVGL patch fight | must find non-ARGB seam (open question) | avoided entirely |
| Wire size | ~8.5 KB PNG | 48 KB packed (bigger, but RTT/waveform dominate — see `feedback_no_transport_optimization`) |
| LVGL compositing of main frame | preserved | given up (fine: main frame is full-screen, UI is separate screens) |
| Gray support | easy (L8 = up to 256, panel shows 1/2bpp) | 1bpp now; 2bpp needs plane-order check |
| Main risk | lodepng seam | LVGL/raw display ownership handoff |
| Backend change | none | none (already supports `?raw`); device must re-register bit_depth 1/2 |
| Aligns with "dumb device" architecture | partially | **yes** ([[project_eink_architecture]]) |

**Lean:** Route C is the better long-term fit (no decode at all, matches the architecture,
backend already serves the exact bytes). Its only hard part is the LVGL-vs-raw display
ownership handoff. Route A is more self-contained but carries the lodepng-patch unknown and
keeps a big-ish heap. **Decide next session.** If we want frames on screen *fast* with
least risk, Route A + the modest pool bump may be the quicker first light; Route C is the
cleaner destination.

## Route C repro / verification

Dump exactly what the device would receive at bit_depth=1 (run as vscode user):
```
docker exec -u vscode dazzling_blackwell bash -lc 'cd /workspaces/eink_llss/app && \
  /usr/local/bin/python - <<PY
from database import SessionLocal; import db_models as M; import frame_converter as fc
db=SessionLocal(); dev=db.query(M.Device).first()
fr=db.query(M.Frame).order_by(M.Frame.id.desc()).first()
data,mt=fc.convert_png_to_framebuffer(png_data=fr.data, target_bit_depth=1,
        expected_width=dev.display_width, expected_height=dev.display_height)
print("bit_depth=1 raw:", len(data), "bytes", mt, "(expect 48000)")
PY'
```

Extra source locations for Route C:
- `eink_llss/app/frame_converter.py`: `convert_png_to_framebuffer()` ~187,
  `convert_to_1bit_packed()` ~? (1bpp), `convert_to_2bit_planes()` ~104,
  `get_expected_framebuffer_size()` ~328.
- Device driver: `src/display_ssd16xx_800x480.c` — `bw_plane`/`red_plane` ~91,
  `CUSTOM_SSD16XX_BUF_BYTES`=48000 ~68, `custom_ssd16xx_write()` (LVGL L8→planes flush)
  ~530, refresh fns ~358+; `src/custom_ssd16xx.h` (public API to extend).
