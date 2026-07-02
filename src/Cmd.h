#pragma once

// bypassLock=true skips the child-lock gate. It is for internal/automation callers (web file
// delete/format, HomeKit, RFID modification cards, auto-pause-on-min-volume) that must keep working
// while the physical controls are locked. Physical button/IR presses call with the default (false).
void Cmd_Action(const uint16_t mod, bool bypassLock = false);
