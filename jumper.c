// obby_full_game.c
// Full playable obby: SDL2 + SDL2_ttf single-file game.
// Features: FPS controls (WASD + mouse), wedges with rotation from JSON,
// translucent menu with TTF text, collisions, HUD, load world.
// Build: gcc -O2 -o obby_full_game obby_full_game.c `sdl2-config --cflags --libs` $(pkg-config --cflags --libs SDL2_ttf) -lm

#include <SDL.h>
#include <SDL_ttf.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#define WIN_W 1280
#define WIN_H 768

#define MAP_DEFAULT_SIZE 64
#define CELL_SIZE 1.0
#define PLAYER_RADIUS 0.28
#define PLAYER_HEIGHT 1.8

/* tile types */
enum { TILE_EMPTY = 0,
	   TILE_CUBE = 1,
	   TILE_WEDGE = 2,
	   TILE_END = 3 };

/* camera/player */
typedef struct {
	double x, y, z;
} Vec3;
typedef struct {
	double x, y, z;
	double yaw, pitch;
	double fov;
} Camera;
typedef struct {
	double px, py, pz;
	double vx, vy, vz;
	double yaw, pitch;
	int grounded;
	double time_since_grounded;
} Player;

/* input */
typedef struct {
	double move_fwd;
	double move_strafe;
	int jump;
	int sprint;
	int mouse_dx;
	int mouse_dy;
} Input;

/* map */
static int map_w = MAP_DEFAULT_SIZE;
static int map_h = MAP_DEFAULT_SIZE;
static uint8_t *map_cells = NULL;
static uint8_t *map_rots = NULL;

/* UI */
static int menu_open = 0;
static int menu_selected = 0;
static int menu_sub = 0; /* 0 none, 1 load, 2 settings, 3 credits */
static char load_path[512] = {0};
static int load_path_len = 0;
static char load_err[256] = {0};

/* settings */
static double mouse_sensitivity = 0.0028;
static int invert_mouse_y = 1;
static int invert_mouse_x = 0;

/* smoothing */
static double mouse_dx_smooth = 0.0, mouse_dy_smooth = 0.0;

/* physics */
static double GRAVITY = 20.0;
static double WALK_ACCEL = 100.0;
static double AIR_ACCEL = 60.0;
static double MAX_WALK_SPEED = 7.0;
static double JUMP_VELOCITY = 8.0;
static double FRICTION = 6.0;
static double BUNNY_HOP_TIME = 0.1;// Allow jumping for 0.1s after leaving ground

/* font */
static TTF_Font *gfont = NULL;

/* helpers */
static double clampd(double v, double a, double b) { return v < a ? a : (v > b ? b : v); }
static double lerp(double a, double b, double t) { return a + (b - a) * t; }
static double approach(double cur, double target, double maxDelta) {
	double d = target - cur;
	if (d > maxDelta) return cur + maxDelta;
	if (d < -maxDelta) return cur - maxDelta;
	return target;
}
static double now_seconds(void) { return SDL_GetPerformanceCounter() / (double) SDL_GetPerformanceFrequency(); }

