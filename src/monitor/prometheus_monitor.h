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

#pragma once

#include "prometheus/exposer.h"
#include "prometheus/gauge.h"

namespace lgraph {
namespace monitor {

class ResourceMonitor {
 public:
    explicit ResourceMonitor(const std::string &host);

    void report_server_info(const std::string& info);

    void report_tugraph_info(const std::string& info);

    // Graph lifecycle cache metrics (Phase 2 lazy loading).
    // See docs/architecture/10-graph-lifecycle-v2.md.
    void report_graph_metrics(int64_t registered_graphs, int64_t open_graphs,
                              int64_t cold_opens, int64_t cache_hits,
                              int64_t cache_misses, int64_t evictions,
                              int64_t evict_skipped_refs);

 private:
    prometheus::Exposer exposer;
    std::shared_ptr<prometheus::Registry> registry;
    prometheus::Gauge *cpu_total;
    prometheus::Gauge *cpu_self;

    prometheus::Gauge *mem_total;
    prometheus::Gauge *mem_available;
    prometheus::Gauge *mem_self;

    prometheus::Gauge *disk_read_rate;
    prometheus::Gauge *disk_write_rate;

    prometheus::Gauge *disk_total;
    prometheus::Gauge *disk_available;
    prometheus::Gauge *disk_self;

    prometheus::Gauge *total_request;
    prometheus::Gauge *write_request;

    prometheus::Gauge *graph_registered;
    prometheus::Gauge *graph_open;
    prometheus::Gauge *graph_cold_opens;
    prometheus::Gauge *graph_cache_hits;
    prometheus::Gauge *graph_cache_misses;
    prometheus::Gauge *graph_evictions;
    prometheus::Gauge *graph_evict_skipped_refs;

    prometheus::Gauge *raft_current_term;
    prometheus::Gauge *raft_commit_index;
    prometheus::Gauge *raft_applied_index;
    prometheus::Gauge *raft_leader;
    prometheus::Gauge *raft_last_log_index;
    prometheus::Gauge *raft_replication_lag;

public:
    void report_raft_metrics(int64_t current_term, int64_t commit_index,
                             int64_t applied_index, bool is_leader,
                             int64_t last_log_index, int64_t replication_lag);
};

}  // end of namespace monitor

}  // end of namespace lgraph
