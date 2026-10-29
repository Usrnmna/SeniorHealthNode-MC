#if defined(ESP32) && ENV_INCLUDE_MPU6050
#include "health/BoundedUsbOutput.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <helpers/HealthUsbDebug.h>

namespace {
StaticQueue_t queue_control;
uint8_t queue_storage[UsbOutputQueue::queue_capacity * sizeof(BoundedUsbPacket)];
StaticTask_t worker_control;
// ESP-IDF specifies xTaskCreateStatic stack depth in bytes, unlike vanilla
// FreeRTOS. The bundled ESP32 port uses one-byte StackType_t elements.
static_assert(sizeof(StackType_t) == 1, "ESP32 worker stack allocation uses byte units");
StackType_t worker_stack[UsbOutputQueue::worker_stack_bytes];
QueueHandle_t queue_handle = nullptr;
TaskHandle_t worker_handle = nullptr;
UsbOutputQueue output_queue;

void outputWorker(void*) {
  BoundedUsbPacket packet;
  for (;;) {
    // Blocking is confined to this task. A stalled USB mutex/consumer cannot
    // delay the health producer, which uses xQueueSend with a zero timeout.
    if (xQueueReceive(queue_handle, &packet, portMAX_DELAY) == pdPASS)
      bounded_usb_output.deliver(packet, Serial);
  }
}
}

BoundedUsbOutput<UsbOutputQueue> bounded_usb_output(output_queue);

bool UsbOutputQueue::begin() {
  if (worker_handle) return true;
  if (!queue_handle) queue_handle = xQueueCreateStatic(queue_capacity,
      sizeof(BoundedUsbPacket), queue_storage, &queue_control);
  if (!queue_handle) return false;
  // Low application priority; the worker sleeps while its queue is empty and
  // never polls USB readiness. No timeout changes to the HWCDC driver are made.
  worker_handle = xTaskCreateStatic(outputWorker, "health-usb", worker_stack_bytes,
      nullptr, tskIDLE_PRIORITY + 1, worker_stack, &worker_control);
  return worker_handle != nullptr;
}
unsigned UsbOutputQueue::spaces() const {
  return worker_handle && queue_handle ? uxQueueSpacesAvailable(queue_handle) : 0;
}
bool UsbOutputQueue::ready() const { return worker_handle && queue_handle; }
unsigned UsbOutputQueue::count() const {
  return queue_handle ? uxQueueMessagesWaiting(queue_handle) : 0;
}
bool UsbOutputQueue::trySend(const BoundedUsbPacket& packet) {
  return worker_handle && queue_handle && xQueueSend(queue_handle, &packet, 0) == pdTRUE;
}
uint32_t UsbOutputQueue::stackHighWaterBytes() const {
  return worker_handle ? uint32_t(uxTaskGetStackHighWaterMark(worker_handle)) : 0;
}
bool beginBoundedUsbOutput() { return output_queue.begin(); }
void healthUsbDebug(const char* format, ...) {
  va_list args;va_start(args,format);bounded_usb_output.vprintf(format,args);va_end(args);
}
#endif
