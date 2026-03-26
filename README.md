This project is for a WiFi enabled SD card - it was designed to allow the transfer of 3D print files from a PC to an SD card fitted 
to a ESP32 S3 DevkitC-1 controller board (the board is connected and powered by the 3D Printer (Anycubic Photon D2) via a 
USBC to USBA data cable.
**The code was written using IDE ver 2.3.8.**
**ESP32 Core version 3.3.7 (by Espressif Systems)**
**The SD Card should be formatted to FAT32 (or can be FAT if below 4Gb)**
Wifi Credentials can be hard coded (within the first tab in the IDE) or can be set using a Wifi.ini file in a folder
called WIFI in the root of the SD Card. - The code will first check this file and if found will use these 
credentials - if not found then it will use whatever is hard coded in the main tab.

Lines 307-323 in Tab 1 contain the SD card connections and configuration - uses 1 Bit resolution for stability
Change lines 309 & 321 to use 4 bit if your SD card can support it for additional SD Card read/write speed 


You may modify the code to suit your needs.
