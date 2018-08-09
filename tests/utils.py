#
# genio test utilities
#
# This file contains some classes and functions useful for testing
# genio handling
#

import os
import genio
import tempfile
import signal
import time
import curses.ascii

debug = 0

class HandlerException(Exception):
    """Exception for HandleData errors"""

    def __init__(self, value):
        self.value = value
    def __repr__(self):
        return repr(self.value)
    def __str__(self):
        return str(self.value)

class HandleData:
    """Data handler for testing genio.

    This is designed to handle input and output from genio.  To write
    data, call set_write_data() to set some data and write it.  To wait
    for data to be read, call set_compare() to wait for the given data
    to be read.

    This just starts things up and runs asynchronously.  You can wait
    for a completion with wait() or wait_timeout().

    The io handler is in the io attribute of the object.  The handler
    object of that io will be this object.
    """

    def __init__(self, o, iostr, bufsize, name = None, chunksize=10240):
        """Start a genio object with this handler"""
        if (name):
            self.name = name
        else:
            self.name = iostr
        self.waiter = genio.waiter(o)
        self.to_write = None
        self.to_compare = None
        self.ignore_input = False
        self.io = genio.genio(o, iostr, bufsize, self)
        self.io.handler = self
        self.chunksize = chunksize
        self.debug = 0
        return

    def set_compare(self, to_compare, start_reader = True):
        """Set some data to compare

        If start_reader is true (default), it enable the read callback.
        If the data does not compare, an exception is raised.
        """
        self.compared = 0
        self.to_compare = to_compare
        if (start_reader):
            self.io.read_cb_enable(True)
        return

    def set_write_data(self, to_write, start_writer = True):
        self.wrpos = 0
        self.wrlen = len(to_write)
        self.to_write = to_write
        if (start_writer):
            self.io.write_cb_enable(True)
        return

    def close(self):
        self.io.close(self)
        return

    def wait(self):
        self.waiter.wait()

    def wait_timeout(self, timeout):
        return self.waiter.wait_timeout(timeout)


    # Everything below here is internal handling functions.

    def read_callback(self, io, err, buf, flags):
        if (debug or self.debug) and self.to_compare:
            print("%s: Got %d bytes at pos %d of %d" % (self.name, len(buf),
                                                        self.compared,
                                                        len(self.to_compare)))
        if (debug >= 2 or self.debug >= 2):
            s = ""
            for i in buf:
                if curses.ascii.isprint(i):
                    s = s + i
                else:
                    s = s + "\\x%2.2x" % ord(i)
            print("%s: Got data: %s" % (self.name, s))
        if (self.ignore_input):
            return len(buf)
        if (not self.to_compare):
            if (debug):
                print(self.name + ": Got data, but nothing to compare")
            io.read_cb_enable(False)
            return len(buf)
        if (err):
            raise HandlerException(self.name + ": read: " + err)

        if (len(buf) > len(self.to_compare)):
            count = len(self.to_compare)
        else:
            count = len(buf)

        for i in range(0, count):
            if (buf[i] != self.to_compare[self.compared]):
                raise HandlerException("%s: compare falure on byte %d, "
                                       "expected %x, got %x" %
                                       (self.name, self.compared,
                                        ord(self.to_compare[self.compared]),
                                        ord(buf[i])))
            self.compared += 1

        if (self.compared >= len(self.to_compare)):
            self.to_compare = None
            io.read_cb_enable(False)
            self.waiter.wake()

        return count

    def write_callback(self, io):
        if (not self.to_write):
            if (debug or self.debug):
                print(self.name + ": Got write, but no data")
            io.write_cb_enable(False)
            return

        if (self.wrpos + self.chunksize > self.wrlen):
            wrdata = self.to_write[self.wrpos:]
        else:
            wrdata = self.to_write[self.wrpos:self.wrpos + self.chunksize]
        count = io.write(wrdata)
        if (debug or self.debug):
            print(self.name + ": wrote %d bytes" % count)

        if (count + self.wrpos >= self.wrlen):
            io.write_cb_enable(False)
            self.to_write = None
            self.waiter.wake()
        else:
            self.wrpos += count
        return

    def urgent_callback(self, io):
        print(self.name + ": Urgent data")
        return

    def close_done(self, io):
        if (debug or self.debug):
            print(self.name + ": Closed")
        self.waiter.wake()
        return

