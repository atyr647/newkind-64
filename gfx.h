/*
 * Elite - The New Kind.
 *
 * Reverse engineered from the BBC disk version of Elite.
 * Additional material by C.J.Pinder.
 *
 * The original Elite code is (C) I.Bell & D.Braben 1984.
 * This version re-engineered in C by C.J.Pinder 1999-2001.
 *
 * email: <christian@newkind.co.uk>
 *
 *
 */


/**
 *
 * Elite - The New Kind.
 *
 * The code in this file has not been derived from the original Elite code.
 * Written by C.J.Pinder 1999/2000.
 *
 **/


#ifndef GFX_H
#define GFX_H

#ifdef RES_640_480

/*
 * N64 layout: a 512-pixel wide play field centred on a 640x480 screen.
 *
 * There are two layouts, switched at run time by gfx_set_layout():
 *
 *   SPACE - the four space views, launch/hyperspace tunnel, escape pod and
 *           game over. The view is 512x320 with the NES-style dashboard
 *           (icon bar + gauges + scanner) below it at SCANNER_Y.
 *
 *   TEXT  - every other screen. The view is the full 512x384 area that the
 *           original New Kind text screens were laid out for, with only the
 *           icon bar underneath (as on the NES, the dashboard is hidden).
 *
 * GFX_VIEW_BY and GFX_Y_CENTRE therefore follow the current layout.
 */

#define GFX_SCALE		(2)
#define GFX_X_OFFSET	(64)
#define GFX_Y_OFFSET	(0)
#define GFX_X_CENTRE	(256)

extern int gfx_view_by;
extern int gfx_y_centre;

#define GFX_Y_CENTRE	(gfx_y_centre)
#define GFX_VIEW_CY		(gfx_y_centre / GFX_SCALE)	/* view centre in 256x192 units */

#define GFX_VIEW_TX		1
#define GFX_VIEW_TY		1
#define GFX_VIEW_BX		509
#define GFX_VIEW_BY		(gfx_view_by)

#define GFX_SPACE_VIEW_BY	319		/* last line of the space view */
#define GFX_TEXT_VIEW_BY	383		/* last line of the text screens */

#define SCANNER_Y		320		/* top of dashboard in the SPACE layout */
#define ICON_BAR_H		32
#define ICON_BAR_TEXT_Y	432		/* top of icon bar in the TEXT layout */

/* Where one-line messages go: near the bottom of whichever view is showing. */
#define GFX_MESSAGE_Y	(gfx_view_by - 25)

#define GFX_LAYOUT_SPACE	0
#define GFX_LAYOUT_TEXT		1
#define GFX_LAYOUT_TITLE	2		/* as TEXT, but no icon bar */

void gfx_set_layout (int layout);
int gfx_get_layout (void);
void gfx_draw_icon_bar (int type, int y);
void gfx_request_direct_frame (void);
const unsigned short *gfx_palette16 (void);
void gfx_blit_span16 (int x, int y, const unsigned short *pixels, int count);

#define N64_SCREEN_W	640
#define N64_SCREEN_H	480

#endif

#ifdef RES_N64_320

#define GFX_SCALE		(1)
#define GFX_X_OFFSET	(32)
#define GFX_Y_OFFSET	(0)
#define GFX_X_CENTRE	(128)
#define GFX_Y_CENTRE	(96)

#define GFX_VIEW_TX		1
#define GFX_VIEW_TY		1
#define GFX_VIEW_BX		253
#define GFX_VIEW_BY		191

#define SCANNER_Y		193

#define N64_SCREEN_W	320
#define N64_SCREEN_H	240

#endif

#ifdef RES_512_512

#define GFX_SCALE		(2)
#define GFX_X_OFFSET	(0)
#define GFX_Y_OFFSET	(0)
#define GFX_X_CENTRE	(256)
#define GFX_Y_CENTRE	(192)

#define GFX_VIEW_TX		1
#define GFX_VIEW_TY		1
#define GFX_VIEW_BX		509
#define GFX_VIEW_BY		381

#endif

#ifdef RES_800_600

#define GFX_SCALE		(2)
#define GFX_X_OFFSET	(144)
#define GFX_Y_OFFSET	(44)
#define GFX_X_CENTRE	(256)
#define GFX_Y_CENTRE	(192)

#define GFX_VIEW_TX		1
#define GFX_VIEW_TY		1
#define GFX_VIEW_BX		509
#define GFX_VIEW_BY		381

#endif

#ifndef GFX_SCALE

