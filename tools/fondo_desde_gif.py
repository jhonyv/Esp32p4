#!/usr/bin/env python3
"""Arma los cuadros de un fondo animado (fondo_00.jpg, fondo_01.jpg, ...) para la SD.

El ESP elige el cuadro segun las RPM: fondo_00 = ralenti, el ultimo = corte.

Uso:
    python fondo_desde_gif.py animacion.gif carpeta_salida [cuadros]
    python fondo_desde_gif.py carpeta_con_imagenes carpeta_salida [cuadros]

- cuadros: cuantas fotos generar (por defecto 24). Se toman repartidas a lo largo
  de la animacion.
- Desde un video: primero sacale las fotos con ffmpeg, por ejemplo
      ffmpeg -i video.mp4 -vf fps=10 cuadros/%04d.png
  y despues pasale la carpeta "cuadros" a este script.

Las fotos salen de 1024x608 (la pantalla es de 1024x600; los 8 pixeles extra de
abajo no se ven) porque el decodificador JPG por hardware del P4 necesita medidas
multiplos de 16. Requiere Pillow (pip install pillow).
"""
import os
import sys
from PIL import Image, ImageSequence

W, H, H_HW = 1024, 600, 608


def load_frames(src):
    if os.path.isdir(src):
        names = sorted(n for n in os.listdir(src)
                       if n.lower().endswith((".png", ".jpg", ".jpeg", ".bmp", ".webp")))
        return [Image.open(os.path.join(src, n)).convert("RGB") for n in names]
    img = Image.open(src)
    return [f.convert("RGB") for f in ImageSequence.Iterator(img)]


def fit(img):
    # escala para cubrir 1024x600 y recorta lo que sobra (centrado)
    s = max(W / img.width, H / img.height)
    img = img.resize((round(img.width * s), round(img.height * s)), Image.LANCZOS)
    x, y = (img.width - W) // 2, (img.height - H) // 2
    img = img.crop((x, y, x + W, y + H))
    out = Image.new("RGB", (W, H_HW))
    out.paste(img, (0, 0))
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    frames = load_frames(sys.argv[1])
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 24
    count = max(1, min(count, len(frames), 100))
    os.makedirs(sys.argv[2], exist_ok=True)
    for i in range(count):
        src = frames[round(i * (len(frames) - 1) / max(1, count - 1))]
        path = os.path.join(sys.argv[2], f"fondo_{i:02d}.jpg")
        fit(src).save(path, quality=85, optimize=True, progressive=False)
    print(f"Listo: {count} cuadros en {sys.argv[2]}")


if __name__ == "__main__":
    main()
