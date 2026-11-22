/*
 * Elite - The New Kind.
 *
 * Nintendo 64: hardware-rendered ships ("Enhanced" graphics).
 *
 * The ships keep their original Elite geometry and face colours (shipdata.c
 * and shipface.c). Instead of being filled by the CPU and sorted face by
 * face, each ship type is turned once into a tiny3d vertex buffer and a
 * recorded display list; every frame the ships in view are transformed and
 * lit by the RSP and drawn by the RDP with a depth buffer. That gives
 * correct overlaps between ships, shading from the system's sun, and far
 * more ships on screen for the same cost.
 *
 * Coordinates: the game works in a view space where x is right, y is up and
 * z is forwards. tiny3d (like OpenGL) looks down the negative z axis, so the
 * model matrix built here negates z; everything else is passed through in
 * game units.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>

#include "config.h"
#include "gfx.h"
#include "elite.h"
#include "vector.h"
#include "shipdata.h"
#include "shipface.h"
#include "space.h"
#include "n64_ships3d.h"

#define MAX_HW_SHIPS	24		/* ships drawn by hardware per frame */
#define VERT_CACHE		64		/* vertices loaded into the RSP at a time */
#define MAX_BATCHES		16

/* The fixed-point matrices hold positions up to +/-32767 game units. Ships
 * further away than this are a few pixels across and are left to the
 * software renderer. */
#define HW_MAX_DISTANCE	30000.0

#define HW_NEAR_PLANE	30.0f
#define HW_FAR_PLANE	34000.0f

struct hw_model
{
	int built;				/* 0 = not tried yet, 1 = ready, -1 = unusable */
	int has_lines;			/* the model has 2-point "line" faces */
	T3DVertPacked *verts;
	sprite_t *texture;		/* NULL = flat face colours */
	int num_batches;
	short batch_base[MAX_BATCHES];		/* first vertex of each batch (even) */
	short batch_verts[MAX_BATCHES];		/* vertices to load (even) */
	short batch_tri_first[MAX_BATCHES + 1];	/* index into tris[] */
	unsigned char *tris;				/* triangles: 3 cache indices each */
};

struct hw_queued
{
	int type;
	struct vector location;
	Matrix rotmat;
};

static struct hw_model models[NO_OF_SHIPS + 1];
static struct hw_queued queue[MAX_HW_SHIPS];
static T3DMat4FP *matrices;
static int queued;

static T3DViewport viewport;
static int initialised;

/* Direction towards the light, in game view space. */
static struct vector light_dir = { -0.4, 0.6, -0.7 };


static void build_model(int type);

void hw_ships_init(void)
{
	t3d_init((T3DInitParams){});
	viewport = t3d_viewport_create();
	matrices = malloc_uncached(sizeof(T3DMat4FP) * MAX_HW_SHIPS);
	memset(models, 0, sizeof(models));
	queued = 0;
	initialised = (matrices != NULL);
#ifdef HW_PREBUILD
	{ int t; for (t = 1; t <= HW_PREBUILD; t++) build_model(t); }
#endif
}


/*
 * Set where the light comes from, as a direction in game view space
 * (it does not need to be normalised). Called once per frame.
 */
void hw_ships_set_light(double x, double y, double z)
{
	double len = sqrt(x * x + y * y + z * z);

	if (len < 0.0001)
		return;

	light_dir.x = x / len;
	light_dir.y = y / len;
	light_dir.z = z / len;
}


static int face_point(const struct ship_face *face, int n)
{
	switch (n)
	{
		case 0: return face->p1;
		case 1: return face->p2;
		case 2: return face->p3;
		case 3: return face->p4;
		case 4: return face->p5;
		case 5: return face->p6;
		case 6: return face->p7;
		default: return face->p8;
	}
}


/*
 * Build the vertex buffer and display list for one ship type.
 *
 * Every face gets its own vertices (so it can carry its own normal and
 * colour: the ships are flat shaded, as they always were) and is drawn as a
 * triangle fan. Faces are grouped into batches that fit the RSP's vertex
 * cache; each batch starts on an even vertex because tiny3d stores
 * vertices in pairs.
 */
