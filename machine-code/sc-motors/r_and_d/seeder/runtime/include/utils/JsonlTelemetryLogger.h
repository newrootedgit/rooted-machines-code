#ifndef JSONL_TELEMETRY_LOGGER_H
#define JSONL_TELEMETRY_LOGGER_H

#include "../RuntimeTypes.h"
#include "TelemetryTypes.h"
#include <string>
#include <fstream> 

class JsonlTelemetryLogger { 
    public: 
        explicit JsonlTelemetryLogger(const std::string& path); 
        ~JsonlTelemetryLogger() = default;

        Result event_handler(
            uint64_t boot_id,
            uint64_t seq,
            uint64_t timestamp_ms,
            TelemetryEventType event_type,
            const std::string& role,
            int node_index,
            int64_t value_i64
        );
        
        Result status_update_handler(
            uint64_t boot_id,
            uint64_t seq,
            uint64_t timestamp_ms,
            bool ready_to_run,
            bool kill_ok,
            bool fault_latched,
            int active_variety,
            int belt_speed,
            uint64_t tray_count,
            int belt_node_index,
            int belt_serial,
            const std::string& belt_model,
            const std::string& belt_firmware,
            const AxisStatus& belt_status,
            bool belt_bus_power_ok,
            double belt_measured_velocity,
            uint64_t belt_active_uptime_ms,
            uint32_t belt_alert_bits = 0
        );  


    private:
        Result ensure_open(); 

        Result log_event(const MachineEvent& event);
        Result log_snapshot(const MachineSnapshot& snapshot);
        std::string path_; 
        std::ofstream file_; 
};

#endif
