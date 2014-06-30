/*=======================================================================
 * pwlibs - Libraries used by the PiWall video wall
 * Copyright (C) 2013-2014  Colin Hogben <colin@piwall.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *-----------------------------------------------------------------------
 *	Combine wall and tile geometry to map picture to screen
 *=======================================================================*/
#include "pwtilemap.h"
#include "pwutil.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/utsname.h>

#ifdef __GNUC__
#  define UNUSED(x) UNUSED_ ## x __attribute__((__unused__))
#else
#  define UNUSED(x) UNUSED_ ## x
#endif

#define ROUND(v) ((v) < 0 ? (int)((v) - 0.5) : (int)((v) + 0.5))

/* Convert to integer and clip to limits */
#define ICLIP(l,h,v) CLAMP(ROUND(v),l,h)

typedef struct {
  gdouble factor;
  gdouble offset;
} Scale;

typedef enum {
  USER_TILECODE = 0x01,		/* Use predefined config */
  USER_AUTO   = 0x02,		/* Get from .pitile */
  USER_ROLE   = 0x04,		/* Get from named role in .piwall */
  USER_CONFIG = 0x08,		/* Get from config in .piwall and id */
  USER_WALL   = 0x10,		/* Explicit wall geometry */
  USER_TILE   = 0x20,		/* Explicit tile geometry */
  USER_ORIENT = 0x40,		/* Explicit orientation */
  USER_FIT    = 0x80,		/* Explicit fit */

  SCREEN_WALL = 0x1000,		/* Wall is derived from screen */
  WALL_TILE   = 0x2000,		/* Tile is same as wall */
  WALL_WINDOW = 0x4000		/* Window is percentage of wall */
} PwTileMapFlags;

struct _PwTileMap {
  gint nrefs;
  PwTileMapFlags flags;
  /* user-defined */
  struct {
    guint tilecode;
    gfloat framex, framey;
    gchar *role;
    gchar *config;
    PwRect wall, tile;
    PwOrient orient;
    PwFit fit;
    PwRect window;
  } user;
  PwIntRect screen;
  PwDefs *defs;
  /* Values used */
  PwRect wall, tile;
  PwOrient orient;
  PwFit fit;
  PwRect window;
};

#define PWTILEMAP_ERROR pwtilemap_error_quark()
#define ERROR(_n,...) g_set_error(error, PWTILEMAP_ERROR,_n, __VA_ARGS__)

/* Forward declarations */
static char *
_pwtilemap_tile_id(PwTileMap *self, GError **error);
static char *
_pwtilemap_pitile_id(PwTileMap *self, GError **error);
static gboolean
_pwtilemap_from_role(PwTileMap *self, const gchar *role, GKeyFile *piwall,
		     GError **error);
static void
_pwtilemap_screen_wall(PwTileMap *);
static void
_pwtilemap_wall_window(PwTileMap *);
static gboolean
_pwtilemap_get_rect(PwTileMap *self, const gchar *section, PwRect *rect,
		    GError **error);

static void scale_init(Scale *self, gdouble factor, gdouble offset);
static void scale_from_factor_point(Scale *self, gdouble factor, gdouble old, gdouble new);
static void scale_from_points(Scale *self, gdouble old0, gdouble old1, gdouble new0, gdouble new1);
static gdouble scale(const Scale *self, gdouble value);
static gdouble unscale(const Scale *self, gdouble value);


static GQuark
pwtilemap_error_quark(void)
{
  return g_quark_from_static_string("pwtilemap-error");
}

/*-----------------------------------------------------------------------
 *	Create object with defaults
 *-----------------------------------------------------------------------*/
PwTileMap *
pwtilemap_create(void)
{
  PwTileMap *self = g_new0(PwTileMap, 1);

  self->nrefs = 1;
  self->flags = (SCREEN_WALL | WALL_TILE | WALL_WINDOW);
  self->user.framex = 1.0;
  self->user.framey = 1.0;
  self->user.role = NULL;
  self->user.config = NULL;
  self->user.orient = PW_ORIENT_UP;
  self->user.fit = PW_FIT_STRETCH;
  PWRECT_SET(self->user.window, 0, 0, 100, 100);
  /* Assume HD screen until told otherwise */
  PWRECT_SET0(self->screen, 1920, 1080);

  return self;
}

