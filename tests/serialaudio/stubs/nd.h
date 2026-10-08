#pragma once
#include "SI5351.hpp"
constexpr char ND_FIRMWARE_VERSION[] = "test";
enum t_chip {CHIP_YM2151=5,CHIP_OKIM6258=14};
enum class FileFormat {Unknown};
struct FreqArray { uint32_t data[3]; unsigned size()const{return 3;} uint32_t& operator[](unsigned i){return data[i];} };
struct NamesArray { uint32_t data[2]; unsigned size()const{return 2;} uint32_t& operator[](unsigned i){return data[i];} };
struct ND {
 static bool canPlay,isPaused;
 static FileFormat fileFormat;
 static FreqArray freq;
 static NamesArray chipNames;
 static uint8_t clockSlot[15],chipSlot[15];
 static uint32_t formatChipName(uint32_t f,t_chip){return f;}
};
