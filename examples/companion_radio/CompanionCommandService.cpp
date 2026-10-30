// Host tests replace transport/rescue boundaries, not RX or health dispatch.
#ifdef HEALTH_MONITOR_HOST_HEADER
#include HEALTH_MONITOR_HOST_HEADER
#else
#include "MyMesh.h"
#endif

// Preserve rescue isolation from the framed companion protocol. USB health
// commands are available beside BLE only when USB is not the framed interface.
void MyMesh::serviceCompanionCommands() {
  if (_cli_rescue) {
    checkCLIRescueCmd();
  } else {
    checkSerialInterface();
#if ENV_INCLUDE_MPU6050 && !defined(ENABLE_USB_INTERFACE)
    checkCLIRescueCmd(true);
#endif
  }
}

void MyMesh::checkCLIRescueCmd(bool health_only) {
#if ENV_INCLUDE_MPU6050
  // Reply/control traffic has priority over recording. RX keeps progressing even
  // when TX stalls, so a stop command cannot be trapped behind its start reply.
  usb_reply.drain(bounded_usb_output);
  serviceMotionRecording();
  if (!usb_reply.pending()) {
    if (motion_stats_row >= 0) continueMotionStatsReply();
    else if (trace_reply_row < trace_reply_count) continueMotionTraceReply();
  }
#else
  if (usb_reply.pending()) { usb_reply.drain(Serial); return; }
#endif
  int len = strlen(cli_command);
  unsigned serial_bytes = 0;
#if ENV_INCLUDE_MPU6050
  const unsigned serial_budget = HealthNodeConfig::serial_rx_bytes_per_loop;
#else
  const unsigned serial_budget = 16;
#endif
  // Keep parsing bounded per loop. Discard the whole line after overflow;
  // never execute a truncated key or interpret its tail as another command.
  while (Serial.available() > 0 && serial_bytes++ < serial_budget) {
    const int received = Serial.read();
    // HWCDC can return -1 for an unavailable RX queue or a failed receive.
    // Preserve the partial line until actual bytes arrive; -1 is not 0xff data.
    if (received < 0) break;
    const char c = char(received);
    if (c == '\n' && len == 0 && !cli_overflow) continue; // CRLF's trailing LF.
    if (c == '\r' || c == '\n') {
      if (cli_overflow) {
        usb_reply.append("Error: command too long or contains invalid bytes\n");
        cli_command[0] = 0; cli_overflow = false;
        return;
      }
      cli_command[len++] = '\r'; cli_command[len] = 0;
      break;
    }
    if (cli_overflow) continue;
    if (c == 0 || len >= sizeof(cli_command) - 2) { cli_overflow = true; continue; }
    cli_command[len++] = c; cli_command[len] = 0;
    // No local echo: a private key must not be copied into USB diagnostics.
  }

  if (len > 0 && cli_command[len - 1] == '\r') {  // received complete line
    cli_command[len - 1] = 0;  // replace newline with C string null terminator

    char reply[160] = {};
#if ENV_INCLUDE_MPU6050
    if (handleRecordingCommand(cli_command) || handleDiagnosticCommand(cli_command)) {
      // Diagnostic replies drain incrementally on subsequent loop iterations.
    } else
#endif
    if (health_only && handleSettingsCommand(cli_command, reply)) {
      if (*reply) usb_reply.append("%s\n", reply);
#if ENV_INCLUDE_MPU6050
    } else if (handleHealthCommand(cli_command, reply)) {
      usb_reply.append("%s\n", reply);
#endif
    } else if (health_only) {
      usb_reply.append("Error: unknown command; use help\n");
    } else {
      executeRescueCommand();
    }

    cli_command[0] = 0;  // reset command buffer
  }
}

