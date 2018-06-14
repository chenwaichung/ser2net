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

/*
 * This include file defines a network I/O abstraction to allow code
 * to use TCP, UDP, stdio, etc. without having to know the underlying
 * details.
 */

#ifndef SER2NET_NETIO_H
#define SER2NET_NETIO_H

#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include "utils/selector.h"

struct netio;

struct netio_callbacks {
    /*
     * Called when data is read from the I/O device.
     *
     * If readerr is zero, buf points to a data buffer and buflen is
     * the number of bytes available.
     *
     * If readerr is set, buf and buflen are undefined.  readerr is
     * a standard *nix errno.
     *
     * The user must return the number of bytes consumed.  If the full
     * number of bytes is not consumed, read will automatically be
     * disabled.  Read is also disabled if an error is reported.
     *
     * Flags are per-type options, they generally don't matter except
     * for some specific situations.
     */
    unsigned int (*read_callback)(struct netio *net, int readerr,
				  unsigned char *buf, unsigned int buflen,
				  unsigned int flags);

    /* Flags for read callbacks. */

/* For stdin client netio, data is from stderr instead of stdout. */    
#define NETIO_ERR_OUTPUT	1

    /*
     * Called when the user may write to the netio.
     */
    void (*write_callback)(struct netio *net);

    /*
     * Called when urgent data is available.  This should only be done
     * on TCP sockets.  Optional.
     */
    void (*urgent_callback)(struct netio *net);

    /*
     * Called when a close operation completes.  May be NULL.
     */
    void (*close_done)(struct netio *net);
};

/*
 * Set the callback data for the net.  This must be done in the
 * new_connection callback for the acceptor before any other operation
 * is done on the netio.  The only exception is that netio_close() may
 * be called with callbacks not set.  This function may be called
 * again if the netio is not enabled.
 */
void netio_set_callbacks(struct netio *net,
			 const struct netio_callbacks *cbs, void *user_data);

/*
 * Return the user data supplied in netio_set_callbacks().
 */
void *netio_get_user_data(struct netio *net);

/*
 * Set the user data.  May be called if the netio is not enabled.
 */
void netio_set_user_data(struct netio *net, void *user_data);

/*
 * Write data to the netio.  This should only be called from the
 * write callback for most general usage.  Writes buflen bytes
 * from buf.
 *
 * Returns errno on error, or 0 on success.  This will NEVER return
 * EAGAIN, EWOULDBLOCK, or EINTR.  Those are handled internally.
 *
 * On a non-error return, count is set to the number of bytes
 * consumed by the write call, with may be less than buflen.  If
 * it is less than buflen, then not all the data was written.
 * Note that count may be set to zero.  This can happen on an
 * EAGAIN type situation.
 */
int netio_write(struct netio *net, int *count,
		const void *buf, unsigned int buflen);

/*
 * Convert the remote address for this network connection to a
 * string.  The string starts at buf + *pos and goes to buf +
 * buflen.  If pos is NULL, then zero is used.  The string is
 * NIL terminated.
 *
 * Returns an errno on an error, and a string error will be put
 * into the buffer.
 *
 * In all cases, if pos is non-NULL it will be updated to be the
 * NIL char after the last byte of the string, where you would
 * want to put any new data into the string.
 */
int netio_raddr_to_str(struct netio *net, int *pos,
		       char *buf, unsigned int buflen);

/*
 * Return the remote address for the connection.
 */
socklen_t netio_get_raddr(struct netio *net,
			  struct sockaddr *addr, socklen_t addrlen);
/*
 * Close the netio.  Note that the close operation is not complete
 * until close_done() is called.
 */
void netio_close(struct netio *net);

/*
 * Enable or disable data to be read from the network connection.
 */
void netio_set_read_callback_enable(struct netio *net, bool enabled);

/*
 * Enable the write_callback when data can be written on the
 * network connection.
 */
void netio_set_write_callback_enable(struct netio *net, bool enabled);

struct netio_acceptor;

struct netio_acceptor_callbacks {
    /*
     * A new net connection for the acceptor is in net.
     */
    void (*new_connection)(struct netio_acceptor *acceptor, struct netio *net);

    /*
     * The shutdown operation is complete.  May be NULL.
     */
    void (*shutdown_done)(struct netio_acceptor *acceptor);
};

/*
 * Return the user data supplied to the allocator.
 */
void *netio_acceptor_get_user_data(struct netio_acceptor *acceptor);

/*
 * Set the user data.  May be called if the acceptor is not enabled.
 */
void netio_acceptor_set_user_data(struct netio_acceptor *acceptor,
				  void *user_data);

