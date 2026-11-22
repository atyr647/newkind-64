/**
 *
 * Elite - The New Kind.
 *
 * Nintendo 64 version of Graphics routines using libdragon.
 *
 * N64 port: Software-rendered framebuffer at 640x480 (interlaced).
 * Based on the Allegro backend (alg_gfx.c) by C.J.Pinder.
 *
 **/

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include <libdragon.h>

#include "config.h"
#include "gfx.h"
#include "elite.h"
#include "n64_assets.h"
#include "n64_ships3d.h"

/* The 256-color palette, stored as RGBA5551 for direct write */
static uint16_t palette_rgba16[256];

/* Private framebuffer - persistent between frames for screens that draw once.
 * Flight views clear and redraw every frame; info screens persist.
 * Aligned to 16 bytes for optimal DMA/cache line operations. */
static uint16_t framebuf[N64_SCREEN_W * N64_SCREEN_H] __attribute__((aligned(16)));

/* Pointer used by all rendering functions. Points to framebuf. */
static uint16_t *active_fb = framebuf;

volatile int frame_count;
static timer_link_t *frame_timer_handle;

/* Clipping region */
static int clip_tx, clip_ty, clip_bx, clip_by;

/* Dashboard and icon bar art, loaded from the ROM filesystem and converted
 * to RGBA5551 once so they can be blitted with plain row copies. */
static uint16_t *scanner_rgba;   /* 512x160: flight icon bar + dashboard */
static int scanner_w, scanner_h;
static uint16_t *iconbar_rgba;   /* 512x96: docked, flight, charts icon bars */
static int iconbar_w, iconbar_h;

/* Palette loaded from scanner BMP */
static uint8_t bmp_palette[256][4]; /* RGBX */

/* Current layout (see gfx.h). */
int gfx_view_by = GFX_TEXT_VIEW_BY;
int gfx_y_centre = (GFX_TEXT_VIEW_BY + 1) / 2;
static int cur_layout = GFX_LAYOUT_TITLE;

/*
 * Presentation state.
 *
 * Normally everything is drawn into the private framebuf, which is copied to
 * a display surface when the frame is shown. That lets text screens be drawn
 * once and left alone. The space views redraw themselves completely every
 * frame, so for those the main loop asks (gfx_request_direct_frame) to draw
 * straight into the next display surface and skip the copy.
 */
static surface_t *cur_surface;   /* surface acquired for the frame being drawn */
static int direct_mode;          /* 1 = active_fb points into cur_surface */
static int direct_hint;          /* one-shot request for the next frame */
static int direct_frames;        /* consecutive direct frames so far */
#define NUM_DISPLAY_BUFFERS 3

/*
 * Hardware drawing (RDP).
 *
 * While a frame is being drawn directly into a display surface, the RDP is
 * attached to that surface and does the jobs it is far better at than the
 * CPU: wiping the view at the start of the frame, and filling the banded
 * planet. Everything else is still drawn by the CPU into the same surface,
 * so the two must never be drawing at once: hw_sync() waits for the RDP
 * to finish before the CPU carries on.
 */
static surface_t zbuffer;        /* depth buffer for the hardware-drawn ships */
static int rdp_attached;         /* RDP is attached to cur_surface */
static int rdp_busy;             /* RDP commands issued since the last sync */
static int view_precleared;      /* the RDP has already wiped this frame's view */

static void hw_sync(void)
{
	if (rdp_busy)
	{
		rspq_wait();
		rdp_busy = 0;
	}
}

/* Hooks for the hardware ship renderer (n64_ships3d.c). */
int gfx_hw_available(void)
{
	return direct_mode && rdp_attached;
}

void gfx_hw_mark_busy(void)
{
	rdp_busy = 1;
}

void gfx_hw_wait(void)
{
	hw_sync();
}

void gfx_palette_rgb(int index, int *r, int *g, int *b)
{
	*b = bmp_palette[index & 255][0];
	*g = bmp_palette[index & 255][1];
	*r = bmp_palette[index & 255][2];
}

static void hw_release(void)
{
	if (rdp_attached)
	{
		rdpq_detach_wait();
		rdp_attached = 0;
	}
	rdp_busy = 0;
	view_precleared = 0;
}

#define MAX_POLYS	100

static int start_poly;
static int total_polys;

struct poly_data
{
	int z;
	int no_points;
	int face_colour;
	int point_list[16];
	int next;
};

static struct poly_data poly_chain[MAX_POLYS];


/*
 * Load the scanner BMP from ROM filesystem and extract its palette.
 * The BMP is an 8-bit indexed color image (256 colors).
 * This gives us the authentic Elite palette used by all game graphics.
 */
static uint16_t *load_bmp(const char *filename, int *out_w, int *out_h, int set_palette)
{
	FILE *fp;
	uint8_t header[54];
	uint8_t pal[256][4];
	uint16_t pal16[256];
	uint8_t *rowbuf;
	uint16_t *out;
	int width, height, bpp, offset;
	int row, x, rowsize;
	char path[64];

	snprintf(path, sizeof(path), "rom:/%s", filename);
	fp = fopen(path, "rb");
	if (!fp)
		return NULL;

	if (fread(header, 1, 54, fp) != 54)
	{
		fclose(fp);
		return NULL;
	}

	offset = header[10] | (header[11] << 8) | (header[12] << 16) | (header[13] << 24);
	width = header[18] | (header[19] << 8) | (header[20] << 16) | (header[21] << 24);
	height = header[22] | (header[23] << 8) | (header[24] << 16) | (header[25] << 24);
	bpp = header[28] | (header[29] << 8);

	if (bpp != 8 || width <= 0 || height <= 0 || fread(pal, 4, 256, fp) != 256)
	{
		fclose(fp);
		return NULL;
	}

	/* Palette entries are BGRX; convert to RGBA5551. */
	for (x = 0; x < 256; x++)
	{
		int r5 = pal[x][2] >> 3, g5 = pal[x][1] >> 3, b5 = pal[x][0] >> 3;
		pal16[x] = (r5 << 11) | (g5 << 6) | (b5 << 1) | (x ? 1 : 0);
	}

	if (set_palette)
	{
		memcpy(bmp_palette, pal, sizeof(bmp_palette));
		memcpy(palette_rgba16, pal16, sizeof(palette_rgba16));
	}

	rowsize = (width + 3) & ~3;
	rowbuf = malloc(rowsize);
	out = malloc(width * height * sizeof(uint16_t));
	if (!rowbuf || !out)
	{
		free(rowbuf);
		free(out);
		fclose(fp);
		return NULL;
	}

	/* BMP rows are stored bottom-up. */
	fseek(fp, offset, SEEK_SET);
	for (row = height - 1; row >= 0; row--)
	{
		if (fread(rowbuf, 1, rowsize, fp) != (size_t)rowsize)
			break;
		for (x = 0; x < width; x++)
			out[row * width + x] = pal16[rowbuf[x]];
	}

	free(rowbuf);
	fclose(fp);
	*out_w = width;
	*out_h = height;
	return out;
}


