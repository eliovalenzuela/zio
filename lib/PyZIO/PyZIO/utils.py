"""@package docstring

@author: Federico Vaga <federico.vaga@cern.ch>
@copyright: Copyright (c) 2016 CERN
@license: GNU Public License version 3
"""

from ctypes import *


class ModuleList(Structure):
    _fields_ = [
        ("len", c_uint),
        ("names", POINTER(c_char_p)),
    ]


def buffer_list():
    libzio = cdll.LoadLibrary("libzio.so")
    libzio.uzio_buffer_list.restype = POINTER(ModuleList)

    mlist = libzio.uzio_buffer_list()
    names = []
    if mlist:
        print(mlist)
        for i in range(0, mlist.contents.len):
            names.append(str(mlist.contents.names[i].decode("utf-8")))

    libzio.uzio_module_list_free.argtypes = [POINTER(ModuleList)]
    libzio.uzio_module_list_free(mlist)

    return names


def device_list():
    libzio = cdll.LoadLibrary("libzio.so")
    libzio.uzio_device_list.restype = POINTER(ModuleList)

    mlist = libzio.uzio_device_list()
    names = []
    if mlist:
        for i in range(0, mlist.contents.len):
            names.append(str(mlist.contents.names[i].decode("utf-8")))
        libzio.uzio_module_list_free.argtypes = [POINTER(ModuleList)]
        libzio.uzio_module_list_free(mlist)

    return names


def trigger_list():
    libzio = cdll.LoadLibrary("libzio.so")
    libzio.uzio_trigger_list.restype = POINTER(ModuleList)

    mlist = libzio.uzio_trigger_list()
    names = []
    if mlist:
        for i in range(0, mlist.contents.len):
            names.append(str(mlist.contents.names[i].decode("utf-8")))
        libzio.uzio_module_list_free.argtypes = [POINTER(ModuleList)]
        libzio.uzio_module_list_free(mlist)

    return names
