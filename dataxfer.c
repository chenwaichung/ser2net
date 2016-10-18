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

/* This code handles the actual transfer of data between the serial
   ports and the TCP ports. */


#include <sys/time.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <syslog.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <fcntl.h>

#include "ser2net.h"
#include "dataxfer.h"
#include "selector.h"
#include "utils.h"
#include "telnet.h"
#include "devio.h"
#include "buffer.h"
#include "locking.h"
#include "led.h"

#define SERIAL "term"
#define NET    "tcp "

/** BASED ON sshd.c FROM openssh.com */
#ifdef HAVE_TCPD_H
#include <tcpd.h>
static char *progname = "ser2net";
#endif /* HAVE_TCPD_H */

/* States for the tcp_to_dev_state and dev_to_tcp_state. */
#define PORT_UNCONNECTED		0 /* The TCP port is not connected
                                             to anything right now. */
#define PORT_WAITING_INPUT		1 /* Waiting for input from the
					     input side. */
#define PORT_WAITING_OUTPUT_CLEAR	2 /* Waiting for output to clear
					     so I can send data. */
#define PORT_CLOSING			3 /* Waiting for output close
					     string to be sent. */
char *state_str[] = { "unconnected", "waiting input", "waiting output",
		      "closing" };

#define PORT_DISABLED		0 /* The port is not open. */
#define PORT_RAW		1 /* Port will not do telnet negotiation. */
#define PORT_RAWLP		2 /* Port will not do telnet negotiation and
                                     termios setting, open for output only. */
#define PORT_TELNET		3 /* Port will do telnet negotiation. */
char *enabled_str[] = { "off", "raw", "rawlp", "telnet" };

typedef struct trace_info_s
{
    int  hexdump;     /* output each block as a hexdump */
    int  timestamp;   /* preceed each line with a timestamp */
    char *filename;   /* open file.  NULL if not used */
    int  fd;          /* open file.  -1 if not used */
} trace_info_t;

typedef struct port_info port_info_t;
struct port_info
{
    DEFINE_LOCK(, lock)
    int            enabled;		/* If PORT_DISABLED, the port
					   is disabled and the TCP
					   accept port is not
					   operational.  If PORT_RAW,
					   the port is enabled and
					   will not do any telnet
					   negotiations.  If
					   PORT_RAWLP, the port is enabled
					   only for output without any
					   termios setting - it allows
					   to redirect /dev/lpX devices If
					   PORT_TELNET, the port is
					   enabled and it will do
					   telnet negotiations. */

    int            timeout;		/* The number of seconds to
					   wait without any I/O before
					   we shut the port down. */

    int            timeout_left;	/* The amount of time left (in
					   seconds) before the timeout
					   goes off. */

    sel_timer_t *timer;			/* Used to timeout when the no
					   I/O has been seen for a
					   certain period of time. */

    sel_timer_t *send_timer;		/* Used to delay a bit when
					   waiting for characters to
					   batch up as many characters
					   as possible. */
    bool send_timer_running;


    sel_runner_t *runshutdown;		/* Used to run things at the
					   base context.  This way we
					   don't have to worry that we
					   are running inside a
					   handler context that needs
					   to be waited for exit. */

    int chardelay;                      /* The amount of time to wait after
					   receiving a character before
					   sending it, unless we receive
					   another character.  Based on
					   bit rate. */

    int bps;				/* Bits per second rate. */
    int bpc;				/* Bits per character. */

    bool enable_chardelay;

    int  chardelay_scale;		/* The number of character
					   periods to wait for the
					   next character, in tenths of
					   a character period. */
    int  chardelay_min;			/* The minimum chardelay, in
					   microseconds. */
    int  chardelay_max;			/* Maximum amount of time to
					   wait before sending the data. */
    struct timeval send_time;		/* When using chardelay, the
					   time when we will send the
					   data, no matter what, set
					   by chardelay_max. */

    /* Information about the TCP port. */
    char               *portname;       /* The name given for the port. */
    int                is_stdio;	/* Do stdio on the port? */
    struct addrinfo    *ai;		/* The address list for the portname. */
    int            *acceptfds;		/* The file descriptor used to
					   accept connections on the
					   TCP port. */
    unsigned int   nr_acceptfds;
    waiter_t       *accept_waiter;      /* Wait for accept changes. */

    int            tcpfd;		/* When connected, the file
                                           descriptor for the TCP
                                           port used for I/O. */
    struct sockaddr_storage remote;	/* The socket address of who
					   is connected to this port. */
    unsigned int tcp_bytes_received;    /* Number of bytes read from the
					   TCP port. */
    unsigned int tcp_bytes_sent;        /* Number of bytes written to the
					   TCP port. */
    struct sbuf *banner;		/* Outgoing banner */

    unsigned int dev_bytes_received;    /* Number of bytes read from the
					   device. */
    unsigned int dev_bytes_sent;        /* Number of bytes written to the
					   device. */
    int (*tcp_write_handler)(port_info_t *);

    /* Information use when transferring information from the TCP port
       to the terminal device. */
    int            tcp_to_dev_state;		/* State of transferring
						   data from the TCP port
                                                   to the device. */
    int            tcp_to_dev_bufsize;
    struct sbuf    tcp_to_dev;			/* Buffer struct for
						   TCP to device
						   transfers. */
    struct controller_info *tcp_monitor; /* If non-null, send any input
					    received from the TCP port
					    to this controller port. */
    struct sbuf *devstr;		 /* Outgoing string */

    /* Information use when transferring information from the terminal
       device to the TCP port. */
    int            dev_to_tcp_state;		/* State of transferring
						   data from the device to
                                                   the TCP port. */
    int            dev_to_tcp_bufsize;
    struct sbuf    dev_to_tcp;			/* Buffer struct for
						   device to TCP
						   transfers. */
    struct controller_info *dev_monitor; /* If non-null, send any input
					    received from the device
					    to this controller port. */

    struct port_info *next;		/* Used to keep a linked list
					   of these. */

    int config_num; /* Keep track of what configuration this was last
		       updated under.  Setting to -1 means to delete
		       the port when the current session is done. */

    struct port_info *new_config; /* If the port is reconfigged while
				     open, this will hold the new
				     configuration that should be
				     loaded when the current session
				     is done. */

    /* Data for the telnet processing */
    telnet_data_t tn_data;
    bool sending_tn_data; /* Are we sending tn data at the moment? */

    /* Is RFC 2217 mode enabled? */
    int is_2217;

    /* Masks for RFC 2217 */
    unsigned char linestate_mask;
    unsigned char modemstate_mask;
    unsigned char last_modemstate;

    /* Allow RFC 2217 mode */
    int allow_2217;

    /* Send a break if we get a sync command? */
    int telnet_brk_on_sync;

    /* kickolduser mode */
    int kickolduser_mode;

    /* Banner to display at startup, or NULL if none. */
    char *bannerstr;

    /* RFC 2217 signature. */
    char *signaturestr;

    /* String to send to device at startup, or NULL if none. */
    char *openstr;

    /* String to send to device at close, or NULL if none. */
    char *closestr;

    /*
     * Close on string to shutdown connection when received from
     * serial side, or NULL if none.
     */
    char *closeon;
    unsigned int closeon_pos;
    unsigned int closeon_len;

    /*
     * Close the session when all the output has been written to the
     * TCP port.
     */
    bool close_on_output_done;

    /*
     * File to read/write trace, NULL if none.  If the same, then
     * trace information is in the same file, only one open is done.
     */
    trace_info_t trace_read;
    trace_info_t trace_write;
    trace_info_t trace_both;

    /*
     * Pointers to the above, that way if two are the same file we can just
     * set up one and point both to it.
     */
    trace_info_t *tr;
    trace_info_t *tw;
    trace_info_t *tb;

    struct devio io; /* For handling I/O operation to the device */
    int (*dev_write_handler)(port_info_t *);
    waiter_t     *waiter;      /* Wait for stuff at shutdown. */


#if HAVE_DECL_TIOCSRS485
    struct serial_rs485 *rs485conf;
#endif

    /*
     * LED to flash for serial traffic
     */
    struct led_s *led_tx;
    struct led_s *led_rx;
};

DEFINE_LOCK_INIT(static, ports_lock)
port_info_t *ports = NULL; /* Linked list of ports. */

static void shutdown_port(port_info_t *port, char *reason);
static void finish_shutdown_port(sel_runner_t *runner, void *cb_data);

/* The init sequence we use. */
static unsigned char telnet_init_seq[] = {
    TN_IAC, TN_WILL, TN_OPT_SUPPRESS_GO_AHEAD,
    TN_IAC, TN_WILL, TN_OPT_ECHO,
    TN_IAC, TN_DONT, TN_OPT_ECHO,
    TN_IAC, TN_DO,   TN_OPT_BINARY_TRANSMISSION,
};

/* Our telnet command table. */
static void com_port_handler(void *cb_data, unsigned char *option, int len);
static int com_port_will(void *cb_data);

static struct telnet_cmd telnet_cmds[] = 
{
    /*                        I will,  I do,  sent will, sent do */
    { TN_OPT_SUPPRESS_GO_AHEAD,	   0,     1,          1,       0, },
    { TN_OPT_ECHO,		   0,     1,          1,       1, },
    { TN_OPT_BINARY_TRANSMISSION,  1,     1,          0,       1, },
    { TN_OPT_COM_PORT,		   1,     1,          0,       0, 0, 0,
      com_port_handler, com_port_will },
    { 255 }
};

static void
add_usec_to_timeval(struct timeval *tv, int usec)
{
    tv->tv_usec += usec;
    while (usec >= 1000000) {
	usec -= 1000000;
	tv->tv_sec += 1;
    }
}

static int
sub_timeval_us(struct timeval *left,
	       struct timeval *right)
{
    struct timeval dest;

    dest.tv_sec = left->tv_sec - right->tv_sec;
    dest.tv_usec = left->tv_usec - right->tv_usec;
    while (dest.tv_usec < 0) {
	dest.tv_usec += 1000000;
	dest.tv_sec--;
    }

    return (dest.tv_sec * 1000000) + dest.tv_usec;
}


/*
 * Generic output function for using a controller output for
 * abstract I/O.
 */
static int
cntrl_absout(struct absout *o, const char *str, ...)
{
    va_list ap;
    int rv;

    va_start(ap, str);
    rv = controller_voutputf(o->data, str, ap);
    va_end(ap);
    return rv;
}

/*
 * Like above but does a new line at the end of the output, generally
 * for error output.
 */
static int
cntrl_abserrout(struct absout *o, const char *str, ...)
{
    va_list ap;
    int rv;

    va_start(ap, str);
    rv = controller_voutputf(o->data, str, ap);
    va_end(ap);
    rv += controller_outputf(o->data, "\r\n");
    return rv;
}

static int
init_port_data(port_info_t *port)
{
    port->enabled = PORT_DISABLED;
    port->tcpfd = -1;
    
    port->tcp_to_dev_state = PORT_UNCONNECTED;
    port->dev_to_tcp_state = PORT_UNCONNECTED;
    port->trace_read.fd = -1;
    port->trace_write.fd = -1;
    port->trace_both.fd = -1;
#if HAVE_DECL_TIOCSRS485
    port->rs485conf = NULL;
#endif

    port->allow_2217 = find_default_int("remctl");
    port->telnet_brk_on_sync = find_default_int("telnet_brk_on_sync");
    port->kickolduser_mode = find_default_int("kickolduser");
    port->enable_chardelay = find_default_int("chardelay");
    port->chardelay_scale = find_default_int("chardelay-scale");
    port->chardelay_min = find_default_int("chardelay-min");
    port->chardelay_max = find_default_int("chardelay-max");
    port->dev_to_tcp_bufsize = find_default_int("dev-to-tcp-bufsize");
    port->tcp_to_dev_bufsize = find_default_int("tcp-to-dev-bufsize");

    if (buffer_init(&port->tcp_to_dev, NULL, port->tcp_to_dev_bufsize))
	return ENOMEM;
    if (buffer_init(&port->dev_to_tcp, NULL, port->dev_to_tcp_bufsize))
	return ENOMEM;

    port->led_tx = NULL;
    port->led_rx = NULL;

    return 0;
}

static void
reset_timer(port_info_t *port)
{
    port->timeout_left = port->timeout;
}


static int
timestamp(trace_info_t *t, char *buf, int size)
{
    time_t result;
    if (!t->timestamp)
        return 0;
    result = time(NULL);
    return strftime(buf, size, "%Y/%m/%d %H:%M:%S ", localtime(&result));
}