/* ---------------- JSON-like loader (supports [type, rot] per cell) ---------------- */
static int load_map_json_like(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = (char *) malloc(sz + 1);
	if (!buf) {
		fclose(f);
		return -2;
	}
	fread(buf, 1, sz, f);
	buf[sz] = '\0';
	fclose(f);

	int w = 0, h = 0;
	char *p = buf;
	while (*p) {
		if (strncmp(p, "\"width\"", 7) == 0 || strncmp(p, "width", 5) == 0) {
			while (*p && (*p < '0' || *p > '9')) ++p;
			w = atoi(p);
		} else if (strncmp(p, "\"height\"", 8) == 0 || strncmp(p, "height", 6) == 0) {
			while (*p && (*p < '0' || *p > '9')) ++p;
			h = atoi(p);
		} else if (strncmp(p, "\"cells\"", 7) == 0 || strncmp(p, "cells", 5) == 0) {
			while (*p && *p != '[') ++p;
			if (!*p) break;
			++p;
			int row = 0, col = 0;
			int tmpw = 128, tmph = 128;
			uint8_t *tmp_types = (uint8_t *) calloc(tmpw * tmph, 1);
			uint8_t *tmp_rots = (uint8_t *) calloc(tmpw * tmph, 0);
			while (*p && row < 10000) {
				while (*p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',')) ++p;
				if (!*p) break;
				if (*p == '[') {
					++p;
					col = 0;
					while (*p && *p != ']') {
						while (*p && (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
						if (!*p || *p == ']') break;
						if (*p == '[') {
							++p;
							while (*p && (*p == ' ' || *p == ',')) ++p;
							int type = 0, rot = 0;
							if (*p == '-' || (*p >= '0' && *p <= '9')) {
								type = atoi(p);
								while (*p && ((*p >= '0' && *p <= '9') || *p == '-')) ++p;
							}
							while (*p && (*p == ' ' || *p == ',')) ++p;
							if (*p == '-' || (*p >= '0' && *p <= '9')) {
								rot = atoi(p);
								while (*p && ((*p >= '0' && *p <= '9') || *p == '-')) ++p;
							}
							while (*p && *p != ']') ++p;
							if (*p == ']') ++p;
							if (row < tmph && col < tmpw) {
								tmp_types[row * tmpw + col] = (uint8_t) type;
								tmp_rots[row * tmpw + col] = (uint8_t) (rot & 3);
							}
							++col;
						} else {
							int val = 0;
							int sign = 1;
							if (*p == '-') {
								sign = -1;
								++p;
							}
							if (*p >= '0' && *p <= '9') {
								val = atoi(p) * sign;
								while (*p && ((*p >= '0' && *p <= '9') || *p == '-')) ++p;
							} else
								++p;
							if (row < tmph && col < tmpw) {
								tmp_types[row * tmpw + col] = (uint8_t) val;
								tmp_rots[row * tmpw + col] = 0;
							}
							++col;
						}
					}
					++row;
					while (*p && *p != ']') ++p;
					if (*p == ']') ++p;
				} else if (*p == ']') {
					++p;
					break;
				} else
					++p;
			}
			if (w <= 0) w = tmpw;
			if (h <= 0) h = row;
			if (tmp_types) {
				map_w = w;
				map_h = h;
				if (map_cells) free(map_cells);
				if (map_rots) free(map_rots);
				map_cells = (uint8_t *) malloc(map_w * map_h);
				map_rots = (uint8_t *) malloc(map_w * map_h);
				for (int rz = 0; rz < map_h; ++rz)
					for (int rx = 0; rx < map_w; ++rx) {
						uint8_t v = 0, r = 0;
						if (rz < tmph && rx < tmpw) {
							v = tmp_types[rz * tmpw + rx];
							r = tmp_rots[rz * tmpw + rx];
						}
						map_cells[rz * map_w + rx] = v;
						map_rots[rz * map_w + rx] = r;
					}
				free(tmp_types);
				free(tmp_rots);
			}
			break;
		} else
			++p;
	}
	free(buf);
	if (!map_cells) return -3;
	return 0;
}

/* demo map */
static void generate_demo_map(void) {
	map_w = 32;
	map_h = 32;
	if (map_cells) free(map_cells);
	if (map_rots) free(map_rots);
	map_cells = (uint8_t *) malloc(map_w * map_h);
	map_rots = (uint8_t *) malloc(map_w * map_h);
	memset(map_cells, TILE_EMPTY, map_w * map_h);
	memset(map_rots, 0, map_w * map_h);
	for (int z = 0; z < map_h; ++z)
		for (int x = 0; x < map_w; ++x)
			if (z == 0 || x == 0 || z == map_h - 1 || x == map_w - 1) map_cells[z * map_w + x] = TILE_CUBE;
			else
				map_cells[z * map_w + x] = TILE_EMPTY;
	map_cells[6 * map_w + 6] = TILE_WEDGE;
	map_rots[6 * map_w + 6] = 0;
	map_cells[6 * map_w + 8] = TILE_WEDGE;
	map_rots[6 * map_w + 8] = 1;
	map_cells[8 * map_w + 6] = TILE_WEDGE;
	map_rots[8 * map_w + 6] = 2;
	map_cells[8 * map_w + 8] = TILE_WEDGE;
	map_rots[8 * map_w + 8] = 3;
	for (int x = 10; x < 18; ++x) map_cells[12 * map_w + x] = TILE_CUBE;
	map_cells[(map_h / 2) * map_w + (map_w / 2)] = TILE_END;
}

/* ---------------- projection and drawing ---------------- */
static int project_point(const Vec3 *p, const Camera *cam, int *sx, int *sy) {
	double rx = p->x - cam->x, ry = p->y - cam->y, rz = p->z - cam->z;
	double cosy = cos(-cam->yaw), siny = sin(-cam->yaw);
	double x1 = rx * cosy - rz * siny;
	double z1 = rx * siny + rz * cosy;
	double cosp = cos(-cam->pitch), sinp = sin(-cam->pitch);
	double y1 = ry * cosp - z1 * sinp;
	double z2 = ry * sinp + z1 * cosp;
	if (z2 <= 0.001) return 0;
	double aspect = (double) WIN_W / (double) WIN_H;
	double scale = 1.0 / tan(cam->fov * 0.5);
	double xndc = (x1 / z2) * scale * aspect;
	double yndc = (y1 / z2) * scale;
	*sx = (int) ((xndc * 0.5 + 0.5) * WIN_W);
	*sy = (int) ((-yndc * 0.5 + 0.5) * WIN_H);
	return 1;
}

static void draw_wire_cube(SDL_Renderer *ren, const Camera *cam, double cx, double cy, double cz, double s, SDL_Color col) {
	Vec3 corners[8];
	double hs = s * 0.5;
	corners[0] = (Vec3) {cx - hs, cy - hs, cz - hs};
	corners[1] = (Vec3) {cx + hs, cy - hs, cz - hs};
	corners[2] = (Vec3) {cx + hs, cy - hs, cz + hs};
	corners[3] = (Vec3) {cx - hs, cy - hs, cz + hs};
	corners[4] = (Vec3) {cx - hs, cy + hs, cz - hs};
	corners[5] = (Vec3) {cx + hs, cy + hs, cz - hs};
	corners[6] = (Vec3) {cx + hs, cy + hs, cz + hs};
	corners[7] = (Vec3) {cx - hs, cy + hs, cz + hs};
	int px[8], py[8], vis[8];
	for (int i = 0; i < 8; ++i) vis[i] = project_point(&corners[i], cam, &px[i], &py[i]);
	SDL_SetRenderDrawColor(ren, col.r, col.g, col.b, col.a);
	int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
	for (int e = 0; e < 12; ++e) {
		int a = edges[e][0], b = edges[e][1];
		if (vis[a] && vis[b]) SDL_RenderDrawLine(ren, px[a], py[a], px[b], py[b]);
	}
}

/* draw wedge with rotation */
static void draw_wedge(SDL_Renderer *ren, const Camera *cam, int tx, int tz, int rot, SDL_Color col) {
	double x0 = tx, x1 = tx + 1.0, z0 = tz, z1 = tz + 1.0;
	double h00, h10, h01, h11;
	if (rot == 0) {
		h00 = 0.0;
		h10 = 1.0;
		h01 = 0.0;
		h11 = 1.0;
	} /* slope along +x */
	else if (rot == 1) {
		h00 = 1.0;
		h10 = 0.0;
		h01 = 1.0;
		h11 = 0.0;
	} /* slope along -x */
	else if (rot == 2) {
		h00 = 0.0;
		h10 = 0.0;
		h01 = 1.0;
		h11 = 1.0;
	} /* slope along +z */
	else {
		h00 = 1.0;
		h10 = 1.0;
		h01 = 0.0;
		h11 = 0.0;
	} /* slope along -z */

	Vec3 corners[8];
	corners[0] = (Vec3) {x0, 0.0, z0};
	corners[1] = (Vec3) {x1, 0.0, z0};
	corners[2] = (Vec3) {x1, 0.0, z1};
	corners[3] = (Vec3) {x0, 0.0, z1};
	corners[4] = (Vec3) {x0, h00, z0};
	corners[5] = (Vec3) {x1, h10, z0};
	corners[6] = (Vec3) {x1, h11, z1};
	corners[7] = (Vec3) {x0, h01, z1};
	int px[8], py[8], vis[8];
	for (int i = 0; i < 8; ++i) vis[i] = project_point(&corners[i], cam, &px[i], &py[i]);
	SDL_SetRenderDrawColor(ren, col.r, col.g, col.b, col.a);
	int edges_bot[4][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}};
	for (int e = 0; e < 4; ++e) {
		int a = edges_bot[e][0], b = edges_bot[e][1];
		if (vis[a] && vis[b]) SDL_RenderDrawLine(ren, px[a], py[a], px[b], py[b]);
	}
	int edges_top[4][2] = {{4, 5}, {5, 6}, {6, 7}, {7, 4}};
	for (int e = 0; e < 4; ++e) {
		int a = edges_top[e][0], b = edges_top[e][1];
		if (vis[a] && vis[b]) SDL_RenderDrawLine(ren, px[a], py[a], px[b], py[b]);
	}
	for (int i = 0; i < 4; ++i) {
		if (vis[i] && vis[i + 4]) SDL_RenderDrawLine(ren, px[i], py[i], px[i + 4], py[i + 4]);
	}
	if (vis[4] && vis[6]) SDL_RenderDrawLine(ren, px[4], py[4], px[6], py[6]);
}

