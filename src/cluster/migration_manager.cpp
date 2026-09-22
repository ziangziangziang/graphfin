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

// R7: allocator row key. 8 bytes of 0xFF can never collide with a real
// graph_uid (allocator starts at 1), and survives Prune() so migration ids
// are never reused even after terminal records are cleaned up.
std::string NextIdKey() { return std::string(8, '\xFF'); }

Value EncodeU64Value(uint64_t v) { return Value(&v, sizeof(v)); }

uint64_t DecodeU64Value(const Value& val) {
    uint64_t v = 0;
    if (val.Size() >= sizeof(v)) std::memcpy(&v, val.Data(), sizeof(v));
    return v;
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
    // R7: post-cutover failures must roll back (CUTOVER/VALIDATING/CLEANUP ->
    // ROLLBACK -> FAILED), never jump terminal directly. A terminal FAILED
    // after cutover would let another migration start before ownership and
    // cleanup are reconciled.
    case MigrationState::CUTOVER:
        return to == MigrationState::VALIDATING || to == MigrationState::ROLLBACK;
    case MigrationState::VALIDATING:
        return to == MigrationState::CLEANUP || to == MigrationState::ROLLBACK;
    case MigrationState::CLEANUP:
        return to == MigrationState::COMPLETED;
    case MigrationState::ROLLBACK:
        return to == MigrationState::FAILED;
    case MigrationState::COMPLETED:
    case MigrationState::FAILED:
        return false;
    }
    return false;
}

