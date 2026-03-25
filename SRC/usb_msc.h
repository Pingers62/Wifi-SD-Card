#pragma once
#include <Arduino.h>
#include <USB.h>
#include <USBMSC.h>

#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

extern sdmmc_card_t* sdcard;   // From main.cpp

static USBMSC msc;
static uint8_t sectorBuf[512] __attribute__((aligned(4)));

// ---------------- READ CALLBACK ----------------
int32_t my_msc_read_cb(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  if (!sdcard) return -1;

  // Read one 512-byte sector
  esp_err_t err = sdmmc_read_sectors(sdcard, sectorBuf, lba, 1);
  if (err != ESP_OK) {
    return -1;
  }

  if (offset + bufsize > 512) {
    return -1;
  }

  memcpy(buffer, sectorBuf + offset, bufsize);
  return (int32_t)bufsize;
}

// ---------------- WRITE CALLBACK ----------------
int32_t my_msc_write_cb(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  // Read-only, same as before
  return -1;
}

// ---------------- INITIALISE ----------------
void usbmsc_init() {
  msc.vendorID("ESP32-S3");
  msc.productID("3D-Printer");
  msc.productRevision("1.0");

  msc.onRead(my_msc_read_cb);
  msc.onWrite(my_msc_write_cb);

  msc.mediaPresent(false);
  USB.begin();
}

// ---------------- MODE SWITCH ----------------
void usb_msc_mediaPresent(bool present) {
  if (present) {
    // --- SWITCHING TO USB ---
    if (sdcard) {
      // Capacity in sectors (512-byte blocks)
      uint32_t block_count = sdcard->csd.capacity;  // already in 512-byte sectors

      msc.mediaPresent(false);
      delay(500);

      if (msc.begin(block_count, 512)) {
        msc.mediaPresent(true);
        Serial.println("USB Mode Active");
      }
    }
  } else {
    // --- SWITCHING TO WIFI ---
    Serial.println("Shutting down USB...");

    msc.mediaPresent(false);
    delay(500);

    msc.end();
    Serial.println("USB Service Stopped");

    delay(100);
  }
}
