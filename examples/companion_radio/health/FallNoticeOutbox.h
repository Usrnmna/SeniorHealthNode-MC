#pragma once
#include "AssistanceDelivery.h"
#include "FallNoticeMessage.h"

// Bounded outbox: one active event, one waiting ordinary event, and one reserved
// confirmation. Response logic retains an event when these slots are occupied.
// One AssistanceDelivery instance sends all parts, preserving even/odd tag ownership.
class FallNoticeOutbox {
public:
  bool enqueue(FallNotice notice, uint32_t incident, const FallNoticeMessage::Bundle& message,
               const FallChannel& channel = FallChannel{}) {
    Slot& waiting = isUserConfirmation(notice) ? confirmation : ordinary;
    if (waiting.notice != FallNotice::None) return false;
    waiting.notice = notice; waiting.incident = incident; waiting.message = message;
    waiting.channel = channel;
    if (isUserConfirmation(notice) && ordinary.incident == incident &&
        isAutomaticWarning(ordinary.notice)) ordinary.notice = FallNotice::None;
    return true;
  }

  // Resolve local cancellation even when a previous confirmation occupies the
  // reserved slot. Never remove another incident's warning or an explicit reply.
  void cancelWarnings(uint32_t incident, AssistanceTransport& link, AssistanceDelivery& delivery) {
    if (ordinary.incident == incident && isAutomaticWarning(ordinary.notice))
      ordinary.notice = FallNotice::None;
    if (active.incident == incident && isAutomaticWarning(active.notice)) {
      delivery.supersede(link);
      active.notice = FallNotice::None;
    }
  }

  // Confirmation supersedes only its own automatic warning, including unsent
  // parts and further retries. An already-started RF packet finishes normally.
  // Each part keeps an independent bounded retry budget. A failed first part
  // does not prevent attempting the second; failure is retained for diagnostics.
  void poll(uint32_t now, AssistanceTransport& link, AssistanceDelivery& delivery) {
    if (confirmation.notice != FallNotice::None && active.incident == confirmation.incident &&
        isAutomaticWarning(active.notice)) {
      delivery.supersede(link);
      active.notice = FallNotice::None;
    }
    if (active.notice != FallNotice::None && !delivery.busy()) {
      if (delivery.state() == AssistanceDelivery::Failed) failed_part = true;
      if (++part < active.message.count) startPart(now, link, delivery);
      else active.notice = FallNotice::None;
    }
    if (active.notice == FallNotice::None) {
      Slot& next = confirmation.notice != FallNotice::None ? confirmation : ordinary;
      if (next.notice != FallNotice::None) {
        active = next; next.notice = FallNotice::None;
        part = 0; failed_part = false;
        startPart(now, link, delivery);
      }
    }
    delivery.poll(now, link);
  }
  bool partFailed() const { return failed_part; }
  bool busy() const {
    return active.notice != FallNotice::None || ordinary.notice != FallNotice::None ||
        confirmation.notice != FallNotice::None;
  }

private:
  struct Slot {
    FallNotice notice = FallNotice::None;
    uint32_t incident = 0;
    FallNoticeMessage::Bundle message;
    FallChannel channel;
  } active, ordinary, confirmation;
  unsigned part = 0;
  bool failed_part = false;
  void startPart(uint32_t now, AssistanceTransport& link, AssistanceDelivery& delivery) {
    delivery.begin(now, link, active.message.payload[part],
        active.message.count ? active.message.length[part] : 0, active.channel);
  }
};