MigrationManager::MigrationManager(KvStore* store) : MigrationManager(store, Config{}) {}

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
        const Value k = it->GetKey();
        if (k.Size() == 8 && k.AsString() == NextIdKey()) continue;  // allocator row
        Record r;
        fma_common::BinaryBuffer buf(it->GetValue().Data(), it->GetValue().Size());
        r.Deserialize(buf);
        if (r.graph_uid == 0) continue;
        auto emplaced = migrations_.emplace(r.graph_uid, r);
        if (!emplaced.second) emplaced.first->second = r;
        next_id_ = std::max(next_id_, r.migration_id + 1);
    }
    // The persisted allocator survives Prune(); take the max so ids are never
    // reused after cleanup.
    {
        Value v;
        if (table_->GetValue(txn, Value::ConstRef(NextIdKey()), v)) {
            next_id_ = std::max(next_id_, DecodeU64Value(v));
        }
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

void MigrationManager::PersistNextIdLocked(KvTransaction& txn, uint64_t next_id) {
    table_->SetValue(txn, Value::ConstRef(NextIdKey()), EncodeU64Value(next_id));
}

// ---- Batch (transaction-owned staging, R2) -------------------------------

MigrationManager::Batch::Batch(MigrationManager* mgr, KvTransaction& kv)
    : mgr_(mgr), kv_(&kv), writer_guard_(mgr->write_mtx_) {}

MigrationManager::Batch::Batch(Batch&& other) noexcept
    : mgr_(other.mgr_),
      kv_(other.kv_),
      writer_guard_(std::move(other.writer_guard_)),
      done_(other.done_),
      committed_(other.committed_),
      staged_(std::move(other.staged_)),
      staged_deleted_(std::move(other.staged_deleted_)) {
    other.mgr_ = nullptr;
    other.kv_ = nullptr;
    other.done_ = true;
}

MigrationManager::Batch::~Batch() {
    if (!done_ && mgr_ && kv_) {
        try {
            Abort();
        } catch (...) {
        }
    }
}

void MigrationManager::Batch::Abort() {
    if (done_) return;
    done_ = true;
    committed_ = false;
    try {
        if (kv_) kv_->Abort();
    } catch (...) {
    }
    staged_.clear();
    staged_deleted_.clear();
    if (writer_guard_.owns_lock()) writer_guard_.unlock();
}

const MigrationManager::Record* MigrationManager::Batch::FindForRead(uint64_t uid) const {
    auto dd = staged_deleted_.find(uid);
    if (dd != staged_deleted_.end() && dd->second) return nullptr;
    auto st = staged_.find(uid);
    if (st != staged_.end()) return &st->second;
    std::lock_guard<std::mutex> lock(mgr_->mtx_);
    auto it = mgr_->migrations_.find(uid);
    if (it == mgr_->migrations_.end()) return nullptr;
    return &it->second;
}

MigrationManager::Record* MigrationManager::Batch::FindForWrite(uint64_t uid) {
    auto dd = staged_deleted_.find(uid);
    if (dd != staged_deleted_.end() && dd->second) return nullptr;
    auto st = staged_.find(uid);
    if (st != staged_.end()) return &st->second;
    std::lock_guard<std::mutex> lock(mgr_->mtx_);
    auto it = mgr_->migrations_.find(uid);
    if (it == mgr_->migrations_.end()) return nullptr;
    // Copy-on-write into staging; committed map untouched until Commit.
    staged_[uid] = it->second;
    return &staged_[uid];
}

bool MigrationManager::Batch::Get(uint64_t graph_uid, Record* out) const {
    const Record* r = FindForRead(graph_uid);
    if (!r) return false;
    if (out) *out = *r;
    return true;
}

bool MigrationManager::Batch::Begin(uint64_t graph_uid, ShardId src, ShardId dst,
                                    PlacementVersion placement_version, uint64_t* out_id) {
    if (done_ || graph_uid == 0 || src == dst || src == INVALID_SHARD_ID ||
        dst == INVALID_SHARD_ID) {
        return false;
    }
    if (!mgr_->table_) return false;
    const Record* cur = FindForRead(graph_uid);
    if (cur && !IsTerminal(cur->state)) return false;
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mgr_->mtx_);
        id = mgr_->next_id_++;
        mgr_->PersistNextIdLocked(*kv_, mgr_->next_id_);
    }
    Record r;
    r.migration_id = id;
    r.graph_uid = graph_uid;
    r.src_shard = src;
    r.dst_shard = dst;
    r.state = MigrationState::SCHEDULED;
    r.placement_version = placement_version;
    r.started_ms = mgr_->Now();
    r.updated_ms = r.started_ms;
    mgr_->PersistLocked(*kv_, r);
    staged_[graph_uid] = r;
    staged_deleted_.erase(graph_uid);
    if (out_id) *out_id = r.migration_id;
    return true;
}

bool MigrationManager::Batch::Advance(uint64_t graph_uid, MigrationState to,
                                       uint64_t expected_id) {
    if (done_ || !mgr_->table_) return false;
    Record* r = FindForWrite(graph_uid);
    if (!r) return false;
    if (expected_id != 0 && r->migration_id != expected_id) return false;
    if (!CanTransition(r->state, to)) return false;
    r->state = to;
    r->updated_ms = mgr_->Now();
    mgr_->PersistLocked(*kv_, *r);
    return true;
}

bool MigrationManager::Batch::Fail(uint64_t graph_uid, const std::string& reason,
                                    uint64_t expected_id) {
    if (done_ || !mgr_->table_) return false;
    Record* r = FindForWrite(graph_uid);
    if (!r || IsTerminal(r->state)) return false;
    if (expected_id != 0 && r->migration_id != expected_id) return false;
    // Post-cutover failures must enter ROLLBACK before becoming terminal.
    if (!CanTransition(r->state, MigrationState::FAILED)) return false;
    r->state = MigrationState::FAILED;
    r->error = reason;
    r->updated_ms = mgr_->Now();
    mgr_->PersistLocked(*kv_, *r);
    return true;
}

