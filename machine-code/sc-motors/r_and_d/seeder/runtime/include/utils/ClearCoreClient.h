#ifndef CLEARCORE_CLIENT_H
#define CLEARCORE_CLIENT_H

#include "../RuntimeTypes.h"
#include "pubSysCls.h"
#include <vector>
#include <string>


class ClearCoreClient {
    public:
        ~ClearCoreClient();
        explicit ClearCoreClient(const ClearCoreConfig& Config);

        Result init();
        void shutdown();


        sFnd::INode& node(size_t index);
        sFnd::IPort& port();
        size_t node_count() const;

        Result enable_node(size_t index);
        Result enable_all();
        void   disable_all();

        bool is_bus_power_ok(size_t index);
        bool is_initialized() const;

    private:
        ClearCoreConfig config_;
        sFnd::SysManager* mgr_ = nullptr;
        size_t open_port_count_ = 0;
        bool initialized_ = false;
        std::vector<std::string> discovered_ports_;

}; 


#endif