static void init_default_palette(void)
{
	/* Fallback palette if BMP load fails - matches Elite's color scheme */
	memset(palette_rgba16, 0, sizeof(palette_rgba16));

	palette_rgba16[0]   = 0x0001;  /* Black (with alpha) - actually use 0 for transparent */
	palette_rgba16[0]   = 0x0000;  /* Black */
	palette_rgba16[1]   = (0x15 << 11) | (0x00 << 6) | (0x00 << 1) | 1; /* dark red */
	palette_rgba16[2]   = (0x00 << 11) | (0x1F << 6) | (0x00 << 1) | 1; /* green */
	palette_rgba16[4]   = (0x00 << 11) | (0x00 << 6) | (0x1F << 1) | 1; /* blue */
	palette_rgba16[11]  = (0x00 << 11) | (0x1F << 6) | (0x1F << 1) | 1; /* cyan */
	palette_rgba16[28]  = (0x18 << 11) | (0x00 << 6) | (0x00 << 1) | 1; /* dark red */
	palette_rgba16[39]  = (0x1F << 11) | (0x18 << 6) | (0x00 << 1) | 1; /* gold */
	palette_rgba16[49]  = (0x1F << 11) | (0x00 << 6) | (0x00 << 1) | 1; /* red */
	palette_rgba16[234] = (0x0D << 11) | (0x0D << 6) | (0x0D << 1) | 1; /* grey */
	palette_rgba16[235] = (0x10 << 11) | (0x10 << 6) | (0x10 << 1) | 1; /* grey */
	palette_rgba16[237] = (0x13 << 11) | (0x13 << 6) | (0x13 << 1) | 1; /* grey */
	palette_rgba16[248] = (0x09 << 11) | (0x09 << 6) | (0x09 << 1) | 1; /* grey */
	palette_rgba16[255] = (0x1F << 11) | (0x1F << 6) | (0x1F << 1) | 1; /* white */

	/* AA grey ramp */
	for (int i = 0; i < 8; i++)
	{
		int v = (i * 31) / 7;
		palette_rgba16[235 + i] = (v << 11) | (v << 6) | (v << 1) | 1;
	}
}


static void frame_timer_callback(int ovfl)
{
	frame_count++;
}


/* Inline pixel set with bounds checking and clipping.
 * Converts palette index to RGBA5551 on write. */
static inline void fb_putpixel(int x, int y, int col)
{
	if (x >= clip_tx && x <= clip_bx && y >= clip_ty && y <= clip_by)
		active_fb[y * N64_SCREEN_W + x] = palette_rgba16[col];
}

/* Fast 64-bit framebuffer copy. */
extern const uint8_t n64_font_8x8[];

static void fast_framebuf_copy(void *dst, const void *src, int nbytes)
{
	const uint64_t *s = (const uint64_t *)src;
	uint64_t *d = (uint64_t *)dst;
	int n = nbytes / 64;
	int i;
	for (i = 0; i < n; i++)
	{
		uint64_t a = s[0], b = s[1], c = s[2], d0 = s[3];
		uint64_t e = s[4], f = s[5], g = s[6], h = s[7];
		d[0] = a; d[1] = b; d[2] = c; d[3] = d0;
		d[4] = e; d[5] = f; d[6] = g; d[7] = h;
		s += 8;
		d += 8;
	}
}

int gfx_graphics_startup(void)
{
	/* 640x480 interlaced, 16-bit colour. */
	display_init(RESOLUTION_640x480, DEPTH_16_BPP, NUM_DISPLAY_BUFFERS, GAMMA_NONE, FILTERS_RESAMPLE);
	rdpq_init();
	zbuffer = surface_alloc(FMT_RGBA16, N64_SCREEN_W, N64_SCREEN_H);
	hw_ships_init();

	/* The dashboard BMP carries the game palette (the original Elite palette
	 * plus the private NES dashboard colours). */
	scanner_rgba = load_bmp(scanner_filename, &scanner_w, &scanner_h, 1);
	if (!scanner_rgba)
		scanner_rgba = load_bmp("scanner.bmp", &scanner_w, &scanner_h, 1);
	if (!scanner_rgba)
		init_default_palette();

	iconbar_rgba = load_bmp("iconbar.bmp", &iconbar_w, &iconbar_h, 0);

	/* Clear framebuffer */
	memset(framebuf, 0, sizeof(framebuf));
	active_fb = framebuf;
	cur_surface = NULL;
	direct_mode = direct_hint = direct_frames = 0;
	rdp_attached = rdp_busy = view_precleared = 0;

	/* Set default clip region to full screen */
	clip_tx = 0;
	clip_ty = 0;
	clip_bx = N64_SCREEN_W - 1;
	clip_by = N64_SCREEN_H - 1;

	/* Scanner and borders are drawn by update_console() during gameplay.
	 * Don't draw at startup - they'd show on the intro screens. */

	/* Setup frame timer for game speed regulation */
	frame_count = 0;
	frame_timer_handle = new_timer(TIMER_TICKS(speed_cap * 1000), TF_CONTINUOUS, frame_timer_callback);

	return 0;
}