/*-----------------------------------------------------------------------
 *	Set definitions to be looked up
 *-----------------------------------------------------------------------*/
void
pwtilemap_set_defs(PwTileMap *self, PwDefs *defs)
{
  if (self->defs) pwdefs_unref(self->defs);
  self->defs = defs;
  pwdefs_ref(defs);
}

/*-----------------------------------------------------------------------
 *	Set canned config
 *-----------------------------------------------------------------------*/
void
pwtilemap_set_tilecode(PwTileMap *self, guint code)
{
  self->flags |= USER_TILECODE;
  self->user.tilecode = code;
}

void
pwtilemap_set_framesize(PwTileMap *self, gdouble framex, gdouble framey)
{
  self->user.framex = CLAMP(framex, 1.0, 1.2);
  self->user.framey = CLAMP(framey, 1.0, 1.2);
}

/*-----------------------------------------------------------------------
 *	Set various attributes
 *-----------------------------------------------------------------------*/
void
pwtilemap_set_auto(PwTileMap *self)
{
  self->flags |= USER_AUTO;
}

void
pwtilemap_set_role(PwTileMap *self, const gchar *role)
{
  g_free(self->user.role);
  if (role != NULL) {
    self->user.role = g_strdup(role);
    self->flags |= USER_ROLE;
  } else {
    self->flags &= ~ USER_ROLE;
  }
}

void
pwtilemap_set_config(PwTileMap *self, const gchar *config)
{
  g_free(self->user.config);
  if (config != NULL) {
    self->user.config = g_strdup(config);
    self->flags |= USER_CONFIG;
  } else {
    self->flags &= ~ USER_CONFIG;
  }
}

void
pwtilemap_set_wall(PwTileMap *self, const PwRect *wall)
{
  self->flags |= USER_WALL;
  self->flags &= ~ SCREEN_WALL;
  self->user.wall = *wall;
  /* Dependencies */
  self->wall = self->user.wall;
  if (self->flags & WALL_TILE) {
    self->tile = self->wall;
  }
  if (self->flags & WALL_WINDOW) {
    _pwtilemap_wall_window(self);
  }
}

void
pwtilemap_set_tile(PwTileMap *self, const PwRect *tile)
{
  self->flags |= USER_TILE;
  self->user.tile = *tile;
  self->tile = self->user.tile;
}

void
pwtilemap_set_orient(PwTileMap *self, PwOrient orient)
{
  self->flags |= USER_ORIENT;
  self->user.orient = orient;
  self->orient = self->user.orient;
}

void
pwtilemap_set_fit(PwTileMap *self, PwFit fit)
{
  self->flags |= USER_FIT;
  self->user.fit = fit;
  self->fit = self->user.fit;
}

void
pwtilemap_set_window(PwTileMap *self, const PwRect *window, gboolean percent)
{
  self->user.window = *window;
  if (percent) {
    self->flags |= WALL_WINDOW;
    _pwtilemap_wall_window(self);
  } else {
    self->flags &= ~ WALL_WINDOW;
    self->window = self->user.window;
  }
}

void
pwtilemap_set_screen(PwTileMap *self, const PwIntRect *screen)
{
  self->screen = *screen;

  /* If wall / tile / window derived, (re)calculate them */
  if (self->flags & SCREEN_WALL) {
    _pwtilemap_screen_wall(self);
  }
}

/*-----------------------------------------------------------------------
 *	Get attributes
 *-----------------------------------------------------------------------*/
void
pwtilemap_get_wall(PwTileMap *self, PwRect *wall)
{
  *wall = self->user.wall;
}

void
pwtilemap_get_tile(PwTileMap *self, PwRect *tile)
{
  *tile = self->user.tile;
}

void
pwtilemap_get_orient(PwTileMap *self, PwOrient *orient)
{
  *orient = self->user.orient;
}

void
pwtilemap_get_fit(PwTileMap *self, PwFit *fit)
{
  *fit = self->user.fit;
}

void
pwtilemap_get_window(PwTileMap *self, PwRect *window)
{
  *window = self->user.window;
}

void
pwtilemap_get_used_window(PwTileMap *self, PwRect *window)
{
  *window = self->window;
}

