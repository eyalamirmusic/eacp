#pragma once

#include "Faults.h"

#include "../CrashReport.h"

#include <optional>
#include <string_view>

// What each fault is expected to produce, on each thread, on each lane
// eacp builds for: the table SelfDestructTests runs a case for, per cell.
namespace eacp::CrashReporting::Faults
{
enum class Lane
{
    macOSArm64,
    macOSX64,
    windowsX64Msvc,
    windowsX64ClangCl,
    windowsArm64Msvc,
    windowsArm64ClangCl,
    linux
};

enum class OutcomeStatus
{
    reported,
    reportedWithLimits,
    notReported,
    notAvailable,
    pendingPR
};

struct ExpectedOutcome
{
    OutcomeStatus status = OutcomeStatus::notAvailable;
    std::string_view reason;

    // A second reason the same fault is seen with, where the platform varies.
    std::string_view alternativeReason;

    CaptureSource capturedBy = CaptureSource::handler;
    bool hasStack = false;
    bool hasMinidump = false;

    // The frames reach the faulting code, not only the reporter and the CRT.
    bool stackReachesFault = false;

    // The limit, the reason it is not reported, or what the pending PR does.
    std::string_view note;

    // pendingPR: the PR that reports it, 0 while it is not opened yet. On a
    // reported cell, a PR that lifts its limit.
    int pullRequest = 0;
};

struct MatrixCell
{
    std::string_view fault;
    ThreadChoice thread = ThreadChoice::main;
    Lane lane = Lane::linux;
    ExpectedOutcome outcome;
};

const Vector<Lane>& listLanes();
std::string_view getLaneName(Lane lane);

// The lane this binary was built for.
Lane getCurrentLane();

// Every fault on both threads on every lane.
const Vector<MatrixCell>& getCrashMatrix();

std::optional<ExpectedOutcome>
    findExpectedOutcome(std::string_view fault, ThreadChoice thread, Lane lane);
} // namespace eacp::CrashReporting::Faults
