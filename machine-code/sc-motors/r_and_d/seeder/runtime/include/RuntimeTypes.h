#ifndef RUNTIME_TYPES_H
#define RUNTIME_TYPES_H

#include <cstdint>
#include <cstddef>

enum class ResultCode {
    Ok,
    Blocked,
    Error,
};

struct Result {
    ResultCode code = ResultCode::Ok;
    const char* message = "";
};

struct AxisStatus {
    bool enabled = false;
    bool faulted = false;
    bool moving = false;
};

struct SafetyState {
    bool kill_ok = false;
    bool fault_latched = false;
};

struct PhotoeyeStatus {
    bool blocked = false;
    bool triggered_this_refresh = false;
    std::uint64_t trigger_count = 0;
};

struct PresetValues {
    bool ready_to_run = false;
    int active_variety = -1;
    int belt_speed = 0;
    int roller_speed = 0;
};

struct ClearCoreConfig {
    std::size_t expected_node_count = 3;
    double enable_timeout_ms = 3000.0;
};
#endif