static int
trace_write_end(char *out, int size, unsigned char *start, int col)
{
    int pos=0, w;

    strncat(out, " |", size-pos);
    pos += 2;
    for(w = 0; w < col; w++) {
        pos += snprintf(out + pos, size - pos, "%c",
			isprint(start[w]) ? start[w] : '.');
    }
    strncat(out+pos, "|\n", size-pos);
    pos += 2;
    return pos;
}

int
trace_write(port_info_t *port, trace_info_t *t, unsigned char *buf,
	    unsigned int buf_len, char *prefix)
{
    int rv = 0, w, col = 0, pos, file = t->fd;
    unsigned int q;
    static char out[1024];
    unsigned char *start;

    if (buf_len == 0) 
        return 0;

    if (!t->hexdump)
        return write(file, buf, buf_len);

    pos = timestamp(t, out, sizeof(out));    
    pos += snprintf(out + pos, sizeof(out) - pos, "%s ", prefix);
        
    start = buf;
    for (q = 0; q < buf_len; q++) {
        pos += snprintf(out + pos, sizeof(out) - pos, "%02x ", buf[q]);
        col++;
        if (col >= 8) {
            pos += trace_write_end(out + pos, sizeof(out) - pos, start, col);
            rv = write(file, out, strlen(out));
            if (rv < 0)
                return rv;           
            pos = timestamp(t, out, sizeof(out));    
            pos += snprintf(out + pos, sizeof(out) - pos, "%s ", prefix);
            col = 0;
            start = buf + q + 1;
        }
    }
    if (col > 0) {
        for (w = 8; w > col; w--) {
            strncat(out + pos, "   ", sizeof(out) - pos);
            pos += 3;
        }
        pos += trace_write_end(out + pos, sizeof(out) - pos, start, col);
        rv = write(file, out, strlen(out));
        if (rv < 0)
            return rv;
    }
    return buf_len;
}

static void
do_trace(port_info_t *port, trace_info_t *t, unsigned char *buf,
	 unsigned int buf_len, char *prefix)
{
    int rv;

    while (buf_len > 0) {
    retry_write:
	rv = trace_write(port, t, buf, buf_len, prefix);
	if (rv == -1) {
	    char errbuf[128];
	    int err = errno;

	    if (err == EINTR)
		goto retry_write;
	    
	    /* Fatal error writing to the file, log it and close the file. */

	    if (strerror_r(err, errbuf, sizeof(errbuf)) == -1)
		syslog(LOG_ERR, "Unable write to trace file on port %s: %d",
		       port->portname, err);
	    else
		syslog(LOG_ERR, "Unable to write to trace file on port %s: %s",
		       port->portname, errbuf);
	    
	    close(t->fd);
	    t->fd = -1;
	    return;
	}

	/* Handle a partial write */
	buf_len -= rv;
	buf += rv;
    }
}

static void
hf_out(port_info_t *port, char *buf, int len)
{
    if (port->tr && port->tr->timestamp)
        write_ignore_fail(port->tr->fd, buf, len);

    /* don't output to write file if it's the same as read file */
    if (port->tw && port->tw != port->tr && port->tw->timestamp)
        write_ignore_fail(port->tw->fd, buf, len);

    /* don't output to both file if it's the same as read or write file */
    if (port->tb && port->tb != port->tr && port->tb != port->tw
		&& port->tb->timestamp)
        write_ignore_fail(port->tb->fd, buf, len);    
}

static void
header_trace(port_info_t *port)
{
    static char buf[1024];
    static trace_info_t tr = { 1, 1, NULL, -1 };
    int len = 0;
    char portstr[NI_MAXSERV];

    len += timestamp(&tr, buf, sizeof(buf));
    len += snprintf(buf + len, sizeof(buf) - len, "OPEN (");
    getnameinfo((struct sockaddr *) &(port->remote), sizeof(port->remote),
		buf + len, sizeof(buf) - len,
		portstr, sizeof(portstr), NI_NUMERICHOST);
    len += strlen(buf + len);
    if ((sizeof(buf) - len) > 2) {
	buf[len] = ':';
	len++;
    }
    strncpy(buf + len, portstr, sizeof(buf) - len);
    len += strlen(buf + len);
    len += snprintf(buf + len, sizeof(buf) - len, ")\n");

    hf_out(port, buf, len);
}

static void
footer_trace(port_info_t *port, char *reason)
{
    static char buf[1024];
    static trace_info_t tr = { 1, 1, NULL, -1 };
    int len = 0;

    len += timestamp(&tr, buf, sizeof(buf));
    len += snprintf(buf + len, sizeof(buf), "CLOSE (%s)\n", reason);

    hf_out(port, buf, len);
}

static int
handle_tcp_send(port_info_t *port)
{
    int count;

 retry_write:
    count = write(port->tcpfd, port->dev_to_tcp.buf, port->dev_to_tcp.cursize);
    if (count == -1) {
	if (errno == EINTR) {
	    /* EINTR means we were interrupted, just retry. */
	    goto retry_write;
	}

	if (errno == EAGAIN || errno == EWOULDBLOCK) {
	    /* This was due to O_NONBLOCK, we need to shut off the reader
	       and start the writer monitor. */
	    port->io.f->read_handler_enable(&port->io, 0);
	    sel_set_fd_write_handler(ser2net_sel, port->tcpfd,
				     SEL_FD_HANDLER_ENABLED);
	    port->dev_to_tcp_state = PORT_WAITING_OUTPUT_CLEAR;
	} else if (errno == EPIPE) {
	    shutdown_port(port, "EPIPE");
	    return -1;
	} else {
	    /* Some other bad error. */
	    syslog(LOG_ERR, "The tcp write for port %s had error: %m",
		   port->portname);
	    shutdown_port(port, "tcp write error");
	    return -1;
	}
    } else {
	port->tcp_bytes_sent += count;
	port->dev_to_tcp.cursize -= count;
	if (port->dev_to_tcp.cursize != 0) {
	    /* We didn't write all the data, shut off the reader and
               start the write monitor. */
	    port->dev_to_tcp.pos = count;
	    port->io.f->read_handler_enable(&port->io, 0);
	    sel_set_fd_write_handler(ser2net_sel, port->tcpfd,
				     SEL_FD_HANDLER_ENABLED);
	    port->dev_to_tcp_state = PORT_WAITING_OUTPUT_CLEAR;
	} else if (port->close_on_output_done) {
	    shutdown_port(port, "closeon sequence found");
	    port->close_on_output_done = false;
	}
    }

    return 0;
}

void
send_timeout(selector_t  *sel,
	     sel_timer_t *timer,
	     void        *data)
{
    port_info_t *port = (port_info_t *) data;

    LOCK(port->lock);

    if (port->dev_to_tcp_state == PORT_CLOSING) {
	UNLOCK(port->lock);
	return;
    }

    port->send_timer_running = false;
    if (port->dev_to_tcp.cursize > 0)
	handle_tcp_send(port);
    UNLOCK(port->lock);
}

/* Data is ready to read on the serial port. */
static void
handle_dev_fd_read(struct devio *io)
{
    port_info_t *port = (port_info_t *) io->user_data;
    int count;
    int curend;
    bool send_now = false;

    LOCK(port->lock);
    curend = port->dev_to_tcp.cursize;
    port->dev_to_tcp.pos = 0;
    if (port->enabled == PORT_TELNET) {
	/* Leave room for IACs. */
	count = port->io.f->read(&port->io, port->dev_to_tcp.buf + curend,
				 (port->dev_to_tcp.maxsize - curend) / 2);
    } else {
	count = port->io.f->read(&port->io, port->dev_to_tcp.buf + curend,
				 port->dev_to_tcp.maxsize - curend);
    }

    if (count <= 0) {
	if (curend != 0) {
	    /* We still have data to send. */
	    send_now = true;
	    count = 0;
	    goto do_send;
	}

	if (count < 0) {
	    if (errno == EAGAIN || errno == EWOULDBLOCK) {
		/* Nothing to read, just return. */
		goto out_unlock;
	    }

	    /* Got an error on the read, shut down the port. */
	    syslog(LOG_ERR, "dev read error for port %s: %m", port->portname);
	    shutdown_port(port, "dev read error");
	} else if (count == 0) {
	    /* The port got closed somehow, shut it down. */
	    shutdown_port(port, "closed port");
	}
	goto out;
    }

    if (port->dev_monitor != NULL && count > 0) {
	controller_write(port->dev_monitor,
			 (char *) port->dev_to_tcp.buf + curend,
			 count);
    }

 do_send:
    if (port->closeon) {
	int i;

	for (i = 0; i < count; i++) {
	    if (port->dev_to_tcp.buf[i] == port->closeon[port->closeon_pos]) {
		port->closeon_pos++;
		if (port->closeon_pos >= port->closeon_len) {
		    port->close_on_output_done = true;
		    /* Ignore everything after the closeon string */
		    count = i + 1;
		    break;
		}
	    } else {
		port->closeon_pos = 0;
	    }
	}
    }

    if (port->tr)
	/* Do read tracing, ignore errors. */
	do_trace(port, port->tr, port->dev_to_tcp.buf, count, SERIAL);
    if (port->tb)
	/* Do both tracing, ignore errors. */
	do_trace(port, port->tb, port->dev_to_tcp.buf, count, SERIAL);

    if (port->led_rx) {
	led_flash(port->led_rx);
    }

    port->dev_bytes_received += count;

    if (port->enabled == PORT_TELNET) {
	int i, j;

	/* Double the IACs on a telnet stream.  This will fit because
	   we only use half the buffer for telnet connections. */
	for (i=0; i<count; i++) {
	    if (port->dev_to_tcp.buf[i] == 255) {
		for (j=count; j>i; j--)
		    port->dev_to_tcp.buf[j+1] = port->dev_to_tcp.buf[j];
		count++;
		i++;
		port->dev_to_tcp.buf[i] = 255;
	    }
	}
    }

    port->dev_to_tcp.cursize += count;

    if (send_now || port->dev_to_tcp.cursize == port->dev_to_tcp.maxsize ||
	port->chardelay == 0) {
    send_it:
	if (handle_tcp_send(port) == 0)
	    reset_timer(port);
    } else {
	struct timeval then;
	int delay;

	sel_get_monotonic_time(&then);
	if (port->send_timer_running) {
	    sel_stop_timer(port->send_timer);
	} else {
	    port->send_time = then;
	    add_usec_to_timeval(&port->send_time, port->chardelay_max);
	}
	delay = sub_timeval_us(&port->send_time, &then);
	if (delay > port->chardelay)
	    delay = port->chardelay;
	else if (delay < 0) {
	    port->send_timer_running = false;
	    goto send_it;
	}
	add_usec_to_timeval(&then, delay);
	sel_start_timer(port->send_timer, &then);
	port->send_timer_running = true;
    }
 out_unlock:
    UNLOCK(port->lock);
 out:
    return;
}

/* The serial port has room to write some data.  This is only activated
   if a write fails to complete, it is deactivated as soon as writing
   is available again. */
static int
dev_fd_write(port_info_t *port, struct sbuf *buf)
{
    int reterr, buferr;

    reterr = buffer_io_write(&port->io, buf, &buferr);
    if (reterr == -1) {
	syslog(LOG_ERR, "The dev write for port %s had error: %m",
	       port->portname);
	shutdown_port(port, "dev write error");
	return 1;
    }

    if (buffer_cursize(buf) == 0) {
	/* We are done writing, turn the reader back on. */
	sel_set_fd_read_handler(ser2net_sel, port->tcpfd,
				SEL_FD_HANDLER_ENABLED);
	port->io.f->write_handler_enable(&port->io, 0);
	port->tcp_to_dev_state = PORT_WAITING_INPUT;
    }

    reset_timer(port);
    return 0;
}

static int
handle_dev_fd_normal_write(port_info_t *port)
{
    return dev_fd_write(port, &port->tcp_to_dev);
}

static void
handle_dev_fd_write(struct devio *io)
{
    port_info_t *port = (port_info_t *) io->user_data;

    LOCK(port->lock);
    if (!port->dev_write_handler(port))
	UNLOCK(port->lock);
}

/* Handle an exception from the serial port. */
static void
handle_dev_fd_except(struct devio *io)
{
    port_info_t *port = (port_info_t *) io->user_data;

    LOCK(port->lock);
    syslog(LOG_ERR, "Select exception on device for port %s",
	   port->portname);
    shutdown_port(port, "fd exception");
}

/* Output the devstr buffer */
static int
handle_dev_fd_devstr_write(port_info_t *port)
{
    if (!dev_fd_write(port, port->devstr)) {
	if (buffer_cursize(port->devstr) == 0) {
	    port->dev_write_handler = handle_dev_fd_normal_write;
	    free(port->devstr->buf);
	    free(port->devstr);
	    port->devstr = NULL;
	}
	return 0;
    } else {
	return 1;
    }
}