void gfx_graphics_shutdown(void)
{
	free(scanner_rgba);
	scanner_rgba = NULL;
	free(iconbar_rgba);
	iconbar_rgba = NULL;
	display_close();
}


void gfx_acquire_screen(void)
{
	/* No-op - see gfx_update_screen */
}

/*
 * Display surfaces live in uncached memory. That is the right place to
 * copy whole frames to (a cached write would first have to read every
 * line in), so the frame copy goes straight there.
 *
 * For drawing directly into a surface, DIRECT_CACHED selects whether the
 * pixels are written through the cached alias (flushed before the surface
 * is shown) or straight to uncached memory.
 */
#ifndef DIRECT_CACHED
#define DIRECT_CACHED 0
#endif

static uint16_t *surface_pixels(surface_t *s)
{
#if DIRECT_CACHED
	return (uint16_t *)CachedAddr(s->buffer);
#else
	return (uint16_t *)s->buffer;
#endif
}

void gfx_request_direct_frame(void)
{
	direct_hint = 1;
}

/* Stop drawing into the display surface and go back to the private
 * framebuf, optionally carrying the picture drawn so far across. */
static void leave_direct_mode(int keep_picture)
{
	if (!direct_mode)
		return;
	hw_release();
	if (keep_picture)
		memcpy(framebuf, surface_pixels(cur_surface), sizeof(framebuf));
	active_fb = framebuf;
	direct_mode = 0;
	direct_frames = 0;
}

#ifdef N64_SHOW_FPS
/*
 * Developer overlay (make -f Makefile.n64 EXTRA_CFLAGS=-DN64_SHOW_FPS): frames per
 * second, measured in real time over the last 30 frames.
 */
static void draw_string(int px, int py, const char *txt, int col, int big);

static void draw_fps_overlay(void)
{
	static int frames, fps10;
	static long long t0;
	char buf[32];
	int y;
	int ctx = clip_tx, cty = clip_ty, cbx = clip_bx, cby = clip_by;

	if (++frames >= 30)
	{
		long long t = timer_ticks();
		if (t0)
			fps10 = (int)(300LL * TICKS_PER_SECOND / (t - t0));
		t0 = t;
		frames = 0;
	}

	snprintf(buf, sizeof(buf), "%d.%d fps L%d C%d", fps10 / 10, fps10 % 10, cur_layout, gfx_y_centre);
	for (y = 14; y < 26; y++)
		memset(&active_fb[y * N64_SCREEN_W + 70], 0, 160 * sizeof(uint16_t));
	clip_tx = 0; clip_ty = 0; clip_bx = N64_SCREEN_W - 1; clip_by = N64_SCREEN_H - 1;
	draw_string(72, 13, buf, GFX_COL_WHITE, 0);
	clip_tx = ctx; clip_ty = cty; clip_bx = cbx; clip_by = cby;
}
#endif

void gfx_update_screen(void)
{
	extern void snd_update_sound(void);
	int want_direct = direct_hint;

	direct_hint = 0;

#ifdef N64_SHOW_FPS
	draw_fps_overlay();
#endif

	/* Keep the audio mixer fed from every loop that shows frames. */
	snd_update_sound();

	/* display_get() blocks until a buffer is free (~vsync locked). */
	if (!cur_surface)
		cur_surface = display_get();

	if (direct_mode)
	{
		hw_release();
#if DIRECT_CACHED
		data_cache_hit_writeback_invalidate(surface_pixels(cur_surface), sizeof(framebuf));
#endif
		if (!want_direct)
			leave_direct_mode(1);
	}
	else
	{
		fast_framebuf_copy(cur_surface->buffer, framebuf, sizeof(framebuf));
	}

	display_show(cur_surface);
	cur_surface = NULL;

	if (want_direct)
	{
		cur_surface = display_get();
		active_fb = surface_pixels(cur_surface);
		direct_mode = 1;

		/* Let the RDP wipe the view while the CPU gets on with reading the
		 * controller and running the simulation. Each display buffer still
		 * holds whatever was shown a few frames ago, so the first time one
		 * is used directly the whole screen is wiped instead. */
		rdpq_attach(cur_surface, &zbuffer);
		rdp_attached = 1;
		rdpq_set_mode_fill(RGBA32(0, 0, 0, 255));
		if (direct_frames < NUM_DISPLAY_BUFFERS || cur_layout != GFX_LAYOUT_SPACE)
		{
			rdpq_fill_rectangle(0, 0, N64_SCREEN_W, N64_SCREEN_H);
			if (direct_frames < NUM_DISPLAY_BUFFERS)
				direct_frames++;
		}
		else
		{
			rdpq_fill_rectangle(GFX_X_OFFSET, GFX_Y_OFFSET,
			                    GFX_X_OFFSET + 512, GFX_Y_OFFSET + GFX_SPACE_VIEW_BY + 1);
		}
		rdp_busy = 1;
		view_precleared = 1;
	}
}


/*
 * Switch between the SPACE and TEXT layouts (see gfx.h). Changing layout
 * wipes the whole play field so nothing from the old layout is left behind.
 */
