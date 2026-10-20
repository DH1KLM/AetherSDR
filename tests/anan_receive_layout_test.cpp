#include "core/backends/anan/AnanReceiveLayout.h"
#include "core/backends/anan/P1Protocol.h"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace AetherSDR::anan;
void check(bool ok,const char* why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
int main(){
 AnanReceiveLayout l;l.reset(5,14'175'000,48);const int second=*l.add(14'178'000,0);
 check(l.occupants(0)==2,"same-span slice shares capture");check(l.tune(0,14'180'000,false)&&l.streams()[0].centerHz==14'175'000,"shared capture cannot follow one VFO");
 check(l.tune(second,7'100'000,false)&&l.slice(second)->stream==1,"out-of-span slice claims independent DDC");check(l.streams()[0].centerHz==14'175'000,"other capture stays in place");
 const int third=*l.add(14'180'000,-1,true);check(l.slice(third)->stream==2,"new pan costs a DDC even at the same frequency");
 check(l.add(14'181'000,0).has_value()&&l.add(14'182'000,0).has_value()&&!l.add(14'183'000,0),"five demodulator limit applies even when sharing");
 check(l.remove(third)&&!l.streams()[2].active,"last occupant frees its DDC");
 l.reset(1,14'175'000,48);const int s=*l.add(14'180'000,0);check(!l.tune(s,7'100'000,false)&&l.slice(s)->frequencyHz==14'180'000,"exhaustion refuses without changing VFO");
 check(!l.add(14'175'000,-1,true),"no spare DDC never creates coupled pan");check(l.remove(s)&&!l.remove(0),"last slice remains protected");
 check(l.tune(0,14'182'000,true)&&l.streams()[0].centerHz==14'175'000,"CTUN holds in-window capture");
 check(l.tune(0,7'100'000,true)&&l.streams()[0].centerHz==7'100'000,"sole CTUN receiver recenters outside span");
 check(!l.add(std::numeric_limits<double>::quiet_NaN()),"NaN refused");
 p1::ReceiveConfig c;c.receivers=5;c.independentFrequencies=true;c.receiveFrequencies={14'175'000,7'100'000,21'250'000,28'500'000,50'150'000,0,0};
 for(int i=0;i<5;++i){const auto row=p1::receiveControl(4+2*i,c);check(bool(row),"P1 RX row exists");std::uint32_t hz=0;for(int j=1;j<5;++j)hz=(hz<<8)|(*row)[j];check(hz==c.receiveFrequencies[i]&&((*row)[0]&1)==0,"independent P1 tuning is correct and never keys TX");}
 c.receiveFrequencies[2]=61'440'001;check(!p1::receiveControl(8,c),"invalid additional tuning fails closed");
 std::cout<<"ANAN receive placement and P1 tuning: all checks passed\n";
}
