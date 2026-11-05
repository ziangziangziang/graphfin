/**
 * Copyright 2022 AntGroup CO., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

/* Import procedures split out of procedure.cpp: import_v3.h / import_online.h
 * are very heavy to parse, and procedure.cpp no longer fits the compiler's
 * memory budget on a small host. The function definitions are unchanged. */

#include "db/galaxy.h"
#include "fma-common/encrypt.h"
#include "import/import_online.h"
#include "cypher/cypher_exception.h"
#include "cypher/procedure/procedure.h"

namespace cypher {

#define CYPHER_ARG_CHECK(pred, msg)           \
    if (!(pred)) {                            \
        throw lgraph::ReminderException(msg); \
    }

#define CYPHER_DB_PROCEDURE_GRAPH_CHECK()                                                     \
    do {                                                                                      \
        if (ctx->graph_.empty()) throw lgraph::CypherException("graph name cannot be empty"); \
        if (!ctx->ac_db_) CYPHER_INTL_ERR();                                                  \
        if (!ctx->txn_) CYPHER_INTL_ERR();                                                    \
    } while (0)

void CheckProcedureYieldItem(const std::string &procedure_name,
                             const cypher::VEC_STR &yield_items);
void FillProcedureYieldItem(const std::string &procedure_name,
                            const cypher::VEC_STR &yield_items,
                            std::vector<Record> *records);

void BuiltinProcedure::DbImportorDataImportor(RTContext *ctx, const Record *record,
                                              const VEC_EXPR &args,
                                              const VEC_STR &yield_items,
                                              std::vector<Record> *records) {
    CheckProcedureYieldItem("db.importor.dataImportor", yield_items);
    CYPHER_ARG_CHECK(args.size() == 5,
                     "need five parameters, e.g. db.importor.dataImportor"
                     "(description, content, continue_on_error, thread_nums, delimiter)")
    CYPHER_ARG_CHECK(args[0].IsString(), "description type should be string")
    CYPHER_ARG_CHECK(args[1].IsString(), "content type should be string")
    CYPHER_ARG_CHECK(args[2].IsBool(), "continue_on_error type should be boolean");
    CYPHER_ARG_CHECK(args[3].IsInteger(), "thread_nums type should be integer")
    CYPHER_ARG_CHECK(args[4].IsString(), "delimiter type should be string")
    CYPHER_DB_PROCEDURE_GRAPH_CHECK();
    if (ctx->txn_) ctx->txn_->Abort();

    lgraph::AccessControlledDB db = ctx->galaxy_->OpenGraph(ctx->user_, ctx->graph_);
    if (db.GetAccessLevel() < lgraph::AccessLevel::WRITE)
        throw lgraph::CypherException("Need write permission to do import.");
    lgraph::import_v2::ImportOnline::Config config;
    config.continue_on_error = args[2].constant.scalar.AsBool();
    config.n_threads = args[3].constant.scalar.integer();
    config.delimiter = args[4].constant.scalar.AsString();
    fma_common::encrypt::Base64 base64;
    std::string desc = base64.Decode(args[0].constant.scalar.AsString());
    std::string content = base64.Decode(args[1].constant.scalar.AsString());
    std::string log = lgraph::import_v2::ImportOnline::HandleOnlineTextPackage(
        std::move(desc), std::move(content), db.GetLightningGraph(), config);
    FillProcedureYieldItem("db.importor.dataImportor", yield_items, records);
}

void BuiltinProcedure::DbImportorFullFileImportor(RTContext *ctx, const Record *record,
                                                  const VEC_EXPR &args,
                                                  const VEC_STR &yield_items,
                                                  std::vector<Record> *records) {
    CheckProcedureYieldItem("db.importor.fullFileImportor", yield_items);
    ctx->txn_.reset();
    ctx->ac_db_.reset();
    CYPHER_ARG_CHECK((args.size() == 2 || args.size() == 3),
                     "need no more than three parameters and no less than two parameters. "
                     "e.g. db.importor.fullFileImportor({\"ha\",\"data.mdb\",[false]})")
    CYPHER_ARG_CHECK(args[0].IsString(), "graph_name type should be string")
    CYPHER_ARG_CHECK(args[1].IsString(), "path type should be string")
    bool remote = false;
    if (args.size() == 3) {
        CYPHER_ARG_CHECK(args[2].IsBool(), "remote type should be bool")
        remote = args[2].constant.scalar.AsBool();
    }
    if (!ctx->galaxy_->IsAdmin(ctx->user_))
        THROW_CODE(Unauthorized, "Admin access right required.");
    std::string graph_name = args[0].constant.scalar.AsString(),
                path = args[1].constant.scalar.AsString();
    if (remote) {
        std::string outputFilename = ctx->galaxy_->GetConfig().dir + "/.import.file.data.mdb";
        std::string command = "wget -O " + outputFilename + " " + path;
        int result = std::system(command.c_str());
        if (result == 0) {
            LOG_INFO() << "Downloaded file saved as: " << outputFilename;
        } else {
            LOG_ERROR() << "Failed to download file";
            throw lgraph::CypherException("Failed to download file");
        }
        path = ctx->galaxy_->GetConfig().dir + "/.import.file.data.mdb";
    }

    lgraph::DBConfig dbConfig;
    dbConfig.dir = ctx->galaxy_->GetConfig().dir;
    dbConfig.name = graph_name;
    dbConfig.create_if_not_exist = true;
    bool success = ctx->galaxy_->CreateGraph(ctx->user_, graph_name, dbConfig, path);
    if (remote) {
        auto &fs_download = fma_common::FileSystem::GetFileSystem(ctx->galaxy_->GetConfig().dir +
                                                                  "/.import.file.data.mdb");
        fs_download.Remove(ctx->galaxy_->GetConfig().dir + "/.import.file.data.mdb");
    }
    if (!success) throw lgraph::GraphCreateException(args[0].constant.scalar.AsString());
    FillProcedureYieldItem("db.importor.fullFileImportor", yield_items, records);
}

void BuiltinProcedure::DbImportorSchemaImportor(RTContext *ctx, const Record *record,
                                                const VEC_EXPR &args,
                                                const VEC_STR &yield_items,
                                                std::vector<Record> *records) {
    CheckProcedureYieldItem("db.importor.schemaImportor", yield_items);
    CYPHER_ARG_CHECK(args.size() == 1,
                     "need one parameters, e.g. db.importor.schemaImportor(description)")
    CYPHER_ARG_CHECK(args[0].IsString(), "description type should be string")
    CYPHER_DB_PROCEDURE_GRAPH_CHECK();
    if (ctx->txn_) ctx->txn_->Abort();

    lgraph::AccessControlledDB db = ctx->galaxy_->OpenGraph(ctx->user_, ctx->graph_);
    if (db.GetAccessLevel() < lgraph::AccessLevel::WRITE)
        throw lgraph::CypherException("Need write permission to do import.");
    fma_common::encrypt::Base64 base64;
    std::string desc = base64.Decode(args[0].constant.scalar.AsString());
    std::string log = ::lgraph::import_v2::ImportOnline::HandleOnlineSchema(std::move(desc), db);
    FillProcedureYieldItem("db.importor.schemaImportor", yield_items, records);
}

void BuiltinProcedure::DbCreateEdgeLabelByJson(RTContext *ctx, const Record *record,
                                               const VEC_EXPR &args,
                                               const VEC_STR &yield_items,
                                               std::vector<Record> *records) {
    CYPHER_ARG_CHECK(args.size() % SPEC_MEMBER_SIZE == 1,
                     "e.g. db.createEdgeLabelByJson(json_data)");
    CYPHER_ARG_CHECK(args[0].IsString(), "json_data type should be json string")
    CheckProcedureYieldItem("db.createEdgeLabelByJson", yield_items);
    nlohmann::json schema;
    schema["schema"] = nlohmann::json::array();
    auto ec = nlohmann::json::parse(args[0].constant.AsString());
    schema["schema"].push_back(ec);
    auto schema_desc = lgraph::import_v2::ImportConfParser::ParseSchema(schema);
    if (schema_desc.label_desc.size() != 1) {
        THROW_CODE(InputError, "json schema size error, size:{}", schema_desc.label_desc.size());
    }
    CYPHER_DB_PROCEDURE_GRAPH_CHECK();
    if (ctx->txn_) ctx->txn_->Abort();
    std::vector<lgraph_api::FieldSpec> fds;
    const auto& ld = schema_desc.label_desc[0];
    if (ld.is_vertex) {
        THROW_CODE(InputError, "is not edge json schema");
    }
    for (auto& p : ld.GetSchemaDef())
        fds.emplace_back(p.second);
    lgraph_api::EdgeOptions eo;
    if (ld.HasTemporalField()) {
        auto tf = ld.GetTemporalField();
        eo.temporal_field = tf.name;
        eo.temporal_field_order = tf.temporal_order;
    }
    eo.edge_constraints = ld.edge_constraints;
    eo.detach_property = ld.detach_property;
    bool ok = ctx->ac_db_->AddLabel(ld.is_vertex, ld.name, fds, eo);
    if (ok) {
        LOG_INFO() << FMA_FMT("Add {} label:{}", ld.is_vertex ? "vertex" : "edge",
                              ld.name);
    } else {
        throw lgraph::LabelExistException(ld.name, false);
    }
    for (auto& spec : ld.columns) {
        if (spec.index) {
            ctx->ac_db_->AddEdgeIndex(ld.name, spec.name, spec.idxType);
            LOG_INFO() << FMA_FMT("Add edge index [label:{}, field:{}, type:{}]",
                                  ld.name, spec.name, static_cast<int>(spec.idxType));
        }
    }
    FillProcedureYieldItem("db.createEdgeLabelByJson", yield_items, records);
}

void BuiltinProcedure::DbCreateVertexLabelByJson(RTContext *ctx, const Record *record,
                                                 const VEC_EXPR &args,
                                                 const VEC_STR &yield_items,
                                                 std::vector<Record> *records) {
    CYPHER_ARG_CHECK(args.size() % SPEC_MEMBER_SIZE == 1,
                     "e.g. db.createVertexLabelByJson(json_data)");
    CYPHER_ARG_CHECK(args[0].IsString(), "json_data type should be json string")
    nlohmann::json schema;
    schema["schema"] = nlohmann::json::array();
    auto ec = nlohmann::json::parse(args[0].constant.AsString());
    schema["schema"].push_back(ec);
    auto schema_desc = lgraph::import_v2::ImportConfParser::ParseSchema(schema);
    if (schema_desc.label_desc.size() != 1) {
        THROW_CODE(InputError, "json schema size error, size:{}", schema_desc.label_desc.size());
    }
    CYPHER_DB_PROCEDURE_GRAPH_CHECK();
    CheckProcedureYieldItem("db.createVertexLabelByJson", yield_items);
    if (ctx->txn_) ctx->txn_->Abort();
    std::vector<lgraph_api::FieldSpec> fds;
    const auto& ld = schema_desc.label_desc[0];
    if (!ld.is_vertex) {
        THROW_CODE(InputError, "is not vertex json schema");
    }
    for (auto& p : ld.GetSchemaDef())
        fds.emplace_back(p.second);
    lgraph_api::VertexOptions vo;
    vo.primary_field = ld.GetPrimaryField().name;
    vo.detach_property = ld.detach_property;
    bool ok = ctx->ac_db_->AddLabel(ld.is_vertex, ld.name, fds, vo);
    if (ok) {
        LOG_INFO() << FMA_FMT("Add {} label:{}", ld.is_vertex ? "vertex" : "edge",
                              ld.name);
    } else {
        throw lgraph::LabelExistException(ld.name, true);
    }
    for (auto& spec : ld.columns) {
        if (spec.index && !spec.primary) {
            ctx->ac_db_->AddVertexIndex(ld.name, spec.name, spec.idxType);
            LOG_INFO() << FMA_FMT("Add vertex index [label:{}, field:{}, type:{}]",
                                  ld.name, spec.name, static_cast<int>(spec.idxType));
        }
    }
    FillProcedureYieldItem("db.createVertexLabelByJson", yield_items, records);
}

}  // namespace cypher
