#ifdef HEALTH_MONITOR_HOST_HEADER
#include HEALTH_MONITOR_HOST_HEADER
#else
#include "MyMesh.h"
#endif
#if ENV_INCLUDE_MPU6050

// Snapshot rows are formatted only when the previous USB row has drained.
void MyMesh::continueMotionTraceReply() {
  if (trace_reply_row >= trace_reply_count) return;
  if (trace_reply_events) {
    const auto& e = event_reply[trace_reply_row++];
    usb_reply.append("t=%lu state=%u reason=%s a=%.3f r=%.2f quality=0x%x\n",
        (unsigned long)e.at, unsigned(e.state), MotionRule::reasonName(MotionRule::Reason(e.reason)),
        e.acceleration, e.rotation, unsigned(e.quality));
  } else {
    const auto& s = trace_reply[trace_reply_row++];
    usb_reply.append("t=%lu raw=%.3f/%.3f/%.3f,%.2f/%.2f/%.2f temp=%.2f gyro=%.2f/%.2f/%.2f state=%u reason=%s quality=0x%x buzzer=%u\n",
        (unsigned long)s.at, s.raw[0],s.raw[1],s.raw[2],s.raw[3],s.raw[4],s.raw[5],s.raw[6],
        s.gyro[0],s.gyro[1],s.gyro[2],unsigned(s.state),MotionRule::reasonName(MotionRule::Reason(s.reason)),
        unsigned(s.quality),unsigned(s.buzzer));
  }
}

// Format one statistic per loop from a single command snapshot. Sensor work
// continues between rows; serial backpressure pauses output, not sensing.
void MyMesh::continueMotionStatsReply() {
  if (motion_stats_row < 0) return;
  static const char* labels[] = {"ax[g]", "ay[g]", "az[g]", "gx[dps]", "gy[dps]", "gz[dps]",
      "accel_magnitude[g]", "gyro_magnitude[dps]", "chip_temp[C]"};
  unsigned field = unsigned(motion_stats_row);
  if (motion_stats_raw && field >= 3 && field <= 5) field += 6;
  if (motion_stats_raw && field == MotionStatistics::Rotation) field = MotionStatistics::RawRotation;
  const auto& stats = motion_stats_reply.fields[field];
  usb_reply.append("%s min=%.5f max=%.5f mean=%.5f ", labels[motion_stats_row],
      stats.minimum, stats.maximum, stats.mean);
  if (motion_stats_reply.samples > 1)
    usb_reply.append("stddev=%.5f\n", stats.standardDeviation(motion_stats_reply.samples));
  else usb_reply.append("stddev=n/a (requires 2 samples)\n");
  if (++motion_stats_row == 9) motion_stats_row = -1;
}

