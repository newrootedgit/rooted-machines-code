#include "../include/utils/ClearCoreClient.h"
#include <cstdio>

ClearCoreClient::ClearCoreClient(const ClearCoreConfig& config)
    : config_(config) {}

ClearCoreClient::~ClearCoreClient() {
    shutdown();
}

Result ClearCoreClient::init() {
    if (initialized_) {
        return {ResultCode::Ok, "Already initialized"};
    }

    mgr_ = SysManager::Instance();

    // 1. Discover hubs
    SysManager::FindComHubPorts(discovered_ports_);
    if (discovered_ports_.empty()) {
        return {ResultCode::Error, "No SC Hub ports found"};
    }

    // 2. Register ports
    open_port_count_ = 0;
    for (size_t i = 0; i < discovered_ports_.size() && i < NET_CONTROLLER_MAX; i++) {
        mgr_->ComHubPort(i, discovered_ports_[i].c_str());
        open_port_count_++;
    }

    // 3. Open ports
    try {
        mgr_->PortsOpen(open_port_count_);
    } catch (mnErr& e) {
        printf("PortsOpen failed: addr=%d err=0x%08x msg=%s\n",
               e.TheAddr, e.ErrorCode, e.ErrorMsg);
        return {ResultCode::Error, "PortsOpen failed"};
    }

    // 4. Validate port state
    for (size_t i = 0; i < open_port_count_; i++) {
        IPort& port = mgr_->Ports(i);
        if (port.OpenState() != openStates::OPENED_ONLINE) {
            printf("Port %zu not online (state=%d)\n", i, port.OpenState());
            mgr_->PortsClose();
            return {ResultCode::Error, "Port not online"};
        }
    }

    // 5. Validate node count (check port 0)
    IPort& port = mgr_->Ports(0);
    if (static_cast<size_t>(port.NodeCount()) != config_.expected_node_count) {
        printf("Expected %zu nodes, found %d\n",
               config_.expected_node_count, port.NodeCount());
        mgr_->PortsClose();
        return {ResultCode::Error, "Wrong node count"};
    }

    // 6. Validate each node
    for (size_t i = 0; i < port.NodeCount(); i++) {
        INode& n = port.Nodes(i);

        if (n.Info.NodeType() != IInfo::CLEARPATH_SC &&
            n.Info.NodeType() != IInfo::CLEARPATH_SC_ADV) {
            printf("Node %zu: unexpected type %d\n", i, n.Info.NodeType());
            mgr_->PortsClose();
            return {ResultCode::Error, "Wrong node type"};
        }

        printf("Node[%zu]: serial=%d fw=%s model=%s\n",
               i,
               n.Info.SerialNumber.Value(),
               n.Info.FirmwareVersion.Value(),
               n.Info.Model.Value());

        if (n.Status.Power.Value().fld.InBusLoss) {
            printf("Node[%zu]: WARNING bus power low\n", i);
        }
    }

    initialized_ = true;
    return {ResultCode::Ok};
}

void ClearCoreClient::shutdown() {
    if (!initialized_) return;

    disable_all();

    try {
        mgr_->PortsClose();
    } catch (mnErr& e) {
        printf("PortsClose failed: addr=%d err=0x%08x\n", e.TheAddr, e.ErrorCode);
    }

    initialized_ = false;
}

INode& ClearCoreClient::node(size_t index) {
    return mgr_->Ports(0).Nodes(index);
}

size_t ClearCoreClient::node_count() const {
    if (!initialized_) return 0;
    return mgr_->Ports(0).NodeCount();
}

Result ClearCoreClient::enable_node(size_t index) {
    try {
        INode& n = node(index);

        n.Status.AlertsClear();
        n.Motion.NodeStopClear();
        n.EnableReq(true);

        double timeout = mgr_->TimeStampMsec() + config_.enable_timeout_ms;
        while (!n.Motion.IsReady()) {
            if (mgr_->TimeStampMsec() > timeout) {
                if (n.Status.Power.Value().fld.InBusLoss) {
                    return {ResultCode::Error, "Enable timed out: bus power low"};
                }
                return {ResultCode::Error, "Enable timed out"};
            }
        }

        return {ResultCode::Ok};
    } catch (mnErr& e) {
        printf("enable_node(%zu) failed: addr=%d err=0x%08x msg=%s\n",
               index, e.TheAddr, e.ErrorCode, e.ErrorMsg);
        return {ResultCode::Error, "Enable failed"};
    }
}

Result ClearCoreClient::enable_all() {
    for (size_t i = 0; i < node_count(); i++) {
        Result r = enable_node(i);
        if (r.code != ResultCode::Ok) return r;
    }
    return {ResultCode::Ok};
}

void ClearCoreClient::disable_all() {
    for (size_t i = 0; i < node_count(); i++) {
        try {
            node(i).EnableReq(false);
        } catch (mnErr& e) {
            printf("disable node %zu failed: err=0x%08x\n", i, e.ErrorCode);
        }
    }
}

bool ClearCoreClient::is_bus_power_ok(size_t index) {
    try {
        return !node(index).Status.Power.Value().fld.InBusLoss;
    } catch (...) {
        return false;
    }
}

bool ClearCoreClient::is_initialized() const {
    return initialized_;
}
