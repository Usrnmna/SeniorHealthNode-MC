#ifdef HEALTH_MONITOR_HOST_HEADER
#include HEALTH_MONITOR_HOST_HEADER
#else
#include "MyMesh.h"
#endif
#if ENV_INCLUDE_MPU6050
#include <initializer_list>
#include <health_source_identity.h>
#include <helpers/ui/AlarmBuzzer.h>

namespace {
void appendCapture(MotionRecorder& recorder, uint8_t kind, const uint8_t* bytes,
                   const MotionCapture::Writer& writer) {
  // Serialization failure is a lost record, never a truncated successful record.
  recorder.append(kind, bytes, writer.ok() ? writer.size() : MotionRecorder::max_payload + 1);
}
}

// Recording is a USB development operation. Starting snapshots live state and
// neither resets evidence nor changes acquisition, alarm, persistence or delivery.
bool MyMesh::handleRecordingCommand(const char* command) {
  using namespace MotionCapture;
  if (!strcmp(command,"motion record start")) {
    if (!bounded_usb_output.ready()) {
      usb_reply.append("Error: USB output worker unavailable; recording disabled\n");return true;
    }
    // Once the previous End is admitted, the single FIFO/worker preserves its
    // order before this Begin. Unrelated BLE/debug traffic need not finish first.
    if (motion_recorder.active() || motion_recorder.pending()) {
      usb_reply.append("Error: recording active or still draining; inspect motion record status\n"); return true;
    }
    if (!capture_boot) {
      getRNG()->random(reinterpret_cast<uint8_t*>(&capture_boot),sizeof(capture_boot));
      if (!capture_boot) capture_boot=1;
    }
    if (capture_session == UINT32_MAX) {
      usb_reply.append("Error: recording session counter exhausted; reboot required\n"); return true;
    }
    ++capture_session;
    // Reserve the prefix, then serialize directly into the fixed staging buffer.
    constexpr size_t prefix_size=100;
    Writer state(capture_buffer+prefix_size,sizeof(capture_buffer)-prefix_size);
    writeCheckpoint(state,motion_rule,fall_response);
    const auto& d=sensors.motion.diagnostics();
    Writer prefix(capture_buffer,prefix_size);
    prefix.u16(version);prefix.raw(HEALTH_SOURCE_ID,64);prefix.u32(configIdentity(motion_rule.settings()));
    prefix.u64(capture_boot);prefix.u32(capture_session);prefix.u32(millis());
    prefix.u32(capture_sample_sequence);prefix.u32(sensors.motion.discontinuityGeneration());
    prefix.u32(d.discarded_samples);prefix.u16(uint16_t(state.size()));
    if (!state.ok() || !prefix.ok() || prefix.size()!=prefix_size ||
        !motion_recorder.begin(capture_buffer,prefix_size+state.size())) {
      usb_reply.append("Error: recording checkpoint could not be serialized\n"); return true;
    }
    capture_invalid=false;capture_invalid_reason=255;
    captureCalibration(true);captureAcquisition(true);captureBuzzer(true);
    usb_reply.append("OK: motion recording v%u started; session=%lu; RAM only; stop to complete capture\n",
        unsigned(version),(unsigned long)capture_session);
  } else if (!strcmp(command,"motion record stop")) {
    if (!motion_recorder.active()) { usb_reply.append("Recording is not active\n");return true; }
    Writer w(capture_buffer,sizeof(capture_buffer));
    w.u32(millis());w.u32(capture_sample_sequence);w.u32(sensors.motion.discontinuityGeneration());
    w.u32(sensors.motion.diagnostics().discarded_samples);w.u32(motion_recorder.droppedRecords());
    w.u32(motion_recorder.shortWrites());w.u32(motion_recorder.issuedRecords()+1);
    motion_recorder.stop(capture_buffer,w.size());
    usb_reply.append("OK: recording stopped; pending frames must drain before capture is complete\n");
  } else if (!strcmp(command,"motion record status")) {
    usb_reply.append("recording=%u draining=%u session=%lu sample_sequence=%lu issued=%lu dropped_records=%lu short_writes=%lu packets=%lu buffered_bytes=%u high_water_bytes=%u capacity_bytes=%u\n",
        unsigned(motion_recorder.active()),unsigned(motion_recorder.pending()),(unsigned long)capture_session,
        (unsigned long)capture_sample_sequence,(unsigned long)motion_recorder.issuedRecords(),
        (unsigned long)motion_recorder.droppedRecords(),(unsigned long)motion_recorder.shortWrites(),
        (unsigned long)motion_recorder.exportedPackets(),unsigned(motion_recorder.bufferedBytes()),
        unsigned(motion_recorder.highWaterBytes()),unsigned(MotionRecorder::capacity));
    usb_reply.append("usb_ready=%u usb_pending_packets=%lu usb_failed_writes=%lu usb_rejected_writes=%lu usb_completed_packets=%lu worker_stack_free_min_bytes=%lu\n",
        unsigned(bounded_usb_output.ready()),(unsigned long)bounded_usb_output.pendingPackets(),
        (unsigned long)bounded_usb_output.failedWrites(),(unsigned long)bounded_usb_output.rejectedWrites(),
        (unsigned long)bounded_usb_output.completedPackets(),(unsigned long)bounded_usb_output.workerStackHighWaterBytes());
  } else return false;
  return true;
}

