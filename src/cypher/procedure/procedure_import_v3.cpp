/**
 * Copyright 2022 AntGroup CO., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

/* Full-import procedure split out of procedure_import.cpp: import_v3.h alone
 * dominates the compiler's memory footprint on a small host. */

#include "db/galaxy.h"
#include "fma-common/file_system.h"
#include "import/import_v3.h"
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

void BuiltinProcedure::DbImportorFullImportor(RTContext *ctx, const Record *record,
                                              const VEC_EXPR &args,
                                              const VEC_STR &yield_items,
                                              std::vector<Record> *records) {
    CheckProcedureYieldItem("db.importor.fullImportor", yield_items);
    ctx->txn_.reset();
    ctx->ac_db_.reset();
    CYPHER_ARG_CHECK(args.size() == 1,
                     "need exactly one parameter. "
                     "e.g. db.importor.fullImportor({})")
    CYPHER_ARG_CHECK(args[0].IsMap(), "conf type should be map")
    if (!ctx->galaxy_->IsAdmin(ctx->user_))
        THROW_CODE(Unauthorized, "Admin access right required.");

    // parse parameter
    lgraph::import_v3::Importer::Config import_config_v3;
    std::map<std::string, lgraph::FieldData> full_import_conf;
    for (auto &kv : *args[0].constant.map) {
        full_import_conf[kv.first] = kv.second.scalar;
    }
    auto check = [&full_import_conf](const std::string &name, lgraph::FieldType type) {
        if (full_import_conf.count(name) && full_import_conf[name].GetType() != type)
            throw lgraph::CypherException("Option " + name + " is not " + to_string(type));
        return full_import_conf.count(name);
    };

    if (!full_import_conf.count("config_file"))
        THROW_CODE(InputError, "Option config_file does not exist.");
    if (!check("graph_name", lgraph::FieldType::STRING))
        full_import_conf["graph_name"] = lgraph::FieldData(lgraph::_detail::DEFAULT_GRAPH_DB_NAME);
    if (!check("delete_if_exists", lgraph::FieldType::BOOL))
        full_import_conf["delete_if_exists"] = lgraph::FieldData(false);
    if (!check("username", lgraph::FieldType::STRING))
        full_import_conf["username"] = lgraph::FieldData(lgraph::_detail::DEFAULT_ADMIN_NAME);
    if (!full_import_conf["delete_if_exists"].AsBool() &&
        ctx->galaxy_->ListGraphsInternal().count(full_import_conf["graph_name"].string())) {
        throw lgraph::CypherException("DB exists and cannot be deleted.");
    }

    std::mutex mu;
    std::condition_variable cv;
    bool ok_to_leave = false;
    std::string data_file_path, log, error;
    std::thread worker([&]() {
        std::lock_guard<std::mutex> l(mu);
        try {
            import_config_v3.db_dir = ctx->galaxy_->GetConfig().dir + "/.import_tmp";
            import_config_v3.graph = lgraph::_detail::DEFAULT_GRAPH_DB_NAME;
            import_config_v3.config_file = full_import_conf["config_file"].string();
            import_config_v3.user = full_import_conf["username"].string();
            if (check("password", lgraph::FieldType::STRING))
                import_config_v3.password = full_import_conf["password"].string();
            if (check("continue_on_error", lgraph::FieldType::BOOL))
                import_config_v3.continue_on_error = full_import_conf["continue_on_error"].AsBool();
            if (check("delimiter", lgraph::FieldType::STRING))
                import_config_v3.delimiter = full_import_conf["delimiter"].string();
            import_config_v3.delete_if_exists = true;
            if (check("parse_block_size", lgraph::FieldType::INT64))
                import_config_v3.parse_block_size = full_import_conf["parse_block_size"].AsInt64();
            if (check("parse_block_threads", lgraph::FieldType::INT64))
                import_config_v3.parse_block_threads =
                    full_import_conf["parse_block_threads"].AsInt64();
            if (check("parse_file_threads", lgraph::FieldType::INT64))
                import_config_v3.parse_file_threads =
                    full_import_conf["parse_file_threads"].AsInt64();
            if (check("generate_sst_threads", lgraph::FieldType::INT64))
                import_config_v3.generate_sst_threads =
                    full_import_conf["generate_sst_threads"].AsInt64();
            if (check("read_rocksdb_threads", lgraph::FieldType::INT64))
                import_config_v3.read_rocksdb_threads =
                    full_import_conf["read_rocksdb_threads"].AsInt64();
            if (check("vid_num_per_reading", lgraph::FieldType::INT64))
                import_config_v3.vid_num_per_reading =
                    full_import_conf["vid_num_per_reading"].AsInt64();
            if (check("max_size_per_reading", lgraph::FieldType::INT64))
                import_config_v3.max_size_per_reading =
                    full_import_conf["max_size_per_reading"].AsInt64();
            if (check("compact", lgraph::FieldType::BOOL))
                import_config_v3.compact = full_import_conf["compact"].AsBool();
            if (check("keep_vid_in_memory", lgraph::FieldType::BOOL))
                import_config_v3.keep_vid_in_memory =
                    full_import_conf["keep_vid_in_memory"].AsBool();
            if (check("enable_fulltext_index", lgraph::FieldType::BOOL))
                import_config_v3.enable_fulltext_index =
                    full_import_conf["enable_fulltext_index"].AsBool();
            if (check("fulltext_index_analyzer", lgraph::FieldType::STRING))
                import_config_v3.fulltext_index_analyzer =
                    full_import_conf["fulltext_index_analyzer"].string();
            import_config_v3.import_online = true;
            lgraph::import_v3::Importer importer(import_config_v3);
            importer.DoImportOffline();
            log = importer.OnlineFullImportLog();
            for (const auto &entry : std::filesystem::directory_iterator(import_config_v3.db_dir)) {
                if (entry.is_directory() && entry.path().filename().string() != ".meta" &&
                    std::filesystem::exists(entry.path().string() + "/data.mdb")) {
                    data_file_path = entry.path().string() + "/data.mdb";
                    break;
                }
            }
        } catch (std::exception &e) {
            error = e.what();
        }
        ok_to_leave = true;
        cv.notify_all();
    });
    worker.detach();
    std::unique_lock<std::mutex> l(mu);
    while (!ok_to_leave) cv.wait(l);
    auto &fs =
        fma_common::FileSystem::GetFileSystem(ctx->galaxy_->GetConfig().dir + "/.import_tmp");
    if (error.empty()) {
        lgraph::DBConfig dbConfig;
        dbConfig.dir = ctx->galaxy_->GetConfig().dir;
        dbConfig.name = full_import_conf["graph_name"].string();
        dbConfig.create_if_not_exist = true;
        ctx->galaxy_->CreateGraph(full_import_conf["username"].string(),
                                  full_import_conf["graph_name"].string(), dbConfig,
                                  data_file_path);
        fs.RemoveDir(ctx->galaxy_->GetConfig().dir + "/.import_tmp");
        Record r;
        r.AddConstant(lgraph::FieldData(log));
        records->emplace_back(r.Snapshot());
    } else {
        fs.RemoveDir(ctx->galaxy_->GetConfig().dir + "/.import_tmp");
        throw lgraph::CypherException(error);
    }
    FillProcedureYieldItem("db.importor.fullImportor", yield_items, records);
}


}  // namespace cypher