static void build_model(int type)
{
	struct hw_model *model = &models[type];
	struct ship_solid *solid = &ship_solids[type];
	struct ship_data *ship = ship_list[type];
	int total = 0;
	int next = 0;
	int batch_count;
	int i, k;

	const struct ship_texture *tex = NULL;

	model->built = -1;

	if (!ship || !solid->face_data || solid->num_faces <= 0)
		return;

	/* Is there a texture for this ship? */
	for (i = 0; ship_textures[i].sprite_file; i++)
	{
		if (ship_textures[i].ship_type == type)
		{
			model->texture = sprite_load(ship_textures[i].sprite_file);
			if (model->texture)
				tex = &ship_textures[i];
			break;
		}
	}

	/* Worst case size: every face, plus one padding vertex per batch. */
	for (i = 0; i < solid->num_faces; i++)
	{
		if (solid->face_data[i].points >= 3)
			total += solid->face_data[i].points + 1;
		else
			model->has_lines = 1;
	}

	if (total == 0)
		return;

	total = (total + 1) & ~1;
	model->verts = malloc_uncached(sizeof(T3DVertPacked) * (total / 2));
	if (!model->verts)
		return;
	memset(model->verts, 0, sizeof(T3DVertPacked) * (total / 2));

	{
		/* Pass 1: fill the vertex buffer, remembering where each face went. */
		int *face_base = malloc(sizeof(int) * solid->num_faces);
		int *face_batch = malloc(sizeof(int) * solid->num_faces);
		int batches = 0;
		int batch_base[MAX_BATCHES + 1];

		if (!face_base || !face_batch)
		{
			free(face_base);
			free(face_batch);
			return;
		}

		next = 0;
		batch_count = 0;
		batch_base[0] = 0;

		for (i = 0; i < solid->num_faces; i++)
		{
			const struct ship_face *face = &solid->face_data[i];
			T3DVec3 normal;
			uint16_t packed_normal;
			int r, g, b;
			uint32_t rgba;
			float len;

			face_batch[i] = -1;

			if (face->points < 3)
				continue;

			if (batch_count + face->points > VERT_CACHE)
			{
				next = (next + 1) & ~1;
				batches++;
				if (batches >= MAX_BATCHES)
					break;
				batch_base[batches] = next;
				batch_count = 0;
			}

			normal.v[0] = face->norm_x;
			normal.v[1] = face->norm_y;
			normal.v[2] = face->norm_z;
			len = sqrtf(normal.v[0] * normal.v[0] + normal.v[1] * normal.v[1] + normal.v[2] * normal.v[2]);
			if (len < 0.001f)
			{
				normal.v[0] = 0;
				normal.v[1] = 1;
				normal.v[2] = 0;
			}
			else
			{
				normal.v[0] /= len;
				normal.v[1] /= len;
				normal.v[2] /= len;
			}
			packed_normal = t3d_vert_pack_normal(&normal);

			/* Textured ships take their colour from the texture. */
			if (tex && i < tex->num_faces)
				r = g = b = 255;
			else
				gfx_palette_rgb(face->colour, &r, &g, &b);
			rgba = ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0xFF;

			face_batch[i] = batches;
			face_base[i] = next - batch_base[batches];

			for (k = 0; k < face->points; k++)
			{
				const struct ship_point *p = &ship->points[face_point(face, k)];
				int16_t *pos = t3d_vertbuffer_get_pos(model->verts, next);

				pos[0] = p->x;
				pos[1] = p->y;
				pos[2] = p->z;
				*t3d_vertbuffer_get_norm(model->verts, next) = packed_normal;
				*t3d_vertbuffer_get_color(model->verts, next) = rgba;
				if (tex && i < tex->num_faces)
				{
					int16_t *uv = t3d_vertbuffer_get_uv(model->verts, next);
					uv[0] = tex->uv[i][k * 2];
					uv[1] = tex->uv[i][k * 2 + 1];
				}
				next++;
				batch_count++;
			}
		}

		/* Pass 2: list each batch's triangles (faces as triangle fans). */
		{
			int num_tris = 0;
			int t;

			for (i = 0; i < solid->num_faces; i++)
				if (face_batch[i] >= 0)
					num_tris += solid->face_data[i].points - 2;

			model->tris = malloc(num_tris * 3 + 1);
			model->num_batches = 0;

			if (model->tris && batches < MAX_BATCHES)
			{
				int n = 0;

				for (k = 0; k <= batches; k++)
				{
					int end = (k < batches) ? batch_base[k + 1] : ((next + 1) & ~1);

					model->batch_base[k] = batch_base[k];
					model->batch_verts[k] = end - batch_base[k];
					model->batch_tri_first[k] = n;

					for (i = 0; i < solid->num_faces; i++)
					{
						if (face_batch[i] != k)
							continue;
						for (t = 1; t + 1 < solid->face_data[i].points; t++)
						{
							model->tris[n * 3] = face_base[i];
							model->tris[n * 3 + 1] = face_base[i] + t;
							model->tris[n * 3 + 2] = face_base[i] + t + 1;
							n++;
						}
					}
				}

				model->batch_tri_first[batches + 1] = n;
				model->num_batches = batches + 1;
			}
		}

		free(face_base);
		free(face_batch);
	}

	if (model->num_batches > 0)
		model->built = 1;
}


