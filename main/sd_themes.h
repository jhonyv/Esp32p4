#pragma once

#include <stddef.h>
#include "dashboard.h"

/* Carpeta en la SD donde van los dashboards: una subcarpeta por dashboard */
#ifndef SD_THEMES_DIR
#define SD_THEMES_DIR    "/sdcard/dashboards"
#endif
/* La misma carpeta vista desde LVGL (letra S: = /sdcard, ver CONFIG_LV_FS_STDIO_*) */
#ifndef SD_THEMES_LV_DIR
#define SD_THEMES_LV_DIR "S:/dashboards"
#endif

/* Busca dashboards en la SD (ya montada) y los arma. Agrega hasta max temas
 * en out y devuelve cuantos agrego. Llamar con el lock de LVGL tomado.
 *
 * Archivos que se reconocen dentro de cada carpeta:
 *   fondo.png / fondo.jpg            fondo fijo
 *   fondo_00.jpg, fondo_01.jpg ...   fondo animado: el cuadro se elige segun las RPM
 *   bar.png + bar_black.png          barra de RPM (encendida + apagada)
 *   reloj1.png + aguja1.png ...      relojes con aguja (hasta 6, ver sd_themes.c)
 *   anim1_00.png, anim1_01.png ...   animaciones que cambian de foto segun un sensor (hasta 4)
 *   fuente.ttf / .otf                letra propia
 *   dashboard.txt                    opcional, ajustes como "rpm_max=8000" */
size_t sd_themes_load(const dash_theme_t **out, size_t max);
