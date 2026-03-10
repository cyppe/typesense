#pragma once

// Typesense logging facade.
//
// Backend: Abseil Logging (ABSL_LOG / ABSL_CHECK).
// Keep the ABSL_ prefixed macros to avoid accidental LOG()/CHECK() collisions.

#include "absl/log/absl_log.h"
#include "absl/log/absl_check.h"

// --- Facade macros (delegate to Abseil) ---

#define TS_LOG(severity)            ABSL_LOG(severity)
#define TS_VLOG(level)              ABSL_VLOG(level)
#define TS_CHECK(cond)              ABSL_CHECK(cond)
#define TS_DCHECK(cond)             ABSL_DCHECK(cond)
#define TS_LOG_IF(severity, cond)   ABSL_LOG_IF(severity, cond)
