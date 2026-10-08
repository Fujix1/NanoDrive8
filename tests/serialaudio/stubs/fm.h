#pragma once
#include <stdint.h>
#define MCK 17
enum tOKIM6258Divider {OKIM6258_DIV_512=512,OKIM6258_DIV_768=768,OKIM6258_DIV_1024=1024};
extern unsigned dataCount, ymCount, lastDivider, playCount, assertionFailures;
extern uint8_t played[131072];
struct FMChip {
 void reset() {}
 void applyPendingChannelMask() {}
 void setOKIM6258dataISR(uint8_t b,int) { if(dataCount<131072)played[dataCount]=b;++dataCount; }
 void setRegisterOPM(uint8_t,uint8_t,int) { ++ymCount; }
 void setOKIM6258divider(tOKIM6258Divider d) { lastDivider=d; }
 void setOKIM6258command(uint8_t v,int) { if(v==2)++playCount;else if(v!=1)++assertionFailures; }
};
extern FMChip FM;