void gfx_set_layout(int layout)
{
	int was_space = (cur_layout == GFX_LAYOUT_SPACE);
	int is_space = (layout == GFX_LAYOUT_SPACE);

	if (layout == cur_layout)
		return;

	/* Text screens are drawn once and kept, so they need the framebuf.
	 * (The title screens redraw every frame, like the space views.) */
	if (layout == GFX_LAYOUT_TEXT)
		leave_direct_mode(0);

	cur_layout = layout;
	gfx_view_by = is_space ? GFX_SPACE_VIEW_BY : GFX_TEXT_VIEW_BY;
	gfx_y_centre = (gfx_view_by + 1) / 2;

	if (was_space != is_space || layout == GFX_LAYOUT_TITLE)
	{
		int y;
		hw_sync();
		view_precleared = 0;
		for (y = 0; y < N64_SCREEN_H; y++)
			memset(&active_fb[y * N64_SCREEN_W + GFX_X_OFFSET], 0, 512 * sizeof(uint16_t));
	}

	gfx_set_clip_region(1, 1, 510, gfx_view_by);
}

int gfx_get_layout(void)
{
	return cur_layout;
}


void gfx_release_screen(void)
{
	/* No-op on N64 - release is handled by gfx_update_screen */
}


/*
 * Fast paths for the planet and sun renderers, which fill whole spans:
 * they look colours up themselves and hand over finished pixels a row
 * at a time. No clipping - the caller has already clipped to the view.
 */
const uint16_t *gfx_palette16(void)
{
	return palette_rgba16;
}

void gfx_blit_span16(int x, int y, const uint16_t *pixels, int count)
{
	memcpy(&active_fb[y * N64_SCREEN_W + x], pixels, count * sizeof(uint16_t));
}

/*
 * Banded planet, drawn by the RDP.
 *
 * The "SNES" planet is a disc of parallel colour bands: along any screen
 * line the band number changes linearly. That is exactly a one-dimensional
 * texture stretched along the line, so each line of the disc becomes one
 * textured rectangle and the RDP fills the pixels.
 *
 * gfx_band_planet_begin() returns 0 when hardware drawing is not available
 * for this frame (the caller then uses the software renderer).
 */
#define BAND_TEX_W		512
#define BAND_TEX_SCALE	(BAND_TEX_W / 128)

static uint16_t band_tex[BAND_TEX_W * 2] __attribute__((aligned(16)));

int gfx_band_planet_begin(const unsigned char *bands, int count)
{
	surface_t tex;
	int i;

	if (!direct_mode || !rdp_attached || count <= 0)
		return 0;

	hw_sync();

	for (i = 0; i < BAND_TEX_W; i++)
	{
		int b = i / BAND_TEX_SCALE;
		if (b >= count) b = count - 1;
		band_tex[i] = band_tex[BAND_TEX_W + i] = palette_rgba16[bands[b]] | 1;
	}
	data_cache_hit_writeback(band_tex, sizeof(band_tex));

	tex = surface_make_linear(band_tex, FMT_RGBA16, BAND_TEX_W, 2);

	rdpq_set_mode_standard();
	rdpq_mode_filter(FILTER_POINT);
	rdpq_mode_combiner(RDPQ_COMBINER_TEX);
	rdpq_tex_upload(TILE0, &tex, NULL);
	rdp_busy = 1;
	return 1;
}

/* One line of the disc: count pixels from (x, y); band is the (fractional)
 * band number at the first pixel and band_step how much it changes per pixel. */
void gfx_band_planet_span(int x, int y, int count, float band, float band_step)
{
	float s0 = band * BAND_TEX_SCALE;
	float s1 = s0 + band_step * BAND_TEX_SCALE * count;

	rdpq_texture_rectangle_scaled(TILE0, x, y, x + count, y + 1, s0, 0, s1, 1);
}

void gfx_band_planet_end(void)
{
	hw_sync();
}

void gfx_fast_plot_pixel(int x, int y, int col)
{
	/* No bounds check - caller is responsible. Used by planet renderer. */
	active_fb[y * N64_SCREEN_W + x] = palette_rgba16[col];
}


void gfx_plot_pixel(int x, int y, int col)
{
	fb_putpixel(x + GFX_X_OFFSET, y + GFX_Y_OFFSET, col);
}


void gfx_draw_filled_circle(int cx, int cy, int radius, int circle_colour)
{
	int y, r2;

	cx += GFX_X_OFFSET;
	cy += GFX_Y_OFFSET;

	if (radius <= 0) return;
	r2 = radius * radius;

	/* Scanline fill: for each row, compute the horizontal span */
	for (y = -radius; y <= radius; y++)
	{
		/* x range where x*x + y*y <= r*r  =>  x = sqrt(r2 - y*y) */
		int y2 = y * y;
		int xspan = 0;
		while (xspan * xspan + y2 <= r2) xspan++;
		xspan--;

		/* Draw horizontal line from cx-xspan to cx+xspan */
		{
			int py = cy + y;
			int sx = cx - xspan;
			int ex = cx + xspan;
			if (py >= clip_ty && py <= clip_by)
			{
				if (sx < clip_tx) sx = clip_tx;
				if (ex > clip_bx) ex = clip_bx;
				if (sx <= ex)
				{
					uint16_t c16 = palette_rgba16[circle_colour];
					uint16_t *row = &active_fb[py * N64_SCREEN_W + sx];
					int len = ex - sx + 1;
					if ((((uintptr_t)row) & 2) && len > 0) { *row++ = c16; len--; }
					{
						uint32_t c32 = ((uint32_t)c16 << 16) | c16;
						uint32_t *r32 = (uint32_t *)row;
						int pairs = len >> 1;
						int j;
						for (j = 0; j < pairs; j++) r32[j] = c32;
						row += pairs * 2;
						len -= pairs * 2;
					}
					if (len > 0) *row = c16;
				}
			}
		}
	}
}


/* Bresenham circle */
void gfx_draw_circle(int cx, int cy, int radius, int circle_colour)
{
	int x = 0, y = radius;
	int d = 3 - 2 * radius;

	cx += GFX_X_OFFSET;
	cy += GFX_Y_OFFSET;

	while (y >= x)
	{
		fb_putpixel(cx + x, cy + y, circle_colour);
		fb_putpixel(cx - x, cy + y, circle_colour);
		fb_putpixel(cx + x, cy - y, circle_colour);
		fb_putpixel(cx - x, cy - y, circle_colour);
		fb_putpixel(cx + y, cy + x, circle_colour);
		fb_putpixel(cx - y, cy + x, circle_colour);
		fb_putpixel(cx + y, cy - x, circle_colour);
		fb_putpixel(cx - y, cy - x, circle_colour);

		x++;
		if (d > 0)
		{
			y--;
			d = d + 4 * (x - y) + 10;
		}
		else
		{
			d = d + 4 * x + 6;
		}
	}
}


