/*
 * TreeFrogUI ebook reader (standalone) - MuPDF backend.
 *
 * Renders epub / mobi / pdf / cbz / fb2 / xps to RGB565 and blits through
 * driver.so (hwdisp), same present path as lgpt/rockbox/pcsx4all. Input via
 * cubevol's /tmp/joy_key shared memory.
 *
 * Reading:
 *   R1 / RIGHT / A   -> next page        L1 / LEFT / B -> previous page
 *   R2 -> jump +10%                      L2 -> jump -10%
 *   SELECT           -> open/close menu
 *   START            -> (in menu) not used; SELECT+START anytime -> quit+save
 * Menu (SELECT): UP/DOWN move, A select, B/SELECT close.
 *
 * Progress is saved to "<book>.pos" (page + font size) and restored on open.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <mupdf/fitz.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "font8x8.h"
#include "port_sf3000/hwdisp.h"

/* joy_key bits (cubevol; see picoarch plat_sdl.c). */
enum {
  K_SELECT = 0,
  K_START = 3,
  K_UP = 4,
  K_RIGHT = 5,
  K_DOWN = 6,
  K_LEFT = 7,
  K_L2 = 8,
  K_R2 = 9,
  K_L1 = 10,
  K_R1 = 11,
  K_X = 12,
  K_A = 13,
  K_B = 14,
  K_Y = 15
};
#define BIT(b) (1u << (b))
#define STATUS_H 24

static volatile uint32_t *g_keys;
static int PW = 854, PH = 480; /* panel pixels */

static uint16_t *FB; /* PW*PH RGB565 */

static void input_init(void) {
  key_t k = ftok("/tmp/joy_key", 'a');
  if (k == (key_t)-1)
    return;
  int id = shmget(k, 4, IPC_CREAT | 0666);
  if (id < 0)
    return;
  void *p = shmat(id, NULL, 0);
  if (p != (void *)-1)
    g_keys = (uint32_t *)p;
}

static uint32_t keys(void) { return g_keys ? (*g_keys & 0xFFFF) : 0; }

/* ---- tiny 8x8 UI text (menu/status), scaled --------------------------------
 */
static void put_char(int x, int y, int sc, char c, uint16_t col) {
  if ((unsigned char)c >= 128)
    c = '?';
  const unsigned char *g = font8x8 + (int)(unsigned char)c * 8;
  for (int row = 0; row < 8; row++) {
    unsigned char bits = g[row];
    for (int cx = 0; cx < 8; cx++) {
      if (!(bits & (0x80 >> cx)))
        continue;
      for (int sy = 0; sy < sc; sy++)
        for (int sx = 0; sx < sc; sx++) {
          int px = x + cx * sc + sx, py = y + row * sc + sy;
          if (px >= 0 && px < PW && py >= 0 && py < PH)
            FB[py * PW + px] = col;
        }
    }
  }
}

static void put_text(int x, int y, int sc, const char *s, uint16_t col) {
  for (; *s; s++) {
    put_char(x, y, sc, *s, col);
    x += 8 * sc;
  }
}

static void fill_rect(int x, int y, int w, int h, uint16_t col) {
  for (int j = 0; j < h; j++) {
    int py = y + j;
    if (py < 0 || py >= PH)
      continue;
    for (int i = 0; i < w; i++) {
      int px = x + i;
      if (px < 0 || px >= PW)
        continue;
      FB[py * PW + px] = col;
    }
  }
}

static uint64_t ticks_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);

  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- MuPDF -----------------------------------------------------------------
 */
static fz_context *ctx;
static fz_colorspace *rgb;
static fz_document *doc;
static int page = 0, count = 1;
static int pos_dirty = 0;

/* ---- font size (stored as on-panel px; render em is 1.5x since we render at
 * 720 tall and the panel is 480 tall) ------------------------------------- */
static int font_px = 24; /* what the user sets/sees */
#define PX_MIN 12
#define PX_MAX 48
static float em_from_px(void) { return font_px * 720.0f / 480.0f; }

/* ---- font face: built-in serif/sans/mono + any TTF/OTF dropped in a fonts
 * dir. Custom faces are injected via an @font-face user CSS pointing at the
 * absolute file path. ------------------------------------------------------ */
typedef struct {
  char name[48];
  char css[512];
  int builtin;
} FontEntry;
static FontEntry fonts[32];
static int font_count = 0, font_sel = 0;

