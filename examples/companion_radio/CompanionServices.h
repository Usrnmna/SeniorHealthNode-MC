#pragma once

// The production cooperative service order, shared with integration tests.
// Each boundary retains its existing budget; this function provides no timing
// guarantee for radio, filesystem, USB, sensors or UI implementations.
// The final callback runs the optional UI with alarm ownership retained across
// both response positions, including a gesture that resolves the alarm.
template<class Network, class Interfaces, class Sensors, class ServiceUI>
void serviceCompanionLoop(Network& network, Interfaces& interfaces, Sensors& sensors,
                          ServiceUI service_ui) {
  bool alarm_owned_button = false;
#if ENV_INCLUDE_MPU6050
  network.pollMotion();
  alarm_owned_button = network.fallAlarmActive();
  network.pollFallResponse();
  network.pollLowBattery();
#endif
  network.loop();
  interfaces.loop();
#if ENV_INCLUDE_MPU6050
  network.pollMotion();
  alarm_owned_button = network.fallAlarmActive() || alarm_owned_button;
  network.pollFallResponse();
#endif
  sensors.loop();
#if ENV_INCLUDE_MPU6050
  alarm_owned_button = network.fallAlarmActive() || alarm_owned_button;
#endif
  service_ui(alarm_owned_button);
}