/* draw map */
static void draw_map(SDL_Renderer *ren, const Camera *cam) {
	for (int z = 0; z < map_h; ++z)
		for (int x = 0; x < map_w; ++x) {
			uint8_t t = map_cells[z * map_w + x];
			if (t == TILE_EMPTY) continue;
			uint8_t r = map_rots[z * map_w + x];
			if (t == TILE_CUBE) draw_wire_cube(ren, cam, x + 0.5, 0.5, z + 0.5, 1.0, (SDL_Color) {0, 200, 0, 255});
			else if (t == TILE_WEDGE)
				draw_wedge(ren, cam, x, z, r, (SDL_Color) {220, 160, 40, 255});
			else if (t == TILE_END)
				draw_wire_cube(ren, cam, x + 0.5, 0.5, z + 0.5, 1.0, (SDL_Color) {200, 0, 0, 255});
		}
}

/* ---------------- text drawing ---------------- */
static void draw_text(SDL_Renderer *ren, const char *s, int x, int y, SDL_Color col) {
	if (!gfont || !s) return;
	SDL_Surface *surf = TTF_RenderUTF8_Blended(gfont, s, (SDL_Color) {col.r, col.g, col.b, col.a});
	if (!surf) return;
	SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
	SDL_Rect dst = {x, y, surf->w, surf->h};
	SDL_FreeSurface(surf);
	if (tex) {
		SDL_RenderCopy(ren, tex, NULL, &dst);
		SDL_DestroyTexture(tex);
	}
}