/*-----------------------------------------------------------------------
 *	Read wall & file definitions from .pitile & .piwall
 *
 *	If the user specifies a tilecode, use one of the predefined
 *	wall/tile definitions, in combination with frame size for bezel
 *	combination.
 *
 *	If the user specifies a config, find the config in the defs files;
 *	find the tile id from defs files; look up the id in the config
 *	to get a role; then continue as for user-specified role.
 *
 *	If the user specifies a role, find this in the defs files; find
 *	the wall name (if given) or use default of "wall"; look up the
 *	wall in the defs files and extract geometry; extract geometry and
 *	(if present) orient and fit.
 *
 *	If the user specified auto, the tile is is used as role name.
 *
 *	Alternatively, the user may specify wall and tile geometry
 *	explicitly, together with orient and fit.  
 *-----------------------------------------------------------------------*/
gboolean
pwtilemap_define(PwTileMap *self, GError **error)
{
  gboolean result = FALSE;
  gchar *id = NULL;
  gchar *role = NULL;
  GKeyFile *piwall = NULL;

  /* Fetch definitions from .pitile and .piwall if needed */
  if (self->flags & (USER_CONFIG | USER_ROLE | USER_AUTO)) {
    if (self->defs == NULL) {
      if (! (self->defs = pwdefs_create_tile(error))) {
	goto fail;
      }
    }
  }

  if (self->flags & USER_TILECODE) {
    gdouble fx = self->user.framex;
    gdouble fy = self->user.framey;
    switch (self->user.tilecode) {
    case 41:			/* 2x2, top left */
      PWRECT_SET0(self->wall, 16*(fx+1), 9*(fy+1));
      PWRECT_SET(self->tile, 16*0, 9*0, 16*1, 9*1);
      break;
    case 42:			/* 2x2, top right */
      PWRECT_SET0(self->wall, 16*(fx+1), 9*(fy+1));
      PWRECT_SET(self->tile, 16*fx, 9*0, 16*(fx+1), 9*1);
      break;
    case 43:			/* 2x2, bottom left */
      PWRECT_SET0(self->wall, 16*(fx+1), 9*(fy+1));
      PWRECT_SET(self->tile, 16*0, 9*fy, 16*1, 9*(fy+1));
      break;
    case 44:			/* 2x2, bottom right */
      PWRECT_SET0(self->wall, 16*(fx+1), 9*(fy+1));
      PWRECT_SET(self->tile, 16*fx, 9*fy, 16*(fx+1), 9*(fy+1));
      break;
    default:
      ERROR(0, "Unknown tile code %d", self->user.tilecode);
      goto fail;
    }

  } else if (self->flags & USER_CONFIG) {
    /* Looking up id in config gives role */
    if (! pwdefs_has_section(self->defs, self->user.config)) {
      ERROR(0, "No [%s] section in ~/.pitile or ~/.piwall", self->user.config);
      goto fail;
    }
    if ((id = _pwtilemap_tile_id(self, error)) == NULL) {
      goto fail;
    }
    if ((role = pwdefs_string(self->defs, self->user.config, id, error)) == NULL) {
      g_clear_error(error);
      ERROR(0, "No %s in [%s] in ~/.pitile or ~/.piwall", id, self->user.config);
      goto fail;
    }
    if (! _pwtilemap_from_role(self, role, piwall, error)) goto fail;

  } else if (self->flags & USER_ROLE) {
    if (! _pwtilemap_from_role(self, self->user.role, piwall, error)) goto fail;

  } else if (self->flags & USER_AUTO) {
    /* Get role (id) from .pitile */
    if ((id = _pwtilemap_tile_id(self, error)) == NULL) {
      goto fail;
    }

    if (! _pwtilemap_from_role(self, id, piwall, error)) goto fail;

  } else {
    /* Use explicit wall and/or tile, defaulting to screen (if known) */
    if (self->flags & USER_WALL) {
      self->wall = self->user.wall;
    } else {
      self->flags |= SCREEN_WALL;
      _pwtilemap_screen_wall(self);
    }
    if (self->flags & USER_TILE) {
      self->wall = self->user.tile;
    } else {
      self->flags |= WALL_TILE;
      self->tile = self->wall;
    }
    self->orient = self->user.orient;
    self->fit = self->user.fit;
  }

  if (self->flags & WALL_WINDOW) {
    _pwtilemap_wall_window(self);
  } else {
    self->window = self->user.window;
  }

  /* SUCCESS */
  result = TRUE;

 fail:
  g_free(role);
  g_free(id);
  return result;
}

