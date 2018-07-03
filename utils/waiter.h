/*
 *  ser2net - A program for allowing telnet connection to serial ports
 *  Copyright (C) 2018  Corey Minyard <minyard@acm.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* Utils for waiting and handling a select loop. */

#ifndef WAITER_H
#define WAITER_H

#include "selector.h"

typedef struct waiter_s waiter_t;

waiter_t *alloc_waiter(struct selector_s *sel, int wake_sig);

void free_waiter(waiter_t *waiter);

int wait_for_waiter_timeout(waiter_t *waiter, unsigned int count,
			    struct timeval *timeout);

void wait_for_waiter(waiter_t *waiter, unsigned int count);

int wait_for_waiter_timeout_intr(waiter_t *waiter, unsigned int count,
				 struct timeval *timeout);

int wait_for_waiter_intr(waiter_t *waiter, unsigned int count);

void wake_waiter(waiter_t *waiter);

#endif /* WAITER_H */