bool MyMesh::handleDiagnosticCommand(const char* command) {
  const uint32_t now = millis();
  const auto& s = motion_statistics;
  if (!strcmp(command, "motion trace") || !strcmp(command, "motion events") || !strcmp(command, "motion evidence")) {
    const bool evidence = !strcmp(command, "motion evidence");
    trace_reply_events = !strcmp(command, "motion events"); trace_reply_row = 0;
    trace_reply_count = trace_reply_events ? motion_trace.eventCount() :
        (evidence ? motion_trace.evidenceCount() : motion_trace.sampleCount());
    for (unsigned i=0; i<trace_reply_count; ++i) {
      if (trace_reply_events) event_reply[i]=motion_trace.event(i);
      else trace_reply[i]=evidence ? motion_trace.evidence(i) : motion_trace.sample(i);
    }
    usb_reply.append("%s rows=%u; estimated acquisition times; raw includes gravity; buzzer=commanded state at processing\n",
        command, trace_reply_count);
  } else if (!strcmp(command, "motion stats reset")) {
    motion_statistics.reset(now, "manual");
    usb_reply.append("OK: motion/timing statistics reset; calibration, detector and alarms unchanged\n");
  } else if (!strcmp(command, "health stats reset")) {
    motion_statistics.reset(now, "health-reset"); sensors.motion.resetDiagnostics();
    usb_reply.append("OK: motion/timing/error statistics reset; faults, alarms, radio counters and startup max_gap_ms unchanged\n");
  } else if (!strcmp(command, "motion stats") || !strcmp(command, "motion stats raw")) {
    if (!s.samples) { usb_reply.append("Motion statistics unavailable: no fresh valid samples since reset\n"); return true; }
    motion_stats_reply = s; motion_stats_raw = !strcmp(command, "motion stats raw");
    usb_reply.append("motion stats %s n=%lu coverage_ms=%llu reset=%s reset_at_ms=%lu first_at_ms=%lu last_at_ms=%lu\n",
        motion_stats_raw ? "uncorrected-scaled" : "corrected", (unsigned long)s.samples,
        (unsigned long long)s.coverage_ms, s.reset_reason, (unsigned long)s.reset_at,
        (unsigned long)s.first_at, (unsigned long)s.last_at);
    usb_reply.append("valid_now=%u; acceleration includes gravity; stddev uses n-1; historical aggregate, not current validity\n", unsigned(sensors.motion.valid));
    motion_stats_row = 0;
  } else if (!strcmp(command, "health detail")) {
    const auto availability=detector_availability.totals(now);
    usb_reply.append("availability=%s unavailable_ms=%llu experimental_context_paths=%u\n",
        motionAvailability(now), (unsigned long long)detector_availability.unavailableMs(now),
        unsigned(motion_rule.settings().context_paths));
    usb_reply.append("monitoring_ms=%llu; exclusive observed causes; monitoring starts at first motion poll\n",
        (unsigned long long)availability.monitoredMs());
    for (unsigned cause=0;cause<DetectorAvailability::CauseCount;++cause)
      usb_reply.append("%s_ms=%llu%s",DetectorAvailability::name(DetectorAvailability::Cause(cause)),
          (unsigned long long)availability.ms[cause],cause+1==DetectorAvailability::CauseCount ? "\n" : " ");
    usb_reply.append("recovery=%s persistence_pending=%u persistence_fault=%u initial_pending=%u coalesced_initials=%lu\n",
        FallIncidentStore::outcomeName(recovery_outcome),unsigned(persistence_dirty),unsigned(persistence_fault),
        unsigned(initial_alert.pending()),(unsigned long)initial_alert.coalescedCount());
    usb_reply.append("persistence_result=%s slot=%u errno=%d close_errno=%d checked_api=%u; physical_durability=unverified\n",
        FallIncidentStore::saveStageName(persistence_result.stage),unsigned(persistence_result.slot),
        persistence_result.error,persistence_result.close_error,unsigned(persistence_result.api_checked));
    usb_reply.append("dropped_confirmations=%lu\n",(unsigned long)fall_response.droppedConfirmations());
    const int mounted = incidentStorageMounted();
    usb_reply.append("storage_boot=%s mounted=%s format_history=unknown\n",
        !storage_startup_attempted ? "not-attempted" : storage_startup_ok ? "ok" : "failed",
        mounted < 0 ? "unknown" : mounted ? "yes" : "no");
    for (unsigned slot = 0; slot < 2; ++slot) {
      const auto& read = recovery_reads[slot];
      usb_reply.append("boot_read%u=%s errno=%d close=%d checked=%u\n", slot,
          FallIncidentStore::readStageName(read.stage), read.error, read.close_error, unsigned(read.api_checked));
    }
    const auto& read = persistence_result.read_result;
    usb_reply.append("save_read=%s slot=%u errno=%d close=%d checked=%u\n",
        FallIncidentStore::readStageName(read.stage), unsigned(read.slot), read.error,
        read.close_error, unsigned(read.api_checked));
    usb_reply.append("uptime_ms=%lu initialized=%u valid=%u fault=%u sample_age_ms=",
        (unsigned long)now, unsigned(sensors.motion.initialized()), unsigned(sensors.motion.valid), unsigned(motion_health.fault()));
    if (sensors.motion.valid) usb_reply.append("%lu\n", (unsigned long)sensors.motion.sampleAgeMs(now));
    else usb_reply.append("n/a\n");
    usb_reply.append("detector=%s refractory=%u calibration_active=%u alarm=%u\nfall=%s help=%s part_failed=%u\n",
        motion_rule.stateName(), unsigned(motion_rule.cooldownActive()), unsigned(gyro_calibration.active()),
        unsigned(fall_response.alarmActive()), fall_delivery.status(), assistance.status(), unsigned(fall_notices.partFailed()));
    char calibration[160]; gyro_calibration.describe(now, calibration, sizeof(calibration));
    usb_reply.append("calibration=%s\nstartup_max_gap_ms=%lu startup_gaps=%lu\n", calibration,
        (unsigned long)motion_health.maxGapMs(), (unsigned long)motion_health.gapCount());
  } else if (!strcmp(command, "health timing")) {
    const auto& d = sensors.motion.diagnostics();
    const auto& t=motion_timing;
    usb_reply.append("service_polls=%llu busy_ms=%llu max_poll_ms=%lu max_service_gap_ms=%lu measured_samples=%llu sample_delay_ms=%llu max_sample_delay_ms=%lu\n",
        (unsigned long long)t.polls,(unsigned long long)t.busy_ms,(unsigned long)t.max_poll_ms,
        (unsigned long)t.max_service_gap_ms,(unsigned long long)t.samples,
        (unsigned long long)t.sample_delay_ms,(unsigned long)t.max_sample_delay_ms);
    usb_reply.append("persistence_calls=%llu persistence_ms=%llu max_persistence_ms=%lu; service counters since first poll, 1ms resolution\n",
        (unsigned long long)t.persistence_calls,(unsigned long long)t.persistence_ms,(unsigned long)t.max_persistence_ms);
    usb_reply.append("fifo_samples=%lu discarded_lower_bound=%lu max_backlog_samples=%lu pending=%u nominal_timestamp_phase_ms=%lu\n",
        (unsigned long)d.samples, (unsigned long)d.discarded_samples, (unsigned long)d.max_fifo_samples,
        sensors.motion.queuedSamples(), (unsigned long)HealthNodeConfig::motion_sample_ms);
    usb_reply.append("nominal_ms=%lu gap_limit_ms=%lu n=%lu intervals=%lu reset=%s coverage_ms=%llu gaps=%lu rejected_nonfinite=%lu\n",
        (unsigned long)HealthNodeConfig::motion_sample_ms, (unsigned long)motion_rule.settings().max_sample_gap_ms,
        (unsigned long)s.samples, (unsigned long)s.interval_count, s.reset_reason,
        (unsigned long long)s.coverage_ms, (unsigned long)s.gaps, (unsigned long)s.rejected);
    if (s.interval_count) usb_reply.append("interval_ms latest=%lu min=%.3f max=%.3f mean=%.3f observed_hz=%.3f\n",
        (unsigned long)s.latest_interval, s.intervals.minimum, s.intervals.maximum, s.intervals.mean,
        s.coverage_ms ? 1000.0 * s.interval_count / double(s.coverage_ms) : 0.0);
    else usb_reply.append("interval_ms=n/a observed_hz=n/a (requires 2 samples)\n");
    usb_reply.append("valid_now=%u startup_max_gap_ms=%lu startup_gaps=%lu\n", unsigned(sensors.motion.valid),
        (unsigned long)motion_health.maxGapMs(), (unsigned long)motion_health.gapCount());
  } else if (!strcmp(command, "health errors")) {
    const auto& d = sensors.motion.diagnostics();
    usb_reply.append("fifo_overflows=%lu fifo_resets=%lu time_discontinuities=%lu saturation_samples=%lu timestamp_adjustments=%lu\n",
        (unsigned long)d.fifo_overflows,(unsigned long)d.fifo_resets,
        (unsigned long)d.timing_discontinuities,(unsigned long)d.saturation_samples,
        (unsigned long)d.timestamp_adjustments);
    const auto& correction = sensors.motion.calibration();
    usb_reply.append("accel_calibration=none; gyro_temperature_delta_C=%.2f calibrated=%u\n",
        sensors.motion.raw_values.temperature-correction.temperature,unsigned(correction.samples!=0));
    usb_reply.append("bus_errors=%lu short_reads=%lu no_data_ready=%lu data_timeouts=%lu\n",
        (unsigned long)d.bus_errors, (unsigned long)d.short_reads, (unsigned long)d.not_ready, (unsigned long)d.data_timeouts);
    usb_reply.append("init_attempts=%lu init_failures=%lu reinitializations=%lu recoveries=%lu config_mismatches=%lu identity_mismatches=%lu\n",
        (unsigned long)d.init_attempts, (unsigned long)d.init_failures, (unsigned long)d.reinitializations,
        (unsigned long)d.recoveries, (unsigned long)d.config_mismatches, (unsigned long)d.identity_mismatches);
  } else if (!strcmp(command, "health radio")) {
    usb_reply.append("freq_MHz=%.3f bw_kHz=%.3f sf=%u cr=%u tx_dBm=%d repeat=%u queued=%u busy=%u\n",
        _prefs.freq, _prefs.bw, unsigned(_prefs.sf), unsigned(_prefs.cr), int(_prefs.tx_power_dbm),
        unsigned(_prefs.isRepeatEn()), unsigned(_mgr->getOutboundTotal()), unsigned(radio_driver.configurationBusy()));
    usb_reply.append("tx_packets=%lu rx_packets=%lu rx_errors=%lu tx_airtime_ms=%lu rx_airtime_ms=%lu\n",
        (unsigned long)radio_driver.getPacketsSent(), (unsigned long)radio_driver.getPacketsRecv(),
        (unsigned long)radio_driver.getPacketsRecvErrors(), getTotalAirTime(), getReceiveAirTime());
    if (radio_driver.getPacketsRecv()) usb_reply.append("last_rssi_dBm=%.1f last_snr_dB=%.1f\n",
        radio_driver.getLastRSSI(), radio_driver.getLastSNR());
    else usb_reply.append("last_rssi_dBm=n/a last_snr_dB=n/a\n");
    usb_reply.append("sent_flood=%lu sent_direct=%lu recv_flood=%lu recv_direct=%lu noise_floor_dBm=%d\n",
        (unsigned long)getNumSentFlood(), (unsigned long)getNumSentDirect(), (unsigned long)getNumRecvFlood(),
        (unsigned long)getNumRecvDirect(), radio_driver.getNoiseFloor());
  } else return false;
  return true;
}
#endif