/*-----------------------------------------------------------------------
 *	Get the tile id from .pitile or fall back to hostname
 *-----------------------------------------------------------------------*/
static char *
_pwtilemap_tile_id(PwTileMap *self, GError **error)
{
  char *id = _pwtilemap_pitile_id(self, error);
  if (id == NULL) {
    struct utsname uts;
    g_clear_error(error);
    if (uname(&uts) < 0) {
      ERROR(0, "Error from uname(): %s", g_strerror(errno));
      return NULL;
    }
    id = g_strdup(uts.nodename);
  }
  return id;
}

/*-----------------------------------------------------------------------
 *	Get the tile id from .pitile	
 *-----------------------------------------------------------------------*/
static char *
_pwtilemap_pitile_id(PwTileMap *self, GError **error)
{
  gchar *id = NULL;
  
  if (! pwdefs_has_section(self->defs, "tile")) {
    ERROR(0, "No [tile] section in ~/.pitile");
    goto fail;
  }
  if ((id = pwdefs_string(self->defs, "tile", "id", error)) == NULL) {
    g_clear_error(error);
    ERROR(0, "No id in [tile] section in ~/.pitile");
    goto fail;
  }
  /* SUCCESS */

 fail:
  return id;
}

/*-----------------------------------------------------------------------
 *	Fetch tile definition from .piwall, except for those elements
 *	already explicitly defined i.e. overridden
 *-----------------------------------------------------------------------*/
static gboolean
_pwtilemap_from_role(PwTileMap *self, const gchar *role, GKeyFile *piwall,
		     GError **error)
{
  gboolean result = FALSE;
  gchar *wall_s = NULL;
  gchar *orient_s = NULL;

  /* Lookup role */
  if (! pwdefs_has_section(self->defs, role)) {
    ERROR(0, "No [%s] section in ~/.pitile or ~/.piwall", role);
    goto fail;
  }

  if (! (self->flags & USER_WALL)) {
    /* Get role's optional wall name, default "wall" */
    if ((wall_s = pwdefs_string(self->defs, role, "wall", error)) == NULL) {
      g_clear_error(error);
      wall_s = g_strdup("wall");
    }

    /* Get wall definition */
    if (! pwdefs_has_section(self->defs, wall_s)) {
      ERROR(0, "No [%s] section in ~/.pitile or ~/.piwall", wall_s);
      goto fail;
    }
    if (! _pwtilemap_get_rect(self, wall_s, &self->wall, error)) goto fail;
  }
  self->flags &= ~ SCREEN_WALL;

  if (! (self->flags & USER_TILE)) {
    /* Get this tile's location */
    if (! _pwtilemap_get_rect(self, role, &self->tile, error)) goto fail;
  }
  self->flags &= ~ WALL_TILE;

  if (! (self->flags & USER_ORIENT)) {
    if ((orient_s = pwdefs_string(self->defs, role, "orient", error)) == NULL) {
      /* orient is optional */
      g_clear_error(error);
      self->orient = PW_ORIENT_UP;
    } else {
      if (! pworient_from_string(&self->orient, orient_s, error)) goto fail;
    }
  }
  /* SUCCESS */
  result = TRUE;

 fail:
  g_free(wall_s);
  g_free(orient_s);
  return result;
}

/*-----------------------------------------------------------------------
 *	When wall is defined as the screen, recalculate dependencies
 *-----------------------------------------------------------------------*/
static void
_pwtilemap_screen_wall(PwTileMap *self)
{
  PWRECT_SET(self->wall,
	     self->screen.x0, self->screen.y0,
	     self->screen.x1, self->screen.y1);
  if (self->flags & WALL_TILE) {
    /* Single screen - tile is same as wall */
    self->tile = self->wall;
    PWRECT_SET(self->tile,
	       self->screen.x0, self->screen.y0,
	       self->screen.x1, self->screen.y1);
  }
  if (self->flags & WALL_WINDOW) {
    /* Window is percentage of wall */
    _pwtilemap_wall_window(self);
  }
}

