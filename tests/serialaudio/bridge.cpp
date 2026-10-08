#include "serialaudio.h"
#include "fm.h"
#include "NJU72342.h"
#include <stddef.h>
unsigned assertionFailures, dataCount, ymCount, lastDivider, playCount, panValue, clockValue;
bool panMuted;
uint8_t played[131072];
FMChip FM;
NJU nju72342;
void (*supplyISR)(void*);
__declspec(align(16)) unsigned char heap[32768];
size_t cursor;
void* heap_caps_malloc(size_t n,int) { size_t start=cursor;cursor=(cursor+n+15)&~size_t(15);return cursor<=sizeof(heap)?heap+start:nullptr; }
void clockChange(uint32_t hz) { clockValue=hz; }
extern "C" {
__declspec(dllexport) void init() { cursor=0; SerialAudio::init(clockChange);SerialAudio::reset();assertionFailures=dataCount=ymCount=lastDivider=playCount=panValue=clockValue=0; }
__declspec(dllexport) void command(unsigned op,const uint8_t* p,unsigned n) { SerialAudio::command(uint8_t(op),p,n,nullptr); }
__declspec(dllexport) void status(uint8_t* p) { SerialAudio::command(0x5b,nullptr,0,p); }
__declspec(dllexport) void tick(unsigned n) { while(n--)supplyISR(nullptr); }
__declspec(dllexport) void service() { SerialAudio::service(); }
__declspec(dllexport) void reset() { SerialAudio::reset(); }
__declspec(dllexport) void lost() { SerialAudio::transportLost(); }
__declspec(dllexport) unsigned output(unsigned i) { return played[i]; }
__declspec(dllexport) unsigned hardware(unsigned i) { switch(i) {case 0:return dataCount;case 1:return ymCount;case 2:return lastDivider;case 3:return playCount;case 4:return panValue;case 5:return panMuted;case 6:return clockValue;default:return assertionFailures;} }
}
