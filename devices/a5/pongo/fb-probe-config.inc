/*
 * P105 iPad mini 1 — 768x1024, confirmed via fb-probe.
 *
 * FB_PHYS      pre-scanout marker (64 px, NOT visible — tags RAM before panel)
 * scanout      FB_PHYS + MARKER_SKIP*4  (= 0x9f6fc000)
 * FB_WIDTH     768 px per row (769 wraps to next row)
 * FB_HEIGHT    1024 rows
 * STRIDE       FB_WIDTH * 4 = 3072
 *
 * ROW / PIXEL_OFF / PIXEL_COUNT — green line relative to scanout base.
 */
.set FB_PHYS,	0x9f6fbf00
.set MARKER_SKIP,	64
.set FB_WIDTH,	768
.set FB_HEIGHT,	1024
.set STRIDE,	3072
.set ROW,	1023
.set PIXEL_OFF,	0
.set PIXEL_COUNT,	768
.set USE_CLCD,	0
.set GREEN,	0xff00ff00
.set MARKER,	0xffff0000
