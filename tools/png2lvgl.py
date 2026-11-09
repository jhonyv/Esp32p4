#!/usr/bin/env python3
"""Convierte un PNG a un archivo .c con un lv_image_dsc_t (LVGL 9, ARGB8888).

Uso: python png2lvgl.py entrada.png salida.c nombre_variable [escala]
Requiere Pillow (pip install pillow).
"""
import sys
from PIL import Image


def convert(src, dst, name, scale=1.0):
    img = Image.open(src).convert("RGBA")
    if scale != 1.0:
        img = img.resize((round(img.width * scale), round(img.height * scale)), Image.LANCZOS)
    w, h = img.size
    raw = img.tobytes()
    data = bytearray(raw)
    data[0::4], data[2::4] = raw[2::4], raw[0::4]  # ARGB8888 en memoria little endian: B, G, R, A
    with open(dst, "w") as f:
        f.write('#include "lvgl.h"\n\n')
        f.write(f"/* {src.split('/')[-1]} -> {w}x{h} ARGB8888 (escala {scale}) */\n")
        f.write(f"static const uint8_t {name}_map[] __attribute__((aligned(4))) = {{\n")
        for i in range(0, len(data), 24):
            f.write("    " + ",".join(f"0x{v:02x}" for v in data[i:i + 24]) + ",\n")
        f.write("};\n\n")
        f.write(f"const lv_image_dsc_t {name} = {{\n")
        f.write("    .header.magic = LV_IMAGE_HEADER_MAGIC,\n")
        f.write("    .header.cf = LV_COLOR_FORMAT_ARGB8888,\n")
        f.write(f"    .header.w = {w},\n    .header.h = {h},\n    .header.stride = {w * 4},\n")
        f.write(f"    .data_size = sizeof({name}_map),\n    .data = {name}_map,\n}};\n")
    return w, h


if __name__ == "__main__":
    a = sys.argv
    print(convert(a[1], a[2], a[3], float(a[4]) if len(a) > 4 else 1.0))
