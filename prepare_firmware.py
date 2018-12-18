"""
Prepare the firmware image for publishing

- Read Firmware version from code
- Generate firmware.version file
- Rename Firmware image
"""

import os
import re
import time

with open("Caracal_Firmware.ino", 'r') as f:
    code = f.read()
    
match = re.search(r"#define HW_GROUP (\d+)", code)
hw_version = match.group(1)

match = re.search(r"FW_VERSION = (\d+);", code)
fw_version = match.group(1)

fname = "firmware{}.bin".format(hw_version)
if os.path.exists("Caracal_Firmware.ino.generic.bin"):
    if os.path.exists(fname):
        os.remove(fname)
    os.rename("Caracal_Firmware.ino.generic.bin", fname)

    with open("firmware{}.version".format(hw_version), 'w') as f:
        f.write(fw_version)