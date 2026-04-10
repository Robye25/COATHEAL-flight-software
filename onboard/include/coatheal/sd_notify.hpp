#pragma once

// Minimal sd_notify(3) implementation for the systemd watchdog protocol.
//
// We deliberately do NOT depend on libsystemd: COATHEAL has to build cleanly
// on the developer's Windows / macOS workstations and on the Pi cross-compile
// environment, neither of which always has libsystemd-dev. The protocol is
// simple — write a UTF-8 message to the AF_UNIX SOCK_DGRAM socket whose path
// is given by the NOTIFY_SOCKET environment variable — so we open-code it.
//
// On non-Linux platforms, or when NOTIFY_SOCKET is unset (running outside
// systemd, e.g. interactively or under CI), every call is a silent no-op.

#include <string>

namespace coatheal {

// Send a single sd_notify message (e.g. "READY=1", "WATCHDOG=1", "STATUS=…").
// Returns true on success, false on any failure (including no NOTIFY_SOCKET).
bool SdNotify(const std::string& message);

// Convenience wrapper for the watchdog ping.
inline bool SdNotifyWatchdog() { return SdNotify("WATCHDOG=1"); }

}  // namespace coatheal
