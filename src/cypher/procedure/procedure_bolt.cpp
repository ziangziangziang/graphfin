/**
 * Copyright 2022 AntGroup CO., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

/* Bolt / bolt-raft procedures split out of procedure.cpp: bolt_raft_server.h
 * and bolt_session.h are very heavy to parse, and procedure.cpp no longer fits
 * the compiler's memory budget on a small host. Definitions are unchanged. */

#include "bolt/connection.h"
#include "server/bolt_session.h"
#include "server/bolt_raft_server.h"
#include "cypher/cypher_exception.h"
#include "cypher/procedure/procedure.h"

namespace cypher {

#define CYPHER_ARG_CHECK(pred, msg)           \
    if (!(pred)) {                            \
        throw lgraph::ReminderException(msg); \
    }

void CheckProcedureYieldItem(const std::string &procedure_name,
                             const cypher::VEC_STR &yield_items);
void FillProcedureYieldItem(const std::string &procedure_name,
                            const cypher::VEC_STR &yield_items,
                            std::vector<Record> *records);

void BuiltinProcedure::DbmsSecurityIsDefaultUserPassword(RTContext *ctx,
                                                         const cypher::Record *record,
                                                         const cypher::VEC_EXPR &args,
                                                         const cypher::VEC_STR &yield_items,
                                                         std::vector<Record> *records) {
    CYPHER_ARG_CHECK(
        args.size() == 0,
        "need 0 parameters, e.g. dbms.security.isDefaultUserPassword()")
    CheckProcedureYieldItem("dbms.security.isDefaultUserPassword", yield_items);
    if (ctx->txn_) ctx->txn_->Abort();
    bool is_default_user_password = false;
    if (ctx->bolt_conn_) {
        auto session = (bolt::BoltSession*)(ctx->bolt_conn_->GetContext());
        is_default_user_password = session->using_default_user_password;
    }
    Record r;
    r.AddConstant(lgraph::FieldData(is_default_user_password));
    records->emplace_back(r.Snapshot());
    FillProcedureYieldItem("dbms.security.isDefaultUserPassword", yield_items, records);
}


void BuiltinProcedure::DbBoltListRaftNodes(RTContext *ctx, const Record *record,
                                           const VEC_EXPR &args, const VEC_STR &yield_items,
                                           std::vector<Record> *records) {
    if (!bolt_raft::BoltRaftServer::Instance().Started()) {
        THROW_CODE(BoltRaftError, "bolt raft is not enabled");
        return;
    }
    CheckProcedureYieldItem("db.bolt.listRaftNodes", yield_items);
    CYPHER_ARG_CHECK(args.empty(), FMA_FMT("Function requires 0 arguments, but {} are "
                                           "given. Usage: db.bolt.listRaftPeers()",
                                           args.size()))
    auto infos = bolt_raft::BoltRaftServer::Instance().raft_driver().GetNodeInfosWithLeader();
    for (auto& [_, node] : infos.nodes()) {
        Record r;
        r.AddConstant(lgraph::FieldData::Int64(node.node_id()));
        r.AddConstant(lgraph::FieldData::String(node.ip()));
        r.AddConstant(lgraph::FieldData::Int64(node.bolt_port()));
        r.AddConstant(lgraph::FieldData::Int64(node.bolt_raft_port()));
        r.AddConstant(lgraph::FieldData::Bool(node.is_leader()));
        r.AddConstant(lgraph::FieldData::Bool(node.is_learner()));
        records->emplace_back(r.Snapshot());
    }
    FillProcedureYieldItem("db.bolt.listRaftNodes", yield_items, records);
}

void BuiltinProcedure::DbBoltAddRaftNode(RTContext *ctx, const Record *record,
                                         const VEC_EXPR &args, const VEC_STR &yield_items,
                                         std::vector<Record> *records) {
    if (!bolt_raft::BoltRaftServer::Instance().Started()) {
        THROW_CODE(BoltRaftError, "bolt raft is not enabled");
        return;
    }
    CheckProcedureYieldItem("db.bolt.addRaftNode", yield_items);
    CYPHER_ARG_CHECK(args.size() == 4, FMA_FMT("Function requires 4 arguments, but {} are given",
                                           args.size()))
    CYPHER_ARG_CHECK(args[0].IsInteger(), "node_id type should be Integer")
    CYPHER_ARG_CHECK(args[1].IsString(), "ip type should be String")
    CYPHER_ARG_CHECK(args[2].IsInteger(), "bolt_port type should be Integer")
    CYPHER_ARG_CHECK(args[3].IsInteger(), "bolt_raft_port type should be Integer")
    bolt_raft::NodeInfo node;
    node.set_is_leader(false);
    node.set_is_learner(false);
    node.set_node_id(args[0].constant.scalar.AsInt64());
    node.set_ip(args[1].constant.scalar.AsString());
    node.set_bolt_port(args[2].constant.scalar.AsInt64());
    node.set_bolt_raft_port(args[3].constant.scalar.AsInt64());
    raftpb::ConfChange cc;
    cc.set_type(raftpb::ConfChangeType::ConfChangeAddNode);
    cc.set_node_id(node.node_id());
    cc.set_context(node.SerializeAsString());
    auto promise = bolt_raft::BoltRaftServer::Instance().raft_driver().ProposeConfChange(cc);
    auto err = promise->proposed.get_future().get();
    if (err != nullptr) {
        THROW_CODE(BoltRaftError, err.String());
    }
    promise->applied.get_future().get();
    FillProcedureYieldItem("db.bolt.addRaftNode", yield_items, records);
}

void BuiltinProcedure::DbBoltAddRaftLearnerNode(RTContext *ctx, const Record *record,
                                                const VEC_EXPR &args, const VEC_STR &yield_items,
                                                std::vector<Record> *records) {
    if (!bolt_raft::BoltRaftServer::Instance().Started()) {
        THROW_CODE(BoltRaftError, "bolt raft is not enabled");
        return;
    }
    CheckProcedureYieldItem("db.bolt.addRaftLearnerNode", yield_items);
    CYPHER_ARG_CHECK(args.size() == 4, FMA_FMT("Function requires 4 arguments, but {} are given",
                                               args.size()))
    CYPHER_ARG_CHECK(args[0].IsInteger(), "node_id type should be Integer")
    CYPHER_ARG_CHECK(args[1].IsString(), "ip type should be String")
    CYPHER_ARG_CHECK(args[2].IsInteger(), "bolt_port type should be Integer")
    CYPHER_ARG_CHECK(args[3].IsInteger(), "bolt_raft_port type should be Integer")
    bolt_raft::NodeInfo node;
    node.set_is_leader(false);
    node.set_is_learner(true);
    node.set_node_id(args[0].constant.scalar.AsInt64());
    node.set_ip(args[1].constant.scalar.AsString());
    node.set_bolt_port(args[2].constant.scalar.AsInt64());
    node.set_bolt_raft_port(args[3].constant.scalar.AsInt64());
    raftpb::ConfChange cc;
    cc.set_type(raftpb::ConfChangeType::ConfChangeAddLearnerNode);
    cc.set_node_id(node.node_id());
    cc.set_context(node.SerializeAsString());
    auto promise = bolt_raft::BoltRaftServer::Instance().raft_driver().ProposeConfChange(cc);
    auto err = promise->proposed.get_future().get();
    if (err != nullptr) {
        THROW_CODE(BoltRaftError, err.String());
    }
    promise->applied.get_future().get();
    FillProcedureYieldItem("db.bolt.addRaftLearnerNode", yield_items, records);
}

void BuiltinProcedure::DbBoltRemoveRaftNode(RTContext *ctx, const Record *record,
                                            const VEC_EXPR &args, const VEC_STR &yield_items,
                                            std::vector<Record> *records) {
    if (!bolt_raft::BoltRaftServer::Instance().Started()) {
        THROW_CODE(BoltRaftError, "bolt raft is not enabled");
        return;
    }
    CheckProcedureYieldItem("db.bolt.removeRaftNode", yield_items);
    CYPHER_ARG_CHECK(args.size() == 1, FMA_FMT("Function requires 4 arguments, but {} are given",
                                               args.size()))
    CYPHER_ARG_CHECK(args[0].IsInteger(), "node_id type should be Integer")
    bolt_raft::NodeInfo node;
    node.set_is_leader(false);
    node.set_node_id(args[0].constant.scalar.AsInt64());
    raftpb::ConfChange cc;
    cc.set_node_id(node.node_id());
    cc.set_type(raftpb::ConfChangeType::ConfChangeRemoveNode);
    cc.set_context(node.SerializeAsString());
    auto promise = bolt_raft::BoltRaftServer::Instance().raft_driver().ProposeConfChange(cc);
    auto err = promise->proposed.get_future().get();
    if (err != nullptr) {
        THROW_CODE(BoltRaftError, err.String());
    }
    promise->applied.get_future().get();
    FillProcedureYieldItem("db.bolt.removeRaftNode", yield_items, records);
}

void BuiltinProcedure::DbBoltGetRaftStatus(RTContext *ctx, const Record *record,
                                           const VEC_EXPR &args, const VEC_STR &yield_items,
                                           std::vector<Record> *records) {
    if (!bolt_raft::BoltRaftServer::Instance().Started()) {
        THROW_CODE(BoltRaftError, "bolt raft is not enabled");
        return;
    }
    CheckProcedureYieldItem("db.bolt.getRaftStatus", yield_items);
    CYPHER_ARG_CHECK(args.empty(), FMA_FMT("Function requires 0 arguments, but {} are given",
                                               args.size()))
    auto status = bolt_raft::BoltRaftServer::Instance().raft_driver().GetRaftStatus();
    nlohmann::json obj = nlohmann::json::parse(status.s.String());
    obj["raftlog"]["first"] = status.first_log;
    obj["raftlog"]["last"] = status.last_log;
    Record r;
    r.AddConstant(lgraph::FieldData::String(obj.dump(4)));
    records->emplace_back(r.Snapshot());
    FillProcedureYieldItem("db.bolt.getRaftStatus", yield_items, records);
}

}  // namespace cypher