static void
_pwtilemap_wall_window(PwTileMap *self)
{
#define PCTX(x) (self->wall.x0 + ((x)/100.0) * PWRECT_WIDTH(self->wall))
#define PCTY(x) (self->wall.y0 + ((x)/100.0) * PWRECT_HEIGHT(self->wall))
  PWRECT_SET(self->window,
	     PCTX(self->user.window.x0),
	     PCTY(self->user.window.y0),
	     PCTX(self->user.window.x1),
	     PCTY(self->user.window.y1));
#undef PCTX
#undef PCTY
}

/*-----------------------------------------------------------------------
 *	Get a rectangle definition from a key file
 *-----------------------------------------------------------------------*/
static gboolean
_pwtilemap_get_rect(PwTileMap *self, const gchar *section, PwRect *rect,
		    GError **error)
{
  double x, y, width, height;

  /* Get x and y, but assume 0 if absent */
  x = pwdefs_double(self->defs, section, "x", error);
  if (g_error_matches(*error,
		      G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND)) {
    g_clear_error(error);
    x = 0.0;
  } else if (*error) {
    return FALSE;
  }
  y = pwdefs_double(self->defs, section, "y", error);
  if (g_error_matches(*error,
		      G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND)) {
    g_clear_error(error);
    y = 0.0;
  } else if (*error) {
    return FALSE;
  }

  /* width and height must be present */
  width = pwdefs_double(self->defs, section, "width", error);
  if (*error) {
    g_clear_error(error);
    ERROR(0, "No width in [%s] in ~/.pitile or ~/.piwall", section);
    return FALSE;
  }
  height = pwdefs_double(self->defs, section, "height", error);
  if (*error) {
    g_clear_error(error);
    ERROR(0, "No height in [%s] in ~/.pitile or ~/.piwall", section);
    return FALSE;
  }

  PWRECT_SET(*rect, x, y, x+width, y+height);
  return TRUE;
}

/*-----------------------------------------------------------------------
 *	Apply mapping given picture dimensions
 *-----------------------------------------------------------------------*/
