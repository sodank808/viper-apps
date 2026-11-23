// Viper: the SDL functions wolf_sdl.h declares, for the engine's 8-bit
// surfaces. Part of the engine, so wolf_names.h gives them its prefix and
// they can't meet the real SDL that the simulator links.
#include "wl_def.h"

SDL_Surface *SDL_CreateSurface8(int w, int h)
{
    SDL_Surface *s = SafeMalloc(sizeof(*s));
    s->w = w;
    s->h = h;
    s->pitch = w;
    s->pixels = SafeMalloc((size_t) w * h);
    memset(s->pixels, 0, (size_t) w * h);
    return s;
}

void SDL_FreeSurface(SDL_Surface *s)
{
    if (!s)
        return;
    free(s->pixels);
    free(s);
}

// Clips `rect` (the whole surface when NULL) to the surface
static int ClipRect(const SDL_Surface *s, const SDL_Rect *rect, SDL_Rect *out)
{
    int x0 = 0, y0 = 0, x1 = s->w, y1 = s->h;

    if (rect)
    {
        x0 = MAX(rect->x, 0);
        y0 = MAX(rect->y, 0);
        x1 = MIN(rect->x + rect->w, s->w);
        y1 = MIN(rect->y + rect->h, s->h);
    }
    out->x = x0;
    out->y = y0;
    out->w = x1 - x0;
    out->h = y1 - y0;
    return out->w > 0 && out->h > 0;
}

int SDL_FillRect(SDL_Surface *s, const SDL_Rect *rect, Uint32 color)
{
    SDL_Rect r;
    int y;

    if (!ClipRect(s, rect, &r))
        return 0;
    for (y = r.y; y < r.y + r.h; y++)
        memset((byte *) s->pixels + y * s->pitch + r.x, (byte) color, r.w);
    return 0;
}

// Same-size 8-bit copies, which is all the engine does: the source rectangle
// (all of it when NULL) to the same place, or to dstrect's corner
int SDL_BlitSurface(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect)
{
    SDL_Rect r;
    int y, dx, dy;

    if (!ClipRect(src, srcrect, &r))
        return 0;
    dx = dstrect ? dstrect->x : r.x;
    dy = dstrect ? dstrect->y : r.y;
    if (dx < 0 || dy < 0)
        return -1;
    r.w = MIN(r.w, dst->w - dx);
    r.h = MIN(r.h, dst->h - dy);
    for (y = 0; y < r.h; y++)
        memcpy((byte *) dst->pixels + (dy + y) * dst->pitch + dx,
               (byte *) src->pixels + (r.y + y) * src->pitch + r.x, r.w);
    return 0;
}

void SDL_ShowSimpleMessageBox(int flags, const char *title, const char *message, void *window)
{
    (void) flags;
    (void) window;
    printf("%s: %s\n", title, message);
}
