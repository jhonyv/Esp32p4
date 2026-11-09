#pragma once

#include <stdbool.h>

/*
 * Modo USB: la pantalla se conecta a la compu como un pendrive y muestra la
 * tarjeta SD, para copiar dashboards sin sacarla.
 *
 * Se usa el puerto Type-C "USB 1.1 FS" (el mismo con el que se programa).
 * Mientras dura el modo USB ese puerto deja de servir para programar / ver
 * el log; al tocar SALIR la pantalla se reinicia y vuelve a la normalidad.
 */

/* Deja el puerto USB como viene de fabrica (programar / log). Llamar al arrancar. */
void usb_mode_restore_phy(void);

/* Devuelve true si en el arranque anterior se pidio entrar al modo USB (y borra el pedido). */
bool usb_mode_requested(void);

/* Guarda el pedido de modo USB y reinicia la pantalla. No vuelve. */
void usb_mode_request(void);

/* Arma la pantalla del modo USB y expone la SD a la compu.
 * Llamar con el display bloqueado (bsp_display_lock), sin haber montado la SD. */
void usb_mode_run(void);
