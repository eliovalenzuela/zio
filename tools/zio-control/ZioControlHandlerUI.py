"""
@author: Federico Vaga
@copyright: Federico Vaga 2012
@license: GPLv2
"""

from ZioCtrl import *
from ZioControlUI import *
import stat, re

try:
    _fromUtf8 = QtCore.QString.fromUtf8
except AttributeError:
    _fromUtf8 = lambda s: s

class ZioAttrMap(object):
    def __init__(self, desc_line):
        field = desc_line.split()
        
                # Set name
        self.fullpath = field[0]
        self.name = self.fullpath.split('/')[-1]
        self.path = self.fullpath.replace(self.name, "")

        # Set type
        self.is_extended = True if field[1] == 'e' else False

        # Set index
        self.index = int(field[2])

        # Set mode
        tmp = int(field[3])
        S_IWUGO = stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH
        S_IRUGO = stat.S_IRUSR | stat.S_IRGRP | stat.S_IROTH
        self.is_writable = True if tmp & S_IWUGO else False
        self.is_readable = True if tmp & S_IRUGO else False

class ZioExtendedAttribute(object):
    """
    This class creates a GUI for an attribute. It creates a QLabel and a
    QLineEdit at a given position (x,y) in a parent widget
    """

    def __init__(self, parent, length):
        self.length = length
        print("Create a layout for " + parent.objectName())
        self.ext_type = "trigger" if "trg" in parent.objectName() else "channel"
        # Get the layout of the parent QWidget
        layout = parent.layout()

        self.__attr_list = []
        for i in range(0, self.length):
            name = "Attribute " + str(i)
            attr = ZioAttributeGUI(parent, name, None)
            #self.layout.addRow(attr.lbl, attr.edt)
            layout.addRow(attr.lbl, attr.edt)
            self.__attr_list.append(attr)

    def updateLabels(self, map_list):
        for map in map_list:
            if not map.is_extended:
                continue
            
            if self.ext_type == "trigger" and self.ext_type in map.fullpath: # trigger
                self.__attr_list[map.index].lbl.setText(_fromUtf8(map.name))
            
            if self.ext_type == "channel" and not "trigger" in map.fullpath: # device
                self.__attr_list[map.index].lbl.setText(_fromUtf8(map.name))

    def setValues(self, mask, value):
        length = len(value)
        for i in range(0, self.length):
            self.setDisabled(i, True)
            self.setValue(i, "")
            if mask & (1 << i) and i < length:
                self.setDisabled(i, False)
                self.setValue(i, value[i])


    def getValues(self, mask):
        values = []
        for i in range(0, self.length):
            if mask & (1 << i):
                val= self.getValue(i)
            else:
                val = 0
            values.append(val)
        return values

    def setDisabledMask(self, mask):
        for i in range(0, self.length):
            if mask & (1 << i):
                self.setDisabled(i, False)
            else:
                self.setDisabled(i, True)

    def setDisabled(self, index, status):
        self.__attr_list[index].edt.setDisabled(status)


    def setValue(self, index, str_val):
        self.__attr_list[index].edt.setText(str(str_val))


    def getValue(self, index):
        try:
            return int(self.__attr_list[index].edt.text())
        except:
            return 0


    def getLength(self):
        return self.length


    def __del__(self):
        del self.__attr_list[:]


class ZioAttributeGUI(object):
    """This class creates a GUI for an attribute. It creates a QLabel and a
    QLineEdit at a given position (x,y) in a parent widget"""
    def __init__(self, parent, name, index):
        # Create the label
        self.lbl = QtGui.QLabel(parent)
        self.lbl.setText(_fromUtf8(name))
        self.lbl.setObjectName(_fromUtf8(name + " label"))
        self.lbl.show()

        # Create the edit field
        self.edt = QtGui.QLineEdit(parent)
        self.edt.setObjectName(_fromUtf8(name + " edit"))
        self.edt.show()

    def __del__(self):
        # destroy label and edit field
        self.lbl.setParent(None)
        self.edt.setParent(None)


