#include "utils/SCNodeInput.h"

using namespace sFnd;

SCNodeInput::SCNodeInput(INode& node)
    : node_(node) {}

Result SCNodeInput::refresh() {
    try {
        // TODO(SDK): verify the right input-read call against the local Teknic SDK
        // header at $HOME/teknic/Linux_Software/sFoundation/inc/inc-pub/pubSysCls.h.
        // Likely options:
        //   bool level = node_.Inputs.A.Value();                     // direct accessor
        //   bool level = (node_.Status.RT.Value().fld.InA != 0);     // status-register
        //   bool level = (node_.Status.Inputs.Value().fld.InA != 0); // status.inputs
        // Whichever the local header exposes, also confirm in ClearView that the
        // motor input pin the photoeye is wired to is set to a general-purpose input
        // mode (not a Move Trigger), otherwise the level may not be readable.
        const bool new_level = node_.Status.RT.Value().cpm.InA;

        if (!has_baseline_) {
            level_ = new_level;
            has_baseline_ = true;
        } else if (new_level != level_) {
            ++edge_count_;
            level_ = new_level;
        }
        return {ResultCode::Ok};
    } catch (...) {
        return {ResultCode::Error, "Failed to read motor input"};
    }
}

bool SCNodeInput::is_active() const {
    return level_;
}

std::uint64_t SCNodeInput::edge_count() const {
    return edge_count_;
}
