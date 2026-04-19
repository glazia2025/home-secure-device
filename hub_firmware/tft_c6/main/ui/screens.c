#include <string.h>

#include "screens.h"
#include "images.h"
#include "fonts.h"
#include "actions.h"
#include "vars.h"
#include "styles.h"
#include "ui.h"

objects_t objects;

static const char *screen_names[] = { "Main" };
static const char *object_names[] = { "main", "toggle", "status_label" };

//
// Event handlers
//

lv_obj_t *tick_value_change_obj;

//
// Screens
//
// Layout for 240 x 320 ILI9341 (portrait).
// Three sections:
//   [0]  Header panel  — "Glazia Hub"  (y: 8..65)
//   [1]  Status panel  — log text      (y: 73..258)
//   [2]  Critical btn  — display only  (y: 268..310)
//

void create_screen_main() {
    void *flowState = getFlowState(0, 0);
    (void)flowState;

    // Root screen object
    lv_obj_t *obj = lv_obj_create(0);
    objects.main = obj;
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, 240, 320);
    lv_obj_set_style_bg_color(obj, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER,  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 0,        LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(obj, 0,             LV_PART_MAIN | LV_STATE_DEFAULT);

    {
        lv_obj_t *parent_obj = obj;

        // ── Header panel ──────────────────────────────────────────────
        {
            lv_obj_t *panel = lv_obj_create(parent_obj);
            lv_obj_set_pos(panel, 8, 8);
            lv_obj_set_size(panel, 224, 57);
            lv_obj_set_style_bg_color(panel, lv_color_make(18, 18, 48), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(panel, lv_color_make(80, 80, 200), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(panel, 1,  LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(panel, 8,        LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_all(panel, 0,       LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
        }
        {
            lv_obj_t *lbl = lv_label_create(parent_obj);
            lv_obj_set_pos(lbl, 0, 8);
            lv_obj_set_size(lbl, 240, 57);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_align(lbl, LV_ALIGN_DEFAULT, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_label_set_text(lbl, "Glazia Hub");
        }

        // ── Status panel ──────────────────────────────────────────────
        {
            lv_obj_t *panel = lv_obj_create(parent_obj);
            lv_obj_set_pos(panel, 8, 73);
            lv_obj_set_size(panel, 224, 185);
            lv_obj_set_style_bg_color(panel, lv_color_make(14, 14, 14), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(panel, lv_color_make(60, 60, 60), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(panel, 1,  LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(panel, 6,        LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_all(panel, 8,       LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

            {
                lv_obj_t *lbl = lv_label_create(panel);
                objects.status_label = lbl;
                lv_label_set_text(lbl, "Starting...");
                lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
                lv_obj_set_width(lbl, 208);
                lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
            }
        }

        // ── Critical button (display-only, no callback) ───────────────
        {
            lv_obj_t *btn = lv_btn_create(parent_obj);
            objects.toggle = btn;
            lv_obj_set_pos(btn, 44, 268);
            lv_obj_set_size(btn, 152, 44);
            lv_obj_set_style_bg_color(btn, lv_color_make(160, 30, 30), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(btn, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);   // no interaction

            {
                lv_obj_t *parent_obj = btn;
                {
                    lv_obj_t *lbl = lv_label_create(parent_obj);
                    lv_obj_set_pos(lbl, 0, 0);
                    lv_obj_set_size(lbl, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                    lv_obj_set_style_align(lbl, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(lbl, "Critical Toggle");
                }
            }
        }
    }

    tick_screen_main();
}

void tick_screen_main() {
    void *flowState = getFlowState(0, 0);
    (void)flowState;
}

typedef void (*tick_screen_func_t)();
tick_screen_func_t tick_screen_funcs[] = {
    tick_screen_main,
};
void tick_screen(int screen_index) {
    tick_screen_funcs[screen_index]();
}
void tick_screen_by_id(enum ScreensEnum screenId) {
    tick_screen_funcs[screenId - 1]();
}

//
// Styles
//

static const char *style_names[] = { "local_style" };

extern void add_style(lv_obj_t *obj, int32_t styleIndex);
extern void remove_style(lv_obj_t *obj, int32_t styleIndex);

//
// Fonts
//

ext_font_desc_t fonts[] = {
#if LV_FONT_MONTSERRAT_14
    { "MONTSERRAT_14", &lv_font_montserrat_14 },
#endif
#if LV_FONT_MONTSERRAT_24
    { "MONTSERRAT_24", &lv_font_montserrat_24 },
#endif
};

//
//
//

void create_screens() {
    // Initialize styles
    eez_flow_init_styles(add_style, remove_style);
    eez_flow_init_style_names(style_names, sizeof(style_names) / sizeof(const char *));

    eez_flow_init_fonts(fonts, sizeof(fonts) / sizeof(ext_font_desc_t));

    // Set default LVGL theme
    lv_disp_t *dispp = lv_disp_get_default();
    lv_theme_t *theme = lv_theme_default_init(dispp,
        lv_palette_main(LV_PALETTE_BLUE),
        lv_palette_main(LV_PALETTE_RED),
        true, LV_FONT_DEFAULT);
    lv_disp_set_theme(dispp, theme);

    // Initialize screens
    eez_flow_init_screen_names(screen_names, sizeof(screen_names) / sizeof(const char *));
    eez_flow_init_object_names(object_names, sizeof(object_names) / sizeof(const char *));

    // Create screens
    create_screen_main();
}
