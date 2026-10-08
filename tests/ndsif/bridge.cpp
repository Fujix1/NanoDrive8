#include "ndsif.h"
ndsif::Protocol protocol;
unsigned audioCalls, audioOpcode, audioSize;
unsigned resets, writes, premature, clocks, lastChip, lastHz;
unsigned char pairs[256];
void reset() { if (protocol.outputSize()) ++premature; ++resets; }
void write(unsigned char a, unsigned char v) { if (protocol.outputSize()) ++premature; if (writes < 128) { pairs[2*writes]=a; pairs[2*writes+1]=v; } ++writes; }
void clock(unsigned char chip, unsigned hz) { if (protocol.outputSize()) ++premature; ++clocks; lastChip=chip; lastHz=hz; }
size_t audio(uint8_t opcode, const uint8_t*, size_t size, uint8_t* response) {
 ++audioCalls; audioOpcode=opcode; audioSize=(unsigned)size;
 if(opcode==0x5b) { for(unsigned i=0;i<40;++i)response[i]=(uint8_t)i;return 40; }
 return 0;
}
extern "C" {
__declspec(dllexport) unsigned audioValue(unsigned kind) { return kind==0?audioCalls:kind==1?audioOpcode:audioSize; }
__declspec(dllexport) void init() { protocol.configure(reset, write, "1.0b8", clock, audio); audioCalls=audioOpcode=audioSize=0; resets=writes=premature=clocks=lastChip=lastHz=0; }
__declspec(dllexport) void feed(unsigned char b, unsigned t) { protocol.feed(b,t); }
__declspec(dllexport) void expire(unsigned t) { protocol.expire(t); }
__declspec(dllexport) void clear(int resync) { protocol.clear(resync != 0); }
__declspec(dllexport) unsigned output(unsigned char* p, unsigned n) { unsigned size=(unsigned)protocol.outputSize(); if(n>size)n=size; for(unsigned i=0;i<n;++i)p[i]=protocol.output()[i]; protocol.consumeOutput(n); return n; }
__declspec(dllexport) unsigned count(unsigned kind) { return kind==0?resets:kind==1?writes:premature; }
__declspec(dllexport) unsigned clockValue(unsigned kind) { return kind==0?clocks:kind==1?lastChip:lastHz; }
__declspec(dllexport) unsigned pair(unsigned i) { return pairs[i]; }
}