/* Bresenham line drawing */
static void draw_line_clipped(int x1, int y1, int x2, int y2, int col)
{
	int dx = abs(x2 - x1);
	int dy = abs(y2 - y1);
	int sx = (x1 < x2) ? 1 : -1;
	int sy = (y1 < y2) ? 1 : -1;
	int err = dx - dy;
	int e2;

	for (;;)
	{
		fb_putpixel(x1, y1, col);
		if (x1 == x2 && y1 == y2)
			break;
		e2 = 2 * err;
		if (e2 > -dy)
		{
			err -= dy;
			x1 += sx;
		}
		if (e2 < dx)
		{
			err += dx;
			y1 += sy;
		}
	}
}


/*
 * Optimized horizontal line - fills with 32-bit writes (2 pixels at a time).
 * This is the single hottest function in the renderer.
 */
static void draw_hline(int x1, int x2, int y, int col)
{
	int tmp;
	uint16_t c16;
	uint16_t *row;
	if (x1 > x2) { tmp = x1; x1 = x2; x2 = tmp; }
	if (y < clip_ty || y > clip_by) return;
	if (x1 < clip_tx) x1 = clip_tx;
	if (x2 > clip_bx) x2 = clip_bx;
	if (x1 > x2) return;

	c16 = palette_rgba16[col];
	row = &active_fb[y * N64_SCREEN_W];

	/* Align to 32-bit boundary */
	if ((x1 & 1) && x1 <= x2)
		row[x1++] = c16;

	/* Fill 2 pixels at a time with 32-bit writes */
	{
		uint32_t c32 = ((uint32_t)c16 << 16) | c16;
		uint32_t *row32 = (uint32_t *)&row[x1];
		int pairs = (x2 - x1 + 1) >> 1;
		int i;
		for (i = 0; i < pairs; i++)
			row32[i] = c32;
		x1 += pairs * 2;
	}

	/* Handle remaining pixel */
	if (x1 <= x2)
		row[x1] = c16;
}


/* Vertical line */
static void draw_vline(int x, int y1, int y2, int col)
{
	int tmp;
	uint16_t c16;
	if (y1 > y2) { tmp = y1; y1 = y2; y2 = tmp; }
	if (x < clip_tx || x > clip_bx) return;
	if (y1 < clip_ty) y1 = clip_ty;
	if (y2 > clip_by) y2 = clip_by;
	c16 = palette_rgba16[col];
	for (; y1 <= y2; y1++)
		active_fb[y1 * N64_SCREEN_W + x] = c16;
}


void gfx_draw_line(int x1, int y1, int x2, int y2)
{
	x1 += GFX_X_OFFSET;
	y1 += GFX_Y_OFFSET;
	x2 += GFX_X_OFFSET;
	y2 += GFX_Y_OFFSET;

	if (y1 == y2)
	{
		draw_hline(x1, x2, y1, GFX_COL_WHITE);
		return;
	}
	if (x1 == x2)
	{
		draw_vline(x1, y1, y2, GFX_COL_WHITE);
		return;
	}

	draw_line_clipped(x1, y1, x2, y2, GFX_COL_WHITE);
}


void gfx_draw_colour_line(int x1, int y1, int x2, int y2, int line_colour)
{
	x1 += GFX_X_OFFSET;
	y1 += GFX_Y_OFFSET;
	x2 += GFX_X_OFFSET;
	y2 += GFX_Y_OFFSET;

	if (y1 == y2)
	{
		draw_hline(x1, x2, y1, line_colour);
		return;
	}
	if (x1 == x2)
	{
		draw_vline(x1, y1, y2, line_colour);
		return;
	}

	draw_line_clipped(x1, y1, x2, y2, line_colour);
}


/* Software triangle fill using scanline */
void gfx_draw_triangle(int x1, int y1, int x2, int y2, int x3, int y3, int col)
{
	int tmp;
	int y;

	x1 += GFX_X_OFFSET; y1 += GFX_Y_OFFSET;
	x2 += GFX_X_OFFSET; y2 += GFX_Y_OFFSET;
	x3 += GFX_X_OFFSET; y3 += GFX_Y_OFFSET;

	/* Sort vertices by y */
	if (y1 > y2) { tmp=x1; x1=x2; x2=tmp; tmp=y1; y1=y2; y2=tmp; }
	if (y1 > y3) { tmp=x1; x1=x3; x3=tmp; tmp=y1; y1=y3; y3=tmp; }
	if (y2 > y3) { tmp=x2; x2=x3; x3=tmp; tmp=y2; y2=y3; y3=tmp; }

	if (y3 == y1) {
		int minx = x1 < x2 ? (x1 < x3 ? x1 : x3) : (x2 < x3 ? x2 : x3);
		int maxx = x1 > x2 ? (x1 > x3 ? x1 : x3) : (x2 > x3 ? x2 : x3);
		draw_hline(minx, maxx, y1, col);
		return;
	}

	for (y = y1; y <= y3; y++)
	{
		int xa, xb;
		/* Edge from (x1,y1) to (x3,y3) is always active */
		xa = x1 + (x3 - x1) * (y - y1) / (y3 - y1);

		if (y < y2)
		{
			if (y2 == y1) xb = x1;
			else xb = x1 + (x2 - x1) * (y - y1) / (y2 - y1);
		}
		else
		{
			if (y3 == y2) xb = x2;
			else xb = x2 + (x3 - x2) * (y - y2) / (y3 - y2);
		}

		draw_hline(xa, xb, y, col);
	}
}