void MyMesh::serviceMotionRecording() { motion_recorder.drain(bounded_usb_output); }

void MyMesh::captureInvalidation(uint32_t at,uint32_t processed,uint8_t mask) {
  if (!motion_recorder.active()) return;
  MotionCapture::Writer w(capture_buffer,sizeof(capture_buffer));
  w.u32(at);w.u32(processed);w.u8(mask);MotionCapture::writeExpected(w,motion_rule,fall_response);
  appendCapture(motion_recorder,MotionCapture::Invalidate,capture_buffer,w);
  capture_invalid=true;capture_invalid_reason=uint8_t(motion_rule.lastReason());
}

void MyMesh::captureSample(uint32_t sampled,uint32_t processed,bool maintenance,bool fired,uint16_t quality) {
  ++capture_sample_sequence; // Independent of resettable sensor diagnostics.
  if (!motion_recorder.active()) return;
  captureCalibration();
  const auto& raw=sensors.motion.raw_values;const auto& v=sensors.motion.values;
  MotionCapture::Writer w(capture_buffer,sizeof(capture_buffer));
  w.u32(capture_sample_sequence);w.u32(sampled);w.u32(processed);
  for (float x : {raw.ax,raw.ay,raw.az,raw.gx,raw.gy,raw.gz,raw.temperature,
                  v.ax,v.ay,v.az,v.gx,v.gy,v.gz,v.temperature}) w.f32(x);
  w.u16(quality);w.u32(capture_calibration_id);w.u8(maintenance);
  MotionCapture::writeExpected(w,motion_rule,fall_response,fired);
  appendCapture(motion_recorder,MotionCapture::Sample,capture_buffer,w);capture_invalid=false;
}

void MyMesh::captureResponse(uint8_t kind,uint32_t now,bool pressed,bool result) {
  if (!motion_recorder.active()) return;
  MotionCapture::Writer w(capture_buffer,sizeof(capture_buffer));w.u32(now);
  if (kind==MotionCapture::Fall || kind==MotionCapture::Service) w.u8(pressed);
  MotionCapture::writeExpected(w,motion_rule,fall_response,result);
  appendCapture(motion_recorder,kind,capture_buffer,w);
}

void MyMesh::captureCalibration(bool force) {
  if (!motion_recorder.active()) return;
  const auto& c=sensors.motion.calibration();
  uint8_t identity[36];MotionCapture::Writer id(identity,sizeof(identity));
  for (float x : c.bias) id.f32(x);for (float x : c.noise) id.f32(x);
  id.f32(c.temperature);id.f32(c.temperature_span);id.u32(c.samples);
  const uint32_t next=MotionCapture::fingerprint(identity,id.size());
  const uint8_t state=uint8_t(gyro_calibration.state());
  if (!force && next==capture_calibration_id && state==capture_calibration_state) return;
  capture_calibration_id=next;capture_calibration_state=state;
  MotionCapture::Writer w(capture_buffer,sizeof(capture_buffer));w.u32(millis());
  w.u32(next);w.u8(state);w.u8(gyro_calibration.active());w.raw(identity,id.size());
  appendCapture(motion_recorder,MotionCapture::Calibration,capture_buffer,w);
}

void MyMesh::captureBuzzer(bool force) {
  if (!motion_recorder.active()) return;
  const bool sounding=alarm_buzzer.sounding();
  if (!force && sounding==capture_buzzer) return;
  capture_buzzer=sounding;MotionCapture::Writer w(capture_buffer,sizeof(capture_buffer));
  w.u32(millis());w.u8(sounding);appendCapture(motion_recorder,MotionCapture::Buzzer,capture_buffer,w);
}

void MyMesh::captureAcquisition(bool force) {
  if (!motion_recorder.active()) return;
  const auto& d=sensors.motion.diagnostics();const uint32_t generation=sensors.motion.discontinuityGeneration();
  if (!force && generation==capture_generation && d.discarded_samples==capture_discarded &&
      d.fifo_overflows==capture_overflows && d.timing_discontinuities==capture_discontinuities) return;
  capture_generation=generation;capture_discarded=d.discarded_samples;
  capture_overflows=d.fifo_overflows;capture_discontinuities=d.timing_discontinuities;
  MotionCapture::Writer w(capture_buffer,sizeof(capture_buffer));w.u32(millis());w.u32(generation);
  w.u32(d.discarded_samples);w.u32(d.fifo_overflows);w.u32(d.timing_discontinuities);
  appendCapture(motion_recorder,MotionCapture::Acquisition,capture_buffer,w);
}
#endif
