#pragma once

// A copy of the log on disk for the run in progress, so a phone that closes the game can still say
// why. On Android it is <files>/logs/last-run.log, next to the config; LauncherActivity reads it
// before the next run truncates it and offers it, with Android's own exit reason, as a crash
// report. A fatal signal adds its number, fault address and a backtrace. Elsewhere every call is a
// no-op: desktop logs already reach a console and the Windows crash reporter.
namespace partyboard::run_log {

// Truncates the file, writes the run's header and catches fatal signals. Call it first thing.
void open() noexcept;
// One line, from any thread.
void write(const char *level, const char *module, const char *message) noexcept;
// The run ended by returning from main: the next launch has nothing to report.
void mark_clean_exit() noexcept;

} // namespace partyboard::run_log