static void add_builtin(const char *name, const char *family) {
  FontEntry *e = &fonts[font_count++];
  snprintf(e->name, sizeof e->name, "%s", name);
  if (family[0])
    snprintf(e->css, sizeof e->css, "* { font-family: %s !important; }",
             family);
  else
    e->css[0] = 0;
  e->builtin = 1;
}
static void scan_font_dir(const char *dir) {
  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *de;
  while ((de = readdir(d)) && font_count < 32) {
    const char *dot = strrchr(de->d_name, '.');
    if (!dot || (strcasecmp(dot, ".ttf") && strcasecmp(dot, ".otf")))
      continue;
    FontEntry *e = &fonts[font_count++];
    snprintf(e->name, sizeof e->name, "%.47s", de->d_name);
    snprintf(e->css, sizeof e->css,
             "@font-face{font-family:\"ebk\";src:url(\"%s/%s\")}"
             "*{font-family:\"ebk\" !important}",
             dir, de->d_name);
    e->builtin = 0;
  }
  closedir(d);
}
static void fonts_init(void) {
  add_builtin("Serif (default)", "");
  add_builtin("Sans", "sans-serif");
  add_builtin("Mono", "monospace");
  scan_font_dir("/mnt/sdcard/frogui/fonts");
  scan_font_dir("/mnt/sdcard/cubegm/fonts");
  scan_font_dir("/mnt/sdcard/roms/Ebook/fonts");
}
static void apply_font(void) {
  if (font_sel < 0 || font_sel >= font_count)
    font_sel = 0;
  fz_set_user_css(ctx, fonts[font_sel].css);
  fz_set_use_document_css(ctx, fonts[font_sel].builtin &&
                                   fonts[font_sel].css[0] == 0);
}

/* Reading themes. Light passes original colors through (images stay true).
 * Sepia/Dark map each pixel's luminance across a two-tone page->ink ramp, which
 * reads cleanly for text and gives a classic e-reader tint on the whole page.
 */
enum { TH_LIGHT, TH_SEPIA, TH_DARK, TH_N };
static int theme = TH_LIGHT;
static const char *theme_name[TH_N] = {"Light", "Sepia", "Dark"};
/* ramp[theme] = { page{r,g,b} at luma 255, ink{r,g,b} at luma 0 } */
static const unsigned char ramp[TH_N][2][3] = {
    {{255, 255, 255}, {0, 0, 0}},    /* light (mapping skipped) */
    {{244, 236, 216}, {91, 64, 40}}, /* sepia: cream page, brown ink */
    {{24, 24, 26}, {212, 212, 208}}, /* dark: near-black page, warm grey ink */
};