gboolean
pwtilemap_map_picture(PwTileMap *self, const PwIntRect *picture,
		      PwIntRect *src, PwIntRect *dest,
		      PwVcTransform *transform,
		      GError **error)
{
  gdouble xmag, ymag;
  Scale w2px, w2py;
  PwRect p;
  PwRect w;
  Scale w2sx, w2sy;
  PwRect s;

  /* See how picture will fit on window
     - scale factors for wall to picture coords
  */
  xmag = (gdouble)PWRECT_WIDTH(*picture) / PWRECT_WIDTH(self->window);
  ymag = (gdouble)PWRECT_HEIGHT(*picture) / PWRECT_HEIGHT(self->window);
  switch (self->fit) {
  case PW_FIT_CLIP:
    xmag = ymag = MIN(xmag, ymag);
    break;
  case PW_FIT_LETTERBOX:
    xmag = ymag = MAX(xmag, ymag);
    break;
  case PW_FIT_STRETCH:
    /* No adjustment */
    break;
  }
  /* Scaling from window to picture coords, mapping centre -> centre */
  scale_from_factor_point(&w2px, xmag,
			  (self->window.x0 + self->window.x1)/2,
			  (gdouble)PWRECT_WIDTH(*picture)/2);
  scale_from_factor_point(&w2py, ymag,
			  (self->window.y0 + self->window.y1)/2,
			  (gdouble)PWRECT_HEIGHT(*picture)/2);
  /* Calculate the rectangle in picture coordinates we want to display
     on the tile */
  p.x0 = scale(&w2px, self->tile.x0);
  p.x1 = scale(&w2px, self->tile.x1);
  p.y0 = scale(&w2py, self->tile.y0);
  p.y1 = scale(&w2py, self->tile.y1);
  /* Work out crop and region bearing in mind that this rectangle may not lie
     entirely within the picture */
  src->x0 = ICLIP(0, PWRECT_WIDTH(*picture), p.x0);
  src->x1 = ICLIP(0, PWRECT_WIDTH(*picture), p.x1);
  src->y0 = ICLIP(0, PWRECT_HEIGHT(*picture), p.y0);
  src->y1 = ICLIP(0, PWRECT_HEIGHT(*picture), p.y1);
  /* Wall coordinates of picture */
  w.x0 = unscale(&w2px, 0);
  w.x1 = unscale(&w2px, (gdouble)PWRECT_WIDTH(*picture));
  w.y0 = unscale(&w2py, 0);
  w.y1 = unscale(&w2py, (gdouble)PWRECT_HEIGHT(*picture));
  /* Now in screen coordinates */
  /* tilex -> 0, tilex+tilew -> screenw */
  switch ((self->orient)) {
  default:
  case PW_ORIENT_UP:
    scale_from_points(&w2sx, self->tile.x0, self->tile.x1,
		      0, PWRECT_WIDTH(self->screen));
    scale_from_points(&w2sy, self->tile.y0, self->tile.y1,
		      0, PWRECT_HEIGHT(self->screen));
    s.x0 = scale(&w2sx, w.x0);
    s.x1 = scale(&w2sx, w.x1);
    s.y0 = scale(&w2sy, w.y0);
    s.y1 = scale(&w2sy, w.y1);
    *transform = PW_VCTRANSFORM_ROT0;
    break;
  case PW_ORIENT_DOWN:
    scale_from_points(&w2sx, self->tile.x0, self->tile.x1,
		      PWRECT_WIDTH(self->screen), 0);
    scale_from_points(&w2sy, self->tile.y0, self->tile.y1,
		      PWRECT_HEIGHT(self->screen), 0);
    s.x0 = scale(&w2sx, w.x1);
    s.x1 = scale(&w2sx, w.x0);
    s.y0 = scale(&w2sy, w.y1);
    s.y1 = scale(&w2sy, w.y0);
    *transform = PW_VCTRANSFORM_ROT180;
    break;
  case PW_ORIENT_LEFT:
    scale_from_points(&w2sx, self->tile.y0, self->tile.y1,
		      PWRECT_WIDTH(self->screen), 0);
    scale_from_points(&w2sy, self->tile.x0, self->tile.x1,
		      0, PWRECT_HEIGHT(self->screen));
    s.x0 = scale(&w2sx, w.y1);
    s.x1 = scale(&w2sx, w.y0);
    s.y0 = scale(&w2sy, w.x0);
    s.y1 = scale(&w2sy, w.x1);
    /* Screen is rotated left, so rotate image to right */
    *transform = PW_VCTRANSFORM_ROT90;
    break;
  case PW_ORIENT_RIGHT:
    scale_from_points(&w2sx, self->tile.y0, self->tile.y1,
		      0, PWRECT_WIDTH(self->screen));
    scale_from_points(&w2sy, self->tile.x0, self->tile.x1,
		      PWRECT_HEIGHT(self->screen), 0);
    s.x0 = scale(&w2sx, w.y0);
    s.x1 = scale(&w2sx, w.y1);
    s.y0 = scale(&w2sy, w.x1);
    s.y1 = scale(&w2sy, w.x0);
    /* Screen is rotated right, so rotate image to left */
    *transform = PW_VCTRANSFORM_ROT270;
    break;
  }
  dest->x0 = ICLIP(0, PWRECT_WIDTH(self->screen), s.x0);
  dest->x1 = ICLIP(0, PWRECT_WIDTH(self->screen), s.x1);
  dest->y0 = ICLIP(0, PWRECT_HEIGHT(self->screen), s.y0);
  dest->y1 = ICLIP(0, PWRECT_HEIGHT(self->screen), s.y1);
  g_clear_error(error);
  return TRUE;
}

/*-----------------------------------------------------------------------
 *	Parse --tile-code option
 *-----------------------------------------------------------------------*/
static gboolean
_pwtilemap_opt_tilecode(const gchar *UNUSED(option_name), const gchar *value,
			gpointer data, GError **error)
{
  PwTileMap *self = data;
  int code;
  char c;
  if (sscanf(value, "%d%c", &code, &c) != 1) {
    g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
		"Invalid tile code");
    return FALSE;
  }
  pwtilemap_set_tilecode(self, code);
  return TRUE;
}

/*-----------------------------------------------------------------------
 *	Parse --frame-size option
 *-----------------------------------------------------------------------*/
static gboolean
_pwtilemap_opt_framesize(const gchar *UNUSED(option_name), const gchar *value,
			 gpointer data, GError **error)
{
  PwTileMap *self = data;
  gfloat fx, fy;
  char c;
  if (sscanf(value, "%gx%g%c", &fx, &fy, &c) != 2) {
    g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
		"Invalid frame dimension");
    return FALSE;
  }
  pwtilemap_set_framesize(self, fx, fy);
  return TRUE;
}

