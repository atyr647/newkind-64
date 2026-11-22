/*
 * Elite - The New Kind.
 *
 * Reverse engineered from the BBC disk version of Elite.
 * Additional material by C.J.Pinder.
 *
 * The original Elite code is (C) I.Bell & D.Braben 1984.
 * This version re-engineered in C by C.J.Pinder 1999-2001.
 *
 * N64 port: Main game handler.
 * Faithful port of alg_main.c with Allegro replaced by libdragon.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <stdlib.h>

#include <libdragon.h>

#include "config.h"
#include "gfx.h"
#include "main.h"
#include "vector.h"
#include "elite.h"
#include "docked.h"
#include "intro.h"
#include "shipdata.h"
#include "shipface.h"
#include "space.h"
#include "sound.h"
#include "threed.h"
#include "swat.h"
#include "random.h"
#include "options.h"
#include "stars.h"
#include "missions.h"
#include "pilot.h"
#include "file.h"
#include "keyboard.h"


int old_cross_x, old_cross_y;
int cross_timer;

/* frame_count is incremented by the timer in n64_gfx.c at speed_cap rate */
extern volatile int frame_count;

/* Screen number used while the pause menu is up (not a real game screen). */
#define SCR_PAUSE_MENU	100

void save_commander_screen(void);
static void redraw_current_screen(void);
void f_pressed(void);

int draw_lasers;
int mcount;
int message_count;
char message_string[80];
int rolling;
int climbing;
int game_paused;
int have_joystick;
#ifdef HACKING
int identify;
#endif
int scanner_zoom = 1;
int remap_keys;

int find_input;
char find_name[20];

/*
 * NES-style icon bar state.
 * The icon bar cursor tracks which icon slot (0-11) is selected.
 *
 * Icon bar type determines which functions are available:
 *   0 = Docked, 1 = Flight, 2 = Charts
 */
int icon_bar_cursor = 0;
int icon_bar_type = 0;      /* 0=docked, 1=flight, 2=charts */

/* NES icon bar function numbers for each type (the NES iconBarButtons table).
 * 0 = no function. Button 7 of the docked bar (change commander name) has no
 * N64 equivalent. */
static const int icon_funcs[3][12] = {
	{  1, 2,  3,  4,  5,  6,  0, 35,  8,  0,  0, 12 },	/* docked */
	{ 17, 2,  3,  4, 21, 22, 23, 24, 25, 26, 27, 12 },	/* flight */
	{  1, 2, 36, 35, 21, 38, 39, 22, 41, 23, 27, 12 },	/* charts */
};

static int is_space_view(void)
{
	return (current_screen == SCR_FRONT_VIEW || current_screen == SCR_REAR_VIEW ||
			current_screen == SCR_LEFT_VIEW || current_screen == SCR_RIGHT_VIEW);
}

static int is_chart(void)
{
	return (current_screen == SCR_GALACTIC_CHART || current_screen == SCR_SHORT_RANGE);
}

static void update_icon_bar_type(void)
{
	if (is_chart())
		icon_bar_type = 2;
	else if (docked)
		icon_bar_type = 0;
	else
		icon_bar_type = 1;
}

static void request_docking_computer(void)
{
	if (docked)
		info_message("Already docked");
	else if (!cmdr.docking_computer)
		info_message("Docking computer not fitted");
	else if ((universe[1].type == SHIP_CORIOLIS || universe[1].type == SHIP_DODEC) &&
			 (universe[1].flags & FLG_ANGRY))
		info_message("Docking permission refused");
	else if (instant_dock)
		engage_docking_computer();
	else
		engage_auto_pilot();
}

static void start_find_planet(void)
{
	if (!is_chart())
	{
		old_cross_x = -1;
		display_galactic_chart();
	}
	f_pressed();
}

/*
 * N64 Pause Menu - shown when Start is pressed during gameplay.
 * A plain list of every screen and function, for when you can't remember
 * the icon or the shortcut.
 */
#define PAUSE_ITEMS 14
static char *pause_menu_labels[PAUSE_ITEMS] = {
	"Resume",
	"Galactic Chart",
	"Short Range Chart",
	"Data on System",
	"Market Prices",
	"Commander Status",
	"Inventory",
	"Equip Ship",
	"Options",
	"Hyperspace",
	"Docking Computer",
	"Find Planet",
	"Save Commander",
	"Quit Game",
};

#define PAUSE_TOP_Y		52
#define PAUSE_STEP_Y	20

static void draw_pause_item(int i, int selected)
{
	int y = PAUSE_TOP_Y + i * PAUSE_STEP_Y;

	gfx_clear_area(96, y - 1, 416, y + 15);
	if (selected)
		gfx_draw_rectangle(96, y - 1, 416, y + 15, GFX_COL_DARK_RED);
	gfx_display_colour_text(160, y, pause_menu_labels[i],
							selected ? GFX_COL_WHITE : GFX_COL_GREY_1);
}