#define GFX_SCALE		(1)
#define GFX_X_OFFSET	(0)
#define GFX_Y_OFFSET	(0)
#define GFX_X_CENTRE	(128)
#define GFX_Y_CENTRE	(96)

#define GFX_VIEW_TX		1
#define GFX_VIEW_TY		1
#define GFX_VIEW_BX		253
#define GFX_VIEW_BY		191

#endif
 


#define GFX_COL_BLACK		0
#define GFX_COL_DARK_RED	28
#define GFX_COL_WHITE		255
#define GFX_COL_GOLD		39
#define GFX_COL_RED			49
#define GFX_COL_CYAN		11

#define GFX_COL_GREY_1		248
#define GFX_COL_GREY_2		235
#define GFX_COL_GREY_3		234
#define GFX_COL_GREY_4		237

#define GFX_COL_BLUE_1		45
#define GFX_COL_BLUE_2		46
#define GFX_COL_BLUE_3		133
#define GFX_COL_BLUE_4		4

#define GFX_COL_RED_3		1
#define GFX_COL_RED_4		71

#define GFX_COL_WHITE_2		242

#define GFX_COL_YELLOW_1	37
#define GFX_COL_YELLOW_2	39
#define GFX_COL_YELLOW_3	89
#define GFX_COL_YELLOW_4	160
#define GFX_COL_YELLOW_5	251

#define GFX_ORANGE_1		76
#define GFX_ORANGE_2		77
#define GFX_ORANGE_3		122

#define GFX_COL_GREEN_1		2
#define GFX_COL_GREEN_2		17
#define GFX_COL_GREEN_3		86

#define GFX_COL_PINK_1		183

/* NES dashboard colours. These slots are private to the dashboard art
 * (see build_dashboard.py); no ship, planet, sprite or text uses them. */
#define GFX_COL_NES_GREEN	224
#define GFX_COL_NES_RED		225
#define GFX_COL_NES_GREY	226
#define GFX_COL_NES_DGREY	227
#define GFX_COL_NES_BLUE	228
#define GFX_COL_NES_LABEL	229
#define GFX_COL_NES_CYAN	230
#define GFX_COL_NES_POINTER	231
#define GFX_COL_NES_AMBER	232

#define IMG_GREEN_DOT		1
#define IMG_RED_DOT			2
#define IMG_BIG_S			3
#define IMG_ELITE_TXT		4
#define IMG_BIG_E			5
#define IMG_DICE			6
#define IMG_MISSILE_GREEN	7
#define IMG_MISSILE_YELLOW	8
#define IMG_MISSILE_RED		9
#define IMG_BLAKE			10


int gfx_graphics_startup (void);
void gfx_graphics_shutdown (void);
#ifndef GFX_VIEW_CY
#define GFX_VIEW_CY		96		/* view centre in 256x192 units */
#define GFX_MESSAGE_Y	358
#endif

void gfx_update_screen (void);
void gfx_acquire_screen (void);
void gfx_release_screen (void);
void gfx_plot_pixel (int x, int y, int col);
void gfx_fast_plot_pixel (int x, int y, int col);
void gfx_draw_filled_circle (int cx, int cy, int radius, int circle_colour);
void gfx_draw_circle (int cx, int cy, int radius, int circle_colour);
void gfx_draw_line (int x1, int y1, int x2, int y2);
void gfx_draw_colour_line (int x1, int y1, int x2, int y2, int line_colour);
void gfx_draw_triangle (int x1, int y1, int x2, int y2, int x3, int y3, int col);
void gfx_draw_rectangle (int tx, int ty, int bx, int by, int col);
void gfx_display_text (int x, int y, char *txt);
void gfx_display_colour_text (int x, int y, char *txt, int col);
void gfx_display_centre_text (int y, char *str, int psize, int col);
void gfx_clear_display (void);
void gfx_clear_text_area (void);
void gfx_clear_area (int tx, int ty, int bx, int by);
void gfx_display_pretty_text (int tx, int ty, int bx, int by, char *txt);
void gfx_draw_scanner (void);
void gfx_set_clip_region (int tx, int ty, int bx, int by);
void gfx_polygon (int num_points, int *poly_list, int face_colour);
void gfx_draw_sprite (int sprite_no, int x, int y);
void gfx_draw_borders (void);
void gfx_start_render (void);
void gfx_render_polygon (int num_points, int *point_list, int face_colour, int zavg);
void gfx_render_line (int x1, int y1, int x2, int y2, int dist, int col);
void gfx_finish_render (void);
int gfx_request_file (char *title, char *path, char *ext);

#endif
