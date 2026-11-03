// Erase and format an SD card (FAT32) on an SPI bus. Shared by every board with a card
// slot (the CYD now, the Marauder V8 later). Unmount the card (SD.end()) first.
// Takes from a few seconds to about a minute depending on the card; it blocks.
#pragma once

#include <Arduino.h>
#include <SPI.h>

namespace ss {

// True when the card was formatted. `msg` gets a short result for the screen / log.
bool sdFormatCard(uint8_t cs, SPIClass& spi, uint32_t hz, char* msg, size_t cap);

}  // namespace ss
