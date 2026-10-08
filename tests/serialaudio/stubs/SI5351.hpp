#pragma once
#include <stdint.h>
using si5351Freq_t = uint32_t;
constexpr si5351Freq_t SI5351_4000=4000000,SI5351_8000=8000000,SI5351_UNDEFINED=0;
struct ClockStub { void setFreq(uint32_t,int){} };
extern ClockStub SI5351;
