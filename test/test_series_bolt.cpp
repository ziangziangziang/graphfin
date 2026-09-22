/**
 * Bolt wire assertions for time-series query results.
 *
 * The live Bolt path serves `Result::BoltRecords()` (`lgraph_result.cpp`),
 * i.e. `ResultElement::ToBolt()` per cell: scalars convert to Bolt natives
 * (INT64, DOUBLE, LocalDateTime, ...), while LIST/MAP cells become JSON
 * *strings* (the structured std::any conversion is commented out upstream).
 * REST, by contrast, serves structured JSON. Both shapes are pinned here
 * end-to-end: Cypher -> Result -> BoltRecords -> PackStream bytes.
 */

#include <any>
#include <memory>
#include <string>
#include <vector>

#include "./graph_factory.h"
/* Make sure include graph_factory.h BEFORE antlr4-runtime.h. Otherwise causing the following error:
 * ‘EOF’ was not declared in this scope.
 * For the former (include/butil) uses macro EOF, which is undefined in antlr4. */
#include "./antlr4-runtime.h"
#include "geax-front-end/ast/AstNode.h"
#include "geax-front-end/ast/AstDumper.h"

#include "cypher/parser/generated/LcypherLexer.h"
#include "cypher/parser/generated/LcypherParser.h"
#include "cypher/parser/cypher_base_visitor_v2.h"
#include "cypher/parser/cypher_error_listener.h"
#include "cypher/rewriter/GenAnonymousAliasRewriter.h"
#include "cypher/rewriter/MultiPathPatternRewriter.h"
#include "fma-common/file_system.h"
#include "db/galaxy.h"
#include "cypher/rewriter/PushDownFilterAstRewriter.h"
#include "cypher/execution_plan/runtime_context.h"
#include "cypher/execution_plan/execution_plan_v2.h"
#include "lgraph/lgraph_result.h"
#include "lgraph/lgraph_types.h"
#include "bolt/pack_stream.h"
#include "bolt/temporal.h"
#include "./ut_utils.h"
#include "./ut_config.h"

using namespace geax::frontend;
using geax::frontend::GEAXErrorCode;

class TestSeriesBolt : public TuGraphTest {
  private:
    std::shared_ptr<cypher::RTContext> ctx_;
    std::shared_ptr<lgraph::Galaxy> galaxy_;
    lgraph::Galaxy::Config gconf_;
    std::string db_dir_ = "./testdb_series_bolt";
    std::string graph_name_ = "default";

  protected:
    void SetUp() override {
        ctx_.reset();
        galaxy_.reset();
        GraphFactory::create_graph(GraphFactory::GRAPH_DATASET_TYPE::EMPTY, db_dir_);
        gconf_.dir = db_dir_;
        galaxy_ = std::make_shared<lgraph::Galaxy>(gconf_, true, nullptr);
        ctx_ = std::make_shared<cypher::RTContext>(
            nullptr, galaxy_.get(), lgraph::_detail::DEFAULT_ADMIN_NAME, graph_name_);
    }

    void TearDown() override {
        ctx_.reset();
        galaxy_.reset();
        fma_common::FileSystem::GetFileSystem(db_dir_).RemoveDir(db_dir_);
    }

    bool RunCypher(const std::string& cypher) {
        try {
            antlr4::ANTLRInputStream input(cypher);
            parser::LcypherLexer lexer(&input);
            antlr4::CommonTokenStream tokens(&lexer);
            parser::LcypherParser parser(&tokens);
            parser.addErrorListener(&parser::CypherErrorListener::INSTANCE);
            geax::common::ObjectArenaAllocator objAlloc_;
            parser::CypherBaseVisitorV2 visitor(objAlloc_, parser.oC_Cypher(), ctx_.get());
            AstNode* node = visitor.result();
            cypher::GenAnonymousAliasRewriter gen_anonymous_alias_rewriter;
            node->accept(gen_anonymous_alias_rewriter);
            cypher::MultiPathPatternRewriter multi_path_pattern_rewriter(objAlloc_);
            node->accept(multi_path_pattern_rewriter);
            cypher::PushDownFilterAstRewriter push_down_filter_ast_writer(objAlloc_, ctx_.get());
            node->accept(push_down_filter_ast_writer);
            AstDumper dumper;
            if (dumper.handle(node) != GEAXErrorCode::GEAX_SUCCEED) return false;
            cypher::ExecutionPlanV2 execution_plan_v2;
            if (execution_plan_v2.Build(node, ctx_.get()) != GEAXErrorCode::GEAX_SUCCEED) {
                return false;
            }
            execution_plan_v2.Execute(ctx_.get());
        } catch (std::exception& e) {
            UT_LOG() << "cypher failed: " << e.what() << " [" << cypher << "]";
            return false;
        }
        return true;
    }

