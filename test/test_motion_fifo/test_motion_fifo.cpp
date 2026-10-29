#include <gtest/gtest.h>
#define ESP32
#include <helpers/sensors/MPU6050.h>
#include <MpuFifoFake.h>
#include "../../examples/companion_radio/health/MotionRule.h"

static void start(MPU6050Sensor& sensor, uint32_t now = 0) {
  EXPECT_FALSE(sensor.poll(now));
  EXPECT_FALSE(sensor.poll(now + 100));
}

TEST(MotionFifo, ConfigurationEnablesCompleteRecordsOnlyAfterSettling) {
  MpuFifoFake hw; MPU6050Sensor sensor;
  sensor.poll(0); EXPECT_EQ(Wire1.registers[0x6A], 0);
  EXPECT_FALSE(sensor.poll(99)); EXPECT_EQ(hw.resets, 0u);
  EXPECT_FALSE(sensor.poll(119)); EXPECT_EQ(hw.resets, 1u);
  EXPECT_EQ(Wire1.registers[0x23], 0xF8); EXPECT_EQ(Wire1.registers[0x6A], 0x40);
  EXPECT_EQ(Wire1.registers[0x38], 0x11);
  hw.push(); EXPECT_TRUE(sensor.poll(139)); EXPECT_TRUE(sensor.valid);
}

TEST(MotionFifo, BatchesPreserveEveryPeakAndOriginalSpacingAcrossSchedulerStall) {
  MpuFifoFake hw; MPU6050Sensor sensor; start(sensor);
  hw.push(.3f); hw.push(3, 300); hw.push(1); hw.push(1); hw.push(1);
  const float expected[] = {.3f, 3, 1, 1, 1};
  for (unsigned i = 0; i < 5; ++i) {
    ASSERT_TRUE(sensor.poll(200));
    EXPECT_NEAR(sensor.values.acceleration(), expected[i], .001);
    EXPECT_EQ(sensor.sampleTimeMs(), 100u + i*20);
    EXPECT_EQ(sensor.queuedSamples(), 4u - i);
    EXPECT_EQ(sensor.sampleAgeMs(200), 100u - i*20);
  }
  EXPECT_FALSE(sensor.poll(200)); EXPECT_EQ(hw.packets_read, 5u);
  EXPECT_EQ(sensor.diagnostics().samples, 5u);
  EXPECT_EQ(sensor.diagnostics().max_fifo_samples, 5u);
  EXPECT_EQ(sensor.sampleQuality(), MPU6050Sensor::EstimatedTime);
}

TEST(MotionFifo, EmptyPollAndIdenticalValuesDoNotInventOrLoseSamples) {
  MpuFifoFake hw; MPU6050Sensor sensor; start(sensor);
  EXPECT_FALSE(sensor.poll(120)); EXPECT_FALSE(sensor.valid);
  hw.push(); ASSERT_TRUE(sensor.poll(140));
  EXPECT_FALSE(sensor.poll(160)); EXPECT_EQ(sensor.diagnostics().samples, 1u);
  hw.push(); ASSERT_TRUE(sensor.poll(180)); EXPECT_EQ(sensor.diagnostics().samples, 2u);
}

TEST(MotionFifo, JitterAndSamplePhaseStillPreserveOrderAcrossClockWrap) {
  for (uint32_t phase : {0u, 3u, 19u}) {
    MpuFifoFake hw; MPU6050Sensor sensor; const uint32_t origin = UINT32_MAX - 180;
    start(sensor, origin); hw.push(); ASSERT_TRUE(sensor.poll(origin + 120 + phase));
    const uint32_t first = sensor.sampleTimeMs();
    hw.push(.3f); hw.push(3, 300); hw.push();
    ASSERT_TRUE(sensor.poll(origin + 180 + phase)); EXPECT_EQ(sensor.sampleTimeMs(), first + 20);
    ASSERT_TRUE(sensor.poll(origin + 180 + phase)); EXPECT_EQ(sensor.sampleTimeMs(), first + 40);
    ASSERT_TRUE(sensor.poll(origin + 180 + phase)); EXPECT_EQ(sensor.sampleTimeMs(), first + 60);
  }
}

TEST(MotionFifo, CountReadLatencyDoesNotDiscardSamplesCapturedDuringI2cWork) {
  for (const uint32_t origin : {0u, UINT32_MAX - 160}) {
    MpuFifoFake hw; MPU6050Sensor sensor; start(sensor, origin);
    uint32_t elapsed = 100, next_capture = 120;
    unsigned attempt = 1;
    // Sensor production follows elapsed time independently of host polling.
    auto advance = [&](uint32_t target) {
      elapsed = target; g_mock_millis = origin + elapsed;
      while (next_capture <= elapsed) {
        hw.push(next_capture == 140 ? .3f : next_capture == 160 ? 3.0f : 1.0f,
                next_capture == 160 ? 300.0f : 0.0f);
        next_capture += 20;
      }
    };
    const auto response = Wire1.response;
    Wire1.response = [&](uint8_t reg, uint8_t count) {
      if (reg == 0x3A) advance(elapsed + (attempt == 1 ? 1 : 2));
      else if (reg == 0x72) advance(elapsed + (attempt == 1 ? 1 : 3));
      else if (reg == 0x74) advance(elapsed + 1);
      return response(reg, count);
    };

    advance(137);
    ASSERT_TRUE(sensor.poll(origin + 137)); // Count observed at 139 ms.
    EXPECT_EQ(origin + 119, sensor.sampleTimeMs());
    EXPECT_NEAR(1.0f, sensor.values.acceleration(), .001f);
    const auto generation = sensor.discontinuityGeneration();
    attempt = 2; advance(157);
    ASSERT_TRUE(sensor.poll(origin + 157)); // Count observed at 162 ms.
    EXPECT_EQ(origin + 122, sensor.sampleTimeMs());
    EXPECT_NEAR(.3f, sensor.values.acceleration(), .001f);
    EXPECT_EQ(1u, sensor.queuedSamples());
    ASSERT_TRUE(sensor.poll(g_mock_millis));
    EXPECT_EQ(origin + 142, sensor.sampleTimeMs());
    EXPECT_NEAR(3.0f, sensor.values.acceleration(), .001f);
    EXPECT_NEAR(300.0f, sensor.values.rotation(), .001f);
    EXPECT_EQ(0u, sensor.queuedSamples());
    EXPECT_EQ(3u, sensor.diagnostics().samples);
    EXPECT_EQ(3u, hw.packets_read);
    EXPECT_EQ(generation, sensor.discontinuityGeneration());
    EXPECT_EQ(0u, sensor.diagnostics().discarded_samples);
    EXPECT_EQ(0u, sensor.diagnostics().timing_discontinuities);
  }
}

TEST(MotionFifo, CountLatchedBeforeReadCompletesPreservesOrderAcrossEveryBatch) {
  for (const uint32_t origin : {0u, UINT32_MAX - 160}) {
    for (const uint32_t second_poll : {157u, 217u}) {
      MpuFifoFake hw; MPU6050Sensor sensor; start(sensor, origin);
      uint32_t elapsed = 100, next_capture = 120;
      unsigned attempt = 1;
      auto advance = [&](uint32_t target) {
        elapsed = target; g_mock_millis = origin + elapsed;
        while (next_capture <= elapsed) {
          hw.push(next_capture == 140 ? .3f : next_capture == 160 ? 3.0f : 1.0f,
                  next_capture == 160 ? 300.0f : 0.0f);
          next_capture += 20;
        }
      };
      const auto response = Wire1.response;
      Wire1.response = [&](uint8_t reg, uint8_t count) {
        if (reg == 0x3A) advance(elapsed + (attempt == 1 ? 1 : 3));
        if (reg == 0x72) {
          // FIFO_COUNT is latched before the remainder of the bus transaction.
          const auto bytes = response(reg, count);
          advance(elapsed + (attempt == 1 ? 3 : 1));
          return bytes;
        }
        if (reg == 0x74) advance(elapsed + 1);
        return response(reg, count);
      };

      advance(137);
      ASSERT_TRUE(sensor.poll(origin + 137)); // Latched at 138, complete at 141.
      EXPECT_EQ(origin + 121, sensor.sampleTimeMs());
      EXPECT_EQ(MPU6050Sensor::EstimatedTime, sensor.sampleQuality());
      const auto generation = sensor.discontinuityGeneration();
      attempt = 2; advance(second_poll);
      const unsigned records = (second_poll + 3 - 120) / 20;
      const uint32_t snapshot_at = origin + second_poll + 4;
      for (unsigned i = 0; i < records; ++i) {
        ASSERT_TRUE(sensor.poll(g_mock_millis));
        EXPECT_EQ(origin + 122 + i * 20, sensor.sampleTimeMs());
        EXPECT_LE(int32_t(sensor.sampleTimeMs() - snapshot_at), 0);
        EXPECT_TRUE(sensor.sampleQuality() & MPU6050Sensor::EstimatedTimeAdjusted);
        EXPECT_NEAR(i == 0 ? .3f : i == 1 ? 3.0f : 1.0f, sensor.values.acceleration(), .001f);
      }
      EXPECT_EQ(0u, sensor.queuedSamples());
      EXPECT_EQ(records + 1, hw.packets_read);
      EXPECT_EQ(generation, sensor.discontinuityGeneration());
      EXPECT_EQ(1u, sensor.diagnostics().timestamp_adjustments);
      EXPECT_EQ(0u, sensor.diagnostics().discarded_samples);
      EXPECT_EQ(0u, sensor.diagnostics().timing_discontinuities);

      // A later unambiguous count snapshot does not inherit the adjustment flag.
      advance(second_poll + 40);
      ASSERT_TRUE(sensor.poll(g_mock_millis));
      EXPECT_FALSE(sensor.sampleQuality() & MPU6050Sensor::EstimatedTimeAdjusted);
      EXPECT_EQ(1u, sensor.diagnostics().timestamp_adjustments);
    }
  }
}

