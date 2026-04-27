#ifndef IINPUT_H
#define IINPUT_H

#include "../RuntimeTypes.h"
#include <cstdint>

class IInput {
public:
    virtual ~IInput() = default;

    virtual Result refresh() = 0;
    virtual bool is_active() const = 0;
    virtual std::uint64_t edge_count() const = 0;
};


struct PhotoeyeConfig {
    const char* name = "photoeye";
};

class Photoeye {
public:
    Photoeye(IInput& input, const PhotoeyeConfig& config);

    Result refresh();

    PhotoeyeStatus status() const { return status_; }
    bool blocked() const { return status_.blocked; }
    bool triggered_this_refresh() const { return status_.triggered_this_refresh; }
    std::uint64_t trigger_count() const { return status_.trigger_count; }
    const char* name() const { return config_.name; }

private:
    IInput& input_;
    PhotoeyeConfig config_;

    bool has_baseline_ = false;
    std::uint64_t last_edge_count_ = 0;
    PhotoeyeStatus status_ {};
};

#endif
