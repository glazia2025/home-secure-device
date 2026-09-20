#!/usr/bin/env python3
"""PNG -> LVGL v8 C image (RGB565, little-endian) converter.

Emits an lv_img_dsc_t C source matching the shape of the existing
main/ui/ui_image_*.c files so it drops straight into the file(GLOB ui/*.c) build.

The firmware is fixed at LV_COLOR_DEPTH=16, LV_COLOR_16_SWAP=0, so we only emit
the 16-bit little-endian pixel block (guarded with #if LV_COLOR_DEPTH == 16).

Modes:
  rgb565    opaque, 2 bytes/px (LV_IMG_CF_TRUE_COLOR)          -> backgrounds
  rgb565a8  color + alpha, 3 bytes/px (LV_IMG_CF_TRUE_COLOR_ALPHA) -> icons

Fit (only when --w/--h given):
  cover   scale to fill, center-crop the overflow (default)
  contain scale to fit inside, pad transparent/white

Usage:
  png_to_lvgl.py in.png --name welcome_bg --w 720 --h 1280 \
      --mode rgb565 --fit cover -o main/ui/ui_image_welcome_bg.c
"""
import argparse
import sys

from PIL import Image


def to_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def fit_image(img, w, h, mode):
    src_w, src_h = img.size
    if mode == "cover":
        scale = max(w / src_w, h / src_h)
        new = img.resize((max(1, round(src_w * scale)), max(1, round(src_h * scale))),
                         Image.LANCZOS)
        left = (new.size[0] - w) // 2
        top = (new.size[1] - h) // 2
        return new.crop((left, top, left + w, top + h))
    else:  # contain
        scale = min(w / src_w, h / src_h)
        new = img.resize((max(1, round(src_w * scale)), max(1, round(src_h * scale))),
                         Image.LANCZOS)
        canvas = Image.new("RGBA", (w, h), (255, 255, 255, 0))
        canvas.paste(new, ((w - new.size[0]) // 2, (h - new.size[1]) // 2))
        return canvas


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("--name", required=True, help="symbol suffix -> img_<name>")
    ap.add_argument("--w", type=int, default=0)
    ap.add_argument("--h", type=int, default=0)
    ap.add_argument("--mode", choices=["rgb565", "rgb565a8"], default="rgb565")
    ap.add_argument("--fit", choices=["cover", "contain"], default="cover")
    ap.add_argument("-o", "--out", required=True)
    args = ap.parse_args()

    img = Image.open(args.input).convert("RGBA")
    if args.w and args.h:
        img = fit_image(img, args.w, args.h, args.fit)
    w, h = img.size
    px = img.load()

    data = bytearray()
    alpha = args.mode == "rgb565a8"
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if not alpha:
                # composite opaque images over white so PNG transparency
                # doesn't turn into black in TRUE_COLOR mode.
                if a < 255:
                    r = (r * a + 255 * (255 - a)) // 255
                    g = (g * a + 255 * (255 - a)) // 255
                    b = (b * a + 255 * (255 - a)) // 255
            c = to_rgb565(r, g, b)
            data.append(c & 0xFF)
            data.append((c >> 8) & 0xFF)
            if alpha:
                data.append(a)

    name = args.name
    up = name.upper()
    cf = "LV_IMG_CF_TRUE_COLOR_ALPHA" if alpha else "LV_IMG_CF_TRUE_COLOR"
    if alpha:
        data_size = f"{w * h} * LV_IMG_PX_SIZE_ALPHA_BYTE"
    else:
        data_size = f"{w} * {h} * LV_COLOR_SIZE / 8"

    lines = []
    ap_ = lines.append
    ap_("#ifdef __has_include")
    ap_('    #if __has_include("lvgl.h")')
    ap_("        #ifndef LV_LVGL_H_INCLUDE_SIMPLE")
    ap_("            #define LV_LVGL_H_INCLUDE_SIMPLE")
    ap_("        #endif")
    ap_("    #endif")
    ap_("#endif")
    ap_("")
    ap_("#if defined(LV_LVGL_H_INCLUDE_SIMPLE)")
    ap_('    #include "lvgl.h"')
    ap_("#else")
    ap_('    #include "lvgl/lvgl.h"')
    ap_("#endif")
    ap_("")
    ap_("#ifndef LV_ATTRIBUTE_MEM_ALIGN")
    ap_("#define LV_ATTRIBUTE_MEM_ALIGN")
    ap_("#endif")
    ap_("")
    ap_(f"#ifndef LV_ATTRIBUTE_IMG_IMG_{up}")
    ap_(f"#define LV_ATTRIBUTE_IMG_IMG_{up}")
    ap_("#endif")
    ap_("")
    ap_(f"const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST "
        f"LV_ATTRIBUTE_IMG_IMG_{up} uint8_t img_{name}_map[] = {{")
    ap_("#if LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP == 0")
    row = []
    for i, byte in enumerate(data):
        row.append(f"0x{byte:02x},")
        if len(row) == 32:
            ap_("  " + " ".join(row))
            row = []
    if row:
        ap_("  " + " ".join(row))
    ap_("#else")
    ap_("  #error \"png_to_lvgl.py output requires LV_COLOR_DEPTH==16, LV_COLOR_16_SWAP==0\"")
    ap_("#endif")
    ap_("};")
    ap_("")
    ap_(f"const lv_img_dsc_t img_{name} = {{")
    ap_(f"  .header.cf = {cf},")
    ap_("  .header.always_zero = 0,")
    ap_("  .header.reserved = 0,")
    ap_(f"  .header.w = {w},")
    ap_(f"  .header.h = {h},")
    ap_(f"  .data_size = {data_size},")
    ap_(f"  .data = img_{name}_map,")
    ap_("};")
    ap_("")

    with open(args.out, "w") as f:
        f.write("\n".join(lines))

    print(f"wrote {args.out}: img_{name} {w}x{h} {args.mode} "
          f"({len(data)} bytes)", file=sys.stderr)


if __name__ == "__main__":
    main()
