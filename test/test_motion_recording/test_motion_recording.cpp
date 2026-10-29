#include <gtest/gtest.h>
#include <vector>
#include <string>
#include "../../examples/companion_radio/health/MotionRecorder.h"

namespace {
struct Consumer {
  int room = 64; size_t limit = 1000, calls = 0;
  std::string output;
  int availableForWrite() const { return room; }
  size_t write(const uint8_t* bytes, size_t size) {
    ++calls; EXPECT_LE(size,size_t(room)); EXPECT_EQ(31u,size);
    size_t n=size<limit?size:limit; output.append(reinterpret_cast<const char*>(bytes),n);return n;
  }
};
void finish(MotionRecorder& r, Consumer& s) {
  for (unsigned i=0;i<2000 && r.pending();++i) r.drain(s);
  ASSERT_FALSE(r.pending());
}
TEST(MotionRecorder, DisabledByDefaultAndStartStopAreExplicit) {
  MotionRecorder r;Consumer s;uint8_t p[]={1,2,3};
  EXPECT_FALSE(r.active());EXPECT_FALSE(r.append(2,p,3));r.drain(s);EXPECT_EQ(0u,s.calls);
  ASSERT_TRUE(r.begin(p,3));EXPECT_FALSE(r.begin(p,3));
  ASSERT_TRUE(r.stop(p,3));EXPECT_FALSE(r.active());EXPECT_TRUE(r.stopping());
  EXPECT_FALSE(r.append(2,p,3));EXPECT_FALSE(r.begin(p,3));finish(r,s);
  EXPECT_EQ(2u,r.issuedRecords());EXPECT_TRUE(r.begin(p,3));
}
TEST(MotionRecorder, EveryFrameIsAtomicChecksummedAndZeroPadded) {
  MotionRecorder r;Consumer s;uint8_t p[]={1,2,3,4};
  ASSERT_TRUE(r.begin(p,4));ASSERT_TRUE(r.stop(nullptr,0));finish(r,s);
  ASSERT_EQ(93u,s.output.size());
  const std::string first=s.output.substr(0,31);
  EXPECT_EQ('\x1e',first[0]);EXPECT_EQ('S',first[1]);EXPECT_EQ('\n',first[30]);
  EXPECT_EQ("01040000000000010203",first.substr(6,20));
  EXPECT_EQ(MotionRecorder::crc16(reinterpret_cast<const uint8_t*>(first.data()+1),25),
            std::stoul(first.substr(26,4),nullptr,16));
  EXPECT_EQ('M',s.output[32]);EXPECT_EQ("04000000000000000000",s.output.substr(37,20));
  EXPECT_EQ('S',s.output[63]);
}
TEST(MotionRecorder, DisconnectedConsumerDropsWholeRecordsWithoutWaiting) {
  MotionRecorder r;Consumer s;s.room=0;uint8_t p[110]={};
  ASSERT_TRUE(r.begin(p,110));
  for(unsigned i=0;i<2000;++i){r.append(2,p,110);r.drain(s);}
  EXPECT_EQ(0u,s.calls);EXPECT_EQ(2001u,r.issuedRecords());
  EXPECT_GT(r.droppedRecords(),1900u);EXPECT_LE(r.bufferedBytes(),r.capacity);
  const auto size=r.bufferedBytes();s.room=30;r.drain(s);EXPECT_EQ(size,r.bufferedBytes());
  ASSERT_TRUE(r.stop(p,20));s.room=31;finish(r,s);
  EXPECT_EQ(2002u,r.issuedRecords());EXPECT_GT(s.calls,0u);
}
TEST(MotionRecorder, ShortWriteIsCountedAndNeverRetriedAsACompleteFrame) {
  MotionRecorder r;Consumer s;uint8_t p[40]={};
  ASSERT_TRUE(r.begin(p,40));s.limit=5;r.drain(s);
  EXPECT_EQ(1u,r.shortWrites());EXPECT_EQ(1u,r.exportedPackets());
  s.limit=1000;r.drain(s);EXPECT_EQ(2u,r.exportedPackets());
  EXPECT_EQ('M',s.output[6]); // New packet follows the truncated first frame.
}
TEST(MotionRecorder, ZeroByteRejectionRetainsStartAndContinuationForLaterRetry) {
  MotionRecorder reference,retried;Consumer fast,slow;uint8_t p[23]={1,2,3};
  ASSERT_TRUE(reference.begin(p,sizeof(p)));ASSERT_TRUE(reference.stop(p,3));finish(reference,fast);
  ASSERT_TRUE(retried.begin(p,sizeof(p)));ASSERT_TRUE(retried.stop(p,3));
  const auto queued=retried.bufferedBytes();slow.limit=0;retried.drain(slow);
  EXPECT_EQ(0u,retried.shortWrites());EXPECT_EQ(0u,retried.exportedPackets());
  // Stop may append its deferred End, but a rejected write consumes no record bytes.
  EXPECT_GE(retried.bufferedBytes(),queued);EXPECT_TRUE(slow.output.empty());
  slow.limit=1000;retried.drain(slow);ASSERT_EQ(31u,slow.output.size());
  const auto remaining=retried.bufferedBytes();slow.limit=0;retried.drain(slow);
  EXPECT_EQ(remaining,retried.bufferedBytes());EXPECT_EQ(1u,retried.exportedPackets());
  EXPECT_EQ(0u,retried.shortWrites());slow.limit=1000;finish(retried,slow);
  EXPECT_EQ(fast.output,slow.output);
}
TEST(MotionRecorder, BackpressureDoesNotChangeAlreadyQueuedPayloads) {
  MotionRecorder a,b;Consumer fast,slow;uint8_t begin[8]={};
  ASSERT_TRUE(a.begin(begin,8));ASSERT_TRUE(b.begin(begin,8));
  for(unsigned n=0;n<40;++n) {
    uint8_t p[30];for(unsigned i=0;i<30;++i)p[i]=uint8_t(n+i);
    ASSERT_TRUE(a.append(2,p,30));ASSERT_TRUE(b.append(2,p,30));
    a.drain(fast);slow.room=n%3?0:64;b.drain(slow);
  }
  ASSERT_TRUE(a.stop(begin,8));ASSERT_TRUE(b.stop(begin,8));slow.room=64;
  finish(a,fast);finish(b,slow);EXPECT_EQ(fast.output,slow.output);
}
TEST(MotionRecorder, RingWrapRetainsCompleteRecordOrder) {
  MotionRecorder r;Consumer s;uint8_t p[100]={};ASSERT_TRUE(r.begin(p,100));
  for(unsigned n=0;n<300;++n) {
    p[0]=uint8_t(n);ASSERT_TRUE(r.append(2,p,100));
    for(unsigned i=0;i<22;++i)r.drain(s);
  }
  ASSERT_TRUE(r.stop(p,8));finish(r,s);EXPECT_EQ(0u,r.droppedRecords());EXPECT_EQ(302u,r.issuedRecords());
}
}
int main(int argc,char**argv){::testing::InitGoogleTest(&argc,argv);return RUN_ALL_TESTS();}