    // The Bolt cells of the last query: one std::any per column per row.
    std::vector<std::vector<std::any>> BoltCells() { return ctx_->result_->BoltRecords(); }

    void ExpectPackable(const std::vector<std::vector<std::any>>& cells) {
        bolt::MarkersInit();
        bolt::PackStream ps;
        ps.AppendRecords(cells);
        EXPECT_FALSE(ps.ConstBuffer().empty());
    }
};

TEST_F(TestSeriesBolt, SeriesScalarsReachBoltAsBoltNatives) {
    ASSERT_TRUE(RunCypher("CALL db.createVertexLabel('Company', 'id', 'id', 'INT64', false)"));
    ASSERT_TRUE(RunCypher(
        "CALL db.createSeriesField('Company', 'prices', "
        "[{name:'close', type:'DOUBLE'}, {name:'volume', type:'INT64'}], "
        "{bucket_max_points:100}) YIELD field RETURN field"));
    ASSERT_TRUE(RunCypher("CREATE (c:Company {id:1})"));
    ASSERT_TRUE(RunCypher(
        "MATCH (c:Company {id:1}) CALL series.append(c, 'prices', {ts: "
        "datetime('2024-01-02 00:00:00'), close: 12.5, volume: 7}) "
        "YIELD written RETURN written"));

    // INT64 measure extremes survive the Bolt int encoding.
    ASSERT_TRUE(RunCypher(
        "MATCH (c:Company {id:1}) CALL series.update(c, 'prices', 'volume', "
        "datetime('2024-01-02 00:00:00'), 9223372036854775807) "
        "YIELD written RETURN written"));
    ASSERT_TRUE(
        RunCypher("MATCH (c:Company {id:1}) RETURN series.count(c, 'prices') AS n"));
    {
        auto cells = BoltCells();
        ASSERT_EQ(cells.size(), 1u);
        ASSERT_EQ(cells[0].size(), 1u);
        EXPECT_EQ(std::any_cast<int64_t>(cells[0][0]), 1);
        ExpectPackable(cells);
    }

    // A summary cell is a MAP upstream, hence a JSON string on Bolt (pinned
    // intentional difference from REST, which serves structured JSON).
    ASSERT_TRUE(RunCypher("MATCH (c:Company {id:1}) RETURN c.prices AS summary"));
    {
        auto cells = BoltCells();
        ASSERT_EQ(cells.size(), 1u);
        ASSERT_EQ(cells[0].size(), 1u);
        const std::string s = std::any_cast<std::string>(cells[0][0]);
        EXPECT_NE(s.find("\"count\":1"), std::string::npos);
        EXPECT_NE(s.find("2024-01-02 00:00:00"), std::string::npos);
        EXPECT_NE(s.find("volume"), std::string::npos);
        ExpectPackable(cells);
    }

    // A point cell is a MAP upstream, hence a JSON string on Bolt as well.
    ASSERT_TRUE(RunCypher(
        "MATCH (c:Company {id:1}) RETURN series.at(c, 'prices', "
        "datetime('2024-01-02 00:00:00')) AS point"));
    {
        auto cells = BoltCells();
        ASSERT_EQ(cells.size(), 1u);
        const std::string s = std::any_cast<std::string>(cells[0][0]);
        EXPECT_NE(s.find("12.5"), std::string::npos);
        EXPECT_NE(s.find("9223372036854775807"), std::string::npos);
        ExpectPackable(cells);
    }

    // A range cell is a LIST upstream, hence one JSON string on Bolt.
    ASSERT_TRUE(RunCypher(
        "MATCH (c:Company {id:1}) RETURN series.range(c, 'prices', "
        "datetime('2024-01-01 00:00:00'), datetime('2024-01-03 00:00:00')) AS pts"));
    {
        auto cells = BoltCells();
        ASSERT_EQ(cells.size(), 1u);
        const std::string s = std::any_cast<std::string>(cells[0][0]);
        EXPECT_EQ(s.front(), '[');
        EXPECT_NE(s.find("12.5"), std::string::npos);
        ExpectPackable(cells);
    }

    // R8: non-finite doubles are rejected before mutation; the stored point
    // and the INT64 measure survive.
    EXPECT_FALSE(RunCypher(
        "MATCH (c:Company {id:1}) CALL series.update(c, 'prices', 'close', "
        "datetime('2024-01-02 00:00:00'), toFloat('NaN')) "
        "YIELD written RETURN written"));
    ASSERT_TRUE(
        RunCypher("MATCH (c:Company {id:1}) RETURN series.at(c, 'prices', "
                  "datetime('2024-01-02 00:00:00')) AS point"));
    {
        auto cells = BoltCells();
        ASSERT_EQ(cells.size(), 1u);
        const std::string s = std::any_cast<std::string>(cells[0][0]);
        EXPECT_NE(s.find("12.5"), std::string::npos);
        EXPECT_NE(s.find("9223372036854775807"), std::string::npos);
        ExpectPackable(cells);
    }
}