static void run_pause_menu(void)
{
	int resume_screen = current_screen;
	int selection = 0;
	int choice = -1;
	int i;

	game_paused = 0;
	find_input = 0;

	current_screen = SCR_PAUSE_MENU;
	gfx_clear_display();
	gfx_clear_area(0, ICON_BAR_TEXT_Y, 511, N64_SCREEN_H - 1);

	gfx_display_centre_text(10, "PAUSED", 140, GFX_COL_GOLD);
	gfx_draw_line(0, 36, 511, 36);
	for (i = 0; i < PAUSE_ITEMS; i++)
		draw_pause_item(i, i == selection);
	gfx_display_centre_text(350, "D-Pad: Select    A: Confirm    Start: Resume", 120, GFX_COL_GREY_1);

	while (choice < 0)
	{
		joypad_buttons_t pressed;
		int old = selection;

		gfx_update_screen();

		joypad_poll();
		pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);

		if (pressed.d_up)
			selection = (selection + PAUSE_ITEMS - 1) % PAUSE_ITEMS;
		if (pressed.d_down)
			selection = (selection + 1) % PAUSE_ITEMS;

		if (selection != old)
		{
			draw_pause_item(old, 0);
			draw_pause_item(selection, 1);
		}

		if (pressed.start || pressed.b)
			choice = 0;
		else if (pressed.a)
			choice = selection;
	}

	/* Put back the screen we came from; the choice may then replace it. */
	current_screen = resume_screen;
	redraw_current_screen();
	frame_count = 0;

	switch (choice)
	{
		case 1:
			old_cross_x = -1;
			display_galactic_chart();
			break;
		case 2:
			old_cross_x = -1;
			display_short_range_chart();
			break;
		case 3:
			display_data_on_planet();
			break;
		case 4:
			if (!witchspace)
				display_market_prices();
			break;
		case 5:
			display_commander_status();
			break;
		case 6:
			display_inventory();
			break;
		case 7:
			if (docked)
				equip_ship();
			else
				info_message("Must be docked to equip");
			break;
		case 8:
			display_options();
			break;
		case 9:
			if (docked)
				info_message("Launch first");
			else
				start_hyperspace();
			break;
		case 10:
			request_docking_computer();
			break;
		case 11:
			start_find_planet();
			break;
		case 12:
			if (docked)
				save_commander_screen();
			else
				info_message("Must be docked to save");
			break;
		case 13:
			quit_screen();
			break;
	}
}


/*
 * Draw the current screen again from scratch (after the pause menu has
 * been drawn over it). The space views redraw themselves every frame.
 */
static void redraw_current_screen(void)
{
	switch (current_screen)
	{
		case SCR_GALACTIC_CHART:
		case SCR_SHORT_RANGE:
		{
			int cx = cross_x, cy = cross_y;
			if (current_screen == SCR_GALACTIC_CHART)
				display_galactic_chart();
			else
				display_short_range_chart();
			cross_x = cx;
			cross_y = cy;
			old_cross_x = -1;
			break;
		}
		case SCR_PLANET_DATA:	display_data_on_planet(); break;
		case SCR_MARKET_PRICES:	display_market_prices(); break;
		case SCR_CMDR_STATUS:	display_commander_status(); break;
		case SCR_INVENTORY:		display_inventory(); break;
		case SCR_EQUIP_SHIP:	equip_ship(); break;
		case SCR_OPTIONS:		display_options(); break;
		case SCR_SETTINGS:		game_settings_screen(); break;
		case SCR_QUIT:			quit_screen(); break;
		case SCR_RESTART:		restart_screen(); break;
		case SCR_SAVE_CMDR:
		case SCR_LOAD_CMDR:		display_options(); break;
		default:				gfx_clear_display(); break;
	}
}


/*
 * Initialise the game parameters.
 */

void initialise_game(void)
{
	set_rand_seed((int)timer_ticks() & 0x7FFFFFFF);
	current_screen = SCR_INTRO_ONE;

	restore_saved_commander();

	flight_speed = 1;
	flight_roll = 0;
	flight_climb = 0;
	docked = 1;
	front_shield = 255;
	aft_shield = 255;
	energy = 255;
	draw_lasers = 0;
	mcount = 0;
	hyper_ready = 0;
	detonate_bomb = 0;
	find_input = 0;
	witchspace = 0;
	game_paused = 0;
	auto_pilot = 0;

	create_new_stars();
	clear_universe();

	cross_x = -1;
	cross_y = -1;
	cross_timer = 0;

	myship.max_speed = 40;
	myship.max_roll = 31;
	myship.max_climb = 8;
	myship.max_fuel = 70;
}


void finish_game(void)
{
	finish = 1;
	game_over = 2;
}


/*
 * Move the planet chart cross hairs to specified position.
 */

void move_cross(int dx, int dy)
{
	cross_timer = 5;

	if (kbd_ctrl_pressed) {
		dx *= 4;
		dy *= 4;
	}

	if (current_screen == SCR_SHORT_RANGE)
	{
		cross_x += (dx * 4);
		cross_y += (dy * 4);
		return;
	}

	if (current_screen == SCR_GALACTIC_CHART)
	{
		cross_x += (dx * 2);
		cross_y += (dy * 2);

		if (cross_x < 1)
			cross_x = 1;

		if (cross_x > 510)
			cross_x = 510;

		if (cross_y < 37)
			cross_y = 37;

		if (cross_y > 293)
			cross_y = 293;
	}
}


/*
 * Draw the cross hairs at the specified position.
 */

void draw_cross(int cx, int cy)
{
	if (current_screen == SCR_SHORT_RANGE)
	{
		gfx_set_clip_region(1, 37, 510, 339);
		gfx_draw_colour_line(cx - 16, cy, cx + 16, cy, GFX_COL_RED);
		gfx_draw_colour_line(cx, cy - 16, cx, cy + 16, GFX_COL_RED);
		gfx_set_clip_region(1, 1, 510, GFX_VIEW_BY);
		return;
	}

	if (current_screen == SCR_GALACTIC_CHART)
	{
		gfx_set_clip_region(1, 37, 510, 293);
		gfx_draw_colour_line(cx - 8, cy, cx + 8, cy, GFX_COL_RED);
		gfx_draw_colour_line(cx, cy - 8, cx, cy + 8, GFX_COL_RED);
		gfx_set_clip_region(1, 1, 510, GFX_VIEW_BY);
	}
}


