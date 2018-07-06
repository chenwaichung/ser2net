// SPDX-License-Identifier: GPL-2.0+

/*
 * serialsim - Emulate a serial device in a loopback and/or pipe
 */

/*
 * TTY IOCTLs for controlling the modem control and for error injection.
 * See serialsim.c for details.
 */

#ifndef LINUX_SERIALSIM_H
#define LINUX_SERIALSIM_H

#include <linux/ioctl.h>
#include <asm/termbits.h>

#define TIOCSERSNULLMODEM	0x54e4
#define TIOCSERSREMMCTRL	0x54e5
#define TIOCSERSREMERR		0x54e6
#define TIOCSERGREMTERMIOS	_IOR('T', 0xe7, struct termios2)

#endif
