#pragma once
enum tPan {PAN_CENTER=0,PAN_LEFT=1,PAN_RIGHT=2,PAN_MUTE=3};
extern bool panMuted;
extern unsigned panValue;
struct NJU { void mute(){} void unmute(){} void setVolumeAll(int){} void setMainAttenuation(unsigned){} void panMute(){panMuted=true;} void panUnmute(){panMuted=false;}
 void panSetPan(tPan p){panValue=p;} };
extern NJU nju72342;
