#ifndef TOUCH_ENCODER_STATE_H
#define TOUCH_ENCODER_STATE_H

#include "../RuntimeTypes.h"

class TouchEncoderState {
public:
    explicit TouchEncoderState(const char* json_path)
        : json_path_(json_path) {}

    Result refresh();

    const PresetValues& values() const {
        return values_;
    }

    int belt_speed() const {
        return values_.belt_speed;
    }

    int roller_speed() const {
        return values_.roller_speed;
    }

    bool ready_to_run() const {
        return values_.ready_to_run;
    }

    int active_variety() const {
        return values_.active_variety;
    }

    const char* json_path() const {
        return json_path_;
    }

private:
    const char* json_path_ = "";
    PresetValues values_ {};
};

#endif
