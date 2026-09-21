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
#include <mutex>
#include <string>
#include <unordered_map>

#include "cluster/cluster_meta_store.h"
#include "cluster/shard_manager.h"

namespace lgraph {
namespace cluster {

/** Where a graph currently lives and how to reach it. */
struct RouteTarget {
    GraphId graph_id = 0;
    ShardId shard_id = INVALID_SHARD_ID;
    std::string endpoint;              // shard leader endpoint to forward to
    PlacementVersion version = 0;      // placement version this target reflects
    uint64_t unique_id = 0;            // graph incarnation (R3); receiver must match
};

enum class RouteStatus {
    OK = 0,
    GRAPH_NOT_FOUND,      // no such logical graph (or tombstoned)
    PLACEMENT_NOT_ACTIVE, // graph exists but is CREATING/MOVING/DELETING
    NO_HEALTHY_SHARD,     // placement exists but its shard is unavailable
    STALE_PLACEMENT,      // caller's placement_version is behind the catalog
};

const char* ToString(RouteStatus s);

/**
 * Resolves a shard's current leader endpoint. Phase 4C abstracts this so the
 * routing logic is testable without a live cluster; the production
 * implementation queries the shard (e.g. dbms.ha.clusterInfo) and caches the
 * result. Returning "" means "unknown" and the router falls back to the
 * shard's first registered endpoint.
 */
class ShardLocator {
 public:
    virtual ~ShardLocator() = default;
    virtual std::string LocateLeader(ShardId shard) = 0;
};

/**
 * @brief   Transparent routing of logical graph requests to the owning shard.
 *
 * Phase 4C. The router answers "which shard/leader serves graph X" from the
 * Phase 4A catalog, cached in a bounded, version-checked table. It does NOT
 * itself move bytes: callers use the returned endpoint to forward. This keeps
 * routing decisions unit-testable and the router stateless enough to scale
 * horizontally (a router loss cannot affect persistent graph state).
 *
 * Memory: the cache holds only hot graphs and is hard-capped (default 64k
 * entries); when full it is dropped wholesale rather than growing unbounded.
 * Compare with the catalog itself, which is the durable source of truth.
 */
class Router {
 public:
    struct Config {
        // A resolved route is trusted for this long before revalidation.
        int64_t cache_ttl_ms = 5000;
        // Hard cap on cached routes (bounds router memory).
        size_t max_cache_entries = 65536;
    };

    Router(ClusterMetaStore* store, ShardManager* shards, ShardLocator* locator);
    Router(ClusterMetaStore* store, ShardManager* shards, ShardLocator* locator,
           Config config);

    /**
     * Resolve a logical graph to its shard and leader endpoint.
     * `now_ms` is the caller's clock (injectable for tests).
     */
    RouteStatus Resolve(const std::string& graph, int64_t now_ms, RouteTarget* out);

    /**
     * Validate a request tagged with the incarnation + placement version the
     * caller last saw (R3/R4). UID mismatch (recreate) or version mismatch
     * (move) returns STALE_PLACEMENT and still fills `out` with the current
     * target so the caller can retry there.
     */
    RouteStatus Validate(const std::string& graph, uint64_t expected_uid,
                         PlacementVersion seen, int64_t now_ms, RouteTarget* out);

    /** Drop cached routing for one graph / all graphs. */
    void Invalidate(const std::string& graph);
    void InvalidateAll();

    size_t CacheSize() const;
    Config GetConfig() const;

 private:
    struct CacheEntry {
        ShardId shard_id = INVALID_SHARD_ID;
        PlacementVersion version = 0;
        uint64_t unique_id = 0;             // R3: cached route is bound to incarnation
        uint64_t shard_config_version = 0;  // detects endpoint/state changes
        int64_t expires_ms = 0;
        std::string endpoint;
    };

    ClusterMetaStore* store_;
    ShardManager* shards_;
    ShardLocator* locator_;
    Config config_;
    mutable std::mutex mtx_;
    std::unordered_map<GraphId, CacheEntry> cache_;
};

}  // namespace cluster
}  // namespace lgraph
