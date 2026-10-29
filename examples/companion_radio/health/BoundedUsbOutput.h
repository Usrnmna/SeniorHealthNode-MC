#pragma once
#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

// One queue message is one complete recording frame or a bounded text fragment.
// The producer never calls USB methods: even an availableForWrite() query can
// wait on the ESP32 HWCDC mutex. Only the low-priority worker touches USB output.
struct BoundedUsbPacket {
  static constexpr size_t capacity = 32;
  uint8_t length = 0;
  uint8_t bytes[capacity] = {};
};

// Queue supplies immediate ready/spaces/count/trySend operations. Keeping the producer
// independent of FreeRTOS lets host tests exercise the exact enqueue and worker
// delivery behavior without substituting a blocking Serial implementation.
template<class Queue> class BoundedUsbOutput {
public:
  explicit BoundedUsbOutput(Queue& backend) : queue(backend) {}
  bool ready() const { return queue.ready(); }
  int availableForWrite() const { return queue.spaces() ? int(BoundedUsbPacket::capacity) : 0; }
  // Debug text is best-effort, at most 95 bytes and three queue attempts. This
  // sink is also safe for BLE callbacks; it never enters the USB driver's mutex.
  void vprintf(const char* format, va_list args) {
    char text[96];
    const int count=vsnprintf(text,sizeof(text),format,args);
    if (count<=0) return;
    const size_t length=size_t(count)<sizeof(text) ? size_t(count) : sizeof(text)-1;
    for (size_t at=0;at<length;at+=BoundedUsbPacket::capacity) {
      const size_t remaining=length-at;
      const size_t part=remaining<BoundedUsbPacket::capacity ? remaining : BoundedUsbPacket::capacity;
      if (write(reinterpret_cast<const uint8_t*>(text+at),part)!=part) break;
    }
  }
  void printf(const char* format, ...) {
    va_list args;va_start(args,format);vprintf(format,args);va_end(args);
  }
  size_t write(const uint8_t* bytes, size_t length) {
    if (!length) return 0;
    if (!bytes || length > BoundedUsbPacket::capacity) {
      rejected.fetch_add(1, std::memory_order_relaxed); return 0;
    }
    BoundedUsbPacket packet;
    packet.length = uint8_t(length); memcpy(packet.bytes, bytes, length);
    // Reserve before publishing: a worker may finish immediately after enqueue.
    // Rolling back a failed enqueue cannot lose an already accepted packet.
    accepted.fetch_add(1, std::memory_order_seq_cst);
    if (!queue.trySend(packet)) {
      accepted.fetch_sub(1, std::memory_order_seq_cst);
      rejected.fetch_add(1, std::memory_order_relaxed); return 0;
    }
    return length;
  }
  unsigned queuedPackets() const { return queue.count(); }
  uint32_t acceptedPackets() const { return accepted.load(std::memory_order_seq_cst); }
  // Includes the worker's in-flight write, which has already left the queue.
  // Completion is read first so a concurrent completion cannot underflow a
  // fresh accepted count. Wrap-safe subtraction is valid for this small queue.
  uint32_t pendingPackets() const {
    const uint32_t done = completed.load(std::memory_order_seq_cst);
    const uint32_t pending = accepted.load(std::memory_order_seq_cst) - done;
    return pending < 0x80000000u ? pending : 0;
  }
  uint32_t failedWrites() const { return failed.load(std::memory_order_relaxed); }
  uint32_t rejectedWrites() const { return rejected.load(std::memory_order_relaxed); }
  uint32_t completedPackets() const { return completed.load(std::memory_order_relaxed); }
  uint32_t workerStackHighWaterBytes() const { return queue.stackHighWaterBytes(); }

  // Worker-only operation. A short USB write is counted and never retried, so
  // truncated recording frames remain detectable rather than being duplicated.
  // Completion describes the USB API result, not receipt by the host consumer.
  template<class Stream> void deliver(const BoundedUsbPacket& packet, Stream& stream) {
    const size_t written = stream.write(packet.bytes, packet.length);
    if (written != packet.length) failed.fetch_add(1, std::memory_order_relaxed);
    completed.fetch_add(1, std::memory_order_seq_cst);
  }
private:
  Queue& queue;
  std::atomic<uint32_t> failed{0}, rejected{0}, accepted{0}, completed{0};
};

#if defined(ESP32) && !defined(HEALTH_MONITOR_HOST_HEADER)
// The single companion owns this fixed queue/task. No additional firmware or
// process is required. Queue and task allocations remain static for node life.
class UsbOutputQueue {
public:
  static constexpr unsigned queue_capacity = 8;
  static constexpr unsigned worker_stack_bytes = 3072;
  bool begin();
  bool ready() const;
  unsigned spaces() const;
  unsigned count() const;
  bool trySend(const BoundedUsbPacket& packet);
  uint32_t stackHighWaterBytes() const;
};
extern BoundedUsbOutput<UsbOutputQueue> bounded_usb_output;
bool beginBoundedUsbOutput();
#endif