TEST_F(TestSeriesBolt, BoltScalarsNullsAndBoundaries) {
    // DATETIME reaches Bolt as a structured LocalDateTime, not text.
    ASSERT_TRUE(RunCypher("RETURN datetime('2024-01-02 00:00:00') AS dt"));
    {
        auto cells = BoltCells();
        ASSERT_EQ(cells.size(), 1u);
        const bolt::LocalDateTime dt = std::any_cast<bolt::LocalDateTime>(cells[0][0]);
        EXPECT_EQ(dt.seconds, 1704153600);
        EXPECT_EQ(dt.nanoseconds, 0);
        ExpectPackable(cells);
    }
    // INT64 boundaries round-trip exactly (the minimum is spelled as an
    // expression: a bare -9223372036854775808 literal overflows int64 lexing).
    ASSERT_TRUE(RunCypher("RETURN 9223372036854775807 AS mx, 0-9223372036854775807-1 AS mn"));
    {
        auto cells = BoltCells();
        ASSERT_EQ(cells.size(), 1u);
        EXPECT_EQ(std::any_cast<int64_t>(cells[0][0]), 9223372036854775807LL);
        EXPECT_EQ(std::any_cast<int64_t>(cells[0][1]),
                  std::numeric_limits<int64_t>::min());
        ExpectPackable(cells);
    }
    // Null stays null: an empty any, never a zero or empty string.
    ASSERT_TRUE(RunCypher("RETURN null AS n"));
    {
        auto cells = BoltCells();
        ASSERT_EQ(cells.size(), 1u);
        EXPECT_FALSE(cells[0][0].has_value());
        ExpectPackable(cells);
    }
    // Empty containers keep their shape as JSON text, never null.
    ASSERT_TRUE(RunCypher("RETURN [] AS e, {} AS m"));
    {
        auto cells = BoltCells();
        ASSERT_EQ(cells.size(), 1u);
        EXPECT_EQ(std::any_cast<std::string>(cells[0][0]), "[]");
        EXPECT_EQ(std::any_cast<std::string>(cells[0][1]), "{}");
        ExpectPackable(cells);
    }
    // A string that looks like JSON stays a string (detached property read).
    ASSERT_TRUE(RunCypher(
        "CALL db.createVertexLabel('Doc', 'id', 'id', 'INT64', false, 'note', 'STRING', true)"));
    ASSERT_TRUE(RunCypher("CREATE (d:Doc {id:1, note:'{\"a\":1}'})"));
    ASSERT_TRUE(RunCypher("MATCH (d:Doc {id:1}) RETURN d.note AS note"));
    {
        auto cells = BoltCells();
        ASSERT_EQ(cells.size(), 1u);
        EXPECT_EQ(std::any_cast<std::string>(cells[0][0]), "{\"a\":1}");
        ExpectPackable(cells);
    }
}
