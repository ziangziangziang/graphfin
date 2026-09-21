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

#include <cstdint>
#include <string>
#include <vector>

#include "core/kv_engine.h"

#include "cluster/cluster_meta_store.h"
#include "cluster/router.h"
#include "cluster/shard_manager.h"

namespace lgraph {
namespace cluster {

enum class ControlStatus {
    OK = 0,
    GRAPH_EXISTS,
    GRAPH_NOT_FOUND,
    NO_HEALTHY_SHARD,
    STALE_PLACEMENT,
    PERSIST_FAILED,
};

const char* ToString(ControlStatus s);

/**
 * @brief   Control-plane facade for whole-graph sharding.
 *
 * Composes the Phase 4 pieces into the operations a server or admin procedure
 * needs, and — critically — provides the **receiver-side placement fence** the
 * review requires: a shard must reject a forwarded write whose placement
 * version is behind the authoritative placement, because router-side version
 * checks alone cannot stop an obsolete destination from accepting writes.
 *
 * It performs no I/O of its own; callers own the KvTransaction and must call
 * ClusterMetaStore::CommitStaged() after a successful commit (see
 * cluster_meta_store.h).
 */
class ClusterControl {
 public:
    ClusterControl(ClusterMetaStore* store, ShardManager* shards, Router* router);

    // ---- shards ----

    /** Register/replace a shard (validated). */
    ControlStatus RegisterShard(KvTransaction& txn, const ShardInfo& info);
    ControlStatus RemoveShard(KvTransaction& txn, ShardId id);
    std::vector<ShardInfo> ListShards() const;

    // ---- graph lifecycle ----

    /**
     * Create a logical graph and place it on a healthy shard chosen by the
     * configured placement strategy. Fails with NO_HEALTHY_SHARD if none is
     * available.
     */
    ControlStatus CreateGraph(KvTransaction& txn, const std::string& name, int64_t now_ms,
                              PlacementVersion* out_version = nullptr,
                              uint64_t* out_uid = nullptr);
    ControlStatus DeleteGraph(KvTransaction& txn, const std::string& name);

    /**
     * Begin a migration: mark the graph MOVING on its current shard. The
     * graph stops routing (MOVING is not ACTIVE) while data moves; the
     * destination must be an existing, ONLINE, healthy shard different from
     * the current one.
     */
    ControlStatus BeginMove(KvTransaction& txn, const std::string& name, ShardId dst,
                            int64_t now_ms, PlacementVersion* out_version = nullptr);
    /**
     * Complete a migration: flip a MOVING graph to ACTIVE on `dst`. The caller
     * performs data transfer/validation first; this is the atomic placement
     * cutover that makes the destination writable and the source stale.
     */
    ControlStatus CompleteMove(KvTransaction& txn, const std::string& name, ShardId dst,
                               PlacementVersion* out_version = nullptr);
    /**
     * Abort a migration: return a MOVING graph to ACTIVE on its current shard.
     */
    ControlStatus AbortMove(KvTransaction& txn, const std::string& name,
                            PlacementVersion* out_version = nullptr);

    /** Current placement of a graph. */
    ControlStatus GetPlacement(const std::string& name, GraphPlacement* out) const;

    // ---- routing ----

    /** Resolve a logical graph to its shard/leader endpoint (cached, versioned). */
    RouteStatus Resolve(const std::string& name, int64_t now_ms, RouteTarget* out);

    // ---- receiver-side fencing ----

    /**
     * Accept or reject an operation forwarded to a shard. `expected` is the
     * placement version the sender routed with; the shard rejects (returns
     * false) when its authoritative placement is newer, i.e. the sender is
     * stale. This must be checked at the receiving shard's write boundary.
     */
    bool Fence(const std::string& name, PlacementVersion expected,
               PlacementVersion* current = nullptr) const;

    /**
     * Destination-aware receiver-side fence: the operation arrived at `shard`.
     * Rejects when the shard does not host the graph OR when the sender is
     * behind the authoritative placement. `current` receives the authoritative
     * version so the caller can route the retry correctly.
     */
    bool FenceAt(ShardId shard, const std::string& name, PlacementVersion expected,
                 PlacementVersion* current = nullptr) const;

    // ---- admin surface ----

    /** Graph names currently placed (non-deleted) on `shard`, sorted. */
    std::vector<std::string> ListGraphsOnShard(ShardId shard) const;
    /** All non-deleted graph names, sorted. */
    std::vector<std::string> ListAllGraphs() const;

 private:
    ClusterMetaStore* store_;
    ShardManager* shards_;
    Router* router_;
};

}  // namespace cluster
}  // namespace lgraph