static inline uint16_t rgb565(int r, int g, int b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static uint16_t bg565(void) {
  const unsigned char *p = ramp[theme][0];
  return theme == TH_LIGHT ? 0xFFFF : rgb565(p[0], p[1], p[2]);
}

static uint16_t theme_lut[256];

static void build_theme_lut(void) {
  if (theme == TH_LIGHT)
    return;

  const unsigned char *bg = ramp[theme][0];
  const unsigned char *ink = ramp[theme][1];

  for (int L = 0; L < 256; L++) {
    int r = ink[0] + (bg[0] - ink[0]) * L / 255;
    int g = ink[1] + (bg[1] - ink[1]) * L / 255;
    int b = ink[2] + (bg[2] - ink[2]) * L / 255;

    theme_lut[L] = rgb565(r, g, b);
  }
}

static void pix_to_fb(const fz_pixmap *pix) {
  int usable_h = PH - STATUS_H;

  int W = pix->w < PW ? pix->w : PW;
  int H = pix->h < usable_h ? pix->h : usable_h;

  int xoff = (PW - W) / 2;
  int yoff = (usable_h - H) / 2;

  uint16_t fill = bg565();

  for (size_t i = 0; i < (size_t)PW * PH; i++)
    FB[i] = fill;

  for (int y = 0; y < H; y++) {
    const unsigned char *s = pix->samples + (size_t)y * pix->stride;
    uint16_t *d = FB + (size_t)(y + yoff) * PW + xoff;

    for (int x = 0; x < W; x++) {
      int r = s[0];
      int g = s[1];
      int b = s[2];

      if (theme == TH_LIGHT) {
        d[x] = rgb565(r, g, b);
      } else {
        int L = (r * 77 + g * 150 + b * 29) >> 8;
        d[x] = theme_lut[L];
      }
      s += pix->n;
    }
  }
}
static void present(void) { hwdisp_present(FB, PW, PH, PW * 2); }

#define UIS                                                                    \
  3 /* 8x8 UI font scale at 1280x720 (24px glyphs -> ~12px on panel) */
static void splash(const char *msg) {
  memset(FB, 0x00, (size_t)PW * PH * 2);
  put_text(PW / 2 - (int)strlen(msg) * 4 * UIS, PH / 2 - 4 * UIS, UIS, msg,
           0xFFFF);
  present();
}

static void render_page(void) {
  if (page < 0)
    page = 0;

  if (page >= count)
    page = count - 1;

  fz_pixmap *pix = NULL;

  fz_try(ctx) {
    pix =
        fz_new_pixmap_from_page_number(ctx, doc, page, fz_scale(1, 1), rgb, 0);

    pix_to_fb(pix);
  }
  fz_always(ctx) {

    if (pix)
      fz_drop_pixmap(ctx, pix);
  }
  fz_catch(ctx) {

    fz_report_error(ctx);

    memset(FB, 0xFF, (size_t)PW * PH * 2);
  }
}

static void reflow(void) {
  splash("Reflowing...");
  float frac = count > 1 ? (float)page / (count - 1) : 0;
  fz_try(ctx) {
    fz_layout_document(ctx, doc, (float)PW, (float)PH, em_from_px());
    count = fz_count_pages(ctx, doc);
  }
  fz_catch(ctx) { fz_report_error(ctx); }
  page = (int)(frac * (count - 1) + 0.5f);
  if (page < 0)
    page = 0;
  if (page >= count)
    page = count - 1;
}

/* ---- progress persistence --------------------------------------------------
 */
static char pos_path[1100];

static void pos_init(const char *book) {
  char dir[1024];
  char base[512];

  snprintf(dir, sizeof(dir), "%s", book);
  snprintf(base, sizeof(base), "%s",
           strrchr(book, '/') ? strrchr(book, '/') + 1 : book);

  char *d = dirname(dir);

  char resdir[1100];
  snprintf(resdir, sizeof(resdir), "%s/.res", d);

  if (mkdir(resdir, 0755) != 0 && errno != EEXIST)
    perror("mkdir");

  snprintf(pos_path, sizeof(pos_path), "%s/%s.pos", resdir, base);
}

static void pos_load(void) {
  FILE *f = fopen(pos_path, "r");
  if (!f)
    return;
  int p, px = 0, fs = 0, th = 0;
  int n = fscanf(f, "%d %d %d %d", &p, &px, &fs, &th);
  if (n >= 1)
    page = p;
  if (n >= 2 && px >= PX_MIN && px <= PX_MAX)
    font_px = px;
  if (n >= 3 && fs >= 0 && fs < font_count)
    font_sel = fs;
  if (n >= 4 && th >= 0 && th < TH_N)
    theme = th;

  build_theme_lut();
  fclose(f);
}

static void pos_save(void) {
  char tmp[1100];
  snprintf(tmp, sizeof(tmp), "%s.tmp", pos_path);

  FILE *fp = fopen(tmp, "wb");
  if (!fp)
    return;

  fprintf(fp, "%d %d %d %d\n", page, font_px, font_sel, theme);

  fclose(fp);

  rename(tmp, pos_path);
}

/* ---- menu ------------------------------------------------------------------
 * Two row kinds:
 *   VALUE rows (Text size, Font)  -> LEFT/RIGHT change the value in place.
 *   ACTION rows (Resume/jump/...) -> A activates.
 */
enum {
  M_SIZE,
  M_FONT,
  M_THEME,
  M_RESUME,
  M_FWD,
  M_BACK,
  M_START,
  M_END,
  M_QUIT,
  M_N
};

static void draw_status(void) {
  char s[96];
  int pct = count > 1 ? page * 100 / (count - 1) : 100;
  snprintf(s, sizeof s, "Page %d / %d   %d%%   [SELECT] menu", page + 1, count,
           pct);
  int bh = 8 * UIS + 6;
  fill_rect(0, PH - bh, PW, bh, 0x0000);
  put_text(6, PH - bh + 3, UIS, s, 0xFFFF);
}

static void menu_label(int i, char *out, size_t n) {
  switch (i) {
  case M_SIZE:
    snprintf(out, n, "< Text size: %d px >", font_px);
    break;
  case M_FONT:
    snprintf(out, n, "< Font: %s >", fonts[font_sel].name);
    break;
  case M_THEME:
    snprintf(out, n, "< Theme: %s >", theme_name[theme]);
    break;
  case M_RESUME:
    snprintf(out, n, "Resume");
    break;
  case M_FWD:
    snprintf(out, n, "Jump forward  (+10%%)");
    break;
  case M_BACK:
    snprintf(out, n, "Jump back     (-10%%)");
    break;
  case M_START:
    snprintf(out, n, "Go to start");
    break;
  case M_END:
    snprintf(out, n, "Go to end");
    break;
  case M_QUIT:
    snprintf(out, n, "Quit");
    break;
  default:
    out[0] = 0;
  }
}

/* returns: 0 stay, 1 quit */
static int menu_loop(void) {
  int sel = 0, redraw = 1,
      dirty = 0; /* dirty: size/font changed, reflow on exit */
  /* Render page + dim into a bg buffer once (the dimmed page is just backdrop;
   * size/font changes only reflow when you LEAVE the menu, not per keypress).
   */
  uint16_t *bg = malloc((size_t)PW * PH * 2);
  if (!bg)
    return 1;

  render_page();
  if (bg)
    for (int i = 0; i < PW * PH; i++)
      bg[i] = (FB[i] >> 1) & 0x7BEF;
  const int rowh = 8 * UIS + 12;
  const int bw = 8 * UIS * 26;
#define MENU_CLOSE()                                                           \
  do {                                                                         \
    if (dirty)                                                                 \
      reflow();                                                                \
    free(bg);                                                                  \
    return 0;                                                                  \
  } while (0)
  uint32_t prev = keys();
  for (;;) {
    if (redraw) {
      redraw = 0;
      build_theme_lut();
      if (bg)
        memcpy(FB, bg, (size_t)PW * PH * 2);
      else
        memset(FB, 0, (size_t)PW * PH * 2);
      int bh = M_N * rowh + 16, bx = (PW - bw) / 2, by = (PH - bh) / 2;
      fill_rect(bx, by, bw, bh, 0x0000);
      fill_rect(bx, by, bw, 3, 0xFFFF);
      fill_rect(bx, by + bh - 3, bw, 3, 0xFFFF);
      for (int i = 0; i < M_N; i++) {
        char lbl[64];
        menu_label(i, lbl, sizeof lbl);
        int ry = by + 8 + i * rowh;
        if (i == sel)
          fill_rect(bx + 4, ry - 4, bw - 8, rowh, 0x2104);
        put_text(bx + 12, ry, UIS, lbl, (i == sel) ? 0xFFE0 : 0xFFFF);
      }
      present();
    }

    uint32_t k = keys(), pr = k & ~prev;
    prev = k;
    if ((k & BIT(K_SELECT)) && (k & BIT(K_START))) {
      free(bg);
      pos_dirty = 1;
      return 1;
    }
    if (pr & BIT(K_UP)) {
      if (sel > 0) {
        sel--;
        redraw = 1;
      }
    }
    if (pr & BIT(K_DOWN)) {
      if (sel < M_N - 1) {
        sel++;
        redraw = 1;
      }
    }

    /* LEFT/RIGHT adjust value rows in place - value + label update now, the
     * (slow) reflow is deferred until the menu closes. */
    if (pr & (BIT(K_LEFT) | BIT(K_RIGHT))) {
      int dir = (pr & BIT(K_RIGHT)) ? 1 : -1;
      if (sel == M_SIZE) {
        int np = font_px + dir * 2;
        if (np >= PX_MIN && np <= PX_MAX) {
          font_px = np;
          dirty = 1;
          redraw = 1;
        }
      } else if (sel == M_FONT) {
        font_sel = (font_sel + dir + font_count) % font_count;
        apply_font();
        dirty = 1;
        redraw = 1; /* CSS now, reflow on close */
      } else if (sel == M_THEME) {
        theme = (theme + dir + TH_N) % TH_N; /* color only: no reflow,
                                                page re-renders on close */
        redraw = 1;
      }
    }

    if (pr & BIT(K_SELECT))
      MENU_CLOSE();
    if (pr & BIT(K_B))
      MENU_CLOSE();
    if (pr & BIT(K_A)) {
      switch (sel) {
      case M_RESUME:
        MENU_CLOSE();
      case M_FWD:
        page += count / 10 + 1;
        if (page >= count)
          page = count - 1;
        MENU_CLOSE();
      case M_BACK:
        page -= count / 10 + 1;
        if (page < 0)
          page = 0;
        MENU_CLOSE();
      case M_START:
        page = 0;
        MENU_CLOSE();
      case M_END:
        page = count - 1;
        MENU_CLOSE();
      case M_QUIT:
        free(bg);
        pos_save();
        return 1;
      default:
        break; /* value rows: A does nothing */
      }
    }
    usleep(15000);
  }
}
static uint32_t last_save = 0;

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: reader <book>\n");
    return 1;
  }
  const char *path = argv[1];

  /* Render at the driver's NATIVE 1280x720 and present it 1:1 - the driver
   * then scales that single fixed size down to whatever panel this device has.
   * Device-agnostic (R36SX / SF3000 / SF3500), and 1280x720 is the only size
   * proven safe on every driver: odd aspect-pad widths (853) hang R36SX, and
   * a smaller source integer-scales to a tiny centered square. No pad, no
   * per-axis surprise. */
  PW = 1280;
  PH = 720;

  if (hwdisp_init() != 0) {
    fprintf(stderr, "hwdisp_init failed\n");
    return 1;
  }
  hwdisp_set_target_aspect(0, 0); /* passthrough: hand the driver 1280x720 */
  hwdisp_set_filter(0);           /* HW bilinear downscale to the panel */
  input_init();

  FB = malloc((size_t)PW * PH * 2);
  if (!FB)
    return 1;

  splash("Loading...");
  ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
  if (!ctx)
    return 1;
  rgb = fz_device_rgb(ctx);
  fonts_init();
  fz_try(ctx) {
    fz_register_document_handlers(ctx);
    doc = fz_open_document(ctx, path);
    pos_init(path);
    pos_load(); /* may set font_px + font_sel first */
    build_theme_lut();
    apply_font();
    fz_layout_document(ctx, doc, (float)PW, (float)PH, em_from_px());
    count = fz_count_pages(ctx, doc);
  }
  fz_catch(ctx) {
    fz_report_error(ctx);
    splash("Cannot open book");
    usleep(1500000);
    return 1;
  }
  if (page >= count)
    page = count - 1;
  if (page < 0)
    page = 0;

  int need_draw = 1, save_ctr = 0;

  uint32_t now = ticks_ms();

  if (pos_dirty && now - last_save > 5000) {
    pos_save();
    pos_dirty = 0;
    last_save = now;
  }

  uint32_t prev = keys();
  for (;;) {
    if (need_draw) {
      need_draw = 0;
      render_page();
      draw_status();
      present();
    }

    uint32_t k = keys(), pr = k & ~prev;
    prev = k;

    if ((k & BIT(K_SELECT)) && (k & BIT(K_START))) {
      pos_save();
      break;
    }

    if (pr & BIT(K_SELECT)) {
      if (menu_loop())
        break; /* quit from menu */
      need_draw = 1;
      prev = keys();
      continue;
    }
    int moved = 0;
    if (pr & (BIT(K_R1) | BIT(K_RIGHT) | BIT(K_A))) {
      if (page < count - 1) {
        page++;
        moved = 1;
      }
    }
    if (pr & (BIT(K_L1) | BIT(K_LEFT) | BIT(K_B))) {
      if (page > 0) {
        page--;
        moved = 1;
      }
    }
    if (pr & BIT(K_R2)) {
      page += count / 10 + 1;
      if (page >= count)
        page = count - 1;
      moved = 1;
    }
    if (pr & BIT(K_L2)) {
      page -= count / 10 + 1;
      if (page < 0)
        page = 0;
      moved = 1;
    }
    if (moved) {
      need_draw = 1;
      pos_dirty = 1;

      if (++save_ctr >= 3) {
        pos_save();
        save_ctr = 0;
      }
    }

    usleep(15000);
  }

  /* Fast exit: fz_drop_document/context walks and frees the whole font/page
   * store, which is SLOW on this CPU for a big epub. We're quitting anyway, so
   * save progress, release the display, and let the kernel reclaim memory. */
  splash("Exiting...");
  if (pos_dirty)
    pos_save();
  hwdisp_deinit();
  _exit(0);
}