/*-----------------------------------------------------------------------
 *	Parse --wall option
 *-----------------------------------------------------------------------*/
static gboolean
_pwtilemap_opt_wall(const gchar *UNUSED(option_name), const gchar *value,
		    gpointer data, GError **error)
{
  PwTileMap *self = data;
  PwRect wall;
  GError *pwerror = NULL;
  if (! pwrect_from_string(&wall, value, &pwerror)) {
    g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED, "%s", pwerror->message);
    g_error_free(pwerror);
    return FALSE;
  }
  pwtilemap_set_wall(self, &wall);
  return TRUE;
}

/*-----------------------------------------------------------------------
 *	Parse "--tile" option value
 *-----------------------------------------------------------------------*/
static gboolean
_pwtilemap_opt_tile(const gchar *UNUSED(option_name), const gchar *value,
		    gpointer data, GError **error)
{
  PwTileMap *self = data;
  PwRect tile;
  GError *pwerror = NULL;
  if (! pwrect_from_string(&tile, value, &pwerror)) {
    g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED, "%s",
		pwerror->message);
    g_error_free(pwerror);
    return FALSE;
  }
  pwtilemap_set_tile(self, &tile);
  return TRUE;
}

/*-----------------------------------------------------------------------
 *	Parse --window option
 *-----------------------------------------------------------------------*/
static gboolean
_pwtilemap_opt_window(const gchar *UNUSED(option_name), const gchar *value,
		      gpointer data, GError **error)
{
  PwTileMap *self = data;
  gchar *most = NULL;
  PwRect window;
  gboolean percent = FALSE;
  GError *pwerror = NULL;
  if (g_str_has_suffix(value, "%")) {
    percent = TRUE;
    value = most = g_strndup(value, strlen(value)-1);
  }
  if (! pwrect_from_string(&window, value, &pwerror)) {
    g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED, "%s", pwerror->message);
    g_error_free(pwerror);
    g_free(most);
    return FALSE;
  }
  pwtilemap_set_window(self, &window, percent);
  g_free(most);
  return TRUE;
}

/*-----------------------------------------------------------------------
 *	Parse "--orient" option
 *-----------------------------------------------------------------------*/
static gboolean
_pwtilemap_opt_orient(const gchar *UNUSED(option_name), const gchar *value,
		      gpointer data, GError **error)
{
  PwTileMap *self = data;
  PwOrient orient;
  GError *pwerror = NULL;
  if (! pworient_from_string(&orient, value, &pwerror)) {
    g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED, "%s",
		pwerror->message);
    g_error_free(pwerror);
    return FALSE;
  }
  pwtilemap_set_orient(self, orient);
  return TRUE;
}

/*-----------------------------------------------------------------------
 *	Parse "--fit" option
 *-----------------------------------------------------------------------*/
static gboolean
_pwtilemap_opt_fit(const gchar *UNUSED(option_name), const gchar *value,
		   gpointer data, GError **error)
{
  PwTileMap *self = data;
  PwFit fit;
  GError *pwerror = NULL;
  if (! pwfit_from_string(&fit, value, &pwerror)) {
    g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED, "%s",
		pwerror->message);
    g_error_free(pwerror);
    return FALSE;
  }
  pwtilemap_set_fit(self, fit);
  return TRUE;
}

static gboolean
_pwtilemap_opt_autotile(const gchar *UNUSED(option_name),
			const gchar *UNUSED(value), gpointer data,
			GError **UNUSED(error))
{
  PwTileMap *self = data;
  self->flags |= USER_AUTO;
  return TRUE;
}

static gboolean
_pwtilemap_opt_role(const gchar *UNUSED(option_name),
		    const gchar *value, gpointer data, GError **error)
{
  PwTileMap *self = data;
  if (strlen(value) == 0) {
    g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
		"Role may not be an empty string");
    return FALSE;
  }
  pwtilemap_set_role(self, value);
  return TRUE;
}

static gboolean
_pwtilemap_opt_config(const gchar *UNUSED(option_name),
		      const gchar *value, gpointer data, GError **error)
{
  PwTileMap *self = data;
  if (strlen(value) == 0) {
    g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
		"Config may not be an empty string");
    return FALSE;
  }
  pwtilemap_set_config(self, value);
  return TRUE;
}

