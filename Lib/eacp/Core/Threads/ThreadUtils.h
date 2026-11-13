#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace eacp::Threads
{
void assertMainThread();
bool isMainThread();

std::uint64_t currentThreadId();

enum class ThreadPriority
{
    Low,
    Normal,
    High
};

// Names the calling thread for debuggers and profilers, cut to the platform's
// limit by fitThreadName (15 bytes on Linux and Android, 63 on Apple).
void setCurrentThreadName(std::string_view name);

// Normal sets the platform default explicitly. On desktop Linux levels do
// nothing but an inherited realtime policy is dropped; Android sets the
// thread's nice value, and raising it may be refused.
void setCurrentThreadPriority(ThreadPriority priority);

// `name` cut to `maxLength` bytes, shortening the text before a trailing
// number rather than the number, so "EACP Pool Thread 12" keeps its "12".
std::string fitThreadName(std::string_view name, int maxLength);

// Milliseconds, fractional; 0 leaves a figure unset.
struct RealtimeOptions
{
    double periodMs = 0.0;
    double processingTimeMs = 0.0;
    double maxProcessingTimeMs = 0.0;

    // One buffer's duration as the deadline, as JUCE's
    // withApproximateAudioProcessingTime does.
    static RealtimeOptions forAudio(int framesPerBuffer, double sampleRate);
};

// Apple's time-constraint policy, MMCSS "Pro Audio" on Windows, SCHED_RR (reset
// for threads it starts) on desktop Linux, nothing on Android. Returns whether it took; when it did not,
// the thread is left High (TIME_CRITICAL on Windows).
bool setCurrentThreadRealtime(const RealtimeOptions& options);

// Undoes setCurrentThreadRealtime where the platform holds a registration
// (MMCSS); nothing elsewhere.
void endCurrentThreadRealtime();

} // namespace eacp::Threads
