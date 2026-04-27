#include "../include/utils/IInput.h"

Photoeye::Photoeye(IInput& input, const PhotoeyeConfig& config)
    : input_(input), config_(config) {}

Result Photoeye::refresh() {
    status_.triggered_this_refresh = false;

    Result r = input_.refresh();
    if (r.code != ResultCode::Ok) {
        return r;
    }

    const std::uint64_t edges = input_.edge_count();

    if (!has_baseline_) {
        last_edge_count_ = edges;
        has_baseline_ = true;
    } else if (edges != last_edge_count_) {
        const std::uint64_t delta = edges - last_edge_count_;
        status_.trigger_count += delta;
        status_.triggered_this_refresh = true;
        last_edge_count_ = edges;
    }

    status_.blocked = input_.is_active();
    return {ResultCode::Ok};
}
