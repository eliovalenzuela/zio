"""@package docstring

@author: Federico Vaga <federico.vaga@cern.ch>
@copyright: Copyright (c) 2016 CERN
@license: GNU Public License version 3
"""
from ctypes import *
import os
import time
import struct

libzio = CDLL("libzio.so", use_errno=True)
uzio_strerror = libzio.uzio_strerror
uzio_strerror.argtypes = [c_uint, ]
uzio_strerror.restype = c_char_p

class zio_tlv(Structure):
    _fields_ = [
        ("type", c_uint32),
        ("length", c_uint32),
        ("payload", c_uint8 * 8),
        ]


class zio_addr(Structure):
    _fields_ = [
        ("sa_family", c_uint16),
        ("host_type", c_uint8),
        ("filler", c_uint8),
        ("hostid", c_char * 8),
        ("dev_id", c_uint32),
        ("cset", c_uint16),
        ("chan", c_uint16),
        ("devname", c_char * 12),
        ]


class zio_timestamp(Structure):
    _fields_ = [
        ("secs", c_uint64),
        ("ticks", c_uint64),
        ("bins", c_uint64),
        ]



class zio_ctrl_attr(Structure):
    _fields_ = [
        ("std_mask", c_uint16),
        ("unused", c_uint16),
        ("ext_mask", c_uint32),
        ("std_val", c_uint32 * 16),
        ("ext_val", c_uint32 * 32),
        ]


class zio_control(Structure):
    _fields_ = [
        ("major_version", c_uint8),
        ("minor_version", c_uint8),
        ("zio_alarms", c_uint8),
        ("drv_alarms", c_uint8),
        ("seq_num", c_uint32),
        ("nsamples", c_uint32),
        ("ssize", c_uint16),
        ("nbits", c_uint16),
        ("addr", zio_addr),
        ("tstamp", zio_timestamp),
        ("mem_offset", c_uint32),
        ("reserved", c_uint32),
        ("flags", c_uint32),
        ("triggername", c_char * 12),
        ("attr_channel", zio_ctrl_attr),
        ("attr_trigger", zio_ctrl_attr),
        ("tlv", zio_tlv * 1),
        ]


class uzio_block(Structure):
    _fields_ = [
        ("ctrl", zio_control),
        ("data", c_void_p),
        ("datalen", c_size_t),
        ]


class uzio_attribute(Structure):
    _fields_ = [
        ("parent", c_void_p),
        ("path", c_char * 256)
        ]


class uzio_object(Structure):
    _fields_ = [
        ("parent", c_void_p),
        ("sysbase", c_char * 256),
        ("name", c_char * 24),
        ("devname", c_char * 24),
        ("type", c_uint),
        ("enable", uzio_attribute),
        ("__name", uzio_attribute),
        ("__devname", uzio_attribute),
        ("__type", uzio_attribute),
        ("std", uzio_attribute * 16),
        ("ext", uzio_attribute * 32),
        ]


class uzio_buffer(Structure):
    _fields_ = [
        ("head", uzio_object),
        ("flush", uzio_attribute),
        ]


class uzio_trigger(Structure):
    _fields_ = [
        ("head", uzio_object),
        ]


class uzio_channel(Structure):
    _fields_ = [
        ("head", uzio_object),
        ("fd_data", c_int),
        ("fd_ctrl", c_int),
        ("current_ctrl", uzio_attribute),
        ("alarms", uzio_attribute),
        ("buffer", uzio_buffer),
        ]


class uzio_cset(Structure):
    _fields_ = [
        ("head", uzio_object),
        ("direction", uzio_attribute),
        ("current_buffer", uzio_attribute),
        ("current_trigger", uzio_attribute),
        ("flags", c_ulong),
        ("trigger", uzio_trigger),
        ("chan", POINTER(uzio_channel)),
        ("n_chan", c_uint),
        ]


class uzio_device(Structure):
    _fields_ = [
        ("head", uzio_object),
        ("cset", POINTER(uzio_cset)),
        ("n_cset", c_uint),
        ]


class PyZIOException(Exception):
    def __init__(self, msg):
        super(PyZIOException, self).__init__(msg)


class Attribute(object):
    def __init__(self, attr):
        self.tkn = attr

    @property
    def name(self):
        return os.path.basename(self.tkn.path).decode()

    @property
    def permissions(self):
        return os.stat(self.tkn.path).st_mode

    @property
    def value(self):
        libzio.uzio_attr_value_get.argtypes = [POINTER(uzio_attribute),
                                               POINTER(c_uint32)]
        libzio.uzio_attr_value_get.restype = c_int
        val = c_uint32(0)
        err = libzio.uzio_attr_value_get(self.tkn, pointer(val))
        if err < 0:
            raise PyZIOException("Cannot read from %s: %d" % \
                                         (repr(self),
                                          uzio_strerror(get_errno())))
        return val.value

    @value.setter
    def value(self, pval):
        libzio.uzio_attr_value_set.argtypes = [POINTER(uzio_attribute),
                                               c_uint32]
        libzio.uzio_attr_value_set.restype = c_int
        val = c_uint32(int(pval))
        err = libzio.uzio_attr_value_set(self.tkn, val)
        if err < 0:
            raise PyZIOException("Cannot write to %s: %d" % \
                                         (repr(self),
                                          uzio_strerror(get_errno())))

    def __repr__(self):
        return "%s = %d" % (self.tkn.path, self.value)

    def __str__(self):
        return "%s = %d" % (self.name, self.value)


