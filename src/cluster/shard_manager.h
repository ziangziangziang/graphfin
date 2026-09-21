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
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/kv_engine.h"

#include "cluster/cluster_meta_store.h"

namespace lgraph {
namespace cluster {

/**
 * @brief   Shard lifecycle on top of ClusterMetaStore: registration with
 *          validation, liveness (heartbeat/health), and selection of a shard
 *          for a new graph.
 *
 * Phase 4B. Health is deliberately kept OUT of the durable catalog — it is
 * ephemeral, per-node, and derived from the heartbeat stream — so the
 * persisted ShardInfo stays immutable-ish and the memory cost is one
 * timestamp per shard (a handful per cluster).
 *
 * Placement selection is O(#shards) using ClusterMetaStore's incremental
 * per-shard graph counts, not an O(#graphs) scan.
 */
class ShardManager {
 public:
    enum class PlacementStrategy : uint8_t {
        LEAST_GRAPH_COUNT = 0,   // fewest graphs, tie-break lowest shard id
        ROUND_ROBIN = 1,
        WEIGHTED_LEAST_LOAD = 2, // min(graph_count / capacity_weight)
    };

    struct Config {
        // A shard with no heartbeat within this window is considered unhealthy.
        int64_t heartbeat_timeout_ms = 30000;
        PlacementStrategy strategy = PlacementStrategy::LEAST_GRAPH_COUNT;
    };

    explicit ShardManager(ClusterMetaStore* store);
    ShardManager(ClusterMetaStore* store, Config config);

    // ---- registration ---------------------------------------------------

    /**
     * Register or refresh a shard. Rejects an invalid shard id, empty name, or
     * empty endpoints. On success the shard's heartbeat is set to now.
     */
    bool RegisterShard(KvTransaction& txn, const ShardInfo& info,
                       std::string* error = nullptr);
    /** Remove a shard. Refused by the store while live graphs remain on it. */
    bool DeregisterShard(KvTransaction& txn, ShardId id);
    bool SetShardState(KvTransaction& txn, ShardId id, ShardState state);

    // ---- liveness -------------------------------------------------------

    /** Record that the shard was heard from at the current time. */
    void Heartbeat(ShardId id);
    bool IsHealthy(ShardId id, int64_t now_ms) const;
    int64_t LastHeartbeat(ShardId id) const;
    /** Online + healthy shards, in shard-id order. */
    std::vector<ShardInfo> HealthyShards(int64_t now_ms) const;

    // ---- placement ------------------------------------------------------

    /**
     * Choose a shard for a new graph among ONLINE, healthy shards according to
     * the configured strategy. Returns false if none is available.
     */
    bool PickShard(int64_t now_ms, ShardId* out);

    // ---- read-through ---------------------------------------------------

    bool GetShard(ShardId id, ShardInfo* out) const { return store_->GetShard(id, out); }
    std::vector<ShardInfo> ListShards() const { return store_->ListShards(); }
    size_t ShardCount() const { return store_->ShardCount(); }

    // ---- testing hooks --------------------------------------------------

    /** Override the clock (milliseconds). Pass {} to restore the default. */
    void SetClock(std::function<int64_t()> clock);
    void SetStrategy(PlacementStrategy strategy);
    PlacementStrategy GetStrategy() const;

 private:
    int64_t Now() const;

    ClusterMetaStore* store_;
    Config config_;
    mutable std::mutex mtx_;
    std::unordered_map<ShardId, int64_t> last_heartbeat_ms_;
    uint64_t rr_cursor_ = 0;
    std::function<int64_t()> clock_;
};

/** All supported placement strategies (for admin listing). */
inline std::vector<ShardManager::PlacementStrategy> PlacementStrategies() {
    return {ShardManager::PlacementStrategy::LEAST_GRAPH_COUNT,
            ShardManager::PlacementStrategy::ROUND_ROBIN,
            ShardManager::PlacementStrategy::WEIGHTED_LEAST_LOAD};
}

/** Short name for a strategy (for admin/logging surfaces). */
inline const char* PlacementStrategyName(ShardManager::PlacementStrategy s) {
    switch (s) {
    case ShardManager::PlacementStrategy::LEAST_GRAPH_COUNT: return "least_graph_count";
    case ShardManager::PlacementStrategy::ROUND_ROBIN: return "round_robin";
    case ShardManager::PlacementStrategy::WEIGHTED_LEAST_LOAD: return "weighted_least_load";
    }
    return "unknown";
}

}  // namespace cluster
}  // namespace lgraph
