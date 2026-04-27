#include "utils/TouchEncoderState.h"

#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/file.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

Result TouchEncoderState::refresh() {
    const std::string lock_path = std::string(json_path_) + ".lock";
    const int lock_fd = open(lock_path.c_str(), O_CREAT | O_RDONLY, 0666);
    if (lock_fd < 0) {
        return {ResultCode::Error, "Failed to open touch encoder lock file"};
    }

    if (flock(lock_fd, LOCK_SH) != 0) {
        close(lock_fd);
        return {ResultCode::Error, "Failed to lock touch encoder state"};
    }

    std::ifstream json_file(json_path_);
    if (!json_file.is_open()) {
        values_ = {};
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return {ResultCode::Ok, "Touch encoder state file missing; defaulted to zero"};
    }

    try {
        const nlohmann::json json = nlohmann::json::parse(json_file, nullptr, true, true);

        values_.ready_to_run = json.value("ready_to_run", false);
        values_.active_variety = json.value("active_variety", -1);
        values_.belt_speed = 0;
        values_.roller_speed = 0;

        if (values_.active_variety >= 0) {
            const std::string variety_key = std::to_string(values_.active_variety);
            if (json.contains(variety_key) && json[variety_key].is_object()) {
                values_.belt_speed = json[variety_key].value("belt_speed", 0);
                values_.roller_speed = json[variety_key].value("roller_speed", values_.belt_speed);
            }
        }

        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return {ResultCode::Ok};
    } catch (const nlohmann::json::exception&) {
        values_ = {};
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return {ResultCode::Ok, "Touch encoder state JSON invalid; defaulted to zero"};
    }
}
