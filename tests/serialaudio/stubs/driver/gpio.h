#pragma once
using gpio_num_t = int;
#define GPIO_INTR_POSEDGE 1
extern void (*supplyISR)(void*);
inline int gpio_set_intr_type(int,int) { return 0; }
inline int gpio_isr_handler_add(int,void (*fn)(void*),void*) { supplyISR=fn;return 0; }