TEST(MotionFifo, EstimateCorrectionIsBoundedAndCannotCreateFutureSamplesOrHideLongGaps) {
  for (const uint32_t origin : {0u, UINT32_MAX - 180}) {
    for (const int32_t separation : {0, -1, -19, -20, -21, 100, 101}) {
      MpuFifoFake hw; MPU6050Sensor sensor; start(sensor, origin);
      hw.push(); ASSERT_TRUE(sensor.poll(origin + 120));
      const auto generation = sensor.discontinuityGeneration();
      for (unsigned i = 0; i < 5; ++i) hw.push();
      const uint32_t now = origin + 200 + separation;
      const bool acceptable = separation > -20 && separation <= 100;
      EXPECT_EQ(acceptable, sensor.poll(now));
      if (!acceptable) {
        EXPECT_FALSE(sensor.valid);
        EXPECT_NE(generation, sensor.discontinuityGeneration());
        EXPECT_EQ(5u, sensor.diagnostics().discarded_samples);
        EXPECT_EQ(1u, sensor.diagnostics().timing_discontinuities);
        EXPECT_EQ(0u, sensor.diagnostics().timestamp_adjustments);
        continue;
      }
      const bool adjusted = separation <= 0;
      const uint32_t first = adjusted ? origin + 101 : origin + 100 + separation;
      EXPECT_EQ(adjusted ? 1u : 0u, sensor.diagnostics().timestamp_adjustments);
      // Diagnostic resets must not erase the active snapshot's quality flag.
      sensor.resetDiagnostics();
      for (unsigned i = 0; i < 5; ++i) {
        if (i) ASSERT_TRUE(sensor.poll(now));
        EXPECT_EQ(first + i * 20, sensor.sampleTimeMs());
        EXPECT_LE(int32_t(sensor.sampleTimeMs() - now), 0);
        EXPECT_EQ(adjusted, bool(sensor.sampleQuality() & MPU6050Sensor::EstimatedTimeAdjusted));
      }
      EXPECT_EQ(generation, sensor.discontinuityGeneration());
      EXPECT_EQ(0u, sensor.diagnostics().timestamp_adjustments);
    }
  }
}

TEST(MotionFifo, OverflowDiscardsDataAndSignalsDiscontinuityBeforeRecovery) {
  MpuFifoFake hw; MPU6050Sensor sensor; start(sensor);
  hw.push(); ASSERT_TRUE(sensor.poll(120)); const auto generation = sensor.discontinuityGeneration();
  for (unsigned i = 0; i < 74; ++i) hw.push();
  EXPECT_FALSE(sensor.poll(140)); EXPECT_FALSE(sensor.valid); EXPECT_TRUE(hw.fifo.empty());
  EXPECT_GT(sensor.discontinuityGeneration(), generation); EXPECT_EQ(sensor.diagnostics().fifo_overflows, 1u);
  hw.push(); ASSERT_TRUE(sensor.poll(160)); EXPECT_EQ(sensor.sampleTimeMs(), 140u);
}