void draw_laser_sights(void)
{
	int laser = 0;
	int x1,y1,x2,y2;

	switch (current_screen)
	{
		case SCR_FRONT_VIEW:
			gfx_display_centre_text(32, "Front View", 120, GFX_COL_WHITE);
			laser = cmdr.front_laser;
			break;

		case SCR_REAR_VIEW:
			gfx_display_centre_text(32, "Rear View", 120, GFX_COL_WHITE);
			laser = cmdr.rear_laser;
			break;

		case SCR_LEFT_VIEW:
			gfx_display_centre_text(32, "Left View", 120, GFX_COL_WHITE);
			laser = cmdr.left_laser;
			break;

		case SCR_RIGHT_VIEW:
			gfx_display_centre_text(32, "Right View", 120, GFX_COL_WHITE);
			laser = cmdr.right_laser;
			break;
	}

	if (laser)
	{
		x1 = 128 * GFX_SCALE;
		y1 = (GFX_VIEW_CY-8) * GFX_SCALE;
		y2 = (GFX_VIEW_CY-16) * GFX_SCALE;

		gfx_draw_colour_line(x1-1, y1, x1-1, y2, GFX_COL_GREY_1);
		gfx_draw_colour_line(x1, y1, x1, y2, GFX_COL_WHITE);
		gfx_draw_colour_line(x1+1, y1, x1+1, y2, GFX_COL_GREY_1);

		y1 = (GFX_VIEW_CY+8) * GFX_SCALE;
		y2 = (GFX_VIEW_CY+16) * GFX_SCALE;

		gfx_draw_colour_line(x1-1, y1, x1-1, y2, GFX_COL_GREY_1);
		gfx_draw_colour_line(x1, y1, x1, y2, GFX_COL_WHITE);
		gfx_draw_colour_line(x1+1, y1, x1+1, y2, GFX_COL_GREY_1);

		x1 = (128-8) * GFX_SCALE;
		y1 = GFX_VIEW_CY * GFX_SCALE;
		x2 = (128-16) * GFX_SCALE;

		gfx_draw_colour_line(x1, y1-1, x2, y1-1, GFX_COL_GREY_1);
		gfx_draw_colour_line(x1, y1, x2, y1, GFX_COL_WHITE);
		gfx_draw_colour_line(x1, y1+1, x2, y1+1, GFX_COL_GREY_1);

		x1 = (128+8) * GFX_SCALE;
		x2 = (128+16) * GFX_SCALE;

		gfx_draw_colour_line(x1, y1-1, x2, y1-1, GFX_COL_GREY_1);
		gfx_draw_colour_line(x1, y1, x2, y1, GFX_COL_WHITE);
		gfx_draw_colour_line(x1, y1+1, x2, y1+1, GFX_COL_GREY_1);
	}
}

/* Forward declarations for flight maneuver functions */
static void roll_left(void);
static void roll_right(void);
static void climb(void);
static void dive(void);

/*
 * NES-style icon bar: move the pointer and carry out the selected button.
 * Called every frame from handle_flight_keys, on every screen.
 */
static void process_icon_bar(void)
{
	int func;

	if (kbd_icon_left_pressed)
		icon_bar_cursor = (icon_bar_cursor + 11) % 12;
	if (kbd_icon_right_pressed)
		icon_bar_cursor = (icon_bar_cursor + 1) % 12;

	if (!kbd_icon_activate_pressed)
		return;

	func = icon_funcs[icon_bar_type][icon_bar_cursor];

	/* The NES button numbers (see the TT102 routine in the NES source). */
	switch (func)
	{
		case 1:  /* Launch */
			if (docked)
				kbd_F1_pressed = 1;
			break;
		case 2:  /* Market Prices */
			kbd_F8_pressed = 1;
			break;
		case 3:  /* Status Mode */
			kbd_F9_pressed = 1;
			break;
		case 4:  /* Charts */
			kbd_F6_pressed = 1;
			break;
		case 5:  /* Equip Ship */
			if (docked)
				kbd_F4_pressed = 1;
			break;
		case 6:  /* Save and Load */
			if (docked)
				kbd_F11_pressed = 1;
			break;
		case 8:  /* Inventory */
			kbd_F10_pressed = 1;
			break;
		case 12: /* Fast-forward: the in-system jump */
			kbd_jump_pressed = 1;
			break;
		case 17: /* Docking Computer */
			request_docking_computer();
			break;
		case 21: /* Front Space View, then rear, left and right */
			if (docked)
				break;
			if (current_screen == SCR_FRONT_VIEW)
				kbd_F2_pressed = 1;
			else if (current_screen == SCR_REAR_VIEW)
				kbd_F3_pressed = 1;
			else if (current_screen == SCR_LEFT_VIEW)
				kbd_F4_pressed = 1;
			else
				kbd_F1_pressed = 1;
			break;
		case 22: /* Hyperspace */
			kbd_hyperspace_pressed = 1;
			break;
		case 23: /* E.C.M. */
			kbd_ecm_pressed = 1;
			break;
		case 24: /* Target Missile (or stand it down again) */
			if (missile_target == MISSILE_UNARMED)
				kbd_target_missile_pressed = 1;
			else
				kbd_unarm_missile_pressed = 1;
			break;
		case 25: /* Fire Targeted Missile */
			kbd_fire_missile_pressed = 1;
			break;
		case 26: /* Energy Bomb */
			kbd_energy_bomb_pressed = 1;
			kbd_ctrl_pressed = 1;
			break;
		case 27: /* Escape Pod */
			kbd_escape_pressed = 1;
			kbd_ctrl_pressed = 1;
			break;
		case 35: /* Data on System */
			kbd_F7_pressed = 1;
			break;
		case 36: /* Switch Chart Range */
			if (current_screen == SCR_GALACTIC_CHART)
				kbd_F6_pressed = 1;
			else
				kbd_F5_pressed = 1;
			break;
		case 38: /* Return Pointer to Current System */
			kbd_origin_pressed = 1;
			break;
		case 39: /* Search for System */
			kbd_find_pressed = 1;
			break;
		case 41: /* Galactic Hyperspace */
			kbd_hyperspace_pressed = 1;
			kbd_ctrl_pressed = 1;
			break;
	}
}


/*
 * Apply the analog stick and the C-buttons to roll and pitch.
 *
 * Sign conventions (as in the original): flight_roll > 0 rolls left,
 * flight_climb > 0 climbs. The stick works like a flight stick: push
 * forward to dive, pull back to climb.
 */
