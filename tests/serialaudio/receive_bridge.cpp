#define private public
#include "serialman.h"
#undef private
#include "serialaudio.h"
#include "SI5351.hpp"
#include "nd.h"
#include "disp.h"
#include "okim6258.h"
#include "vgm.h"
extern size_t cursor;
extern unsigned assertionFailures;
SerialStub Serial;
uint32_t fakeMillis;
ClockStub SI5351;
PlayerStub playerWindow;
OkiStub okim6258;
VgmStub vgm;
bool ND::canPlay, ND::isPaused;
FileFormat ND::fileFormat;
FreqArray ND::freq;
NamesArray ND::chipNames;
uint8_t ND::clockSlot[15],ND::chipSlot[15];
extern "C" {
__declspec(dllexport) void receiveInit() {
 cursor=0;fakeMillis=0;assertionFailures=0;
 Serial.head=Serial.tail=0;Serial.plugged=Serial.connected=true;
 for(unsigned i=0;i<15;++i){ND::clockSlot[i]=3;ND::chipSlot[i]=0;}
 ND::clockSlot[5]=0;ND::clockSlot[14]=1;ND::chipSlot[14]=1;
 serialMan.status={};serialMan.receiveHead=serialMan.receiveTail=0;
 serialMan.protocolResetPending=false;serialMan.transportLossPending=0;serialMan.usbDiscardRemaining=0;
 serialMan.init();
}
__declspec(dllexport) void queueBytes(const uint8_t* p,unsigned n) {Serial.append(p,n);}
__declspec(dllexport) void sof(unsigned plugged) {Serial.plugged=plugged!=0;}
__declspec(dllexport) void receivePass(unsigned process) {++fakeMillis;serialMan.receive();if(process)serialMan.processCommands();}
__declspec(dllexport) void busReset() {Serial.busReset(nullptr,nullptr,0,nullptr);}
__declspec(dllexport) void discard() {serialMan.clearReceiveBuffer();serialMan.processCommands();}
__declspec(dllexport) unsigned receiveValue(unsigned i) {
 const auto s=serialMan.receiveStatus();
 switch(i){case 0:return unsigned(s.bufferedBytes);case 1:return s.overflows;
 case 2:return unsigned(s.discardedBytes);case 3:return s.busResets;
 case 4:return s.connections;default:return s.disconnections;}
}
}
