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

#include <algorithm>

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

ControlStatus ClusterControl::RegisterShard(ClusterMetaStore::Batch& batch,
                                              const ShardInfo& info) {
    if (!shards_->RegisterShard(batch, info)) return ControlStatus::PERSIST_FAILED;
    return ControlStatus::OK;
}

ControlStatus ClusterControl::RemoveShard(ClusterMetaStore::Batch& batch, ShardId id) {
    if (!shards_->DeregisterShard(batch, id)) return ControlStatus::PERSIST_FAILED;
    return ControlStatus::OK;
}

std::vector<ShardInfo> ClusterControl::ListShards() const { return shards_->ListShards(); }

ControlStatus ClusterControl::CreateGraph(ClusterMetaStore::Batch& batch, const std::string& name,
                                          int64_t now_ms, PlacementVersion* out_version,
                                          uint64_t* out_uid) {
    // Batch-aware duplicate check (R8): two creates in one batch must not both
    // succeed.
    if (batch.HasGraph(name)) return ControlStatus::GRAPH_EXISTS;
    ShardId shard = INVALID_SHARD_ID;
    if (!shards_->PickShard(now_ms, &shard)) return ControlStatus::NO_HEALTHY_SHARD;
    if (!batch.PutGraphPlacement(name, shard, PlacementState::ACTIVE, out_version, out_uid)) {
        return ControlStatus::PERSIST_FAILED;
    }
    return ControlStatus::OK;
}

ControlStatus ClusterControl::DeleteGraph(ClusterMetaStore::Batch& batch, const std::string& name) {
    if (!batch.HasGraph(name)) return ControlStatus::GRAPH_NOT_FOUND;
    if (!batch.DeleteGraphPlacement(name)) return ControlStatus::PERSIST_FAILED;
    return ControlStatus::OK;
}

ControlStatus ClusterControl::BeginMove(ClusterMetaStore::Batch& batch, const std::string& name,
                                        ShardId dst, int64_t now_ms,
                                        PlacementVersion* out_version, uint64_t* out_uid) {
    GraphPlacement cur;
    if (!batch.GetGraphPlacement(name, &cur)) return ControlStatus::GRAPH_NOT_FOUND;
    if (cur.State() != PlacementState::ACTIVE) return ControlStatus::PERSIST_FAILED;
    if (dst == cur.shard_id) return ControlStatus::PERSIST_FAILED;
    ShardInfo info;
    if (!batch.GetShard(dst, &info) || info.state != ShardState::ONLINE) {
        return ControlStatus::NO_HEALTHY_SHARD;
    }
    if (!shards_->IsHealthy(dst, now_ms)) return ControlStatus::NO_HEALTHY_SHARD;
    PlacementVersion v = 0;
    if (!batch.PutGraphPlacement(name, cur.shard_id, PlacementState::MOVING, &v)) {
        return ControlStatus::PERSIST_FAILED;
    }
    if (out_version) *out_version = v;
    if (out_uid) *out_uid = cur.unique_id;
    return ControlStatus::OK;
}

ControlStatus ClusterControl::CompleteMove(ClusterMetaStore::Batch& batch, const std::string& name,
                                           ShardId dst, uint64_t expected_uid,
                                           PlacementVersion expected_version,
                                           PlacementVersion* out_version) {
    GraphPlacement cur;
    if (!batch.GetGraphPlacement(name, &cur)) return ControlStatus::GRAPH_NOT_FOUND;
    // Idempotent replay: already cut over to dst with this incarnation.
    if (cur.State() == PlacementState::ACTIVE && cur.shard_id == dst &&
        cur.unique_id == expected_uid) {
        if (out_version) *out_version = cur.placement_version;
        return ControlStatus::OK;
    }
    // Bind to the attempt: UID and the MOVING version from Begin must match.
    if (cur.unique_id != expected_uid || cur.placement_version != expected_version) {
        if (out_version) *out_version = cur.placement_version;
        return ControlStatus::STALE_PLACEMENT;
    }
    if (cur.State() != PlacementState::MOVING) return ControlStatus::PERSIST_FAILED;
    ShardInfo info;
    if (!batch.GetShard(dst, &info) || info.state != ShardState::ONLINE) {
        return ControlStatus::NO_HEALTHY_SHARD;
    }
    PlacementVersion v = 0;
    if (!batch.PutGraphPlacement(name, dst, PlacementState::ACTIVE, &v)) {
        return ControlStatus::PERSIST_FAILED;
    }
    if (out_version) *out_version = v;
    return ControlStatus::OK;
}

