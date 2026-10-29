#pragma once
#include <vector>
#include <string>
#include "../../examples/companion_radio/health/MotionCaptureFormat.h"
#include "../../src/helpers/GyroCalibration.h"

// Synthetic capture inputs exercise the wire contract and production classes.
// Actual HealthMonitor orchestration has a separate integration suite.
struct HealthCaptureFixture {
  MotionRule rule; FallResponse response;
  GyroCalibration::Result calibration;
  std::vector<uint8_t> bytes;
  uint32_t now=0,sample_sequence=0,record_sequence=0,generation=0,discarded=0;
  bool recording=false;
  const std::string source;
  explicit HealthCaptureFixture(const std::string& identity=std::string(64,'a')) : source(identity) {}
  template<class Build> void record(MotionCapture::Kind kind,Build build) {
    if(!recording) return;
    uint8_t payload[MotionCapture::checkpoint_capacity+128];
    MotionCapture::Writer writer(payload,sizeof(payload)); build(writer);
    if(!writer.ok()) return;
    uint8_t header[7]; MotionCapture::Writer h(header,sizeof(header));
    MotionCapture::writeHeader(h,kind,uint16_t(writer.size()),record_sequence++);
    bytes.insert(bytes.end(),header,header+sizeof(header)); bytes.insert(bytes.end(),payload,payload+writer.size());
  }
  void begin() {
    recording=true;
    record(MotionCapture::Begin,[&](MotionCapture::Writer& w){
      uint8_t checkpoint[MotionCapture::checkpoint_capacity]; MotionCapture::Writer state(checkpoint,sizeof(checkpoint));
      MotionCapture::writeCheckpoint(state,rule,response);
      w.u16(MotionCapture::version); w.raw(source.data(),source.size()); w.u32(MotionCapture::configIdentity(rule.settings()));
      w.u64(0x1020304050607080ull); w.u32(1); w.u32(now); w.u32(sample_sequence); w.u32(generation); w.u32(discarded);
      w.u16(uint16_t(state.size())); w.raw(checkpoint,state.size());
    });
    calibrationRecord(false);
  }
  void calibrationRecord(bool active) {
    record(MotionCapture::Calibration,[&](MotionCapture::Writer& w){
      w.u32(now); w.u32(MotionCapture::calibrationIdentity(calibration)); w.u8(active?1:0); w.u8(active);
      for(float v:calibration.bias) w.f32(v); for(float v:calibration.noise) w.f32(v);
      w.f32(calibration.temperature); w.f32(calibration.temperature_span); w.u32(calibration.samples);
    });
  }
  void end() {
    record(MotionCapture::End,[&](MotionCapture::Writer& w){
      w.u32(now); w.u32(sample_sequence); w.u32(generation); w.u32(discarded); w.u32(0); w.u32(0); w.u32(record_sequence+1);
    });
    recording=false;
  }
  void service(bool pressed=false,bool accept=false) {
    bool changed=response.updateButton(now,pressed); response.updateDeadline(now);
    record(MotionCapture::Service,[&](MotionCapture::Writer& w){
      w.u32(now); w.u8(pressed); MotionCapture::writeExpected(w,rule,response,changed);
    });
    if(accept && response.messagePending()) {
      response.messageAccepted(); record(MotionCapture::Accept,[&](MotionCapture::Writer& w){
        w.u32(now); MotionCapture::writeExpected(w,rule,response);
      });
    }
  }
  bool sample(float acceleration=1,float angle=0,float rotation=0,uint32_t step=20,bool maintenance=false) {
    now+=step; ++sample_sequence;
    const float radians=angle*0.0174532925f;
    float raw[7]={acceleration*sinf(radians),0,acceleration*cosf(radians),rotation+calibration.bias[0],
      calibration.bias[1],calibration.bias[2],25};
    float corrected[7]; memcpy(corrected,raw,sizeof(raw));
    for(unsigned i=0;i<3;++i) corrected[i+3]-=calibration.bias[i];
    const float r=sqrtf(corrected[3]*corrected[3]+corrected[4]*corrected[4]+corrected[5]*corrected[5]);
    bool fired=false;
    if(maintenance) { response.motionUnavailable(); rule.update(now,0,0,false); }
    else { response.observeMotion(now,corrected[0],corrected[1],corrected[2],r);
      fired=rule.update(now,corrected[0],corrected[1],corrected[2],r,true); }
    record(MotionCapture::Sample,[&](MotionCapture::Writer& w){
      w.u32(sample_sequence); w.u32(now); w.u32(now); for(float v:raw) w.f32(v); for(float v:corrected) w.f32(v);
      w.u16(1|(maintenance?0x200:0)); w.u32(MotionCapture::calibrationIdentity(calibration)); w.u8(maintenance);
      MotionCapture::writeExpected(w,rule,response,fired);
    });
    if(fired) {
      bool opened=response.onFall(now,false);
      record(MotionCapture::Fall,[&](MotionCapture::Writer& w){
        w.u32(now); w.u8(0); MotionCapture::writeExpected(w,rule,response,opened);
      });
    }
    service(false,true); return fired;
  }
  void hold(uint32_t ms,float acceleration=1,float angle=0,float rotation=0) {
    for(uint32_t n=0;n<ms;n+=20) sample(acceleration,angle,rotation);
  }
  void invalidate(uint8_t mask=3) {
    if(mask&2) response.motionUnavailable(); if(mask&1) rule.update(now,0,0,false);
    record(MotionCapture::Invalidate,[&](MotionCapture::Writer& w){
      w.u32(now); w.u32(now); w.u8(mask); MotionCapture::writeExpected(w,rule,response);
    });
  }
  void discontinuity() {
    ++generation; discarded+=7;
    record(MotionCapture::Acquisition,[&](MotionCapture::Writer& w){
      w.u32(now); w.u32(generation); w.u32(discarded); w.u32(1); w.u32(0);
    });
    invalidate();
  }
};