/* Output the devstr buffer */
static int
handle_dev_fd_close_write(port_info_t *port)
{
    int reterr, buferr;

    reterr = buffer_io_write(&port->io, port->devstr, &buferr);
    if (reterr == -1) {
	syslog(LOG_ERR, "The dev write for port %s had error: %m",
	       port->portname);
	goto closeit;
    }

    if (buffer_cursize(port->devstr) != 0)
	return 0;

closeit:
    sel_run(port->runshutdown, finish_shutdown_port, port);
    return 0;
}

/* Data is ready to read on the TCP port. */
static void
handle_tcp_fd_read(int fd, void *data)
{
    port_info_t *port = (port_info_t *) data;
    int count;

    LOCK(port->lock);
    port->tcp_to_dev.pos = 0;
    count = read(fd, port->tcp_to_dev.buf, port->tcp_to_dev.maxsize);
    if (count < 0) {
	if (errno == EAGAIN || errno == EWOULDBLOCK) {
	    /* Nothing to read, just return. */
	    goto out_unlock;
	}

	/* Got an error on the read, shut down the port. */
	syslog(LOG_ERR, "read error for port %s: %m", port->portname);
	shutdown_port(port, "tcp read error");
	goto out;
    } else if (count == 0) {
	/* The other end closed the port, shut it down. */
	shutdown_port(port, "tcp read close");
	goto out;
    }
    port->tcp_to_dev.cursize = count;

    port->tcp_bytes_received += count;

    if (port->enabled == PORT_TELNET) {
	port->tcp_to_dev.cursize = process_telnet_data(port->tcp_to_dev.buf,
						       count,
						       &port->tn_data);
	if (port->tn_data.error) {
	    shutdown_port(port, "telnet output error");
	    goto out;
	}
	if (port->tcp_to_dev.cursize == 0) {
	    /* We are out of characters; they were all processed.  We
	       don't want to continue with 0, because that will mess
	       up the other processing and it's not necessary. */
	    goto out_unlock;
	}
    }

    if (port->tcp_monitor != NULL) {
	controller_write(port->tcp_monitor,
			 (char *) port->tcp_to_dev.buf,
			 port->tcp_to_dev.cursize);
    }

    if (port->tw)
	/* Do write tracing, ignore errors. */
	do_trace(port, port->tw,
		 port->tcp_to_dev.buf, port->tcp_to_dev.cursize, NET);
    if (port->tb)
	/* Do both tracing, ignore errors. */
	do_trace(port, port->tb,
		 port->tcp_to_dev.buf, port->tcp_to_dev.cursize, NET);

 retry_write:
    count = port->io.f->write(&port->io, port->tcp_to_dev.buf,
			      port->tcp_to_dev.cursize);
    if (count == -1) {
	if (errno == EINTR) {
	    /* EINTR means we were interrupted, just retry. */
	    goto retry_write;
	}

	if (errno == EAGAIN || errno == EWOULDBLOCK) {
	    /* This was due to O_NONBLOCK, we need to shut off the reader
	       and start the writer monitor. */
	    sel_set_fd_read_handler(ser2net_sel, port->tcpfd,
				    SEL_FD_HANDLER_DISABLED);
	    port->io.f->write_handler_enable(&port->io, 1);
	    port->tcp_to_dev_state = PORT_WAITING_OUTPUT_CLEAR;
	} else {
	    /* Some other bad error. */
	    syslog(LOG_ERR, "The dev write for port %s had error: %m",
		   port->portname);
	    shutdown_port(port, "dev write error");
	    goto out;
	}
    } else {
	if (port->led_tx) {
	    led_flash(port->led_tx);
	}
	port->dev_bytes_sent += count;
	port->tcp_to_dev.cursize -= count;
	if (port->tcp_to_dev.cursize != 0) {
	    /* We didn't write all the data, shut off the reader and
               start the write monitor. */
	    port->tcp_to_dev.pos = count;
	    sel_set_fd_read_handler(ser2net_sel, port->tcpfd,
				    SEL_FD_HANDLER_DISABLED);
	    port->io.f->write_handler_enable(&port->io, 1);
	    port->tcp_to_dev_state = PORT_WAITING_OUTPUT_CLEAR;
	}
    }

    reset_timer(port);
 out_unlock:
    UNLOCK(port->lock);
 out: /* shutdown_port frees the lock */
    return;
}

/* The TCP port has room to write some data.  This is only activated
   if a write fails to complete, it is deactivated as soon as writing
   is available again.  Returns 1 if it freed the lock, 0 otherwise.*/
static int
tcp_fd_write(port_info_t *port, struct sbuf *buf)
{
    telnet_data_t *td = &port->tn_data;
    int buferr, reterr;

    if (port->sending_tn_data) {
    send_tn_data:
	reterr = buffer_write(port->tcpfd, &td->out_telnet_cmd, &buferr);
	if (reterr == -1) {
	    if (buferr == EPIPE) {
		shutdown_port(port, "EPIPE");
		return 1;
	    } else {
		/* Some other bad error. */
		syslog(LOG_ERR, "The tcp write for port %s had error: %m",
		       port->portname);
		shutdown_port(port, "tcp write error");
		return 1;
	    }
	}

	if (buffer_cursize(&td->out_telnet_cmd) > 0) {
	    /* If we have more telnet command data to send, don't
	       send any real data. */
	    return 0;
	}
    }

    reterr = buffer_write(port->tcpfd, buf, &buferr);
    if (reterr == -1) {
	if (buferr == EPIPE) {
	    shutdown_port(port, "EPIPE");
	} else {
	    /* Some other bad error. */
	    syslog(LOG_ERR, "The tcp write for port %s had error: %m",
		   port->portname);
	    shutdown_port(port, "tcp write error");
	}
	return 1;
    }

    if (buffer_cursize(buf) == 0) {
	/* Start telnet data write when the data write is done. */
	if (buffer_cursize(&td->out_telnet_cmd) > 0) {
	    port->sending_tn_data = true;
	    goto send_tn_data;
	}

	/* We are done writing, turn the reader back on. */
	port->io.f->read_handler_enable(&port->io, 1);
	sel_set_fd_write_handler(ser2net_sel, port->tcpfd,
				 SEL_FD_HANDLER_DISABLED);
	port->dev_to_tcp_state = PORT_WAITING_INPUT;

	if (port->close_on_output_done) {
	    port->close_on_output_done = false;
	    shutdown_port(port, "closeon sequence found");
	    return 1;
	}
    }

    reset_timer(port);
    return 0;
}

/* The TCP port has room to write some data.  This is only activated
   if a write fails to complete, it is deactivated as soon as writing
   is available again. */
static int
handle_tcp_fd_write(port_info_t *port)
{
    return tcp_fd_write(port, &port->dev_to_tcp);
}

static void
handle_tcp_fd_write_mux(int fd, void *data)
{
    port_info_t *port = (port_info_t *) data;

    LOCK(port->lock);
    if (!port->tcp_write_handler(port))
	UNLOCK(port->lock);
}

/* Handle an exception from the TCP port. */
static void
handle_tcp_fd_except(int fd, void *data)
{
    port_info_t *port = (port_info_t *) data;
    int rv, val;
    unsigned char c;
    int cmd_pos;

    /* We should have urgent data, a DATA MARK in the stream.  Read
       then urgent data (whose contents are irrelevant) then discard
       user data until we find the DATA_MARK command. */

    LOCK(port->lock);
    while ((rv = recv(fd, &c, 1, MSG_OOB)) > 0)
	;
    /* Ignore any errors, they are irrelevant. */

    if (port->enabled != PORT_TELNET)
	goto out;

    /* Flush the data in the local and device queue. */
    port->tcp_to_dev.cursize = 0;
    val = 0;
    port->io.f->flush(&port->io, &val);

    /* Store it if we last got an IAC, and abort any current
       telnet processing. */
    cmd_pos = port->tn_data.telnet_cmd_pos;
    if (cmd_pos != 1)
	cmd_pos = 0;
    port->tn_data.telnet_cmd_pos = 0;
    port->tn_data.suboption_iac = 0;

    while ((rv = read(fd, &c, 1)) > 0) {
	if (cmd_pos == 1) {
	    if (c == TN_DATA_MARK) {
		/* Found it. */
		if (port->telnet_brk_on_sync)
		    port->io.f->send_break(&port->io);
		break;
	    }
	    cmd_pos = 0;
	} else if (c == TN_IAC) {
	    cmd_pos = 1;
	}
    }
 out:
    UNLOCK(port->lock);
}

static void port_tcp_fd_cleared(int fd, void *cb_data);

static int
handle_tcp_fd_banner_write(port_info_t *port)
{
    if (!tcp_fd_write(port, port->banner)) {
	if (buffer_cursize(port->banner) == 0) {
	    port->tcp_write_handler = handle_tcp_fd_write;
	    free(port->banner->buf);
	    free(port->banner);
	    port->banner = NULL;
	}
	return 0;
    } else {
	return 1;
    }
}

static void
telnet_cmd_handler(void *cb_data, unsigned char cmd)
{
    port_info_t *port = cb_data;

    if ((cmd == TN_BREAK) || (port->telnet_brk_on_sync && cmd == TN_DATA_MARK))
	port->io.f->send_break(&port->io);
}

/* Called when the telnet code has output ready. */
static void
telnet_output_ready(void *cb_data)
{
    port_info_t *port = cb_data;

    /* If we are currently sending some data, wait until it is done.
       it might have IACs in it, and we don't want to split those. */
    if (buffer_cursize(&port->dev_to_tcp) != 0)
	return;
    if (port->banner && buffer_cursize(port->banner) != 0)
	return;

    port->sending_tn_data = true;
    port->io.f->read_handler_enable(&port->io, 0);
    sel_set_fd_write_handler(ser2net_sel, port->tcpfd,
			     SEL_FD_HANDLER_ENABLED);
}

/* Checks to see if some other port has the same device in use.  Must
   be called with ports_lock held. */
static int
is_device_already_inuse(port_info_t *check_port)
{
    port_info_t *port = ports;

    while (port != NULL) {
	if (port != check_port) {
	    if ((strcmp(port->io.devname, check_port->io.devname) == 0) 
		&& (port->tcp_to_dev_state != PORT_UNCONNECTED))
	    {
		return 1;
	    }
	}    
	port = port->next;
    }

    return 0;
}

static int
from_hex_digit(char c)
{
    if ((c >= '0') && (c <= '9'))
	return c - '0';
    if ((c >= 'A') && (c <= 'F'))
	return c - 'A' + 10;
    if ((c >= 'a') && (c <= 'f'))
	return c - 'a' + 10;
    return 0;
}