bool MigrationManager::Batch::Cancel(uint64_t graph_uid, uint64_t expected_id) {
    if (done_ || !mgr_->table_) return false;
    Record* r = FindForWrite(graph_uid);
    if (!r) return false;
    if (expected_id != 0 && r->migration_id != expected_id) return false;
    switch (r->state) {
    case MigrationState::SCHEDULED:
    case MigrationState::SNAPSHOTTING:
    case MigrationState::COPYING:
    case MigrationState::CATCHING_UP:
    case MigrationState::PREPARE_CUTOVER:
        r->state = MigrationState::FAILED;
        r->error = "cancelled";
        r->updated_ms = mgr_->Now();
        mgr_->PersistLocked(*kv_, *r);
        return true;
    default:
        return false;
    }
}

bool MigrationManager::Batch::Prune(uint64_t graph_uid, uint64_t expected_id) {
    if (done_ || !mgr_->table_) return false;
    const Record* r = FindForRead(graph_uid);
    if (!r || !IsTerminal(r->state)) return false;
    if (expected_id != 0 && r->migration_id != expected_id) return false;
    mgr_->DeleteRowLocked(*kv_, graph_uid);
    staged_.erase(graph_uid);
    staged_deleted_[graph_uid] = true;
    return true;
}

bool MigrationManager::Batch::ReportProgress(uint64_t graph_uid, double fraction,
                                             uint64_t bytes_delta, uint64_t expected_id) {
    if (done_ || !mgr_->table_) return false;
    Record* r = FindForWrite(graph_uid);
    if (!r) return false;
    if (expected_id != 0 && r->migration_id != expected_id) return false;
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;
    r->progress = fraction;
    r->bytes_transferred += bytes_delta;
    r->updated_ms = mgr_->Now();
    mgr_->PersistLocked(*kv_, *r);
    return true;
}

bool MigrationManager::Batch::RecordFailure(uint64_t graph_uid, bool retried,
                                             uint64_t expected_id) {
    if (done_ || !mgr_->table_) return false;
    Record* r = FindForWrite(graph_uid);
    if (!r) return false;
    if (expected_id != 0 && r->migration_id != expected_id) return false;
    r->failures += 1;
    if (retried) r->retries += 1;
    r->updated_ms = mgr_->Now();
    mgr_->PersistLocked(*kv_, *r);
    return true;
}

bool MigrationManager::Batch::Commit() {
    if (done_) return committed_;
    try {
        kv_->Commit();
    } catch (...) {
        staged_.clear();
        staged_deleted_.clear();
        done_ = true;
        committed_ = false;
        if (writer_guard_.owns_lock()) writer_guard_.unlock();
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(mgr_->mtx_);
        for (auto& kv : staged_) {
            mgr_->migrations_[kv.first] = kv.second;
            mgr_->next_id_ = std::max(mgr_->next_id_, kv.second.migration_id + 1);
        }
        for (auto& kv : staged_deleted_) {
            if (kv.second) mgr_->migrations_.erase(kv.first);
        }
    } catch (...) {
        try {
            auto rtxn = mgr_->store_->CreateReadTxn();
            std::lock_guard<std::mutex> lock(mgr_->mtx_);
            mgr_->migrations_.clear();
            mgr_->next_id_ = 1;
            auto it = mgr_->table_->GetIterator(*rtxn);
            for (it->GotoFirstKey(); it->IsValid(); it->Next()) {
                Record r;
                fma_common::BinaryBuffer buf(it->GetValue().Data(), it->GetValue().Size());
                r.Deserialize(buf);
                if (r.graph_uid == 0) continue;
                mgr_->migrations_[r.graph_uid] = r;
                mgr_->next_id_ = std::max(mgr_->next_id_, r.migration_id + 1);
            }
        } catch (...) {
        }
        staged_.clear();
        staged_deleted_.clear();
        done_ = true;
        committed_ = false;
        if (writer_guard_.owns_lock()) writer_guard_.unlock();
        return false;
    }
    staged_.clear();
    staged_deleted_.clear();
    done_ = true;
    committed_ = true;
    if (writer_guard_.owns_lock()) writer_guard_.unlock();
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
