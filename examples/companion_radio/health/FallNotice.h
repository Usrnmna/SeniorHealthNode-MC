#pragma once
#include <HealthNodeConfig.h>

// Add a named directive here, its text in HealthNodeConfig, and its condition in
// FallResponse. Delivery mechanics do not decide the wearer's response state.
enum class FallNotice { None, AssistanceRequested, UserCouldNotConfirm, NoResidualMovement,
                        UserConfirmedFall, UserConfirmedFalseAlarm };

inline bool isUserConfirmation(FallNotice notice) {
  return notice == FallNotice::UserConfirmedFall || notice == FallNotice::UserConfirmedFalseAlarm;
}

inline bool isAutomaticWarning(FallNotice notice) {
  return notice == FallNotice::UserCouldNotConfirm || notice == FallNotice::NoResidualMovement;
}

inline const char* fallNoticeText(FallNotice notice) {
  switch (notice) {
    case FallNotice::AssistanceRequested: return HealthNodeConfig::ack_message;
    case FallNotice::UserCouldNotConfirm: return HealthNodeConfig::unconfirmed_message;
    case FallNotice::NoResidualMovement: return HealthNodeConfig::no_movement_message;
    case FallNotice::UserConfirmedFall: return HealthNodeConfig::confirmed_message;
    case FallNotice::UserConfirmedFalseAlarm: return HealthNodeConfig::false_alarm_message;
    default: return "";
  }
}