ControlStatus ClusterControl::AbortMove(ClusterMetaStore::Batch& batch, const std::string& name,
                                        uint64_t expected_uid, PlacementVersion expected_version,
                                        PlacementVersion* out_version) {
    GraphPlacement cur;
    if (!batch.GetGraphPlacement(name, &cur)) return ControlStatus::GRAPH_NOT_FOUND;
    // Idempotent replay: already ACTIVE with this incarnation (aborted
    // before, or never left ACTIVE). No mutation; report current version.
    if (cur.State() == PlacementState::ACTIVE && cur.unique_id == expected_uid) {
        if (out_version) *out_version = cur.placement_version;
        return ControlStatus::OK;
    }
    // Bind to the attempt so a stale abort cannot cancel a newer move.
    if (cur.unique_id != expected_uid || cur.placement_version != expected_version) {
        if (out_version) *out_version = cur.placement_version;
        return ControlStatus::STALE_PLACEMENT;
    }
    if (cur.State() != PlacementState::MOVING) return ControlStatus::PERSIST_FAILED;
    PlacementVersion v = 0;
    if (!batch.PutGraphPlacement(name, cur.shard_id, PlacementState::ACTIVE, &v)) {
        return ControlStatus::PERSIST_FAILED;
    }
    if (out_version) *out_version = v;
    return ControlStatus::OK;
}

ControlStatus ClusterControl::GetPlacement(const std::string& name, GraphPlacement* out) const {
    if (!store_->GetGraphPlacement(name, out)) return ControlStatus::GRAPH_NOT_FOUND;
    return ControlStatus::OK;
}

RouteStatus ClusterControl::Resolve(const std::string& name, int64_t now_ms, RouteTarget* out) {
    return router_->Resolve(name, now_ms, out);
}

bool ClusterControl::Fence(const std::string& name, uint64_t expected_uid,
                           PlacementVersion expected_version,
                           PlacementVersion* current_version, uint64_t* current_uid) const {
    GraphPlacement p;
    GraphId id = 0;
    if (!store_->GetGraphPlacement(name, &p, &id)) return false;
    return FenceAt(p.shard_id, name, expected_uid, expected_version, current_version,
                   current_uid);
}

bool ClusterControl::FenceAt(ShardId shard, const std::string& name, uint64_t expected_uid,
                             PlacementVersion expected_version,
                             PlacementVersion* current_version, uint64_t* current_uid) const {
    GraphPlacement p;
    GraphId id = 0;
    if (!store_->GetGraphPlacement(name, &p, &id)) return false;
    // Always report the authoritative tuple (so callers can route the retry),
    // even when rejecting.
    if (current_version) *current_version = p.placement_version;
    if (current_uid) *current_uid = p.unique_id;
    // Ownership guard: exact incarnation, exact epoch, exact owner, and the
    // placement must be serving. A lagging source (old tuple) and a source
    // racing ahead (future tuple, e.g. catalog lag the other way) both fail
    // closed and refresh from the reported authoritative tuple.
    if (p.unique_id != expected_uid) return false;
    if (p.placement_version != expected_version) return false;
    if (p.shard_id != shard) return false;
    if (p.State() != PlacementState::ACTIVE) return false;
    return true;
}

std::vector<std::string> ClusterControl::ListGraphsOnShard(ShardId shard) const {
    std::vector<std::string> out;
    store_->ForEachGraph([&](GraphId, const std::string& name, const GraphPlacement& p) {
        if (p.shard_id == shard) out.push_back(name);
    });
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> ClusterControl::ListAllGraphs() const {
    std::vector<std::string> out;
    store_->ForEachGraph(
        [&](GraphId, const std::string& name, const GraphPlacement&) { out.push_back(name); });
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace cluster
}  // namespace lgraph
