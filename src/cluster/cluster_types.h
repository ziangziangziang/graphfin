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
#include <type_traits>
#include <vector>

#include "fma-common/binary_buffer.h"
#include "fma-common/binary_read_write_helper.h"

// Phase 4A — cluster metadata model.
//
// Memory is the scarce resource on the test fleet, so these types are
// deliberately compact and trivially copyable. See
// docs/architecture/11-cluster-metadata.md for the full rationale.

namespace lgraph {
namespace cluster {

// Dense, process-local index into the placement vector (0..N-1).
using GraphId = uint32_t;
// Shard identities are small and bounded; 16 bits allows 65535 shards.
using ShardId = uint16_t;
// Monotonic per-graph version. Bumped on every placement change; guards
// against a router acting on a stale graph->shard mapping.
using PlacementVersion = uint64_t;
// Monotonic cluster-wide version. Bumped on every placement/shard mutation.
using ConfigVersion = uint64_t;

inline constexpr ShardId INVALID_SHARD_ID = static_cast<ShardId>(0xFFFF);

enum class ShardState : uint8_t {
    OFFLINE = 0,   // registered but not serving
    ONLINE = 1,    // serving reads/writes
    DRAINING = 2,  // no new graphs; migrate existing ones away
    REMOVED = 3,   // permanently gone
};

enum class PlacementState : uint8_t {
    CREATING = 0,  // graph created, data not yet usable
    ACTIVE = 1,    // serving normally
    MOVING = 2,    // being migrated (Phase 5)
    DELETING = 3,  // delete in progress
    DELETED = 4,   // tombstone; id may be reclaimed
};

inline const char* ToString(ShardState s) {
    switch (s) {
    case ShardState::OFFLINE: return "OFFLINE";
    case ShardState::ONLINE: return "ONLINE";
    case ShardState::DRAINING: return "DRAINING";
    case ShardState::REMOVED: return "REMOVED";
    }
    return "UNKNOWN";
}

inline const char* ToString(PlacementState s) {
    switch (s) {
    case PlacementState::CREATING: return "CREATING";
    case PlacementState::ACTIVE: return "ACTIVE";
    case PlacementState::MOVING: return "MOVING";
    case PlacementState::DELETING: return "DELETING";
    case PlacementState::DELETED: return "DELETED";
    }
    return "UNKNOWN";
}

// One placement record per registered graph. Fixed 16 bytes, naturally
// aligned, trivially copyable — stored densely in a single vector indexed by
// GraphId, so 100k graphs cost 1.6 MiB with zero per-entry allocations.
struct GraphPlacement {
    PlacementVersion placement_version = 0;  // +0
    uint32_t shard_id = INVALID_SHARD_ID;    // +8
    uint16_t generation = 0;                 // +12  (number of moves)
    uint8_t state = static_cast<uint8_t>(PlacementState::CREATING);  // +14
    uint8_t reserved = 0;                    // +15

    PlacementState State() const { return static_cast<PlacementState>(state); }
    void SetState(PlacementState s) { state = static_cast<uint8_t>(s); }
};
static_assert(sizeof(GraphPlacement) == 16, "GraphPlacement must stay 16 bytes");
static_assert(std::is_trivially_copyable<GraphPlacement>::value,
              "GraphPlacement must be trivially copyable");

// Shard descriptor. There are only a handful per cluster, so variable-length
// endpoint strings are acceptable here (unlike the per-graph placement).
struct ShardInfo {
    ShardId shard_id = INVALID_SHARD_ID;
    ShardState state = ShardState::OFFLINE;
    uint32_t capacity_weight = 1;  // relative placement weight
    uint64_t config_version = 0;
    std::string name;                     // logical name, e.g. "shard-001"
    std::vector<std::string> endpoints;   // replica endpoints (host:rpc_port)

    bool IsUsable() const { return state == ShardState::ONLINE; }

    template <typename StreamT>
    size_t Serialize(StreamT& stream) const {
        size_t n = 0;
        n += fma_common::BinaryWrite(stream, shard_id);
        n += fma_common::BinaryWrite(stream, static_cast<uint8_t>(state));
        n += fma_common::BinaryWrite(stream, capacity_weight);
        n += fma_common::BinaryWrite(stream, config_version);
        n += fma_common::BinaryWrite(stream, name);
        n += fma_common::BinaryWrite(stream, static_cast<uint32_t>(endpoints.size()));
        for (const auto& e : endpoints) n += fma_common::BinaryWrite(stream, e);
        return n;
    }

    template <typename StreamT>
    size_t Deserialize(StreamT& stream) {
        uint8_t st = 0;
        uint32_t cnt = 0;
        size_t n = 0;
        n += fma_common::BinaryRead(stream, shard_id);
        n += fma_common::BinaryRead(stream, st);
        n += fma_common::BinaryRead(stream, capacity_weight);
        n += fma_common::BinaryRead(stream, config_version);
        n += fma_common::BinaryRead(stream, name);
        n += fma_common::BinaryRead(stream, cnt);
        state = static_cast<ShardState>(st);
        endpoints.clear();
        endpoints.resize(cnt);
        for (uint32_t i = 0; i < cnt; i++) n += fma_common::BinaryRead(stream, endpoints[i]);
        return n;
    }
};

}  // namespace cluster
}  // namespace lgraph
