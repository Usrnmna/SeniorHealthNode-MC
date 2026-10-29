#include <gtest/gtest.h>
#include <deque>
#include <string>
#include <functional>
#include "../../examples/companion_radio/health/BoundedUsbOutput.h"
#include "../../examples/companion_radio/health/MotionRecorder.h"
#include "../../examples/companion_radio/UsbReply.h"

namespace {
struct Queue {
  static constexpr unsigned capacity = 8;
  bool initialized = true;
  unsigned send_calls = 0;
  std::function<void()> before_send;
  std::deque<BoundedUsbPacket> packets;
  bool ready() const { return initialized; }
  unsigned spaces() const { return initialized ? capacity - unsigned(packets.size()) : 0; }
  unsigned count() const { return unsigned(packets.size()); }
  uint32_t stackHighWaterBytes() const { return 1234; }
  bool trySend(const BoundedUsbPacket& packet) {
    ++send_calls;
    if (before_send) { auto callback=std::move(before_send);before_send=nullptr;callback(); }
    if (!spaces()) return false;
    packets.push_back(packet); return true;
  }
  BoundedUsbPacket pop() { auto packet = packets.front(); packets.pop_front(); return packet; }
};
struct Usb {
  unsigned writes = 0;
  size_t maximum = 32;
  std::string bytes;
  int availableForWrite() const { return 32; }
  size_t write(const uint8_t* data, size_t size) {
    ++writes; size = size < maximum ? size : maximum;
    bytes.append(reinterpret_cast<const char*>(data), size); return size;
  }
};

TEST(BoundedUsbOutput, ProducerNeverTouchesUsbAndAdmitsOnlyWholeMessages) {
  Queue queue; Usb usb; BoundedUsbOutput<Queue> output(queue);
  uint8_t message[32] = {};
  for (unsigned i = 0; i < queue.capacity; ++i) {
    EXPECT_EQ(32, output.availableForWrite()); EXPECT_EQ(32u, output.write(message, sizeof(message)));
  }
  EXPECT_EQ(0, output.availableForWrite()); EXPECT_EQ(0u, output.write(message, sizeof(message)));
  EXPECT_EQ(0u, usb.writes); EXPECT_EQ(8u, output.queuedPackets()); EXPECT_EQ(1u, output.rejectedWrites());
  EXPECT_EQ(8u, output.acceptedPackets()); EXPECT_EQ(8u, output.pendingPackets());
  output.deliver(queue.pop(), usb);
  EXPECT_EQ(1u, usb.writes); EXPECT_EQ(1u, output.completedPackets()); EXPECT_EQ(0u, output.failedWrites());
  EXPECT_EQ(7u, output.pendingPackets());
}

TEST(BoundedUsbOutput, DebugFormattingIsBoundedAndNeverWaitsForUsb) {
  Queue queue;Usb usb;BoundedUsbOutput<Queue> output(queue);
  const std::string oversized(300,'x');
  output.printf("BLE: %s",oversized.c_str());
  EXPECT_EQ(3u,queue.send_calls);EXPECT_EQ(3u,output.pendingPackets());EXPECT_EQ(0u,usb.writes);
  while(queue.count()) output.deliver(queue.pop(),usb);
  EXPECT_EQ(95u,usb.bytes.size());EXPECT_EQ("BLE: ",usb.bytes.substr(0,5));
  for(unsigned i=0;i<8;++i) output.printf("full");
  const auto attempts=queue.send_calls;
  output.printf("BLE: %s",oversized.c_str());
  EXPECT_EQ(attempts+1,queue.send_calls);EXPECT_EQ(1u,output.rejectedWrites());
}

TEST(BoundedUsbOutput, FailedInitializationAndOversizedInputHaveNoOutputSideEffects) {
  Queue queue; queue.initialized = false; BoundedUsbOutput<Queue> output(queue);
  uint8_t message[33] = {};
  EXPECT_FALSE(output.ready());
  EXPECT_EQ(0, output.availableForWrite()); EXPECT_EQ(0u, output.write(message, 32));
  queue.initialized = true; EXPECT_TRUE(output.ready());
  EXPECT_EQ(0u, output.write(message, 33)); EXPECT_EQ(0u, output.write(nullptr, 1));
  EXPECT_EQ(0u, output.write(nullptr, 0));
  EXPECT_EQ(3u, output.rejectedWrites()); EXPECT_EQ(1u, queue.send_calls); EXPECT_EQ(0u, queue.count());
  EXPECT_EQ(0u, output.acceptedPackets()); EXPECT_EQ(0u, output.pendingPackets());
}

TEST(BoundedUsbOutput, DequeuedWriteRemainsPendingUntilTheWorkerCallCompletes) {
  Queue queue; BoundedUsbOutput<Queue> output(queue);
  struct SlowUsb {
    BoundedUsbOutput<Queue>& output;
    size_t write(const uint8_t*, size_t size) {
      EXPECT_EQ(0u, output.queuedPackets());
      EXPECT_EQ(1u, output.pendingPackets());
      EXPECT_EQ(0u, output.completedPackets());
      return size;
    }
  } usb{output};
  const uint8_t end[] = {12};
  ASSERT_EQ(1u, output.write(end, sizeof(end)));
  auto in_flight = queue.pop(); EXPECT_EQ(1u, output.pendingPackets());
  output.deliver(in_flight, usb);
  EXPECT_EQ(0u, output.pendingPackets()); EXPECT_EQ(1u, output.completedPackets());
}

TEST(BoundedUsbOutput, WorkerShortWritesAreCountedWithoutRetryingOrDuplicatingFrames) {
  Queue queue; Usb usb; BoundedUsbOutput<Queue> output(queue);
  const uint8_t message[] = {'a', 'b', 'c'};
  ASSERT_EQ(3u, output.write(message, 3)); usb.maximum = 1;
  output.deliver(queue.pop(), usb);
  EXPECT_EQ("a", usb.bytes); EXPECT_EQ(1u, usb.writes); EXPECT_EQ(1u, output.failedWrites());
  EXPECT_EQ(0u, queue.count()); EXPECT_EQ(1u, output.completedPackets());
  ASSERT_EQ(3u, output.write(message, 3)); usb.maximum = 0;
  output.deliver(queue.pop(), usb);
  EXPECT_EQ(2u, output.failedWrites()); EXPECT_EQ("a", usb.bytes);
}

TEST(BoundedUsbOutput, QueuedRecordsRetainPayloadAndOrderAcrossDelayedConsumption) {
  Queue queue; Usb usb; BoundedUsbOutput<Queue> output(queue);
  uint8_t message[2] = {'a', '0'};
  for (unsigned n = 0; n < queue.capacity; ++n) {
    message[1] = uint8_t('0' + n); ASSERT_EQ(2u, output.write(message, 2));
  }
  memset(message, 0, sizeof(message));
  while (queue.count()) output.deliver(queue.pop(), usb);
  EXPECT_EQ("a0a1a2a3a4a5a6a7", usb.bytes);
  EXPECT_EQ(8u, output.completedPackets()); EXPECT_EQ(1234u, output.workerStackHighWaterBytes());
}

TEST(BoundedUsbOutput, StalledWorkerCannotMakeRecorderOrReplyDrainCallUsb) {
  Queue queue; Usb usb; BoundedUsbOutput<Queue> output(queue);
  MotionRecorder recorder; UsbReply reply; uint8_t sample[100] = {};
  ASSERT_TRUE(recorder.begin(sample, sizeof(sample)));
  reply.append("recording started\n");
  for (unsigned n = 0; n < 200; ++n) {
    recorder.append(2, sample, sizeof(sample)); recorder.drain(output); reply.drain(output);
  }
  EXPECT_EQ(0u, usb.writes); EXPECT_EQ(8u, output.queuedPackets());
  EXPECT_GT(recorder.droppedRecords(), 0u); EXPECT_LE(recorder.bufferedBytes(), recorder.capacity);
  EXPECT_EQ(0u, output.failedWrites());
}

TEST(BoundedUsbOutput, CompleteRecordingFramesSurviveWorkerQueueWithInterleavedReplyText) {
  Queue queue; Usb usb; BoundedUsbOutput<Queue> output(queue);
  MotionRecorder recorder; UsbReply reply; uint8_t payload[] = {1, 2, 3};
  ASSERT_TRUE(recorder.begin(payload, sizeof(payload))); ASSERT_TRUE(recorder.stop(nullptr, 0));
  reply.append("status\n"); reply.drain(output);
  while (recorder.pending() || queue.count()) {
    recorder.drain(output);
    if (queue.count()) output.deliver(queue.pop(), usb);
  }
  EXPECT_EQ("status\n", usb.bytes.substr(0, 7));
  EXPECT_EQ('\x1e', usb.bytes[7]); EXPECT_EQ('\n', usb.bytes[37]);
  EXPECT_EQ('\x1e', usb.bytes[38]); EXPECT_EQ('\n', usb.bytes[68]);
  EXPECT_EQ(0u, output.failedWrites()); EXPECT_EQ(0u, recorder.shortWrites());
}

TEST(BoundedUsbOutput, ConcurrentDebugAdmissionDoesNotDiscardAnUnwrittenRecordingFrame) {
  Queue queue;Usb usb;BoundedUsbOutput<Queue> output(queue);
  MotionRecorder recorder;uint8_t payload[]={1,2,3};
  ASSERT_TRUE(recorder.begin(payload,sizeof(payload)));ASSERT_TRUE(recorder.stop(nullptr,0));
  for(unsigned i=0;i<7;++i) output.printf("queued text\n");
  ASSERT_EQ(32,output.availableForWrite());
  // A BLE callback wins the final queue slot after the recorder's readiness check.
  queue.before_send=[&output] { output.printf("concurrent text\n"); };
  recorder.drain(output);
  EXPECT_EQ(1u,output.rejectedWrites());EXPECT_EQ(8u,output.pendingPackets());
  EXPECT_EQ(0u,recorder.shortWrites());EXPECT_EQ(0u,recorder.exportedPackets());
  while(recorder.pending() || queue.count()) {
    if(queue.count()) output.deliver(queue.pop(),usb);
    recorder.drain(output);
  }
  const auto first=usb.bytes.find('\x1e');ASSERT_NE(std::string::npos,first);
  const auto capture=usb.bytes.substr(first);
  ASSERT_EQ(62u,capture.size());EXPECT_EQ('S',capture[1]);EXPECT_EQ("0000",capture.substr(2,4));
  EXPECT_EQ('S',capture[32]);EXPECT_EQ("0001",capture.substr(33,4));
  EXPECT_EQ(0u,output.pendingPackets());
}

TEST(BoundedUsbOutput, AppendedRepliesReuseConsumedCapacityWithoutDiscardingPendingText) {
  UsbReply reply;Usb usb;const std::string original(2000,'a'),next(100,'b');
  reply.append("%s",original.c_str());
  for(unsigned i=0;i<62;++i) reply.drain(usb);
  ASSERT_EQ(1984u,usb.bytes.size());ASSERT_TRUE(reply.pending());
  reply.append("%s",next.c_str());
  while(reply.pending()) reply.drain(usb);
  EXPECT_EQ(original+next,usb.bytes);
}
}
int main(int argc, char** argv) { ::testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }
