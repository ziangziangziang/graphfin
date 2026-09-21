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
#include <cstring>

#include "fma-common/binary_buffer.h"

#include "cluster/migration_manager.h"

namespace lgraph {
namespace cluster {

namespace {

int64_t DefaultClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string UidKey(uint64_t graph_uid) {
    std::string k(8, '\0');
    for (int i = 7; i >= 0; i--) {
        k[7 - i] = static_cast<char>((graph_uid >> (i * 8)) & 0xFF);
    }
    return k;
}

}  // namespace

const char* ToString(MigrationState s) {
    switch (s) {
    case MigrationState::NONE: return "NONE";
    case MigrationState::SCHEDULED: return "SCHEDULED";
    case MigrationState::SNAPSHOTTING: return "SNAPSHOTTING";
    case MigrationState::COPYING: return "COPYING";
    case MigrationState::CATCHING_UP: return "CATCHING_UP";
    case MigrationState::PREPARE_CUTOVER: return "PREPARE_CUTOVER";
    case MigrationState::CUTOVER: return "CUTOVER";
    case MigrationState::VALIDATING: return "VALIDATING";
    case MigrationState::CLEANUP: return "CLEANUP";
    case MigrationState::COMPLETED: return "COMPLETED";
    case MigrationState::FAILED: return "FAILED";
    case MigrationState::ROLLBACK: return "ROLLBACK";
    }
    return "UNKNOWN";
}

bool MigrationManager::IsTerminal(MigrationState s) {
    return s == MigrationState::COMPLETED || s == MigrationState::FAILED;
}

bool MigrationManager::CanTransition(MigrationState from, MigrationState to) {
    switch (from) {
    case MigrationState::NONE:
        return to == MigrationState::SCHEDULED;
    case MigrationState::SCHEDULED:
        return to == MigrationState::SNAPSHOTTING || to == MigrationState::FAILED;
    case MigrationState::SNAPSHOTTING:
        return to == MigrationState::COPYING || to == MigrationState::FAILED;
    case MigrationState::COPYING:
        return to == MigrationState::CATCHING_UP || to == MigrationState::FAILED;
    case MigrationState::CATCHING_UP:
        return to == MigrationState::PREPARE_CUTOVER || to == MigrationState::FAILED;
    case MigrationState::PREPARE_CUTOVER:
        return to == MigrationState::CUTOVER || to == MigrationState::FAILED;
    case MigrationState::CUTOVER:
        return to == MigrationState::VALIDATING || to == MigrationState::ROLLBACK ||
               to == MigrationState::FAILED;
    case MigrationState::VALIDATING:
        return to == MigrationState::CLEANUP || to == MigrationState::ROLLBACK ||
               to == MigrationState::FAILED;
    case MigrationState::CLEANUP:
        return to == MigrationState::COMPLETED || to == MigrationState::FAILED;
    case MigrationState::ROLLBACK:
        return to == MigrationState::FAILED;
    case MigrationState::COMPLETED:
    case MigrationState::FAILED:
        return false;
    }
    return false;
}

MigrationManager::MigrationManager(KvStore* store, Config config)
    : store_(store), config_(config) {}

void MigrationManager::SetClock(std::function<int64_t()> clock) {
    std::lock_guard<std::mutex> l(mtx_);
    clock_ = std::move(clock);
}

int64_t MigrationManager::Now() const {
    return clock_ ? clock_() : DefaultClockMs();
}

void MigrationManager::Init(KvTransaction& txn, bool create_if_not_exist) {
    std::lock_guard<std::mutex> lock(mtx_);
    table_ = store_->OpenTable(txn, config_.migration_table, create_if_not_exist,
                               ComparatorDesc::DefaultComparator());
    migrations_.clear();
    next_id_ = 1;
    auto it = table_->GetIterator(txn);
    for (it->GotoFirstKey(); it->IsValid(); it->Next()) {
        Record r;
        fma_common::BinaryBuffer buf(it->GetValue().Data(), it->GetValue().Size());
        r.Deserialize(buf);
        if (r.graph_uid == 0) continue;
        auto emplaced = migrations_.emplace(r.graph_uid, r);
        if (!emplaced.second) emplaced.first->second = r;
        next_id_ = std::max(next_id_, r.migration_id + 1);
    }
}

void MigrationManager::PersistLocked(KvTransaction& txn, const Record& r) {
    fma_common::BinaryBuffer buf;
    r.Serialize(buf);
    table_->SetValue(txn, Value::ConstRef(UidKey(r.graph_uid)),
                     Value(buf.GetBuf(), buf.GetSize()));
}

void MigrationManager::DeleteRowLocked(KvTransaction& txn, uint64_t graph_uid) {
    table_->DeleteKey(txn, Value::ConstRef(UidKey(graph_uid)));
}

bool MigrationManager::Begin(KvTransaction& txn, uint64_t graph_uid, ShardId src, ShardId dst,
                             PlacementVersion placement_version, uint64_t* out_id) {
    if (graph_uid == 0 || src == dst || src == INVALID_SHARD_ID ||
        dst == INVALID_SHARD_ID) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mtx_);
    if (!table_) return false;
    auto it = migrations_.find(graph_uid);
    if (it != migrations_.end() && !IsTerminal(it->second.state)) return false;
    Record r;
    r.migration_id = next_id_++;
    r.graph_uid = graph_uid;
    r.src_shard = src;
    r.dst_shard = dst;
    r.state = MigrationState::SCHEDULED;
    r.placement_version = placement_version;
    r.started_ms = Now();
    r.updated_ms = r.started_ms;
    PersistLocked(txn, r);
    migrations_[graph_uid] = r;
    if (out_id) *out_id = r.migration_id;
    return true;
}

bool MigrationManager::Advance(KvTransaction& txn, uint64_t graph_uid, MigrationState to) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!table_) return false;
    auto it = migrations_.find(graph_uid);
    if (it == migrations_.end()) return false;
    if (!CanTransition(it->second.state, to)) return false;
    it->second.state = to;
    it->second.updated_ms = Now();
    PersistLocked(txn, it->second);
    return true;
}

