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

#include "cypher/resultset/record.h"

#include "execution_plan/runtime_context.h"
#include <boost/algorithm/string.hpp>
#include "parser/symbol_table.h"

namespace cypher {

namespace {

/**
 * What `RETURN n.<series field>` reports: the point count and the two ends, read
 * from the bucket headers, plus the measure names - which the encoded buckets do
 * not carry, since they store measure indices. The points themselves are never
 * materialised; that is what keeps this cheap enough to be a property read.
 */
cypher::FieldData SeriesSummaryToMap(const lgraph::series::SeriesSummary &s) {
    cypher::FieldData::CYPHER_FIELD_DATA_MAP m;
    m.emplace("count", cypher::FieldData(lgraph::FieldData(static_cast<int64_t>(s.count))));
    m.emplace("first", s.has_points
                           ? cypher::FieldData(lgraph::FieldData(lgraph::DateTime(s.first_ts)))
                           : cypher::FieldData(lgraph::FieldData()));
    m.emplace("last", s.has_points
                          ? cypher::FieldData(lgraph::FieldData(lgraph::DateTime(s.last_ts)))
                          : cypher::FieldData(lgraph::FieldData()));
    cypher::FieldData::CYPHER_FIELD_DATA_LIST names;
    names.reserve(s.measures.size());
    for (const auto &mr : s.measures) {
        names.emplace_back(cypher::FieldData(lgraph::FieldData(mr.name)));
    }
    m.emplace("measures", cypher::FieldData(std::move(names)));
    return cypher::FieldData(std::move(m));
}

}  // namespace

cypher::FieldData Entry::GetEntityField(RTContext *ctx, const std::string &fd) const {
    switch (type) {
    case NODE:
        {
            auto vit = node->ItRef();
            CYPHER_THROW_ASSERT(node && vit);
            if (!node->IsValidAfterMaterialize(ctx)) return cypher::FieldData(lgraph::FieldData());
            // Asked before the stored value, because a series field has no bytes
            // in the record to return and its summary is the useful answer.
            lgraph::series::SeriesSummary summary;
            if (ctx->txn_->GetTxn()->ProbeVertexSeries(node->PullVid(), fd, &summary)) {
                return SeriesSummaryToMap(summary);
            }
            return cypher::FieldData(vit->GetField(fd));
        }
    case RELATIONSHIP:
        {
            auto eit = relationship->ItRef();
            CYPHER_THROW_ASSERT(relationship && eit);
            if (!eit->IsValid()) return cypher::FieldData(lgraph::FieldData());
            // Asked before the stored value, like the vertex branch: a series
            // field has no bytes in the record and its summary is the answer.
            lgraph::series::SeriesSummary summary;
            if (ctx->txn_->GetTxn()->ProbeEdgeSeries(eit->GetUid(), fd, &summary)) {
                return SeriesSummaryToMap(summary);
            }
            return cypher::FieldData(eit->GetField(fd));
        }
    case NODE_SNAPSHOT:
        {
            // extract vid from snapshot, "V[{id}]"
            CYPHER_THROW_ASSERT(constant.type == cypher::FieldData::SCALAR &&
                                constant.scalar.type == lgraph::FieldType::STRING);
            auto vid =
                std::stoi(constant.scalar.string().substr(2, constant.scalar.string().size() - 3));
            lgraph::series::SeriesSummary summary;
            if (ctx->txn_->GetTxn()->ProbeVertexSeries(vid, fd, &summary)) {
                return SeriesSummaryToMap(summary);
            }
            return cypher::FieldData(ctx->txn_->GetTxn()->GetVertexField(vid, fd));
        }
    case CONSTANT:
        {
            if (constant.type != cypher::FieldData::MAP) {
                THROW_CODE(CypherException, "Only support for map type");
            }
            // The whole member, not just scalars: a summary's `measures` is a
            // list, and a point's entries are scalars - both have to survive
            // `WITH m AS x RETURN x.y`.
            auto it = constant.map->find(fd);
            if (it == constant.map->end()) {
                THROW_CODE(CypherException, "Not found or type mismatch");
            }
            return it->second;
        }
    case RELP_SNAPSHOT:
    default:
        CYPHER_TODO();
    }
}

bool Entry::CheckEntityEfficient(RTContext *ctx) const {
    switch (type) {
    case NODE:
        {
            auto vit = node->ItRef();
            CYPHER_THROW_ASSERT(node && vit);
            return node->IsValidAfterMaterialize(ctx);
        }
    case RELATIONSHIP:
        {
            if (relationship->VarLen()) {
                auto &eits = relationship->ItsRef();
                for (auto &it : eits) {
                    if (!it.IsValid()) return false;
                }
                return true;
            } else {
                auto eit = relationship->ItRef();
                CYPHER_THROW_ASSERT(relationship && eit);
                return eit->IsValid();
            }
        }
    case NODE_SNAPSHOT:
        {
            CYPHER_THROW_ASSERT(constant.type == cypher::FieldData::SCALAR &&
                                constant.scalar.type == lgraph::FieldType::STRING);
            auto vid =
                std::stoi(constant.scalar.string().substr(2, constant.scalar.string().size() - 3));
            auto vit = ctx->txn_->GetVertexIterator(vid, true);
            return vit.IsValid();
        }
    case RELP_SNAPSHOT:
        {
            CYPHER_THROW_ASSERT(constant.type == cypher::FieldData::SCALAR &&
                                constant.scalar.type == lgraph::FieldType::STRING);
            auto str = constant.scalar.string().substr(1, constant.scalar.string().size() - 2);
            std::vector<std::string> euid;
            boost::split(euid, str, boost::is_any_of("_"));
            int64_t src = std::stoi(euid[0]);
            int64_t dst = std::stoi(euid[1]);
            uint16_t lid = std::stoi(euid[2]);
            int64_t tid = std::stoi(euid[3]);
            int64_t eid = std::stoi(euid[4]);
            auto eit = ctx->txn_->GetOutEdgeIterator({src, dst, lid, tid, eid}, true);
            return eit.IsValid();
        }
    case VAR_LEN_RELP:
        {
            auto paths = relationship->path_;
            auto len = paths.Length();
            for (size_t idx = 0; idx < len; ++idx) {
                auto euid = paths.GetNthEdge(idx);
                int64_t vid;
                if (paths.dirs_[idx]) {
                    vid = euid.dst;
                } else {
                    vid = euid.src;
                }
                auto vit = ctx->txn_->GetVertexIterator(vid, true);
                if (!vit.IsValid()) return false;
                auto eit = ctx->txn_->GetOutEdgeIterator(euid, true);
                if (!eit.IsValid()) return false;
                if (idx == len - 1) {
                    int64_t last_vid;
                    if (paths.dirs_[idx]) {
                        last_vid = euid.dst;
                    } else {
                        last_vid = euid.src;
                    }
                    auto vit = ctx->txn_->GetVertexIterator(last_vid, true);
                    if (!vit.IsValid()) return false;
                }
            }
            return true;
        }
    default:
        return false;
    }
}

void Record::SetParameter(const PARAM_TAB &ptab) {
    if (!symbol_table || ptab.empty()) return;
    for (auto &param : ptab) {
        auto it = symbol_table->symbols.find(param.first);
        if (it != symbol_table->symbols.end()) {
            values[it->second.id] = Entry(param.second);
        } else {
            // LOG_WARN() << "Invalid parameter: " << param.first;
        }
    }
}
}  // namespace cypher