TEST(MotionFifo, OverflowDuringBurstNeverPublishesCorruptedBatch) {
  MpuFifoFake hw; MPU6050Sensor sensor; start(sensor); hw.push(); hw.push();
  hw.overflow_after_read = true;
  EXPECT_FALSE(sensor.poll(140)); EXPECT_FALSE(sensor.valid); EXPECT_EQ(sensor.diagnostics().samples, 0u);
  EXPECT_EQ(sensor.queuedSamples(), 0u);
}

TEST(MotionFifo, PartialBurstInvalidatesWholeBatchAndResetsPacketAlignment) {
  for (int bytes = 0; bytes < 28; ++bytes) {
    MpuFifoFake hw; MPU6050Sensor sensor; start(sensor); hw.push(.3f); hw.push(3, 300);
    const auto generation = sensor.discontinuityGeneration(); hw.short_bytes = bytes;
    EXPECT_FALSE(sensor.poll(140)); EXPECT_FALSE(sensor.valid);
    EXPECT_GT(sensor.discontinuityGeneration(), generation);
    EXPECT_EQ(sensor.diagnostics().samples, 0u); EXPECT_EQ(sensor.queuedSamples(), 0u);
    hw.short_bytes = -1; EXPECT_FALSE(sensor.poll(160)); EXPECT_TRUE(hw.fifo.empty());
    hw.push(); ASSERT_TRUE(sensor.poll(180)); EXPECT_FLOAT_EQ(sensor.values.az, 1);
  }
}

TEST(MotionFifo, LongStallCannotRetimestampOldSamplesAsCurrentEvidence) {
  MpuFifoFake hw; MPU6050Sensor sensor; start(sensor); hw.push(); ASSERT_TRUE(sensor.poll(120));
  hw.push(.3f); EXPECT_FALSE(sensor.poll(2120)); EXPECT_FALSE(sensor.valid);
  EXPECT_EQ(sensor.diagnostics().timing_discontinuities, 1u);
  EXPECT_EQ(sensor.diagnostics().discarded_samples, 1u);
}

TEST(MotionFifo, QueuedSoftwareRecordsExpireInsteadOfClearingAnOutage) {
  MpuFifoFake hw; MPU6050Sensor sensor; start(sensor); hw.push(); hw.push();
  ASSERT_TRUE(sensor.poll(140)); EXPECT_EQ(sensor.queuedSamples(), 1u);
  EXPECT_FALSE(sensor.poll(2140)); EXPECT_FALSE(sensor.valid); EXPECT_EQ(sensor.queuedSamples(), 0u);
}

TEST(MotionFifo, SaturationIsVisibleAndRawCorrectionAndGravityStayIndependent) {
  MpuFifoFake hw; MPU6050Sensor sensor; start(sensor);
  hw.pushRaw({{32767, 0, 4096, 0, -32768, 131, 0}});
  ASSERT_TRUE(sensor.poll(120));
  EXPECT_TRUE(sensor.sampleQuality() & MPU6050Sensor::AccelSaturated);
  EXPECT_TRUE(sensor.sampleQuality() & MPU6050Sensor::GyroSaturated);
  EXPECT_FLOAT_EQ(sensor.raw_values.gy, 2); EXPECT_FLOAT_EQ(sensor.values.az, 1);
  const auto generation = sensor.discontinuityGeneration(); sensor.resetDiagnostics();
  EXPECT_EQ(sensor.discontinuityGeneration(), generation); EXPECT_TRUE(sensor.valid);
}

TEST(MotionFifo, IncompleteTrailingPacketRemainsUntilItIsComplete) {
  MpuFifoFake hw; MPU6050Sensor sensor; start(sensor);
  hw.push(); hw.fifo.push_back(0); hw.fifo.push_back(0);
  ASSERT_TRUE(sensor.poll(120)); EXPECT_EQ(hw.fifo.size(), 2u);
  EXPECT_FALSE(sensor.poll(140)); EXPECT_EQ(hw.fifo.size(), 2u);
  EXPECT_EQ(sensor.diagnostics().samples, 1u);
}

