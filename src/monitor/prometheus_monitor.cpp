/**
 * Copyright 2022 AntGroup CO., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

#include "monitor/prometheus_monitor.h"
#include "prometheus/registry.h"
#include "fma-common/hardware_info.h"
#include "fma-common/file_system.h"
#include "tools/json.hpp"

namespace lgraph {
namespace monitor {

ResourceMonitor::ResourceMonitor(const std::string& host)
    : exposer(host)
    , registry(std::make_shared<prometheus::Registry>()) {
    prometheus::Family<prometheus::Gauge>& gf = prometheus::BuildGauge().Name("resources_report")
            .Help("tugraph resources monitor gauge").Register(*registry);
    cpu_total = &gf.Add({{"resouces_type", "cpu"}, {"type", "total"}});
    cpu_total->SetToCurrentTime();

    cpu_self = &gf.Add({{"resouces_type", "cpu"}, {"type", "self"}});
    cpu_self->SetToCurrentTime();

    mem_total = &gf.Add({{"resouces_type", "memory"}, {"type", "total"}});
    mem_total->SetToCurrentTime();
    mem_available = &gf.Add({{"resouces_type", "memory"}, {"type", "available"}});
    mem_available->SetToCurrentTime();
    mem_self = &gf.Add({{"resouces_type", "memory"}, {"type", "self"}});
    mem_self->SetToCurrentTime();

    disk_read_rate = &gf.Add({{"resouces_type", "disk_rate"}, {"type", "read"}});
    disk_read_rate->SetToCurrentTime();
    disk_write_rate = &gf.Add({{"resouces_type", "disk_rate"}, {"type", "write"}});
    disk_write_rate->SetToCurrentTime();

    disk_total = &gf.Add({{"resouces_type", "disk"}, {"type", "total"}});
    disk_total->SetToCurrentTime();
    disk_available = &gf.Add({{"resouces_type", "disk"}, {"type", "available"}});
    disk_available->SetToCurrentTime();
    disk_self = &gf.Add({{"resouces_type", "disk"}, {"type", "self"}});
    disk_self->SetToCurrentTime();

    total_request = &gf.Add({{"resouces_type", "request"}, {"type", "total"}});
    total_request->SetToCurrentTime();
    write_request = &gf.Add({{"resouces_type", "request"}, {"type", "write"}});
    write_request->SetToCurrentTime();

    prometheus::Family<prometheus::Gauge>& gf3 =
        prometheus::BuildGauge()
            .Name("tugraph_raft")
            .Help("TuGraph Raft consensus metrics (Phase 3 HA)")
            .Register(*registry);
    raft_current_term = &gf3.Add({{"metric", "current_term"}});
    raft_commit_index = &gf3.Add({{"metric", "commit_index"}});
    raft_applied_index = &gf3.Add({{"metric", "applied_index"}});
    raft_leader = &gf3.Add({{"metric", "is_leader"}});
    raft_last_log_index = &gf3.Add({{"metric", "last_log_index"}});
    raft_replication_lag = &gf3.Add({{"metric", "replication_lag"}});

    prometheus::Family<prometheus::Gauge>& gf2 =
        prometheus::BuildGauge()
            .Name("tugraph_graph_cache")
            .Help("TuGraph graph lifecycle cache metrics (Phase 2 lazy loading)")
            .Register(*registry);
    graph_registered = &gf2.Add({{"metric", "registered_graphs"}});
    graph_open = &gf2.Add({{"metric", "open_graphs"}});
    graph_cold_opens = &gf2.Add({{"metric", "cold_opens"}});
    graph_cache_hits = &gf2.Add({{"metric", "cache_hits"}});
    graph_cache_misses = &gf2.Add({{"metric", "cache_misses"}});
    graph_evictions = &gf2.Add({{"metric", "evictions"}});
    graph_evict_skipped_refs = &gf2.Add({{"metric", "evict_skipped_refs"}});

    exposer.RegisterCollectable(registry);
}

void ResourceMonitor::report_server_info(const std::string &info) {
    nlohmann::json value = nlohmann::json::parse(info);
    cpu_total->Set(value["cpu"]["server"]);
    cpu_self->Set(value["cpu"]["self"]);
    mem_total->Set(value["memory"]["total"]);
    mem_available->Set(value["memory"]["available"]);
    mem_self->Set(value["memory"]["self"]);
    disk_read_rate->Set(value["disk_rate"]["read"]);
    disk_write_rate->Set(value["disk_rate"]["write"]);
    disk_total->Set(value["disk_storage"]["total"]);
    disk_available->Set(value["disk_storage"]["available"]);
    disk_self->Set(value["disk_storage"]["self"]);
}

void ResourceMonitor::report_tugraph_info(const std::string& info) {
    nlohmann::json value = nlohmann::json::parse(info);
    total_request->Set(value["request"]["requests/second"]);
    write_request->Set(value["request"]["writes/second"]);
}

void ResourceMonitor::report_graph_metrics(int64_t registered_graphs, int64_t open_graphs,
                                          int64_t cold_opens, int64_t cache_hits,
                                          int64_t cache_misses, int64_t evictions,
                                          int64_t evict_skipped_refs) {
    graph_registered->Set(static_cast<double>(registered_graphs));
    graph_open->Set(static_cast<double>(open_graphs));
    graph_cold_opens->Set(static_cast<double>(cold_opens));
    graph_cache_hits->Set(static_cast<double>(cache_hits));
    graph_cache_misses->Set(static_cast<double>(cache_misses));
    graph_evictions->Set(static_cast<double>(evictions));
    graph_evict_skipped_refs->Set(static_cast<double>(evict_skipped_refs));
}

void ResourceMonitor::report_raft_metrics(int64_t current_term, int64_t commit_index,
                                          int64_t applied_index, bool is_leader,
                                          int64_t last_log_index, int64_t replication_lag) {
    raft_current_term->Set(static_cast<double>(current_term));
    raft_commit_index->Set(static_cast<double>(commit_index));
    raft_applied_index->Set(static_cast<double>(applied_index));
    raft_leader->Set(is_leader ? 1.0 : 0.0);
    raft_last_log_index->Set(static_cast<double>(last_log_index));
    raft_replication_lag->Set(static_cast<double>(replication_lag));
}

}  // end of namespace monitor

}  // end of namespace lgraph
