#pragma once

// Compute kernels executed on the CPU: a ComputeKernel (or a bare ShaderGraph)
// decoded once into a Plan and run over plain arrays, bound through Bindings,
// with no device and no allocation per dispatch.

#include "Bindings.h"
#include "CpuUniformVisitor.h"
#include "Executor.h"
#include "Plan.h"
#include "Workspace.h"
