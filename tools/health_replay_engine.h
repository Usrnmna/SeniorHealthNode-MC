#pragma once
#include <string>
#include <sstream>
#include <vector>
#include <cstring>
#include "../examples/companion_radio/health/MotionCaptureFormat.h"
#include "../src/helpers/GyroCalibration.h"

// Host-only validator/executor. Production detector and response classes own all
// classification and incident decisions. Metadata never supplies quiet samples.
class HealthReplay {
public:
  struct Totals {
    uint32_t records=0,samples=0,falls=0,rejections=0,invalidations=0;
    uint32_t calibration_changes=0,buzzer_changes=0,acquisition_changes=0;
  } totals;
  explicit HealthReplay(const std::string& expected_source) : source(expected_source) {}
  bool feed(const uint8_t* bytes,size_t size) {
    using namespace MotionCapture;
    if(!error.empty()) return false;
    if(size<header_size) return fail("truncated record header");
    Reader header(bytes,header_size); const auto kind=static_cast<Kind>(header.u8());
    const auto payload_length=header.u16(); const auto seq=header.u32();
    if(payload_length!=size-header_size) return fail("record length mismatch");
    if(seq!=next_record) return fail("record sequence gap (recording/export loss)");
    if(ended) return fail("records after End; ambiguous session boundary");
    if(!begun && kind!=Begin) return fail("missing Begin/checkpoint");
    Reader r(bytes+header_size,payload_length); bool result=false,compare=false;
    if(kind==Begin) {
      if(begun) return fail("multiple Begin records; split sessions explicitly");
      if(r.u16()!=version) return fail("unsupported format version");
      char identity[65]={}; r.raw(identity,64);
      if(source.size()!=64 || source!=identity) return fail("source/configuration content identity mismatch");
      const uint32_t config=r.u32();
      if(config!=configIdentity()) return fail("incompatible detector/response settings");
      boot=r.u64(); session=r.u32(); const uint32_t now=r.u32();
      sample_sequence=r.u32(); generation=r.u32(); discarded=r.u32();
      const uint16_t length=r.u16();
      if(!boot || !session || length>checkpoint_capacity || length!=r.remaining())
        return fail("invalid session/checkpoint boundary");
      std::vector<uint8_t> state(length); r.raw(state.data(),length);
      Reader checkpoint(state.data(),state.size());
      if(!readCheckpoint(checkpoint,rule,response) || !checkpoint.done() ||
          configIdentity(rule.settings())!=config) return fail("invalid/incompatible checkpoint");
      begun=true; wall=now;
    } else if(kind==Sample) {
      const uint32_t seq_sample=r.u32(),captured=r.u32(),processed=r.u32();
      if(seq_sample!=sample_sequence+1) return fail("sample sequence gap (recording/export loss)");
      sample_sequence=seq_sample;
      float raw[7],corrected[7]; for(float& v:raw) v=r.f32(); for(float& v:corrected) v=r.f32();
      const uint16_t quality=r.u16(); const uint32_t cal=r.u32(); const bool maintenance=r.boolean();
      if(!r.ok() || !time(processed)) return fail("invalid sample values or processing time");
      if(uint32_t(processed-captured)>=0x80000000u) return fail("capture timestamp is after processing time");
      if(!have_calibration || cal!=calibration_id) return fail("sample has undeclared calibration identity");
      for(unsigned i=0;i<7;++i) {
        const float expected_value=(i>=3 && i<6) ? raw[i]-bias[i-3] : raw[i];
        if(expected_value!=corrected[i]) return fail("raw/corrected measurements do not match calibration");
      }
      if(bool(quality&0x200)!=maintenance) return fail("maintenance quality mismatch");
      if(acquisition_break_pending) return fail("acquisition discontinuity lacks explicit invalidation");
      if(maintenance) { response.motionUnavailable(); rule.update(captured,0,0,false); }
      else {
        const float rotation=sqrtf(corrected[3]*corrected[3]+corrected[4]*corrected[4]+corrected[5]*corrected[5]);
        response.observeMotion(captured,corrected[0],corrected[1],corrected[2],rotation);
        result=rule.update(captured,corrected[0],corrected[1],corrected[2],rotation,true);
      }
      ++totals.samples; if(result) ++totals.falls;
      if(rule.lastReason()==MotionRule::CandidateExpired) ++totals.rejections;
      compare=true;
    } else if(kind==Invalidate) {
      const uint32_t at=r.u32(),processed=r.u32();
      const uint8_t mask=r.u8();
      if(!time(processed)) return false;
      if(!mask || mask>3) return fail("invalid continuity mask");
      if(mask&2) response.motionUnavailable(); if(mask&1) rule.update(at,0,0,false);
      if(mask==3) acquisition_break_pending=false;
      ++totals.invalidations; compare=true;
    } else if(kind==Fall || kind==Service) {
      const uint32_t now=r.u32(); const bool pressed=r.boolean();
      if(!time(now)) return false;
      if(kind==Fall) result=response.onFall(now,pressed);
      else { result=response.updateButton(now,pressed); response.updateDeadline(now); }
      compare=true;
    } else if(kind==Accept) {
      if(!time(r.u32())) return false;
      if(!response.messagePending()) return fail("message acceptance without a pending owner");
      response.messageAccepted(); compare=true;
    } else if(kind==Restore) {
      const uint32_t now=r.u32(),elapsed=r.u32(),incident=r.u32(); const bool pressed=r.boolean();
      if(!time(now)) return false;
      result=response.restore(now,elapsed,incident,FallChannel{},pressed); compare=true;
    } else if(kind==Seed) {
      const uint32_t now=r.u32(),incident=r.u32(); if(!time(now)) return false;
      response.seedIncident(incident); compare=true;
    } else if(kind==Calibration) {
      const uint32_t now=r.u32(),id=r.u32(); const uint8_t state=r.u8(); const bool active=r.boolean();
      if(!metadataTime(now) || state>7 || active!=(state>=1 && state<=3)) return fail("invalid calibration state");
      const size_t correction_bytes=36;
      uint8_t correction[correction_bytes]; r.raw(correction,correction_bytes);
      if(fingerprint(correction,correction_bytes)!=id) return fail("calibration identity mismatch");
      Reader correction_reader(correction,correction_bytes);
      GyroCalibration::Result applied;
      for(float& v:applied.bias) v=correction_reader.f32();
      for(float& v:applied.noise) v=correction_reader.f32();
      applied.temperature=correction_reader.f32(); applied.temperature_span=correction_reader.f32();
      applied.samples=correction_reader.u32();
      // A self-consistent fingerprint does not establish that the sensor could
      // apply this correction. Share its production validity rules.
      if(!correction_reader.done() || !GyroCalibration::validResult(applied))
        return fail("invalid calibration values");
      for(unsigned i=0;i<3;++i) bias[i]=applied.bias[i];
      calibration_id=id; have_calibration=true; ++totals.calibration_changes;
    } else if(kind==Buzzer) {
      if(!metadataTime(r.u32())) return false; r.boolean(); ++totals.buzzer_changes;
    } else if(kind==Acquisition) {
      if(!metadataTime(r.u32())) return false;
      const uint32_t next_generation=r.u32(); discarded=r.u32(); r.u32(); r.u32();
      if(next_generation!=generation) acquisition_break_pending=true;
      generation=next_generation; ++totals.acquisition_changes;
    } else if(kind==End) {
      if(!time(r.u32())) return false;
      const uint32_t final_sample=r.u32(),final_generation=r.u32(),final_discarded=r.u32();
      const uint32_t dropped=r.u32(),short_writes=r.u32(),issued=r.u32();
      if(dropped || short_writes) return fail("recording/export loss reported at End");
      if(final_sample!=sample_sequence || final_generation!=generation || final_discarded!=discarded || issued!=next_record+1)
        return fail("incomplete final counters");
      ended=true;
    } else return fail("unsupported record kind");
    if(compare) {
      const auto stored=readExpected(r),actual=expected(rule,response,result);
      if(!matches(stored,actual)) {
        std::ostringstream text; text<<"replay divergence: kind="<<unsigned(kind)
          <<" state="<<unsigned(actual.state)<<"/"<<unsigned(stored.state)
          <<" reason="<<unsigned(actual.reason)<<"/"<<unsigned(stored.reason)
          <<" notice="<<unsigned(actual.notice)<<"/"<<unsigned(stored.notice);
        return fail(text.str());
      }
    }
    if(!r.done()) return fail("malformed payload or trailing fields");
    ++next_record; ++totals.records; return true;
  }
  bool finish() { return !error.empty() ? false : (begun && ended ? true : fail("incomplete session: Begin and End are required")); }
  const std::string& failure() const { return error; }
  std::string summary() const {
    std::ostringstream out; out<<"records="<<totals.records<<" samples="<<totals.samples<<" falls="<<totals.falls
      <<" candidate_expirations="<<totals.rejections<<" invalidations="<<totals.invalidations
      <<" acquisition_changes="<<totals.acquisition_changes<<" calibration_records="<<totals.calibration_changes
      <<" buzzer_changes="<<totals.buzzer_changes; return out.str();
  }
private:
  MotionRule rule; FallResponse response; std::string source,error;
  uint32_t next_record=0,sample_sequence=0,generation=0,discarded=0,wall=0,session=0,calibration_id=0;
  uint64_t boot=0; float bias[3]={};
  bool begun=false,ended=false,have_calibration=false,acquisition_break_pending=false;
  bool fail(const std::string& why) { if(error.empty()) error="record "+std::to_string(next_record)+": "+why; return false; }
  bool time(uint32_t now) { if(uint32_t(now-wall)>=0x80000000u) return fail("ambiguous/backward processing time"); wall=now; return true; }
  // Metadata is observed when serialized; a following operation may retain its
  // earlier poll-entry timestamp. Record order, not a global timestamp sort,
  // defines replay. Compare metadata with the latest replay-input clock so a
  // long interval without a metadata change cannot alias after millis rollover.
  bool metadataTime(uint32_t now) { if(uint32_t(now-wall)>=0x80000000u) return fail("ambiguous/backward metadata time"); return true; }
};