/* ---------------- UI drawing ---------------- */
static void draw_main_menu(SDL_Renderer *ren) {
	int cx = WIN_W / 2 - 220, cy = WIN_H / 2 - 180;
	SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
	SDL_Rect panel = {cx - 24, cy - 24, 480, 360};
	SDL_SetRenderDrawColor(ren, 0, 0, 0, 180);
	SDL_RenderFillRect(ren, &panel);
	SDL_SetRenderDrawColor(ren, 0, 200, 0, 220);
	SDL_RenderDrawRect(ren, &panel);
	const char *items[] = {"Resume", "Load World", "Settings", "Credits", "Quit"};
	int nitems = 5;
	for (int i = 0; i < nitems; ++i) {
		SDL_Rect r = {cx, cy + i * 64, 420, 48};
		SDL_SetRenderDrawColor(ren, 0, 0, 0, 120);
		SDL_RenderFillRect(ren, &r);
		if (i == menu_selected) SDL_SetRenderDrawColor(ren, 0, 255, 0, 255);
		else
			SDL_SetRenderDrawColor(ren, 0, 160, 0, 200);
		SDL_RenderDrawRect(ren, &r);
		if (gfont) {
			SDL_Color shadow = {0, 0, 0, 200};
			SDL_Color text = {180, 255, 180, 255};
			int tx = r.x + 18, ty = r.y + 10;
			draw_text(ren, items[i], tx + 2, ty + 2, shadow);
			draw_text(ren, items[i], tx, ty, text);
		} else {
			SDL_Rect tick = {r.x + 8, r.y + 10, 28, 28};
			SDL_SetRenderDrawColor(ren, i == menu_selected ? 0 : 0, i == menu_selected ? 255 : 140, 0, 255);
			SDL_RenderFillRect(ren, &tick);
		}
	}
}

static void draw_load_overlay(SDL_Renderer *ren) {
	int cx = WIN_W / 2 - 320, cy = WIN_H / 2 - 80;
	SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
	SDL_Rect outer = {cx - 12, cy - 12, 664, 164};
	SDL_SetRenderDrawColor(ren, 0, 0, 0, 200);
	SDL_RenderFillRect(ren, &outer);
	SDL_SetRenderDrawColor(ren, 0, 200, 0, 255);
	SDL_RenderDrawRect(ren, &outer);
	SDL_Rect box = {cx, cy, 640, 40};
	SDL_SetRenderDrawColor(ren, 20, 20, 20, 220);
	SDL_RenderFillRect(ren, &box);
	SDL_SetRenderDrawColor(ren, 0, 200, 0, 255);
	SDL_RenderDrawRect(ren, &box);
	if (gfont) {
		draw_text(ren, "Type path and press Enter to load (Esc to cancel):", cx, cy - 28, (SDL_Color) {0, 200, 0, 255});
		draw_text(ren, load_path, cx + 8, cy + 8, (SDL_Color) {0, 255, 0, 255});
		if (load_err[0]) draw_text(ren, load_err, cx, cy + 56, (SDL_Color) {255, 80, 80, 255});
	}
}

static void draw_settings_overlay(SDL_Renderer *ren) {
	int cx = WIN_W / 2 - 260, cy = WIN_H / 2 - 140;
	SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
	SDL_Rect outer = {cx - 12, cy - 12, 524, 284};
	SDL_SetRenderDrawColor(ren, 0, 0, 0, 200);
	SDL_RenderFillRect(ren, &outer);
	SDL_SetRenderDrawColor(ren, 0, 200, 0, 255);
	SDL_RenderDrawRect(ren, &outer);
	if (gfont) {
		draw_text(ren, "Settings:", cx + 12, cy + 8, (SDL_Color) {0, 200, 0, 255});
		char buf[128];
		snprintf(buf, sizeof(buf), "Mouse Sensitivity: %.4f (Left/Right)", mouse_sensitivity);
		draw_text(ren, buf, cx + 12, cy + 48, (SDL_Color) {0, 200, 0, 255});
		snprintf(buf, sizeof(buf), "Invert Mouse Y: %s (press I)", invert_mouse_y ? "On" : "Off");
		draw_text(ren, buf, cx + 12, cy + 80, (SDL_Color) {0, 200, 0, 255});
		snprintf(buf, sizeof(buf), "Invert Mouse X: %s (press X)", invert_mouse_x ? "On" : "Off");
		draw_text(ren, buf, cx + 12, cy + 112, (SDL_Color) {0, 200, 0, 255});
	}
}

static void draw_credits_overlay(SDL_Renderer *ren) {
	int cx = WIN_W / 2 - 200, cy = WIN_H / 2 - 100;
	SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
	SDL_Rect outer = {cx - 12, cy - 12, 424, 224};
	SDL_SetRenderDrawColor(ren, 0, 0, 0, 200);
	SDL_RenderFillRect(ren, &outer);
	SDL_SetRenderDrawColor(ren, 0, 200, 0, 255);
	SDL_RenderDrawRect(ren, &outer);
	if (gfont) {
		draw_text(ren, "Credits:", cx + 12, cy + 8, (SDL_Color) {0, 200, 0, 255});
		draw_text(ren, "M2/19 Zac, James, Poom", cx + 12, cy + 48, (SDL_Color) {0, 200, 0, 255});
		draw_text(ren, "Copilot (for debugging)", cx + 12, cy + 80, (SDL_Color) {0, 200, 0, 255});
	}
}

/* ---------------- collisions ---------------- */
static inline int in_map(int x, int z) { return x >= 0 && z >= 0 && x < map_w && z < map_h; }
static inline uint8_t tile_at(int x, int z) {
	if (!in_map(x, z)) return TILE_CUBE;
	return map_cells[z * map_w + x];
}

