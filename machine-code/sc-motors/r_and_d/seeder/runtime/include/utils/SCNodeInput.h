#ifndef SCNODE_INPUT_H
#define SCNODE_INPUT_H

#include "IInput.h"
#include "pubSysCls.h"
#include <cstdint>

class SCNodeInput : public IInput {
public:
    explicit SCNodeInput(sFnd::INode& node);

    Result refresh() override;
    bool is_active() const override;
    std::uint64_t edge_count() const override;

private:
    sFnd::INode& node_;
    bool level_ = false;
    bool has_baseline_ = false;
    std::uint64_t edge_count_ = 0;
};

#endif
