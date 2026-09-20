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

#include "cluster/cluster_control.h"

namespace lgraph {
namespace cluster {

const char* ToString(ControlStatus s) {
    switch (s) {
    case ControlStatus::OK: return "OK";
    case ControlStatus::GRAPH_EXISTS: return "GRAPH_EXISTS";
    case ControlStatus::GRAPH_NOT_FOUND: return "GRAPH_NOT_FOUND";
    case ControlStatus::NO_HEALTHY_SHARD: return "NO_HEALTHY_SHARD";
    case ControlStatus::STALE_PLACEMENT: return "STALE_PLACEMENT";
    case ControlStatus::PERSIST_FAILED: return "PERSIST_FAILED";
    }
    return "UNKNOWN";
}

ClusterControl::ClusterControl(ClusterMetaStore* store, ShardManager* shards, Router* router)
    : store_(store), shards_(shards), router_(router) {}

ControlStatus ClusterControl::RegisterShard(KvTransaction& txn, const ShardInfo& info) {
    if (!shards_->RegisterShard(txn, info)) return ControlStatus::PERSIST_FAILED;
    return ControlStatus::OK;
}

ControlStatus ClusterControl::RemoveShard(KvTransaction& txn, ShardId id) {
    if (!shards_->DeregisterShard(txn, id)) return ControlStatus::PERSIST_FAILED;
    return ControlStatus::OK;
}

std::vector<ShardInfo> ClusterControl::ListShards() const { return shards_->ListShards(); }

ControlStatus ClusterControl::CreateGraph(KvTransaction& txn, const std::string& name,
                                          int64_t now_ms, PlacementVersion* out_version,
                                          uint64_t* out_uid) {
    if (store_->HasGraph(name)) return ControlStatus::GRAPH_EXISTS;
    ShardId shard = INVALID_SHARD_ID;
    if (!shards_->PickShard(now_ms, &shard)) return ControlStatus::NO_HEALTHY_SHARD;
    if (!store_->PutGraphPlacement(txn, name, shard, PlacementState::ACTIVE, out_version,
                                   out_uid)) {
        return ControlStatus::PERSIST_FAILED;
    }
    return ControlStatus::OK;
}

ControlStatus ClusterControl::DeleteGraph(KvTransaction& txn, const std::string& name) {
    if (!store_->HasGraph(name)) return ControlStatus::GRAPH_NOT_FOUND;
    if (!store_->DeleteGraphPlacement(txn, name)) return ControlStatus::PERSIST_FAILED;
    return ControlStatus::OK;
}

ControlStatus ClusterControl::GetPlacement(const std::string& name, GraphPlacement* out) const {
    if (!store_->GetGraphPlacement(name, out)) return ControlStatus::GRAPH_NOT_FOUND;
    return ControlStatus::OK;
}

RouteStatus ClusterControl::Resolve(const std::string& name, int64_t now_ms, RouteTarget* out) {
    return router_->Resolve(name, now_ms, out);
}

bool ClusterControl::Fence(const std::string& name, PlacementVersion expected,
                           PlacementVersion* current) const {
    GraphPlacement p;
    if (!store_->GetGraphPlacement(name, &p)) return false;
    if (current) *current = p.placement_version;
    // Reject a sender that is behind the authoritative placement. A sender at
    // the same (or a newer, e.g. mid-move) version is allowed.
    return expected >= p.placement_version;
}

}  // namespace cluster
}  // namespace lgraph
