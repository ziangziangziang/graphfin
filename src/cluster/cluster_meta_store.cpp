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
#include <cstring>
#include <utility>

#include "fma-common/binary_buffer.h"
#include "fma-common/binary_read_write_helper.h"

#include "cluster/cluster_meta_store.h"

namespace lgraph {
namespace cluster {

namespace {

const char kVersionKey[] = "v";
const char kNextUidKey[] = "u";  // monotonic graph unique_id allocator

uint64_t HashName(const char* data, size_t len) {
    // FNV-1a, 64-bit.
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= static_cast<unsigned char>(data[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

std::string EncodeShardKey(ShardId id) {
    std::string k(2, '\0');
    k[0] = static_cast<char>((id >> 8) & 0xFF);
    k[1] = static_cast<char>(id & 0xFF);
    return k;
}

void EncodePlacement(const GraphPlacement& p, std::string* out) {
    fma_common::BinaryBuffer buf;
    fma_common::BinaryWrite(buf, p.unique_id);
    fma_common::BinaryWrite(buf, p.placement_version);
    fma_common::BinaryWrite(buf, p.shard_id);
    fma_common::BinaryWrite(buf, p.generation);
    fma_common::BinaryWrite(buf, p.state);
    out->assign(buf.GetBuf(), buf.GetSize());
}

bool DecodePlacement(const Value& v, GraphPlacement* p) {
    const size_t kMin = sizeof(uint64_t) * 2 + sizeof(uint32_t) + sizeof(uint16_t) +
                        sizeof(uint8_t);
    if (v.Size() < kMin) return false;
    fma_common::BinaryBuffer buf(v.Data(), v.Size());
    uint64_t uid = 0;
    uint64_t ver = 0;
    uint32_t shard = 0;
    uint16_t gen = 0;
    uint8_t st = 0;
    size_t n = 0;
    n += fma_common::BinaryRead(buf, uid);
    n += fma_common::BinaryRead(buf, ver);
    n += fma_common::BinaryRead(buf, shard);
    n += fma_common::BinaryRead(buf, gen);
    n += fma_common::BinaryRead(buf, st);
    if (n > v.Size()) return false;
    p->unique_id = uid;
    p->placement_version = ver;
    p->shard_id = static_cast<ShardId>(shard);
    p->generation = gen;
    p->state = st;
    return true;
}

Value EncodeU64(uint64_t v) { return Value(&v, sizeof(v)); }

uint64_t DecodeU64(const Value& val) {
    uint64_t v = 0;
    if (val.Size() >= sizeof(v)) std::memcpy(&v, val.Data(), sizeof(v));
    return v;
}

Value EncodeVersion(ConfigVersion v) { return Value(&v, sizeof(v)); }

ConfigVersion DecodeVersion(const Value& val) {
    ConfigVersion v = 0;
    if (val.Size() >= sizeof(v)) std::memcpy(&v, val.Data(), sizeof(v));
    return v;
}

}  // namespace

void ClusterMetaStore::Init(KvStore* store, KvTransaction& txn, bool create_if_not_exist) {
    Init(store, txn, Config(), create_if_not_exist);
}

void ClusterMetaStore::Init(KvStore* store, KvTransaction& txn, const Config& config,
                            bool create_if_not_exist) {
    std::unique_lock<std::shared_mutex> lock(mtx_);
    store_ = store;
    config_ = config;
    shard_table_ = store_->OpenTable(txn, config_.shard_table, create_if_not_exist,
                                     ComparatorDesc::DefaultComparator());
    graph_table_ = store_->OpenTable(txn, config_.graph_table, create_if_not_exist,
                                     ComparatorDesc::DefaultComparator());
    meta_table_ = store_->OpenTable(txn, config_.meta_table, create_if_not_exist,
                                    ComparatorDesc::DefaultComparator());
    Reload(txn);
}

void ClusterMetaStore::Reload(KvTransaction& txn) {
    placements_.clear();
    live_count_ = 0;
    shard_counts_.clear();
    name_offset_.clear();
    name_arena_.clear();
    bucket_.clear();
    bucket_mask_ = 0;
    shards_.clear();
    version_ = 0;

    // Graphs: assign dense ids in iteration order and index the names.
    if (graph_table_) {
        auto it = graph_table_->GetIterator(txn);
        for (it->GotoFirstKey(); it->IsValid(); it->Next()) {
            GraphPlacement p;
            if (!DecodePlacement(it->GetValue(), &p)) continue;
            const std::string name = it->GetKey().AsString();
            GraphId id = static_cast<GraphId>(placements_.size());
            placements_.push_back(p);
            IndexInsert(id, name.data(), name.size());
        }
        live_count_ = placements_.size();
        for (const auto& p : placements_) {
            if (p.State() != PlacementState::DELETED) shard_counts_[p.shard_id]++;
        }
    }

    // Shards.
    if (shard_table_) {
        auto it = shard_table_->GetIterator(txn);
        for (it->GotoFirstKey(); it->IsValid(); it->Next()) {
            const Value& v = it->GetValue();
            fma_common::BinaryBuffer buf(v.Data(), v.Size());
            ShardInfo info;
            info.Deserialize(buf);
            shards_[info.shard_id] = std::move(info);
        }
    }

    if (meta_table_) {
        Value v;
        if (meta_table_->GetValue(txn, Value::ConstRef(std::string(kVersionKey)), v)) {
            version_ = DecodeVersion(v);
        }
        Value u;
        if (meta_table_->GetValue(txn, Value::ConstRef(std::string(kNextUidKey)), u)) {
            next_uid_ = DecodeU64(u);
        }
    }
    // Never reuse an id: the allocator must exceed every persisted unique_id.
    for (const auto& pl : placements_) next_uid_ = std::max(next_uid_, pl.unique_id);
}

// ---- name index ---------------------------------------------------------

void ClusterMetaStore::IndexInsert(GraphId id, const char* data, size_t len) {
    size_t current = name_offset_.size();
    if (bucket_.empty()) {
        IndexResize(8);
    } else if ((current + 1) * 4 >= bucket_.size() * 3) {
        IndexResize(bucket_.size() * 2);
    }

    uint32_t len32 = static_cast<uint32_t>(len);
    uint32_t offset = static_cast<uint32_t>(name_arena_.size());
    name_arena_.resize(name_arena_.size() + sizeof(uint32_t) + len);
    std::memcpy(name_arena_.data() + offset, &len32, sizeof(len32));
    if (len) std::memcpy(name_arena_.data() + offset + sizeof(uint32_t), data, len);
    name_offset_.push_back(offset);

    uint64_t h = HashName(data, len);
    size_t mask = bucket_mask_;
    size_t i = h & mask;
    while (bucket_[i] != 0) i = (i + 1) & mask;
    bucket_[i] = id + 1;
}

void ClusterMetaStore::IndexResize(size_t new_bucket_count) {
    std::vector<uint32_t> nb(new_bucket_count, 0);
    size_t mask = new_bucket_count - 1;
    for (GraphId id = 0; id < static_cast<GraphId>(name_offset_.size()); id++) {
        uint32_t len = 0;
        const char* name = NameOf(id, &len);
        uint64_t h = HashName(name, len);
        size_t i = h & mask;
        while (nb[i] != 0) i = (i + 1) & mask;
        nb[i] = id + 1;
    }
    bucket_.swap(nb);
    bucket_mask_ = mask;
}

bool ClusterMetaStore::IndexFind(const std::string& name, GraphId* id) const {
    if (bucket_.empty()) return false;
    uint64_t h = HashName(name.data(), name.size());
    size_t mask = bucket_mask_;
    size_t i = h & mask;
    while (bucket_[i] != 0) {
        GraphId cand = bucket_[i] - 1;
        uint32_t len = 0;
        const char* cand_name = NameOf(cand, &len);
        if (len == name.size() && std::memcmp(cand_name, name.data(), len) == 0) {
            *id = cand;
            return true;
        }
        i = (i + 1) & mask;
    }
    return false;
}

const char* ClusterMetaStore::NameOf(GraphId id, uint32_t* len) const {
    uint32_t offset = name_offset_[id];
    uint32_t len32 = 0;
    std::memcpy(&len32, name_arena_.data() + offset, sizeof(len32));
    *len = len32;
    return name_arena_.data() + offset + sizeof(uint32_t);
}

// ---- committed reads ----------------------------------------------------

bool ClusterMetaStore::GetShard(ShardId id, ShardInfo* out) const {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    auto it = shards_.find(id);
    if (it == shards_.end()) return false;
    if (out) *out = it->second;
    return true;
}

std::vector<ShardInfo> ClusterMetaStore::ListShards() const {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    std::vector<ShardInfo> ret;
    ret.reserve(shards_.size());
    for (const auto& kv : shards_) ret.push_back(kv.second);
    std::sort(ret.begin(), ret.end(),
              [](const ShardInfo& a, const ShardInfo& b) { return a.shard_id < b.shard_id; });
    return ret;
}

size_t ClusterMetaStore::ShardCount() const {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    return shards_.size();
}

bool ClusterMetaStore::HasGraph(const std::string& name) const {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    GraphId id = 0;
    if (!IndexFind(name, &id)) return false;
    return placements_[id].State() != PlacementState::DELETED;
}

bool ClusterMetaStore::GetGraphPlacement(const std::string& name, GraphPlacement* out,
                                         GraphId* out_id) const {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    GraphId id = 0;
    if (!IndexFind(name, &id)) return false;
    if (placements_[id].State() == PlacementState::DELETED) return false;
    if (out) *out = placements_[id];
    if (out_id) *out_id = id;
    return true;
}

bool ClusterMetaStore::GetGraphPlacement(GraphId id, GraphPlacement* out) const {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    if (id >= placements_.size()) return false;
    if (out) *out = placements_[id];
    return true;
}

bool ClusterMetaStore::GetGraphName(GraphId id, std::string* out) const {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    if (id >= name_offset_.size()) return false;
    uint32_t len = 0;
    const char* name = NameOf(id, &len);
    if (out) out->assign(name, len);
    return true;
}

size_t ClusterMetaStore::GraphCount() const {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    return live_count_;
}

size_t ClusterMetaStore::GraphCountOnShard(ShardId shard) const {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    auto it = shard_counts_.find(shard);
    return it == shard_counts_.end() ? 0 : it->second;
}

void ClusterMetaStore::AdjustShardCount(ShardId shard, int delta) {
    auto it = shard_counts_.find(shard);
    if (delta > 0) {
        if (it == shard_counts_.end()) {
            shard_counts_[shard] = static_cast<size_t>(delta);
        } else {
            it->second += static_cast<size_t>(delta);
        }
    } else if (it != shard_counts_.end()) {
        size_t d = static_cast<size_t>(-delta);
        if (it->second <= d) {
            shard_counts_.erase(it);
        } else {
            it->second -= d;
        }
    }
}

ConfigVersion ClusterMetaStore::Version() const {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    return version_;
}

void ClusterMetaStore::WriteVersion(KvTransaction& txn) {
    // Legacy helper: writes committed version_. Batch writes its staged
    // version directly; see Batch mutators below.
    if (meta_table_) {
        meta_table_->SetValue(txn, Value::ConstRef(std::string(kVersionKey)),
                              EncodeVersion(version_));
    }
}

void ClusterMetaStore::WriteNextUid(KvTransaction& txn) {
    if (meta_table_) {
        meta_table_->SetValue(txn, Value::ConstRef(std::string(kNextUidKey)),
                              EncodeU64(next_uid_));
    }
}

void ClusterMetaStore::ApplyPutGraph(const std::string& name, const GraphPlacement& placement) {
    GraphId id = 0;
    const bool existing = IndexFind(name, &id);
    const bool old_live = existing && placements_[id].State() != PlacementState::DELETED;
    const ShardId old_shard = existing ? placements_[id].shard_id : INVALID_SHARD_ID;
    const bool new_live = placement.State() != PlacementState::DELETED;

    if (!old_live && new_live) live_count_++;
    if (old_live && !new_live) live_count_--;
    if (old_live && (!new_live || old_shard != placement.shard_id)) {
        AdjustShardCount(old_shard, -1);
    }
    if (new_live && (!old_live || old_shard != placement.shard_id)) {
        AdjustShardCount(placement.shard_id, +1);
    }
    if (existing) {
        placements_[id] = placement;
    } else {
        id = static_cast<GraphId>(placements_.size());
        placements_.push_back(placement);
        IndexInsert(id, name.data(), name.size());
    }
}

void ClusterMetaStore::ApplyDeleteGraph(const std::string& name, ConfigVersion version) {
    GraphId id = 0;
    if (!IndexFind(name, &id)) return;
    if (placements_[id].State() != PlacementState::DELETED) {
        live_count_--;
        AdjustShardCount(placements_[id].shard_id, -1);
    }
    placements_[id].SetState(PlacementState::DELETED);
    placements_[id].placement_version = version;
}

// ---- Batch (transaction-owned staging, R1/R10) --------------------------

ClusterMetaStore::Batch::Batch(ClusterMetaStore* store, KvTransaction& kv)
    : store_(store), kv_(&kv), writer_guard_(store->write_mtx_) {
    std::shared_lock<std::shared_mutex> lock(store_->mtx_);
    staged_version_ = store_->version_;
}

ClusterMetaStore::Batch::Batch(Batch&& other) noexcept
    : store_(other.store_),
      kv_(other.kv_),
      writer_guard_(std::move(other.writer_guard_)),
      pending_(std::move(other.pending_)),
      staged_version_(other.staged_version_),
      done_(other.done_),
      committed_(other.committed_),
      batch_uids_(std::move(other.batch_uids_)),
      batch_puts_(std::move(other.batch_puts_)),
      batch_deletes_(std::move(other.batch_deletes_)),
      batch_shards_(std::move(other.batch_shards_)),
      batch_shard_removed_(std::move(other.batch_shard_removed_)) {
    other.store_ = nullptr;
    other.kv_ = nullptr;
    other.done_ = true;
}

ClusterMetaStore::Batch::~Batch() {
    if (!done_ && store_ && kv_) {
        try {
            Abort();
        } catch (...) {
        }
    }
}

void ClusterMetaStore::Batch::Abort() {
    if (done_) return;
    done_ = true;
    committed_ = false;
    try {
        if (kv_) kv_->Abort();
    } catch (...) {
    }
    pending_.clear();
    batch_uids_.clear();
    batch_puts_.clear();
    batch_deletes_.clear();
    batch_shards_.clear();
    batch_shard_removed_.clear();
    if (writer_guard_.owns_lock()) writer_guard_.unlock();
}

size_t ClusterMetaStore::Batch::StagedCount() const { return pending_.size(); }

bool ClusterMetaStore::Batch::GetShard(ShardId id, ShardInfo* out) const {
    auto rm = batch_shard_removed_.find(id);
    if (rm != batch_shard_removed_.end() && rm->second) return false;
    auto it = batch_shards_.find(id);
    if (it != batch_shards_.end()) {
        if (out) *out = it->second;
        return true;
    }
    return store_->GetShard(id, out);
}

bool ClusterMetaStore::Batch::HasGraph(const std::string& name) const {
    if (batch_deletes_.count(name)) return false;
    auto it = batch_puts_.find(name);
    if (it != batch_puts_.end()) return it->second.State() != PlacementState::DELETED;
    return store_->HasGraph(name);
}

bool ClusterMetaStore::Batch::GetGraphPlacement(const std::string& name, GraphPlacement* out,
                                                GraphId* out_id) const {
    if (batch_deletes_.count(name)) return false;
    auto it = batch_puts_.find(name);
    if (it != batch_puts_.end()) {
        if (out) *out = it->second;
        if (out_id) {
            GraphId id = 0;
            std::shared_lock<std::shared_mutex> lock(store_->mtx_);
            if (store_->IndexFind(name, &id)) {
                *out_id = id;
            } else {
                // New name staged in this batch: no dense id until publication.
                // Callers needing the id must use committed reads after Commit.
                return false;
            }
        }
        return true;
    }
    return store_->GetGraphPlacement(name, out, out_id);
}

size_t ClusterMetaStore::Batch::GraphCountOnShard(ShardId shard) const {
    size_t base = store_->GraphCountOnShard(shard);
    // Apply transaction-local deltas (R8: batches see their own writes).
    // For each staged put, compare against committed placement.
    for (const auto& kv : batch_puts_) {
        GraphPlacement committed;
        GraphId id = 0;
        bool had = store_->GetGraphPlacement(kv.first, &committed, &id);
        bool old_live = had;
        ShardId old_shard = had ? committed.shard_id : INVALID_SHARD_ID;
        bool new_live = kv.second.State() != PlacementState::DELETED;
        ShardId new_shard = kv.second.shard_id;
        if (old_live && old_shard == shard && (!new_live || new_shard != shard)) {
            if (base > 0) base--;
        }
        if (new_live && new_shard == shard && (!old_live || old_shard != shard)) {
            base++;
        }
    }
    for (const auto& name : batch_deletes_) {
        GraphPlacement committed;
        if (store_->GetGraphPlacement(name, &committed)) {
            if (committed.shard_id == shard) {
                // Skip if the same name was also re-put in this batch (Put clears
                // the delete marker, so a surviving delete marker means no later
                // put for that name).
                if (batch_puts_.count(name) == 0 && base > 0) base--;
            }
        }
    }
    return base;
}

bool ClusterMetaStore::Batch::FindPlacementForWrite(const std::string& name,
                                                    GraphPlacement* out,
                                                    bool* is_new) const {
    if (batch_deletes_.count(name)) {
        // R3: deleted earlier in this batch. A later Put is a NEW incarnation
        // and must allocate a fresh UID — never resurrect the committed or
        // staged UID. Report "not found" so the caller allocates fresh.
        if (is_new) *is_new = true;
        return false;
    }
    auto it = batch_puts_.find(name);
    if (it != batch_puts_.end()) {
        if (out) *out = it->second;
        if (is_new) *is_new = false;
        return true;
    }
    GraphPlacement committed;
    GraphId id = 0;
    if (store_->GetGraphPlacement(name, &committed, &id)) {
        if (out) *out = committed;
        if (is_new) *is_new = false;
        return true;
    }
    // Check batch UID map for a name put then deleted then re-put tracking.
    auto bu = batch_uids_.find(name);
    if (bu != batch_uids_.end()) {
        GraphPlacement p;
        p.unique_id = bu->second;
        if (out) *out = p;
        if (is_new) *is_new = true;
        return true;
    }
    if (is_new) *is_new = true;
    return false;
}

bool ClusterMetaStore::Batch::RegisterShard(const ShardInfo& info) {
    if (done_ || !store_->shard_table_) return false;
    staged_version_ += 1;
    ShardInfo stored = info;
    stored.config_version = staged_version_;
    fma_common::BinaryBuffer buf;
    stored.Serialize(buf);
    std::string key = EncodeShardKey(stored.shard_id);
    store_->shard_table_->SetValue(*kv_, Value::ConstRef(key),
                                   Value(buf.GetBuf(), buf.GetSize()));
    if (store_->meta_table_) {
        store_->meta_table_->SetValue(*kv_, Value::ConstRef(std::string(kVersionKey)),
                                      EncodeVersion(staged_version_));
    }
    Pending op;
    op.type = PendingType::REGISTER_SHARD;
    op.shard = stored;
    op.version = staged_version_;
    pending_.push_back(std::move(op));
    batch_shards_[stored.shard_id] = stored;
    batch_shard_removed_.erase(stored.shard_id);
    return true;
}

bool ClusterMetaStore::Batch::RemoveShard(ShardId id) {
    if (done_ || !store_->shard_table_) return false;
    // Refuse while any live graph (committed adjusted by this batch) lives
    // there, or a graph is staged onto it in this same batch (R8-aware).
    for (const auto& kv : batch_puts_) {
        if (kv.second.shard_id == id && kv.second.State() != PlacementState::DELETED) return false;
    }
    {
        // If any committed graph on id is NOT deleted in this batch, refuse.
        // Iterate committed index (shared lock) and consult batch_deletes_.
        std::shared_lock<std::shared_mutex> lock(store_->mtx_);
        for (GraphId gid = 0; gid < static_cast<GraphId>(store_->placements_.size()); gid++) {
            const auto& p = store_->placements_[gid];
            if (p.shard_id != id || p.State() == PlacementState::DELETED) continue;
            uint32_t len = 0;
            const char* data = store_->NameOf(gid, &len);
            std::string name(data, len);
            if (batch_deletes_.count(name)) continue;
            auto itp = batch_puts_.find(name);
            if (itp != batch_puts_.end()) {
                // Moved away in this batch?
                if (itp->second.shard_id != id ||
                    itp->second.State() == PlacementState::DELETED)
                    continue;
            }
            return false;
        }
    }
    std::string key = EncodeShardKey(id);
    bool removed = store_->shard_table_->DeleteKey(*kv_, Value::ConstRef(key));
    if (!removed) return false;
    staged_version_ += 1;
    if (store_->meta_table_) {
        store_->meta_table_->SetValue(*kv_, Value::ConstRef(std::string(kVersionKey)),
                                      EncodeVersion(staged_version_));
    }
    Pending op;
    op.type = PendingType::REMOVE_SHARD;
    op.shard_id = id;
    op.version = staged_version_;
    pending_.push_back(std::move(op));
    batch_shards_.erase(id);
    batch_shard_removed_[id] = true;
    return true;
}

bool ClusterMetaStore::Batch::SetShardState(ShardId id, ShardState state) {
    if (done_) return false;
    ShardInfo base;
    if (!GetShard(id, &base)) return false;
    staged_version_ += 1;
    ShardInfo updated = base;
    updated.state = state;
    updated.config_version = staged_version_;
    fma_common::BinaryBuffer buf;
    updated.Serialize(buf);
    std::string key = EncodeShardKey(id);
    store_->shard_table_->SetValue(*kv_, Value::ConstRef(key),
                                   Value(buf.GetBuf(), buf.GetSize()));
    if (store_->meta_table_) {
        store_->meta_table_->SetValue(*kv_, Value::ConstRef(std::string(kVersionKey)),
                                      EncodeVersion(staged_version_));
    }
    Pending op;
    op.type = PendingType::SHARD_STATE;
    op.shard = updated;  // R10: publish the full descriptor incl. config_version
    op.version = staged_version_;
    pending_.push_back(std::move(op));
    batch_shards_[id] = updated;
    return true;
}

bool ClusterMetaStore::Batch::PutGraphPlacement(const std::string& name, ShardId shard,
                                                PlacementState state,
                                                PlacementVersion* out_version,
                                                uint64_t* out_uid) {
    if (done_ || !store_->graph_table_) return false;
    staged_version_ += 1;
    GraphPlacement p;
    p.shard_id = shard;
    p.SetState(state);
    p.placement_version = staged_version_;
    GraphPlacement existing;
    bool is_new = true;
    bool had = FindPlacementForWrite(name, &existing, &is_new);
    if (had) {
        p.unique_id = existing.unique_id;
        if (p.unique_id == 0) {
            // Staged placeholder without UID (should not happen); allocate.
            std::unique_lock<std::shared_mutex> lock(store_->mtx_);
            p.unique_id = ++store_->next_uid_;
            uint64_t uid = store_->next_uid_;
            lock.unlock();
            if (store_->meta_table_) {
                store_->meta_table_->SetValue(*kv_, Value::ConstRef(std::string(kNextUidKey)),
                                              EncodeU64(uid));
            }
        }
    } else {
        auto bu = batch_uids_.find(name);
        if (bu != batch_uids_.end()) {
            p.unique_id = bu->second;
        } else {
            std::unique_lock<std::shared_mutex> lock(store_->mtx_);
            p.unique_id = ++store_->next_uid_;
            uint64_t uid = store_->next_uid_;
            lock.unlock();
            if (store_->meta_table_) {
                store_->meta_table_->SetValue(*kv_, Value::ConstRef(std::string(kNextUidKey)),
                                              EncodeU64(uid));
            }
        }
    }

    std::string encoded;
    EncodePlacement(p, &encoded);
    store_->graph_table_->SetValue(*kv_, Value::ConstRef(name),
                                   Value(encoded.data(), encoded.size()));
    if (store_->meta_table_) {
        store_->meta_table_->SetValue(*kv_, Value::ConstRef(std::string(kVersionKey)),
                                      EncodeVersion(staged_version_));
    }
    Pending op;
    op.type = PendingType::PUT_GRAPH;
    op.name = name;
    op.placement = p;
    op.version = staged_version_;
    pending_.push_back(std::move(op));
    batch_puts_[name] = p;
    batch_uids_[name] = p.unique_id;
    batch_deletes_.erase(name);
    if (out_version) *out_version = p.placement_version;
    if (out_uid) *out_uid = p.unique_id;
    return true;
}

bool ClusterMetaStore::Batch::DeleteGraphPlacement(const std::string& name) {
    if (done_) return false;
    // Batch-aware existence (R8): deletable if committed live or staged put.
    bool exists = false;
    if (batch_puts_.count(name)) {
        exists = true;
    } else if (!batch_deletes_.count(name)) {
        exists = store_->HasGraph(name);
    }
    if (!exists) return false;
    bool removed = store_->graph_table_->DeleteKey(*kv_, Value::ConstRef(name));
    if (!removed) {
        // Deleting a name that exists only as staged-put in this batch: the
        // durable row was written by this same txn, so DeleteKey should have
        // found it. If the backend reports missing, still stage the delete so
        // publication converges (tolerates backends without read-your-writes).
        // Fall through and stage.
    }
    staged_version_ += 1;
    if (store_->meta_table_) {
        store_->meta_table_->SetValue(*kv_, Value::ConstRef(std::string(kVersionKey)),
                                      EncodeVersion(staged_version_));
    }
    Pending op;
    op.type = PendingType::DELETE_GRAPH;
    op.name = name;
    op.version = staged_version_;
    pending_.push_back(std::move(op));
    batch_puts_.erase(name);
    batch_deletes_.insert(name);
    // R3: drop the staged UID so a later recreate in this batch allocates a
    // fresh incarnation UID instead of reusing the deleted one.
    batch_uids_.erase(name);
    return true;
}

bool ClusterMetaStore::Batch::SetVersion(ConfigVersion v) {
    if (done_ || !store_->meta_table_) return false;
    staged_version_ = v;
    store_->meta_table_->SetValue(*kv_, Value::ConstRef(std::string(kVersionKey)),
                                  EncodeVersion(staged_version_));
    Pending op;
    op.type = PendingType::SET_VERSION;
    op.version = v;
    pending_.push_back(std::move(op));
    return true;
}

bool ClusterMetaStore::Batch::Commit() {
    if (done_) return committed_;
    // 1) Durable commit. On failure nothing is published (R1).
    try {
        kv_->Commit();
    } catch (...) {
        pending_.clear();
        done_ = true;
        committed_ = false;
        if (writer_guard_.owns_lock()) writer_guard_.unlock();
        return false;
    }
    // 2) In-memory publication, allocation-failure-safe: pre-reserve, then
    // apply; on allocation failure reload committed state (fail closed).
    try {
        std::unique_lock<std::shared_mutex> lock(store_->mtx_);
        size_t new_names = 0;
        size_t arena_growth = 0;
        for (auto& op : pending_) {
            if (op.type == PendingType::PUT_GRAPH) {
                GraphId id = 0;
                if (!store_->IndexFind(op.name, &id)) {
                    new_names++;
                    arena_growth += sizeof(uint32_t) + op.name.size();
                }
            }
        }
        if (new_names > 0) {
            store_->placements_.reserve(store_->placements_.size() + new_names);
            store_->name_offset_.reserve(store_->name_offset_.size() + new_names);
            store_->name_arena_.reserve(store_->name_arena_.size() + arena_growth);
            size_t need = store_->name_offset_.size() + new_names;
            size_t buckets = store_->bucket_.empty() ? 8 : store_->bucket_.size();
            while (need * 4 >= buckets * 3) buckets *= 2;
            if (buckets != store_->bucket_.size()) {
                std::vector<uint32_t> nb(buckets, 0);
                // Rehash happens in IndexResize; reserve bucket capacity here.
                store_->bucket_.reserve(buckets);
            }
        }
        for (auto& op : pending_) {
            switch (op.type) {
            case PendingType::PUT_GRAPH:
                store_->ApplyPutGraph(op.name, op.placement);
                break;
            case PendingType::DELETE_GRAPH:
                store_->ApplyDeleteGraph(op.name, op.version);
                break;
            case PendingType::REGISTER_SHARD:
                store_->shards_[op.shard.shard_id] = op.shard;
                break;
            case PendingType::REMOVE_SHARD:
                store_->shards_.erase(op.shard_id);
                break;
            case PendingType::SHARD_STATE:
                // R10: publish the complete descriptor, not just .state.
                store_->shards_[op.shard.shard_id] = op.shard;
                break;
            case PendingType::SET_VERSION:
                break;
            }
        }
        store_->version_ = staged_version_;
    } catch (...) {
        // Fail closed: reload committed (durable) state so memory cannot be
        // partially published.
        try {
            auto rtxn = store_->store_->CreateReadTxn();
            std::unique_lock<std::shared_mutex> lock(store_->mtx_);
            store_->Reload(*rtxn);
        } catch (...) {
        }
        pending_.clear();
        done_ = true;
        committed_ = false;
        if (writer_guard_.owns_lock()) writer_guard_.unlock();
        return false;
    }
    pending_.clear();
    done_ = true;
    committed_ = true;
    if (writer_guard_.owns_lock()) writer_guard_.unlock();
    return true;
}

size_t ClusterMetaStore::MemoryFootprint() const {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    size_t bytes = 0;
    bytes += placements_.capacity() * sizeof(GraphPlacement);
    bytes += name_offset_.capacity() * sizeof(uint32_t);
    bytes += name_arena_.capacity() * sizeof(char);
    bytes += bucket_.capacity() * sizeof(uint32_t);
    for (const auto& kv : shards_) {
        bytes += sizeof(ShardInfo) + kv.second.name.capacity();
        for (const auto& e : kv.second.endpoints) bytes += sizeof(std::string) + e.capacity();
    }
    // NOTE (R9): this is the committed index only; per-Batch staging,
    // hash overhead beyond capacity, and tombstones are accounted separately
    // in the memory-honesty follow-up.
    return bytes;
}

}  // namespace cluster
}  // namespace lgraph
