# Genera imagenes de ejemplo: barra de RPM (apagada/encendida) y luces de cambio (cuadros)
import sys
from PIL import Image, ImageDraw, ImageFilter
out = sys.argv[1]

def seg_color(t):
    if t < 0.6: return (40, 220, 90)
    if t < 0.85: return (255, 200, 0)
    return (255, 40, 30)

W, H, N = 1000, 56, 50
for name, lit in (("rpm_bar_off", False), ("rpm_bar_on", True)):
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    sw = W / N
    for i in range(N):
        t = i / (N - 1)
        c = seg_color(t)
        if not lit:
            c = tuple(int(v * 0.18) + 25 for v in c)
        x0 = int(i * sw) + 2
        hh = int(H * (0.45 + 0.55 * t))  # segmentos mas altos hacia la derecha
        d.rounded_rectangle((x0, H - hh, int((i + 1) * sw) - 2, H - 1), radius=3, fill=c + (255,))
    if lit:
        glow = img.filter(ImageFilter.GaussianBlur(4))
        img = Image.alpha_composite(glow, img)
    img.save(f"{out}/{name}.png")

# Luces de cambio: 10 LEDs, 11 cuadros (0 = todo apagado)
LW, LH, L = 400, 40, 10
for f in range(L + 1):
    img = Image.new("RGBA", (LW, LH), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for i in range(L):
        c = (40, 220, 90) if i < 5 else (255, 200, 0) if i < 8 else (255, 40, 30)
        on = i < f
        if f == L:  # al limite, todas en rojo
            c = (255, 40, 30)
        col = c if on else tuple(int(v * 0.15) + 20 for v in c)
        cx = int((i + 0.5) * LW / L)
        d.ellipse((cx - 14, 6, cx + 14, 34), fill=col + (255,))
    if f:
        img = Image.alpha_composite(img.filter(ImageFilter.GaussianBlur(3)), img)
    img.save(f"{out}/shift_{f:02d}.png")
