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
    fma_common::BinaryWrite(buf, p.placement_version);
    fma_common::BinaryWrite(buf, p.shard_id);
    fma_common::BinaryWrite(buf, p.generation);
    fma_common::BinaryWrite(buf, p.state);
    out->assign(buf.GetBuf(), buf.GetSize());
}

bool DecodePlacement(const Value& v, GraphPlacement* p) {
    if (v.Size() < sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint8_t)) {
        return false;
    }
    fma_common::BinaryBuffer buf(v.Data(), v.Size());
    uint64_t ver = 0;
    uint32_t shard = 0;
    uint16_t gen = 0;
    uint8_t st = 0;
    size_t n = 0;
    n += fma_common::BinaryRead(buf, ver);
    n += fma_common::BinaryRead(buf, shard);
    n += fma_common::BinaryRead(buf, gen);
    n += fma_common::BinaryRead(buf, st);
    if (n > v.Size()) return false;
    p->placement_version = ver;
    p->shard_id = static_cast<ShardId>(shard);
    p->generation = gen;
    p->state = st;
    return true;
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
    }
}

// ---- name index ---------------------------------------------------------

void ClusterMetaStore::IndexInsert(GraphId id, const char* data, size_t len) {
    // Grow BEFORE adding this entry so IndexResize rehashes exactly the
    // already-indexed ids, and this id is inserted exactly once below.
    size_t current = name_offset_.size();
    if (bucket_.empty()) {
        IndexResize(8);
    } else if ((current + 1) * 4 >= bucket_.size() * 3) {
        IndexResize(bucket_.size() * 2);
    }

    // Append the name into the arena (length-prefixed) and remember its offset.
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

// ---- shards -------------------------------------------------------------

bool ClusterMetaStore::RegisterShard(KvTransaction& txn, const ShardInfo& info) {
    std::unique_lock<std::shared_mutex> lock(mtx_);
    if (!shard_table_) return false;
    fma_common::BinaryBuffer buf;
    info.Serialize(buf);
    std::string key = EncodeShardKey(info.shard_id);
    shard_table_->SetValue(txn, Value::ConstRef(key),
                           Value(buf.GetBuf(), buf.GetSize()));
    shards_[info.shard_id] = info;
    return BumpVersion(txn);
}

bool ClusterMetaStore::RemoveShard(KvTransaction& txn, ShardId id) {
    std::unique_lock<std::shared_mutex> lock(mtx_);
    if (!shard_table_) return false;
    // Refuse if any live graph still lives on the shard.
    for (const auto& p : placements_) {
        if (p.shard_id == id && p.State() != PlacementState::DELETED) return false;
    }
    std::string key = EncodeShardKey(id);
    bool removed = shard_table_->DeleteKey(txn, Value::ConstRef(key));
    shards_.erase(id);
    if (removed) BumpVersion(txn);
    return removed;
}

bool ClusterMetaStore::SetShardState(KvTransaction& txn, ShardId id, ShardState state) {
    std::unique_lock<std::shared_mutex> lock(mtx_);
    auto it = shards_.find(id);
    if (it == shards_.end()) return false;
    it->second.state = state;
    fma_common::BinaryBuffer buf;
    it->second.Serialize(buf);
    std::string key = EncodeShardKey(id);
    shard_table_->SetValue(txn, Value::ConstRef(key), Value(buf.GetBuf(), buf.GetSize()));
    return BumpVersion(txn);
}

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

// ---- graph placement ----------------------------------------------------

bool ClusterMetaStore::PutGraphPlacement(KvTransaction& txn, const std::string& name,
                                         ShardId shard, PlacementState state,
                                         PlacementVersion* out_version) {
    std::unique_lock<std::shared_mutex> lock(mtx_);
    if (!graph_table_) return false;

    ConfigVersion next_version = version_ + 1;
    GraphId id = 0;
    if (IndexFind(name, &id)) {
        GraphPlacement& p = placements_[id];
        if (p.State() == PlacementState::DELETED) {
            live_count_++;  // revive a tombstoned placement
        }
        p.shard_id = shard;
        p.SetState(state);
        p.placement_version = next_version;
    } else {
        GraphPlacement p;
        p.shard_id = shard;
        p.SetState(state);
        p.placement_version = next_version;
        id = static_cast<GraphId>(placements_.size());
        placements_.push_back(p);
        IndexInsert(id, name.data(), name.size());
        live_count_++;
    }
    version_ = next_version;

    std::string encoded;
    EncodePlacement(placements_[id], &encoded);
    graph_table_->SetValue(txn, Value::ConstRef(name), Value(encoded.data(), encoded.size()));
    meta_table_->SetValue(txn, Value::ConstRef(std::string(kVersionKey)), EncodeVersion(version_));
    if (out_version) *out_version = placements_[id].placement_version;
    return true;
}

bool ClusterMetaStore::DeleteGraphPlacement(KvTransaction& txn, const std::string& name) {
    std::unique_lock<std::shared_mutex> lock(mtx_);
    GraphId id = 0;
    if (!IndexFind(name, &id)) return false;
    // Tombstone in memory (ids stay stable); drop the durable row.
    if (placements_[id].State() != PlacementState::DELETED) live_count_--;
    placements_[id].SetState(PlacementState::DELETED);
    placements_[id].placement_version = ++version_;
    bool removed = graph_table_->DeleteKey(txn, Value::ConstRef(name));
    meta_table_->SetValue(txn, Value::ConstRef(std::string(kVersionKey)), EncodeVersion(version_));
    return removed;
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
    size_t n = 0;
    for (const auto& p : placements_) {
        if (p.shard_id == shard && p.State() != PlacementState::DELETED) n++;
    }
    return n;
}

// ---- version ------------------------------------------------------------

ConfigVersion ClusterMetaStore::Version() const {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    return version_;
}

bool ClusterMetaStore::SetVersion(KvTransaction& txn, ConfigVersion v) {
    std::unique_lock<std::shared_mutex> lock(mtx_);
    if (!meta_table_) return false;
    version_ = v;
    meta_table_->SetValue(txn, Value::ConstRef(std::string(kVersionKey)), EncodeVersion(version_));
    return true;
}

bool ClusterMetaStore::BumpVersion(KvTransaction& txn) {
    version_ += 1;
    if (meta_table_) {
        meta_table_->SetValue(txn, Value::ConstRef(std::string(kVersionKey)),
                              EncodeVersion(version_));
    }
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
    return bytes;
}

}  // namespace cluster
}  // namespace lgraph
