/*******************************************************************************
 * Size: 9 px
 * Bpp: 1
 * Opts: --bpp 1 --size 9 --stride 1 --align 1 --font Geneva-14.ttf --range 32-126,160-255 --format lvgl -o geneva_9.c
 ******************************************************************************/

#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
    #include "lvgl.h"
#else
    #include "lvgl/lvgl.h"
#endif



#ifndef GENEVA_9
#define GENEVA_9 1
#endif

#if GENEVA_9

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0020 " " */
    0x0,

    /* U+0021 "!" */
    0xff,

    /* U+0022 "\"" */
    0xb4,

    /* U+0023 "#" */
    0x28, 0xa7, 0xfe, 0x61, 0x80,

    /* U+0024 "$" */
    0x5f, 0x24, 0xfa, 0x40,

    /* U+0025 "%" */
    0x5e, 0xba, 0x94, 0x29, 0x55, 0x62,

    /* U+0026 "&" */
    0x20, 0xa1, 0x83, 0x9, 0x51, 0xb3, 0x19,

    /* U+0027 "'" */
    0xe0,

    /* U+0028 "(" */
    0x2a, 0x49, 0x24, 0x44,

    /* U+0029 ")" */
    0x88, 0x92, 0x49, 0x50,

    /* U+002A "*" */
    0x93, 0x3e, 0x8f, 0x0,

    /* U+002B "+" */
    0x21, 0x3e, 0x42, 0x10,

    /* U+002C "," */
    0x58,

    /* U+002D "-" */
    0xf0,

    /* U+002E "." */
    0x80,

    /* U+002F "/" */
    0x12, 0x44, 0x48, 0x80,

    /* U+0030 "0" */
    0x74, 0x63, 0x18, 0xc5, 0x44,

    /* U+0031 "1" */
    0xd5, 0x55,

    /* U+0032 "2" */
    0x74, 0x42, 0x22, 0x13, 0x1f,

    /* U+0033 "3" */
    0xf8, 0x88, 0xe0, 0x86, 0x4c,

    /* U+0034 "4" */
    0x18, 0xa2, 0xb2, 0xfc, 0x20, 0x82,

    /* U+0035 "5" */
    0xfc, 0x21, 0xe0, 0x86, 0x4c,

    /* U+0036 "6" */
    0x64, 0x29, 0xb8, 0xc5, 0x44,

    /* U+0037 "7" */
    0xf8, 0x44, 0x21, 0x10, 0x84,

    /* U+0038 "8" */
    0x22, 0x94, 0xe8, 0xc5, 0x44,

    /* U+0039 "9" */
    0x74, 0x63, 0x15, 0x94, 0x44,

    /* U+003A ":" */
    0x88,

    /* U+003B ";" */
    0x40, 0x16,

    /* U+003C "<" */
    0x5a, 0x50,

    /* U+003D "=" */
    0xf8, 0x3e,

    /* U+003E ">" */
    0xa5, 0xa0,

    /* U+003F "?" */
    0xe9, 0x12, 0x44, 0x44,

    /* U+0040 "@" */
    0x7c, 0x8a, 0xed, 0x5a, 0xba, 0x9c, 0x0,

    /* U+0041 "A" */
    0x10, 0x43, 0x12, 0x49, 0xe8, 0xa1,

    /* U+0042 "B" */
    0xe4, 0xa5, 0xe8, 0xc6, 0x5c,

    /* U+0043 "C" */
    0x7c, 0x21, 0x8, 0x41, 0x26,

    /* U+0044 "D" */
    0xf4, 0x63, 0x18, 0xc6, 0x5c,

    /* U+0045 "E" */
    0xf8, 0x8f, 0x88, 0x8f,

    /* U+0046 "F" */
    0xf8, 0x8f, 0x88, 0x88,

    /* U+0047 "G" */
    0x7a, 0x8, 0x27, 0x86, 0x14, 0x8c,

    /* U+0048 "H" */
    0x8c, 0x63, 0xf8, 0xc6, 0x31,

    /* U+0049 "I" */
    0xff,

    /* U+004A "J" */
    0x8, 0x42, 0x10, 0xc5, 0x44,

    /* U+004B "K" */
    0x8c, 0xa9, 0x8c, 0x52, 0x51,

    /* U+004C "L" */
    0x88, 0x88, 0x88, 0x8f,

    /* U+004D "M" */
    0xcf, 0x5b, 0x69, 0x86, 0x18, 0x61,

    /* U+004E "N" */
    0xc7, 0x1c, 0x69, 0x96, 0x58, 0xe1,

    /* U+004F "O" */
    0x7a, 0x18, 0x61, 0x86, 0x14, 0x8c,

    /* U+0050 "P" */
    0xf4, 0x63, 0x2e, 0x42, 0x10,

    /* U+0051 "Q" */
    0x7a, 0x18, 0x61, 0x86, 0x1c, 0xcd,

    /* U+0052 "R" */
    0xf2, 0x28, 0xa4, 0xe2, 0x89, 0xa1,

    /* U+0053 "S" */
    0x74, 0x60, 0xc1, 0xb, 0x64,

    /* U+0054 "T" */
    0xf9, 0x8, 0x42, 0x10, 0x84,

    /* U+0055 "U" */
    0x86, 0x18, 0x61, 0x86, 0x14, 0x8c,

    /* U+0056 "V" */
    0x85, 0x14, 0x92, 0x30, 0x41, 0x4,

    /* U+0057 "W" */
    0x81, 0x52, 0x52, 0x5c, 0x5c, 0x5c, 0x24, 0x24,

    /* U+0058 "X" */
    0x8a, 0x98, 0x46, 0x2a, 0x51,

    /* U+0059 "Y" */
    0x8a, 0x94, 0xa2, 0x10, 0x84,

    /* U+005A "Z" */
    0xfc, 0x31, 0x8, 0x21, 0xc, 0x3f,

    /* U+005B "[" */
    0xea, 0xaa, 0xb0,

    /* U+005C "\\" */
    0x84, 0x44, 0x22, 0x10,

    /* U+005D "]" */
    0xd5, 0x55, 0x70,

    /* U+005E "^" */
    0xd4,

    /* U+005F "_" */
    0xfc,

    /* U+0060 "`" */
    0x90,

    /* U+0061 "a" */
    0x6f, 0x59, 0xb7,

    /* U+0062 "b" */
    0x84, 0x29, 0xb8, 0xc7, 0x54,

    /* U+0063 "c" */
    0x3d, 0x88, 0x53,

    /* U+0064 "d" */
    0x8, 0x4b, 0xb8, 0xc5, 0x65,

    /* U+0065 "e" */
    0x36, 0x7f, 0x5, 0x18,

    /* U+0066 "f" */
    0x2b, 0xa4, 0x92,

    /* U+0067 "g" */
    0x7c, 0x63, 0x15, 0x94, 0x44,

    /* U+0068 "h" */
    0x88, 0xed, 0x99, 0x99,

    /* U+0069 "i" */
    0x4d, 0x55,

    /* U+006A "j" */
    0x4d, 0x55, 0x60,

    /* U+006B "k" */
    0x84, 0x25, 0x6c, 0x72, 0x51,

    /* U+006C "l" */
    0xd5, 0x55,

    /* U+006D "m" */
    0xed, 0xb6, 0x4c, 0x99, 0x32, 0x40,

    /* U+006E "n" */
    0xed, 0x99, 0x99,

    /* U+006F "o" */
    0x26, 0xe3, 0x15, 0x10,

    /* U+0070 "p" */
    0xf4, 0x63, 0x1d, 0x52, 0x10,

    /* U+0071 "q" */
    0x7c, 0x63, 0x15, 0x94, 0x21,

    /* U+0072 "r" */
    0xbd, 0x88, 0x88,

    /* U+0073 "s" */
    0x69, 0x61, 0x96,

    /* U+0074 "t" */
    0x44, 0xe4, 0x44, 0x52,

    /* U+0075 "u" */
    0x99, 0x99, 0xb7,

    /* U+0076 "v" */
    0x8c, 0x94, 0xa6, 0x10,

    /* U+0077 "w" */
    0xd5, 0x57, 0x56, 0x49, 0x20,

    /* U+0078 "x" */
    0x8a, 0x88, 0xc5, 0x44,

    /* U+0079 "y" */
    0x8c, 0x94, 0xa6, 0x11, 0x8, 0x40,

    /* U+007A "z" */
    0xf1, 0x24, 0x8f,

    /* U+007B "{" */
    0x69, 0x44, 0x92, 0x20,

    /* U+007C "|" */
    0xff, 0x80,

    /* U+007D "}" */
    0xc9, 0x14, 0x92, 0x80,

    /* U+007E "~" */
    0xcd, 0x80,

    /* U+00A0 " " */
    0x5d, 0x20,

    /* U+00A1 "¡" */
    0x55, 0x0,

    /* U+00A2 "¢" */
    0x27, 0x7a, 0xaa, 0x72,

    /* U+00A3 "£" */
    0x72, 0x11, 0xc4, 0x21, 0x3e,

    /* U+00A4 "¤" */
    0x74, 0x51, 0xe2, 0x8a, 0x2e,

    /* U+00A5 "¥" */
    0x77, 0xff, 0xff, 0xb8,

    /* U+00A6 "¦" */
    0x7c, 0xe7, 0x35, 0x9c, 0x63,

    /* U+00A7 "§" */
    0x30, 0xa5, 0x16, 0x45, 0x14, 0xb4,

    /* U+00A8 "¨" */
    0x7a, 0x2f, 0x73, 0xf7, 0x37, 0x80,

    /* U+00A9 "©" */
    0x72, 0x2f, 0x71, 0xd6, 0xf7, 0x0,

    /* U+00AA "ª" */
    0xef, 0x49, 0x49,

    /* U+00AB "«" */
    0xc0,

    /* U+00AC "¬" */
    0xc0,

    /* U+00AD "­" */
    0x10, 0xbe, 0x4f, 0xa1, 0x0,

    /* U+00AE "®" */
    0x1e, 0x61, 0x43, 0xe5, 0xa, 0x14, 0x4f,

    /* U+00AF "¯" */
    0x7e, 0x59, 0x65, 0xa7, 0x18, 0xbc,

    /* U+00B0 "°" */
    0x77, 0x4c, 0x62, 0x2e, 0xe0,

    /* U+00B1 "±" */
    0x21, 0x3e, 0x42, 0x7c,

    /* U+00B2 "²" */
    0x52, 0x2e,

    /* U+00B3 "³" */
    0x8d, 0x4e,

    /* U+00B4 "´" */
    0x8a, 0xbe, 0x4f, 0x90, 0x84,

    /* U+00B5 "µ" */
    0x49, 0x24, 0x92, 0x49, 0xd4, 0x20,

    /* U+00B6 "¶" */
    0x62, 0x17, 0x59, 0xa6,

    /* U+00B7 "·" */
    0xfd, 0x92, 0x6, 0x10, 0x86, 0x7f,

    /* U+00B8 "¸" */
    0xfd, 0x24, 0x92, 0x49, 0x24, 0x92, 0x48,

    /* U+00B9 "¹" */
    0xfd, 0x24, 0x92, 0x4a, 0x40,

    /* U+00BA "º" */
    0x69, 0x24, 0x92, 0x4a, 0x0,

    /* U+00BB "»" */
    0xf7, 0x97, 0xf0,

    /* U+00BC "¼" */
    0x69, 0x99, 0x6f,

    /* U+00BD "½" */
    0x7a, 0x18, 0x61, 0x49, 0x45, 0x16,

    /* U+00BE "¾" */
    0x66, 0x7c, 0xd7, 0xf2, 0xb, 0x93, 0xb0,

    /* U+00BF "¿" */
    0x35, 0x65, 0x9a, 0x6a, 0xc0,

    /* U+00C0 "À" */
    0x22, 0x24, 0x48, 0x97,

    /* U+00C1 "Á" */
    0xbf,

    /* U+00C2 "Â" */
    0xf1, 0x10,

    /* U+00C3 "Ã" */
    0x8, 0x8c, 0x69, 0x30, 0x84,

    /* U+00C4 "Ä" */
    0x29, 0x2e, 0x92, 0x49, 0x40,

    /* U+00C5 "Å" */
    0xf, 0xa3, 0xe8, 0x0,

    /* U+00C6 "Æ" */
    0x10, 0x50, 0xa1, 0x44, 0x50, 0x7f, 0x80,

    /* U+00C7 "Ç" */
    0x5d, 0x31, 0xe4, 0x80,

    /* U+00C8 "È" */
    0xa2, 0x93, 0xec, 0x0,

    /* U+00C9 "É" */
    0xa8,

    /* U+00CA "Ê" */
    0x0,

    /* U+00CB "Ë" */
    0x10, 0x0, 0x4, 0x10, 0xc5, 0x12, 0x89, 0xe8,
    0x40,

    /* U+00CC "Ì" */
    0x31, 0x20, 0x4, 0x10, 0xc5, 0x12, 0x89, 0xe8,
    0x40,

    /* U+00CD "Í" */
    0x29, 0x40, 0x1e, 0x86, 0x18, 0x61, 0x85, 0x23,
    0x0,

    /* U+00CE "Î" */
    0x7f, 0x88, 0x88, 0x8f, 0x88, 0x88, 0x58, 0x2f,

    /* U+00CF "Ï" */
    0x66, 0x4c, 0xe7, 0xf2, 0x9, 0x93, 0x30,

    /* U+00D0 "Ð" */
    0xe0,

    /* U+00D1 "Ñ" */
    0xf8,

    /* U+00D2 "Ò" */
    0x5a, 0xa0,

    /* U+00D3 "Ó" */
    0x55, 0xf0,

    /* U+00D4 "Ô" */
    0x68,

    /* U+00D5 "Õ" */
    0x5c,

    /* U+00D6 "Ö" */
    0x40, 0x3e, 0x80,

    /* U+00D7 "×" */
    0x21, 0x4c, 0xa1, 0x89, 0x62, 0x0,

    /* U+00D8 "Ø" */
    0x40, 0x23, 0x25, 0x29, 0x84, 0x42, 0x10,

    /* U+00D9 "Ù" */
    0x1, 0x80, 0x60, 0x1c, 0x7, 0x81, 0xa1, 0xff,
    0xc0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 44, .box_w = 1, .box_h = 1, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1, .adv_w = 44, .box_w = 1, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2, .adv_w = 67, .box_w = 3, .box_h = 2, .ofs_x = 1, .ofs_y = 5},
    {.bitmap_index = 3, .adv_w = 100, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 2},
    {.bitmap_index = 8, .adv_w = 77, .box_w = 3, .box_h = 9, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 12, .adv_w = 122, .box_w = 6, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 18, .adv_w = 122, .box_w = 7, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 25, .adv_w = 33, .box_w = 1, .box_h = 3, .ofs_x = 1, .ofs_y = 5},
    {.bitmap_index = 26, .adv_w = 67, .box_w = 3, .box_h = 10, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 30, .adv_w = 67, .box_w = 3, .box_h = 10, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 34, .adv_w = 100, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 38, .adv_w = 100, .box_w = 5, .box_h = 6, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 42, .adv_w = 44, .box_w = 2, .box_h = 3, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 43, .adv_w = 89, .box_w = 4, .box_h = 1, .ofs_x = 1, .ofs_y = 4},
    {.bitmap_index = 44, .adv_w = 33, .box_w = 1, .box_h = 1, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 45, .adv_w = 77, .box_w = 4, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 49, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 54, .adv_w = 100, .box_w = 2, .box_h = 8, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 56, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 61, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 66, .adv_w = 100, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 72, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 77, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 82, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 87, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 92, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 97, .adv_w = 33, .box_w = 1, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 98, .adv_w = 44, .box_w = 2, .box_h = 8, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 100, .adv_w = 77, .box_w = 2, .box_h = 6, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 102, .adv_w = 122, .box_w = 5, .box_h = 3, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 104, .adv_w = 77, .box_w = 2, .box_h = 6, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 106, .adv_w = 89, .box_w = 4, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 110, .adv_w = 133, .box_w = 7, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 117, .adv_w = 122, .box_w = 6, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 123, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 128, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 133, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 138, .adv_w = 89, .box_w = 4, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 142, .adv_w = 89, .box_w = 4, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 146, .adv_w = 111, .box_w = 6, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 152, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 157, .adv_w = 55, .box_w = 1, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 158, .adv_w = 89, .box_w = 5, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 163, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 168, .adv_w = 89, .box_w = 4, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 172, .adv_w = 122, .box_w = 6, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 178, .adv_w = 111, .box_w = 6, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 184, .adv_w = 111, .box_w = 6, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 190, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 195, .adv_w = 111, .box_w = 6, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 201, .adv_w = 111, .box_w = 6, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 207, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 212, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 217, .adv_w = 111, .box_w = 6, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 223, .adv_w = 122, .box_w = 6, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 229, .adv_w = 144, .box_w = 8, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 237, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 242, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 247, .adv_w = 100, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 253, .adv_w = 55, .box_w = 2, .box_h = 10, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 256, .adv_w = 77, .box_w = 4, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 260, .adv_w = 55, .box_w = 2, .box_h = 10, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 263, .adv_w = 77, .box_w = 3, .box_h = 2, .ofs_x = 1, .ofs_y = 7},
    {.bitmap_index = 264, .adv_w = 100, .box_w = 6, .box_h = 1, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 265, .adv_w = 55, .box_w = 2, .box_h = 2, .ofs_x = 1, .ofs_y = 7},
    {.bitmap_index = 266, .adv_w = 89, .box_w = 4, .box_h = 6, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 269, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 274, .adv_w = 89, .box_w = 4, .box_h = 6, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 277, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 282, .adv_w = 100, .box_w = 5, .box_h = 6, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 286, .adv_w = 67, .box_w = 3, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 289, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 294, .adv_w = 89, .box_w = 4, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 298, .adv_w = 55, .box_w = 2, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 300, .adv_w = 55, .box_w = 2, .box_h = 10, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 303, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 308, .adv_w = 55, .box_w = 2, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 310, .adv_w = 144, .box_w = 7, .box_h = 6, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 316, .adv_w = 89, .box_w = 4, .box_h = 6, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 319, .adv_w = 100, .box_w = 5, .box_h = 6, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 323, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 328, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 333, .adv_w = 77, .box_w = 4, .box_h = 6, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 336, .adv_w = 89, .box_w = 4, .box_h = 6, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 339, .adv_w = 77, .box_w = 4, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 343, .adv_w = 89, .box_w = 4, .box_h = 6, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 346, .adv_w = 100, .box_w = 5, .box_h = 6, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 350, .adv_w = 122, .box_w = 6, .box_h = 6, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 355, .adv_w = 100, .box_w = 5, .box_h = 6, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 359, .adv_w = 100, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 365, .adv_w = 89, .box_w = 4, .box_h = 6, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 368, .adv_w = 55, .box_w = 3, .box_h = 9, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 372, .adv_w = 33, .box_w = 1, .box_h = 9, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 374, .adv_w = 55, .box_w = 3, .box_h = 9, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 378, .adv_w = 100, .box_w = 5, .box_h = 2, .ofs_x = 1, .ofs_y = 6},
    {.bitmap_index = 380, .adv_w = 55, .box_w = 3, .box_h = 4, .ofs_x = 0, .ofs_y = 5},
    {.bitmap_index = 382, .adv_w = 67, .box_w = 3, .box_h = 3, .ofs_x = 1, .ofs_y = 5},
    {.bitmap_index = 384, .adv_w = 89, .box_w = 4, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 388, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 393, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 398, .adv_w = 100, .box_w = 5, .box_h = 6, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 402, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 407, .adv_w = 111, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 413, .adv_w = 122, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 419, .adv_w = 122, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 425, .adv_w = 133, .box_w = 8, .box_h = 3, .ofs_x = 0, .ofs_y = 4},
    {.bitmap_index = 428, .adv_w = 44, .box_w = 1, .box_h = 2, .ofs_x = 1, .ofs_y = 6},
    {.bitmap_index = 429, .adv_w = 67, .box_w = 2, .box_h = 1, .ofs_x = 1, .ofs_y = 7},
    {.bitmap_index = 430, .adv_w = 100, .box_w = 5, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 435, .adv_w = 144, .box_w = 7, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 442, .adv_w = 111, .box_w = 6, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 448, .adv_w = 166, .box_w = 9, .box_h = 4, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 453, .adv_w = 100, .box_w = 5, .box_h = 6, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 457, .adv_w = 77, .box_w = 3, .box_h = 5, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 459, .adv_w = 77, .box_w = 3, .box_h = 5, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 461, .adv_w = 100, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 466, .adv_w = 111, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 472, .adv_w = 89, .box_w = 4, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 476, .adv_w = 111, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 482, .adv_w = 111, .box_w = 6, .box_h = 9, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 489, .adv_w = 122, .box_w = 6, .box_h = 6, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 494, .adv_w = 55, .box_w = 3, .box_h = 11, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 499, .adv_w = 77, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 502, .adv_w = 77, .box_w = 4, .box_h = 6, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 505, .adv_w = 111, .box_w = 6, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 511, .adv_w = 155, .box_w = 9, .box_h = 6, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 518, .adv_w = 89, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 523, .adv_w = 100, .box_w = 4, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 527, .adv_w = 44, .box_w = 1, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 528, .adv_w = 89, .box_w = 4, .box_h = 3, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 530, .adv_w = 133, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 535, .adv_w = 55, .box_w = 3, .box_h = 12, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 540, .adv_w = 100, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 544, .adv_w = 122, .box_w = 7, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 551, .adv_w = 100, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 555, .adv_w = 100, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 559, .adv_w = 111, .box_w = 5, .box_h = 1, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 560, .adv_w = 100, .box_w = 1, .box_h = 1, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 561, .adv_w = 122, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 570, .adv_w = 122, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 579, .adv_w = 111, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 588, .adv_w = 155, .box_w = 8, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 596, .adv_w = 155, .box_w = 9, .box_h = 6, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 603, .adv_w = 77, .box_w = 3, .box_h = 1, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 604, .adv_w = 111, .box_w = 5, .box_h = 1, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 605, .adv_w = 89, .box_w = 4, .box_h = 3, .ofs_x = 1, .ofs_y = 5},
    {.bitmap_index = 607, .adv_w = 89, .box_w = 4, .box_h = 3, .ofs_x = 1, .ofs_y = 5},
    {.bitmap_index = 609, .adv_w = 44, .box_w = 2, .box_h = 3, .ofs_x = 1, .ofs_y = 5},
    {.bitmap_index = 610, .adv_w = 44, .box_w = 2, .box_h = 3, .ofs_x = 0, .ofs_y = 5},
    {.bitmap_index = 611, .adv_w = 100, .box_w = 5, .box_h = 4, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 614, .adv_w = 122, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 620, .adv_w = 100, .box_w = 5, .box_h = 11, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 627, .adv_w = 166, .box_w = 11, .box_h = 6, .ofs_x = 0, .ofs_y = 0}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 32, .range_length = 95, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    },
    {
        .range_start = 160, .range_length = 58, .glyph_id_start = 96,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 2,
    .bpp = 1,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif

};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t geneva_9 = {
#else
lv_font_t geneva_9 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 14,          /*The maximum line height required by the font*/
    .base_line = 3,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 0,
#endif
    .static_bitmap = 0,
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if GENEVA_9*/