void gfx_draw_rectangle(int tx, int ty, int bx, int by, int col)
{
	int y;
	tx += GFX_X_OFFSET;
	ty += GFX_Y_OFFSET;
	bx += GFX_X_OFFSET;
	by += GFX_Y_OFFSET;

	for (y = ty; y <= by; y++)
		draw_hline(tx, bx, y, col);
}


/*
 * Embedded bitmap font - 8x8 pixel, ASCII 32-127.
 * Used for both ELITE_1 (normal 8px text) and ELITE_2 (title text).
 * At 640x480 the 8x8 font is a faithful representation for both sizes.
 */
/*
 * Draw one glyph of the 8x8 font, optionally doubled to 16x16.
 */
static void draw_char(int x, int y, char ch, int col, int big)
{
	int cx, cy;
	const uint8_t *glyph;
	int idx = (unsigned char)ch;

	if (idx < 32 || idx > 127) return;
	glyph = &n64_font_8x8[(idx - 32) * 8];

	for (cy = 0; cy < 8; cy++)
	{
		uint8_t row = glyph[cy];
		if (!row)
			continue;
		for (cx = 0; cx < 8; cx++)
		{
			if (!(row & (0x80 >> cx)))
				continue;
			if (big)
			{
				int px = x + cx * 2, py = y + cy * 2;
				fb_putpixel(px, py, col);
				fb_putpixel(px + 1, py, col);
				fb_putpixel(px, py + 1, col);
				fb_putpixel(px + 1, py + 1, col);
			}
			else
				fb_putpixel(x + cx, y + cy, col);
		}
	}
}

/*
 * The original text screens were laid out for a font about 14 pixels tall
 * (highlight bars span y-1..y+14). The 8x8 font is dropped by this much so
 * it sits in the middle of that line instead of hugging the top of it.
 */
#define SMALL_TEXT_DROP	3

static void draw_string(int px, int py, const char *txt, int col, int big)
{
	int step = big ? 16 : 8;

	if (!big)
		py += SMALL_TEXT_DROP;

	while (*txt)
	{
		draw_char(px, py, *txt, col, big);
		px += step;
		txt++;
	}
}


void gfx_display_text(int x, int y, char *txt)
{
	draw_string((x / (2 / GFX_SCALE)) + GFX_X_OFFSET,
	            (y / (2 / GFX_SCALE)) + GFX_Y_OFFSET, txt, GFX_COL_WHITE, 0);
}


void gfx_display_colour_text(int x, int y, char *txt, int col)
{
	draw_string((x / (2 / GFX_SCALE)) + GFX_X_OFFSET,
	            (y / (2 / GFX_SCALE)) + GFX_Y_OFFSET, txt, col, 0);
}


/*
 * Centred text. psize 140 is the original's large gold heading font; here
 * that is the same font doubled (as long as the line fits), which is also
 * how the NES draws all its text.
 */
void gfx_display_centre_text(int y, char *str, int psize, int col)
{
	int len = strlen(str);
	int big = (psize == 140) && (len * 16 <= 500);
	int py = (y / (2 / GFX_SCALE)) + GFX_Y_OFFSET;
	int px = (128 * GFX_SCALE) + GFX_X_OFFSET - (len * (big ? 8 : 4));

	if (psize == 140)
		col = GFX_COL_GOLD;

	draw_string(px, py, str, col, big);
}


/* Fast zero-fill a region of the framebuffer. */
static void fast_clear_region(int x1, int y1, int x2, int y2)
{
	int y;
	int w_bytes = (x2 - x1 + 1) * 2;
	for (y = y1; y <= y2; y++)
		memset(&active_fb[y * N64_SCREEN_W + x1], 0, w_bytes);
}

/*
 * Pick the layout that goes with the screen about to be drawn. Every screen
 * starts by calling gfx_clear_display(), so this is the one place that has
 * to know which screens use which layout.
 */
static void sync_layout(void)
{
	switch (current_screen)
	{
		case SCR_FRONT_VIEW:
		case SCR_REAR_VIEW:
		case SCR_LEFT_VIEW:
		case SCR_RIGHT_VIEW:
		case SCR_BREAK_PATTERN:
		case SCR_ESCAPE_POD:
		case SCR_GAME_OVER:
			gfx_set_layout(GFX_LAYOUT_SPACE);
			break;

		case SCR_INTRO_ONE:
		case SCR_INTRO_TWO:
			gfx_set_layout(GFX_LAYOUT_TITLE);
			break;

		default:
			gfx_set_layout(GFX_LAYOUT_TEXT);
			break;
	}
}

void gfx_clear_display(void)
{
	sync_layout();

	if (cur_layout == GFX_LAYOUT_SPACE)
	{
		if (direct_mode && view_precleared)
		{
			/* Already wiped by the RDP when the frame was started. */
			hw_sync();
			view_precleared = 0;
		}
		else
		{
			hw_sync();
			fast_clear_region(GFX_X_OFFSET, GFX_Y_OFFSET,
			                  GFX_X_OFFSET + 511, GFX_Y_OFFSET + GFX_SPACE_VIEW_BY);
		}
	}
	else if (direct_mode && view_precleared)
	{
		hw_sync();
		view_precleared = 0;
	}
	else
	{
		/* Everything above the icon bar. */
		hw_sync();
		fast_clear_region(GFX_X_OFFSET, GFX_Y_OFFSET,
		                  GFX_X_OFFSET + 511, GFX_Y_OFFSET + ICON_BAR_TEXT_Y - 1);
	}
}

/* The two status lines at the bottom of the charts and market screens. */
void gfx_clear_text_area(void)
{
	fast_clear_region(GFX_X_OFFSET + 1, GFX_Y_OFFSET + gfx_view_by - 43,
	                  GFX_X_OFFSET + 510, GFX_Y_OFFSET + gfx_view_by);
}

