/*
 * prelude.h: forced in front of every firmware file in the simulator build
 * (-include). The firmware's printf output feeds the web console through a
 * stdio driver (commands.c); on the host that is sim_printf, which writes to
 * the terminal and to the drivers the firmware enabled.
 */
#pragma once
#include <stdio.h>
#include "sim_sdk.h"
#define printf  sim_printf
#define putchar sim_putchar
#define puts    sim_puts