bool MigrationManager::Fail(KvTransaction& txn, uint64_t graph_uid, const std::string& reason) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!table_) return false;
    auto it = migrations_.find(graph_uid);
    if (it == migrations_.end() || IsTerminal(it->second.state)) return false;
    it->second.state = MigrationState::FAILED;
    it->second.error = reason;
    it->second.updated_ms = Now();
    PersistLocked(txn, it->second);
    return true;
}

bool MigrationManager::Cancel(KvTransaction& txn, uint64_t graph_uid) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!table_) return false;
    auto it = migrations_.find(graph_uid);
    if (it == migrations_.end()) return false;
    switch (it->second.state) {
    case MigrationState::SCHEDULED:
    case MigrationState::SNAPSHOTTING:
    case MigrationState::COPYING:
    case MigrationState::CATCHING_UP:
    case MigrationState::PREPARE_CUTOVER:
        it->second.state = MigrationState::FAILED;
        it->second.error = "cancelled";
        it->second.updated_ms = Now();
        PersistLocked(txn, it->second);
        return true;
    default:
        return false;  // cutover and beyond must roll back, not cancel
    }
}

bool MigrationManager::Prune(KvTransaction& txn, uint64_t graph_uid) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!table_) return false;
    auto it = migrations_.find(graph_uid);
    if (it == migrations_.end()) return false;
    if (!IsTerminal(it->second.state)) return false;
    DeleteRowLocked(txn, graph_uid);
    migrations_.erase(it);
    return true;
}

bool MigrationManager::ReportProgress(KvTransaction& txn, uint64_t graph_uid, double fraction,
                                        uint64_t bytes_delta) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!table_) return false;
    auto it = migrations_.find(graph_uid);
    if (it == migrations_.end()) return false;
    Record& r = it->second;
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;
    r.progress = fraction;
    r.bytes_transferred += bytes_delta;
    r.updated_ms = Now();
    PersistLocked(txn, r);
    return true;
}

bool MigrationManager::RecordFailure(KvTransaction& txn, uint64_t graph_uid, bool retried) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!table_) return false;
    auto it = migrations_.find(graph_uid);
    if (it == migrations_.end()) return false;
    it->second.failures += 1;
    if (retried) it->second.retries += 1;
    it->second.updated_ms = Now();
    PersistLocked(txn, it->second);
    return true;
}

bool MigrationManager::Get(uint64_t graph_uid, Record* out) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = migrations_.find(graph_uid);
    if (it == migrations_.end()) return false;
    if (out) *out = it->second;
    return true;
}

std::vector<MigrationManager::Record> MigrationManager::ListActive() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<Record> out;
    for (const auto& kv : migrations_) {
        if (!IsTerminal(kv.second.state)) out.push_back(kv.second);
    }
    std::sort(out.begin(), out.end(), [](const Record& a, const Record& b) {
        return a.graph_uid < b.graph_uid;
    });
    return out;
}

size_t MigrationManager::ActiveCount() const {
    return ListActive().size();
}

std::vector<RebalanceMove> PlanRebalanceMoves(const std::vector<RebalanceInput>& shards,
                                              size_t max_moves) {
    struct Node {
        ShardId shard = INVALID_SHARD_ID;
        long long graphs = 0;
        uint32_t weight = 1;
        double disk = 0.0;
    };
    std::vector<Node> v;
    v.reserve(shards.size());
    for (const auto& s : shards) {
        Node n;
        n.shard = s.shard;
        n.graphs = static_cast<long long>(s.graphs);
        n.weight = s.weight == 0 ? 1 : s.weight;
        n.disk = s.disk_used;
        v.push_back(n);
    }
    auto load = [](const Node& n) { return (double)n.graphs / (double)n.weight; };
    auto peak = [&]() {
        double p = 0.0;
        for (const auto& n : v) p = std::max(p, load(n));
        return p;
    };

    std::vector<RebalanceMove> moves;
    while (moves.size() < max_moves && v.size() >= 2) {
        // Source: highest load with at least one graph (ties: lowest shard).
        // Destination: lowest load among the rest (ties: lowest shard).
        size_t si = v.size(), di = v.size();
        for (size_t i = 0; i < v.size(); i++) {
            if (v[i].graphs <= 0) continue;
            if (si == v.size() || load(v[i]) > load(v[si]) ||
                (load(v[i]) == load(v[si]) &&
                 (v[i].disk > v[si].disk ||
                  (v[i].disk == v[si].disk && v[i].shard < v[si].shard)))) {
                si = i;
            }
        }
        if (si == v.size()) break;
        for (size_t i = 0; i < v.size(); i++) {
            if (i == si) continue;
            if (di == v.size() || load(v[i]) < load(v[di]) ||
                (load(v[i]) == load(v[di]) &&
                 (v[i].disk < v[di].disk ||
                  (v[i].disk == v[di].disk && v[i].shard < v[di].shard)))) {
                di = i;
            }
        }
        if (di == v.size()) break;

        double before = peak();
        v[si].graphs--;
        v[di].graphs++;
        if (peak() >= before) {
            v[si].graphs++;  // revert: no strict improvement
            v[di].graphs--;
            break;
        }
        moves.push_back({v[si].shard, v[di].shard});
    }
    return moves;
}

}  // namespace cluster
}  // namespace lgraph
