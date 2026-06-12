/*******************************************************************************
 * Size: 14 px
 * Bpp: 1
 * Opts: --bpp 1 --size 14 --stride 1 --align 1 --font Geneva-14.ttf --range 32-126,160-255 --format lvgl -o geneva_14.c
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



#ifndef GENEVA_14
#define GENEVA_14 1
#endif

#if GENEVA_14

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0020 " " */
    0x0,

    /* U+0021 "!" */
    0xff, 0xd0,

    /* U+0022 "\"" */
    0x99, 0x90,

    /* U+0023 "#" */
    0x14, 0x14, 0x7f, 0x28, 0x28, 0x28, 0xfc, 0x50,
    0x50,

    /* U+0024 "$" */
    0x23, 0xab, 0x4a, 0x30, 0xc6, 0x29, 0x6a, 0xe2,
    0x0,

    /* U+0025 "%" */
    0x6f, 0xe4, 0x19, 0xa, 0x4c, 0x64, 0x2, 0x1,
    0x38, 0x92, 0x24, 0x51, 0x18, 0x46, 0xe,

    /* U+0026 "&" */
    0x30, 0x12, 0x4, 0x81, 0x40, 0x20, 0x14, 0x8,
    0x8a, 0x22, 0x84, 0xa0, 0xc4, 0x58, 0xe1,

    /* U+0027 "'" */
    0xe0,

    /* U+0028 "(" */
    0x29, 0x49, 0x24, 0x92, 0x24, 0x40,

    /* U+0029 ")" */
    0x89, 0x12, 0x49, 0x24, 0xa5, 0x0,

    /* U+002A "*" */
    0x44, 0x28, 0x10, 0xff, 0x10, 0x28, 0x44,

    /* U+002B "+" */
    0x10, 0x10, 0x10, 0x10, 0xff, 0x10, 0x10, 0x10,
    0x10,

    /* U+002C "," */
    0x56,

    /* U+002D "-" */
    0xfe,

    /* U+002E "." */
    0x80,

    /* U+002F "/" */
    0x8, 0x44, 0x22, 0x11, 0x8, 0x44, 0x20,

    /* U+0030 "0" */
    0x38, 0x8a, 0xc, 0x18, 0x30, 0x60, 0xc1, 0x83,
    0x5, 0x11, 0xc0,

    /* U+0031 "1" */
    0x75, 0x55, 0x55,

    /* U+0032 "2" */
    0x38, 0x8a, 0x8, 0x10, 0x20, 0x82, 0x8, 0x10,
    0x41, 0x7, 0xf0,

    /* U+0033 "3" */
    0xfe, 0x8, 0x20, 0x83, 0x80, 0x81, 0x1, 0x2,
    0x6, 0x13, 0xc0,

    /* U+0034 "4" */
    0x4, 0xc, 0x14, 0x24, 0x44, 0x84, 0xff, 0x4,
    0x4, 0x4, 0x4, 0x4,

    /* U+0035 "5" */
    0xff, 0x2, 0x4, 0xf, 0x80, 0x81, 0x1, 0x2,
    0x6, 0x13, 0xc0,

    /* U+0036 "6" */
    0x38, 0x82, 0x5, 0xcc, 0x50, 0x60, 0xc1, 0x83,
    0x9, 0x11, 0xc0,

    /* U+0037 "7" */
    0xfe, 0x4, 0x8, 0x20, 0x41, 0x2, 0x8, 0x10,
    0x20, 0x40, 0x80,

    /* U+0038 "8" */
    0x38, 0x89, 0x12, 0x23, 0x88, 0xa1, 0x41, 0x83,
    0x5, 0x11, 0xc0,

    /* U+0039 "9" */
    0x38, 0x8a, 0xc, 0x18, 0x30, 0x51, 0xa5, 0x3a,
    0x4, 0x11, 0xc0,

    /* U+003A ":" */
    0x81,

    /* U+003B ";" */
    0x40, 0x1, 0x58,

    /* U+003C "<" */
    0x12, 0x48, 0x42, 0x21,

    /* U+003D "=" */
    0xff, 0x0, 0x0, 0xff,

    /* U+003E ">" */
    0x84, 0x21, 0x24, 0x88,

    /* U+003F "?" */
    0x7a, 0x10, 0x41, 0x8, 0x42, 0x8, 0x20, 0x80,
    0x8,

    /* U+0040 "@" */
    0x3f, 0x88, 0xa, 0x38, 0xc9, 0x19, 0x23, 0x24,
    0x65, 0x54, 0x66, 0x40, 0x7, 0x80,

    /* U+0041 "A" */
    0x8, 0x2, 0x0, 0x80, 0x50, 0x14, 0x8, 0x82,
    0x20, 0x88, 0x7f, 0x90, 0x28, 0x6, 0x1,

    /* U+0042 "B" */
    0xf9, 0xa, 0x14, 0x2f, 0x90, 0xa1, 0x41, 0x83,
    0x6, 0x17, 0xc0,

    /* U+0043 "C" */
    0x3e, 0x82, 0x4, 0x8, 0x10, 0x20, 0x40, 0x81,
    0x1, 0x1, 0xf0,

    /* U+0044 "D" */
    0xf9, 0xa, 0xc, 0x18, 0x30, 0x60, 0xc1, 0x83,
    0x6, 0x17, 0xc0,

    /* U+0045 "E" */
    0xff, 0x2, 0x4, 0x8, 0x1f, 0xa0, 0x40, 0x81,
    0x2, 0x7, 0xf0,

    /* U+0046 "F" */
    0xff, 0x2, 0x4, 0x8, 0x1f, 0xa0, 0x40, 0x81,
    0x2, 0x4, 0x0,

    /* U+0047 "G" */
    0x3e, 0x20, 0x20, 0x10, 0x8, 0x4, 0x7e, 0x3,
    0x1, 0x80, 0xc0, 0x50, 0xc7, 0xc0,

    /* U+0048 "H" */
    0x83, 0x6, 0xc, 0x18, 0x3f, 0xe0, 0xc1, 0x83,
    0x6, 0xc, 0x10,

    /* U+0049 "I" */
    0xff, 0xf0,

    /* U+004A "J" */
    0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1,
    0x81, 0x81, 0x46, 0x38,

    /* U+004B "K" */
    0x83, 0xa, 0x24, 0x8a, 0x18, 0x28, 0x50, 0x91,
    0x12, 0x14, 0x10,

    /* U+004C "L" */
    0x81, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x81,
    0x2, 0x7, 0xf0,

    /* U+004D "M" */
    0x80, 0x70, 0x7a, 0x26, 0x51, 0x88, 0x60, 0x18,
    0x6, 0x1, 0x80, 0x60, 0x18, 0x6, 0x1,

    /* U+004E "N" */
    0xc0, 0xe0, 0x68, 0x34, 0x19, 0xc, 0x46, 0x23,
    0x11, 0x84, 0xc1, 0xe0, 0x70, 0x10,

    /* U+004F "O" */
    0x3e, 0x21, 0x20, 0x70, 0x18, 0xc, 0x6, 0x3,
    0x1, 0x80, 0xc0, 0x50, 0xc7, 0xc0,

    /* U+0050 "P" */
    0xf9, 0xa, 0xc, 0x18, 0x30, 0xbe, 0x40, 0x81,
    0x2, 0x4, 0x0,

    /* U+0051 "Q" */
    0x3c, 0x21, 0x20, 0x70, 0x18, 0xc, 0x6, 0x3,
    0x1, 0x80, 0xc2, 0x50, 0xc7, 0xb0,

    /* U+0052 "R" */
    0xf8, 0x42, 0x20, 0x90, 0x48, 0x24, 0x23, 0xe1,
    0x10, 0x88, 0x42, 0x20, 0x90, 0x30,

    /* U+0053 "S" */
    0x38, 0x8a, 0xc, 0x4, 0x7, 0x1, 0x2, 0x3,
    0x5, 0x11, 0xc0,

    /* U+0054 "T" */
    0xff, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    0x10, 0x10, 0x10, 0x10,

    /* U+0055 "U" */
    0x80, 0xc0, 0x60, 0x30, 0x18, 0xc, 0x6, 0x3,
    0x1, 0x80, 0xc0, 0x50, 0xc7, 0xc0,

    /* U+0056 "V" */
    0x80, 0x60, 0x14, 0x9, 0x2, 0x23, 0x8, 0x81,
    0x40, 0x50, 0x18, 0x2, 0x0, 0x80, 0x20,

    /* U+0057 "W" */
    0x80, 0x18, 0x1, 0x44, 0x24, 0x42, 0x44, 0x22,
    0xa4, 0x2a, 0x42, 0xa4, 0x31, 0x81, 0x8, 0x10,
    0x81, 0x8,

    /* U+0058 "X" */
    0x83, 0x5, 0x12, 0x22, 0x82, 0xa, 0x24, 0x44,
    0x8a, 0xc, 0x10,

    /* U+0059 "Y" */
    0x83, 0x5, 0x12, 0x22, 0x85, 0xc, 0x18, 0x10,
    0x20, 0x40, 0x80,

    /* U+005A "Z" */
    0xfe, 0x4, 0x10, 0x40, 0x82, 0x8, 0x10, 0x40,
    0x82, 0x7, 0xf0,

    /* U+005B "[" */
    0xf2, 0x49, 0x24, 0x92, 0x49, 0x38,

    /* U+005C "\\" */
    0x84, 0x10, 0x82, 0x10, 0x82, 0x10, 0x42,

    /* U+005D "]" */
    0xe4, 0x92, 0x49, 0x24, 0x92, 0x78,

    /* U+005E "^" */
    0x22, 0xa2,

    /* U+005F "_" */
    0xff, 0xc0,

    /* U+0060 "`" */
    0x88, 0x80,

    /* U+0061 "a" */
    0x7a, 0x13, 0xd1, 0x86, 0x18, 0x63, 0x74,

    /* U+0062 "b" */
    0x81, 0x2, 0x5, 0xcc, 0x50, 0x60, 0xc1, 0x83,
    0xb, 0x15, 0xc0,

    /* U+0063 "c" */
    0x39, 0x18, 0x20, 0x82, 0x8, 0x11, 0x38,

    /* U+0064 "d" */
    0x2, 0x4, 0x9, 0xd4, 0x70, 0x60, 0xc1, 0x83,
    0x5, 0x19, 0xd0,

    /* U+0065 "e" */
    0x38, 0x8a, 0xf, 0xf8, 0x10, 0x20, 0x22, 0x38,

    /* U+0066 "f" */
    0x34, 0x4f, 0x44, 0x44, 0x44, 0x44,

    /* U+0067 "g" */
    0x3a, 0x8e, 0xc, 0x18, 0x30, 0x51, 0xa5, 0x3a,
    0x4, 0x11, 0xc0,

    /* U+0068 "h" */
    0x82, 0x8, 0x2e, 0xc6, 0x18, 0x61, 0x86, 0x18,
    0x61,

    /* U+0069 "i" */
    0x43, 0x55, 0x55,

    /* U+006A "j" */
    0x20, 0x32, 0x49, 0x24, 0x92, 0x70,

    /* U+006B "k" */
    0x81, 0x2, 0x4, 0x28, 0x92, 0x28, 0x70, 0xd1,
    0x12, 0x14, 0x10,

    /* U+006C "l" */
    0xd5, 0x55, 0x55,

    /* U+006D "m" */
    0xb8, 0xec, 0x71, 0x84, 0x18, 0x41, 0x84, 0x18,
    0x41, 0x84, 0x18, 0x41, 0x84, 0x10,

    /* U+006E "n" */
    0xbb, 0x18, 0x61, 0x86, 0x18, 0x61, 0x84,

    /* U+006F "o" */
    0x38, 0x8a, 0xc, 0x18, 0x30, 0x61, 0x22, 0x38,

    /* U+0070 "p" */
    0xb9, 0x8a, 0xc, 0x18, 0x30, 0x71, 0x64, 0xb9,
    0x2, 0x4, 0x0,

    /* U+0071 "q" */
    0x3a, 0x8e, 0xc, 0x18, 0x30, 0x51, 0xa5, 0x3a,
    0x4, 0x8, 0x10,

    /* U+0072 "r" */
    0xbb, 0x18, 0x20, 0x82, 0x8, 0x20, 0x80,

    /* U+0073 "s" */
    0x7a, 0x18, 0x1e, 0x8, 0x10, 0x61, 0x78,

    /* U+0074 "t" */
    0x42, 0x11, 0xe4, 0x21, 0x8, 0x42, 0x12, 0x60,

    /* U+0075 "u" */
    0x86, 0x18, 0x61, 0x86, 0x18, 0x63, 0x74,

    /* U+0076 "v" */
    0x83, 0x5, 0x12, 0x24, 0x85, 0xa, 0x8, 0x10,

    /* U+0077 "w" */
    0x88, 0x62, 0x14, 0x89, 0x52, 0x54, 0x99, 0x42,
    0x20, 0x88, 0x22, 0x0,

    /* U+0078 "x" */
    0x82, 0x88, 0xa0, 0x81, 0x6, 0xa, 0x22, 0x82,

    /* U+0079 "y" */
    0x83, 0x5, 0x12, 0x24, 0x85, 0xa, 0x8, 0x10,
    0x40, 0x82, 0x4, 0x0,

    /* U+007A "z" */
    0xfc, 0x10, 0x84, 0x20, 0x84, 0x20, 0xfc,

    /* U+007B "{" */
    0x29, 0x25, 0x24, 0x49, 0x22,

    /* U+007C "|" */
    0xff, 0xfc,

    /* U+007D "}" */
    0x89, 0x24, 0x52, 0x49, 0x28,

    /* U+007E "~" */
    0x73, 0x18,

    /* U+00A0 " " */
    0xe9, 0x20,

    /* U+00A1 "¡" */
    0x69, 0x96,

    /* U+00A2 "¢" */
    0x10, 0x43, 0x95, 0x92, 0x49, 0x24, 0x91, 0x53,
    0x84,

    /* U+00A3 "£" */
    0x38, 0x89, 0x2, 0xf, 0x8, 0x10, 0x20, 0x40,
    0x85, 0xf, 0xe0,

    /* U+00A4 "¤" */
    0x7d, 0x6, 0x3, 0x9, 0x88, 0x8c, 0x8a, 0xc,
    0x6, 0xb, 0xe0,

    /* U+00A5 "¥" */
    0x7e, 0x7e, 0x7e, 0xff, 0xff, 0xff, 0xff, 0x7e,

    /* U+00A6 "¦" */
    0x3e, 0x96, 0x2c, 0x58, 0xb1, 0x52, 0xa5, 0x3e,
    0x14, 0x28, 0x50,

    /* U+00A7 "§" */
    0x1c, 0x22, 0x42, 0x44, 0x4c, 0x42, 0x42, 0x41,
    0x41, 0x41, 0x42, 0xcc,

    /* U+00A8 "¨" */
    0x3e, 0x10, 0x6b, 0xe6, 0x89, 0xa2, 0x68, 0x9b,
    0xe6, 0x89, 0x41, 0x8f, 0xc0,

    /* U+00A9 "©" */
    0x3e, 0x10, 0x69, 0xc6, 0x89, 0xa0, 0x68, 0x1a,
    0x26, 0x71, 0x40, 0x8f, 0xc0,

    /* U+00AA "ª" */
    0xf2, 0x28, 0x6d, 0xa, 0xa1, 0x10,

    /* U+00AB "«" */
    0x60,

    /* U+00AC "¬" */
    0x90,

    /* U+00AD "­" */
    0x8, 0x8, 0x8, 0xff, 0x10, 0x10, 0xff, 0x20,
    0x20,

    /* U+00AE "®" */
    0xf, 0xf1, 0x40, 0x14, 0x2, 0x40, 0x24, 0x3,
    0xfc, 0x44, 0x4, 0x40, 0x44, 0x4, 0x40, 0x84,
    0x8, 0x7f,

    /* U+00AF "¯" */
    0x3c, 0xa1, 0xa1, 0x70, 0x98, 0x8c, 0x46, 0x43,
    0x41, 0xa0, 0xd0, 0x50, 0xd7, 0x80,

    /* U+00B0 "°" */
    0x78, 0x7a, 0x22, 0x28, 0x48, 0x60, 0xc1, 0x85,
    0x85, 0xe1, 0xe0,

    /* U+00B1 "±" */
    0x10, 0x10, 0x10, 0xff, 0x10, 0x10, 0x10, 0x10,
    0xff,

    /* U+00B2 "²" */
    0x11, 0x11, 0x8, 0x20, 0x82, 0xf8,

    /* U+00B3 "³" */
    0x82, 0x8, 0x41, 0x11, 0x10, 0xf8,

    /* U+00B4 "´" */
    0x82, 0x89, 0x21, 0x4f, 0xe2, 0x4, 0x7f, 0x10,
    0x20, 0x40, 0x81, 0x0,

    /* U+00B5 "µ" */
    0x40, 0x21, 0x10, 0x88, 0x44, 0x22, 0x11, 0x8,
    0x84, 0x7f, 0xa0, 0x10, 0x10, 0x0,

    /* U+00B6 "¶" */
    0x70, 0x20, 0x47, 0x25, 0x18, 0x61, 0x86, 0x27,
    0x0,

    /* U+00B7 "·" */
    0xff, 0x41, 0x20, 0x10, 0x8, 0x4, 0x8, 0x10,
    0x10, 0x20, 0x41, 0xff,

    /* U+00B8 "¸" */
    0xff, 0xa1, 0x10, 0x88, 0x44, 0x22, 0x11, 0x8,
    0x84, 0x42, 0x21, 0x10, 0x88, 0x40,

    /* U+00B9 "¹" */
    0x0, 0x5f, 0xea, 0x20, 0x88, 0x22, 0x8, 0x82,
    0x20, 0x88, 0x22, 0x11, 0x0,

    /* U+00BA "º" */
    0x29, 0x24, 0x92, 0x49, 0x24, 0x92, 0x80,

    /* U+00BB "»" */
    0x74, 0x5f, 0x18, 0xbc, 0x1f,

    /* U+00BC "¼" */
    0x74, 0x63, 0x18, 0xc9, 0xc0, 0xf8,

    /* U+00BD "½" */
    0x3c, 0x21, 0x20, 0x70, 0x18, 0xc, 0x5, 0xc,
    0x84, 0x24, 0x12, 0x9, 0xc, 0xc0,

    /* U+00BE "¾" */
    0x78, 0xe4, 0x38, 0x8f, 0x2, 0x8f, 0xf8, 0x40,
    0x42, 0x2, 0x10, 0x11, 0xc2, 0x75, 0xe0,

    /* U+00BF "¿" */
    0x3e, 0xa0, 0x91, 0xc9, 0x25, 0x12, 0x89, 0x84,
    0x82, 0xbe, 0x0,

    /* U+00C0 "À" */
    0x10, 0x1, 0x4, 0x10, 0x84, 0x20, 0x82, 0x8,
    0x5e,

    /* U+00C1 "Á" */
    0xbf, 0xf0,

    /* U+00C2 "Â" */
    0xfe, 0x4, 0x8, 0x10,

    /* U+00C3 "Ã" */
    0x0, 0x60, 0x10, 0x2, 0x1, 0x80, 0x20, 0x8,
    0x31, 0x2, 0x40, 0x28, 0x5, 0x0, 0x40, 0x8,
    0x0,

    /* U+00C4 "Ä" */
    0x29, 0x24, 0x97, 0x49, 0x24, 0x92, 0x50,

    /* U+00C5 "Å" */
    0x0, 0xfe, 0x0, 0x7, 0xf0, 0x20, 0x0,

    /* U+00C6 "Æ" */
    0x8, 0x2, 0x1, 0x40, 0x50, 0x22, 0x8, 0x84,
    0x11, 0x2, 0x40, 0xa0, 0x1f, 0xfc,

    /* U+00C7 "Ç" */
    0x12, 0x49, 0x24, 0x89, 0x9, 0x9, 0x9,

    /* U+00C8 "È" */
    0x90, 0x90, 0x91, 0x21, 0x24, 0x92, 0x48,

    /* U+00C9 "É" */
    0x92,

    /* U+00CA "Ê" */
    0x0,

    /* U+00CB "Ë" */
    0x8, 0x1, 0x0, 0x0, 0x0, 0x8, 0x2, 0x0,
    0x80, 0x50, 0x14, 0x8, 0x82, 0x21, 0x4, 0x40,
    0x9f, 0xe8, 0x6, 0x1,

    /* U+00CC "Ì" */
    0x18, 0x89, 0xc0, 0x0, 0x0, 0x8, 0x2, 0x0,
    0x80, 0x50, 0x14, 0x8, 0x82, 0x21, 0x4, 0x40,
    0x9f, 0xe8, 0x6, 0x1,

    /* U+00CD "Í" */
    0x32, 0x26, 0x0, 0x0, 0x3, 0xc2, 0x12, 0x7,
    0x1, 0x80, 0xc0, 0x60, 0x30, 0x18, 0xc, 0x5,
    0xc, 0x78,

    /* U+00CE "Î" */
    0x3b, 0xfa, 0x30, 0x20, 0x81, 0x4, 0x8, 0x20,
    0x41, 0xfa, 0x8, 0x10, 0x40, 0x82, 0x4, 0x10,
    0x11, 0x80, 0x77, 0xf0,

    /* U+00CF "Ï" */
    0x78, 0xe8, 0x76, 0x84, 0x18, 0x7f, 0x84, 0x8,
    0x40, 0x84, 0x8, 0x76, 0x78, 0xe0,

    /* U+00D0 "Ð" */
    0xfc,

    /* U+00D1 "Ñ" */
    0xff, 0x80,

    /* U+00D2 "Ò" */
    0x4c, 0xa5, 0x20,

    /* U+00D3 "Ó" */
    0x4a, 0x53, 0x20,

    /* U+00D4 "Ô" */
    0x6a,

    /* U+00D5 "Õ" */
    0x56,

    /* U+00D6 "Ö" */
    0x10, 0x0, 0xff, 0x0, 0x10,

    /* U+00D7 "×" */
    0x8, 0x5, 0x2, 0x21, 0x6, 0x80, 0x60, 0x24,
    0x8, 0x8c, 0x14, 0x2, 0x0,

    /* U+00D8 "Ø" */
    0x44, 0x0, 0x4, 0x18, 0x28, 0x91, 0x24, 0x28,
    0x50, 0x40, 0x82, 0x4, 0x10, 0x20,

    /* U+00D9 "Ù" */
    0x0, 0xc, 0x0, 0x1f, 0x0, 0x1c, 0x0, 0xe,
    0x0, 0xf, 0x0, 0xf, 0x80, 0xf, 0xc0, 0xc,
    0x80, 0xff, 0xff, 0x80
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 69, .box_w = 1, .box_h = 1, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1, .adv_w = 69, .box_w = 1, .box_h = 12, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 3, .adv_w = 103, .box_w = 4, .box_h = 3, .ofs_x = 1, .ofs_y = 8},
    {.bitmap_index = 5, .adv_w = 155, .box_w = 8, .box_h = 9, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 14, .adv_w = 121, .box_w = 5, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 23, .adv_w = 190, .box_w = 10, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 38, .adv_w = 190, .box_w = 10, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 53, .adv_w = 52, .box_w = 1, .box_h = 3, .ofs_x = 1, .ofs_y = 9},
    {.bitmap_index = 54, .adv_w = 103, .box_w = 3, .box_h = 14, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 60, .adv_w = 103, .box_w = 3, .box_h = 14, .ofs_x = 2, .ofs_y = -1},
    {.bitmap_index = 66, .adv_w = 155, .box_w = 8, .box_h = 7, .ofs_x = 1, .ofs_y = 5},
    {.bitmap_index = 73, .adv_w = 155, .box_w = 8, .box_h = 9, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 82, .adv_w = 69, .box_w = 2, .box_h = 4, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 83, .adv_w = 138, .box_w = 7, .box_h = 1, .ofs_x = 1, .ofs_y = 6},
    {.bitmap_index = 84, .adv_w = 52, .box_w = 1, .box_h = 1, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 85, .adv_w = 121, .box_w = 5, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 92, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 103, .adv_w = 155, .box_w = 2, .box_h = 12, .ofs_x = 3, .ofs_y = 0},
    {.bitmap_index = 106, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 117, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 128, .adv_w = 155, .box_w = 8, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 140, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 151, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 162, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 173, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 184, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 195, .adv_w = 52, .box_w = 1, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 196, .adv_w = 69, .box_w = 2, .box_h = 11, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 199, .adv_w = 121, .box_w = 4, .box_h = 8, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 203, .adv_w = 190, .box_w = 8, .box_h = 4, .ofs_x = 2, .ofs_y = 3},
    {.bitmap_index = 207, .adv_w = 121, .box_w = 4, .box_h = 8, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 211, .adv_w = 138, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 220, .adv_w = 207, .box_w = 11, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 234, .adv_w = 190, .box_w = 10, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 249, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 260, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 271, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 282, .adv_w = 138, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 293, .adv_w = 138, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 304, .adv_w = 172, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 318, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 329, .adv_w = 86, .box_w = 1, .box_h = 12, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 331, .adv_w = 138, .box_w = 8, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 343, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 354, .adv_w = 138, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 365, .adv_w = 190, .box_w = 10, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 380, .adv_w = 172, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 394, .adv_w = 172, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 408, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 419, .adv_w = 172, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 433, .adv_w = 172, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 447, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 458, .adv_w = 155, .box_w = 8, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 470, .adv_w = 172, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 484, .adv_w = 190, .box_w = 10, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 499, .adv_w = 224, .box_w = 12, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 517, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 528, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 539, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 550, .adv_w = 86, .box_w = 3, .box_h = 15, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 556, .adv_w = 121, .box_w = 5, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 563, .adv_w = 86, .box_w = 3, .box_h = 15, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 569, .adv_w = 121, .box_w = 5, .box_h = 3, .ofs_x = 1, .ofs_y = 10},
    {.bitmap_index = 571, .adv_w = 155, .box_w = 10, .box_h = 1, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 573, .adv_w = 86, .box_w = 3, .box_h = 3, .ofs_x = 1, .ofs_y = 10},
    {.bitmap_index = 575, .adv_w = 138, .box_w = 6, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 582, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 593, .adv_w = 138, .box_w = 6, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 600, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 611, .adv_w = 155, .box_w = 7, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 619, .adv_w = 103, .box_w = 4, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 625, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 636, .adv_w = 138, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 645, .adv_w = 86, .box_w = 2, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 648, .adv_w = 86, .box_w = 3, .box_h = 15, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 654, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 665, .adv_w = 86, .box_w = 2, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 668, .adv_w = 224, .box_w = 12, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 682, .adv_w = 138, .box_w = 6, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 689, .adv_w = 155, .box_w = 7, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 697, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 708, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 719, .adv_w = 121, .box_w = 6, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 726, .adv_w = 138, .box_w = 6, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 733, .adv_w = 121, .box_w = 5, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 741, .adv_w = 138, .box_w = 6, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 748, .adv_w = 155, .box_w = 7, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 756, .adv_w = 190, .box_w = 10, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 768, .adv_w = 155, .box_w = 7, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 776, .adv_w = 155, .box_w = 7, .box_h = 13, .ofs_x = 1, .ofs_y = -4},
    {.bitmap_index = 788, .adv_w = 138, .box_w = 6, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 795, .adv_w = 86, .box_w = 3, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 800, .adv_w = 52, .box_w = 1, .box_h = 14, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 802, .adv_w = 86, .box_w = 3, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 807, .adv_w = 155, .box_w = 7, .box_h = 2, .ofs_x = 1, .ofs_y = 9},
    {.bitmap_index = 809, .adv_w = 86, .box_w = 3, .box_h = 4, .ofs_x = 1, .ofs_y = 8},
    {.bitmap_index = 811, .adv_w = 103, .box_w = 4, .box_h = 4, .ofs_x = 1, .ofs_y = 8},
    {.bitmap_index = 813, .adv_w = 138, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 822, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 833, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 844, .adv_w = 155, .box_w = 8, .box_h = 8, .ofs_x = 1, .ofs_y = 4},
    {.bitmap_index = 852, .adv_w = 155, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 863, .adv_w = 172, .box_w = 8, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 875, .adv_w = 190, .box_w = 10, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 888, .adv_w = 190, .box_w = 10, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 901, .adv_w = 207, .box_w = 11, .box_h = 4, .ofs_x = 1, .ofs_y = 8},
    {.bitmap_index = 907, .adv_w = 69, .box_w = 2, .box_h = 2, .ofs_x = 1, .ofs_y = 10},
    {.bitmap_index = 908, .adv_w = 103, .box_w = 4, .box_h = 1, .ofs_x = 1, .ofs_y = 11},
    {.bitmap_index = 909, .adv_w = 155, .box_w = 8, .box_h = 9, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 918, .adv_w = 224, .box_w = 12, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 936, .adv_w = 172, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 950, .adv_w = 258, .box_w = 14, .box_h = 6, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 961, .adv_w = 155, .box_w = 8, .box_h = 9, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 970, .adv_w = 121, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 976, .adv_w = 121, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 982, .adv_w = 155, .box_w = 7, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 994, .adv_w = 172, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 1008, .adv_w = 138, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1017, .adv_w = 172, .box_w = 8, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1029, .adv_w = 172, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1043, .adv_w = 190, .box_w = 10, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1056, .adv_w = 86, .box_w = 3, .box_h = 17, .ofs_x = 1, .ofs_y = -4},
    {.bitmap_index = 1063, .adv_w = 121, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 4},
    {.bitmap_index = 1068, .adv_w = 121, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 1074, .adv_w = 172, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1088, .adv_w = 241, .box_w = 13, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1103, .adv_w = 138, .box_w = 9, .box_h = 9, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1114, .adv_w = 155, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1123, .adv_w = 69, .box_w = 1, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1125, .adv_w = 138, .box_w = 7, .box_h = 4, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 1129, .adv_w = 207, .box_w = 11, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1146, .adv_w = 86, .box_w = 3, .box_h = 18, .ofs_x = 1, .ofs_y = -4},
    {.bitmap_index = 1153, .adv_w = 155, .box_w = 7, .box_h = 7, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 1160, .adv_w = 190, .box_w = 10, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1174, .adv_w = 155, .box_w = 7, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1181, .adv_w = 155, .box_w = 7, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1188, .adv_w = 172, .box_w = 7, .box_h = 1, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 1189, .adv_w = 155, .box_w = 1, .box_h = 1, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1190, .adv_w = 190, .box_w = 10, .box_h = 16, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1210, .adv_w = 190, .box_w = 10, .box_h = 16, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1230, .adv_w = 172, .box_w = 9, .box_h = 16, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1248, .adv_w = 241, .box_w = 13, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1268, .adv_w = 241, .box_w = 12, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1282, .adv_w = 121, .box_w = 6, .box_h = 1, .ofs_x = 1, .ofs_y = 5},
    {.bitmap_index = 1283, .adv_w = 172, .box_w = 9, .box_h = 1, .ofs_x = 1, .ofs_y = 5},
    {.bitmap_index = 1285, .adv_w = 138, .box_w = 5, .box_h = 4, .ofs_x = 1, .ofs_y = 8},
    {.bitmap_index = 1288, .adv_w = 138, .box_w = 5, .box_h = 4, .ofs_x = 2, .ofs_y = 8},
    {.bitmap_index = 1291, .adv_w = 69, .box_w = 2, .box_h = 4, .ofs_x = 1, .ofs_y = 8},
    {.bitmap_index = 1292, .adv_w = 69, .box_w = 2, .box_h = 4, .ofs_x = 1, .ofs_y = 8},
    {.bitmap_index = 1293, .adv_w = 155, .box_w = 8, .box_h = 5, .ofs_x = 1, .ofs_y = 4},
    {.bitmap_index = 1298, .adv_w = 190, .box_w = 10, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1311, .adv_w = 155, .box_w = 7, .box_h = 16, .ofs_x = 1, .ofs_y = -4},
    {.bitmap_index = 1325, .adv_w = 258, .box_w = 17, .box_h = 9, .ofs_x = 0, .ofs_y = 0}
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
const lv_font_t geneva_14 = {
#else
lv_font_t geneva_14 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 20,          /*The maximum line height required by the font*/
    .base_line = 4,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .static_bitmap = 0,
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if GENEVA_14*/
