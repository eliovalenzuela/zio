#!/usr/bin/python3

import stat
import PyZIO
import threading
import tkinter as tk
import matplotlib as matplot

from tkinter import ttk
matplot.use("TkAgg")
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.backends.backend_tkagg import NavigationToolbar2TkAgg

zgui = None
device = None
cset = None


class PyZIOAcquisition(threading.Thread):
    def __init__(self, trig):
        self.trig = trig
        self.__stop = False
        super(PyZIOAcquisition, self).__init__()

    def stop(self):
        print("Stopping ...")
        self.__stop = True

    def run(self):
        print("Acquiring ...")
        count = int(self.trig.spn_count.get())
        while (not self.__stop) and count > 0:
            blocks = []
            print("Acquisition %d" % count)

            gdev = zgui.notebook.winfo_children()[0]  # Device Tab

            for gchan in gdev.channels.winfo_children():
                chan = gchan.chan
                print(chan.enable)
                if chan.enable:
                    print("  Reading from channel %s ..." % chan)
                    block = chan.block_read()
                    print("    [Done]")
                    blocks.append((gchan, block[0], block[1]))
            self.trig.spn_count.delete(0, tk.END)
            self.trig.spn_count.insert(0, str(count))
            zgui.graph.plot(blocks)
            count -= 1

        self.__stop = True
        print("All acquisition completed")
        self.trig.event_generate("<<AcquisitionStopped>>")


class ZGuiAttribute(object):
    """
    This class describe a generic zio attribute as a set of graphical objects.

        [  LABEL  ] [GET] [SET]
    """
    def __init__(self, parent, attr, prow):
        self.parent = parent
        self.attr = attr
        self.prow = prow
        self.initUI()

    def initUI(self):
        self.lbl = ttk.Label(self.parent, text=str(self.attr.name))
        self.ent = ttk.Entry(self.parent)
        self.get = ttk.Button(self.parent, text="get", command=self.__get)
        self.set = ttk.Button(self.parent, text="set", command=self.__set)
        self.__get()

        if self.attr.permissions & (stat.S_IWOTH | stat.S_IWGRP | stat.S_IWUSR):
            self.ent.configure(background="white")

        else:
            self.ent.configure(background="gray", state="readonly")
            self.set.configure(state=tk.DISABLED)

        self.lbl.grid(row=self.prow, column=0)
        self.ent.grid(row=self.prow, column=1)
        self.get.grid(row=self.prow, column=2)
        self.set.grid(row=self.prow, column=3)

    def __get(self):
        self.ent.delete(0, tk.END)
        self.ent.insert(0, self.attr.value)

    def __set(self):
        try:
            self.attr.value = self.ent.get()
        except PyZIO.PyZIOException as e:
            print(e)
        finally:
            self.__get()


class ZGuiAttributes(ttk.Frame):
    OBJ_TYPE_DEV = 0
    OBJ_TYPE_CSET = 1
    OBJ_TYPE_CHAN = 2
    OBJ_TYPE_TRIG = 3
    OBJ_TYPE_BUF = 4

    def __init__(self, parent, tkn):
        self.tkn = tkn
        self.parent = parent
        super(ZGuiAttributes, self).__init__(self.parent)
        self.initUI()

    def initUI(self):
        self.gui_attrs = []
        for idx,attr in enumerate(self.tkn.attr):
            a = ZGuiAttribute(self, attr, idx)
            self.gui_attrs.append(a)

    def _update_attributes(self, head):
        self.attrCnt = tk.Frame(self)
        self.attrCnt.configure(pady=10)
        self.attrCnt.pack(fill="x", side="top")

        self.gui_attrs = []
        for attr in self.tkn.head.attr:
            self.gui_attrs.append(ZGuiAttribute(self.attrCnt, attr))


