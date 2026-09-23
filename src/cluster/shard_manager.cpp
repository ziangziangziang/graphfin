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
#include <chrono>
#include <limits>

#include "cluster/shard_manager.h"

namespace lgraph {
namespace cluster {

namespace {

int64_t DefaultClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

ShardManager::ShardManager(ClusterMetaStore* store) : ShardManager(store, Config{}) {}

ShardManager::ShardManager(ClusterMetaStore* store, Config config)
    : store_(store), config_(config) {}

int64_t ShardManager::Now() const {
    return clock_ ? clock_() : DefaultClockMs();
}

void ShardManager::SetClock(std::function<int64_t()> clock) {
    std::lock_guard<std::mutex> l(mtx_);
    clock_ = std::move(clock);
}

void ShardManager::SetStrategy(PlacementStrategy strategy) {
    std::lock_guard<std::mutex> l(mtx_);
    config_.strategy = strategy;
}

ShardManager::PlacementStrategy ShardManager::GetStrategy() const {
    std::lock_guard<std::mutex> l(mtx_);
    return config_.strategy;
}

bool ShardManager::RegisterShard(ClusterMetaStore::Batch& batch, const ShardInfo& info,
                                 std::string* error) {
    if (info.shard_id == INVALID_SHARD_ID) {
        if (error) *error = "invalid shard id";
        return false;
    }
    if (info.name.empty()) {
        if (error) *error = "shard name must not be empty";
        return false;
    }
    if (info.endpoints.empty()) {
        if (error) *error = "shard must have at least one endpoint";
        return false;
    }
    for (const auto& e : info.endpoints) {
        if (e.empty()) {
            if (error) *error = "shard endpoint must not be empty";
            return false;
        }
    }
    if (!batch.RegisterShard(info)) {
        if (error) *error = "failed to persist shard";
        return false;
    }
    std::lock_guard<std::mutex> l(mtx_);
    last_heartbeat_ms_[info.shard_id] = Now();
    return true;
}

bool ShardManager::DeregisterShard(ClusterMetaStore::Batch& batch, ShardId id) {
    if (!batch.RemoveShard(id)) return false;
    std::lock_guard<std::mutex> l(mtx_);
    last_heartbeat_ms_.erase(id);
    return true;
}

bool ShardManager::SetShardState(ClusterMetaStore::Batch& batch, ShardId id,
                                ShardState state) {
    return batch.SetShardState(id, state);
}

void ShardManager::Heartbeat(ShardId id) {
    std::lock_guard<std::mutex> l(mtx_);
    last_heartbeat_ms_[id] = Now();
}

bool ShardManager::IsHealthy(ShardId id, int64_t now_ms) const {
    ShardInfo info;
    if (!store_->GetShard(id, &info)) return false;
    if (info.state != ShardState::ONLINE) return false;
    std::lock_guard<std::mutex> l(mtx_);
    auto it = last_heartbeat_ms_.find(id);
    if (it == last_heartbeat_ms_.end()) return false;
    return (now_ms - it->second) <= config_.heartbeat_timeout_ms;
}

int64_t ShardManager::LastHeartbeat(ShardId id) const {
    std::lock_guard<std::mutex> l(mtx_);
    auto it = last_heartbeat_ms_.find(id);
    return it == last_heartbeat_ms_.end() ? -1 : it->second;
}

std::vector<ShardInfo> ShardManager::HealthyShards(int64_t now_ms) const {
    std::vector<ShardInfo> ret;
    for (const auto& s : store_->ListShards()) {
        if (IsHealthy(s.shard_id, now_ms)) ret.push_back(s);
    }
    return ret;  // ListShards is already sorted by shard id
}

bool ShardManager::PickShard(int64_t now_ms, ShardId* out) {
    std::lock_guard<std::mutex> l(mtx_);
    std::vector<ShardInfo> healthy;
    for (const auto& s : store_->ListShards()) {
        auto it = last_heartbeat_ms_.find(s.shard_id);
        if (s.state != ShardState::ONLINE) continue;
        if (it == last_heartbeat_ms_.end()) continue;
        if ((now_ms - it->second) > config_.heartbeat_timeout_ms) continue;
        healthy.push_back(s);
    }
    if (healthy.empty()) return false;

    ShardId chosen = INVALID_SHARD_ID;
    if (config_.strategy == PlacementStrategy::ROUND_ROBIN) {
        chosen = healthy[rr_cursor_ % healthy.size()].shard_id;
        rr_cursor_++;
    } else if (config_.strategy == PlacementStrategy::WEIGHTED_LEAST_LOAD) {
        // Choose min(graph_count / capacity_weight); compare with integer
        // cross-multiplication to avoid floating point.
        size_t best_count = 0;
        uint64_t best_w = 1;
        for (const auto& s : healthy) {
            size_t n = store_->GraphCountOnShard(s.shard_id);
            uint64_t w = s.capacity_weight == 0 ? 1 : s.capacity_weight;
            if (chosen == INVALID_SHARD_ID) {
                chosen = s.shard_id;
                best_count = n;
                best_w = w;
                continue;
            }
            uint64_t lhs = static_cast<uint64_t>(n) * best_w;
            uint64_t rhs = static_cast<uint64_t>(best_count) * w;
            if (lhs < rhs || (lhs == rhs && s.shard_id < chosen)) {
                chosen = s.shard_id;
                best_count = n;
                best_w = w;
            }
        }
    } else {  // LEAST_GRAPH_COUNT
        size_t best = std::numeric_limits<size_t>::max();
        for (const auto& s : healthy) {
            size_t n = store_->GraphCountOnShard(s.shard_id);
            if (n < best || (n == best && s.shard_id < chosen)) {
                best = n;
                chosen = s.shard_id;
            }
        }
    }
    if (chosen == INVALID_SHARD_ID) return false;
    if (out) *out = chosen;
    return true;
}

}  // namespace cluster
}  // namespace lgraph
