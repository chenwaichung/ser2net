/*
 *  ser2net - A program for allowing telnet connection to serial ports
 *  Copyright (C) 2001  Corey Minyard <minyard@acm.org>
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

#ifndef UTILS
#define UTILS

#include <stdbool.h>

/* Returns true if the string is a numeric zero, false if not. */
int strisallzero(const char *str);

/*
 * Returns true if the first strlen(prefix) characters of s are the
 * same as prefix.  If true is returned, end is set to the character
 * after the last byte that compares.
 */
int cmpstrval(const char *s, const char *prefix, unsigned int *end);

/* Scan for a positive integer, and return it.  Return -1 if the
   integer was invalid.  Spaces are not handled. */
int scan_int(const char *str);

/* Write the data completely out, return without comment on error. */
void write_ignore_fail(int fd, const char *data, size_t count);

/* Separate out a string into an argv array, returning the argc/argv
   values given.  Returns -ENOMEM when out of memory or -EINVAL if
   there is something wrong with the string.  seps is a list of
   separators, parameters will be separated by that vlaue.  If seps is
   NULL it will default to the equivalent of isspace().  The argv
   array must be freed with str_to_argv_free(). */
int str_to_argv(const char *s, int *argc, char ***argv, char *seps);

/* Free the return of str_to_argv */
void str_to_argv_free(int argc, char **argv);

struct absout {
    int (*out)(struct absout *e, const char *str, ...);
    void *data;
};

/*
 * Given an integer baud rate (300 for 300baud, for instance) if
 * "cisco" is false, or an cisco IOS baud rate if "cisco" is true,
 * return the termios value for the given baud rate.  Returns 1 if
 * successful and 0 if the given integer baud rate is not supported.
 * If successful, the integer baud rate is returned in bps if that is
 * not NULL;
 */
int get_baud_rate(int rate, int *val, bool cisco, int *bps);

/*
 * Given the termios value in "baud_rate", return either the actual
 * integer baud rate in "val" if cisco is false, or the Cisco IOS baud
 * rate in "val" if cisco is true.  Always return the actual integer
 * baud rate in bps if that is not NULL.  If the baud rate is not
 * supported, val is set to zero and bps is undefined.
 */
void get_rate_from_baud_rate(int baud_rate, int *val, bool cisco, int *bps);

#endif /* UTILS */