static void process_analog_flight(void)
{
	if (docked || !is_space_view())
		return;

	if (joy_roll != 0)
	{
		flight_roll = -(joy_roll * myship.max_roll) / 80;
		if (flight_roll > myship.max_roll) flight_roll = myship.max_roll;
		if (flight_roll < -myship.max_roll) flight_roll = -myship.max_roll;
		rolling = 1;
	}

	if (joy_pitch != 0)
	{
		flight_climb = (joy_pitch * myship.max_climb) / 80;
		if (flight_climb > myship.max_climb) flight_climb = myship.max_climb;
		if (flight_climb < -myship.max_climb) flight_climb = -myship.max_climb;
		climbing = 1;
	}

	if (kbd_climb_pressed)		/* C-Up: nose down */
		dive();
	if (kbd_dive_pressed)		/* C-Down: nose up */
		climb();
	if (kbd_roll_left_pressed)
		roll_left();
	if (kbd_roll_right_pressed)
		roll_right();
}


static void roll_left(void)
{
	if (flight_roll < 0)
		flight_roll = 0;
	else
	{
		increase_flight_roll();
		increase_flight_roll();
		rolling = 1;
	}
}

static void roll_right(void)
{
	if (flight_roll > 0)
		flight_roll = 0;
	else
	{
		decrease_flight_roll();
		decrease_flight_roll();
		rolling = 1;
	}
}

static void climb(void)
{
	if (flight_climb < 0)
		flight_climb = 0;
	else
	{
		increase_flight_climb();
	}
	climbing = 1;
}

static void dive(void)
{
	if (flight_climb > 0)
		flight_climb = 0;
	else
	{
		decrease_flight_climb();
	}
	climbing = 1;
}

void arrow_right(void)
{
	switch (current_screen)
	{
		case SCR_MARKET_PRICES:
			buy_stock();
			break;

		case SCR_SETTINGS:
			select_right_setting();
			break;

		case SCR_SHORT_RANGE:
		case SCR_GALACTIC_CHART:
			move_cross(1, 0);
			break;

		case SCR_FRONT_VIEW:
			roll_right();
			break;
		case SCR_REAR_VIEW:
			if (remap_keys) roll_left(); else roll_right();
			break;
		case SCR_RIGHT_VIEW:
			if (remap_keys) climb(); else roll_right();
			break;
		case SCR_LEFT_VIEW:
			if (remap_keys) dive(); else roll_right();
			break;
	}
}


void arrow_left(void)
{
	switch (current_screen)
	{
		case SCR_MARKET_PRICES:
			sell_stock();
			break;

		case SCR_SETTINGS:
			select_left_setting();
			break;

		case SCR_SHORT_RANGE:
		case SCR_GALACTIC_CHART:
			move_cross(-1, 0);
			break;

		case SCR_FRONT_VIEW:
			roll_left();
			break;
		case SCR_REAR_VIEW:
			if (remap_keys) roll_right(); else roll_left();
			break;
		case SCR_RIGHT_VIEW:
			if (remap_keys) dive(); else roll_left();
			break;
		case SCR_LEFT_VIEW:
			if (remap_keys) climb(); else roll_left();
			break;
	}
}


void arrow_up(void)
{
	switch (current_screen)
	{
		case SCR_MARKET_PRICES:
			select_previous_stock();
			break;

		case SCR_EQUIP_SHIP:
			select_previous_equip();
			break;

		case SCR_OPTIONS:
			select_previous_option();
			break;

		case SCR_SETTINGS:
			select_up_setting();
			break;

		case SCR_SHORT_RANGE:
		case SCR_GALACTIC_CHART:
			move_cross(0, -1);
			break;

		case SCR_FRONT_VIEW:
			dive();
			break;
		case SCR_REAR_VIEW:
			if (remap_keys) climb(); else dive();
			break;
		case SCR_RIGHT_VIEW:
			if (remap_keys) roll_right(); else dive();
			break;
		case SCR_LEFT_VIEW:
			if (remap_keys) roll_left(); else dive();
			break;
	}
}


void arrow_down(void)
{
	switch (current_screen)
	{
		case SCR_MARKET_PRICES:
			select_next_stock();
			break;

		case SCR_EQUIP_SHIP:
			select_next_equip();
			break;

		case SCR_OPTIONS:
			select_next_option();
			break;

		case SCR_SETTINGS:
			select_down_setting();
			break;

		case SCR_SHORT_RANGE:
		case SCR_GALACTIC_CHART:
			move_cross(0, 1);
			break;

		case SCR_FRONT_VIEW:
			climb();
			break;
		case SCR_REAR_VIEW:
			if (remap_keys) dive(); else climb();
			break;
		case SCR_RIGHT_VIEW:
			if (remap_keys) roll_left(); else climb();
			break;
		case SCR_LEFT_VIEW:
			if (remap_keys) roll_right(); else climb();
			break;
	}
}


void return_pressed(void)
{
	switch (current_screen)
	{
		case SCR_EQUIP_SHIP:
			buy_equip();
			break;

		case SCR_OPTIONS:
			do_option();
			break;

		case SCR_SETTINGS:
			toggle_setting();
			break;
	}
}


void y_pressed(void)
{
	switch (current_screen)
	{
		case SCR_QUIT:
			finish_game();
			break;
		case SCR_RESTART:
			game_over = 2;
			break;
	}
}


void n_pressed(void)
{
	switch (current_screen)
	{
		case SCR_QUIT:
		case SCR_RESTART:
			if (docked)
				display_commander_status();
			else
				current_screen = SCR_FRONT_VIEW;
			break;
	}
}


void d_pressed(void)
{
	switch (current_screen)
	{
		case SCR_GALACTIC_CHART:
		case SCR_SHORT_RANGE:
			show_distance_to_planet();
			break;

		case SCR_FRONT_VIEW:
		case SCR_REAR_VIEW:
		case SCR_RIGHT_VIEW:
		case SCR_LEFT_VIEW:
			if (auto_pilot)
				disengage_auto_pilot();
			break;
	}
}