void gfx_clear_area(int tx, int ty, int bx, int by)
{
	tx += GFX_X_OFFSET;
	ty += GFX_Y_OFFSET;
	bx += GFX_X_OFFSET;
	by += GFX_Y_OFFSET;

	if (tx < 0) tx = 0;
	if (ty < 0) ty = 0;
	if (bx >= N64_SCREEN_W) bx = N64_SCREEN_W - 1;
	if (by >= N64_SCREEN_H) by = N64_SCREEN_H - 1;

	fast_clear_region(tx, ty, bx, by);
}


void gfx_display_pretty_text(int tx, int ty, int bx, int by, char *txt)
{
	char strbuf[100];
	char *str;
	char *bptr;
	int len;
	int pos;
	int maxlen;

	maxlen = (bx - tx) / 8;

	str = txt;
	len = strlen(txt);

	while (len > 0)
	{
		pos = maxlen;
		if (pos > len)
			pos = len;

		while ((str[pos] != ' ') && (str[pos] != ',') &&
		       (str[pos] != '.') && (str[pos] != '\0'))
		{
			pos--;
		}

		len = len - pos - 1;

		for (bptr = strbuf; pos >= 0; pos--)
			*bptr++ = *str++;

		*bptr = '\0';

		gfx_display_text(tx, ty, strbuf);
		ty += (8 * GFX_SCALE);
	}
}


static void blit_rows(const uint16_t *src, int w, int rows, int dst_y)
{
	int y;

	if (w > 512)
		w = 512;
	for (y = 0; y < rows; y++, dst_y++)
	{
		if (dst_y < 0 || dst_y >= N64_SCREEN_H)
			continue;
		memcpy(&active_fb[dst_y * N64_SCREEN_W + GFX_X_OFFSET], src + y * w, w * sizeof(uint16_t));
	}
}

/* Draw the dashboard (flight icon bar, gauges and scanner). */
void gfx_draw_scanner(void)
{
	if (scanner_rgba)
		blit_rows(scanner_rgba, scanner_w, scanner_h, SCANNER_Y + GFX_Y_OFFSET);
}

/*
 * Blank out one button of the icon bar just drawn at line y, for functions
 * that cannot be used right now (as the NES does). The buttons are 40
 * pixels apart; the docked bar's unused tenth button supplies the blank.
 */
static int icon_face_x(int slot)
{
	return slot * 40 + 16;
}

void gfx_blank_icon(int slot, int y)
{
	int row;

	if (!iconbar_rgba || slot < 0 || slot > 11 || iconbar_w < 512)
		return;

	for (row = 0; row < ICON_BAR_H; row++)
	{
		int dy = y + GFX_Y_OFFSET + row;
		if (dy < 0 || dy >= N64_SCREEN_H)
			continue;
		memcpy(&active_fb[dy * N64_SCREEN_W + GFX_X_OFFSET + icon_face_x(slot)],
		       &iconbar_rgba[row * iconbar_w + icon_face_x(9)], 40 * sizeof(uint16_t));
	}
}

/* Draw one of the icon bars (0 = docked, 1 = flight, 2 = charts) at line y. */
void gfx_draw_icon_bar(int type, int y)
{
	if (!iconbar_rgba || type < 0 || (type + 1) * ICON_BAR_H > iconbar_h)
		return;
	blit_rows(iconbar_rgba + type * ICON_BAR_H * iconbar_w, iconbar_w, ICON_BAR_H, y + GFX_Y_OFFSET);
}


void gfx_set_clip_region(int tx, int ty, int bx, int by)
{
	clip_tx = tx + GFX_X_OFFSET;
	clip_ty = ty + GFX_Y_OFFSET;
	clip_bx = bx + GFX_X_OFFSET;
	clip_by = by + GFX_Y_OFFSET;

	/* Clamp */
	if (clip_tx < 0) clip_tx = 0;
	if (clip_ty < 0) clip_ty = 0;
	if (clip_bx >= N64_SCREEN_W) clip_bx = N64_SCREEN_W - 1;
	if (clip_by >= N64_SCREEN_H) clip_by = N64_SCREEN_H - 1;
}


void gfx_draw_borders(void)
{
	/* NES dashboard has its own border tiles - no vanilla border lines needed.
	 * The NES view area has no white border frame. */
}


void gfx_start_render(void)
{
	start_poly = 0;
	total_polys = 0;
}


void gfx_render_polygon(int num_points, int *point_list, int face_colour, int zavg)
{
	int i;
	int x;
	int nx;

	if (total_polys == MAX_POLYS)
		return;

	x = total_polys;
	total_polys++;

	poly_chain[x].no_points = num_points;
	poly_chain[x].face_colour = face_colour;
	poly_chain[x].z = zavg;
	poly_chain[x].next = -1;

	for (i = 0; i < 16; i++)
		poly_chain[x].point_list[i] = point_list[i];

	if (x == 0)
		return;

	if (zavg > poly_chain[start_poly].z)
	{
		poly_chain[x].next = start_poly;
		start_poly = x;
		return;
	}

	for (i = start_poly; poly_chain[i].next != -1; i = poly_chain[i].next)
	{
		nx = poly_chain[i].next;

		if (zavg > poly_chain[nx].z)
		{
			poly_chain[i].next = x;
			poly_chain[x].next = nx;
			return;
		}
	}

	poly_chain[i].next = x;
}


void gfx_render_line(int x1, int y1, int x2, int y2, int dist, int col)
{
	int point_list[4];

	point_list[0] = x1;
	point_list[1] = y1;
	point_list[2] = x2;
	point_list[3] = y2;

	gfx_render_polygon(2, point_list, col, dist);
}


void gfx_finish_render(void)
{
	int num_points;
	int *pl;
	int i;
	int col;

	if (total_polys == 0)
	{
		hw_ships_flush();
		return;
	}

	for (i = start_poly; i != -1; i = poly_chain[i].next)
	{
		num_points = poly_chain[i].no_points;
		pl = poly_chain[i].point_list;
		col = poly_chain[i].face_colour;

		if (num_points == 2)
		{
			gfx_draw_colour_line(pl[0], pl[1], pl[2], pl[3], col);
			continue;
		}

		gfx_polygon(num_points, pl, col);
	};

	hw_ships_flush();
}


