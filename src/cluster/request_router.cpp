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

#include "cluster/request_router.h"

namespace lgraph {
namespace cluster {

const char* ToString(Disposition d) {
    switch (d) {
    case Disposition::HANDLE_LOCALLY: return "HANDLE_LOCALLY";
    case Disposition::FORWARD: return "FORWARD";
    case Disposition::REJECT_UNKNOWN_GRAPH: return "REJECT_UNKNOWN_GRAPH";
    case Disposition::REJECT_NOT_ACTIVE: return "REJECT_NOT_ACTIVE";
    case Disposition::REJECT_NO_SHARD: return "REJECT_NO_SHARD";
    case Disposition::REJECT_STALE: return "REJECT_STALE";
    }
    return "UNKNOWN";
}

ClusterRequestHandler::ClusterRequestHandler(ClusterMetaStore* store, ShardManager* shards,
                                             Router* router, ClusterControl* control,
                                             Forwarder* forwarder, ShardId local_shard)
    : store_(store),
      shards_(shards),
      router_(router),
      control_(control),
      forwarder_(forwarder),
      local_shard_(local_shard) {}

HandleResult ClusterRequestHandler::Handle(const std::string& graph, uint64_t expected_uid,
                                           PlacementVersion seen_version,
                                           const std::string& payload, int64_t now_ms) {
    HandleResult out;
    RouteTarget target;
    RouteStatus st = router_->Resolve(graph, now_ms, &target);
    out.target = target;
    switch (st) {
    case RouteStatus::GRAPH_NOT_FOUND:
        out.disposition = Disposition::REJECT_UNKNOWN_GRAPH;
        out.reason = "no such graph: " + graph;
        return out;
    case RouteStatus::PLACEMENT_NOT_ACTIVE:
        out.disposition = Disposition::REJECT_NOT_ACTIVE;
        out.reason = "graph is not ACTIVE: " + graph;
        return out;
    case RouteStatus::NO_HEALTHY_SHARD:
        out.disposition = Disposition::REJECT_NO_SHARD;
        out.reason = "no healthy shard for graph: " + graph;
        return out;
    case RouteStatus::STALE_PLACEMENT:
        // Resolve() never returns this; Validate() does. Re-validate so a
        // caller-supplied version is compared against the fresh target.
        break;
    case RouteStatus::OK:
        break;
    }

    // Incarnation check first: a recreated graph never accepts the old UID,
    // even if the version coincidentally matches.
    if (expected_uid != 0 && expected_uid != target.unique_id) {
        out.disposition = Disposition::REJECT_STALE;
        out.reason = "stale graph incarnation for graph: " + graph;
        return out;
    }
    const uint64_t fence_uid = expected_uid == 0 ? target.unique_id : expected_uid;
    const PlacementVersion fence_version = seen_version == 0 ? target.version : seen_version;

    if (target.shard_id == local_shard_) {
        // Write boundary of the receiving shard: enforce the fence here, not
        // just in the router, so a directly-reached obsolete destination still
        // refuses a stale write.
        PlacementVersion current = 0;
        uint64_t current_uid = 0;
        if (!control_->FenceAt(local_shard_, graph, fence_uid, fence_version, &current,
                               &current_uid)) {
            out.disposition = Disposition::REJECT_STALE;
            out.reason = "stale placement version for graph: " + graph;
            return out;
        }
        out.disposition = Disposition::HANDLE_LOCALLY;
        return out;
    }

    // Another shard owns the graph: forward (or detect the caller is stale).
    if (seen_version != 0 && seen_version != target.version) {
        out.disposition = Disposition::REJECT_STALE;
        out.reason = "stale placement version for graph: " + graph;
        return out;
    }
    out.disposition = Disposition::FORWARD;
    if (forwarder_) {
        out.forwarded = forwarder_->Forward(target, payload);
    } else {
        out.forwarded.ok = false;
        out.forwarded.error = "no forwarder configured";
    }
    return out;
}

}  // namespace cluster
}  // namespace lgraph