/* Planet finder: the name is built one letter at a time. */
static int find_letter;

static void show_find_prompt(void)
{
	char str[48];

	sprintf(str, "Planet Name? %s%c", find_name, 'A' + find_letter);
	gfx_clear_text_area();
	gfx_display_text(16, 340, str);
	gfx_display_colour_text(16, 356, "Up/Down:letter  Right:next  A:search  B:delete", GFX_COL_GREY_1);
}

void f_pressed(void)
{
	if (is_chart())
	{
		find_input = 1;
		find_letter = 0;
		*find_name = '\0';
		show_find_prompt();
	}
}


void add_find_char(int letter)
{
	int len = strlen(find_name);

	if (len >= 16)
		return;

	find_name[len] = toupper(letter);
	find_name[len + 1] = '\0';
	show_find_prompt();
}


void delete_find_char(void)
{
	int len = strlen(find_name);

	if (len == 0)
		return;

	find_name[len - 1] = '\0';
	show_find_prompt();
}

void o_pressed()
{
	switch (current_screen)
	{
		case SCR_GALACTIC_CHART:
		case SCR_SHORT_RANGE:
			move_cursor_to_origin();
			break;
	}
}


void auto_dock(void)
{
	struct univ_object ship;

	ship.location.x = 0;
	ship.location.y = 0;
	ship.location.z = 0;

	set_init_matrix(ship.rotmat);
	ship.rotmat[2].z = 1;
	ship.rotmat[0].x = -1;
	ship.type = -96;
	ship.velocity = flight_speed;
	ship.acceleration = 0;
	ship.bravery = 0;
	ship.rotz = 0;
	ship.rotx = 0;

	auto_pilot_ship(&ship);

	if (ship.velocity > 22)
		flight_speed = 22;
	else
		flight_speed = ship.velocity;

	if (ship.acceleration > 0)
	{
		flight_speed++;
		if (flight_speed > 22)
			flight_speed = 22;
	}

	if (ship.acceleration < 0)
	{
		flight_speed--;
		if (flight_speed < 1)
			flight_speed = 1;
	}

	if (ship.rotx == 0)
		flight_climb = 0;

	if (ship.rotx < 0)
	{
		increase_flight_climb();
		if (ship.rotx < -1)
			increase_flight_climb();
	}

	if (ship.rotx > 0)
	{
		decrease_flight_climb();
		if (ship.rotx > 1)
			decrease_flight_climb();
	}

	if (ship.rotz == 127)
		flight_roll = -14;
	else
	{
		if (ship.rotz == 0)
			flight_roll = 0;

		if (ship.rotz > 0)
		{
			increase_flight_roll();
			if (ship.rotz > 1)
				increase_flight_roll();
		}

		if (ship.rotz < 0)
		{
			decrease_flight_roll();
			if (ship.rotz < -1)
				decrease_flight_roll();
		}
	}
}


void run_escape_sequence(void)
{
	int i;
	int newship;
	Matrix rotmat;

	current_screen = SCR_ESCAPE_POD;

	flight_speed = 1;
	flight_roll = 0;
	flight_climb = 0;

	set_init_matrix(rotmat);
	rotmat[2].z = 1.0;

	/* This loop simulates on every frame, whatever the main loop was doing. */
	universe_render_only = 0;
	star_delta_scale = 1.0;

	newship = add_new_ship(SHIP_COBRA3, 0, 0, 200, rotmat, -127, -127);
	universe[newship].velocity = 7;
	snd_play_sample(SND_LAUNCH);

	for (i = 0; i < 90; i++)
	{
		if (i == 40)
		{
			universe[newship].flags |= FLG_DEAD;
			snd_play_sample(SND_EXPLODE);
		}

		gfx_set_clip_region(1, 1, 510, GFX_VIEW_BY);
		gfx_clear_display();
		update_starfield();
		update_universe();

		universe[newship].location.x = 0;
		universe[newship].location.y = 0;
		universe[newship].location.z += 2;

		gfx_display_centre_text(GFX_MESSAGE_Y, "Escape pod launched - Ship auto-destuct initiated.", 120, GFX_COL_WHITE);

		update_console();
		gfx_update_screen();
	}

	while ((ship_count[SHIP_CORIOLIS] == 0) &&
		   (ship_count[SHIP_DODEC] == 0))
	{
		auto_dock();

		if ((abs(flight_roll) < 3) && (abs(flight_climb) < 3))
		{
			for (i = 0; i < MAX_UNIV_OBJECTS; i++)
			{
				if (universe[i].type != 0)
					universe[i].location.z -= 1500;
			}
		}

		warp_stars = 1;
		gfx_set_clip_region(1, 1, 510, GFX_VIEW_BY);
		gfx_clear_display();
		update_starfield();
		update_universe();
		update_console();
		gfx_update_screen();
	}

	abandon_ship();
}