class Ser2netDaemon:
    """Create a ser2net daemon instance and start it up

    ser2net is started with the given config data as a config file
    The SER2NET_EXEC environment variable can be set to tell ser2net
    to run ser2net with a specific path.

    For testing stdio handling for ser2net, you may use the io
    attribute for it but you must set it's handler's ignore_input
    attribute to False or you won't get any data, and you must
    set it back to True when done.
    """

    def __init__(self, o, configdata, extra_args = ""):
        """Create a running ser2net program

        The given config data is written to a file and used as the config file.
        It is started with the -r and -d options set, you can supply extra
        options if you like as a string.
        """
        
        prog = os.getenv("SER2NET_EXEC")
        if (not prog):
            prog = "ser2net"
        self.cfile = tempfile.NamedTemporaryFile(mode="w+")
        self.cfile.write(configdata)
        self.cfile.flush()

        args = "stdio," + prog + " -r -d -c " + self.cfile.name + " " + extra_args
        if (debug):
            print("Running: " + args)
        self.handler = HandleData(o, args, 1024, name="ser2net daemon")

        self.io = self.handler.io
        self.io.open_s()

        self.pid = self.io.remote_id()
        self.handler.set_compare("Ready\n")
        if (self.handler.wait_timeout(2000)):
            raise Exception("Timeout waiting for ser2net to start")

        self.handler.ignore_input = True

        # Uncomment the following or set it yourself to get output from
        # the ser2net daemon printed.
        #self.handler.debug = 2

        # Leave read on so if we enable debug we can see output from the
        # daemon.
        self.io.read_cb_enable(True)
        return

    def __del__(self):
        if (self.handler):
            self.terminate()
        return

    def signal(self, sig):
        """"Send a signal to ser2net"""
        os.kill(self.pid, sig)
        return

    def terminate(self):
        """Terminate the running ser2net

        This closes the io and sends a SIGTERM to ser2net and waits
        a bit for it to terminate.  If it does not terminate, send
        SIGTERM a few more times.  If it still refuses to close, send
        a SIGKILL.  If all that fails, raise an exception.
        """
        if (debug):
            print("Terminating")
        self.handler.close()
        count = 10
        while (count > 0):
            if (count < 6):
                self.signal(signal.SIGTERM)
            else:
                self.signal(signal.SIGKILL)
            # It would be really nice if waitpid had a timeout options,
            # in absense of that simulate it, sort of.
            subcount = 500
            while (subcount > 0):
                time.sleep(.01)
                pid, rv = os.waitpid(self.pid, os.WNOHANG)
                if (pid > 0):
                    self.handler = None
                    return
                subcount -= 1
            count -= 1
        raise Exception("ser2net did not terminate");

def alloc_io(o, iostr, do_open = True, bufsize = 1024):
    """Allocate an io instance with a HandlerData handler

    If do_open is True (default), open it, too.
    """
    h = HandleData(o, iostr, bufsize)
    if (do_open):
        h.io.open_s()
    return h.io

def test_dataxfer(io1, io2, data, timeout = 1000):
    """Test a transfer of data from io1 to io2

    If the transfer does not complete by "timeout" milliseconds, raise
    an exception.
    """
    io1.handler.set_write_data(data)
    io2.handler.set_compare(data)
    if (io1.handler.wait_timeout(timeout)):
        raise Exception("%s: %s: Timed out waiting for write completion" %
                        ("test_dataxfer", io1.handler.name))
    if (io2.handler.wait_timeout(timeout)):
        raise Exception("%s: %s: Timed out waiting for read completion" %
                        ("test_dataxfer", io2.handler.name))
    return

def test_dataxfer_simul(io1, io2, data, timeout = 10000):
    """Test a simultaneous bidirectional transfer of data between io1 to io2

    If the transfer does not complete by "timeout" milliseconds, raise
    an exception.
    """
    io1.handler.set_write_data(data)
    io1.handler.set_compare(data)
    io2.handler.set_write_data(data)
    io2.handler.set_compare(data)
    if (io1.handler.wait_timeout(timeout)):
        raise Exception("%s: %s: Timed out waiting for write completion" %
                        ("test_dataxfer", io1.handler.name))
    if (io2.handler.wait_timeout(timeout)):
        raise Exception("%s: %s: Timed out waiting for write completion" %
                        ("test_dataxfer", io2.handler.name))
    if (io1.handler.wait_timeout(timeout)):
        raise Exception("%s: %s: Timed out waiting for read completion" %
                        ("test_dataxfer", io1.handler.name))
    if (io2.handler.wait_timeout(timeout)):
        raise Exception("%s: %s: Timed out waiting for read completion" %
                        ("test_dataxfer", io2.handler.name))
    return

def io_close(io, timeout = 1000):
    """close the given genio

    If it does not succeed in timeout milliseconds, raise and exception.
    """
    io.handler.close()
    if (io.handler.wait_timeout(timeout)):
        raise Exception("%s: %s: Timed out waiting for close" %
                        ("io_close", io.handler.name))
    return

def setup_2_ser2net(o, config, io1str, io2str):
    """Setup a ser2net daemon and two genio connections

    Create a ser2net daemon instance with the given config and two
    genio connections with the given strings.

    If io1str is None, use the stdio of the ser2net connection as
    io1 (generally for testing stdio to ser2net).

    A "closeme" boolean attribute is added to io1 telling if They
    should be closed upon completion of the test, this is set to false
    for ser2net stdio.
    """
    io1 = None
    io2 = None
    ser2net = Ser2netDaemon(o, config)
    try:
        if (io1str):
            io1 = alloc_io(o, io1str)
            io1.closeme = True
        else:
            io1 = ser2net.io
            io1.handler.ignore_input = False
            io1.closeme = False
        io2 = alloc_io(o, io2str)
    except:
        if io1:
            if io1.closeme:
                io_close(io1)
        if io2:
            io_close(io2)
        ser2net.terminate()
        raise
    return (ser2net, io1, io2)

def finish_2_ser2net(ser2net, io1, io2):
    if (io1.closeme):
        io_close(io1)
    else:
        io1.handler.ignore_input = True
    io_close(io2)
    ser2net.terminate()
    return