/*
 * An acceptor is allocated without opening any sockets.  This
 * actually starts up the acceptor, allocating the sockets and
 * such.  It is started with accepts enabled.
 *
 * Returns a standard errno on an error, zero otherwise.
 */
int netio_acc_startup(struct netio_acceptor *acceptor);

/*
 * Closes all sockets and disables everything.  shutdown_complete()
 * will be called if successful after the shutdown is complete.
 *
 * Returns a EAGAIN if the acceptor is already shut down, zero
 * otherwise.
 */
int netio_acc_shutdown(struct netio_acceptor *acceptor);

/*
 * Enable the accept callback when connections come in.
 */
void netio_acc_set_accept_callback_enable(struct netio_acceptor *acceptor,
					  bool enabled);

/*
 * Free the network acceptor.  If the network acceptor is started
 * up, this shuts it down first and shutdown_complete() is NOT called.
 */
void netio_acc_free(struct netio_acceptor *acceptor);

/*
 * Returns if the acceptor requests exit on close.  A hack for stdio.
 */
bool netio_acc_exit_on_close(struct netio_acceptor *acceptor);

/*
 * Convert a string representation of a network address into a network
 * acceptor.  max_read_size is the internal read buffer size for the
 * connections.
 */
int str_to_netio_acceptor(const char *str, struct selector_s *sel,
			  unsigned int max_read_size,
			  const struct netio_acceptor_callbacks *cbs,
			  void *user_data,
			  struct netio_acceptor **acceptor);

/*
 * Convert a string representation of a network address into a
 * client netio.
 */
int str_to_netio(const char *str,
		 struct selector_s *sel,
		 unsigned int max_read_size,
		 const struct netio_callbacks *cbs,
		 void *user_data,
		 struct netio **netio);

/*
 * Allocators for different I/O types.
 */
int tcp_netio_acceptor_alloc(const char *name,
			     struct selector_s *sel,
			     struct addrinfo *ai,
			     unsigned int max_read_size,
			     const struct netio_acceptor_callbacks *cbs,
			     void *user_data,
			     struct netio_acceptor **acceptor);
int udp_netio_acceptor_alloc(const char *name,
			     struct selector_s *sel,
			     struct addrinfo *ai,
			     unsigned int max_read_size,
			     const struct netio_acceptor_callbacks *cbs,
			     void *user_data,
			     struct netio_acceptor **acceptor);
int stdio_netio_acceptor_alloc(struct selector_s *sel,
			       unsigned int max_read_size,
			       const struct netio_acceptor_callbacks *cbs,
			       void *user_data,
			       struct netio_acceptor **acceptor);

/* Client allocators. */

int tcp_netio_alloc(struct addrinfo *ai,
		    struct selector_s *sel,
		    unsigned int max_read_size,
		    const struct netio_callbacks *cbs,
		    void *user_data,
		    struct netio **new_netio);

int udp_netio_alloc(struct addrinfo *ai,
		    struct selector_s *sel,
		    unsigned int max_read_size,
		    const struct netio_callbacks *cbs,
		    void *user_data,
		    struct netio **new_netio);

/* Run a program (in argv[0]) and attach to it's stdio. */
int stdio_netio_alloc(char *const argv[],
		      struct selector_s *sel,
		      unsigned int max_read_size,
		      const struct netio_callbacks *cbs,
		      void *user_data,
		      struct netio **new_netio);

/*
 * Compare two sockaddr structure and return TRUE if they are equal
 * and FALSE if not.  Only works for AF_INET4 and AF_INET6.
 * If a2->sin_port is zero, then the port comparison is ignored.
 */
bool sockaddr_equal(const struct sockaddr *a1, socklen_t l1,
		    const struct sockaddr *a2, socklen_t l2,
		    bool compare_ports);

/*
 * Scan for a network port in the form:
 *
 *   [ipv4|ipv6,][tcp|udp,][<hostname>,]<port>
 *
 * If neither ipv4 nor ipv6 is specified, addresses for both are
 * returned.  If neither tcp nor udp is specified, tcp is assumed.
 * The hostname can be a resolvable hostname, an IPv4 octet, or an
 * IPv6 address.  If it is not supplied, inaddr_any is used.  In the
 * absence of a hostname specification, a wildcard address is used.
 * The mandatory second part is the port number or a service name.
 *
 * If the port is all zero, then is_port_set is set to true, false
 * otherwise.  If the address is UDP, is_dgram is set to true, false
 * otherwise.
 */
int scan_network_port(const char *str, struct addrinfo **ai, bool *is_dgram,
		      bool *is_port_set);

/*
 * Helper function for dealing with buffers writing to netio.
 */
 int netio_buffer_do_write(void *cb_data,
			   void  *buf, size_t buflen, size_t *written);

#endif /* SER2NET_NETIO_H */
