#include "core/backends/anan/AnanBackend.h"
#include "core/backends/WdspNoiseBlanker.h"
#include <QMetaObject>
#include <cmath>

namespace AetherSDR::anan {
namespace {
QString rxPan(int n){return QStringLiteral("anan-%1").arg(n);}
QByteArray rxBytes(const std::vector<float>& v){return {reinterpret_cast<const char*>(v.data()),int(v.size()*sizeof(float))};}
}
int AnanBackend::receiveSliceCeiling()const{const int b=effectiveBoardId();return b==1?4:b==2?2:5;}
int AnanBackend::receiveStreamCeiling()const
{
 const int board=m_discoveryInfoReceived?m_discoveredBoardId:m_pendingParams.expectedBoardId;
 const auto physical=receiveProfile(board),effective=receiveProfile(board,m_pendingParams.model,m_pendingParams.overrideDetection);
 if(!physical||!effective)return 1;
 if(m_pendingParams.protocol==1){const auto p=P1Client::profile(effectiveBoardId(),m_pendingParams.redPitayaP1);return p?std::min(4,p->frameSlotCount):1;}
 int n=std::min({5,physical->physicalDdcLimit-effective->firstDdcIndex,effective->physicalDdcLimit-effective->firstDdcIndex});
 if(m_discoveredReceiverCount>0)n=std::min(n,int(m_discoveredReceiverCount));return std::max(1,n);
}
bool AnanBackend::primaryVisible()const{return m_receiveLayout.slice(0)!=nullptr;}
int AnanBackend::primaryPanIndex()const{const auto* s=m_receiveLayout.slice(0);return s?s->stream:0;}
QString AnanBackend::primaryPanId()const{return rxPan(primaryPanIndex());}
int AnanBackend::streamForPan(const QString& pan)const
{for(int st=0;st<m_receiveLayout.limit();++st)if(m_receiveLayout.streams()[st].active&&rxPan(st)==pan)return st;return -1;}
void AnanBackend::retireEmptyReceiveWindows(const std::array<AnanReceiveLayout::Stream,5>& before)
{for(int st=0;st<5;++st)if(before[st].active&&!m_receiveLayout.streams()[st].active)emit panRemoved(rxPan(st));}
void AnanBackend::publishReceiveWindows()
{
 for(int st=0;st<m_receiveLayout.limit();++st){const auto& w=m_receiveLayout.streams()[st];if(!w.active)continue;
  if(primaryVisible()&&st==primaryPanIndex()){emitPanState();continue;}
  emit panBandwidthLimitsChanged(rxPan(st),0.048,nearestSessionRateKsps(kDdc0RatesKsps.back())/1000.0);
  emit panCenterBandwidthChanged(rxPan(st),w.centerHz/1e6,w.rateKsps/1000.0);
 }
}
void AnanBackend::updateReceiveStreams()
{
 if(!m_connected)return;int count=1;
 for(int st=0;st<m_receiveLayout.limit();++st)if(m_receiveLayout.streams()[st].active)count=st+1;
 std::vector<DdcConfig> configs;std::vector<double> centers;
 for(int st=0;st<count;++st){const auto& w=m_receiveLayout.streams()[st];configs.push_back({w.active?w.rateKsps:m_pendingParams.ddc0RateKsps,m_pendingParams.ddc0AdcIndex});centers.push_back(w.active?w.centerHz:m_ddcCenterHz);}
 std::vector<std::pair<std::shared_ptr<std::atomic<int>>,int>> routes;
 for(const auto& [id,r]:m_additionalRx)if(const auto* s=m_receiveLayout.slice(id))routes.emplace_back(r->route,r->ready?s->stream:-1);
 const int first=primaryVisible()?primaryPanIndex():0;const auto primary=m_primaryRoute;
 const auto epoch=m_receiveEpoch;const auto writer=m_streamApplyOverride;
 QMetaObject::invokeMethod(m_client,[this,configs,centers,routes,first,primary,epoch,writer]{
  if(writer?writer(configs,centers):m_client->setReceiveStreams(configs,centers)){primary->store(first);for(const auto& [r,st]:routes)r->store(st);}
  else QMetaObject::invokeMethod(this,[this,epoch]{if(epoch==m_receiveEpoch&&m_connected){emit connectionError(QStringLiteral("ANAN receive-stream configuration failed"));disconnectRadio();}},Qt::QueuedConnection);
 },Qt::QueuedConnection);
}
bool AnanBackend::createSlice(const QString& pan,double hz){const int st=streamForPan(pan);return st>=0&&addReceiveSlice(hz,st,false);}
bool AnanBackend::createPanadapter(){return addReceiveSlice(m_sliceFreqHz,-1,true);}
bool AnanBackend::addReceiveSlice(double hz,int requested,bool own)
{
 if(!m_connected||m_rateChanging||m_receiveBuilding||m_droopCalibrator.isRunning())return false;
 const auto previous=m_receiveLayout;const auto id=m_receiveLayout.add(hz,requested,own);if(!id)return false;
 const auto* slice=m_receiveLayout.slice(*id);auto r=std::make_shared<AdditionalRx>();r->id=*id;r->mode=m_mode;r->low=m_filterLowHz;r->high=m_filterHighHz;
 r->config=m_pendingDspConfig;r->config.inputSampleRateHz=m_receiveLayout.streams()[slice->stream].rateKsps*1000;
 r->dsp=new AnanRxDsp;r->dsp->moveToThread(m_ioThread);m_additionalRx[*id]=r;m_receiveBuilding=true;
 const auto epoch=m_receiveEpoch;
 connect(m_client,&AnanClient::ddcIqReady,r->dsp,[r](int slot,const std::vector<std::complex<float>>& iq){if(slot==r->route->load()){r->seenIq.store(true);r->dsp->processIqBlock(iq);}},Qt::DirectConnection);
 connect(m_client,&AnanClient::ddcSequenceGap,r->dsp,[r](int slot){if(slot==r->route->load())r->dsp->onSequenceGap();},Qt::DirectConnection);
 connect(r->dsp,&AnanRxDsp::pcmReady,this,[this,r,epoch](const PcmFrame& frame){if(epoch==m_receiveEpoch&&r->ready&&m_additionalRx.contains(r->id))collectReceiveAudio(r->id,frame.legacyStereo24());});
 connect(r->dsp,&AnanRxDsp::spectrumReady,this,[this,r,epoch](const std::vector<float>& bins){
  if(epoch!=m_receiveEpoch||!r->ready||!m_additionalRx.contains(r->id))return;const auto* s=m_receiveLayout.slice(r->id);if(!s)return;
  if(primaryVisible()&&primaryPanIndex()==s->stream)return;
  for(const auto& [id,other]:m_additionalRx){const auto* p=m_receiveLayout.slice(id);if(other->ready&&p&&p->stream==s->stream){if(id!=r->id)return;break;}}
  auto values=bins;for(auto& v:values)v+=float(m_attenuationDb);emit spectrumFrameReady(s->stream,rxBytes(values));
 });
 connect(r->dsp,&AnanRxDsp::meterUpdate,this,[this,r,epoch](float dbfs){if(epoch==m_receiveEpoch&&r->ready&&m_additionalRx.contains(r->id))emit meterUpdate(QStringLiteral("SLC%1:LEVEL").arg(r->id),double(dbfs)+m_attenuationDb);});
 std::vector<QPointer<AnanRxDsp>> running{m_dsp};for(const auto& [sid,other]:m_additionalRx)if(other->ready)running.push_back(other->dsp);
 const auto config=r->config;
 // All IQ processing and routing stays on the existing I/O thread. The existing
 // build thread owns planning; live controls defer while its global lock is held.
 QMetaObject::invokeMethod(m_client,[this,r,epoch,config,previous,running]{
  for(auto dsp:running)if(dsp)dsp->beginRebuild();r->dsp->beginInitialBuild(config);
  QMetaObject::invokeMethod(m_dspBuildContext,[this,r,epoch,config,previous,running]{
   auto result=std::make_shared<AnanRxDsp::RebuildResult>(AnanRxDsp::buildChannel(config));
   QMetaObject::invokeMethod(this,[this,r,epoch,result,previous,running]{
    if(epoch!=m_receiveEpoch||!m_additionalRx.contains(r->id))return;
    for(auto dsp:running)if(dsp)QMetaObject::invokeMethod(dsp,[dsp]{dsp->resumeAfterPlanning();},Qt::QueuedConnection);
    if(!result->channel){m_receiveBuilding=false;m_receiveLayout=previous;m_additionalRx.erase(r->id);r->dsp->deleteLater();emit sliceLifecycleFailed(QStringLiteral("create"),-1,QString::fromStdString(result->error));return;}
    QMetaObject::invokeMethod(r->dsp,[this,r,epoch,result]{
     const bool ok=r->dsp->installRebuiltChannel(std::move(*result));
     QMetaObject::invokeMethod(this,[this,r,epoch,ok]{
      if(epoch!=m_receiveEpoch||!m_additionalRx.contains(r->id))return;m_receiveBuilding=false;
      if(!ok){emit connectionError(QStringLiteral("ANAN additional DSP install failed"));disconnectRadio();return;}
      r->ready=true;applyAdditionalRx(r->id);publishReceiveWindows();publishAdditionalRx(r->id);updateReceiveStreams();emit capabilitiesChanged();
      MeterDef meter;meter.index=r->id+10;meter.source=QStringLiteral("SLC");meter.sourceIndex=r->id;meter.name=QStringLiteral("LEVEL");meter.unit=QStringLiteral("dBm");meter.low=-140;meter.high=20;emit meterDefined(meter);
      if(!m_receiveMixer->isActive())m_receiveMixer->start();
      QTimer::singleShot(5000,this,[this,r,epoch]{if(epoch!=m_receiveEpoch||!m_additionalRx.contains(r->id)||r->seenIq.load())return;if(removeSlice(r->id))emit sliceLifecycleFailed(QStringLiteral("create"),r->id,QStringLiteral("No IQ arrived for the additional ANAN receiver"));});
     },Qt::QueuedConnection);
    },Qt::QueuedConnection);
   },Qt::QueuedConnection);
  },Qt::QueuedConnection);
 },Qt::QueuedConnection);return true;
}
void AnanBackend::applyAdditionalRx(int id)
{
 auto i=m_additionalRx.find(id);const auto* s=m_receiveLayout.slice(id);if(i==m_additionalRx.end()||!s||!i->second->ready)return;const auto r=i->second;
 const double bfo=cwBfoOffsetHz(r->mode,m_cwPitchHz),shift=s->frequencyHz-m_receiveLayout.streams()[s->stream].centerHz-bfo;
 const auto mode=modeFromString(r->mode);const double low=r->low+bfo,high=r->high+bfo,gain=agcKnobDb(r->threshold),fixed=agcKnobDb(r->off);
 const int agc=r->agc,level=r->nbLevel;const auto nb=AetherSDR::toWdsp(r->nb);const auto fill=AetherSDR::toWdsp(r->nbFill);
 QMetaObject::invokeMethod(r->dsp,[dsp=r->dsp,mode,low,high,shift,agc,gain,fixed,nb,level,fill]{dsp->setMode(mode);dsp->setFilter(low,high);dsp->setShift(shift);dsp->setAgc(agc,gain);dsp->setAgcFixedGain(fixed);dsp->setNoiseBlanker(nb,level,fill);},Qt::QueuedConnection);
 publishAdditionalRx(id);
}
void AnanBackend::publishAdditionalRx(int id)
{
 auto i=m_additionalRx.find(id);const auto* s=m_receiveLayout.slice(id);if(i==m_additionalRx.end()||!s||!i->second->ready)return;const auto& r=*i->second;
 SliceDelta d;d.frequency=s->frequencyHz/1e6;d.panId=rxPan(s->stream);d.mode=r.mode;d.filterLow=r.low;d.filterHigh=r.high;d.audioGain=r.gain;d.audioMute=r.mute;d.active=id==m_activeReceiveSlice;
 d.nb=r.nb!=AetherSDR::NoiseBlankerKind::Off;d.nbKind=r.nb;d.nbLevel=r.nbLevel;d.nbFill=r.nbFill;
 d.agcMode=r.agc==0?QStringLiteral("off"):r.agc==2?QStringLiteral("slow"):r.agc==4?QStringLiteral("fast"):QStringLiteral("med");d.agcThreshold=r.threshold;d.agcOffLevel=r.off;
 const auto a=activeReceiveAntenna();QStringList ports;for(std::size_t n=0;n<a.count;++n)ports.append(QString::fromLatin1(a.ports[n].data(),int(a.ports[n].size())));d.rxAntennaList=ports;d.rxAntenna=QString::fromLatin1(a.current.data(),int(a.current.size()));emit sliceChanged(id,d);
}
void AnanBackend::setActiveSlice(int id){if(!m_receiveLayout.slice(id))return;m_activeReceiveSlice=id;emitSliceState();for(const auto& [sid,r]:m_additionalRx)publishAdditionalRx(sid);}
bool AnanBackend::removeSlice(int id)
{
 if(!m_connected||m_receiveBuilding||m_rateChanging)return false;const auto before=m_receiveLayout.streams();if(!m_receiveLayout.remove(id))return false;
 if(auto i=m_additionalRx.find(id);i!=m_additionalRx.end()){i->second->route->store(-1);i->second->dsp->deleteLater();m_additionalRx.erase(i);}
 if(id==0)m_primaryAudio.clear();if(m_activeReceiveSlice==id)m_activeReceiveSlice=m_receiveLayout.slices().begin()->first;
 emit sliceRemoved(id);retireEmptyReceiveWindows(before);updateReceiveStreams();setActiveSlice(m_activeReceiveSlice);emit capabilitiesChanged();if(m_additionalRx.empty())m_receiveMixer->stop();return true;
}
bool AnanBackend::removePanadapter(const QString& pan)
{
 const int st=streamForPan(pan);if(st<0)return false;std::vector<int> ids;
 for(const auto& [id,s]:m_receiveLayout.slices())if(s.stream==st)ids.push_back(id);if(ids.size()==m_receiveLayout.slices().size())return false;
 for(int id:ids)if(!removeSlice(id))return false;return true;
}
void AnanBackend::clearAdditionalRx()
{
 ++m_receiveEpoch;m_receiveBuilding=false;if(m_receiveMixer)m_receiveMixer->stop();m_primaryAudio.clear();
 for(const auto& [id,r]:m_additionalRx){r->route->store(-1);if(r->dsp)r->dsp->deleteLater();}m_additionalRx.clear();m_primaryRoute->store(0);m_activeReceiveSlice=0;
}
void AnanBackend::collectReceiveAudio(int id,const QByteArray& bytes)
{
 if(bytes.isEmpty()||!m_connected)return;auto stage=bytes;std::deque<float>* queue=nullptr;
 if(id==0){if(primaryVisible())publishLegacySliceAudio(0,bytes);else stage.fill(0);applySliceAudioInPlace(reinterpret_cast<float*>(stage.data()),stage.size()/(2*sizeof(float)),m_sliceAudioMuted,m_sliceAudioGainPercent,m_sliceAudioPanPercent);queue=&m_primaryAudio;
  if(m_additionalRx.empty()){if(primaryVisible()){publishLegacyAudio(stage);sendSpeakerAudioToRadio(stage);}return;}}
 else{auto i=m_additionalRx.find(id);if(i==m_additionalRx.end())return;const auto& r=*i->second;publishLegacySliceAudio(id,bytes);applySliceAudioInPlace(reinterpret_cast<float*>(stage.data()),stage.size()/(2*sizeof(float)),r.mute,r.gain,r.pan);queue=&i->second->audio;}
 const float* p=reinterpret_cast<const float*>(stage.constData());queue->insert(queue->end(),p,p+stage.size()/sizeof(float));while(queue->size()>12000){queue->pop_front();queue->pop_front();}
}
void AnanBackend::mixReceiveAudio()
{
 if(!m_connected||m_additionalRx.empty())return;std::vector<float> mix(480,0);
 auto add=[&](std::deque<float>& q){for(auto& v:mix){if(q.empty())break;v+=q.front();q.pop_front();}};
 if(primaryVisible())add(m_primaryAudio);else m_primaryAudio.clear();for(const auto& [id,r]:m_additionalRx)if(r->ready)add(r->audio);
 for(auto& v:mix)v=std::clamp(v,-1.0f,1.0f);const auto bytes=rxBytes(mix);publishLegacyAudio(bytes);sendSpeakerAudioToRadio(bytes);
}
}