static char *smonths[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
			   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static char *sdays[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

static void
process_str(port_info_t *port, struct tm *time, struct timeval *tv,
	    const char *s,
	    void (*op)(void *data, char val), void *data, int isfilename)
{
    char val;
    char *t;

    while (*s) {
	if (*s == '\\') {
	    s++;
	    if (!*s)
		return;
	    switch (*s) {
	    /* Standard "C" characters. */
	    case 'a': op(data, 7); break;
	    case 'b': op(data, 8); break;
	    case 'f': op(data, 12); break;
	    case 'n': op(data, 10); break;
	    case 'r': op(data, 13); break;
	    case 't': op(data, 9); break;
	    case 'v': op(data, 11); break;
	    case '\\': op(data, '\\'); break;
	    case '?': op(data, '?'); break;
	    case '\'': op(data, '\''); break;
	    case '"': op(data, '"'); break;

	    case 'd':
		/* ser2net device name. */
		if (isfilename) {
		    /* Can't have '/' in a filename. */
		    t = strrchr(port->io.devname, '/');
		    if (t)
			t++;
		    else
			t = port->io.devname;
		} else
		    t = port->io.devname;
		for (; *t; t++)
		    op(data, *t);
		break;

	    case 'p':
		/* ser2net TCP port. */
		for (t=port->portname; *t; t++)
		    op(data, *t);
		break;

	    case 's':
		if (isfilename)
		    goto seconds;
		goto serparms;

	    case 'B':
	    serparms:
		/* ser2net serial parms. */
		{
		    char str[15];
		    port->io.f->serparm_to_str(&port->io, str, sizeof(str));
		    for (t=str; *t; t++)
			op(data, *t);
		}
		break;

	    case '0': case '1': case '2': case '3': case '4': case '5':
	    case '6': case '7':
		/* Octal digit */
		val = (*s) - '0';
		s++;
		if (!*s) {
		    op(data, val);
		    return;
		}
		if (!isdigit(*s)) {
		    continue;
		}
		val = (val * 8) + (*s) - '0';
		s++;
		if (!*s) {
		    op(data, val);
		    return;
		}
		if (!isdigit(*s)) {
		    continue;
		}
		val = (val * 8) + (*s) - '0';
		break;

	    case 'x':
		/* Hex digit */
		s++;
		if (!*s)
		    return;
		if (!isxdigit(*s))
		    continue;
		val = from_hex_digit(*s);
		s++;
		if (!*s) {
		    op(data, val);
		    return;
		}
		if (!isdigit(*s))
		    continue;
		val = (val * 16) + from_hex_digit(*s);
		break;

	    /* \Y -> year */
	    case 'Y':
	    {
		char d[10], *dp;
		snprintf(d, sizeof(d), "%d", time->tm_year + 1900);
		for (dp = d; *dp; dp++)
		    op(data, *dp);
		break;
	    }

	    /* \y -> day of the year (days since Jan 1) */
	    case 'y':
	    {
		char d[10], *dp;
		snprintf(d, sizeof(d), "%d", time->tm_yday);
		for (dp = d; *dp; dp++)
		    op(data, *dp);
		break;
	    }

	    /* \M -> month (Jan, Feb, Mar, etc.) */
	    case 'M':
		if (time->tm_mon >= 12)
		    op(data, '?');
		else {
		    char *dp = smonths[time->tm_mon];
		    for (; *dp; dp++)
			op(data, *dp);
		}
		break;

	    /* \m -> month (as a number) */
	    case 'm':
	    {
		char d[10], *dp;
		snprintf(d, sizeof(d), "%d", time->tm_mon);
		for (dp = d; *dp; dp++)
		    op(data, *dp);
		break;
	    }

	    /* \A -> day of the week (Mon, Tue, etc.) */
	    case 'A':
		if (time->tm_wday >= 7)
		    op(data, '?');
		else {
		    char *dp = sdays[time->tm_wday];
		    for (; *dp; dp++)
			op(data, *dp);
		}
		break;

	    /* \D -> day of the month */
	    case 'D':
	    {
		char d[10], *dp;
		snprintf(d, sizeof(d), "%d", time->tm_mday);
		for (dp = d; *dp; dp++)
		    op(data, *dp);
		break;
	    }

	    /* \H -> hour (24-hour time) */
	    case 'H':
	    {
		char d[10], *dp;
		snprintf(d, sizeof(d), "%2.2d", time->tm_hour);
		for (dp = d; *dp; dp++)
		    op(data, *dp);
		break;
	    }

	    /* \h -> hour (12-hour time) */
	    case 'h':
	    {
		char d[10], *dp;
		int v;

		v = time->tm_hour;
		if (v == 0)
		    v = 12;
		else if (v > 12)
		    v -= 12;
		snprintf(d, sizeof(d), "%2.2d", v);
		for (dp = d; *dp; dp++)
		    op(data, *dp);
		break;
	    }

	    /* \i -> minute */
	    case 'i':
	    {
		char d[10], *dp;
		snprintf(d, sizeof(d), "%2.2d", time->tm_min);
		for (dp = d; *dp; dp++)
		    op(data, *dp);
		break;
	    }

	    /* \S -> second */
	    case 'S':
	    seconds:
	    {
		char d[10], *dp;
		snprintf(d, sizeof(d), "%2.2d", time->tm_sec);
		for (dp = d; *dp; dp++)
		    op(data, *dp);
		break;
	    }

	    /* \q -> am/pm */
	    case 'q':
		if (time->tm_hour < 12) {
		    op(data, 'a');
		} else {
		    op(data, 'p');
		}
		op(data, 'm');
		break;

	    /* \P -> AM/PM */
	    case 'P':
		if (time->tm_hour < 12) {
		    op(data, 'A');
		} else {
		    op(data, 'P');
		}
		op(data, 'M');
		break;

	    /* \T -> time (HH:MM:SS) */
	    case 'T':
	    {
		char d[10], *dp;
		snprintf(d, sizeof(d), "%2.2d:%2.2d:%2.2d",
			 time->tm_hour, time->tm_min, time->tm_sec);
		for (dp = d; *dp; dp++)
		    op(data, *dp);
		break;
	    }

	    /* \e -> epoc (seconds since Jan 1, 1970) */
	    case 'e':
	    {
		char d[30], *dp;
		snprintf(d, sizeof(d), "%ld", tv->tv_sec);
		for (dp = d; *dp; dp++)
		    op(data, *dp);
		break;
	    }

	    /* \U -> microseconds in the current second */
	    case 'U':
	    {
		char d[10], *dp;
		snprintf(d, sizeof(d), "%6.6ld", tv->tv_usec);
		for (dp = d; *dp; dp++)
		    op(data, *dp);
		break;
	    }

	    /* \I -> remote IP address (in dot format) */
	    case 'I':
	    {
		char ip[100], *ipp;
		if (!getnameinfo((struct sockaddr *) &(port->remote),
				 sizeof(port->remote),
				 ip, sizeof(ip), NULL, 0, NI_NUMERICHOST))
		    break;
		for (ipp = ip; *ipp; ipp++)
		    op(data, *ipp);
		break;
	    }

	    default:
		op(data, *s);
	    }
	} else
	    op(data, *s);
	s++;
    }
}

static void
count_op(void *data, char c)
{
    unsigned int *idata = data;

    (*idata)++;
}

struct bufop_data {
    unsigned int pos;
    char *str;
};

static void
buffer_op(void *data, char c)
{
    struct bufop_data *bufop = data;
    bufop->str[bufop->pos] = c;
    (bufop->pos)++;
}

static char *
process_str_to_str(port_info_t *port, const char *str, struct timeval *tv,
		   unsigned int *lenrv, int isfilename)
{
    unsigned int len = 0;
    struct tm now;
    struct bufop_data bufop;

    localtime_r(&tv->tv_sec, &now);
    process_str(port, &now, tv, str, count_op, &len, isfilename);
    if (!lenrv)
	/* If we don't return a length, append a nil char. */
	len++;
    bufop.pos = 0;
    if (len == 0)
	/* malloc(0) sometimes return NULL */
	bufop.str = malloc(1);
    else
	bufop.str = malloc(len);
    if (!bufop.str) {
	syslog(LOG_ERR, "Out of memory processing string: %s", port->portname);
	return NULL;
    }
    process_str(port, &now, tv, str, buffer_op, &bufop, isfilename);

    if (lenrv)
	*lenrv = len;
    else
	bufop.str[bufop.pos] = '\0';

    return bufop.str;
}

static struct sbuf *
process_str_to_buf(port_info_t *port, const char *str)
{
    const char *bstr;
    struct sbuf *buf;
    unsigned int len;
    struct timeval tv;

    if (!str)
	return NULL;
    gettimeofday(&tv, NULL);

    buf = malloc(sizeof(*buf));
    if (!buf) {
	syslog(LOG_ERR, "Out of memory processing string: %s", port->portname);
	return NULL;
    }
    bstr = process_str_to_str(port, str, &tv, &len, 0);
    if (!bstr) {
	free(buf);
	syslog(LOG_ERR, "Error processing string: %s", port->portname);
	return NULL;
    }
    buffer_init(buf, (unsigned char *) bstr, len);
    buf->cursize = len;
    return buf;
}

static void
open_trace_file(port_info_t *port, 
                trace_info_t *t,
                struct timeval *tv,
                trace_info_t **out)
{
    int rv;
    char *trfile;

    trfile = process_str_to_str(port, t->filename, tv, NULL, 1);
    if (!trfile) {
	syslog(LOG_ERR, "Unable to translate trace file %s", t->filename);
	t->fd = -1;
	return;
    }

    rv = open(trfile, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (rv == -1) {
	char errbuf[128];
	int err = errno;

	if (strerror_r(err, errbuf, sizeof(errbuf)) == -1)
	    syslog(LOG_ERR, "Unable to open trace file %s: %d",
		   trfile, err);
	else
	    syslog(LOG_ERR, "Unable to open trace file %s: %s",
		   trfile, errbuf);
    }

    free(trfile);
    t->fd = rv;
    *out = t;
}

static void
setup_trace(port_info_t *port)
{
    struct timeval tv;

    /* Only get the time once so all trace files have consistent times. */
    gettimeofday(&tv, NULL);

    port->tw = NULL;
    if (port->trace_write.filename)
	open_trace_file(port, &port->trace_write, &tv, &port->tw);

    port->tr = NULL;
    if (port->trace_read.filename) {
	trace_info_t *np = &port->trace_read;
	if (port->tw && (strcmp(np->filename, port->tw->filename) == 0))
	    port->tr = port->tw;
	else
	    open_trace_file(port, np, &tv, &port->tr);
    }

    port->tb = NULL;
    if (port->trace_both.filename) {
	trace_info_t *np = &port->trace_both;
	if (port->tw && (strcmp(np->filename, port->tw->filename) == 0))
	    port->tb = port->tw;
	else if (port->tr && (strcmp(np->filename, port->tr->filename) == 0))
	    port->tb = port->tr;
	else
	    open_trace_file(port, np, &tv, &port->tb);
    }

    return;
}

static void
recalc_port_chardelay(port_info_t *port)
{
    /* delay is (((1 / bps) * bpc) * scale) seconds */
    if (!port->enable_chardelay) {
	port->chardelay = 0;
	return;
    }

    /* We are working in microseconds here. */
    port->chardelay = (port->bpc * 100000 * port->chardelay_scale) / port->bps;
    if (port->chardelay < port->chardelay_min)
	port->chardelay = port->chardelay_min;
}

/* Called to set up a new connection's file descriptor. */
static int
setup_tcp_port(port_info_t *port)
{
    int options;
    struct timeval then;
    const char *errstr;

    if (fcntl(port->tcpfd, F_SETFL, O_NONBLOCK) == -1) {
	close(port->tcpfd);
	port->tcpfd = -1;
	syslog(LOG_ERR, "Could not fcntl the tcp port %s: %m", port->portname);
	return -1;
    }
    options = 1;
    if (setsockopt(port->tcpfd, IPPROTO_TCP, TCP_NODELAY,
		   (char *) &options, sizeof(options)) == -1) {
	close(port->tcpfd);
	port->tcpfd = -1;
	syslog(LOG_ERR, "Could not enable TCP_NODELAY tcp port %s: %m",
	       port->portname);
	return -1;
    }

#ifdef HAVE_TCPD_H
    {
	struct request_info req;
	
	request_init(&req, RQ_DAEMON, progname, RQ_FILE, port->tcpfd, NULL);
	fromhost(&req);

	if (!hosts_access(&req)) {
	    char *err = "Access denied\r\n";
	    write(port->tcpfd, err, strlen(err));
	    close(port->tcpfd);
	    port->tcpfd = -1;
	    return -1;
	}
    }
#endif /* HAVE_TCPD_H */

    errstr = NULL;
    if (port->io.f->setup(&port->io, port->portname, &errstr,
			  &port->bps, &port->bpc) == -1) {
	if (errstr)
	    write_ignore_fail(port->tcpfd, errstr, strlen(errstr));
	close(port->tcpfd);
	port->tcpfd = -1;
	return -1;
    }
    recalc_port_chardelay(port);
    port->is_2217 = 0;

    port->banner = process_str_to_buf(port, port->bannerstr);
    if (port->banner)
	port->tcp_write_handler = handle_tcp_fd_banner_write;
    else
	port->tcp_write_handler = handle_tcp_fd_write;

    port->devstr = process_str_to_buf(port, port->openstr);
    if (port->devstr)
	port->dev_write_handler = handle_dev_fd_devstr_write;
    else
	port->dev_write_handler = handle_dev_fd_normal_write;

    port->io.read_handler = (port->enabled == PORT_RAWLP
			     ? NULL
			     : handle_dev_fd_read);
    port->io.write_handler = handle_dev_fd_write;
    port->io.except_handler = handle_dev_fd_except;
    port->io.f->read_handler_enable(&port->io, port->enabled != PORT_RAWLP);
    port->io.f->except_handler_enable(&port->io, 1);
    if (port->devstr)
	port->io.f->write_handler_enable(&port->io, 1);
    port->dev_to_tcp_state = PORT_WAITING_INPUT;

    sel_set_fd_handlers(ser2net_sel,
			port->tcpfd,
			port,
			handle_tcp_fd_read,
			handle_tcp_fd_write_mux,
			handle_tcp_fd_except,
			port_tcp_fd_cleared);
    sel_set_fd_read_handler(ser2net_sel, port->tcpfd,
			    SEL_FD_HANDLER_ENABLED);
    sel_set_fd_except_handler(ser2net_sel, port->tcpfd,
			      SEL_FD_HANDLER_ENABLED);
    port->tcp_to_dev_state = PORT_WAITING_INPUT;

    if (port->enabled == PORT_TELNET) {
	telnet_init(&port->tn_data, port, telnet_output_ready,
		    telnet_cmd_handler,
		    telnet_cmds,
		    telnet_init_seq, sizeof(telnet_init_seq));
	/* May not have been set if we have banner output. */
	sel_set_fd_write_handler(ser2net_sel, port->tcpfd,
				 SEL_FD_HANDLER_ENABLED);
    } else {
	buffer_init(&port->tn_data.out_telnet_cmd, NULL, 0);
	port->io.f->read_handler_enable(&port->io, 1);
	if (port->banner)
	    sel_set_fd_write_handler(ser2net_sel, port->tcpfd,
				     SEL_FD_HANDLER_ENABLED);
    }

    setup_trace(port);
    header_trace(port);

    sel_get_monotonic_time(&then);
    then.tv_sec += 1;
    sel_start_timer(port->timer, &then);

    reset_timer(port);
    return 0;
}

static port_info_t *
is_port_free(char *portname)
{
    port_info_t *port = ports;

    while (port) {
	if (strcmp(port->portname, portname) == 0)
	    if (port->tcp_to_dev_state == PORT_UNCONNECTED &&
			!is_device_already_inuse(port))
		return port;
	port = port->next;
    }

    return NULL;
}

static void
handle_port_accept(port_info_t *port, int fd)
{
    socklen_t len;
    int optval;

    len = sizeof(port->remote);

    port->tcpfd = accept(fd, (struct sockaddr *) &(port->remote), &len);
    if (port->tcpfd == -1) {
	if (errno != EAGAIN && errno != EWOULDBLOCK)
	    syslog(LOG_ERR, "Could not accept on port %s: %m", port->portname);
	return;
    }

    optval = 1;
    if (setsockopt(port->tcpfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&optval,
		   sizeof(optval)) == -1) {
	close(port->tcpfd);
	syslog(LOG_ERR, "Could not enable SO_KEEPALIVE on tcp port %s: %m",
	       port->portname);
	return;
    }

    /* XXX log port->remote */
    setup_tcp_port(port);
}

typedef struct rotator
{
    /* Rotators use the ports_lock for mutex. */
    int curr_port;
    char **portv;
    int portc;

    char *portname;

    struct addrinfo    *ai;		/* The address list for the portname. */
    int            *acceptfds;		/* The file descriptor used to
					   accept connections on the
					   TCP port. */
    unsigned int   nr_acceptfds;
    waiter_t       *accept_waiter;

    struct rotator *next;
} rotator_t;

rotator_t *rotators = NULL;

/* A connection request has come in on a port. */
static void
handle_rot_port_read(int fd, void *data)
{
    rotator_t *rot = (rotator_t *) data;
    port_info_t *port;
    struct sockaddr_storage dummy_sockaddr;
    socklen_t len = sizeof(dummy_sockaddr);
    int i, new_fd;
    char *err;

    LOCK(ports_lock);
    i = rot->curr_port;
    do {
	port = is_port_free(rot->portv[i]);
	if (++i >= rot->portc)
	    i = 0;
	if (port) {
	    rot->curr_port = i;
	    LOCK(port->lock);
	    handle_port_accept(port, fd);
	    UNLOCK(port->lock);
	    UNLOCK(ports_lock);
	    return;
	}
    } while (i != rot->curr_port);
    UNLOCK(ports_lock);

    new_fd = accept(fd, (struct sockaddr *) &dummy_sockaddr, &len);
    if (new_fd != -1) {
	err = "No free port found\r\n";
	write_ignore_fail(new_fd, err, strlen(err));
	close(new_fd);
    }
}

static void
free_rotator(rotator_t *rot)
{
    int i;

    for (i = 0; i < rot->nr_acceptfds; i++) {
	sel_set_fd_read_handler(ser2net_sel,
				rot->acceptfds[i],
				SEL_FD_HANDLER_DISABLED);
	sel_clear_fd_handlers(ser2net_sel, rot->acceptfds[i]);
	wait_for_waiter(rot->accept_waiter);
	close(rot->acceptfds[i]);
    }
    if (rot->accept_waiter)
	free_waiter(rot->accept_waiter);
    if (rot->portname)
	free(rot->portname);
    if (rot->ai)
	freeaddrinfo(rot->ai);
    if (rot->acceptfds)
	free(rot->acceptfds);
    if (rot->portv)
	str_to_argv_free(rot->portc, rot->portv);
    free(rot);
}

void
free_rotators(void)
{
    rotator_t *rot, *next;

    rot = rotators;
    while (rot) {
	next = rot->next;
	free_rotator(rot);
	rot = next;
    }
    rotators = NULL;
}

static void
rotator_fd_cleared(int fd, void *cb_data)
{
    rotator_t *rot = cb_data;

    wake_waiter(rot->accept_waiter);
}

int
add_rotator(char *portname, char *ports, int lineno)
{
    rotator_t *rot;
    int rv;

    rot = malloc(sizeof(*rot));
    if (!rot)
	return ENOMEM;
    memset(rot, 0, sizeof(*rot));

    rot->accept_waiter = alloc_waiter();
    if (!rot->accept_waiter) {
	free_rotator(rot);
	return ENOMEM;
    }

    rot->portname = strdup(portname);
    if (!rot->portname) {
	free_waiter(rot->accept_waiter);
	free_rotator(rot);
	return ENOMEM;
    }

    rv = str_to_argv(ports, &rot->portc, &rot->portv, NULL);
    if (rv)
	goto out;

    rv = scan_tcp_port(rot->portname, &rot->ai);
    if (rv) {
	syslog(LOG_ERR, "port number was invalid on line %d", lineno);
	goto out;
    }

    rot->acceptfds = open_socket(rot->ai, handle_rot_port_read, rot,
				 &rot->nr_acceptfds, rotator_fd_cleared);
    if (rot->acceptfds == NULL) {
	syslog(LOG_ERR, "Unable to create TCP socket on line %d", lineno);
	rv = ENOMEM;
	goto out;
    }

    rot->next = rotators;
    rotators = rot;

 out:
    if (rv)
	free_rotator(rot);
    return rv;
}

/* A connection request has come in on a port. */
static void
handle_accept_port_read(int fd, void *data)
{
    port_info_t *port = (port_info_t *) data;
    char *err = NULL;

    LOCK(port->lock);

    if (port->enabled == PORT_DISABLED)
	goto out;

    if (port->tcp_to_dev_state != PORT_UNCONNECTED) {
	if (port->kickolduser_mode) {
	    shutdown_port(port, "kicked off, new user is coming\r\n");
	    /* Wait the port to be unconnected and clean, go back to main loop*/
	    return;
	}
	err = "Port already in use\r\n";
    }

    if (!err && is_device_already_inuse(port)) {
	err = "Port's device already in use\r\n";
    }

    if (err != NULL) {
	struct sockaddr_storage dummy_sockaddr;
	socklen_t len = sizeof(dummy_sockaddr);
	int new_fd = accept(fd, (struct sockaddr *) &dummy_sockaddr, &len);

	UNLOCK(port->lock);
	if (new_fd != -1) {
	    write_ignore_fail(new_fd, err, strlen(err));
	    close(new_fd);
	}
	return;
    }

    /* We have to hold the ports_lock until after this call so the
       device won't get used (from is_device_already_inuse()). */
    handle_port_accept(port, fd);
 out:
    UNLOCK(port->lock);
}

static void
port_accept_fd_cleared(int fd, void *cb_data)
{
    port_info_t *port = cb_data;

    wake_waiter(port->accept_waiter);
}

/* Start monitoring for connections on a specific port. */
static int
startup_port(struct absout *eout, port_info_t *port)
{
    if (port->is_stdio) {
	if (is_device_already_inuse(port)) {
	    if (eout)
		eout->out(eout, "Port's device already in use");
	    return -1;
	} else {
	    port->acceptfds = NULL;
	    port->tcpfd = 0; /* stdin */
	    if (setup_tcp_port(port) == -1)
		return -1;
	}
	return 0;
    }

    port->acceptfds = open_socket(port->ai, handle_accept_port_read, port,
				  &port->nr_acceptfds, port_accept_fd_cleared);
    if (port->acceptfds == NULL) {
	if (eout)
	    eout->out(eout, "Unable to create TCP socket");
	else
	    syslog(LOG_ERR, "Unable to create TCP socket for port %s: %s",
		   port->portname, strerror(errno));

	return -1;
    }

    return 0;
}

static void
redo_port_handlers(port_info_t *port)
{
    unsigned int i;

    for (i = 0; i < port->nr_acceptfds; i++) {
	sel_set_fd_handlers(ser2net_sel, port->acceptfds[i], port,
			    handle_accept_port_read, NULL, NULL,
			    port_accept_fd_cleared);
	wait_for_waiter(port->accept_waiter);
    }
}

int
change_port_state(struct absout *eout, port_info_t *port, int state)
{
    int rv = 0;

    if (port->enabled == state) {
	UNLOCK(port->lock);
	return 0;
    }

    if (state == PORT_DISABLED) {
	port->enabled = PORT_DISABLED; /* Stop accepts */
	UNLOCK(port->lock);

	if (port->acceptfds != NULL) {
	    unsigned int i;

	    for (i = 0; i < port->nr_acceptfds; i++) {
		sel_set_fd_read_handler(ser2net_sel,
					port->acceptfds[i],
					SEL_FD_HANDLER_DISABLED);
		sel_clear_fd_handlers(ser2net_sel, port->acceptfds[i]);
		wait_for_waiter(port->accept_waiter);
		close(port->acceptfds[i]);
	    }
	    free(port->acceptfds);
	    port->acceptfds = NULL;
	    port->nr_acceptfds = 0;
	}
    } else if (port->enabled == PORT_DISABLED) {
	if (state == PORT_RAWLP)
	    port->io.read_disabled = 1;
	else
	    port->io.read_disabled = 0;
	rv = startup_port(eout, port);
	port->enabled = state;
	UNLOCK(port->lock);
    }

    return rv;
}

static void
free_port(port_info_t *port)
{
    FREE_LOCK(port->lock);
    if (port->dev_to_tcp.buf)
	free(port->dev_to_tcp.buf);
    if (port->tcp_to_dev.buf)
	free(port->tcp_to_dev.buf);
    if (port->timer)
	sel_free_timer(port->timer);
    if (port->send_timer)
	sel_free_timer(port->send_timer);
    if (port->runshutdown)
	sel_free_runner(port->runshutdown);
    if (port->accept_waiter)
	free_waiter(port->accept_waiter);
    if (port->waiter)
	free_waiter(port->waiter);
    if (port->io.f)
	port->io.f->free(&port->io);
    if (port->trace_read.filename)
	free(port->trace_read.filename);
    if (port->trace_write.filename)
	free(port->trace_write.filename);
    if (port->trace_both.filename)
	free(port->trace_both.filename);
    if (port->io.devname)
	free(port->io.devname);
    if (port->portname)
	free(port->portname);
    if (port->new_config)
	free_port(port->new_config);
    if (port->ai)
	freeaddrinfo(port->ai);
    if (port->acceptfds)
	free(port->acceptfds);
    if (port->bannerstr)
	free(port->bannerstr);
    if (port->signaturestr)
	free(port->signaturestr);
    if (port->openstr)
	free(port->openstr);
    if (port->closestr)
	free(port->closestr);
    if (port->closeon)
	free(port->closeon);
    free(port);
}

static void
switchout_port(struct absout *eout, port_info_t *new_port,
	       port_info_t *curr, port_info_t *prev)
{
    int new_state = new_port->enabled;
    waiter_t *tmp_waiter;

    /* Keep the same accept ports as the old port. */
    new_port->enabled = curr->enabled;
    new_port->acceptfds = curr->acceptfds;
    new_port->nr_acceptfds = curr->nr_acceptfds;

    /*
     * The strange switcharoo with the waiter keep the waiter the same
     * on the old and new data, otherwise the wakeup and the wait
     * would be on different waiters.
     */
    tmp_waiter = new_port->accept_waiter;
    new_port->accept_waiter = curr->accept_waiter;
    curr->acceptfds = NULL;
    redo_port_handlers(new_port);
    curr->accept_waiter = tmp_waiter;

    if (prev == NULL) {
	ports = new_port;
    } else {
	prev->next = new_port;
    }
    new_port->next = curr->next;
    free_port(curr);
    change_port_state(eout, new_port, new_state); /* releases lock */
}

static void
io_shutdown_done(struct devio *io)
{
    port_info_t *port = io->user_data;

    wake_waiter(port->waiter);
}

static void
timer_shutdown_done(selector_t *sel, sel_timer_t *timer, void *cb_data)
{
    port_info_t *port = cb_data;

    wake_waiter(port->waiter);
}

static void
finish_shutdown_port(sel_runner_t *runner, void *cb_data)
{
    port_info_t *port = cb_data;

    if (port->io.f) {
	port->io.f->shutdown(&port->io, io_shutdown_done);
	wait_for_waiter(port->waiter);
    }

    sel_stop_timer_with_done(port->send_timer, timer_shutdown_done, port);
    wait_for_waiter(port->waiter);

    /* At this point nothing can happen on the port, so no need for a lock */

    port->tcp_to_dev_state = PORT_UNCONNECTED;
    buffer_reset(&port->tcp_to_dev);
    port->tcp_bytes_received = 0;
    port->tcp_bytes_sent = 0;
    if (port->banner) {
	free(port->banner->buf);
	free(port->banner);
	port->banner = NULL;
    }
    if (port->devstr) {
	free(port->devstr->buf);
	free(port->devstr);
	port->devstr = NULL;
    }
    buffer_reset(&port->dev_to_tcp);
    port->dev_bytes_received = 0;
    port->dev_bytes_sent = 0;

    if (port->is_stdio)
	/* This was a zero port (for stdin/stdout), this is only
	   allowed with one port at a time, and we shut down when it
	   closes. */
	exit(0);

    /* If the port has been disabled, then delete it.  Check this before
       the new config so the port will be deleted properly and not
       reconfigured on a reconfig. */
    if (port->config_num == -1) {
	port_info_t *curr, *prev;

	prev = NULL;
	LOCK(ports_lock);
	curr = ports;
	while ((curr != NULL) && (curr != port)) {
	    prev = curr;
	    curr = curr->next;
	}
	if (curr != NULL) {
	    if (prev == NULL)
		ports = curr->next;
	    else
		prev->next = curr->next;
	}
	UNLOCK(ports_lock);
	free_port(port);
	return; /* We have to return here because we no longer have a port. */
    }

    /*
     * The configuration for this port has changed, install it now that
     * the user has closed the connection.
     */
    if (port->new_config != NULL) {
	port_info_t *curr, *prev;

	prev = NULL;
	LOCK(ports_lock);
	curr = ports;
	while ((curr != NULL) && (curr != port)) {
	    prev = curr;
	    curr = curr->next;
	}
	if (curr != NULL) {
	    port = curr->new_config;
	    curr->new_config = NULL;
	    LOCK(port->lock);
	    switchout_port(NULL, port, curr, prev); /* Releases lock */
	}
	UNLOCK(ports_lock);
    }

    /* Wait until here to let anything start on the port. */
    LOCK(port->lock);
    port->dev_to_tcp_state = PORT_UNCONNECTED;
    UNLOCK(port->lock);
}

/*
 * Run at base context so we don't have to worry about, say, waiting
 * for a timer to stop when we are running in that timer's context.
 */
static void shutdown_port2(sel_runner_t *runner, void *cb_data)
{
    port_info_t *port = cb_data;

    sel_stop_timer_with_done(port->timer, timer_shutdown_done, port);
    wait_for_waiter(port->waiter);

    LOCK(port->lock);
    if (port->devstr) {
	free(port->devstr->buf);
	free(port->devstr);
    }
    port->devstr = process_str_to_buf(port, port->closestr);
    if (port->devstr && (port->tcp_to_dev_state != PORT_UNCONNECTED)) {
	port->io.f->read_handler_enable(&port->io, 0);
	port->io.f->except_handler_enable(&port->io, 0);
	port->dev_write_handler = handle_dev_fd_close_write;
	port->io.f->write_handler_enable(&port->io, 1);
	UNLOCK(port->lock);
    } else {
	UNLOCK(port->lock);
	finish_shutdown_port(NULL, port);
    }
}

static void
port_tcp_fd_cleared(int fd, void *cb_data)
{
    port_info_t *port = (port_info_t *) cb_data;

    close(port->tcpfd);
    port->tcpfd = -1;
    sel_run(port->runshutdown, shutdown_port2, port);
}

static void
shutdown_port(port_info_t *port, char *reason)
{
    if (port->dev_to_tcp_state == PORT_CLOSING) {
	UNLOCK(port->lock);
	return;
    }

    footer_trace(port, reason);
    
    if (port->trace_write.fd != -1) {
	close(port->trace_write.fd);
	port->trace_write.fd = -1;
    }
    if (port->trace_read.fd != -1) {
	close(port->trace_read.fd);
	port->trace_read.fd = -1;
    }
    if (port->trace_both.fd != -1) {
	close(port->trace_both.fd);
	port->trace_both.fd = -1;
    }
    port->tw = port->tr = port->tb = NULL;

    port->dev_to_tcp_state = PORT_CLOSING;
    UNLOCK(port->lock);
    if (port->tcpfd != -1)
	sel_clear_fd_handlers(ser2net_sel, port->tcpfd);
    else
	sel_run(port->runshutdown, shutdown_port2, port);
}

void
got_timeout(selector_t  *sel,
	    sel_timer_t *timer,
	    void        *data)
{
    port_info_t *port = (port_info_t *) data;
    struct timeval then;
    unsigned char modemstate;

    LOCK(port->lock);

    if (port->dev_to_tcp_state == PORT_CLOSING) {
	UNLOCK(port->lock);
	return;
    }

    if (port->timeout) {
	port->timeout_left--;
	if (port->timeout_left < 0) {
	    shutdown_port(port, "timeout"); /* Releases lock */
	    return;
	}
    }

    if (port->is_2217 &&
		(port->io.f->get_modem_state(&port->io, &modemstate) != -1)) {
	modemstate &= port->modemstate_mask;
	if (modemstate != port->last_modemstate) {
	    unsigned char data[3];
	    data[0] = TN_OPT_COM_PORT;
	    data[1] = 107; /* Notify modemstate */
	    data[2] = modemstate;
	    port->last_modemstate = modemstate;
	    telnet_send_option(&port->tn_data, data, 3);
	}
    }
    
    sel_get_monotonic_time(&then);
    then.tv_sec += 1;
    sel_start_timer(port->timer, &then);
    UNLOCK(port->lock);
}

static int
isallzero(char *str)
{
    if (*str == '\0')
	return 0;

    while (*str == '0')
	str++;
    return *str == '\0';
}

static int cmpstrval(const char *s, const char *prefix, unsigned int *end)
{
    int len = strlen(prefix);

    if (strncmp(s, prefix, len))
	return 0;
    *end = len;
    return 1;
}

static int cmpstrint(const char *s, const char *prefix, int *val,
		     struct absout *eout)
{
    unsigned int end;
    char *endpos;

    if (!cmpstrval(s, prefix, &end))
	return 0;

    *val = strtoul(s + end, &endpos, 10);
    if (endpos == s + end || *endpos != '\0') {
	eout->out(eout, "Invalid number for %s: %s\n", prefix, s + end);
	return -1;
    }
    return 1;
}

static int
myconfig(void *data, struct absout *eout, const char *pos)
{
    port_info_t *port = data;
    enum str_type stype;
    char *s;
    unsigned int len, end;
    int rv, val;

    if (strcmp(pos, "remctl") == 0) {
	port->allow_2217 = 1;
    } else if (strcmp(pos, "-remctl") == 0) {
	port->allow_2217 = 0;
    } else if (strcmp(pos, "kickolduser") == 0) {
        port->kickolduser_mode = 1;
    } else if (strcmp(pos, "-kickolduser") == 0) {
        port->kickolduser_mode = 0;
    } else if (strcmp(pos, "hexdump") == 0 ||
	       strcmp(pos, "-hexdump") == 0) {
	port->trace_read.hexdump = (*pos != '-');
	port->trace_write.hexdump = (*pos != '-');
	port->trace_both.hexdump = (*pos != '-');
    } else if (strcmp(pos, "timestamp") == 0 ||
	       strcmp(pos, "-timestamp") == 0) {
	port->trace_read.timestamp = (*pos != '-');
	port->trace_write.timestamp = (*pos != '-');
	port->trace_both.timestamp = (*pos != '-');
    } else if (strcmp(pos, "tr-hexdump") == 0 ||
	       strcmp(pos, "-tr-hexdump") == 0) {
	port->trace_read.hexdump = (*pos != '-');
    } else if (strcmp(pos, "tr-timestamp") == 0 ||
	       strcmp(pos, "-tr-timestamp") == 0) {
	port->trace_read.timestamp = (*pos != '-');
    } else if (strcmp(pos, "tw-hexdump") == 0 ||
	       strcmp(pos, "-tw-hexdump") == 0) {
	port->trace_write.hexdump = (*pos != '-');
    } else if (strcmp(pos, "tw-timestamp") == 0 ||
	       strcmp(pos, "-tw-timestamp") == 0) {
	port->trace_write.timestamp = (*pos != '-');
    } else if (strcmp(pos, "tb-hexdump") == 0 ||
	       strcmp(pos, "-tb-hexdump") == 0) {
	port->trace_both.hexdump = (*pos != '-');
    } else if (strcmp(pos, "tb-timestamp") == 0 ||
	       strcmp(pos, "-tb-timestamp") == 0) {
	port->trace_both.timestamp = (*pos != '-');
    } else if (cmpstrval(pos, "tr=", &end)) {
	/* trace read, data from the port to the socket */
	port->trace_read.filename = find_tracefile(pos + end);
    } else if (cmpstrval(pos, "tw=", &end)) {
	/* trace write, data from the socket to the port */
	port->trace_write.filename = find_tracefile(pos + end);
    } else if (cmpstrval(pos, "tb=", &end)) {
	/* trace both directions. */
	port->trace_both.filename = find_tracefile(pos + end);
    } else if (cmpstrval(pos, "led-rx=", &end)) {
	/* LED for UART RX traffic */
	port->led_rx = find_led(pos + end);
    } else if (cmpstrval(pos, "led-tx=", &end)) {
	/* LED for UART TX traffic */
	port->led_tx = find_led(pos + end);
#if HAVE_DECL_TIOCSRS485
    } else if (cmpstrval(pos, "rs485=", &end)) {
	/* get RS485 configuration. */
	port->rs485conf = find_rs485conf(pos + end);
#endif
    } else if (strcmp(pos, "telnet_brk_on_sync") == 0) {
	port->telnet_brk_on_sync = 1;
    } else if (strcmp(pos, "-telnet_brk_on_sync") == 0) {
	port->telnet_brk_on_sync = 0;
    } else if (strcmp(pos, "chardelay") == 0) {
	port->enable_chardelay = true;
    } else if (strcmp(pos, "-chardelay") == 0) {
	port->enable_chardelay = false;
    } else if ((rv = cmpstrint(pos, "chardelay-scale=", &val, eout))) {
	if (rv == -1)
	    return -1;
	port->chardelay_scale = val;
    } else if ((rv = cmpstrint(pos, "chardelay-min=", &val, eout))) {
	if (rv == -1)
	    return -1;
	port->chardelay_min = val;
    } else if ((rv = cmpstrint(pos, "chardelay-max=", &val, eout))) {
	if (rv == -1)
	    return -1;
	port->chardelay_max = val;
    } else if ((rv = cmpstrint(pos, "dev-to-tcp-bufsize=", &val, eout))) {
	if (rv == -1)
	    return -1;
	port->dev_to_tcp_bufsize = val;
    } else if ((rv = cmpstrint(pos, "tcp-to-dev-bufsize=", &val, eout))) {
	if (rv == -1)
	    return -1;
	port->tcp_to_dev_bufsize = val;
    } else if ((s = find_str(pos, &stype, &len))) {
	/* It's a startup banner, signature or open/close string, it's
	   already set. */
	switch (stype) {
	case BANNER: port->bannerstr = s; break;
	case SIGNATURE: port->signaturestr = s; break;
	case OPENSTR: port->openstr = s; break;
	case CLOSESTR: port->closestr = s; break;
	case CLOSEON: port->closeon = s; port->closeon_len = len; break;
	default: free(s); goto unknown;
	}
    } else {
    unknown:
	eout->out(eout, "Unknown config item: %s", pos);
	return -1;
    }

    return 0;
}

/* Create a port based on a set of parameters passed in. */
int
portconfig(struct absout *eout,
	   char *portnum,
	   char *state,
	   char *timeout,
	   char *devname,
	   char *devcfg,
	   int  config_num)
{
    port_info_t *new_port, *curr, *prev;
    enum str_type str_type;

    new_port = malloc(sizeof(port_info_t));
    if (new_port == NULL) {
	eout->out(eout, "Could not allocate a port data structure");
	return -1;
    }
    memset(new_port, 0, sizeof(*new_port));

    INIT_LOCK(new_port->lock);

    new_port->accept_waiter = alloc_waiter();
    if (!new_port->accept_waiter) {
	eout->out(eout, "Could not allocate accept waiter data");
	goto errout;
    }

    new_port->waiter = alloc_waiter();
    if (!new_port->waiter) {
	eout->out(eout, "Could not allocate accept waiter data");
	goto errout;
    }

    if (sel_alloc_timer(ser2net_sel,
			got_timeout, new_port,
			&new_port->timer))
    {
	eout->out(eout, "Could not allocate timer data");
	goto errout;
    }

    if (sel_alloc_timer(ser2net_sel,
			send_timeout, new_port,
			&new_port->send_timer))
    {
	eout->out(eout, "Could not allocate timer data");
	goto errout;
    }

    if (sel_alloc_runner(ser2net_sel, &new_port->runshutdown)) {
	goto errout;
    }

    new_port->portname = strdup(portnum);
    if (!new_port->portname) {
	goto errout;
    }

    new_port->io.devname = find_str(devname, &str_type, NULL);
    if (new_port->io.devname) {
	if (str_type != DEVNAME) {
	    free(new_port->io.devname);
	    new_port->io.devname = NULL;
	}
    }
    if (!new_port->io.devname)
	new_port->io.devname = strdup(devname);
    if (!new_port->io.devname) {
	eout->out(eout, "unable to allocate device name");
	goto errout;
    }

    /* Errors from here on out must goto errout. */
    init_port_data(new_port);

    if (isallzero(new_port->portname)) {
	new_port->is_stdio = 1;
    } else if (scan_tcp_port(new_port->portname, &new_port->ai)) {
	eout->out(eout, "port number was invalid");
	goto errout;
    }

    if (strcmp(state, "raw") == 0) {
	new_port->enabled = PORT_RAW;
    } else if (strcmp(state, "rawlp") == 0) {
	new_port->enabled = PORT_RAWLP;
	new_port->io.read_disabled = 1;
    } else if (strcmp(state, "telnet") == 0) {
	new_port->enabled = PORT_TELNET;
    } else if (strcmp(state, "off") == 0) {
	new_port->enabled = PORT_DISABLED;
    } else {
	eout->out(eout, "state was invalid");
	goto errout;
    }

    new_port->timeout = scan_int(timeout);
    if (new_port->timeout == -1) {
	eout->out(eout, "timeout was invalid");
	goto errout;
    }

    new_port->io.user_data = new_port;

    if (strncmp(new_port->io.devname, "sol.", 4) == 0) {
	if (solcfg_init(&new_port->io, eout, devcfg, myconfig,
			new_port) == -1) {
	    eout->out(eout, "device configuration invalid");
	    goto errout;
	}
    } else {
	if (devcfg_init(&new_port->io, eout, devcfg, myconfig,
			new_port) == -1) {
	    eout->out(eout, "device configuration invalid");
	    goto errout;
	}
    }

    new_port->config_num = config_num;

    /* See if the port already exists, and reconfigure it if so. */
    prev = NULL;
    LOCK(ports_lock);
    curr = ports;
    while (curr != NULL) {
	if (strcmp(curr->portname, new_port->portname) == 0) {
	    /* We are reconfiguring this port. */
	    LOCK(curr->lock);
	    if (curr->dev_to_tcp_state == PORT_UNCONNECTED) {
		/* Port is disconnected, switch it now. */
		switchout_port(eout, new_port, curr, prev); /* releases lock */
	    } else {
		/* Mark it to be replaced later. */
		if (curr->new_config != NULL)
		    free_port(curr->new_config);
		curr->config_num = config_num;
		curr->new_config = new_port;
		UNLOCK(curr->lock);
	    }
	    goto out;
	} else {
	    prev = curr;
	    curr = curr->next;
	}
    }

    /* If we get here, the port is brand new, so don't do anything that
       would affect a port replacement here. */

    if (new_port->enabled != PORT_DISABLED) {
	if (startup_port(eout, new_port) == -1)
	    goto errout_unlock;
    }

    /* Tack it on to the end of the list of ports. */
    new_port->next = NULL;
    if (ports == NULL) {
	ports = new_port;
    } else {
	curr = ports;
	while (curr->next != NULL) {
	    curr = curr->next;
	}
	curr->next = new_port;
    }
 out:
    UNLOCK(ports_lock);

    return 0;

errout_unlock:
    UNLOCK(ports_lock);
errout:
    free_port(new_port);
    return -1;
}

void
clear_old_port_config(int curr_config)
{
    port_info_t *curr, *prev;

    prev = NULL;
    curr = ports;
    LOCK(ports_lock);
    while (curr != NULL) {
	if (curr->config_num != curr_config) {
	    /* The port was removed, remove it. */
	    LOCK(curr->lock);
	    if (curr->dev_to_tcp_state == PORT_UNCONNECTED) {
		change_port_state(NULL, curr, PORT_DISABLED); /* releases lock*/
		if (prev == NULL) {
		    ports = curr->next;
		    free_port(curr);
		    curr = ports;
		} else {
		    prev->next = curr->next;
		    free_port(curr);
		    curr = prev->next;
		}
	    } else {
		curr->config_num = -1;
		change_port_state(NULL, curr, PORT_DISABLED); /* releases lock*/
		prev = curr;
		curr = curr->next;
	    }
	} else {
	    prev = curr;
	    curr = curr->next;
	}
    }
    UNLOCK(ports_lock);
}

/* Print information about a port to the control port given in cntlr. */
static void
showshortport(struct controller_info *cntlr, port_info_t *port)
{
    char buffer[NI_MAXHOST], portbuff[NI_MAXSERV];
    int  count;
    int  need_space = 0;
    int  err;
    struct absout out = { .out = cntrl_absout, .data = cntlr };

    controller_outputf(cntlr, "%-22s ", port->portname);
    if (port->config_num == -1)
	controller_outputf(cntlr, "%-6s ", "DEL");
    else
	controller_outputf(cntlr, "%-6s ", enabled_str[port->enabled]);
    controller_outputf(cntlr, "%7d ", port->timeout);

    err = getnameinfo((struct sockaddr *) &(port->remote), sizeof(port->remote),
		      buffer, sizeof(buffer),
		      portbuff, sizeof(portbuff),
		      NI_NUMERICHOST | NI_NUMERICSERV);
    if (err) {
	strcpy(buffer, "*err*");
	sprintf(portbuff, "%s", gai_strerror(err));
    }

    count = controller_outputf(cntlr, "%s,%s", buffer, portbuff);
    while (count < 23) {
	controller_output(cntlr, " ", 1);
	count++;
    }

    controller_outputf(cntlr, "%-22s ", port->io.devname);
    controller_outputf(cntlr, "%-14s ", state_str[port->tcp_to_dev_state]);
    controller_outputf(cntlr, "%-14s ", state_str[port->dev_to_tcp_state]);
    controller_outputf(cntlr, "%9d ", port->tcp_bytes_received);
    controller_outputf(cntlr, "%9d ", port->tcp_bytes_sent);
    controller_outputf(cntlr, "%9d ", port->dev_bytes_received);
    controller_outputf(cntlr, "%9d ", port->dev_bytes_sent);

    if (port->enabled != PORT_RAWLP) {
	port->io.f->show_devcfg(&port->io, &out);
	need_space = 1;
    }

    if (port->tcp_to_dev_state != PORT_UNCONNECTED) {
	if (need_space) {
	    controller_output(cntlr, " ", 1);
	}
	    
	port->io.f->show_devcontrol(&port->io, &out);
    }
    controller_output(cntlr, "\r\n", 2);

}

/* Print information about a port to the control port given in cntlr. */
static void
showport(struct controller_info *cntlr, port_info_t *port)
{
    char buffer[NI_MAXHOST], portbuff[NI_MAXSERV];
    struct absout out = { .out = cntrl_absout, .data = cntlr };
    int err;

    controller_outputf(cntlr, "TCP Port %s\r\n", port->portname);
    controller_outputf(cntlr, "  enable state: %s\r\n",
		       enabled_str[port->enabled]);
    controller_outputf(cntlr, "  timeout: %d\r\n", port->timeout);

    err = getnameinfo((struct sockaddr *) &(port->remote), sizeof(port->remote),
		      buffer, sizeof(buffer),
		      portbuff, sizeof(portbuff),
		      NI_NUMERICHOST | NI_NUMERICSERV);
    if (err) {
	strcpy(buffer, "*err*");
	sprintf(portbuff, "%s", gai_strerror(err));
    }
    controller_outputf(cntlr, "  connected to (or last connection): %s,%s\r\n",
		       buffer, portbuff);

    controller_outputf(cntlr, "  device: %s\r\n", port->io.devname);

    controller_outputf(cntlr, "  device config: ");
    if (port->enabled == PORT_RAWLP) {
	controller_outputf(cntlr, "none\r\n");
    } else {
	port->io.f->show_devcfg(&port->io, &out);
	controller_outputf(cntlr, "\r\n");
    }

    controller_outputf(cntlr, "  device controls: ");
    if (port->tcp_to_dev_state == PORT_UNCONNECTED) {
	controller_outputf(cntlr, "not currently connected\r\n");
    } else {
	port->io.f->show_devcontrol(&port->io, &out);
	controller_outputf(cntlr, "\r\n");
    }

    controller_outputf(cntlr, "  tcp to device state: %s\r\n",
		      state_str[port->tcp_to_dev_state]);

    controller_outputf(cntlr, "  device to tcp state: %s\r\n",
		      state_str[port->dev_to_tcp_state]);

    controller_outputf(cntlr, "  bytes read from TCP: %d\r\n",
		      port->tcp_bytes_received);

    controller_outputf(cntlr, "  bytes written to TCP: %d\r\n",
		      port->tcp_bytes_sent);

    controller_outputf(cntlr, "  bytes read from device: %d\r\n",
		      port->dev_bytes_received);

    controller_outputf(cntlr, "  bytes written to device: %d\r\n",
		      port->dev_bytes_sent);

    if (port->config_num == -1) {
	controller_outputf(cntlr, "  Port will be deleted when current"
			   " session closes.\r\n");
    } else if (port->new_config != NULL) {
	controller_outputf(cntlr, "  Port will be reconfigured when current"
			   " session closes.\r\n");
    }
}

/* Find a port data structure given a port number. */
static port_info_t *
find_port_by_num(char *portstr, bool allow_deleted)
{
    port_info_t *port;

    port = ports;
    while (port != NULL) {
	if (strcmp(portstr, port->portname) == 0) {
	    LOCK(port->lock);
	    UNLOCK(ports_lock);
	    if (port->config_num == -1 && !allow_deleted) {
		UNLOCK(port->lock);
		return NULL;
	    }
	    return port;
	}
	port = port->next;
    }

    UNLOCK(ports_lock);
    return NULL;
}

/* Handle a showport command from the control port. */
void
showports(struct controller_info *cntlr, char *portspec)
{
    port_info_t *port;

    LOCK(ports_lock);
    if (portspec == NULL) {
	/* Dump everything. */
	port = ports;
	while (port != NULL) {
	    LOCK(port->lock);
	    showport(cntlr, port);
	    UNLOCK(port->lock);
	    port = port->next;
	}
	UNLOCK(ports_lock);
    } else {
	port = find_port_by_num(portspec, true);
	if (port == NULL) {
	    controller_outputf(cntlr, "Invalid port number: %s\r\n", portspec);
	} else {
	    showport(cntlr, port);
	    UNLOCK(port->lock);
	}
    }
}

/* Handle a showport command from the control port. */
void
showshortports(struct controller_info *cntlr, char *portspec)
{
    port_info_t *port;

    controller_outputf(cntlr,
	    "%-22s %-6s %7s %-22s %-22s %-14s %-14s %9s %9s %9s %9s %s\r\n",
	    "Port name",
	    "Type",
	    "Timeout",
	    "Remote address",
	    "Device",
	    "TCP to device",
	    "Device to TCP",
	    "TCP in",
	    "TCP out",
	    "Dev in",
	    "Dev out",
	    "State");
    LOCK(ports_lock);
    if (portspec == NULL) {
	/* Dump everything. */
	port = ports;
	while (port != NULL) {
	    LOCK(port->lock);
	    showshortport(cntlr, port);
	    UNLOCK(port->lock);
	    port = port->next;
	}
	UNLOCK(ports_lock);
    } else {
	port = find_port_by_num(portspec, true);
	if (port == NULL) {
	    controller_outputf(cntlr, "Invalid port number: %s\r\n", portspec);
	} else {
	    showshortport(cntlr, port);	    
	    UNLOCK(port->lock);
	}
    }
}

/* Set the timeout on a port.  The port number and timeout are passed
   in as strings, this code will convert them, return any errors, and
   perform the operation. */
void
setporttimeout(struct controller_info *cntlr, char *portspec, char *timeout)
{
    int timeout_num;
    port_info_t *port;

    LOCK(ports_lock);
    port = find_port_by_num(portspec, true);
    if (port == NULL) {
	controller_outputf(cntlr, "Invalid port number: %s\r\n", portspec);
    } else {
	timeout_num = scan_int(timeout);
	if (timeout_num == -1) {
	    controller_outputf(cntlr, "Invalid timeout: %s\r\n", timeout);
	} else {
	    port->timeout = timeout_num;
	    if (port->tcpfd != -1) {
		reset_timer(port);
	    }
	}
	UNLOCK(port->lock);
    }
}

/* Configure a port.  The port number and configuration are passed in
   as strings, this code will get the port and then call the code to
   configure the device. */
void
setportdevcfg(struct controller_info *cntlr, char *portspec, char *devcfg)
{
    port_info_t *port;
    struct absout out = { .out = cntrl_abserrout, .data = cntlr };

    LOCK(ports_lock);
    port = find_port_by_num(portspec, false);
    if (port == NULL) {
	controller_outputf(cntlr, "Invalid port number: %s\r\n", portspec);
    } else {
	if (port->io.f->reconfig(&port->io, &out, devcfg, myconfig, port) == -1)
	{
	    controller_outputf(cntlr, "Invalid device config\r\n");
	}
	UNLOCK(port->lock);
    }
}

/* Modify the controls of a port.  The port number and configuration
   are passed in as strings, this code will get the port and then call
   the code to control the device. */
void
setportcontrol(struct controller_info *cntlr, char *portspec, char *controls)
{
    port_info_t *port;

    LOCK(ports_lock);
    port = find_port_by_num(portspec, false);
    if (port == NULL) {
	controller_outputf(cntlr, "Invalid port number: %s\r\n", portspec);
	goto out;
    } else if (port->tcp_to_dev_state == PORT_UNCONNECTED) {
	controller_outputf(cntlr, "Port is not currently connected: %s\r\n",
			   portspec);
    } else {
	if (port->io.f->set_devcontrol(&port->io, controls) == -1) {
	    controller_outputf(cntlr, "Invalid device controls\r\n");
	}
    }
    UNLOCK(port->lock);
 out:
    return;
}

/* Set the enable state of a port. */
void
setportenable(struct controller_info *cntlr, char *portspec, char *enable)
{
    port_info_t *port;
    int         new_enable;
    struct absout eout = { .out = cntrl_abserrout, .data = cntlr };

    LOCK(ports_lock);
    port = find_port_by_num(portspec, false);
    if (port == NULL) {
	controller_outputf(cntlr, "Invalid port number: %s\r\n", portspec);
	goto out_unlock;
    }

    if (strcmp(enable, "off") == 0) {
	new_enable = PORT_DISABLED;
    } else if (strcmp(enable, "raw") == 0) {
	new_enable = PORT_RAW;
    } else if (strcmp(enable, "rawlp") == 0) {
	new_enable = PORT_RAWLP;
    } else if (strcmp(enable, "telnet") == 0) {
	new_enable = PORT_TELNET;
    } else {
	controller_outputf(cntlr, "Invalid enable: %s\r\n", enable);
	goto out_unlock;
    }

    change_port_state(&eout, port, new_enable); /* releases lock */
    return;

 out_unlock:
    UNLOCK(port->lock);
}

#if HAVE_DECL_TIOCSRS485
struct serial_rs485 *get_rs485_conf(void *data)
{
    port_info_t *port = data;

    return port->rs485conf;
}
#endif

/* Start data monitoring on the given port, type may be either "tcp" or
   "term" and only one direction may be monitored.  This return NULL if
   the monitor fails.  The monitor output will go to "fd". */
void *
data_monitor_start(struct controller_info *cntlr,
		   char                   *type,
		   char                   *portspec)
{
    port_info_t *port;

    LOCK(ports_lock);
    port = find_port_by_num(portspec, true);
    if (port == NULL) {
	char *err = "Invalid port number: ";
	controller_output(cntlr, err, strlen(err));
	controller_output(cntlr, portspec, strlen(portspec));
	controller_output(cntlr, "\r\n", 2);
	goto out;
    }

    if ((port->tcp_monitor != NULL) || (port->dev_monitor != NULL)) {
	char *err = "Port is already being monitored";
	controller_output(cntlr, err, strlen(err));
	controller_output(cntlr, "\r\n", 2);
	goto out_unlock;
    }
 
    if (strcmp(type, "tcp") == 0) {
	port->tcp_monitor = cntlr;
    } else if (strcmp(type, "term") == 0) {
	port->dev_monitor = cntlr;
    } else {
	char *err = "invalid monitor type: ";
	controller_output(cntlr, err, strlen(err));
	controller_output(cntlr, type, strlen(type));
	controller_output(cntlr, "\r\n", 2);
	port = NULL;
    }
 out_unlock:
    UNLOCK(port->lock);
 out:
    return port;
}

/* Stop monitoring the given id. */
void
data_monitor_stop(struct controller_info *cntlr,
		  void                   *monitor_id)
{
    port_info_t *port = (port_info_t *) monitor_id;
    port_info_t *curr;

    LOCK(ports_lock);
    curr = ports;
    while (curr) {
	if (curr == port) {
	    LOCK(port->lock);
	    port->tcp_monitor = NULL;
	    port->dev_monitor = NULL;
	    UNLOCK(port->lock);
	    break;
	}
    }
    UNLOCK(ports_lock);
}

void
disconnect_port(struct controller_info *cntlr,
		char *portspec)
{
    port_info_t *port;

    LOCK(ports_lock);
    port = find_port_by_num(portspec, true);
    if (port == NULL) {
	char *err = "Invalid port number: ";
 	controller_output(cntlr, err, strlen(err));
	controller_output(cntlr, portspec, strlen(portspec));
	controller_output(cntlr, "\r\n", 2);
	goto out;
    } else if (port->tcp_to_dev_state == PORT_UNCONNECTED) {
	char *err = "Port not connected: ";
 	controller_output(cntlr, err, strlen(err));
	controller_output(cntlr, portspec, strlen(portspec));
	controller_output(cntlr, "\r\n", 2);
	goto out_unlock;
    }

    shutdown_port(port, "disconnect");
 out:
    return;

 out_unlock:
    UNLOCK(port->lock);
}

static int
com_port_will(void *cb_data)
{
    port_info_t *port = cb_data;
    unsigned char data[3];

    if (! port->allow_2217)
	return 0;

    /* The remote end turned on RFC2217 handling. */
    port->is_2217 = 1;
    port->linestate_mask = 0;
    port->modemstate_mask = 255;
    port->last_modemstate = 0;

    
    /* send a modemstate notify */
    data[0] = TN_OPT_COM_PORT;
    data[1] = 107; /* Notify modemstate */
    data[2] = 0;
    if (port->io.f->get_modem_state(&port->io, data + 2) != -1) {
	port->last_modemstate = data[2];
    }
    telnet_send_option(&port->tn_data, data, 3);
    return 1;
}

static void
com_port_handler(void *cb_data, unsigned char *option, int len)
{
    port_info_t *port = cb_data;
    unsigned char outopt[MAX_TELNET_CMD_XMIT_BUF];
    int val;
    unsigned char ucval;
    int cisco_ios_baud_rates = 0;
    
    if (len < 2) 
	return;

    switch (option[1]) {
    case 0: /* SIGNATURE? */
    {
	/* truncate signature, if it exceeds buffer size */
	char *sig = port->signaturestr;
	int sign_len;

	if (!sig)
	    /* If not set, use a default. */
	    sig = rfc2217_signature;

	sign_len = strlen(sig);
	if (sign_len > (MAX_TELNET_CMD_XMIT_BUF - 2))
	    sign_len = MAX_TELNET_CMD_XMIT_BUF - 2;
	
	outopt[0] = 44;
	outopt[1] = 100;
	strncpy((char *) outopt+2, sig, sign_len);
	telnet_send_option(&port->tn_data, outopt, 2 + sign_len);
	break;
    }

    case 1: /* SET-BAUDRATE */
	if (len == 3) {
	    cisco_ios_baud_rates = 1;
	    val = option[2];
	} else {
	    if (len < 6)
		return;
	    /* Basically the same as:
	     *  val = ntohl(*((uint32_t *) (option+2)));
	     * but handled unaligned cases */
	    val = option[2] << 24;
	    val |= option[3] << 16;
	    val |= option[4] << 8;
	    val |= option[5];
	}

	port->io.f->baud_rate(&port->io, &val, cisco_ios_baud_rates,
			      &port->bps);
	recalc_port_chardelay(port);
	outopt[0] = 44;
	outopt[1] = 101;
	if (cisco_ios_baud_rates) {
	    outopt[2] = val;
	    telnet_send_option(&port->tn_data, outopt, 3);
	} else {
	    /* Basically the same as:
	     * *((uint32_t *) (outopt+2)) = htonl(val);
	     * but handles unaligned cases */
	    outopt[2] = val >> 24;
	    outopt[3] = val >> 16;
	    outopt[4] = val >> 8;
	    outopt[5] = val;
	    telnet_send_option(&port->tn_data, outopt, 6);
	}
	break;

    case 2: /* SET-DATASIZE */
	if (len < 3)
	    return;

	ucval = option[2];
	port->io.f->data_size(&port->io, &ucval, &port->bpc);
	recalc_port_chardelay(port);
	outopt[0] = 44;
	outopt[1] = 102;
	outopt[2] = ucval;
	telnet_send_option(&port->tn_data, outopt, 3);
	break;

    case 3: /* SET-PARITY */
	if (len < 3)
	    return;

	ucval = option[2];
	port->io.f->parity(&port->io, &ucval, &port->bpc);
	recalc_port_chardelay(port);
	outopt[0] = 44;
	outopt[1] = 103;
	outopt[2] = ucval;
	telnet_send_option(&port->tn_data, outopt, 3);
	break;

    case 4: /* SET-STOPSIZE */
	if (len < 3)
	    return;

	ucval = option[2];
	port->io.f->stop_size(&port->io, &ucval, &port->bpc);
	recalc_port_chardelay(port);
	outopt[0] = 44;
	outopt[1] = 104;
	outopt[2] = ucval;
	telnet_send_option(&port->tn_data, outopt, 3);
	break;

    case 5: /* SET-CONTROL */
	if (len < 3)
	    return;

	ucval = option[2];
	port->io.f->control(&port->io, &ucval);
	outopt[0] = 44;
	outopt[1] = 105;
	outopt[2] = ucval;
	telnet_send_option(&port->tn_data, outopt, 3);
	break;

    case 8: /* FLOWCONTROL-SUSPEND */
	port->io.f->flow_control(&port->io, 1);
	outopt[0] = 44;
	outopt[1] = 108;
	telnet_send_option(&port->tn_data, outopt, 2);
	break;

    case 9: /* FLOWCONTROL-RESUME */
	port->io.f->flow_control(&port->io, 0);
	outopt[0] = 44;
	outopt[1] = 109;
	telnet_send_option(&port->tn_data, outopt, 2);
	break;

    case 10: /* SET-LINESTATE-MASK */
	if (len < 3)
	    return;
	port->linestate_mask = option[2];
	outopt[0] = 44;
	outopt[1] = 110;
	outopt[2] = port->linestate_mask;
	telnet_send_option(&port->tn_data, outopt, 3);
	break;

    case 11: /* SET-MODEMSTATE-MASK */
	if (len < 3)
	    return;
	port->modemstate_mask = option[2];
	outopt[0] = 44;
	outopt[1] = 111;
	outopt[2] = port->modemstate_mask;
	telnet_send_option(&port->tn_data, outopt, 3);
	break;

    case 12: /* PURGE_DATA */
	if (len < 3)
	    return;
	val = option[2];
	port->io.f->flush(&port->io, &val);
	outopt[0] = 44;
	outopt[1] = 112;
	outopt[2] = val;
	telnet_send_option(&port->tn_data, outopt, 3);
	break;

    case 6: /* NOTIFY-LINESTATE */
    case 7: /* NOTIFY-MODEMSTATE */
    default:
	break;
    }
}

void
shutdown_ports(void)
{
    port_info_t *port = ports, *next;

    /* No need for a lock here, nothing can reconfigure the port list at
       this point. */
    while (port != NULL) {
	port->config_num = -1;
	next = port->next;
	change_port_state(NULL, port, PORT_DISABLED);
	shutdown_port(port, "program shutdown");
	port = next;
    }
}

int
check_ports_shutdown(void)
{
    return ports == NULL;
}