/* Send one model's vertices and triangles to the RSP. */
static void draw_model(const struct hw_model *model)
{
	int k, t;

	for (k = 0; k < model->num_batches; k++)
	{
		if (model->batch_verts[k] <= 0)
			continue;

		t3d_vert_load(&model->verts[model->batch_base[k] / 2], 0, model->batch_verts[k]);

		for (t = model->batch_tri_first[k]; t < model->batch_tri_first[k + 1]; t++)
			t3d_tri_draw(model->tris[t * 3], model->tris[t * 3 + 1], model->tris[t * 3 + 2]);

		t3d_tri_sync();
	}
}



/*
 * Ask for a ship to be drawn by hardware this frame. Returns 0 if it
 * cannot be (the caller then draws it in software as before).
 */
int hw_ships_queue(struct univ_object *ship)
{
	T3DMat4 m;
	int type = ship->type;
	int i;

	if (!initialised || !gfx_hw_available())
		return 0;

	if (type <= 0 || type > NO_OF_SHIPS || queued >= MAX_HW_SHIPS)
		return 0;

	if (fabs(ship->location.x) > HW_MAX_DISTANCE ||
		fabs(ship->location.y) > HW_MAX_DISTANCE ||
		fabs(ship->location.z) > HW_MAX_DISTANCE)
		return 0;

	if (models[type].built == 0)
		build_model(type);
	if (models[type].built != 1)
		return 0;

	/* Columns are the ship's side, roof and nose vectors, then its
	 * position; z is negated to go from game space to tiny3d's. */
	t3d_mat4_identity(&m);
	for (i = 0; i < 3; i++)
	{
		m.m[i][0] = ship->rotmat[i].x;
		m.m[i][1] = ship->rotmat[i].y;
		m.m[i][2] = -ship->rotmat[i].z;
	}
	m.m[3][0] = ship->location.x;
	m.m[3][1] = ship->location.y;
	m.m[3][2] = -ship->location.z;

	t3d_mat4_to_fixed(&matrices[queued], &m);

	queue[queued].type = type;
	queue[queued].location = ship->location;
	memcpy(queue[queued].rotmat, ship->rotmat, sizeof(Matrix));
	queued++;

	return 1;
}


/*
 * Some ship models include single lines (aerials and the like). They are
 * drawn by the CPU on top of the finished hardware picture.
 */