/*-----------------------------------------------------------------------
 *	Create an option group for option parsing
 *-----------------------------------------------------------------------*/
static GOptionGroup *
_pwtilemap_option_group(PwTileMap *self, gboolean in_main)
{
  GOptionFlags flags = in_main ? G_OPTION_FLAG_IN_MAIN : 0;
  GOptionEntry options[] = {
    {"tile-code", 0, flags, G_OPTION_ARG_CALLBACK, &_pwtilemap_opt_tilecode,
     "Define code for tile position", "TILECODE"},
    {"frame-size", 0, flags, G_OPTION_ARG_CALLBACK, &_pwtilemap_opt_framesize,
     "Set size of screen + bezel relative to displayed area", "FXxFY"},
    {"wall", 'W', flags, G_OPTION_ARG_CALLBACK, &_pwtilemap_opt_wall,
     "Define virtual wall geometry", "XxY+L+T"},
    {"window", 'w', flags, G_OPTION_ARG_CALLBACK, &_pwtilemap_opt_window,
     "Define window within wall, maybe as percentage", "XxY+L+T[%]"},
    {"tile", 'T', flags, G_OPTION_ARG_CALLBACK, &_pwtilemap_opt_tile,
     "Define tile", "XxY+L+T"},
    {"orient", 'O', flags, G_OPTION_ARG_CALLBACK, &_pwtilemap_opt_orient,
     "Define orientation", "DIRN"},
    {"fit", 'F', flags, G_OPTION_ARG_CALLBACK, &_pwtilemap_opt_fit,
     "How to fit picture on wall", "FIT"},
    {"autotile", 'A', flags | G_OPTION_FLAG_NO_ARG,
     G_OPTION_ARG_CALLBACK, &_pwtilemap_opt_autotile,
     "Define mapping from .pitile and .piwall", 0},
    {"role", 'R', flags, G_OPTION_ARG_CALLBACK, &_pwtilemap_opt_role,
     "Role within .piwall definition", "ROLE"},
    {"config", 'C', flags, G_OPTION_ARG_CALLBACK, &_pwtilemap_opt_config,
     "Configuration within .piwall definition", "CONFIG"},
    {NULL,0,0,0,0,0,0},
  };
  GOptionGroup *group = g_option_group_new("tilemap",
					   "Video wall tile mapping options:",
					   "Options for mapping a tile in a video wall",
					   self,
					   NULL); /* Assume PwTileMap destroyed by caller */
  
  g_option_group_add_entries(group, options);

  return group;
}

void
pwtilemap_add_options(PwTileMap *self, GOptionContext *context)
{
  GOptionGroup *group = _pwtilemap_option_group(self, TRUE);
  g_option_context_add_group(context, group);
}

void
pwtilemap_add_option_group(PwTileMap *self, GOptionContext *context)
{
  GOptionGroup *group = _pwtilemap_option_group(self, FALSE);
  g_option_context_add_group(context, group);
}

/*-----------------------------------------------------------------------
 *	Release resources
 *-----------------------------------------------------------------------*/
void
pwtilemap_ref(PwTileMap *self)
{
  ++ self->nrefs;
}

void
pwtilemap_unref(PwTileMap *self)
{
  if (-- self->nrefs <= 0) pwtilemap_free(self);
}

void
pwtilemap_free(PwTileMap *self)
{
  g_free(self->user.role);
  if (self->defs) pwdefs_unref(self->defs);
  g_free(self);
}


/*-----------------------------------------------------------------------
 *	Functions for linear scaling
 *-----------------------------------------------------------------------*/
static void
scale_init(Scale *self, gdouble factor, gdouble offset)
{
  self->factor = factor;
  self->offset = offset;
}

static void
scale_from_factor_point(Scale *self, gdouble factor, gdouble old, gdouble new)
{
  scale_init(self, factor, new - old * factor);
}

static void
scale_from_points(Scale *self,
		  gdouble old0, gdouble old1, gdouble new0, gdouble new1)
{
  gdouble factor = (new1 -new0) / (old1 - old0);
  scale_init(self, factor, new0 - old0 * factor);
}

static gdouble
scale(const Scale *self, gdouble value)
{
  return value * self->factor + self->offset;
}

static gdouble
unscale(const Scale *self, gdouble value)
{
  return (value - self->offset) / self->factor;
}
