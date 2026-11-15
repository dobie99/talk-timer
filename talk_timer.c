/*
 * Talk Timer: a program picker plus segmented timers for the glasses.
 *
 * Buttons are translated in one place (action_from_event) into three named
 * actions; every screen reacts only to those actions:
 *
 *   action   button        Menu             Timer              Done
 *   NEXT     single click  move highlight   start / pause      restart program
 *   SELECT   double click  open program     next segment       back to menu
 *   BACK     long press    exit             back to menu       exit
 *
 * The timer reads only from the selected timer_program_t, so programs from
 * another source (for example the phone) can be added without touching the
 * timer logic.
 */
#include "../common/call_ui.h"

#include "gm_plugin_lvgl_api.h"

/* ------------------------------------------------------------------------ */
/* DEV MODE                                                                 */
/* 1 = every segment's minutes run as seconds, and a "DEV" marker is shown  */
/*     on every screen. Set to 0 before shipping.                           */
/* ------------------------------------------------------------------------ */
#define DEV_FAST_TIME 0

#if DEV_FAST_TIME
#define MS_PER_SEGMENT_UNIT 1000U   /* "minutes" are seconds */
#else
#define MS_PER_SEGMENT_UNIT 60000U
#endif

/* ------------------------------------------------------------------------ */
/* Program data.                                                            */
/* ------------------------------------------------------------------------ */

typedef enum {
    SEGMENT_NORMAL = 0,
    SEGMENT_QUIET = 1,
} segment_style_t;

typedef struct {
    char label[16];
    uint16_t minutes;
    uint8_t style;          /* segment_style_t */
} timer_segment_t;

typedef struct {
    char name[20];
    bool auto_advance;      /* true: move on at 0:00, no overtime */
    uint8_t segment_count;
    const timer_segment_t *segments;
} timer_program_t;

#define COUNT_OF(array) ((uint8_t)(sizeof(array) / sizeof((array)[0])))

static const timer_segment_t k_class_period[] = {
    { "Warm-up",     5U, SEGMENT_NORMAL },
    { "Mini-lesson", 10U, SEGMENT_NORMAL },
    { "Activity",    20U, SEGMENT_NORMAL },
    { "Share-out",   7U, SEGMENT_NORMAL },
    { "Wrap-up",     3U, SEGMENT_NORMAL },
};

static const timer_segment_t k_conference_talk[] = {
    { "Hook",      2U, SEGMENT_NORMAL },
    { "Problem",   4U, SEGMENT_NORMAL },
    { "Main idea", 8U, SEGMENT_NORMAL },
    { "Demo",      4U, SEGMENT_NORMAL },
    { "Close",     2U, SEGMENT_NORMAL },
};

static const timer_segment_t k_lightning_talk[] = {
    { "Hook",  1U, SEGMENT_NORMAL },
    { "Point", 3U, SEGMENT_NORMAL },
    { "Close", 1U, SEGMENT_NORMAL },
};

static const timer_segment_t k_pomodoro[] = {
    { "Focus", 25U, SEGMENT_QUIET },  { "Break",  5U, SEGMENT_NORMAL },
    { "Focus", 25U, SEGMENT_QUIET },  { "Break",  5U, SEGMENT_NORMAL },
    { "Focus", 25U, SEGMENT_QUIET },  { "Break",  5U, SEGMENT_NORMAL },
    { "Focus", 25U, SEGMENT_QUIET },  { "Break", 15U, SEGMENT_NORMAL },
};

static const timer_program_t k_programs[] = {
    { "Class Period",    false, COUNT_OF(k_class_period),    k_class_period },
    { "Conference Talk", false, COUNT_OF(k_conference_talk), k_conference_talk },
    { "Lightning Talk",  false, COUNT_OF(k_lightning_talk),  k_lightning_talk },
    { "Pomodoro",        true,  COUNT_OF(k_pomodoro),        k_pomodoro },
};

/* ------------------------------------------------------------------------ */
/* Small string helpers (no libc dependency).                               */
/* ------------------------------------------------------------------------ */

#define TEXT_MAX 48U