class Head(object):
    def __init__(self, head):
        self.head = head

        i = 0
        self.attr = []
        while len(self.head.std[i].path) > 0:
            print("%s :%d" % (self.head.std[i].path, i))
            self.attr.append(Attribute(self.head.std[i]))
            i += 1
        i = 0
        while len(head.ext[i].path) > 0:
            self.attr.append(Attribute(self.head.ext[i]))
            i += 1

    def enable(self):
        """
        It enables the ZIO object
        """
        with open(self.head.enable.path, 'w') as f:
            f.write("1")

    def disable(self):
        """
        It disables the ZIO object
        """
        with open(self.head.enable.path, 'w') as f:
            f.write("0")

    def __repr__(self):
        return "%s" % (self.head.devname.decode())

    def __str__(self):
        return "%s" % (self.head.name.decode())


class Trigger(Head):
    def __init__(self, trig):
        self.tkn = trig

        super(Trigger, self).__init__(self.tkn.head)


class Buffer(Head):
    def __init__(self, buf):
        self.tkn = buf

        super(Buffer, self).__init__(self.tkn.head)


class Channel(Head):
    def __init__(self, chan):
        self.tkn = chan

        super(Channel, self).__init__(self.tkn.head)

    def __binary_to_list(self, ctrl, binary):
        size = "b"
        if ctrl.ssize == 1:
            data = cast(binary, POINTER(c_uint8 * ctrl.nsamples))
        elif ctrl.ssize == 2:
            data = cast(binary, POINTER(c_uint16 * ctrl.nsamples))
        elif ctrl.ssize == 4:
            data = cast(binary, POINTER(c_uint32 * ctrl.nsamples))
        elif ctrl.ssize == 8:
            data = cast(binary, POINTER(c_uint64 * ctrl.nsamples))

        return data.contents

    def block_read(self):
        uzio_block_read = libzio.uzio_block_read
        uzio_block_read.argtypes = [POINTER(uzio_channel), ]
        uzio_block_read.restype = POINTER(uzio_block)

        block = uzio_block_read(self.tkn)
        if not block:
            raise PyZIOException("Cannot read block on channel '%s': %s" % \
                                 (self, uzio_strerror(get_errno())))
        ctrl = block.contents.ctrl
        data = self.__binary_to_list(ctrl, block.contents.data)
        return (ctrl, data)


class ChannelSet(Head):
    def __init__(self, cset):
        self.tkn = cset

        # Start scanning Csets
        self.chans = []
        for i in range(0, self.tkn.n_chan):
            self.chans.append(Channel(self.tkn.chan[i]))

        super(ChannelSet, self).__init__(self.tkn.head)

    def block_read(self):
        """
        It reads all block from channels
        """
        blocks = []
        for chan in self.chans:
            blocks.append(chan.read_block())
        return blocks


class Device(Head):
    def __init__(self, devname=None, devid=None):
        self.device = None
        if devname is None:
            raise PyZIOException("Device-name ismandatory in order to create a device instance")
        self.devname = devname.encode()
        self.devid = devid


        if self.devid is None:
            uzio_device_open = libzio.uzio_device_open_by_name
            uzio_device_open.argtypes = [c_char_p, ]
            uzio_device_open.restype = POINTER(uzio_device)
            self.device = uzio_device_open(self.devname)
            if not self.device:
                raise PyZIOException("Failed to open device %s: %s" % \
                                         (self.devname,
                                          uzio_strerror(get_errno())))
        else:
            uzio_device_open = libzio.uzio_device_open
            uzio_device_open.argtypes = [c_char_p, c_uint32]
            uzio_device_open.restype = POINTER(uzio_device)
            self.device = uzio_device_open(self.devname, self.devid)
            if not self.device:
                raise PyZIOException("Failed to open device %s-%04x: %s" % \
                                         (self.devname, self.devid,
                                          uzio_strerror(get_errno())))

        self.tkn = self.device.contents
        super(Device, self).__init__(self.tkn.head)

        # Start scanning Csets
        self.csets = []
        for i in range(0, self.device.contents.n_cset):
            self.csets.append(ChannelSet(self.device.contents.cset[i]))


    def __del__(self):
        if self.device is None:
            return
        uzio_device_close = libzio.uzio_device_close
        uzio_device_close.argtypes = [POINTER(uzio_device), ]
        uzio_device_close(self.device)