static void resolve_cube(Player *p, int cx, int cz) {
	double cell_min_x = cx * CELL_SIZE, cell_max_x = (cx + 1) * CELL_SIZE;
	double cell_min_y = 0.0, cell_max_y = 1.0;
	double cell_min_z = cz * CELL_SIZE, cell_max_z = (cz + 1) * CELL_SIZE;
	double pmin_x = p->px - PLAYER_RADIUS, pmax_x = p->px + PLAYER_RADIUS;
	double pmin_y = p->py, pmax_y = p->py + PLAYER_HEIGHT;
	double pmin_z = p->pz - PLAYER_RADIUS, pmax_z = p->pz + PLAYER_RADIUS;
	if (pmax_x <= cell_min_x || pmin_x >= cell_max_x || pmax_y <= cell_min_y || pmin_y >= cell_max_y || pmax_z <= cell_min_z || pmin_z >= cell_max_z) return;
	double pen_x = fmin(pmax_x - cell_min_x, cell_max_x - pmin_x);
	double pen_y = fmin(pmax_y - cell_min_y, cell_max_y - pmin_y);
	double pen_z = fmin(pmax_z - cell_min_z, cell_max_z - pmin_z);
	if (pen_y <= pen_x && pen_y <= pen_z) {
		double cell_center_y = (cell_min_y + cell_max_y) * 0.5;
		double player_center_y = (pmin_y + pmax_y) * 0.5;
		if (player_center_y > cell_center_y) {
			p->py = cell_max_y + 0.001;
			p->vy = 0.0;
			p->grounded = 1;
		} else {
			p->py = cell_min_y - PLAYER_HEIGHT - 0.001;
			if (p->vy > 0) p->vy = 0.0;
		}
	} else if (pen_x <= pen_z) {
		if (p->px < (cell_min_x + cell_max_x) * 0.5) p->px -= pen_x;
		else
			p->px += pen_x;
		p->vx *= 0.3;// Preserve some momentum instead of killing it
	} else {
		if (p->pz < (cell_min_z + cell_max_z) * 0.5) p->pz -= pen_z;
		else
			p->pz += pen_z;
		p->vz *= 0.3;// Preserve some momentum instead of killing it
	}
}

static double wedge_height_at_local(double lx, double lz, int rot) {
	lx = clampd(lx, 0.0, 1.0);
	lz = clampd(lz, 0.0, 1.0);
	if (rot == 0) return lx;
	if (rot == 1) return 1.0 - lx;
	if (rot == 2) return lz;
	return 1.0 - lz;
}

static void resolve_wedge(Player *p, int cx, int cz, int rot) {
	double lx = p->px - cx;
	double lz = p->pz - cz;
	if (lx < -PLAYER_RADIUS || lx > 1.0 + PLAYER_RADIUS || lz < -PLAYER_RADIUS || lz > 1.0 + PLAYER_RADIUS) return;
	double surf = wedge_height_at_local(lx, lz, rot);
	if (p->py <= surf + 0.001) {
		p->py = surf + 0.001;
		if (p->vy < 0.0) p->vy = 0.0;
		p->grounded = 1;
	}
}

static void resolve_collisions(Player *p, int *level_complete) {
	int cx = (int) floor(p->px);
	int cz = (int) floor(p->pz);
	for (int oz = -1; oz <= 1; ++oz)
		for (int ox = -1; ox <= 1; ++ox) {
			int mx = cx + ox, mz = cz + oz;
			if (!in_map(mx, mz)) continue;
			uint8_t t = tile_at(mx, mz);
			if (t == TILE_CUBE) resolve_cube(p, mx, mz);
			else if (t == TILE_WEDGE)
				resolve_wedge(p, mx, mz, map_rots[mz * map_w + mx]);
			else if (t == TILE_END) {
				double minx = mx, maxx = mx + 1, minz = mz, maxz = mz + 1;
				if (p->px + PLAYER_RADIUS >= minx && p->px - PLAYER_RADIUS <= maxx && p->pz + PLAYER_RADIUS >= minz && p->pz - PLAYER_RADIUS <= maxz) *level_complete = 1;
			}
		}
	if (p->py < 0.0) {
		p->py = 0.0;
		p->vy = 0.0;
		p->grounded = 1;
	}
}

/* ---------------- physics step (camera-relative movement) ---------------- */
static void physics_step(Player *p, const Input *in, double dt, int *level_complete) {
	/* movement uses camera yaw directly (p->yaw is camera yaw) */
	double yaw_for_move = p->yaw;
	double forward_x = sin(yaw_for_move), forward_z = cos(yaw_for_move);
	double right_x = forward_z, right_z = -forward_x;

	double raw_fwd = in->move_fwd;
	double raw_str = in->move_strafe;
	double in_len = sqrt(raw_fwd * raw_fwd + raw_str * raw_str);
	if (in_len > 1.0) {
		raw_fwd /= in_len;
		raw_str /= in_len;
	}

	double wish_x = forward_x * raw_fwd + right_x * raw_str;
	double wish_z = forward_z * raw_fwd + right_z * raw_str;
	double wish_len = sqrt(wish_x * wish_x + wish_z * wish_z);
	if (wish_len > 1e-6) {
		wish_x /= wish_len;
		wish_z /= wish_len;
	}

	double accel = p->grounded ? WALK_ACCEL : AIR_ACCEL;
	double target_speed = p->grounded ? MAX_WALK_SPEED * (in->sprint ? 1.5 : 1.0) : MAX_WALK_SPEED;
	double target_vx = wish_x * target_speed;
	double target_vz = wish_z * target_speed;
	double maxdv = accel * dt;
	p->vx = approach(p->vx, target_vx, maxdv);
	p->vz = approach(p->vz, target_vz, maxdv);

	if (p->grounded && wish_len < 1e-3) {
		p->vx = approach(p->vx, 0.0, FRICTION * dt);
		p->vz = approach(p->vz, 0.0, FRICTION * dt);
	}

	// Update time since grounded
	if (p->grounded) {
		p->time_since_grounded = 0.0;
	} else {
		p->time_since_grounded += dt;
	}

	p->vy -= GRAVITY * dt;
	if ((p->grounded || p->time_since_grounded < BUNNY_HOP_TIME) && in->jump) {
		p->vy = JUMP_VELOCITY;
		p->grounded = 0;
		p->time_since_grounded = BUNNY_HOP_TIME;// Prevent immediate re-jump
	}

	p->px += p->vx * dt;
	p->py += p->vy * dt;
	p->pz += p->vz * dt;

	resolve_collisions(p, level_complete);
}