/* Software polygon fill using scanline rasterization */
void gfx_polygon(int num_points, int *poly_list, int face_colour)
{
	int i;
	int x, y;
	int min_y, max_y;
	int scanline_min[N64_SCREEN_H];
	int scanline_max[N64_SCREEN_H];
	int j;

	x = 0;
	y = 1;
	for (i = 0; i < num_points; i++)
	{
		poly_list[x] += GFX_X_OFFSET;
		poly_list[y] += GFX_Y_OFFSET;
		x += 2;
		y += 2;
	}

	/* Find Y extents */
	min_y = N64_SCREEN_H;
	max_y = 0;
	for (i = 0; i < num_points; i++)
	{
		int py = poly_list[i * 2 + 1];
		if (py < min_y) min_y = py;
		if (py > max_y) max_y = py;
	}

	if (min_y < clip_ty) min_y = clip_ty;
	if (max_y > clip_by) max_y = clip_by;
	if (min_y > max_y) return;

	/* Init scanline bounds */
	for (i = min_y; i <= max_y; i++)
	{
		scanline_min[i] = N64_SCREEN_W;
		scanline_max[i] = 0;
	}

	/* Trace all edges */
	for (i = 0; i < num_points; i++)
	{
		int x1 = poly_list[i * 2];
		int y1 = poly_list[i * 2 + 1];
		j = (i + 1) % num_points;
		int x2 = poly_list[j * 2];
		int y2 = poly_list[j * 2 + 1];

		int dy = abs(y2 - y1);
		int dx = abs(x2 - x1);
		int sx = (x1 < x2) ? 1 : -1;
		int sy = (y1 < y2) ? 1 : -1;
		int err = dx - dy;
		int cx = x1, cy = y1;

		for (;;)
		{
			if (cy >= min_y && cy <= max_y)
			{
				if (cx < scanline_min[cy]) scanline_min[cy] = cx;
				if (cx > scanline_max[cy]) scanline_max[cy] = cx;
			}
			if (cx == x2 && cy == y2) break;
			int e2 = 2 * err;
			if (e2 > -dy) { err -= dy; cx += sx; }
			if (e2 < dx) { err += dx; cy += sy; }
		}
	}

	/* Fill scanlines */
	for (i = min_y; i <= max_y; i++)
	{
		if (scanline_min[i] <= scanline_max[i])
		{
			int sx = scanline_min[i];
			int ex = scanline_max[i];
			if (sx < clip_tx) sx = clip_tx;
			if (ex > clip_bx) ex = clip_bx;
			if (sx <= ex)
				{
					uint16_t c16 = palette_rgba16[face_colour];
					uint16_t *row = &active_fb[i * N64_SCREEN_W + sx];
					int len = ex - sx + 1;
					/* 32-bit fill for speed */
					if ((((uintptr_t)row) & 2) && len > 0) { *row++ = c16; len--; }
					{
						uint32_t c32 = ((uint32_t)c16 << 16) | c16;
						uint32_t *r32 = (uint32_t *)row;
						int pairs = len >> 1;
						int j;
						for (j = 0; j < pairs; j++) r32[j] = c32;
						row += pairs * 2;
						len -= pairs * 2;
					}
					if (len > 0) *row = c16;
				}
		}
	}
}


/*
 * Sprite data lookup: n64_sprite_data[], n64_sprite_widths[],
 * n64_sprite_heights[] are provided by n64_spritedata.c (generated
 * from .sprite files by tools/convert_assets.py) and declared in
 * n64_assets.h.  Arrays are indexed by alg_data.h constants
 * (BLAKE=0, ECM=2, ELITETXT=5, etc.)
 */

/* Map IMG_* constants (from gfx.h) to alg_data.h sprite indices */
static int img_to_sprite(int sprite_no)
{
	switch (sprite_no)
	{
		case IMG_GREEN_DOT:      return 7;   /* GRNDOT */
		case IMG_RED_DOT:        return 11;  /* REDDOT */
		case IMG_BIG_S:          return 12;  /* SAFE */
		case IMG_ELITE_TXT:      return 5;   /* ELITETXT */
		case IMG_BIG_E:          return 2;   /* ECM */
		case IMG_DICE:           return 6;   /* FRONTV */
		case IMG_BLAKE:          return 0;   /* BLAKE */
		case IMG_MISSILE_GREEN:  return 8;   /* MISSILE_G */
		case IMG_MISSILE_YELLOW: return 10;  /* MISSILE_Y */
		case IMG_MISSILE_RED:    return 9;   /* MISSILE_R */
		default: return -1;
	}
}


void gfx_draw_sprite(int sprite_no, int x, int y)
{
	int idx;
	int sx, sy;
	int w, h;
	const uint8_t *pixels;

	idx = img_to_sprite(sprite_no);
	if (idx < 0 || !n64_sprite_data[idx])
		return;

	pixels = n64_sprite_data[idx];
	w = n64_sprite_widths[idx];
	h = n64_sprite_heights[idx];

	if (x == -1)
		x = ((256 * GFX_SCALE) - w) / 2;

	x += GFX_X_OFFSET;
	y += GFX_Y_OFFSET;

	for (sy = 0; sy < h; sy++)
	{
		for (sx = 0; sx < w; sx++)
		{
			uint8_t pixel = pixels[sy * w + sx];
			if (pixel != 0)  /* 0 = transparent */
				fb_putpixel(x + sx, y + sy, pixel);
		}
	}
}


int gfx_request_file(char *title, char *path, char *ext)
{
	/* No file dialog on N64 - use fixed save slots */
	/* Return 1 to indicate "ok" with the default path */
	return 1;
}
