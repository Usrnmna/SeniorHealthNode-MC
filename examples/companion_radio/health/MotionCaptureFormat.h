#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <type_traits>
#include "MotionRule.h"
#include "FallResponse.h"

// Development wire format, independent of compiler padding, endianness and
// float formatting. Samples retain IEEE-754 binary32 values, including gravity.
namespace MotionCapture {
static constexpr uint16_t version = 1;
static constexpr size_t header_size = 7;
static constexpr size_t checkpoint_capacity = 2048;
static constexpr size_t operation_capacity = 128;
enum Kind : uint8_t { Begin=1, Sample, Invalidate, Fall, Service, Accept,
  Restore, Seed, Calibration, Buzzer, Acquisition, End };

class Writer {
  uint8_t* bytes; size_t capacity, offset=0; bool valid=true;
public:
  Writer(uint8_t* output, size_t size) : bytes(output), capacity(size) {}
  size_t size() const { return offset; }
  bool ok() const { return valid; }
  void u8(uint8_t v) { if (offset<capacity) bytes[offset++]=v; else valid=false; }
  void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v>>8)); }
  void u32(uint32_t v) { u16(uint16_t(v)); u16(uint16_t(v>>16)); }
  void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v>>32)); }
  void f32(float v) { static_assert(sizeof(float)==4,"Capture requires binary32"); uint32_t bits; memcpy(&bits,&v,4); u32(bits); }
  void raw(const void* data,size_t size) { const auto* p=static_cast<const uint8_t*>(data); for(size_t i=0;i<size;++i) u8(p[i]); }
  void value(float v) { f32(v); }
  template<class T> typename std::enable_if<std::is_integral<T>::value || std::is_enum<T>::value>::type value(T v) {
    if (std::is_enum<T>::value || sizeof(T)==1) u8(uint8_t(v)); else u32(uint32_t(v));
  }
  void operator()() {}
  template<class T,class... Rest> void operator()(const T& v,const Rest&... rest) { value(v); (*this)(rest...); }
};

class Reader {
  const uint8_t* bytes; size_t length, offset=0; bool valid=true;
public:
  Reader(const uint8_t* input,size_t size) : bytes(input),length(size) {}
  bool ok() const { return valid; }
  bool done() const { return valid && offset==length; }
  size_t remaining() const { return offset<=length ? length-offset : 0; }
  void reject() { valid=false; }
  uint8_t u8() { if(offset<length) return bytes[offset++]; valid=false; return 0; }
  uint16_t u16() { uint16_t v=u8(); return v | uint16_t(u8())<<8; }
  uint32_t u32() { uint32_t v=u16(); return v | uint32_t(u16())<<16; }
  uint64_t u64() { uint64_t v=u32(); return v | uint64_t(u32())<<32; }
  float f32() { uint32_t bits=u32(); float v; memcpy(&v,&bits,4); if(!isfinite(v)) reject(); return v; }
  bool boolean() { uint8_t v=u8(); if(v>1) reject(); return v!=0; }
  void raw(void* output,size_t size) { auto* p=static_cast<uint8_t*>(output); for(size_t i=0;i<size;++i) p[i]=u8(); }
  void value(float& v) { v=f32(); }
  void value(const float& v) { if(f32()!=v) reject(); }
  template<class T> typename std::enable_if<std::is_integral<T>::value || std::is_enum<T>::value>::type value(T& v) {
    uint32_t n=(std::is_enum<T>::value || sizeof(T)==1) ? u8() : u32();
    if(std::is_same<T,bool>::value && n>1) reject();
    v=static_cast<T>(n);
  }
  template<class T> typename std::enable_if<std::is_integral<T>::value || std::is_enum<T>::value>::type value(const T& v) {
    T read{}; value(read); if(read!=v) reject();
  }
  void operator()() {}
  template<class T,class... Rest> void operator()(T& v,Rest&... rest) { value(v); (*this)(rest...); }
};