/* ---------------- main ---------------- */
int main(int argc, char **argv) {
	const char *mapfile = NULL;
	if (argc >= 2) mapfile = argv[1];

	if (mapfile) {
		if (load_map_json_like(mapfile) != 0) {
			fprintf(stderr, "Failed to load %s, generating demo map\n", mapfile);
			generate_demo_map();
		}
	} else
		generate_demo_map();

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}
	if (TTF_Init() != 0) { fprintf(stderr, "TTF_Init failed: %s\n", TTF_GetError()); /* continue without text */ }

	SDL_Window *win = SDL_CreateWindow("Obby Full Game", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H, SDL_WINDOW_SHOWN);
	if (!win) {
		fprintf(stderr, "CreateWindow failed: %s\n", SDL_GetError());
		TTF_Quit();
		SDL_Quit();
		return 1;
	}
	SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!ren) {
		fprintf(stderr, "CreateRenderer failed: %s\n", SDL_GetError());
		SDL_DestroyWindow(win);
		TTF_Quit();
		SDL_Quit();
		return 1;
	}

	/* try common fonts */
	const char *font_paths[] = {"assets/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", NULL};
	gfont = NULL;
	for (int i = 0; font_paths[i]; ++i) {
		if (access(font_paths[i], R_OK) == 0) {
			gfont = TTF_OpenFont(font_paths[i], 16);
			if (gfont) {
				fprintf(stderr, "Loaded font: %s\n", font_paths[i]);
				break;
			} else
				fprintf(stderr, "TTF_OpenFont failed for %s: %s\n", font_paths[i], TTF_GetError());
		}
	}
	if (!gfont) fprintf(stderr, "Warning: TTF font not found; text will be limited.\n");

	SDL_SetRelativeMouseMode(SDL_TRUE);
	SDL_StartTextInput();

	Player state_prev, state_curr;
	memset(&state_prev, 0, sizeof(state_prev));
	memset(&state_curr, 0, sizeof(state_curr));
	state_curr.px = 3.5;
	state_curr.pz = 3.5;
	state_curr.py = 2.0;
	state_curr.yaw = 0.0;
	state_curr.pitch = 0.0;
	state_prev = state_curr;

	Camera cam;
	cam.x = state_curr.px;
	cam.y = state_curr.py + 0.6;
	cam.z = state_curr.pz;
	cam.yaw = state_curr.yaw;
	cam.pitch = state_curr.pitch;
	cam.fov = 60.0 * M_PI / 180.0;

	Input in = {0};
	int running = 1;
	int level_complete = 0;
	double PHYS_DT = 1.0 / 120.0;
	double accumulator = 0.0;
	double prev_time = now_seconds();

	int debug_frame = 0;

	while (running) {
		double cur = now_seconds();
		double frame_dt = clampd(cur - prev_time, 0.0, 0.25);
		prev_time = cur;
		accumulator += frame_dt;

		in.move_fwd = 0;
		in.move_strafe = 0;
		in.jump = 0;
		in.sprint = 0;
		in.mouse_dx = 0;
		in.mouse_dy = 0;
		SDL_Event ev;
		const Uint8 *kb = SDL_GetKeyboardState(NULL);
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_QUIT) running = 0;
			if (ev.type == SDL_KEYDOWN) {
				if (!menu_open && ev.key.keysym.sym == SDLK_ESCAPE) {
					menu_open = 1;
					menu_selected = 0;
					menu_sub = 0;
					SDL_SetRelativeMouseMode(SDL_FALSE);
				} else if (menu_open && ev.key.keysym.sym == SDLK_ESCAPE) {
					if (menu_sub != 0) {
						menu_sub = 0;
						load_path_len = 0;
						load_path[0] = '\0';
						load_err[0] = '\0';
						SDL_StopTextInput();
						SDL_SetRelativeMouseMode(SDL_FALSE);
					} else {
						menu_open = 0;
						menu_sub = 0;
						SDL_SetRelativeMouseMode(SDL_TRUE);
					}
				} else if (menu_open && ev.key.keysym.sym == SDLK_UP) {
					menu_selected = (menu_selected + 4) % 5;
				} else if (menu_open && ev.key.keysym.sym == SDLK_DOWN) {
					menu_selected = (menu_selected + 1) % 5;
				} else if (menu_open && ev.key.keysym.sym == SDLK_RETURN) {
					if (menu_sub == 1) {
						/* handled in text input Enter below */
					} else {
						if (menu_selected == 0) {
							menu_open = 0;
							menu_sub = 0;
							SDL_SetRelativeMouseMode(SDL_TRUE);
						} else if (menu_selected == 1) {
							menu_sub = 1;
							load_path_len = 0;
							load_path[0] = '\0';
							load_err[0] = '\0';
							SDL_StartTextInput();
							SDL_SetRelativeMouseMode(SDL_FALSE);
						} else if (menu_selected == 2) {
							menu_sub = 2;
							SDL_SetRelativeMouseMode(SDL_FALSE);
						} else if (menu_selected == 3) {
							menu_sub = 3;
							SDL_SetRelativeMouseMode(SDL_FALSE);
						} else if (menu_selected == 4) {
							running = 0;
						}
					}
				} else if (menu_open && menu_sub == 2) {
					if (ev.key.keysym.sym == SDLK_LEFT) mouse_sensitivity = clampd(mouse_sensitivity - 0.0005, 0.0005, 0.01);
					if (ev.key.keysym.sym == SDLK_RIGHT) mouse_sensitivity = clampd(mouse_sensitivity + 0.0005, 0.0005, 0.01);
					if (ev.key.keysym.sym == SDLK_i) invert_mouse_y = !invert_mouse_y;
					if (ev.key.keysym.sym == SDLK_x) invert_mouse_x = !invert_mouse_x;
					if (ev.key.keysym.sym == SDLK_ESCAPE) {
						menu_sub = 0;
						SDL_SetRelativeMouseMode(SDL_FALSE);
					}
				}
			}
			if (ev.type == SDL_TEXTINPUT && menu_sub == 1) {
				int add = (int) strlen(ev.text.text);
				if (load_path_len + add < (int) sizeof(load_path) - 1) {
					strcat(load_path, ev.text.text);
					load_path_len += add;
				}
			}
			if (ev.type == SDL_MOUSEMOTION) {
				if (!menu_open) {
					in.mouse_dx += ev.motion.xrel;
					in.mouse_dy += ev.motion.yrel;
				}
			}
			if (ev.type == SDL_KEYDOWN && menu_sub == 1) {
				if (ev.key.keysym.sym == SDLK_BACKSPACE) {
					if (load_path_len > 0) {
						load_path_len--;
						load_path[load_path_len] = '\0';
					}
				} else if (ev.key.keysym.sym == SDLK_RETURN) {
					load_err[0] = '\0';
					if (load_path_len > 0) {
						uint8_t *old_cells = map_cells;
						uint8_t *old_rots = map_rots;
						map_cells = NULL;
						map_rots = NULL;
						int res = load_map_json_like(load_path);
						if (res == 0) {
							state_curr.px = 3.5;
							state_curr.pz = 3.5;
							state_curr.py = 2.0;
							state_curr.vx = state_curr.vy = state_curr.vz = 0.0;
							state_curr.grounded = 0;
							state_curr.time_since_grounded = 0.0;
							menu_sub = 0;
							menu_open = 0;
							SDL_StopTextInput();
							SDL_SetRelativeMouseMode(SDL_TRUE);
							if (old_cells) free(old_cells);
							if (old_rots) free(old_rots);
						} else {
							if (map_cells) {
								free(map_cells);
								map_cells = NULL;
							}
							if (map_rots) {
								free(map_rots);
								map_rots = NULL;
							}
							map_cells = old_cells;
							map_rots = old_rots;
							snprintf(load_err, sizeof(load_err), "Failed to load (code %d)", res);
						}
					} else
						snprintf(load_err, sizeof(load_err), "Enter a path first");
				} else if (ev.key.keysym.sym == SDLK_ESCAPE) {
					menu_sub = 0;
					load_path_len = 0;
					load_path[0] = '\0';
					load_err[0] = '\0';
					SDL_StopTextInput();
					SDL_SetRelativeMouseMode(SDL_FALSE);
				}
			}
		} /* events */

		/* continuous keys */
		if (!menu_open) {
			in.move_fwd = (kb[SDL_SCANCODE_W] ? 1.0 : 0.0) - (kb[SDL_SCANCODE_S] ? 1.0 : 0.0);
			in.move_strafe = (kb[SDL_SCANCODE_D] ? 1.0 : 0.0) - (kb[SDL_SCANCODE_A] ? 1.0 : 0.0);
			in.jump = kb[SDL_SCANCODE_SPACE];
			in.sprint = kb[SDL_SCANCODE_LSHIFT] || kb[SDL_SCANCODE_RSHIFT];
		} else {
			in.move_fwd = in.move_strafe = in.jump = in.sprint = 0;
		}

		/* mouse smoothing and apply to camera yaw/pitch */
		const double MOUSE_SMOOTH = 0.6;
		mouse_dx_smooth = lerp(mouse_dx_smooth, (double) in.mouse_dx, 1.0 - MOUSE_SMOOTH);
		mouse_dy_smooth = lerp(mouse_dy_smooth, (double) in.mouse_dy, 1.0 - MOUSE_SMOOTH);

		if (!menu_open) {
			double xsign = invert_mouse_x ? -1.0 : 1.0;
			state_curr.yaw += xsign * mouse_dx_smooth * mouse_sensitivity;
			double ysign = invert_mouse_y ? 1.0 : -1.0;
			state_curr.pitch += ysign * mouse_dy_smooth * mouse_sensitivity;
			state_curr.pitch = clampd(state_curr.pitch, -1.45, 1.45);
		}

		/* physics stepping */
		state_prev = state_curr;
		int substeps = 2;
		while (accumulator >= PHYS_DT) {
			for (int s = 0; s < substeps; ++s) physics_step(&state_curr, &in, PHYS_DT / substeps, &level_complete);
			accumulator -= PHYS_DT;
		}
		double alpha = accumulator / PHYS_DT;
		Player render_player;
		render_player.px = state_prev.px + (state_curr.px - state_prev.px) * alpha;
		render_player.py = state_prev.py + (state_curr.py - state_prev.py) * alpha;
		render_player.pz = state_prev.pz + (state_curr.pz - state_prev.pz) * alpha;
		render_player.vx = state_prev.vx + (state_curr.vx - state_prev.vx) * alpha;
		render_player.vy = state_prev.vy + (state_curr.vy - state_prev.vy) * alpha;
		render_player.vz = state_prev.vz + (state_curr.vz - state_prev.vz) * alpha;
		render_player.yaw = state_prev.yaw + (state_curr.yaw - state_prev.yaw) * alpha;
		render_player.pitch = state_prev.pitch + (state_curr.pitch - state_prev.pitch) * alpha;
		render_player.grounded = state_curr.grounded;

		/* camera follow */
		cam.x = lerp(cam.x, render_player.px, 0.12);
		cam.y = lerp(cam.y, render_player.py + 0.6, 0.12);
		cam.z = lerp(cam.z, render_player.pz, 0.12);
		cam.yaw = lerp(cam.yaw, render_player.yaw, 0.18);
		cam.pitch = lerp(cam.pitch, render_player.pitch, 0.18);

		/* render */
		SDL_SetRenderDrawColor(ren, 12, 12, 20, 255);
		SDL_RenderClear(ren);

		draw_map(ren, &cam);

		/* crosshair */
		SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
		SDL_RenderDrawLine(ren, WIN_W / 2 - 8, WIN_H / 2, WIN_W / 2 + 8, WIN_H / 2);
		SDL_RenderDrawLine(ren, WIN_W / 2, WIN_H / 2 - 8, WIN_W / 2, WIN_H / 2 + 8);

		/* HUD */
		if (gfont) {
			char hud[256];
			snprintf(hud, sizeof(hud), "Pos: %.2f %.2f %.2f  Vel: %.2f %.2f %.2f", render_player.px, render_player.py, render_player.pz, render_player.vx, render_player.vy, render_player.vz);
			draw_text(ren, hud, 10, 10, (SDL_Color) {0, 200, 0, 255});
			char s2[128];
			snprintf(s2, sizeof(s2), "Sens: %.4f  InvY:%s InvX:%s", mouse_sensitivity, invert_mouse_y ? "On" : "Off", invert_mouse_x ? "On" : "Off");
			draw_text(ren, s2, 10, 30, (SDL_Color) {0, 180, 0, 255});
		} else {
			/* fallback small HUD blocks */
			SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
			SDL_SetRenderDrawColor(ren, 0, 0, 0, 160);
			SDL_Rect bg = {8, 8, 220, 36};
			SDL_RenderFillRect(ren, &bg);
			SDL_SetRenderDrawColor(ren, 0, 200, 0, 255);
			for (int i = 0; i < 20; ++i) {
				SDL_Rect b = {12 + i * 10, 14, 6, 20};
				SDL_RenderFillRect(ren, &b);
			}
		}

		if (menu_open) {
			draw_main_menu(ren);
			if (menu_sub == 1) draw_load_overlay(ren);
			else if (menu_sub == 2)
				draw_settings_overlay(ren);
			else if (menu_sub == 3)
				draw_credits_overlay(ren);
		}

		if (level_complete) {
			SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
			SDL_SetRenderDrawColor(ren, 0, 0, 0, 160);
			SDL_Rect full = {0, 0, WIN_W, WIN_H};
			SDL_RenderFillRect(ren, &full);
			SDL_SetRenderDrawColor(ren, 0, 200, 0, 255);
			SDL_Rect box = {WIN_W / 2 - 200, WIN_H / 2 - 40, 400, 80};
			SDL_RenderDrawRect(ren, &box);
			if (gfont) draw_text(ren, "Level Complete! Press R to restart.", WIN_W / 2 - 160, WIN_H / 2 - 8, (SDL_Color) {0, 200, 0, 255});
			if (kb[SDL_SCANCODE_R]) {
				level_complete = 0;
				state_curr.px = 3.5;
				state_curr.pz = 3.5;
				state_curr.py = 2.0;
				state_curr.vx = state_curr.vy = state_curr.vz = 0.0;
				state_curr.grounded = 0;
			}
		}

		SDL_RenderPresent(ren);

		/* debug print occasionally */
		if (++debug_frame % 240 == 0) {
			double fy = state_curr.yaw;
			double fx = sin(fy), fz = cos(fy);
			double rx = fz, rz = -fx;
			printf("DBG: in fwd=%.2f str=%.2f forward=(%.3f,%.3f) right=(%.3f,%.3f) yaw=%.3f\n",
				   in.move_fwd, in.move_strafe, fx, fz, rx, rz, state_curr.yaw);
		}

		SDL_Delay(1);
	}

	if (map_cells) free(map_cells);
	if (map_rots) free(map_rots);
	if (gfont) TTF_CloseFont(gfont);
	TTF_Quit();
	SDL_StopTextInput();
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
