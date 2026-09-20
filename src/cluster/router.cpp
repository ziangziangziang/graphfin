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

#include "cluster/router.h"

namespace lgraph {
namespace cluster {

const char* ToString(RouteStatus s) {
    switch (s) {
    case RouteStatus::OK: return "OK";
    case RouteStatus::GRAPH_NOT_FOUND: return "GRAPH_NOT_FOUND";
    case RouteStatus::PLACEMENT_NOT_ACTIVE: return "PLACEMENT_NOT_ACTIVE";
    case RouteStatus::NO_HEALTHY_SHARD: return "NO_HEALTHY_SHARD";
    case RouteStatus::STALE_PLACEMENT: return "STALE_PLACEMENT";
    }
    return "UNKNOWN";
}

Router::Router(ClusterMetaStore* store, ShardManager* shards, ShardLocator* locator)
    : Router(store, shards, locator, Config{}) {}

Router::Router(ClusterMetaStore* store, ShardManager* shards, ShardLocator* locator,
               Config config)
    : store_(store), shards_(shards), locator_(locator), config_(config) {}

RouteStatus Router::Resolve(const std::string& graph, int64_t now_ms, RouteTarget* out) {
    GraphPlacement placement;
    GraphId gid = 0;
    if (!store_->GetGraphPlacement(graph, &placement, &gid)) {
        return RouteStatus::GRAPH_NOT_FOUND;
    }

    out->graph_id = gid;
    out->shard_id = placement.shard_id;
    out->version = placement.placement_version;

    // Only ACTIVE placements serve traffic.
    if (placement.State() != PlacementState::ACTIVE) {
        return RouteStatus::PLACEMENT_NOT_ACTIVE;
    }

    // Shard health/existence is checked on EVERY resolve, including cache hits,
    // so marking a shard OFFLINE (or changing its endpoints, detected via the
    // shard config version) takes effect immediately rather than waiting for
    // the cache TTL. ShardManager health is an in-memory check.
    ShardInfo shard;
    if (!store_->GetShard(placement.shard_id, &shard)) {
        return RouteStatus::NO_HEALTHY_SHARD;
    }
    if (!shards_->IsHealthy(placement.shard_id, now_ms)) {
        return RouteStatus::NO_HEALTHY_SHARD;
    }

    // Cache hit: same placement version, same shard config version, not expired.
    {
        std::lock_guard<std::mutex> l(mtx_);
        auto it = cache_.find(gid);
        if (it != cache_.end() && it->second.version == placement.placement_version &&
            it->second.shard_config_version == shard.config_version &&
            now_ms < it->second.expires_ms && !it->second.endpoint.empty()) {
            out->endpoint = it->second.endpoint;
            return RouteStatus::OK;
        }
    }

    // Leader discovery may become a network call, so it runs WITHOUT the router
    // mutex held (a slow lookup must not block unrelated cache reads).
    std::string endpoint;
    if (locator_) endpoint = locator_->LocateLeader(placement.shard_id);
    if (endpoint.empty() && !shard.endpoints.empty()) endpoint = shard.endpoints.front();
    if (endpoint.empty()) {
        return RouteStatus::NO_HEALTHY_SHARD;
    }
    out->endpoint = endpoint;

    {
        std::lock_guard<std::mutex> l(mtx_);
        if (cache_.size() >= config_.max_cache_entries) cache_.clear();
        CacheEntry e;
        e.shard_id = placement.shard_id;
        e.version = placement.placement_version;
        e.shard_config_version = shard.config_version;
        e.expires_ms = now_ms + config_.cache_ttl_ms;
        e.endpoint = endpoint;
        cache_[gid] = std::move(e);
    }
    return RouteStatus::OK;
}

RouteStatus Router::Validate(const std::string& graph, PlacementVersion seen, int64_t now_ms,
                             RouteTarget* out) {
    RouteStatus st = Resolve(graph, now_ms, out);
    if (st != RouteStatus::OK) return st;
    if (out->version != seen) return RouteStatus::STALE_PLACEMENT;
    return RouteStatus::OK;
}

void Router::Invalidate(const std::string& graph) {
    std::lock_guard<std::mutex> l(mtx_);
    GraphPlacement placement;
    GraphId gid = 0;
    if (store_->GetGraphPlacement(graph, &placement, &gid)) cache_.erase(gid);
}

void Router::InvalidateAll() {
    std::lock_guard<std::mutex> l(mtx_);
    cache_.clear();
}

size_t Router::CacheSize() const {
    std::lock_guard<std::mutex> l(mtx_);
    return cache_.size();
}

Router::Config Router::GetConfig() const {
    std::lock_guard<std::mutex> l(mtx_);
    return config_;
}

}  // namespace cluster
}  // namespace lgraph