inline uint32_t fingerprint(const uint8_t* bytes,size_t size) {
  uint32_t value=2166136261u; for(size_t i=0;i<size;++i) { value^=bytes[i]; value*=16777619u; } return value;
}
// CRC protects one complete atomic ASCII frame (25 bytes after RS).
inline uint16_t crc16(const uint8_t* bytes,size_t size) {
  uint16_t crc=0xffff; for(size_t i=0;i<size;++i) { crc^=uint16_t(bytes[i])<<8;
    for(unsigned bit=0;bit<8;++bit) crc=uint16_t((crc<<1)^((crc&0x8000)?0x1021:0)); } return crc;
}
inline void writeHeader(Writer& w,Kind kind,uint16_t payload_size,uint32_t sequence) {
  w.u8(uint8_t(kind)); w.u16(payload_size); w.u32(sequence);
}

// Expected outputs are evidence, not inputs. Replay compares them after calling
// the production methods; channel secrets never enter this structure.
struct Expected {
  uint8_t state=0,reason=0,event=0; bool result=false,alarm=false;
  uint8_t notice=0; uint32_t incident=0,pending_incident=0,dropped=0;
};
inline Expected expected(const MotionRule& rule,const FallResponse& response,bool result=false) {
  Expected e; e.state=uint8_t(rule.currentState()); e.reason=uint8_t(rule.lastReason());
  e.event=uint8_t(rule.eventKind()); e.result=result; e.alarm=response.alarmActive();
  e.notice=uint8_t(response.pendingNotice()); e.incident=response.incidentId();
  e.pending_incident=response.pendingIncident(); e.dropped=response.droppedConfirmations(); return e;
}
inline void writeExpected(Writer& w,const Expected& e) {
  w(e.state,e.reason,e.event,e.result,e.alarm,e.notice,e.incident,e.pending_incident,e.dropped);
}
inline void writeExpected(Writer& w,const MotionRule& rule,const FallResponse& response,bool result=false) {
  writeExpected(w,expected(rule,response,result));
}
inline Expected readExpected(Reader& r) {
  Expected e; r(e.state,e.reason,e.event,e.result,e.alarm,e.notice,e.incident,e.pending_incident,e.dropped);
  if(e.state>MotionRule::ConfigurationFault || e.reason>MotionRule::LowEpisodeRenewed ||
      e.event>MotionRule::LowEpisodeRenewed || e.notice>uint8_t(FallNotice::UserConfirmedFalseAlarm)) r.reject();
  return e;
}
inline bool matches(const Expected& a,const Expected& b) {
  return a.state==b.state && a.reason==b.reason && a.event==b.event && a.result==b.result &&
    a.alarm==b.alarm && a.notice==b.notice && a.incident==b.incident &&
    a.pending_incident==b.pending_incident && a.dropped==b.dropped;
}
}

