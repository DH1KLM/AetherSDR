#include "TestSettingsProfile.h"
#include "core/backends/anan/AnanBackend.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <cstdlib>
#include <iostream>
using namespace AetherSDR;using namespace AetherSDR::anan;
namespace AetherSDR::anan {
struct AnanMultiRxTestAccess {
 static void prepare(AnanBackend& b){b.m_streamApplyOverride=[](const auto&,const auto&){return true;};b.m_connected=true;b.m_discoveryInfoReceived=true;b.m_discoveredBoardId=4;b.m_discoveredReceiverCount=7;
 b.m_pendingParams.expectedBoardId=4;b.m_pendingParams.model=AnanModel::Anan200D;b.m_sliceFreqHz=b.m_ddcCenterHz=14'175'000;b.m_pendingParams.ddc0RateKsps=48;b.m_pendingDspConfig.inputSampleRateHz=48000;b.m_pendingDspConfig.blockForOutput=true;b.m_receiveLayout.reset(5,b.m_sliceFreqHz,48);emit b.connected();}
 static bool busy(const AnanBackend& b){return b.m_receiveBuilding;}static int stream(const AnanBackend& b,int id){const auto* s=b.m_receiveLayout.slice(id);return s?s->stream:-1;}
 static double center(const AnanBackend& b,int st){return b.m_receiveLayout.streams()[st].centerHz;}static int rate(const AnanBackend& b,int st){return b.m_receiveLayout.streams()[st].rateKsps;}
 static bool empty(const AnanBackend& b){return b.m_additionalRx.empty();}
 static void feed(AnanBackend& b,int st){std::vector<std::complex<float>> iq(1024);for(int i=0;i<1024;++i)iq[i]=std::polar(0.01f,float(2*3.141592653589793*1000*i/48000));
 QMetaObject::invokeMethod(b.m_client,[c=b.m_client,st,iq]{for(int i=0;i<25;++i)emit c->ddcIqReady(st,iq);},Qt::QueuedConnection);}
 static void barrier(AnanBackend& b){QMetaObject::invokeMethod(b.m_client,[]{},Qt::BlockingQueuedConnection);}
};}
void check(bool ok,const char* why){if(!ok){std::cerr<<"FAIL: "<<why<<'\n';std::exit(1);}}
template<class F>void wait(F done){QElapsedTimer t;t.start();while(!done()&&t.elapsed()<90000){QCoreApplication::processEvents();QThread::msleep(1);}check(done(),"bounded DSP completion");}
int main(int argc,char**argv){TestSettingsProfile profile(QStringLiteral("anan-multi-rx"));QCoreApplication app(argc,argv);AnanBackend b;AnanMultiRxTestAccess::prepare(b);
 std::map<int,SliceDelta> state;std::map<int,int> audio;std::vector<QString> removed;
 QObject::connect(&b,&IRadioBackend::sliceChanged,[&](int id,const SliceDelta& d){state[id]=d;});QObject::connect(&b,&IRadioBackend::sliceAudioFrameReady,[&](int id,const PcmFrame& pcm){audio[id]+=pcm.legacyStereo24().size();});QObject::connect(&b,&IRadioBackend::panRemoved,[&](const QString& p){removed.push_back(p);});
 check(b.capabilities().maxSlices==5&&b.capabilities().maxPanadapters==5,"Orion limits expose software slices and hardware streams");
 check(b.createSlice(QStringLiteral("anan-0"),14'178'000),"create shared slice");wait([&]{return !AnanMultiRxTestAccess::busy(b);});
 check(AnanMultiRxTestAccess::stream(b,1)==0&&state.contains(1),"shared slice has its own observed identity");AnanMultiRxTestAccess::feed(b,0);AnanMultiRxTestAccess::barrier(b);QCoreApplication::processEvents();check(audio[1]>0,"actual additional DSP produces per-slice audio");
 b.setSliceMode(1,QStringLiteral("LSB"));b.setSliceFilter(1,-2700,-200);b.setSliceAudioGain(1,35);check(state[1].mode==QStringLiteral("LSB")&&state[1].filterLow==-2700&&state[1].audioGain==35,"extra slice controls are independent");
 b.setSliceFrequency(1,7'100'000);check(AnanMultiRxTestAccess::stream(b,1)==1&&AnanMultiRxTestAccess::center(b,0)==14'175'000,"leaving shared capture preserves the other window");
 check(b.createPanadapter(),"create independent pan");wait([&]{return !AnanMultiRxTestAccess::busy(b);});check(AnanMultiRxTestAccess::stream(b,2)==2,"explicit pan always owns a DDC");
 b.setPanBandwidth(QStringLiteral("anan-1"),96000);wait([&]{return !AnanMultiRxTestAccess::busy(b);});check(AnanMultiRxTestAccess::rate(b,1)==96&&AnanMultiRxTestAccess::rate(b,0)==48,"P2 rates change per capture");
 check(b.removeSlice(1)&&!removed.empty(),"last slice retires its independent window");const int oldAudio=audio[1];AnanMultiRxTestAccess::feed(b,2);AnanMultiRxTestAccess::barrier(b);QCoreApplication::processEvents();check(audio[2]>0&&audio[1]==oldAudio,"retired DSP cannot publish queued audio");
 check(b.removeSlice(0)&&!b.removeSlice(2),"original slice may close; final slice remains protected");
 check(b.createSlice(QStringLiteral("anan-2"),14'176'000),"accept another asynchronous build");b.disconnectRadio();QCoreApplication::processEvents();check(AnanMultiRxTestAccess::empty(b),"disconnect retires pending and live receiver objects");
 std::cout<<"ANAN multi-receive DSP/lifecycle/rate checks passed\n";
}
