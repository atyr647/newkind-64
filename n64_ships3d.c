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
	const struct ship_mesh *mesh;		/* replacement model, or NULL */
	sprite_t **sprites;					/* its textures */
	T3DVertPacked **mesh_verts;			/* its vertices, one buffer per batch */
	T3DVertPacked **mesh_env_verts;		/* the same with smooth normals */
	T3DVertPacked *env_verts;			/* plain model: verts with smooth normals */
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

/*
 * Metal reflections. After a ship is drawn, its triangles are drawn again
 * with an environment texture whose coordinates the RSP works out from each
 * vertex's normal as seen from the camera (spherical environment mapping).
 * That pass is blended over the paint where the ship already is, so
 * highlights slide across the hull as it turns. It uses smooth normals (the
 * average of the faces meeting at a corner), kept in a second set of
 * vertex buffers, so a highlight moves across a face rather than lighting
 * it all at once.
 */
static sprite_t *env_sprite;

/* Direction towards the light, in game view space. */
static struct vector light_dir = { -0.4, 0.6, -0.7 };


static void build_model(int type);
static void build_mesh_model(struct hw_model *model, const struct ship_mesh *mesh);

void hw_ships_init(void)
{
	t3d_init((T3DInitParams){});
	viewport = t3d_viewport_create();
	matrices = malloc_uncached(sizeof(T3DMat4FP) * MAX_HW_SHIPS);
	memset(models, 0, sizeof(models));
	queued = 0;
	initialised = (matrices != NULL);
	env_sprite = sprite_load("rom:/env_metal.sprite");
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
 * Set up a replacement model: load its textures and pack its vertices
 * into tiny3d's format, one buffer per batch.
 */
static void build_mesh_model(struct hw_model *model, const struct ship_mesh *mesh)
{
	int i, k;

	model->sprites = calloc(mesh->num_charts, sizeof(sprite_t *));
	model->mesh_verts = calloc(mesh->num_batches, sizeof(T3DVertPacked *));
	model->mesh_env_verts = calloc(mesh->num_batches, sizeof(T3DVertPacked *));
	if (!model->sprites || !model->mesh_verts || !model->mesh_env_verts)
		return;

	for (i = 0; i < mesh->num_charts; i++)
	{
		model->sprites[i] = sprite_load(mesh->sprite_files[i]);
		if (!model->sprites[i])
			return;
	}

	for (i = 0; i < mesh->num_batches; i++)
	{
		const struct ship_mesh_batch *b = &mesh->batches[i];
		int pairs = (b->num_verts + 1) / 2;
		T3DVertPacked *buf = malloc_uncached(sizeof(T3DVertPacked) * pairs);
		T3DVertPacked *env = malloc_uncached(sizeof(T3DVertPacked) * pairs);

		if (!buf || !env)
			return;
		memset(buf, 0, sizeof(T3DVertPacked) * pairs);
		memset(env, 0, sizeof(T3DVertPacked) * pairs);

		for (k = 0; k < b->num_verts; k++)
		{
			const short *v = &b->verts[k * SHIP_MESH_STRIDE];
			int16_t *pos = t3d_vertbuffer_get_pos(buf, k);
			int16_t *uv = t3d_vertbuffer_get_uv(buf, k);
			T3DVec3 n = {{ v[3] / 127.0f, v[4] / 127.0f, v[5] / 127.0f }};

			pos[0] = v[0];
			pos[1] = v[1];
			pos[2] = v[2];
			uv[0] = v[6];
			uv[1] = v[7];
			*t3d_vertbuffer_get_norm(buf, k) = t3d_vert_pack_normal(&n);
			*t3d_vertbuffer_get_color(buf, k) = 0xFFFFFFFF;

			{
				T3DVec3 sn = {{ v[8] / 127.0f, v[9] / 127.0f, v[10] / 127.0f }};
				int16_t *epos = t3d_vertbuffer_get_pos(env, k);
				epos[0] = v[0];
				epos[1] = v[1];
				epos[2] = v[2];
				*t3d_vertbuffer_get_norm(env, k) = t3d_vert_pack_normal(&sn);
				*t3d_vertbuffer_get_color(env, k) = 0xFFFFFFFF;
			}
		}

		model->mesh_verts[i] = buf;
		model->mesh_env_verts[i] = env;
	}

	model->mesh = mesh;
	model->built = 1;
}


/*
 * The smooth normal at one corner of a ship: the average of the faces that
 * meet there, leaving out faces at a sharp angle to this one so hard
 * creases stay crisp.
 */
static T3DVec3 smooth_normal(const struct ship_solid *solid, int point, const T3DVec3 *face_normal)
{
	T3DVec3 sn = {{0, 0, 0}};
	int f, q;

	for (f = 0; f < solid->num_faces; f++)
	{
		const struct ship_face *g = &solid->face_data[f];
		int hit = 0;
		float gl;

		if (g->points < 3)
			continue;
		for (q = 0; q < g->points; q++)
			if (face_point(g, q) == point)
				hit = 1;
		if (!hit)
			continue;
		gl = sqrtf(g->norm_x * g->norm_x + g->norm_y * g->norm_y + g->norm_z * g->norm_z);
		if (gl < 0.001f)
			continue;
		if ((g->norm_x * face_normal->v[0] + g->norm_y * face_normal->v[1] + g->norm_z * face_normal->v[2]) / gl < 0.2f)
			continue;
		sn.v[0] += g->norm_x / gl;
		sn.v[1] += g->norm_y / gl;
		sn.v[2] += g->norm_z / gl;
	}

	if (sn.v[0] == 0 && sn.v[1] == 0 && sn.v[2] == 0)
		return *face_normal;
	t3d_vec3_norm(&sn);
	return sn;
}


/*
 * Some faces lie exactly on top of a larger one (the slot on a station,
 * engines on a rear face). With a depth buffer those would flicker, so a
 * face that shares its plane with a bigger face is lifted slightly off it.
 * Returns how far to move face i along its normal.
 */
static float face_area(const struct ship_data *ship, const struct ship_face *face)
{
	const struct ship_point *p0 = &ship->points[face_point(face, 0)];
	float ax = 0, ay = 0, az = 0;
	int k;

	for (k = 1; k + 1 < face->points; k++)
	{
		const struct ship_point *p1 = &ship->points[face_point(face, k)];
		const struct ship_point *p2 = &ship->points[face_point(face, k + 1)];
		float ux = p1->x - p0->x, uy = p1->y - p0->y, uz = p1->z - p0->z;
		float vx = p2->x - p0->x, vy = p2->y - p0->y, vz = p2->z - p0->z;
		ax += uy * vz - uz * vy;
		ay += uz * vx - ux * vz;
		az += ux * vy - uy * vx;
	}
	return 0.5f * sqrtf(ax * ax + ay * ay + az * az);
}

static float face_lift(const struct ship_data *ship, const struct ship_solid *solid, int i)
{
	const struct ship_face *fi = &solid->face_data[i];
	float li = sqrtf(fi->norm_x * fi->norm_x + fi->norm_y * fi->norm_y + fi->norm_z * fi->norm_z);
	const struct ship_point *pi = &ship->points[face_point(fi, 0)];
	float di, ai;
	int j;

	if (li < 0.001f || fi->points < 3)
		return 0;

	di = (pi->x * fi->norm_x + pi->y * fi->norm_y + pi->z * fi->norm_z) / li;
	ai = face_area(ship, fi);

	for (j = 0; j < solid->num_faces; j++)
	{
		const struct ship_face *fj = &solid->face_data[j];
		float lj = sqrtf(fj->norm_x * fj->norm_x + fj->norm_y * fj->norm_y + fj->norm_z * fj->norm_z);
		const struct ship_point *pj;
		float cosang, dj;

		if (j == i || fj->points < 3 || lj < 0.001f)
			continue;

		cosang = (fi->norm_x * fj->norm_x + fi->norm_y * fj->norm_y + fi->norm_z * fj->norm_z) / (li * lj);
		if (cosang < 0.999f)
			continue;

		pj = &ship->points[face_point(fj, 0)];
		dj = (pj->x * fi->norm_x + pj->y * fi->norm_y + pj->z * fi->norm_z) / li;
		if (fabsf(di - dj) < 1.0f && face_area(ship, fj) > ai * 1.05f)
			return 2.0f;
	}
	return 0;
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

	model->built = -1;

	/* Single-line faces (aerials and so on) are drawn separately. */
	if (solid->face_data)
		for (i = 0; i < solid->num_faces; i++)
			if (solid->face_data[i].points == 2)
				model->has_lines = 1;

	/* A detailed model takes precedence over the plain geometry. */
	for (i = 0; ship_meshes[i].num_batches; i++)
	{
		if (ship_meshes[i].ship_type == type)
		{
			build_mesh_model(model, &ship_meshes[i]);
			if (model->built == 1)
				return;
			break;
		}
	}

	if (!ship || !solid->face_data || solid->num_faces <= 0)
		return;

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
	model->env_verts = malloc_uncached(sizeof(T3DVertPacked) * (total / 2));
	if (!model->verts || !model->env_verts)
		return;
	memset(model->verts, 0, sizeof(T3DVertPacked) * (total / 2));
	memset(model->env_verts, 0, sizeof(T3DVertPacked) * (total / 2));

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
			float lift;

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
			lift = face_lift(ship, solid, i);

			gfx_palette_rgb(face->colour, &r, &g, &b);
			rgba = ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0xFF;

			face_batch[i] = batches;
			face_base[i] = next - batch_base[batches];

			for (k = 0; k < face->points; k++)
			{
				const struct ship_point *p = &ship->points[face_point(face, k)];
				int16_t *pos = t3d_vertbuffer_get_pos(model->verts, next);

				pos[0] = p->x + (int)lroundf(normal.v[0] * lift);
				pos[1] = p->y + (int)lroundf(normal.v[1] * lift);
				pos[2] = p->z + (int)lroundf(normal.v[2] * lift);
				*t3d_vertbuffer_get_norm(model->verts, next) = packed_normal;
				{
					T3DVec3 sn = smooth_normal(solid, face_point(face, k), &normal);
					int16_t *epos = t3d_vertbuffer_get_pos(model->env_verts, next);
					epos[0] = pos[0];
					epos[1] = pos[1];
					epos[2] = pos[2];
					*t3d_vertbuffer_get_norm(model->env_verts, next) = t3d_vert_pack_normal(&sn);
					*t3d_vertbuffer_get_color(model->env_verts, next) = 0xFFFFFFFF;
				}

				*t3d_vertbuffer_get_color(model->verts, next) = rgba;

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
	static const uint8_t ambient[4] = { 165, 165, 175, 255 };
	static const uint8_t sunlight[4] = { 170, 165, 150, 255 };
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
		if (models[queue[i].type].mesh)
			continue;
		t3d_matrix_push(&matrices[i]);
		draw_model(&models[queue[i].type]);
		t3d_matrix_pop(1);
	}


	/* Ships with replacement models: textured, one texture per batch. */
	{
		sprite_t *current = NULL;

		for (i = 0; i < queued; i++)
		{
			const struct hw_model *model = &models[queue[i].type];
			int b, t;

			if (!model->mesh)
				continue;

			if (!current)
			{
				rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
				rdpq_mode_filter(FILTER_BILINEAR);
				t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | T3D_FLAG_TEXTURED);
			}

			t3d_matrix_push(&matrices[i]);

			for (b = 0; b < model->mesh->num_batches; b++)
			{
				const struct ship_mesh_batch *batch = &model->mesh->batches[b];
				sprite_t *sprite = model->sprites[batch->chart];

				if (sprite != current)
				{
					rdpq_sprite_upload(TILE0, sprite, NULL);
					current = sprite;
				}

				t3d_vert_load(model->mesh_verts[b], 0, (batch->num_verts + 1) & ~1);
				for (t = 0; t < batch->num_tris; t++)
					t3d_tri_draw(batch->tris[t * 3], batch->tris[t * 3 + 1], batch->tris[t * 3 + 2]);
				t3d_tri_sync();
			}

			t3d_matrix_pop(1);
		}
	}

	/* Metal reflections over everything drawn above. */
	if (env_sprite)
	{
		rdpq_mode_begin();
			rdpq_mode_combiner(RDPQ_COMBINER_TEX);
			rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
			rdpq_mode_zbuf(true, false);
			rdpq_mode_filter(FILTER_BILINEAR);
		rdpq_mode_end();
		rdpq_change_other_modes_raw(SOM_ZMODE_MASK, SOM_ZMODE_DECAL);
		rdpq_sprite_upload(TILE0, env_sprite, NULL);
		t3d_state_set_vertex_fx(T3D_VERTEX_FX_SPHERICAL_UV, 32, 32);
		t3d_state_set_drawflags(T3D_FLAG_DEPTH | T3D_FLAG_TEXTURED);

		for (i = 0; i < queued; i++)
		{
			const struct hw_model *model = &models[queue[i].type];
			int b, t;

			t3d_matrix_push(&matrices[i]);

			if (model->mesh)
			{
				for (b = 0; b < model->mesh->num_batches; b++)
				{
					const struct ship_mesh_batch *batch = &model->mesh->batches[b];

					t3d_vert_load(model->mesh_env_verts[b], 0, (batch->num_verts + 1) & ~1);
					for (t = 0; t < batch->num_tris; t++)
						t3d_tri_draw(batch->tris[t * 3], batch->tris[t * 3 + 1], batch->tris[t * 3 + 2]);
					t3d_tri_sync();
				}
			}
			else
			{
				for (b = 0; b < model->num_batches; b++)
				{
					if (model->batch_verts[b] <= 0)
						continue;
					t3d_vert_load(&model->env_verts[model->batch_base[b] / 2], 0, model->batch_verts[b]);
					for (t = model->batch_tri_first[b]; t < model->batch_tri_first[b + 1]; t++)
						t3d_tri_draw(model->tris[t * 3], model->tris[t * 3 + 1], model->tris[t * 3 + 2]);
					t3d_tri_sync();
				}
			}

			t3d_matrix_pop(1);
		}

		t3d_state_set_vertex_fx(T3D_VERTEX_FX_NONE, 0, 0);
		rdpq_change_other_modes_raw(SOM_ZMODE_MASK, SOM_ZMODE_OPAQUE);
		rdpq_mode_begin();
			rdpq_mode_blender(0);
			rdpq_mode_zbuf(true, true);
		rdpq_mode_end();
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
