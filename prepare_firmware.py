"""
Prepare the firmware image for publishing

- Read Firmware version from code
- Generate firmware.version file
- Rename Firmware image
"""

import os
import re
import time

with open("WiFi_Shield.ino", 'r') as f:
    code = f.read()

match = re.search(r"FW_VERSION = (\d+);", code)
fw_version = match.group(1)

with open("firmware.version", 'w') as f:
    f.write(fw_version)

if os.path.exists("WiFi_Shield.ino.generic.bin"):
    if os.path.exists("firmware.bin"):
        os.remove("firmware.bin")
    os.rename("WiFi_Shield.ino.generic.bin", "firmware.bin")