class ZioControlHandlerUI(object):
    """
    This class handles the GUI interface.
    """

    def __init__(self, ui):
        """
        Initialize the GUI. Mainly, it connects GUI events to specific
        function of ZioControlHandlerUI class, and it initializes some
        parameters.
        """
        self.ctrl = ZioCtrl()
        self.ui = ui
        self.__attr_map_list = []

        # Handle object events
        self.ui.btn_get.clicked.connect(self.get_control)
        self.ui.btn_set.clicked.connect(self.set_control)
        self.ui.edt_trg_post.textChanged.connect(self.update_nsample)
        self.ui.edt_trg_pre.textChanged.connect(self.update_nsample)
        self.ui.edt_dev_nbit.textChanged.connect(self.update_nbit)
        self.ui.ckb_lock_field.stateChanged.connect(self.lock_fields)
        self.ui.edt_dev_stdmask.editingFinished.connect(self.__dev_std_mask_edited)
        self.ui.edt_dev_extmask.editingFinished.connect(self.__dev_ext_mask_edited)
        self.ui.edt_trg_stdmask.editingFinished.connect(self.__trg_std_mask_edited)
        self.ui.edt_trg_extmask.editingFinished.connect(self.__trg_ext_mask_edited)

        # Create a list of the device standard attributes
        self.dev_std_attr = [self.ui.edt_dev_nbit, \
                             self.ui.edt_dev_gain, \
                             self.ui.edt_dev_offset, \
                             self.ui.edt_dev_rate, \
                             self.ui.edt_dev_vref,\
                             self.ui.edt_dev_version]

        # Create a list of the trigger standard attributes
        self.trg_std_attr = [self.ui.edt_trg_nshot, \
                             self.ui.edt_trg_post, \
                             self.ui.edt_trg_pre, \
                             self.ui.edt_trg_version]

        # Initialize extended attributes list
        self.dev_ext_attr = ZioExtendedAttribute(self.ui.scrl_dev_ext_content,\
                                                 32)
        self.trg_ext_attr = ZioExtendedAttribute(self.ui.scrl_trg_ext_content,\
                                                 32)


        # Initialize trigger list
        self.__fill_combo_available_trigger(self.ui.cmb_trg_name, "user")


    def __std_mask_edited(self, attrs_gui, mask):
        print("[zio-control] Enable/Disable standard attributes (" + str(mask) + ")")
        i = 0
        for attr in attrs_gui:
            if mask & (1 << i):
                attr.setDisabled(False)
            else:
                attr.setDisabled(True)
            i = i + 1

    def __dev_std_mask_edited(self):
        self.__std_mask_edited(self.dev_std_attr, int(str(self.ui.edt_dev_stdmask.text()), 16))

    def __trg_std_mask_edited(self):
        self.__std_mask_edited(self.trg_std_attr, int(str(self.ui.edt_trg_stdmask.text()), 16))


    def __ext_mask_edited(self, attrs_gui, mask):
        print("[zio-control] Enable/Disable extended attributes (" + str(mask) + ")")
        attrs_gui.setDisabledMask(mask)

    def __dev_ext_mask_edited(self):
        self.__ext_mask_edited(self.dev_ext_attr, int(str(self.ui.edt_dev_extmask.text()), 16))

    def __trg_ext_mask_edited(self):
        self.__ext_mask_edited(self.trg_ext_attr, int(str(self.ui.edt_trg_extmask.text()), 16))


    def lock_fields(self):
        """
        Lock or un-lock fields that usually do not change.
        """
        state = self.ui.ckb_lock_field.isChecked()
        if state:
            print("[zio-control] Lock fields")
        else:
            print("[zio-control] Unlock fields");
        self.ui.edt_vm1.setDisabled(state)          # Version Major
        self.ui.edt_vm2.setDisabled(state)          # Version Minor
        self.ui.edt_nbits.setDisabled(state)        # N bit
        self.ui.edt_nsample.setDisabled(state)      # Number of samples
        self.ui.edt_hosttype.setDisabled(state)     # Host Type
        self.ui.edt_hostid.setDisabled(state)       # Host ID
        self.ui.edt_devid.setDisabled(state)        # Device ID
        self.ui.edt_cset.setDisabled(state)         # Channel set number
        self.ui.edt_chan.setDisabled(state)         # Channel number
        self.ui.edt_devname.setDisabled(state)      # Device name

        # Tabs
        self.ui.edt_dev_name.setDisabled(state)     # Device name
        self.ui.edt_dev_stdmask.setDisabled(state)  # Device standard mask
        self.ui.edt_dev_extmask.setDisabled(state)  # Device extended mask


    def __fill_combo_available_trigger(self, combo, selection):
        combo.clear()
        triggers = []
        with open("/sys/bus/zio/available_triggers", "r") as f:
            for line in f:
                trg_name = line.rstrip('\n')
                trg_name = trg_name + ('\x00' * (12 - len(trg_name)))
                triggers.append(trg_name)

        combo.addItems(triggers)
        combo.setCurrentIndex(combo.findText(selection))

    def __update_extended_attributes(self, attrs_gui, attr_set):
        """
        This function is part of the update_form() process. It updates
        the extended attribute according to control values
        """
        attrs_gui.setValues(attr_set.ext_mask, attr_set.ext_val)
        attrs_gui.updateLabels(self.__attr_map_list)

    def __update_standard_attributes(self, attrs_gui, attr_set):
        """
        This function is part of the update_form() process. It updates
        the standard attribute according to control values
        """
        i = 0
        for attr in attrs_gui:
            if attr_set.std_mask & (1 << i):
                attr.setDisabled(False)
                attr.setText(str(attr_set.std_val[i]))
            else:
                attr.setDisabled(True)
                attr.setText("")
            i = i + 1

        if attr_set.std_mask & (1 << 15):
            attrs_gui[-1].setText("{0:#x}".format(attr_set.std_val[15]))
            attr.setDisabled(False)
        else:
            attrs_gui[-1].setText("")
            attr.setDisabled(True)

    def update_nsample(self, text):
        """
        The nsample field must be syncronized with the pre-sample and post-sample
        trigger's attributes. Each time one of these attributes change, this
        function update the value of nsample with the sum of pre-sample and
        post-sample
        """
        nsample = 0

        post = str(self.ui.edt_trg_post.text())
        if post.isdigit():
            nsample += int(self.ui.edt_trg_post.text())

        pre = str(self.ui.edt_trg_pre.text())
        if pre.isdigit():
            nsample += int(self.ui.edt_trg_pre.text())

        self.ui.edt_nsample.setText(str(nsample))

    def update_nbit(self, text):
        """
        The nbits field must be syncronized with the nbit device's attribute. Each
        time the user updates the nbit attribute, this function update the nbit
        field with the same value
        """
        if str(text).isdigit():
            self.ui.edt_nbits.setText(str(text))
        else:
            self.ui.edt_nbits.setText("")

    def update_form(self):
        """
        This function update the GUI with the new information from control
        """
        # General information
        self.ui.edt_vm1.setText(str(self.ctrl.major_version))
        self.ui.edt_vm2.setText(str(self.ctrl.minor_version))
        self.ui.edt_nseq.setText(str(self.ctrl.seq_num))
        self.ui.edt_ssize.setText(str(self.ctrl.ssize))
        self.ui.edt_nbits.setText(str(self.ctrl.nbits))
        self.ui.edt_nsample.setText(str(self.ctrl.nsamples))
        self.ui.edt_alarm_zio.setText("{0:#x}".format(self.ctrl.alarms_zio))
        self.ui.edt_alarm_dev.setText("{0:#x}".format(self.ctrl.alarms_dev))
        self.ui.edt_flags.setText("{0:#x}".format(self.ctrl.flags))
        # Address - Read Only
        self.ui.edt_hosttype.setText(str(self.ctrl.addr.host_type))
        self.ui.edt_hostid.setText(str(self.ctrl.addr.hostid))
        self.ui.edt_devid.setText(str(self.ctrl.addr.dev_id))
        self.ui.edt_cset.setText(str(self.ctrl.addr.cset_i))
        self.ui.edt_chan.setText(str(self.ctrl.addr.chan_i))
        self.ui.edt_devname.setText(self.ctrl.addr.devname)
        # Time stamp
        self.ui.edt_sec.setText(str(self.ctrl.tstamp.seconds))
        self.ui.edt_ticks.setText(str(self.ctrl.tstamp.ticks))
        self.ui.edt_bins.setText(str(self.ctrl.tstamp.bins))
        # Device
        self.ui.edt_dev_name.setText(str(self.ctrl.addr.devname))
        self.ui.edt_dev_stdmask.setText("{0:#x}".format(self.ctrl.attr_channel.std_mask))
        self.ui.edt_dev_extmask.setText("{0:#x}".format(self.ctrl.attr_channel.ext_mask))

        self.__update_standard_attributes(self.dev_std_attr, \
                                          self.ctrl.attr_channel)
        self.__update_extended_attributes(self.dev_ext_attr, \
                                          self.ctrl.attr_channel)

        # Trigger
        self.__fill_combo_available_trigger(self.ui.cmb_trg_name, \
                                            str(self.ctrl.triggername))
        self.ui.edt_trg_stdmask.setText("{0:#x}".format(self.ctrl.attr_trigger.std_mask))
        self.ui.edt_trg_extmask.setText("{0:#x}".format(self.ctrl.attr_trigger.ext_mask))

        self.__update_standard_attributes(self.trg_std_attr, \
                                          self.ctrl.attr_trigger)
        self.__update_extended_attributes(self.trg_ext_attr, \
                                          self.ctrl.attr_trigger)


    def update_ctrl(self):
        """
        Update the control structure with the value in the form
        """
        # Save value into control
        self.ctrl.major_version = int(self.ui.edt_vm1.text())
        self.ctrl.minor_version = int(self.ui.edt_vm2.text())
        self.ctrl.seq_num = int(self.ui.edt_nseq.text())
        self.ctrl.ssize = int(self.ui.edt_ssize.text())
        self.ctrl.nbits = int(self.ui.edt_nbits.text())
        self.ctrl.nsamples = int(self.ui.edt_nsample.text())
        self.ctrl.alarms_zio = int(str(self.ui.edt_alarm_zio.text()), 16)
        self.ctrl.alarms_dev = int(str(self.ui.edt_alarm_dev.text()), 16)
        self.ctrl.flags = int(str(self.ui.edt_flags.text()), 16)

        #address
        self.ctrl.addr.host_type = int(self.ui.edt_hosttype.text())

        #self.ctrl.addr.hostid = int(self.ui.edt_hostid.text())
        self.ctrl.addr.dev_id = int(self.ui.edt_devid.text())
        self.ctrl.addr.cset_i = int(self.ui.edt_cset.text())
        self.ctrl.addr.chan_i = int(self.ui.edt_chan.text())
        self.ctrl.addr.devname = str(self.ui.edt_devname.text())

        #time stamp
        self.ctrl.tstamp.seconds = int(self.ui.edt_sec.text())
        self.ctrl.tstamp.ticks = int(self.ui.edt_ticks.text())
        self.ctrl.tstamp.bins = int(self.ui.edt_bins.text())

        # Device
        self.ctrl.attr_channel.std_mask = int(str(self.ui.edt_dev_stdmask.text()), 16)
        self.ctrl.attr_channel.ext_mask = int(str(self.ui.edt_dev_extmask.text()), 16)

        i = 0
        for attr in self.dev_std_attr:
            if self.ctrl.attr_channel.std_mask & (1 << i):
                self.ctrl.attr_channel.std_val[i] = int(attr.text())
            i = i + 1

        self.ctrl.attr_channel.ext_val = self.dev_ext_attr.getValues(self.ctrl.attr_channel.ext_mask)

        # Trigger
        self.ctrl.triggername = str(self.ui.cmb_trg_name.currentText())
        self.ctrl.attr_trigger.std_mask = int(str(self.ui.edt_trg_stdmask.text()), 16)
        self.ctrl.attr_trigger.ext_mask = int(str(self.ui.edt_trg_extmask.text()), 16)

        i = 0
        for attr in self.trg_std_attr:
            if self.ctrl.attr_trigger.std_mask & (1 << i):
                self.ctrl.attr_trigger.std_val[i] = int(attr.text())
            i = i + 1

        self.ctrl.attr_trigger.ext_val = self.trg_ext_attr.getValues(self.ctrl.attr_trigger.ext_mask)

    def get_control_descrition(self, path):
        with open(path + "/control-description", 'r') as fdcd:
            attr_map = fdcd.readlines();
            del attr_map[0]
            for map in attr_map:
                self.__attr_map_list.append(ZioAttrMap(map))

    def get_control(self):
        """
        Update the form with the value in the control structure
        """
        sys_dev_path = None
        path = str(self.ui.edt_file.text())
        tmp = path.split('/')
        
        if tmp[1] != "sys" and tmp[1] != "dev":
            print("Invalid control path")
            return

        # look in  both /dev and /sys path for device name
        m = re.search(r'[\w\-]+-\d{4}', path)
        if m != None:
            sys_dev_path = "/sys/bus/zio/devices/" + m.group(0)
            print("trovato " + sys_dev_path)
            self.get_control_descrition(sys_dev_path)

        self.read_ctrl(path)
        self.update_form()


    def set_control(self):
        """
        Update the device with the current control writed on the GUI
        """
        self.update_ctrl()
        path = self.ui.edt_file.text()
        self.write_ctrl(path)
        # get again the control
        self.get_control()


    def write_ctrl(self, path):
        """
        It writes the control structure to a file
        """
        with open(path, 'w') as fdw:
            try:
                fdw.write(self.ctrl.pack_to_bin())
            except IOError as err:
                print("I/O error({0}): {1}".format(err.errno, err.strerror))
            except:
                print("Unexpected error:")
                raise

    def read_ctrl(self, path):
        """
        It reads the control structure from a file
        """
        with open(path, "r") as fdr:
            try:
                data = fdr.read(512)
            except IOError as err:
                print("I/O error({0}): {1}".format(err.errno, err.strerror))
            except:
                print("Unexpected error:")
                raise
        self.ctrl.unpack_to_ctrl(data)
