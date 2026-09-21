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
  * It performs no I/O of its own; callers own the ClusterMetaStore::Batch
  * (which owns the KvTransaction) and commit via Batch::Commit() (see
  * cluster_meta_store.h).
 */
class ClusterControl {
 public:
    ClusterControl(ClusterMetaStore* store, ShardManager* shards, Router* router);

    // ---- shards ----

    /** Register/replace a shard (validated). Takes the store Batch (R1). */
    ControlStatus RegisterShard(ClusterMetaStore::Batch& batch, const ShardInfo& info);
    ControlStatus RemoveShard(ClusterMetaStore::Batch& batch, ShardId id);
    std::vector<ShardInfo> ListShards() const;

    // ---- graph lifecycle ----

    /**
     * Create a logical graph and place it on a healthy shard chosen by the
     * configured placement strategy. Fails with NO_HEALTHY_SHARD if none is
     * available. Batch-aware: sees own writes (R8).
     */
    ControlStatus CreateGraph(ClusterMetaStore::Batch& batch, const std::string& name,
                              int64_t now_ms, PlacementVersion* out_version = nullptr,
                              uint64_t* out_uid = nullptr);
    ControlStatus DeleteGraph(ClusterMetaStore::Batch& batch, const std::string& name);

    /**
     * Begin a migration: mark the graph MOVING on its current shard. Offline
     * prototype: the graph stops routing (MOVING is not ACTIVE) for the
     * duration of the move. The destination must be an existing, ONLINE,
     * healthy shard different from the current one.
     */
    ControlStatus BeginMove(ClusterMetaStore::Batch& batch, const std::string& name, ShardId dst,
                            int64_t now_ms, PlacementVersion* out_version = nullptr,
                            uint64_t* out_uid = nullptr);
    /**
     * Complete a migration: flip a MOVING graph to ACTIVE on `dst` (R7).
     * Compare-and-set against the attempt tuple: the caller passes the UID
     * and the MOVING placement version observed at Begin; mismatch returns
     * STALE_PLACEMENT so a delayed completion from an aborted attempt can
     * never complete a later attempt. Repeating an already-applied cutover
     * (ACTIVE on dst, same UID) returns OK idempotently.
     */
    ControlStatus CompleteMove(ClusterMetaStore::Batch& batch, const std::string& name,
                               ShardId dst, uint64_t expected_uid,
                               PlacementVersion expected_version,
                               PlacementVersion* out_version = nullptr);
    /**
     * Abort a migration: return a MOVING graph to ACTIVE on its current
     * shard (R7). Bound to the attempt like CompleteMove; a stale abort for
     * an older version never cancels a newer move. Already-aborted (ACTIVE,
     * same UID) returns OK idempotently without mutating.
     */
    ControlStatus AbortMove(ClusterMetaStore::Batch& batch, const std::string& name,
                            uint64_t expected_uid, PlacementVersion expected_version,
                            PlacementVersion* out_version = nullptr);

    /** Current placement of a graph. */
    ControlStatus GetPlacement(const std::string& name, GraphPlacement* out) const;

    // ---- routing ----

    /** Resolve a logical graph to its shard/leader endpoint (cached, versioned). */
    RouteStatus Resolve(const std::string& name, int64_t now_ms, RouteTarget* out);

    // ---- receiver-side fencing ----

    /**
     * Ownership guard for the receiving shard's write boundary (R3/R4).
     * Accepts only when the authoritative placement matches the sender's
     * incarnation AND epoch exactly, lives on the receiving shard (FenceAt),
     * and is in ACTIVE serving state. Rejects stale versions, future
     * versions (catalog lag must refresh, never jump ahead), wrong UIDs
     * (recreate), wrong shards, missing graphs, and non-ACTIVE placements.
     * `current_*` always receive the authoritative values when the graph is
     * found, so the caller can route the retry correctly. A partitioned
     * source holding old metadata cannot pass: it presents an old
     * (uid, version) tuple that never equals the authoritative one.
     */
    bool Fence(const std::string& name, uint64_t expected_uid,
               PlacementVersion expected_version,
               PlacementVersion* current_version = nullptr,
               uint64_t* current_uid = nullptr) const;

    /**
     * Destination-aware ownership guard: the operation arrived at `shard`.
     * Same admission rule as Fence, plus the authoritative owner must equal
     * `shard`.
     */
    bool FenceAt(ShardId shard, const std::string& name, uint64_t expected_uid,
                 PlacementVersion expected_version,
                 PlacementVersion* current_version = nullptr,
                 uint64_t* current_uid = nullptr) const;

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