class ZGuiChannel(ttk.Frame):
    default_color = 0
    def __init__(self, parent, chan):
        self.parent = parent
        self.chan = chan
        super(ZGuiChannel, self).__init__(self.parent)
        self.initUI()

    def initUI(self):
        self.frm_btn = ttk.Frame(self)
        self.btn_enable = tk.Button(self.frm_btn, text="enable")
        self.btn_flush = tk.Button(self.frm_btn, text="flush",
                                   command=self.flush)
        self.frm_style = ttk.Frame(self)
        self.lbl_style = ttk.Label(self.frm_style, text="Style")
        self.cmb_color = ttk.Combobox(self.frm_style,
                                      values=["blue", "green", "red", "cyan",
                                              "magenta", "yellow", "black",
                                              "purple"],
                                      width=7, state="readonly")

        self.cmb_draw_mode = ttk.Combobox(self.frm_style, values=["Lines",
                                                                  "Points",
                                                                  "Points+Lines"],
                                          width=12, state="readonly")
        self.chan_attrs = ZGuiAttributes(self, self.chan)
        self.buf_attrs = ZGuiAttributes(self, self.chan.buffer)

        self.cmb_color.current(ZGuiChannel.default_color)
        ZGuiChannel.default_color = (ZGuiChannel.default_color + 1 ) & 0xF
        self.cmb_draw_mode.current(0)
        self.btn_cmd_disable()

        self.lbl_style.pack(side=tk.LEFT)
        self.cmb_color.pack(side=tk.LEFT)
        self.cmb_draw_mode.pack(side=tk.LEFT)

        self.btn_enable.pack(side=tk.LEFT)
        self.btn_flush.pack(side=tk.LEFT)

        self.frm_btn.pack(side=tk.TOP)
        self.frm_style.pack(side=tk.TOP)
        self.chan_attrs.pack(side=tk.TOP, expand=True, fill=tk.X)
        self.buf_attrs.pack(side=tk.TOP, expand=True, fill=tk.X)

    def btn_cmd_enable(self):
        self.chan.enable = True
        if self.chan.enable != True:
            return  # Nothing to do, failed to change status
        self.btn_enable.configure(text="enable",
                                  activebackground="#00FF00",
                                  background="#00FF00",
                                  command=self.btn_cmd_disable)

    def btn_cmd_disable(self):
        self.chan.enable = False
        if self.chan.enable != False:
            return  # Nothing to do, failed to change status
        self.btn_enable.configure(text="disable",
                                  activebackground="#FF0000",
                                  background="#FF0000",
                                  command=self.btn_cmd_enable)

    def flush(self):
        self.chan.buffer.flush()


class ZGuiNotebookChannels(ttk.Notebook):
    def __init__(self, parent):
        self.parent = parent
        super(ZGuiNotebookChannels, self).__init__(self.parent)
        self.initUI()

    def initUI(self):
        for chan in cset.chans:
            self.add(ZGuiChannel(self, chan), text=str(chan))


class ZGuiDevice(ttk.Frame):
    def __init__(self, parent):
        self.parent = parent
        super(ZGuiDevice, self).__init__(self.parent)
        self.initUI()

    def initUI(self):
        if device is None or cset is None:
            return

        self.btn_enable = tk.Button(self, text="enable")
        self.cmb_trg = ttk.Combobox(self, values=PyZIO.trigger_list(),
                                    state="readonly")
        self.cmb_trg.bind("<<ComboboxSelected>>", self.trg_cmd_change)
        self.device_attrs = ZGuiAttributes(self, device)
        self.cset_attrs = ZGuiAttributes(self, cset)
        self.channels = ZGuiNotebookChannels(self)

        self.cmb_trg.set(str(cset.trigger).split("-")[0])
        self.btn_cmd_enable()

        self.btn_enable.pack(side=tk.TOP)
        self.cmb_trg.pack(side=tk.TOP)
        self.device_attrs.pack(side=tk.TOP, expand=True, fill=tk.X, pady=5)
        self.cset_attrs.pack(side=tk.TOP, expand=True, fill=tk.X, pady=5)
        self.channels.pack(side=tk.TOP, expand=True, fill=tk.BOTH)

    def btn_cmd_enable(self):
        device.enable = True
        if device.enable == False:
            print("Cannot disable device %s" % device)
            return
        cset.enable = True
        if cset.enable == False:
            print("Cannot disable channel-set %s" % cset)
            return

        self.btn_enable.configure(text="enable",
                                  activebackground="#00FF00",
                                  background="#00FF00",
                                  command=self.btn_cmd_disable)

    def btn_cmd_disable(self):
        device.enable = False
        if device.enable == True:
            print("Cannot disable device %s" % device)
            return
        cset.enable = False
        if cset.enable == True:
            print("Cannot disable channel-set %s" % cset)
            return

        self.btn_enable.configure(text="disable",
                                  activebackground="#FF0000",
                                  background="#FF0000",
                                  command=self.btn_cmd_enable)

    def trg_cmd_change(self, event):
        pass


class ZGuiTrigger(ttk.Frame):
    def __init__(self, parent):
        self.parent = parent
        super(ZGuiTrigger, self).__init__(self.parent)
        self.initUI()

    def initUI(self):
        if device is None or cset is None:
            return

        self.btn_enable = tk.Button(self)
        self.spn_count = tk.Spinbox(self, from_=1, to=1000000)
        self.cset_attrs = ZGuiAttributes(self, cset.trigger)

        self.btn_cmd_disarm()

        self.btn_enable.pack(side=tk.TOP)
        self.spn_count.pack(side=tk.TOP)
        self.cset_attrs.pack(side=tk.TOP)

        self.bind("<<AcquisitionStopped>>", self.acq_stopped)

    def acq_stopped(self, event):
        self.btn_cmd_disarm()

    def btn_cmd_arm(self):
        self.acq = PyZIOAcquisition(self)
        self.acq.start()

        cset.trigger.enable = True
        print("Trigger has been armed")
        self.btn_enable.configure(text="armed",
                                  activebackground="#00FF00",
                                  background="#00FF00",
                                  command=self.btn_cmd_disarm)

    def btn_cmd_disarm(self):
        if hasattr(self, "acq"):
            print("Trigger disarm")
            self.acq.stop()
            print("Wait active acquisition to complete ...")