void handle_flight_keys(void)
{
	kbd_poll_keyboard();

	update_icon_bar_type();

	if (kbd_pause_pressed)
	{
		run_pause_menu();
		update_icon_bar_type();
		return;
	}

	/* NES-style icon bar navigation and activation */
	process_icon_bar();

	/* Analog joystick + C-button digital flight controls */
	process_analog_flight();

	if (kbd_F1_pressed)
	{
		find_input = 0;

		if (docked)
			launch_player();
		else
		{
			if (current_screen != SCR_FRONT_VIEW)
			{
				current_screen = SCR_FRONT_VIEW;
				flip_stars();
			}
		}
	}

	if (kbd_F2_pressed)
	{
		find_input = 0;

		if (!docked)
		{
			if (current_screen != SCR_REAR_VIEW)
			{
				current_screen = SCR_REAR_VIEW;
				flip_stars();
			}
		}
	}

	if (kbd_F3_pressed)
	{
		find_input = 0;

		if (!docked)
		{
			if (current_screen != SCR_LEFT_VIEW)
			{
				current_screen = SCR_LEFT_VIEW;
				flip_stars();
			}
		}
	}

	if (kbd_F4_pressed)
	{
		find_input = 0;

		if (docked)
			equip_ship();
		else
		{
			if (current_screen != SCR_RIGHT_VIEW)
			{
				current_screen = SCR_RIGHT_VIEW;
				flip_stars();
			}
		}
	}

	if (kbd_F5_pressed)
	{
		find_input = 0;
		old_cross_x = -1;
		display_galactic_chart();
	}

	if (kbd_F6_pressed)
	{
		find_input = 0;
		old_cross_x = -1;
		display_short_range_chart();
	}

	if (kbd_F7_pressed)
	{
		find_input = 0;
		display_data_on_planet();
	}

	if (kbd_F8_pressed && (!witchspace))
	{
		find_input = 0;
		display_market_prices();
	}

	if (kbd_F9_pressed)
	{
		find_input = 0;
		display_commander_status();
	}

	if (kbd_F10_pressed)
	{
		find_input = 0;
		display_inventory();
	}

	if (kbd_F11_pressed)
	{
		find_input = 0;
		display_options();
	}

	if (find_input)
	{
		/* Up/Down picks a letter, Right keeps it, A searches, B deletes
		 * (or cancels when the name is empty). */
		if (!is_chart())
		{
			find_input = 0;
		}
		else
		{
			if (kbd_up_pressed)
			{
				find_letter = (find_letter + 1) % 26;
				show_find_prompt();
			}
			else if (kbd_down_pressed)
			{
				find_letter = (find_letter + 25) % 26;
				show_find_prompt();
			}
			else if (kbd_right_pressed)
			{
				add_find_char('A' + find_letter);
			}
			else if (kbd_enter_pressed)
			{
				/* The letter on show counts as part of the name. */
				add_find_char('A' + find_letter);
				find_input = 0;
				find_planet_by_name(find_name);
			}
			else if (kbd_backspace_pressed)
			{
				if (strlen(find_name) > 0)
					delete_find_char();
				else
				{
					find_input = 0;
					gfx_clear_text_area();
					show_distance_to_planet();
				}
			}
			return;
		}
	}

	if (kbd_y_pressed)
		y_pressed();

	if (kbd_n_pressed)
	{
		if (!docked && !is_space_view() &&
			current_screen != SCR_QUIT && current_screen != SCR_RESTART)
		{
			current_screen = SCR_FRONT_VIEW;
			flip_stars();
		}
		else
			n_pressed();
	}

#ifdef HACKING
	if (kbd_i_pressed == 1)
		identify = !identify;
#endif
	if (kbd_zoom_pressed == 1)
		scanner_zoom ^= 3;

	if (kbd_fire_pressed)
	{
		if ((!docked) && (draw_lasers == 0))
			draw_lasers = fire_laser();
	}

	if (kbd_dock_pressed)
		request_docking_computer();

	if (kbd_d_pressed)
		d_pressed();

	if (kbd_ecm_pressed)
	{
		if (!docked && cmdr.ecm)
			activate_ecm(1);
	}

	if (kbd_find_pressed)
		f_pressed();

	if (kbd_hyperspace_pressed && (!docked))
	{
		if (kbd_ctrl_pressed)
			start_galactic_hyperspace();
		else
			start_hyperspace();
	}

	if (kbd_jump_pressed && (!docked) && (!witchspace))
	{
		jump_warp();
	}

	if (kbd_fire_missile_pressed)
	{
		if (!docked)
			fire_missile();
	}

	if (kbd_origin_pressed)
		o_pressed();

	if (kbd_target_missile_pressed)
	{
		if (!docked)
			arm_missile();
	}

	if (kbd_unarm_missile_pressed)
	{
		if (!docked)
			unarm_missile();
	}

	if (kbd_inc_speed_pressed)
	{
		if (!docked)
		{
			if (flight_speed < myship.max_speed)
				flight_speed++;
		}
	}

	if (kbd_dec_speed_pressed)
	{
		if (!docked)
		{
			if (flight_speed > 1)
				flight_speed--;
		}
	}

	if (kbd_up_pressed)
		arrow_up();

	if (kbd_down_pressed)
		arrow_down();

	if (kbd_left_pressed)
		arrow_left();

	if (kbd_right_pressed)
		arrow_right();

	if (kbd_enter_pressed)
		return_pressed();

	if (kbd_energy_bomb_pressed && kbd_ctrl_pressed)
	{
		if ((!docked) && (cmdr.energy_bomb))
		{
			detonate_bomb = 1;
			cmdr.energy_bomb = 0;
		}
	}

	if (kbd_escape_pressed && kbd_ctrl_pressed)
	{
		if ((!docked) && (cmdr.escape_pod) && (!witchspace))
			run_escape_sequence();
	}
}


void set_commander_name(char *path)
{
	char *fname, *cname;
	int i;

	/* Extract filename from path */
	fname = path;
	for (i = 0; path[i]; i++)
	{
		if (path[i] == '/' || path[i] == '\\')
			fname = &path[i + 1];
	}

	cname = cmdr.name;

	for (i = 0; i < 31; i++)
	{
		if (!isalnum((unsigned char)*fname))
			break;

		*cname++ = toupper((unsigned char)*fname++);
	}

	*cname = '\0';
}


void save_commander_screen(void)
{
	int rv;

	current_screen = SCR_SAVE_CMDR;

	gfx_clear_display();
	gfx_display_centre_text(10, "SAVE COMMANDER", 140, GFX_COL_GOLD);
	gfx_draw_line(0, 36, 511, 36);
	gfx_update_screen();

	/* On N64 there is a single save slot in the cartridge EEPROM. */
	rv = save_commander_file(cmdr.name);

	if (rv)
	{
		gfx_display_centre_text(175, "Error Saving Commander!", 140, GFX_COL_GOLD);
		gfx_update_screen();
		return;
	}

	gfx_display_centre_text(175, "Commander Saved.", 140, GFX_COL_GOLD);
	gfx_update_screen();

	saved_cmdr = cmdr;
	saved_cmdr.ship_x = docked_planet.d;
	saved_cmdr.ship_y = docked_planet.b;
}


