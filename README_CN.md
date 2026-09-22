# GraphFin

**原生支持时序数据的通用图数据库。**

[English](README.md) · [文档](docs/README.md) · [快速开始](docs/getting-started.md) · [测试计划](docs/testing/post-merge.md) · [发布状态](RELEASE.md)

![实体关系、采样观测与紧凑存储层的概念示意](docs/images/product/graph-time-hero.png)

将实体关系与随时间变化的观测保存在一起。GraphFin 基于 TuGraph 的属性图能力，
增加顶点与边的原生时序字段、多图资源管理，并整合已有复制基础设施。
金融依赖分析、供应链和设备遥测使用同一套通用接口。

**当前状态：Alpha 版本验证进行中。** 已确认产品版本为 `0.1.0-alpha`，
目前不代表已达到生产发布条件。整图分片目前主要提供控制面组件，真实请求转发及迁移数据搬运
尚未完成。发布阻塞项见 [TASK.md](TASK.md)。

## 可以用来做什么

| 能力 | 用途 |
| --- | --- |
| 关系与观测结合 | 顶点和边的有类型时序字段，例如证券价格、传感器读数、依赖关系权重 |
| 原生时序操作 | 追加或替换观测点、修改指标、比较后更新，以及点查询、范围查询、首尾值和聚合 |
| 多个独立图 | 独立图存储、按需加载与同时打开图数量限制 |
| 复制基础 | 已有 HA 机制；合并后的时序数据仍须通过故障切换与恢复验证 |
| 放置与路由组件 | 持久身份、放置版本和路由决策；面向用户的分布式转发尚未完成 |
| 常用接口 | Cypher、REST、RPC、Bolt 和 Python，以及明确的传输契约 |

应用可以沿着 `SUPPLIES` 关系选取供应商并读取其观测；设备场景则沿着
`FEEDS` 关系读取上下游机器的指标。GraphFin 提供存储和查询基础，
领域模型和统计分析由上层应用实现。

```cypher
CALL db.createVertexLabel('Sensor', 'id', 'id', 'INT64', false);
CALL db.createSeriesField('Sensor', 'readings',
  [{name:'temperature', type:'DOUBLE'}], {}) YIELD field RETURN field;
CREATE (s:Sensor {id:1});
MATCH (s:Sensor {id:1})
CALL series.append(s, 'readings',
  {ts:datetime('2024-01-02 00:00:00'), temperature:21.5})
YIELD written RETURN written;
MATCH (s:Sensor {id:1}) RETURN series.latest(s, 'readings') AS latest;
```

请在新图中逐条执行。可运行的[遥测示例](demo/SeriesTelemetry/telemetry.py)与
[金融示例](demo/SeriesFinancial/financial.py)演示客户端请求和数据导出。

## 本地启动

按照[开发环境快速开始](docs/getting-started.md)准备环境、编译本仓库并启动服务。
上游 TuGraph 的运行镜像不包含本次合并的功能。

```bash
# 准备好文档指定的编译镜像后：
CLEAN=0 JOBS=2 BUILD_TYPE=RelWithDebInfo bash ci/phase0/build.sh

# 复用二进制；以下命令不会重新编译：
bash ci/merge/run.sh unit
bash ci/merge/run.sh smoke
```

运行器隔离测试数据库，必测用例被跳过时判定失败，并记录源码、二进制和镜像来源。
[测试计划](docs/testing/post-merge.md)分别定义快速回归、三节点 HA 和规模／长时间运行验证。

## 能力边界

- 每个时间戳目前只有一个可修改观测点。修订历史、知识时间查询和时态图选择仍在规划中。
- 时间戳精度为微秒，不携带时区标识。应用必须约定统一时间基准。
- 范围查询包含两端；有界流式读取和稳定游标接口尚待实现。
- 追加会替换已有时间点。基于值的 CAS 不等同于请求去重或修订令牌协议。
- 新写入的 DOUBLE 指标必须是有限值。集合与 INT64 处理遵循
  [客户端契约](docs/architecture/09-series-client-contracts.md)。
- 整图分片不等于图内分片、跨分片查询、分布式事务或在线迁移。

部署前请阅读[能力矩阵](docs/product.md)、[路线图](docs/roadmap.md)与[发布清单](RELEASE.md)。
上方图片仅表达产品概念，不代表已通过验证的集群拓扑。

## 开发与致谢

GraphFin 是 [TuGraph](https://github.com/TuGraph-family/tugraph-db) 的衍生项目，
保留上游作者、版权声明及 [Apache-2.0 许可证](LICENSE)。
为兼容既有集成，保留 `lgraph_*` 可执行文件和 API 名称；引擎兼容版本 `4.5.2`
与 GraphFin 产品发布版本分开管理。

欢迎在本仓库提交问题与改进。提交格式示例：
`test(series): cover snapshot restore across tenants`。
继承的上游参考手册与当前产品文档分别索引，避免将历史说明误当成当前支持承诺。