static bool text_equal(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

/* Appends src at dst[*len], always NUL-terminating within cap. */
static void text_append(char *dst, uint32_t cap, uint32_t *len, const char *src)
{
    while (*src != '\0' && *len + 1U < cap) dst[(*len)++] = *src++;
    dst[*len] = '\0';
}

/* Writes "[sign]mm:ss"; minutes grow past two digits (capped at 999). */
static void format_mmss(char *out, uint32_t cap, char sign, uint32_t seconds)
{
    uint32_t minutes = seconds / 60U;
    uint32_t secs = seconds % 60U;
    char buffer[12];
    uint32_t n = 0U;

    if (minutes > 999U) {
        minutes = 999U;
        secs = 59U;
    }
    if (sign != '\0') buffer[n++] = sign;
    if (minutes >= 100U) buffer[n++] = (char)('0' + minutes / 100U);
    buffer[n++] = (char)('0' + (minutes / 10U) % 10U);
    buffer[n++] = (char)('0' + minutes % 10U);
    buffer[n++] = ':';
    buffer[n++] = (char)('0' + secs / 10U);
    buffer[n++] = (char)('0' + secs % 10U);
    buffer[n] = '\0';

    n = 0U;
    out[0] = '\0';
    text_append(out, cap, &n, buffer);
}

/* ------------------------------------------------------------------------ */
/* Input: the only place that looks at raw button events.                   */
/* ------------------------------------------------------------------------ */

typedef enum {
    ACTION_NONE = 0,
    ACTION_NEXT,    /* PRIMARY single click */
    ACTION_SELECT,  /* PRIMARY double click */
    ACTION_BACK,    /* PRIMARY long press   */
} ui_action_t;

/* Unknown buttons/actions (including VERY_LONG and RELEASE that may follow a
 * long press) map to ACTION_NONE and are left unhandled. */
static ui_action_t action_from_event(const gm_plugin_event_t *event)
{
    if (event == NULL || event->struct_size < GM_PLUGIN_EVENT_MIN_SIZE ||
        event->type != GM_PLUGIN_EVENT_BUTTON ||
        event->data.button.button != GM_PLUGIN_BUTTON_PRIMARY)
        return ACTION_NONE;
    switch (event->data.button.action) {
    case GM_PLUGIN_BUTTON_ACTION_SINGLE: return ACTION_NEXT;
    case GM_PLUGIN_BUTTON_ACTION_DOUBLE: return ACTION_SELECT;
    case GM_PLUGIN_BUTTON_ACTION_LONG:   return ACTION_BACK;
    default:                             return ACTION_NONE;
    }
}

/* ------------------------------------------------------------------------ */
/* Timer model: reads only from the program it was given.                   */
/* ------------------------------------------------------------------------ */

typedef struct {
    const timer_program_t *program;
    uint32_t index;        /* current segment */
    uint32_t segment_ms;   /* running time in the current segment */
    uint32_t total_ms;     /* running time across the program */
    bool running;
    bool done;
} talk_timer_t;

static uint32_t segment_duration_ms(const timer_segment_t *segment)
{
    return (uint32_t)segment->minutes * MS_PER_SEGMENT_UNIT;
}

/* Loads a program paused at its first segment; NULL clears the timer. */
static void timer_load(talk_timer_t *timer, const timer_program_t *program)
{
    timer->program = program;
    timer->index = 0U;
    timer->segment_ms = 0U;
    timer->total_ms = 0U;
    timer->running = false;
    timer->done = program == NULL || program->segment_count == 0U ||
                  program->segments == NULL;
}

static const timer_segment_t *timer_segment(const talk_timer_t *timer)
{
    return &timer->program->segments[timer->index];
}

static bool timer_has_next(const talk_timer_t *timer)
{
    return timer->index + 1U < timer->program->segment_count;
}

static void timer_toggle(talk_timer_t *timer)
{
    if (!timer->done) timer->running = !timer->running;
}

/* Moves to the next segment keeping run/pause state, or finishes. */
static void timer_next_segment(talk_timer_t *timer)
{
    if (timer->done) return;
    if (timer_has_next(timer)) {
        ++timer->index;
        timer->segment_ms = 0U;
    } else {
        timer->done = true;
        timer->running = false;
    }
}

static void timer_tick(talk_timer_t *timer, uint32_t elapsed_ms)
{
    if (!timer->running || timer->done) return;
    timer->segment_ms += elapsed_ms;
    timer->total_ms += elapsed_ms;
    if (!timer->program->auto_advance) return; /* manual: overtime counts up */

    /* Auto advance: carry any overshoot into the following segment(s). */
    for (;;) {
        uint32_t duration = segment_duration_ms(timer_segment(timer));
        if (timer->segment_ms < duration) return;
        timer->segment_ms -= duration;
        if (timer_has_next(timer)) {
            ++timer->index;
        } else {
            timer->total_ms -= timer->segment_ms; /* stop exactly at the end */
            timer->segment_ms = 0U;
            timer->done = true;
            timer->running = false;
            return;
        }
    }
}

/* Countdown text for the current segment; returns true when in overtime. */
static bool timer_format_clock(const talk_timer_t *timer, char out[TEXT_MAX])
{
    uint32_t duration = segment_duration_ms(timer_segment(timer));
    if (timer->segment_ms >= duration) {
        format_mmss(out, TEXT_MAX, '+', (timer->segment_ms - duration) / 1000U);
        return true;
    }
    /* Round up: reads 05:00 at the start and 00:00 only at the end. */
    format_mmss(out, TEXT_MAX, '\0', (duration - timer->segment_ms + 999U) / 1000U);
    return false;
}

/* ------------------------------------------------------------------------ */
/* Reusable UI helpers: metrics, panels, and change-only labels.            */
/* ------------------------------------------------------------------------ */

#define SHADE_BRIGHT 0xFFU
#define SHADE_NORMAL 0xC0U
#define SHADE_SOFT   0x90U
#define SHADE_HINT   0x80U
#define SHADE_DIM    0x50U
#define SHADE_BLACK  0x00U

typedef struct {
    const gm_plugin_lvgl_api_t *ui;
    const gm_plugin_lvgl_font_t *small;
    const gm_plugin_lvgl_font_t *large;
    gm_plugin_lvgl_coord_t small_h;
    gm_plugin_lvgl_coord_t large_h;
    gm_plugin_lvgl_coord_t gap;       /* half a small line */
    gm_plugin_lvgl_coord_t margin;    /* one small line */
    gm_plugin_lvgl_coord_t width;
    gm_plugin_lvgl_coord_t height;
    gm_plugin_lvgl_coord_t content_w; /* width minus both margins */
} ui_metrics_t;

typedef struct {
    gm_plugin_lvgl_obj_t *obj;
    char shown[TEXT_MAX];
    int16_t shade;     /* -1 = not applied yet */
    int8_t highlight;  /* -1 = not applied yet */
} ui_label_t;

static void ui_style(const ui_metrics_t *m, gm_plugin_lvgl_obj_t *object,
                     gm_plugin_lvgl_style_prop_t property,
                     gm_plugin_lvgl_style_value_t value)
{
    m->ui->style_set(object, property, value, GM_PLUGIN_LVGL_SELECTOR_MAIN);
}

static gm_plugin_result_t ui_metrics_init(ui_metrics_t *m,
                                          const gm_plugin_host_api_t *host)
{
    gm_plugin_display_info_t display;
    m->ui = host->graphics.lvgl;
    m->small = m->ui->font_default;
    m->large = m->ui->font_large;
    if (host->display_get_info(&display) != GM_PLUGIN_OK) return GM_PLUGIN_ESTATE;
    m->small_h = m->ui->font_get_line_height(m->small);
    m->large_h = m->ui->font_get_line_height(m->large);
    m->gap = m->small_h / 2;
    m->margin = m->small_h;
    m->width = (gm_plugin_lvgl_coord_t)display.width;
    m->height = (gm_plugin_lvgl_coord_t)display.height;
    m->content_w = m->width - 2 * m->margin;
    if (m->small_h <= 0 || m->large_h <= 0 || m->content_w <= 0 ||
        m->height < 4 * m->small_h)
        return GM_PLUGIN_ESTATE;
    return GM_PLUGIN_OK;
}

/* One-line width of text in a Host font. */
static gm_plugin_lvgl_coord_t ui_text_width(const ui_metrics_t *m,
                                            const gm_plugin_lvgl_font_t *font,
                                            const char *text,
                                            gm_plugin_lvgl_coord_t *height)
{
    gm_plugin_lvgl_point_t size;
    m->ui->text_get_size(&size, text, font, 0, 0, m->width * 8,
                         GM_PLUGIN_LVGL_TEXT_FLAG_NONE);
    if (height != NULL) *height = size.y;
    return size.x;
}

/* Returns the long text if it fits on one line of the content width. */
static const char *ui_pick_fitting(const ui_metrics_t *m, const char *long_text,
                                   const char *short_text)
{
    return ui_text_width(m, m->small, long_text, NULL) <= m->content_w
        ? long_text : short_text;
}

/* Full-canvas, black, non-scrolling container, created hidden. */
static gm_plugin_lvgl_obj_t *ui_panel_create(const ui_metrics_t *m,
                                             gm_plugin_lvgl_obj_t *parent)
{
    gm_plugin_lvgl_obj_t *panel = m->ui->obj_create(parent);
    if (panel == NULL) return NULL;
    m->ui->obj_set_size(panel, m->width, m->height);
    m->ui->obj_align(panel, GM_PLUGIN_LVGL_ALIGN_CENTER, 0, 0);
    ui_style(m, panel, GM_PLUGIN_LVGL_STYLE_BG_COLOR,
             gm_plugin_lvgl_style_color(SHADE_BLACK));
    ui_style(m, panel, GM_PLUGIN_LVGL_STYLE_BG_OPA,
             gm_plugin_lvgl_style_number(GM_PLUGIN_LVGL_OPA_COVER));
    ui_style(m, panel, GM_PLUGIN_LVGL_STYLE_BORDER_WIDTH,
             gm_plugin_lvgl_style_number(0));
    ui_style(m, panel, GM_PLUGIN_LVGL_STYLE_PAD_TOP, gm_plugin_lvgl_style_number(0));
    ui_style(m, panel, GM_PLUGIN_LVGL_STYLE_PAD_BOTTOM, gm_plugin_lvgl_style_number(0));
    ui_style(m, panel, GM_PLUGIN_LVGL_STYLE_PAD_LEFT, gm_plugin_lvgl_style_number(0));
    ui_style(m, panel, GM_PLUGIN_LVGL_STYLE_PAD_RIGHT, gm_plugin_lvgl_style_number(0));
    m->ui->obj_clear_flag(panel, GM_PLUGIN_LVGL_FLAG_SCROLLABLE);
    m->ui->obj_add_flag(panel, GM_PLUGIN_LVGL_FLAG_HIDDEN);
    return panel;
}

static bool ui_label_create(const ui_metrics_t *m, gm_plugin_lvgl_obj_t *parent,
                            ui_label_t *label, const gm_plugin_lvgl_font_t *font,
                            gm_plugin_lvgl_coord_t width,
                            gm_plugin_lvgl_coord_t height)
{
    label->shown[0] = '\0';
    label->shade = -1;
    label->highlight = -1;
    label->obj = m->ui->label_create(parent);
    if (label->obj == NULL) return false;
    m->ui->label_set_text(label->obj, "");
    m->ui->label_set_long_mode(label->obj, GM_PLUGIN_LVGL_LABEL_DOT);
    m->ui->obj_set_size(label->obj, width, height);
    ui_style(m, label->obj, GM_PLUGIN_LVGL_STYLE_TEXT_FONT,
             (gm_plugin_lvgl_style_value_t){ .ptr = font });
    ui_style(m, label->obj, GM_PLUGIN_LVGL_STYLE_TEXT_ALIGN,
             gm_plugin_lvgl_style_number(GM_PLUGIN_LVGL_TEXT_ALIGN_CENTER));
    return true;
}

static void ui_label_set_text(const ui_metrics_t *m, ui_label_t *label,
                              const char *text)
{
    uint32_t n = 0U;
    if (label->obj == NULL) return;
    if (text == NULL) text = "";
    if (text_equal(label->shown, text)) return;
    text_append(label->shown, TEXT_MAX, &n, text);
    m->ui->label_set_text(label->obj, label->shown);
}

static void ui_label_set_shade(const ui_metrics_t *m, ui_label_t *label,
                               uint8_t shade)
{
    if (label->obj == NULL || label->shade == (int16_t)shade) return;
    label->shade = (int16_t)shade;
    ui_style(m, label->obj, GM_PLUGIN_LVGL_STYLE_TEXT_COLOR,
             gm_plugin_lvgl_style_color(shade));
}

/* Inverse video for a focused list row. */
static void ui_label_set_highlight(const ui_metrics_t *m, ui_label_t *label,
                                   bool on)
{
    if (label->obj == NULL || label->highlight == (int8_t)on) return;
    label->highlight = (int8_t)on;
    ui_style(m, label->obj, GM_PLUGIN_LVGL_STYLE_BG_COLOR,
             gm_plugin_lvgl_style_color(SHADE_BRIGHT));
    ui_style(m, label->obj, GM_PLUGIN_LVGL_STYLE_BG_OPA,
             gm_plugin_lvgl_style_number(on ? GM_PLUGIN_LVGL_OPA_COVER
                                            : GM_PLUGIN_LVGL_OPA_TRANSPARENT));
    ui_label_set_shade(m, label, on ? SHADE_BLACK : SHADE_NORMAL);
}

static void ui_set_visible(const ui_metrics_t *m, gm_plugin_lvgl_obj_t *object,
                           bool visible)
{
    if (object == NULL) return;
    if (visible) m->ui->obj_clear_flag(object, GM_PLUGIN_LVGL_FLAG_HIDDEN);
    else m->ui->obj_add_flag(object, GM_PLUGIN_LVGL_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------------ */
/* Menu screen: scrolling list with one highlighted row.                    */
/* ------------------------------------------------------------------------ */

#define MENU_MAX_ROWS 8U

typedef struct {
    gm_plugin_lvgl_obj_t *panel;
    ui_label_t title;
    ui_label_t rows[MENU_MAX_ROWS];
    ui_label_t hint;
    uint32_t row_count;   /* rows that fit and are created */
    uint32_t first;       /* item index shown in row 0 */
    uint32_t focus;       /* highlighted item index */
} menu_screen_t;

static gm_plugin_result_t menu_create(menu_screen_t *menu, const ui_metrics_t *m,
                                      gm_plugin_lvgl_obj_t *root,
                                      uint32_t item_count, const char *hint)
{
    gm_plugin_lvgl_coord_t row_h = m->small_h + m->gap;
    gm_plugin_lvgl_coord_t rows_top = m->gap + m->small_h + m->gap;
    gm_plugin_lvgl_coord_t hint_band = m->small_h + m->gap;
    gm_plugin_lvgl_coord_t rows_avail = m->height - rows_top - hint_band;
    uint32_t fit, i;

    menu->row_count = 0U;
    menu->first = 0U;
    menu->panel = ui_panel_create(m, root);
    if (menu->panel == NULL) return GM_PLUGIN_ENOMEM;

    fit = rows_avail > row_h ? (uint32_t)(rows_avail / row_h) : 1U;
    if (fit > MENU_MAX_ROWS) fit = MENU_MAX_ROWS;
    if (fit > item_count) fit = item_count;

    if (!ui_label_create(m, menu->panel, &menu->title, m->small, m->content_w,
                         m->small_h) ||
        !ui_label_create(m, menu->panel, &menu->hint, m->small, m->content_w,
                         m->small_h))
        return GM_PLUGIN_ENOMEM;
    m->ui->obj_align(menu->title.obj, GM_PLUGIN_LVGL_ALIGN_TOP_MID, 0, m->gap);
    m->ui->obj_align(menu->hint.obj, GM_PLUGIN_LVGL_ALIGN_BOTTOM_MID, 0, -m->gap);
    ui_label_set_shade(m, &menu->title, SHADE_SOFT);
    ui_label_set_text(m, &menu->title, "Choose a program");
    ui_label_set_shade(m, &menu->hint, SHADE_HINT);
    ui_label_set_text(m, &menu->hint, hint);

    for (i = 0U; i < fit; ++i) {
        ui_label_t *row = &menu->rows[i];
        if (!ui_label_create(m, menu->panel, row, m->small, m->content_w, row_h))
            return GM_PLUGIN_ENOMEM;
        ui_style(m, row->obj, GM_PLUGIN_LVGL_STYLE_PAD_TOP,
                 gm_plugin_lvgl_style_number((row_h - m->small_h) / 2));
        ui_style(m, row->obj, GM_PLUGIN_LVGL_STYLE_RADIUS,
                 gm_plugin_lvgl_style_number(m->gap / 2));
        m->ui->obj_align(row->obj, GM_PLUGIN_LVGL_ALIGN_TOP_MID, 0,
                         rows_top + (gm_plugin_lvgl_coord_t)i * row_h);
        menu->row_count = i + 1U;
    }
    return GM_PLUGIN_OK;
}

static void menu_render(menu_screen_t *menu, const ui_metrics_t *m,
                        const timer_program_t *programs, uint32_t count)
{
    uint32_t r;
    if (menu->row_count == 0U) return;
    /* Scroll so the highlighted item is always visible. */
    if (menu->focus < menu->first) menu->first = menu->focus;
    if (menu->focus >= menu->first + menu->row_count)
        menu->first = menu->focus + 1U - menu->row_count;
    for (r = 0U; r < menu->row_count; ++r) {
        uint32_t item = menu->first + r;
        ui_label_set_text(m, &menu->rows[r], item < count ? programs[item].name : "");
        ui_label_set_highlight(m, &menu->rows[r], item == menu->focus);
    }
}

/* ------------------------------------------------------------------------ */
/* Card screen: NORMAL timer and Done share this layout.                    */
/*   title / BIG / status / next  (centered block)   hint (bottom)          */
/* ------------------------------------------------------------------------ */

typedef struct {
    const char *title;
    const char *big;
    const char *status;
    const char *next;
    const char *hint;
    bool big_dimmed;
} card_view_t;

typedef struct {
    gm_plugin_lvgl_obj_t *panel;
    ui_label_t title, big, status, next, hint;
} card_screen_t;

static gm_plugin_result_t card_create(card_screen_t *card, const ui_metrics_t *m,
                                      gm_plugin_lvgl_obj_t *root,
                                      const char *widest_big)
{
    gm_plugin_lvgl_coord_t big_h = m->large_h;
    gm_plugin_lvgl_coord_t big_w, measured_h, block_h, hint_band, top, y;

    card->panel = ui_panel_create(m, root);
    if (card->panel == NULL) return GM_PLUGIN_ENOMEM;

    big_w = ui_text_width(m, m->large, widest_big, &measured_h) + m->small_h;
    if (big_w > m->content_w) big_w = m->content_w;
    if (measured_h > big_h) big_h = measured_h;

    block_h = m->small_h + m->gap + big_h + m->gap + m->small_h + m->gap +
              m->small_h;
    hint_band = m->small_h + m->gap;
    top = (m->height - hint_band - block_h) / 2;
    if (top < 0) top = 0;

    if (!ui_label_create(m, card->panel, &card->title, m->small, m->content_w, m->small_h) ||
        !ui_label_create(m, card->panel, &card->big, m->large, big_w, big_h) ||
        !ui_label_create(m, card->panel, &card->status, m->small, m->content_w, m->small_h) ||
        !ui_label_create(m, card->panel, &card->next, m->small, m->content_w, m->small_h) ||
        !ui_label_create(m, card->panel, &card->hint, m->small, m->content_w, m->small_h))
        return GM_PLUGIN_ENOMEM;

    m->ui->label_set_long_mode(card->big.obj, GM_PLUGIN_LVGL_LABEL_CLIP);
    y = top;
    m->ui->obj_align(card->title.obj, GM_PLUGIN_LVGL_ALIGN_TOP_MID, 0, y);
    y += m->small_h + m->gap;
    m->ui->obj_align(card->big.obj, GM_PLUGIN_LVGL_ALIGN_TOP_MID, 0, y);
    y += big_h + m->gap;
    m->ui->obj_align(card->status.obj, GM_PLUGIN_LVGL_ALIGN_TOP_MID, 0, y);
    y += m->small_h + m->gap;
    m->ui->obj_align(card->next.obj, GM_PLUGIN_LVGL_ALIGN_TOP_MID, 0, y);
    m->ui->obj_align(card->hint.obj, GM_PLUGIN_LVGL_ALIGN_BOTTOM_MID, 0, -m->gap);

    ui_label_set_shade(m, &card->title, SHADE_NORMAL);
    ui_label_set_shade(m, &card->status, SHADE_BRIGHT);
    ui_label_set_shade(m, &card->next, SHADE_SOFT);
    ui_label_set_shade(m, &card->hint, SHADE_HINT);
    return GM_PLUGIN_OK;
}

static void card_render(card_screen_t *card, const ui_metrics_t *m,
                        const card_view_t *view)
{
    ui_label_set_text(m, &card->title, view->title);
    ui_label_set_text(m, &card->big, view->big);
    ui_label_set_shade(m, &card->big, view->big_dimmed ? SHADE_DIM : SHADE_BRIGHT);
    ui_label_set_text(m, &card->status, view->status);
    ui_label_set_text(m, &card->next, view->next);
    ui_label_set_text(m, &card->hint, view->hint);
}

/* ------------------------------------------------------------------------ */
/* Quiet screen: one small countdown low and to the side.                   */
/* ------------------------------------------------------------------------ */

typedef struct {
    gm_plugin_lvgl_obj_t *panel;
    ui_label_t clock;
} quiet_screen_t;

static gm_plugin_result_t quiet_create(quiet_screen_t *quiet, const ui_metrics_t *m,
                                       gm_plugin_lvgl_obj_t *root,
                                       const char *widest_clock)
{
    gm_plugin_lvgl_coord_t h, w;
    quiet->panel = ui_panel_create(m, root);
    if (quiet->panel == NULL) return GM_PLUGIN_ENOMEM;
    w = ui_text_width(m, m->small, widest_clock, &h) + m->gap;
    if (w > m->content_w) w = m->content_w;
    if (h < m->small_h) h = m->small_h;
    if (!ui_label_create(m, quiet->panel, &quiet->clock, m->small, w, h))
        return GM_PLUGIN_ENOMEM;
    m->ui->label_set_long_mode(quiet->clock.obj, GM_PLUGIN_LVGL_LABEL_CLIP);
    /* Bottom-right, a full margin in from the side and half a line up:
     * entirely inside the canvas and away from the wearer's center view. */
    m->ui->obj_align(quiet->clock.obj, GM_PLUGIN_LVGL_ALIGN_BOTTOM_RIGHT,
                     -m->margin, -m->gap);
    return GM_PLUGIN_OK;
}

static void quiet_render(quiet_screen_t *quiet, const ui_metrics_t *m,
                         const char *clock, bool dimmed)
{
    ui_label_set_text(m, &quiet->clock, clock);
    ui_label_set_shade(m, &quiet->clock, dimmed ? SHADE_DIM : SHADE_SOFT);
}

/* ------------------------------------------------------------------------ */
/* Application: screen routing, refresh, and lifecycle.                     */
/* ------------------------------------------------------------------------ */

typedef enum {
    SCREEN_NONE = 0,   /* not started: no screen receives input */
    SCREEN_MENU,
    SCREEN_TIMER,
    SCREEN_DONE,
} screen_id_t;

typedef struct {
    const gm_plugin_host_api_t *host;
    example_call_ui_t call_ui;
    ui_metrics_t metrics;

    /* Program source. Replace these two fields to supply external programs. */
    const timer_program_t *programs;
    uint32_t program_count;

    menu_screen_t menu;
    card_screen_t card;
    quiet_screen_t quiet;
    ui_label_t dev_marker;
    gm_plugin_lvgl_obj_t *visible_panel;
    screen_id_t screen;

    talk_timer_t timer;
    char clock_text[TEXT_MAX];
    char next_text[TEXT_MAX];
    const char *hint_timer;
    const char *hint_done;
} talk_app_t;

static talk_app_t s_app;

static void app_show_panel(talk_app_t *app, gm_plugin_lvgl_obj_t *panel)
{
    if (app->visible_panel == panel) return;
    ui_set_visible(&app->metrics, app->visible_panel, false);
    ui_set_visible(&app->metrics, panel, true);
    app->visible_panel = panel;
}

static void app_render_timer(talk_app_t *app)
{
    talk_timer_t *timer = &app->timer;
    const timer_segment_t *segment = timer_segment(timer);
    bool overtime = timer_format_clock(timer, app->clock_text);
    uint32_t n = 0U;
    card_view_t view;

    if (segment->style == SEGMENT_QUIET) {
        app_show_panel(app, app->quiet.panel);
        quiet_render(&app->quiet, &app->metrics, app->clock_text, !timer->running);
        return;
    }

    app->next_text[0] = '\0';
    if (timer_has_next(timer)) {
        text_append(app->next_text, TEXT_MAX, &n, "Next: ");
        text_append(app->next_text, TEXT_MAX, &n,
                    timer->program->segments[timer->index + 1U].label);
    } else {
        text_append(app->next_text, TEXT_MAX, &n, "Last segment");
    }

    view.title = segment->label;
    view.big = app->clock_text;
    if (!timer->running) view.status = overtime ? "Time - Paused" : "Paused";
    else view.status = overtime ? "Time" : "";
    view.next = app->next_text;
    view.hint = app->hint_timer;
    view.big_dimmed = !timer->running;
    app_show_panel(app, app->card.panel);
    card_render(&app->card, &app->metrics, &view);
}

static void app_render_done(talk_app_t *app)
{
    card_view_t view;
    format_mmss(app->clock_text, TEXT_MAX, '\0', app->timer.total_ms / 1000U);
    view.title = "Done";
    view.big = app->clock_text;
    view.status = "Total elapsed";
    view.next = app->timer.program != NULL ? app->timer.program->name : "";
    view.hint = app->hint_done;
    view.big_dimmed = false;
    app_show_panel(app, app->card.panel);
    card_render(&app->card, &app->metrics, &view);
}

/* Redraws the active screen; only changed labels are touched. */
static void app_refresh(talk_app_t *app)
{
    switch (app->screen) {
    case SCREEN_MENU:
        app_show_panel(app, app->menu.panel);
        menu_render(&app->menu, &app->metrics, app->programs, app->program_count);
        break;
    case SCREEN_TIMER:
        app_render_timer(app);
        break;
    case SCREEN_DONE:
        app_render_done(app);
        break;
    default:
        break;
    }
}

/* Switches screens, keeping focus and timer state valid for the target. */
static void app_go(talk_app_t *app, screen_id_t screen)
{
    if (screen == SCREEN_MENU || app->timer.program == NULL) {
        if (app->menu.focus >= app->program_count) app->menu.focus = 0U;
        timer_load(&app->timer, NULL);
        screen = SCREEN_MENU;
    } else if (screen == SCREEN_TIMER && app->timer.done) {
        screen = SCREEN_DONE;
    }
    app->screen = screen;
    app_refresh(app);
}

static void menu_on_action(talk_app_t *app, ui_action_t action)
{
    if (app->program_count == 0U) return;
    switch (action) {
    case ACTION_NEXT:
        app->menu.focus = (app->menu.focus + 1U) % app->program_count;
        app_refresh(app);
        break;
    case ACTION_SELECT:
        timer_load(&app->timer, &app->programs[app->menu.focus]);
        app_go(app, SCREEN_TIMER);
        break;
    default:
        break;
    }
}

static void timer_on_action(talk_app_t *app, ui_action_t action)
{
    switch (action) {
    case ACTION_NEXT:
        timer_toggle(&app->timer);
        app_refresh(app);
        break;
    case ACTION_SELECT:
        timer_next_segment(&app->timer);
        app_go(app, app->timer.done ? SCREEN_DONE : SCREEN_TIMER);
        break;
    case ACTION_BACK:
        app_go(app, SCREEN_MENU);
        break;
    default:
        break;
    }
}

static void done_on_action(talk_app_t *app, ui_action_t action)
{
    switch (action) {
    case ACTION_NEXT: /* restart the same program, running */
        timer_load(&app->timer, app->timer.program);
        timer_toggle(&app->timer);
        app_go(app, SCREEN_TIMER);
        break;
    case ACTION_SELECT:
        app_go(app, SCREEN_MENU);
        break;
    default:
        break;
    }
}

static bool app_event(void *context, const gm_plugin_event_t *event)
{
    talk_app_t *app = context;
    ui_action_t action = action_from_event(event);

    if (action == ACTION_NONE || app->screen == SCREEN_NONE) return false;
    if (action == ACTION_BACK && app->screen != SCREEN_TIMER) {
        app->host->app_exit(); /* asynchronous; no further UI work */
        return true;
    }
    switch (app->screen) {
    case SCREEN_MENU:  menu_on_action(app, action); break;
    case SCREEN_TIMER: timer_on_action(app, action); break;
    case SCREEN_DONE:  done_on_action(app, action); break;
    default: break;
    }
    return true;
}

static void app_loop(void *context, uint32_t elapsed_ms)
{
    talk_app_t *app = context;
    if (app->screen != SCREEN_TIMER || !app->timer.running) return;
    timer_tick(&app->timer, elapsed_ms);
    if (app->timer.done) app_go(app, SCREEN_DONE);
    else app_refresh(app);
}

/* Deletes every object under the Host root and forgets all handles. */
static void app_release_ui(talk_app_t *app)
{
    gm_plugin_lvgl_obj_t *root = app->metrics.ui != NULL
        ? app->metrics.ui->root_get() : NULL;
    if (root != NULL) app->metrics.ui->obj_clean(root);
    app->menu.panel = NULL;
    app->menu.row_count = 0U;
    app->card.panel = NULL;
    app->quiet.panel = NULL;
    app->dev_marker.obj = NULL;
    app->visible_panel = NULL;
    app->screen = SCREEN_NONE;
    timer_load(&app->timer, NULL);
}

static gm_plugin_result_t app_build_ui(talk_app_t *app)
{
    const ui_metrics_t *m = &app->metrics;
    gm_plugin_lvgl_obj_t *root = m->ui->root_get();
    gm_plugin_result_t result;

    if (root == NULL) return GM_PLUGIN_ESTATE;
    m->ui->obj_clean(root);

    app->hint_timer = ui_pick_fitting(m, "Click: start/pause   Double: next   Hold: menu",
                                      "1x run/pause  2x next  hold menu");
    app->hint_done = ui_pick_fitting(m, "Click: restart   Double: menu   Hold: exit",
                                     "1x restart  2x menu  hold exit");

    result = menu_create(&app->menu, m, root, app->program_count,
                         ui_pick_fitting(m, "Click: move   Double: open   Hold: exit",
                                         "1x move  2x open  hold exit"));
    if (result == GM_PLUGIN_OK)
        result = card_create(&app->card, m, root, "+000:00");
    if (result == GM_PLUGIN_OK)
        result = quiet_create(&app->quiet, m, root, "+000:00");
#if DEV_FAST_TIME
    if (result == GM_PLUGIN_OK) {
        gm_plugin_lvgl_coord_t h;
        gm_plugin_lvgl_coord_t w = ui_text_width(m, m->small, "DEV", &h) + m->gap;
        if (h < m->small_h) h = m->small_h;
        /* Created last so it stays above every panel, including QUIET. */
        if (!ui_label_create(m, root, &app->dev_marker, m->small, w, h)) {
            result = GM_PLUGIN_ENOMEM;
        } else {
            m->ui->obj_align(app->dev_marker.obj, GM_PLUGIN_LVGL_ALIGN_TOP_RIGHT,
                             -m->gap, m->gap / 2);
            ui_label_set_highlight(m, &app->dev_marker, true);
            ui_label_set_text(m, &app->dev_marker, "DEV");
        }
    }
#endif
    return result;
}

static gm_plugin_result_t app_start(void *context)
{
    talk_app_t *app = context;
    gm_plugin_result_t result = example_call_ui_start(&app->call_ui);
    if (result != GM_PLUGIN_OK) return result;

    result = ui_metrics_init(&app->metrics, app->host);
    if (result == GM_PLUGIN_OK) result = app_build_ui(app);
    if (result != GM_PLUGIN_OK) {
        /* on_stop is not called after a failed start: clean up here. */
        app->host->log("talk_timer: start failed (%d)", (int)result);
        app_release_ui(app);
        example_call_ui_stop(&app->call_ui);
        return result;
    }
    app->visible_panel = NULL;
    app->menu.focus = 0U;
    app->menu.first = 0U;
    timer_load(&app->timer, NULL);
    app_go(app, SCREEN_MENU);
    return GM_PLUGIN_OK;
}

static void app_stop(void *context)
{
    talk_app_t *app = context;
    app_release_ui(app);
    example_call_ui_stop(&app->call_ui);
}

gm_plugin_result_t gm_plugin_entry(const gm_plugin_host_api_t *host,
                                   gm_plugin_descriptor_t *plugin)
{
    const gm_plugin_lvgl_api_t *ui;

    if (host == NULL || plugin == NULL ||
        plugin->struct_size < GM_PLUGIN_DESCRIPTOR_MIN_SIZE)
        return GM_PLUGIN_EINVAL;
    if (host->struct_size < GM_PLUGIN_HOST_API_MIN_SIZE ||
        !GM_PLUGIN_VERSION_COMPATIBLE(host->abi_version,
                                      GM_PLUGIN_ABI_MIN_VERSION))
        return GM_PLUGIN_EVERSION;
    if ((host->capabilities & GM_PLUGIN_CAP_BUTTON) == 0U ||
        host->log == NULL || host->display_get_info == NULL ||
        host->app_exit == NULL || host->graphics.lvgl == NULL)
        return GM_PLUGIN_ENOTSUP;

    ui = host->graphics.lvgl;
    if (ui->struct_size < GM_PLUGIN_LVGL_API_MIN_SIZE ||
        !GM_PLUGIN_VERSION_COMPATIBLE(ui->api_version,
                                      GM_PLUGIN_LVGL_API_MIN_VERSION))
        return GM_PLUGIN_EVERSION;
    if (ui->root_get == NULL || ui->obj_create == NULL ||
        ui->obj_clean == NULL || ui->obj_set_size == NULL ||
        ui->obj_align == NULL || ui->obj_add_flag == NULL ||
        ui->obj_clear_flag == NULL || ui->style_set == NULL ||
        ui->label_create == NULL || ui->label_set_text == NULL ||
        ui->label_set_long_mode == NULL || ui->font_default == NULL ||
        ui->font_large == NULL || ui->font_get_line_height == NULL ||
        ui->text_get_size == NULL)
        return GM_PLUGIN_ENOTSUP;

    s_app.host = host;
    s_app.call_ui.host = host;
    s_app.programs = k_programs;
    s_app.program_count = COUNT_OF(k_programs);
    s_app.screen = SCREEN_NONE;
    plugin->abi_version = GM_PLUGIN_ABI_MIN_VERSION;
    plugin->context = &s_app;
    plugin->on_start = app_start;
    plugin->on_loop = app_loop;
    plugin->on_event = app_event;
    plugin->on_stop = app_stop;
    return GM_PLUGIN_OK;
}