namespace AetherSDR::anan {
void AnanBackend::changeReceiveStreamRate(int stream,int rate)
{
 if(!m_connected||m_receiveBuilding||m_rateChanging||m_droopCalibrator.isRunning())return;
 const bool global=m_pendingParams.protocol==1;const auto before=m_receiveLayout;auto target=before;
 for(int st=0;st<target.limit();++st){const auto w=target.streams()[st];if(w.active&&(global||st==stream)&&!target.setWindow(st,w.centerHz,rate)){publishReceiveWindows();return;}}
 if(target.streams()[stream].rateKsps==before.streams()[stream].rateKsps)return;
 struct Build{int id;QPointer<AnanRxDsp> dsp;AnanRxDsp::Config config;AnanRxDsp::RebuildResult result;};
 auto builds=std::make_shared<std::vector<Build>>();const bool primary=primaryVisible()&&(global||primaryPanIndex()==stream);
 if(primary){auto cfg=m_pendingDspConfig;applyLiveRxState(cfg);cfg.inputSampleRateHz=rate*1000;builds->push_back({0,m_dsp,cfg,{}});}
 std::vector<QPointer<AnanRxDsp>> running{m_dsp};
 for(const auto& [id,r]:m_additionalRx){if(r->ready)running.push_back(r->dsp);const auto* s=target.slice(id);
  if(r->ready&&s&&(global||s->stream==stream)){auto cfg=r->config;cfg.inputSampleRateHz=rate*1000;builds->push_back({id,r->dsp,cfg,{}});}}
 m_receiveBuilding=true;const auto epoch=m_receiveEpoch;
 QMetaObject::invokeMethod(m_client,[this,builds,target,epoch,rate,primary,global,running]{
  for(auto dsp:running)if(dsp)dsp->beginRebuild();
  QMetaObject::invokeMethod(m_dspBuildContext,[this,builds,target,epoch,rate,primary,global,running]{
   bool ok=true;for(auto& b:*builds){b.result=AnanRxDsp::buildChannel(b.config);if(!b.result.channel){ok=false;break;}}
   QMetaObject::invokeMethod(this,[this,builds,target,epoch,rate,primary,global,running,ok]{
    if(epoch!=m_receiveEpoch)return;
    if(!ok){for(auto dsp:running)if(dsp)QMetaObject::invokeMethod(dsp,[dsp]{dsp->resumeAfterPlanning();},Qt::QueuedConnection);m_receiveBuilding=false;publishReceiveWindows();emit connectionError(QStringLiteral("ANAN DSP build failed; previous receive rate retained"));return;}
    m_receiveLayout=target;if(global||primary){m_pendingParams.ddc0RateKsps=rate;m_pendingDspConfig.inputSampleRateHz=rate*1000;m_receiveLayout.setDefaultRate(rate);}
    for(const auto& b:*builds)if(b.id!=0)m_additionalRx.at(b.id)->config.inputSampleRateHz=rate*1000;
    QMetaObject::invokeMethod(m_client,[this,builds,epoch,rate,global,running]{
     for(auto& b:*builds)if(b.dsp){b.dsp->setAudioMuted(true);b.dsp->installRebuiltChannel(std::move(b.result));}
     for(auto dsp:running)if(dsp)dsp->resumeAfterPlanning();
     const bool ok=!global||m_client->setDdcRateLive(0,rate);
     QMetaObject::invokeMethod(this,[this,builds,epoch,ok]{
      if(epoch!=m_receiveEpoch)return;if(!ok){m_receiveBuilding=false;emit connectionError(QStringLiteral("ANAN rate write failed"));disconnectRadio();return;}
      updateReceiveStreams();publishReceiveWindows();for(const auto& [id,r]:m_additionalRx){r->audio.clear();applyAdditionalRx(id);}m_primaryAudio.clear();pushModeFilterShift();
      QTimer::singleShot(200,this,[this,builds,epoch]{if(epoch!=m_receiveEpoch)return;for(auto& b:*builds)if(b.dsp)QMetaObject::invokeMethod(b.dsp,[dsp=b.dsp]{dsp->setAudioMuted(false);},Qt::QueuedConnection);m_receiveBuilding=false;});
     },Qt::QueuedConnection);
    },Qt::QueuedConnection);
   },Qt::QueuedConnection);
  },Qt::QueuedConnection);
 },Qt::QueuedConnection);
}
}