// One explicit field list is shared by read and write. No pointer, object padding,
// channel key, channel label, location or radio retry state is captured. Restored
// response channels are placeholders: replay verifies response logic, not routing.
struct MotionCaptureCheckpoint {
  template<class A,class C> static void config(A& a,C& c) {
    a(c.low_g,c.impact_g,c.rotation_dps,c.event_window_ms,c.pretrigger_ms,c.recovery_ms,
      c.recovery_samples,c.refractory_ms,c.max_sample_gap_ms,c.quiet_min_g,c.quiet_max_g,
      c.quiet_max_dps,c.context_paths,c.context_angle_deg,c.slow_angle_deg,c.support_impact_g,
      c.support_rotation_dps,c.unload_g,c.slow_peak_g,c.context_window_ms,c.confirm_quiet_ms,
      c.slow_quiet_ms,c.confirm_timeout_ms,c.context_feature_ms);
  }
  template<class A,class F> static void feature(A& a,F& f) { a(f.at,f.seen); }
  template<class A,class R> static void motion(A& a,R& r) {
    config(a,r.cfg);
    a(r.strong_pair.seen,r.strong_pair.low_at,r.strong_pair.impact_at,r.strong_pair.impact_peak);
    feature(a,r.context_impact); feature(a,r.context_support); feature(a,r.context_unload);
    feature(a,r.context_rotation); feature(a,r.context_slow_peak);
    for(auto& h:r.history) a(h.at,h.x,h.y,h.z);
    a(r.head,r.history_count,r.settings_valid,r.have_sample,r.recovery_started,r.refractory_pending,
      r.state,r.reason,r.event_kind,r.last_sample,r.recovery_at,r.recovery_count,r.fired_at,
      r.low_seen,r.gyro_seen,r.low_active,r.candidate_low_seen,r.low_at,r.gyro_at,r.candidate_at,
      r.support_at,r.quiet_at,r.reference_at,r.reference_valid,r.supported,r.quiet_tracking,
      r.ref_x,r.ref_y,r.ref_z,r.peak_accel,r.peak_rotation,r.trough,r.reported_accel,
      r.reported_rotation,r.preceding_rotation,r.support_kind);
  }
  template<class A,class M> static void movement(A& a,M& m) {
    a(m.have_sample,m.quiet_tracking,m.last_sample,m.quiet_since,m.base_x,m.base_y,m.base_z);
  }
  template<class A,class B> static void button(A& a,B& b) {
    a(b.window,b.double_window,b.long_press,b.debounce,b.edge_at,b.down_at,b.first_down,
      b.count,b.stable,b.candidate,b.ignore_release,b.double_pending);
  }
  template<class A,class R> static void response(A& a,R& r) {
    button(a,r.clicks); movement(a,r.movement); movement(a,r.deadline_movement);
    a(r.pending,r.deferred_initial,r.last_response,r.started_at,r.incident,r.pending_incident,
      r.dropped_confirmations,r.active,r.warned,r.quiet_reported,r.deadline_snapshot);
  }
  static bool valid(const MotionRule& r,const FallResponse& f) {
    return r.head<r.history_capacity && r.history_count<=r.history_capacity &&
      r.settings_valid==MotionRule::validConfig(r.cfg) && r.state<=MotionRule::ConfigurationFault &&
      r.reason<=MotionRule::LowEpisodeRenewed && r.event_kind<=MotionRule::LowEpisodeRenewed &&
      r.support_kind<=MotionRule::LowEpisodeRenewed && f.clicks.count<=2 &&
      uint8_t(f.pending)<=uint8_t(FallNotice::UserConfirmedFalseAlarm) &&
      uint8_t(f.deferred_initial)<=uint8_t(FallNotice::UserConfirmedFalseAlarm) &&
      uint8_t(f.last_response)<=uint8_t(FallNotice::UserConfirmedFalseAlarm) &&
      (!f.active || f.incident!=0);
  }
};

namespace MotionCapture {
inline void writeCheckpoint(Writer& w,const MotionRule& rule,const FallResponse& response) {
  MotionCaptureCheckpoint::motion(w,rule); MotionCaptureCheckpoint::response(w,response);
}
inline bool readCheckpoint(Reader& r,MotionRule& rule,FallResponse& response) {
  MotionCaptureCheckpoint::motion(r,rule); MotionCaptureCheckpoint::response(r,response);
  if(!MotionCaptureCheckpoint::valid(rule,response)) r.reject(); return r.ok();
}
inline uint32_t responseFingerprint(const FallResponse& response) {
  uint8_t bytes[160]; Writer w(bytes,sizeof(bytes)); MotionCaptureCheckpoint::response(w,response);
  return w.ok() ? fingerprint(bytes,w.size()) : 0;
}
template<class CalibrationResult> inline uint32_t calibrationIdentity(const CalibrationResult& c) {
  uint8_t bytes[36]; Writer w(bytes,sizeof(bytes));
  for(float v:c.bias) w.f32(v); for(float v:c.noise) w.f32(v);
  w.f32(c.temperature); w.f32(c.temperature_span); w.u32(c.samples);
  return fingerprint(bytes,w.size());
}
// Runtime detector settings plus response constants: changing either is rejected
// even when a capture is replayed with otherwise identical source files.
inline uint32_t configIdentity(const FallDetectionConfig& config=FallDetectionConfig{}) {
  uint8_t bytes[192]; Writer w(bytes,sizeof(bytes)); MotionCaptureCheckpoint::config(w,config);
  using namespace HealthNodeConfig;
  w(response_timeout_ms,residual_quiet_required_ms,residual_max_sample_gap_ms,
    residual_acceleration_change_g,residual_gravity_tolerance_g,residual_rotation_dps,
    click_window_ms,false_alarm_click_window_ms,long_press_ms,debounce_ms,motion_sample_ms);
  return w.ok() ? fingerprint(bytes,w.size()) : 0;
}
}
