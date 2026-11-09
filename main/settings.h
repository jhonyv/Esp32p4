#pragma once

#include <stdbool.h>

/* Menu de configuracion a pantalla completa. Mientras esta abierto el dashboard
 * queda en pausa (no se dibuja ni se animan los fondos), asi el menu no se traba.
 * Se llama desde el LVGL (con la pantalla tomada), por ejemplo al mantener apretado. */
void settings_open(void);

bool settings_is_open(void);

/* Los pone dashboard.c. Hasta que esten, settings.c usa versiones vacias. */
void dashboard_set_paused(bool paused);
void dashboard_menu_open(void);
