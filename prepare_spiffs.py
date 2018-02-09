"""
Prepare the SPIFFS image for publishing

- Set version number in config.json
- Generate spiffs.version file
- Build SPIFFS image
"""

import json
import os
import time

with open("data/config.json", 'r') as f:
    data = json.load(f)

data['SPVersion'] = int(time.strftime("%y%m%d%H%M"))

with open("data/config.json", 'w') as f:
    json.dump(data, f)

with open("spiffs.version", 'w') as f:
    f.write(str(data['SPVersion']))

os.system("mkspiffs -c data -p 256 -b 8192 -s 1028096 spiffs.bin")