void load_commander_screen(void)
{
	int rv;

	gfx_clear_display();
	gfx_display_centre_text(10, "LOAD COMMANDER", 140, GFX_COL_GOLD);
	gfx_draw_line(0, 36, 511, 36);
	gfx_update_screen();

	/* On N64 there is a single save slot in the cartridge EEPROM. */
	rv = load_commander_file("jameson");

	if (rv)
	{
		saved_cmdr = cmdr;
		gfx_display_centre_text(175, "Error Loading Commander!", 140, GFX_COL_GOLD);
		gfx_display_centre_text(200, "Press A to continue.", 140, GFX_COL_GOLD);
		gfx_update_screen();

		/* Wait for button press */
		for (;;)
		{
			joypad_buttons_t btns;
			gfx_update_screen();
			joypad_poll();
			btns = joypad_get_buttons_pressed(JOYPAD_PORT_1);
			if (btns.a || btns.b || btns.start)
				break;
		}
		return;
	}

	restore_saved_commander();
	set_commander_name("JAMESON");
	saved_cmdr = cmdr;
}


extern int n64_save_exists(void);

void run_first_intro_screen(void)
{
	current_screen = SCR_INTRO_ONE;
	universe_render_only = 0;
	star_delta_scale = 1.0;

	snd_play_midi(SND_ELITE_THEME, 1);

	initialise_intro1();
#ifdef HACKING
	identify = 0;
#endif

	for (;;)
	{
		update_intro1();

		gfx_update_screen();

		kbd_poll_keyboard();

		/* A loads the saved commander (if there is one); B or Start
		 * begins with a new Commander Jameson. */
		if (kbd_y_pressed && n64_save_exists())
		{
			snd_stop_midi();
			load_commander_screen();
			break;
		}

		if (kbd_n_pressed || kbd_space_pressed ||
			(kbd_y_pressed && !n64_save_exists()))
		{
			snd_stop_midi();
			break;
		}
	}
}


void run_second_intro_screen(void)
{
	current_screen = SCR_INTRO_TWO;
	universe_render_only = 0;
	star_delta_scale = 1.0;

	snd_play_midi(SND_BLUE_DANUBE, 1);

#ifdef HACKING
	identify = 0;
#endif
	initialise_intro2();

	flight_speed = 3;
	flight_roll = 0;
	flight_climb = 0;

	for (;;)
	{
		update_intro2();

		gfx_update_screen();

		kbd_poll_keyboard();

		if (kbd_space_pressed)
			break;
	}

	snd_stop_midi();
}


/*
 * Draw the game over sequence.
 */

void run_game_over_screen()
{
	int i;
	int newship;
	Matrix rotmat;
	int type;

	current_screen = SCR_GAME_OVER;
	universe_render_only = 0;
	star_delta_scale = 1.0;

	flight_speed = 6;
	flight_roll = 0;
	flight_climb = 0;
#ifdef HACKING
	identify = 0;
#endif
	clear_universe();

	set_init_matrix(rotmat);

	newship = add_new_ship(SHIP_COBRA3, 0, 0, -400, rotmat, 0, 0);
	universe[newship].flags |= FLG_DEAD;

	for (i = 0; i < 5; i++)
	{
		type = (rand255() & 1) ? SHIP_CARGO : SHIP_ALLOY;
		newship = add_new_ship(type, (rand255() & 63) - 32,
								(rand255() & 63) - 32, -400, rotmat, 0, 0);
		universe[newship].rotz = ((rand255() * 2) & 255) - 128;
		universe[newship].rotx = ((rand255() * 2) & 255) - 128;
		universe[newship].velocity = rand255() & 15;
	}


	for (i = 0; i < 100; i++)
	{
		gfx_clear_display();
		gfx_set_clip_region(1, 1, 510, GFX_VIEW_BY);
		update_starfield();
		update_universe();
		gfx_display_centre_text(GFX_Y_CENTRE - 8, "GAME OVER", 140, GFX_COL_GOLD);
		update_console();
		gfx_update_screen();
	}
}


/*
 * Draw a break pattern (for launching, docking and hyperspacing).
 */

void display_break_pattern(void)
{
	int i;

	gfx_clear_display();
	gfx_set_clip_region(1, 1, 510, GFX_VIEW_BY);
	update_console();

	for (i = 0; i < 20; i++)
	{
		gfx_set_clip_region(1, 1, 510, GFX_VIEW_BY);
		gfx_draw_circle(256, GFX_Y_CENTRE, 30 + i * 15, GFX_COL_WHITE);
		gfx_update_screen();
	}

	if (docked)
	{
		check_mission_brief();
		update_condition();
		display_commander_status();
	}
	else
		current_screen = SCR_FRONT_VIEW;

	frame_count = 0;
}


void info_message(char *message)
{
	strcpy(message_string, message);
	message_count = 37;
}


void update_screen(void)
{
	gfx_update_screen();
}


/*
 * N64 initialization - replaces initialise_allegro()
 */
/* Declared in n64_file.c */
extern void n64_save_init(void);

static void initialise_n64(void)
{
	/* Initialize libdragon subsystems */
	timer_init();
	dfs_init(DFS_DEFAULT_LOCATION);

	/* Initialize EEPROM for save/load */
	n64_save_init();

	/* Controller init is done in kbd_keyboard_startup() */
	have_joystick = 1; /* N64 always has a controller */
}



int main(void)
{
	initialise_n64();
	read_config_file();

	if (gfx_graphics_startup() == 1)
	{
		return 1;
	}

	/* Start the sound system... */
	snd_sound_startup();

	/* Do any setup necessary for the input... */
	kbd_keyboard_startup();

	finish = 0;
	auto_pilot = 0;

	while (!finish)
	{
		game_over = 0;
		initialise_game();
		dock_player();

		current_screen = SCR_FRONT_VIEW;
		run_first_intro_screen();
		run_second_intro_screen();

		old_cross_x = -1;
		old_cross_y = -1;

		dock_player();
		display_commander_status();

		while (!game_over)
		{
			/*
			 * One pass = one displayed frame. The simulation runs at the
			 * original game speed (one step every speed_cap milliseconds,
			 * counted by the frame timer); frames in between only redraw,
			 * which keeps the stars and the controls smooth.
			 */
			static long long last_frame_ticks = 0;
			long long now_ticks;
			int do_sim;
			int space_view;

			/* The space views are redrawn in full every frame, so they can
			 * be drawn straight into the display buffer. */
			if (!docked && is_space_view())
				gfx_request_direct_frame();
			gfx_update_screen();

			do_sim = (frame_count >= 1);
			if (do_sim)
				frame_count = 0;

			/* ===== INPUT PHASE (every frame for responsive controls) ===== */
			gfx_set_clip_region(1, 1, 510, GFX_VIEW_BY);
			rolling = 0;
			climbing = 0;
			handle_flight_keys();
			update_icon_bar_type();

			if (game_over)
				break;

			/* ===== SIMULATION PHASE (only on sim ticks) ===== */
			if (do_sim)
			{
				snd_tick_cooldowns();

				if (message_count > 0)
					message_count--;

				if (!docked && auto_pilot)
				{
					auto_dock();
					if ((mcount & 127) == 0)
						info_message("Docking Computers On");
				}

				/* Roll and pitch settle back to level when not being steered */
				if (!rolling)
				{
					if (flight_roll > 0) decrease_flight_roll();
					if (flight_roll < 0) increase_flight_roll();
				}

				if (!climbing)
				{
					if (flight_climb > 0) decrease_flight_climb();
					if (flight_climb < 0) increase_flight_climb();
				}
			}

			/* ===== RENDER PHASE ===== */

			/* Move the stars by the fraction of a simulation step that
			 * really passed since the last frame. */
			now_ticks = timer_ticks();
			star_delta_scale = 1.0;
			if (last_frame_ticks != 0 && speed_cap > 0)
			{
				star_delta_scale = (double)TIMER_MICROS_LL(now_ticks - last_frame_ticks) /
								   (speed_cap * 1000.0);
				if (star_delta_scale > 1.5) star_delta_scale = 1.5;
				if (star_delta_scale < 0.0) star_delta_scale = 0.0;
			}
			last_frame_ticks = now_ticks;

			universe_render_only = !do_sim;
			space_view = is_space_view();

			if (!docked)
			{
				if (space_view)
				{
					gfx_clear_display();
					gfx_set_clip_region(1, 1, 510, GFX_VIEW_BY);
					update_starfield();
				}

				/* On the text screens the universe still moves, but it is
				 * drawn clipped away to nothing. */
				if (!space_view)
					gfx_set_clip_region(0, 0, -1, -1);
				update_universe();
				gfx_set_clip_region(1, 1, 510, GFX_VIEW_BY);

				if (!docked)
				{
					space_view = is_space_view();

					if (space_view)
					{
						if (draw_lasers)
						{
							draw_laser_lines();
							if (do_sim) draw_lasers--;
						}
						draw_laser_sights();

						if (message_count > 0)
						{
							/* A black strip keeps it readable over planets and suns. */
							int half = strlen(message_string) * 4 + 6;
							gfx_clear_area(256 - half, GFX_MESSAGE_Y - 2, 256 + half, GFX_MESSAGE_Y + 13);
							gfx_display_centre_text(GFX_MESSAGE_Y, message_string, 120, GFX_COL_WHITE);
						}
					}
					else if (do_sim && draw_lasers)
						draw_lasers--;

					if (hyper_ready)
					{
						display_hyper_status();
						if (do_sim && (mcount & 3) == 0)
							countdown_hyperspace();
					}

					if (do_sim)
					{
						mcount--;
						if (mcount < 0) mcount = 255;

						if ((mcount & 7) == 0) regenerate_shields();

						if ((mcount & 31) == 10)
						{
							if (energy < 50)
							{
								info_message("ENERGY LOW");
								snd_play_sample(SND_BEEP);
							}
							update_altitude();
						}

						if ((mcount & 31) == 20)
							update_cabin_temp();

						if ((mcount == 0) && (!witchspace))
							random_encounter();

						cool_laser();
						time_ecm();
					}
				}
			}

			/* These flags only mean anything inside this loop. */
			universe_render_only = 0;
			star_delta_scale = 1.0;

			if (game_over)
				break;

			if (current_screen == SCR_BREAK_PATTERN)
				display_break_pattern();

			/* Text screens are drawn once by their display_*() function and
			 * then left alone. The charts are the exception: they are drawn
			 * again whenever the cursor moves, so it leaves no trail. */
			if (is_chart())
			{
				if (cross_timer > 0)
				{
					cross_timer--;
					if (cross_timer == 0 && !find_input)
						show_distance_to_planet();
				}

				if (cross_x != old_cross_x || cross_y != old_cross_y)
				{
					int cx = cross_x, cy = cross_y;
					int had_find = find_input;

					if (current_screen == SCR_GALACTIC_CHART)
						display_galactic_chart();
					else
						display_short_range_chart();

					find_input = had_find;
					cross_x = cx;
					cross_y = cy;

					if (find_input)
						show_find_prompt();
					else if (cross_timer == 0)
					{
						show_distance_to_planet();
					}

					old_cross_x = cross_x;
					old_cross_y = cross_y;
					draw_cross(cross_x, cross_y);
				}
			}

			/* Dashboard or icon bar, whichever this screen has. */
			update_icon_bar_type();
			gfx_set_clip_region(1, 1, 510, GFX_VIEW_BY);
			update_console();
		}

		if (game_over < 2)
			run_game_over_screen();
	}

	snd_sound_shutdown();

	gfx_graphics_shutdown();

	return 0;
}
