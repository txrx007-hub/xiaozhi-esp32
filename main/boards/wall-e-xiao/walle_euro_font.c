/* The euro sign (U+20AC) for the scrolling ticker: a one-glyph 4 bpp font drawn from Segoe UI at 14 px by setup tools (not in Montserrat 14 or Noto Sans). Every other character
 * comes from the fallback, Montserrat 14, the ticker's normal font. Generated; see walle_display.cc (CreateTicker). */
#include "lvgl.h"

static const uint8_t glyph_bitmap[] = { 0x00, 0x18, 0xde, 0x70, 0x0a, 0xb2, 0x18, 0x03, 0xe1, 0x00, 0x04, 0xff, 0xff, 0xf2, 0x09, 0x90, 0x00, 0x04, 0xff, 0xff, 0xf2, 0x05, 0xc0, 0x00, 0x00, 0x2f, 0x20, 0x00, 0x00, 0x9c, 0x22, 0x90, 0x01, 0x9e, 0xd6 };

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 131, .box_w = 7, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
};

static const lv_font_fmt_txt_cmap_t cmaps[] = {
    {.range_start = 0x20AC, .range_length = 1, .glyph_id_start = 1, .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY},
};

static const lv_font_fmt_txt_dsc_t font_dsc = {
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
};

const lv_font_t walle_euro_font = {
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = 16,
    .base_line = 3,
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = -1,
    .underline_thickness = 1,
    .dsc = &font_dsc,
    .fallback = &lv_font_montserrat_14,
    .user_data = NULL,
};