TEST(MotionFifo, LossCannotJoinOldTroughToImpactAndWalkingRecoveryAcceptsNewEvent) {
  MpuFifoFake hw; MPU6050Sensor sensor; MotionRule rule;
  start(sensor); uint32_t generation = sensor.discontinuityGeneration(), now = 100;
  unsigned falls = 0;
  auto feed = [&](float a, float gyro) {
    now += 20; hw.push(a, gyro);
    const bool fresh = sensor.poll(now);
    if (generation != sensor.discontinuityGeneration() || !sensor.valid) {
      generation = sensor.discontinuityGeneration(); rule.update(now, 0, 0, false);
    }
    if (fresh && rule.update(sensor.sampleTimeMs(), sensor.values.acceleration(), sensor.values.rotation(), true)) ++falls;
  };
  for (unsigned i = 0; i < 20; ++i) feed(1.4f, 60); // Moving throughout recovery.
  ASSERT_TRUE(rule.available()); feed(.3f, 0);
  hw.short_bytes = 0; feed(3, 300); EXPECT_EQ(falls, 0u);
  hw.short_bytes = -1;
  for (unsigned i = 0; i < 20; ++i) feed(1.4f, 60);
  ASSERT_TRUE(rule.available()); feed(3, 300); EXPECT_EQ(falls, 0u);
  feed(.3f, 0); feed(3, 300); EXPECT_EQ(falls, 1u);
}

TEST(MotionFifo, CompleteBufferedStrongSequenceIsDetectedBeforeLaterDataLoss) {
  MpuFifoFake hw; MPU6050Sensor sensor; MotionRule rule; start(sensor);
  for (uint32_t now = 120; now <= 500; now += 20) {
    hw.push(); ASSERT_TRUE(sensor.poll(now));
    rule.update(sensor.sampleTimeMs(), sensor.values.acceleration(), sensor.values.rotation(), true);
  }
  hw.push(.3f); hw.push(3, 300); hw.push(1.5f, 40);
  unsigned falls = 0;
  while (sensor.poll(560)) {
    if (rule.update(sensor.sampleTimeMs(), sensor.values.acceleration(), sensor.values.rotation(), true)) ++falls;
    if (!sensor.queuedSamples()) break;
  }
  ASSERT_EQ(falls, 1u); // No stillness interval was supplied.
  hw.push(); hw.short_bytes = 0; EXPECT_FALSE(sensor.poll(580));
  EXPECT_FALSE(rule.update(580, 0, 0, false)); EXPECT_EQ(falls, 1u);
}

// Sample a time-defined synthetic motion independently of uneven host polling.
// This distinguishes sensor sampling phase from mere polling timestamp offsets.
static unsigned replayPhase(unsigned phase, unsigned trough_duration) {
  MpuFifoFake hw;MPU6050Sensor sensor;MotionRule rule;start(sensor);
  uint32_t generation=sensor.discontinuityGeneration();unsigned alerts=0;
  for(uint32_t wall=101;wall<=1200;++wall) {
    if(wall>=120+phase && (wall-120-phase)%20==0) {
      const bool low=wall>=500 && wall<500+trough_duration;
      const bool impact=wall>=560 && wall<620;
      hw.push(low?.3f:(impact?3.0f:1.0f),impact?300.0f:0.0f);
    }
    if(wall%37 && wall%113)continue;
    for(unsigned budget=0;budget<4;++budget) {
      const bool fresh=sensor.poll(wall);
      if(generation!=sensor.discontinuityGeneration() || !sensor.valid) {
        generation=sensor.discontinuityGeneration();rule.update(wall,0,0,false);
      }
      if(!fresh)break;
      if(rule.update(sensor.sampleTimeMs(),sensor.values.acceleration(),sensor.values.rotation(),true))++alerts;
      if(!sensor.queuedSamples())break;
    }
  }
  EXPECT_EQ(0u,sensor.diagnostics().fifo_overflows);
  return alerts;
}
TEST(MotionFifo, AcquisitionPhaseAndUnevenPollingPreserveSufficientlyWideEvents) {
  for(unsigned phase:{0u,3u,10u,19u})EXPECT_EQ(1u,replayPhase(phase,50));
}
TEST(MotionFifo, SubPeriodTroughCanStillBeMissedDespitePerfectFifoDelivery) {
  EXPECT_EQ(1u,replayPhase(0,5));
  EXPECT_EQ(0u,replayPhase(10,5)); // Explicit 50 Hz limit, not a claimed detector improvement.
}

int main(int argc, char** argv) { ::testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }
