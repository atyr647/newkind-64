/*
 * Elite - The New Kind.
 *
 * Nintendo 64 controller input routines using libdragon.
 *
 * The game logic still thinks in terms of the original keyboard flags
 * (kbd_F1_pressed and friends); this file turns the controller into those.
 *
 *   SPACE VIEWS (front / rear / left / right):
 *     Analog Stick     Pitch and roll
 *     C-Buttons        Pitch and roll (digital)
 *     D-Pad Up/Down    Speed up / slow down
 *     Z (held)         Slow down
 *     A (held)         Fire laser
 *     D-Pad Left/Right Move the icon bar pointer
 *     B                Use the selected icon
 *     R                Next view (front -> rear -> left -> right)
 *     L                Front view
 *     Start            Pause menu
 *
 *   EVERY OTHER SCREEN (status, market, charts, equip, ...):
 *     D-Pad / Stick    Move through lists, buy/sell, move the chart cursor
 *     A                Confirm / buy / select
 *     B                Cancel; in flight, back to the front view
 *     C-Left/C-Right   Move the icon bar pointer
 *     C-Down           Use the selected icon
 *     L                In flight: back to the front view
 *     Start            Pause menu
 *
 *   SHORTCUTS (hold the shoulder button, then press):
 *     R + D-Up         Galactic chart        L + D-Up     Commander status
 *     R + D-Down       Short range chart     L + D-Down   Inventory
 *     R + D-Left       Data on system        L + D-Left   Equip ship (docked)
 *     R + D-Right      Market prices         L + D-Right  Options
 *
 *     L+R + A          Hyperspace            L+R + B        Galactic hyperspace
 *     L+R + C-Up       Docking computer      L+R + C-Down   In-system jump
 *     L+R + C-Left     Energy bomb           L+R + C-Right  Escape pod
 *     L+R + D-Up       Chart distance / docking computer off
 *     L+R + D-Down     Scanner zoom          L+R + D-Left   Chart cursor home
 *     L+R + Z          Find planet (on a chart)
 *
 * (L+R+Start is not used: the controller itself treats that as "reset the
 * analog stick" and never reports it.)
 *
 * L and R on their own act when the button is RELEASED, and only if it was
 * not used for a shortcut, so reaching for a shortcut never changes the view.
 * Neither does anything while docked, so they can never launch the ship.
 */

#include <stdlib.h>
#include <string.h>

#include <libdragon.h>

#include "config.h"
#include "gfx.h"
#include "keyboard.h"
#include "elite.h"

extern int find_input;	/* n64_main.c: entering a planet name */

/* Analog stick deadzone */
#define STICK_DEADZONE  20

/* Auto-repeat for list navigation, in milliseconds. */
#define REPEAT_DELAY_MS  320
#define REPEAT_RATE_MS   90

int kbd_F1_pressed;
int kbd_F2_pressed;
int kbd_F3_pressed;
int kbd_F4_pressed;
int kbd_F5_pressed;
int kbd_F6_pressed;
int kbd_F7_pressed;
int kbd_F8_pressed;
int kbd_F9_pressed;
int kbd_F10_pressed;
int kbd_F11_pressed;
int kbd_F12_pressed;
int kbd_y_pressed;
int kbd_n_pressed;
int kbd_zoom_pressed;
int kbd_fire_pressed;
int kbd_ecm_pressed;
int kbd_energy_bomb_pressed;
int kbd_hyperspace_pressed;
int kbd_ctrl_pressed;
int kbd_jump_pressed;
int kbd_escape_pressed;
int kbd_dock_pressed;
int kbd_d_pressed;
int kbd_origin_pressed;
int kbd_find_pressed;
int kbd_fire_missile_pressed;
int kbd_target_missile_pressed;
int kbd_unarm_missile_pressed;
int kbd_pause_pressed;
int kbd_resume_pressed;
int kbd_inc_speed_pressed;
int kbd_dec_speed_pressed;
int kbd_up_pressed;
int kbd_down_pressed;
int kbd_left_pressed;
int kbd_right_pressed;
int kbd_enter_pressed;
int kbd_backspace_pressed;
int kbd_space_pressed;
int kbd_i_pressed;

/* Analog flight control (joystick mapped to roll/pitch) */
int joy_roll;       /* -127..+127, positive = stick right */
int joy_pitch;      /* -127..+127, positive = stick down (pull back) */

/* Icon bar navigation */
int kbd_icon_left_pressed;
int kbd_icon_right_pressed;
int kbd_icon_activate_pressed;

/* Digital flight from C-buttons */
int kbd_climb_pressed;
int kbd_dive_pressed;
int kbd_roll_left_pressed;
int kbd_roll_right_pressed;

char old_key[128];

/* Shoulder buttons: was the current hold used for a shortcut? */
static int prev_l, prev_r;
static int l_used, r_used;

/* After a shortcut the D-pad is ignored until it has been let go, so the
 * direction that opened a screen is not also acted on by that screen. */
static int dpad_locked;

/* Auto-repeat state for the four navigation directions. */
static int nav_held[4];
static long long nav_next[4];


int kbd_keyboard_startup(void)
{
	joypad_init();
	memset(old_key, 0, sizeof(old_key));
	prev_l = prev_r = l_used = r_used = 0;
	dpad_locked = 0;
	memset(nav_held, 0, sizeof(nav_held));
	return 0;
}

int kbd_keyboard_shutdown(void)
{
	return 0;
}

static int is_space_view(void)
{
	return (current_screen == SCR_FRONT_VIEW ||
		current_screen == SCR_REAR_VIEW ||
		current_screen == SCR_LEFT_VIEW ||
		current_screen == SCR_RIGHT_VIEW);
}

static int is_chart(void)
{
	return (current_screen == SCR_GALACTIC_CHART ||
		current_screen == SCR_SHORT_RANGE);
}

/*
 * Turn a held direction into "act now" pulses: once when first pressed,
 * then repeating after a short delay. The timing is in real time, so list
 * navigation feels the same whatever the frame rate.
 */
static int nav_repeat(int dir, int held)
{
	long long now;

	if (!held)
	{
		nav_held[dir] = 0;
		return 0;
	}

	now = timer_ticks();

	if (!nav_held[dir])
	{
		nav_held[dir] = 1;
		nav_next[dir] = now + TIMER_TICKS(REPEAT_DELAY_MS * 1000LL);
		return 1;
	}

	if (now >= nav_next[dir])
	{
		nav_next[dir] = now + TIMER_TICKS(REPEAT_RATE_MS * 1000LL);
		return 1;
	}

	return 0;
}

static void clear_flags(void)
{
	kbd_F1_pressed = 0;
	kbd_F2_pressed = 0;
	kbd_F3_pressed = 0;
	kbd_F4_pressed = 0;
	kbd_F5_pressed = 0;
	kbd_F6_pressed = 0;
	kbd_F7_pressed = 0;
	kbd_F8_pressed = 0;
	kbd_F9_pressed = 0;
	kbd_F10_pressed = 0;
	kbd_F11_pressed = 0;
	kbd_F12_pressed = 0;
	kbd_y_pressed = 0;
	kbd_n_pressed = 0;
	kbd_zoom_pressed = 0;
	kbd_fire_pressed = 0;
	kbd_ecm_pressed = 0;
	kbd_energy_bomb_pressed = 0;
	kbd_hyperspace_pressed = 0;
	kbd_ctrl_pressed = 0;
	kbd_jump_pressed = 0;
	kbd_escape_pressed = 0;
	kbd_dock_pressed = 0;
	kbd_d_pressed = 0;
	kbd_origin_pressed = 0;
	kbd_find_pressed = 0;
	kbd_fire_missile_pressed = 0;
	kbd_target_missile_pressed = 0;
	kbd_unarm_missile_pressed = 0;
	kbd_pause_pressed = 0;
	kbd_resume_pressed = 0;
	kbd_inc_speed_pressed = 0;
	kbd_dec_speed_pressed = 0;
	kbd_up_pressed = 0;
	kbd_down_pressed = 0;
	kbd_left_pressed = 0;
	kbd_right_pressed = 0;
	kbd_enter_pressed = 0;
	kbd_backspace_pressed = 0;
	kbd_space_pressed = 0;
	kbd_i_pressed = 0;

	joy_roll = 0;
	joy_pitch = 0;
	kbd_icon_left_pressed = 0;
	kbd_icon_right_pressed = 0;
	kbd_icon_activate_pressed = 0;
	kbd_climb_pressed = 0;
	kbd_dive_pressed = 0;
	kbd_roll_left_pressed = 0;
	kbd_roll_right_pressed = 0;
}


void kbd_poll_keyboard(void)
{
	joypad_buttons_t btns, pressed;
	joypad_inputs_t inputs;
	int stick_x, stick_y;
	int l_held, r_held;
	int l_released, r_released;
	int dpad_any, dpad_hit;
	int space_view;

	joypad_poll();
	btns = joypad_get_buttons(JOYPAD_PORT_1);
	pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);
	inputs = joypad_get_inputs(JOYPAD_PORT_1);

	stick_x = inputs.stick_x;
	stick_y = inputs.stick_y;

	clear_flags();

	space_view = is_space_view();

	/* Shoulder button bookkeeping. */
	l_held = btns.l;
	r_held = btns.r;
	l_released = prev_l && !l_held;
	r_released = prev_r && !r_held;
	if (l_held && !prev_l) l_used = 0;
	if (r_held && !prev_r) r_used = 0;
	prev_l = l_held;
	prev_r = r_held;

	/* D-pad lock after a shortcut. */
	dpad_any = btns.d_up || btns.d_down || btns.d_left || btns.d_right;
	if (dpad_locked)
	{
		if (!dpad_any)
			dpad_locked = 0;
		btns.d_up = btns.d_down = btns.d_left = btns.d_right = 0;
		pressed.d_up = pressed.d_down = pressed.d_left = pressed.d_right = 0;
	}
	dpad_hit = pressed.d_up || pressed.d_down || pressed.d_left || pressed.d_right;

	/*
	 * L + R held together: the action shortcuts.
	 */
	if (l_held && r_held)
	{
		l_used = r_used = 1;

		if (pressed.a)       kbd_hyperspace_pressed = 1;
		if (pressed.b)       { kbd_hyperspace_pressed = 1; kbd_ctrl_pressed = 1; }
		if (pressed.c_up)    kbd_dock_pressed = 1;
		if (pressed.c_down)  kbd_jump_pressed = 1;
		if (pressed.c_left)  { kbd_energy_bomb_pressed = 1; kbd_ctrl_pressed = 1; }
		if (pressed.c_right) { kbd_escape_pressed = 1; kbd_ctrl_pressed = 1; }
		if (pressed.z)       kbd_find_pressed = 1;
		if (pressed.d_up)    kbd_d_pressed = 1;
		if (pressed.d_down)  kbd_zoom_pressed = 1;
		if (pressed.d_left)  kbd_origin_pressed = 1;

		if (dpad_hit)
			dpad_locked = 1;
		return;
	}

	/*
	 * R + D-pad and L + D-pad: jump straight to a screen.
	 */
	if (r_held && dpad_hit)
	{
		if (pressed.d_up)         kbd_F5_pressed = 1;   /* Galactic chart */
		else if (pressed.d_down)  kbd_F6_pressed = 1;   /* Short range chart */
		else if (pressed.d_left)  kbd_F7_pressed = 1;   /* Data on system */
		else                      kbd_F8_pressed = 1;   /* Market prices */
		r_used = 1;
		dpad_locked = 1;
		return;
	}

	if (l_held && dpad_hit)
	{
		if (pressed.d_up)         kbd_F9_pressed = 1;   /* Commander status */
		else if (pressed.d_down)  kbd_F10_pressed = 1;  /* Inventory */
		else if (pressed.d_left)  { if (docked) kbd_F4_pressed = 1; } /* Equip ship */
		else                      kbd_F11_pressed = 1;  /* Options */
		l_used = 1;
		dpad_locked = 1;
		return;
	}

	/*
	 * L or R tapped on their own (in flight only).
	 */
	if (!docked)
	{
		if (l_released && !l_used)
			kbd_F1_pressed = 1;                         /* Front view */

		if (r_released && !r_used)
		{
			switch (current_screen)
			{
				case SCR_FRONT_VIEW: kbd_F2_pressed = 1; break;  /* -> rear */
				case SCR_REAR_VIEW:  kbd_F3_pressed = 1; break;  /* -> left */
				case SCR_LEFT_VIEW:  kbd_F4_pressed = 1; break;  /* -> right */
				default:             kbd_F1_pressed = 1; break;  /* -> front */
			}
		}
	}

	/*
	 * A: fire laser (held) / confirm (pressed).
	 */
	if (btns.a && space_view)
		kbd_fire_pressed = 1;
	if (pressed.a)
	{
		kbd_enter_pressed = 1;
		kbd_y_pressed = 1;
	}

	/*
	 * Start: pause menu in the game; "continue" on title and briefings.
	 */
	if (pressed.start)
	{
		kbd_pause_pressed = 1;
		kbd_resume_pressed = 1;
		kbd_space_pressed = 1;
	}

	if (space_view)
	{
		/* B uses the selected icon; D-pad left/right moves the pointer. */
		if (pressed.b)        kbd_icon_activate_pressed = 1;
		if (pressed.d_left)   kbd_icon_left_pressed = 1;
		if (pressed.d_right)  kbd_icon_right_pressed = 1;

		/* Throttle. */
		if (btns.d_up)        kbd_inc_speed_pressed = 1;
		if (btns.d_down)      kbd_dec_speed_pressed = 1;
		if (btns.z)           kbd_dec_speed_pressed = 1;

		/* C-buttons: digital pitch and roll. */
		if (btns.c_up)    kbd_climb_pressed = 1;
		if (btns.c_down)  kbd_dive_pressed = 1;
		if (btns.c_left)  kbd_roll_left_pressed = 1;
		if (btns.c_right) kbd_roll_right_pressed = 1;

		/* Analog stick: pitch and roll. */
		if (abs(stick_x) > STICK_DEADZONE)
			joy_roll = stick_x;
		if (abs(stick_y) > STICK_DEADZONE)
			joy_pitch = -stick_y;

		memset(nav_held, 0, sizeof(nav_held));
	}
	else
	{
		int up    = btns.d_up    || (stick_y >  STICK_DEADZONE * 2);
		int down  = btns.d_down  || (stick_y < -STICK_DEADZONE * 2);
		int left  = btns.d_left  || (stick_x < -STICK_DEADZONE * 2);
		int right = btns.d_right || (stick_x >  STICK_DEADZONE * 2);

		/* B cancels / answers "no" / deletes a letter. */
		if (pressed.b)
		{
			kbd_n_pressed = 1;
			kbd_backspace_pressed = 1;
		}

		/* The icon bar is on the C-buttons here, as the D-pad is busy. */
		if (pressed.c_left)   kbd_icon_left_pressed = 1;
		if (pressed.c_right)  kbd_icon_right_pressed = 1;
		if (pressed.c_down)   kbd_icon_activate_pressed = 1;

		if (is_chart() && !find_input)
		{
			/* The chart cursor glides for as long as the direction is held. */
			kbd_up_pressed = up;
			kbd_down_pressed = down;
			kbd_left_pressed = left;
			kbd_right_pressed = right;
			memset(nav_held, 0, sizeof(nav_held));
		}
		else
		{
			kbd_up_pressed    = nav_repeat(0, up);
			kbd_down_pressed  = nav_repeat(1, down);
			kbd_left_pressed  = nav_repeat(2, left);
			kbd_right_pressed = nav_repeat(3, right);
		}
	}
}


/*
 * Read a single key for text input (planet finder).
 * On N64 this is a no-op - actual input handled via kbd_* flags.
 */
int kbd_read_key(void)
{
	return 0;
}


void kbd_clear_key_buffer(void)
{
	/* No-op on N64 */
}