static void draw_model_lines(const struct hw_queued *q)
{
	struct ship_solid *solid = &ship_solids[q->type];
	struct ship_data *ship = ship_list[q->type];
	int i, k;

	for (i = 0; i < solid->num_faces; i++)
	{
		const struct ship_face *face = &solid->face_data[i];
		int sx[2], sy[2];
		int ok = 1;

		if (face->points != 2)
			continue;

		for (k = 0; k < 2; k++)
		{
			const struct ship_point *p = &ship->points[face_point(face, k)];
			double rx = p->x * q->rotmat[0].x + p->y * q->rotmat[1].x + p->z * q->rotmat[2].x + q->location.x;
			double ry = p->x * q->rotmat[0].y + p->y * q->rotmat[1].y + p->z * q->rotmat[2].y + q->location.y;
			double rz = p->x * q->rotmat[0].z + p->y * q->rotmat[1].z + p->z * q->rotmat[2].z + q->location.z;

			if (rz <= 1.0)
			{
				ok = 0;
				break;
			}

			sx[k] = (int)(((rx * 256) / rz + 128) * GFX_SCALE);
			sy[k] = (int)((-(ry * 256) / rz + GFX_VIEW_CY) * GFX_SCALE);
		}

		if (ok)
			gfx_draw_colour_line(sx[0], sy[0], sx[1], sy[1], face->colour);
	}
}


/*
 * Draw everything queued this frame. Called once the software part of
 * the 3D scene (stars, planet, sun, distant ships) is in place.
 */
void hw_ships_flush(void)
{
	static const uint8_t ambient[4] = { 110, 110, 125, 255 };
	static const uint8_t sunlight[4] = { 215, 210, 195, 255 };
	T3DVec3 dir;
	int height = GFX_VIEW_BY + 1;
	int i;

	if (queued == 0)
		return;

	if (!gfx_hw_available())
	{
		queued = 0;
		return;
	}

	gfx_hw_wait();

	t3d_frame_start();

	/* Match the game's own projection: a point at (x, y, z) lands
	 * 512 * x / z pixels from the centre of the view. */
	t3d_viewport_set_area(&viewport, GFX_X_OFFSET, GFX_Y_OFFSET, 512, height);
	t3d_viewport_set_projection(&viewport, 2.0f * atanf((height / 2.0f) / 512.0f),
								HW_NEAR_PLANE, HW_FAR_PLANE);
	t3d_viewport_look_at(&viewport, &(T3DVec3){{0, 0, 0}}, &(T3DVec3){{0, 0, -1}}, &(T3DVec3){{0, 1, 0}});
	t3d_viewport_attach(&viewport);

	rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
	t3d_screen_clear_depth();

#ifndef HW_LIGHT_SIGN
#define HW_LIGHT_SIGN 1
#endif
	dir.v[0] = HW_LIGHT_SIGN * light_dir.x;
	dir.v[1] = HW_LIGHT_SIGN * light_dir.y;
	dir.v[2] = HW_LIGHT_SIGN * -light_dir.z;
	t3d_light_set_ambient(ambient);
	t3d_light_set_directional(0, sunlight, &dir);
	t3d_light_set_count(1);

	/* Flat-coloured ships first, then the textured ones, so the render
	 * mode only has to change when the texture does. */
	t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH);

	for (i = 0; i < queued; i++)
	{
		if (models[queue[i].type].texture)
			continue;
		t3d_matrix_push(&matrices[i]);
		draw_model(&models[queue[i].type]);
		t3d_matrix_pop(1);
	}

	{
		sprite_t *current = NULL;

		for (i = 0; i < queued; i++)
		{
			sprite_t *texture = models[queue[i].type].texture;

			if (!texture)
				continue;

			if (texture != current)
			{
				if (!current)
				{
					rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
					rdpq_mode_filter(FILTER_BILINEAR);
					t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | T3D_FLAG_TEXTURED);
				}
				rdpq_sprite_upload(TILE0, texture, NULL);
				current = texture;
			}

			t3d_matrix_push(&matrices[i]);
			draw_model(&models[queue[i].type]);
			t3d_matrix_pop(1);
		}
	}

	/* Wait for the RDP, then give the whole screen back to 2D drawing. */
	gfx_hw_mark_busy();
	gfx_hw_wait();
	rdpq_set_scissor(0, 0, N64_SCREEN_W, N64_SCREEN_H);

	for (i = 0; i < queued; i++)
		if (models[queue[i].type].has_lines)
			draw_model_lines(&queue[i]);

	queued = 0;
}