#            self.acq.join()
            del self.acq
        cset.trigger.enable = False
        print("Trigger has been disarmed")
        self.btn_enable.configure(text="disarmed",
                                  activebackground="#FF0000",
                                  background="#FF0000",
                                  command=self.btn_cmd_arm)


class ZGuiBuffer(ttk.Frame):
    def __init__(self, parent):
        self.parent = parent
        super(ZGuiBuffer, self).__init__(self.parent)
        self.initUI()

    def initUI(self):
        if device is None:
            return


class ZGuiNotebook(ttk.Notebook):
    def __init__(self, parent):
        self.parent = parent
        super(ZGuiNotebook, self).__init__(self.parent)
        self.initUI()

    def initUI(self):
        self.add(ZGuiDevice(self), text="Device")
        self.add(ZGuiTrigger(self), text="Trigger")


class ZGuiAcquisitionGraph(ttk.Frame):
    def __init__(self, parent):
        self.parent = parent
        super(ZGuiAcquisitionGraph, self).__init__(parent)
        self.initUI()

    def initUI(self):
        # Graph
        self.f = matplot.figure.Figure(figsize=(5, 4), dpi=100)
        self.a = self.f.add_subplot(111)
        self.p = self.a.plot([0, 1], [0, 0])

        self.canvas = FigureCanvasTkAgg(self.f, self)
        self.toolbar = NavigationToolbar2TkAgg(self.canvas, self)
        self.canvas.get_tk_widget().pack(side=tk.TOP,
                                         fill=tk.BOTH, expand=True)
        self.toolbar.pack(side=tk.TOP)

    def get_color(self, signal):
        signal.cmb_color.get()

    def plot(self, blocks):
        self.a.clear()
        for block in blocks:
            mode = block[0].cmb_draw_mode.current()
            if mode == 1:
                fmt = "x"
            elif mode == 2:
                fmt = "x-"
            else:
                fmt = "-"
            x = [x for x in range(block[1].nsamples)]
            self.p = self.a.plot(x, block[2], fmt,
                                 color=block[0].cmb_color.get())
        self.canvas.draw()


class ZGuiSelection(ttk.LabelFrame):
    def __init__(self, parent):
        self.parent = parent
        super(ZGuiSelection, self).__init__(self.parent, text="Selection")
        self.initUI()

    def initUI(self):
        self.lbl_dev = ttk.Label(self, text="Device")
        self.lbl_dev.pack(side=tk.LEFT)
        self.cmb_dev = ttk.Combobox(self, values=self.list_dev(),
                                    state="readonly")
        self.cmb_dev.bind("<<ComboboxSelected>>", self.change_dev)
        self.cmb_dev.pack(side=tk.LEFT)

        self.lbl_cset = ttk.Label(self, text="Channel-Set")
        self.lbl_cset.pack(side=tk.LEFT)
        self.cmb_cset = ttk.Combobox(self, state="readonly")
        self.cmb_cset.bind("<<ComboboxSelected>>", self.change_cset)
        self.cmb_cset.pack(side=tk.LEFT)

    def list_dev(self):
        return PyZIO.device_list()

    def list_cset(self):
        names = []
        if device is not None:
            for cset in device.csets:
                names.append(repr(cset))
        return names

    def change_dev(self, event):
        print("Device: selected")
        try:
            global device
            global cset
            device = PyZIO.Device(event.widget.get())
            cset = None
            # Update channel-set list
            self.cmb_cset.set("")
            self.cmb_cset.configure(values=self.list_cset())
        except PyZIO.PyZIOException as e:
            print(e)
            event.widget.set("")
            device = None

    def change_cset(self, event):
        print("Channel-Set: selected")
        global cset
        cset = device.csets[self.cmb_cset.current()]
        self.event_generate("<<DeviceSelected>>")


class ZGui(tk.Frame):
    def __init__(self, parent):
        self.parent = parent
        super(ZGui, self).__init__(self.parent)
        self.initUI()

    def initUI(self):
        self.selection = ZGuiSelection(self)
        self.selection.pack(side=tk.TOP)
        self.selection.bind("<<DeviceSelected>>", self.device_change)
        self.graph = ZGuiAcquisitionGraph(self)
        self.graph.pack(side=tk.LEFT, expand=True, fill=tk.BOTH)
        self.notebook = ZGuiNotebook(self)
        self.notebook.pack(side=tk.RIGHT, expand=True, fill=tk.BOTH)

    def device_change(self, event):
        print("Device has changed: %s - %s" % (device, cset))
        self.notebook.destroy()
        self.notebook = ZGuiNotebook(self)
        self.notebook.pack(side=tk.RIGHT, expand=True, fill=tk.BOTH)


if __name__ == "__main__":
    root = tk.Tk()
    zgui = ZGui(root)
    zgui.pack(expand=True, fill=tk.BOTH)
    root.mainloop()
