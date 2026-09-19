# TuGraph Scaling Project — Engineering Prompts

## Phase 0 — Architecture Baseline and Test Harness

### Project demand

You are working on a fork of TuGraph DB with the long-term objective of supporting a horizontally scalable, highly available, multi-graph database platform.

Before changing database behavior, establish a reliable engineering baseline.

The goals of this phase are to:

* make the project reproducibly buildable;
* document the existing architecture relevant to graph lifecycle, storage, transactions, HA, Raft replication, metadata, backup, and recovery;
* create automated correctness and performance test infrastructure;
* establish measurements that future phases can compare against.

Do not introduce architectural changes during this phase unless they are required to make testing or reproducible builds possible.

### Methodology

1. Build TuGraph from a clean environment and document all required dependencies, compiler versions, build flags, and runtime dependencies.

2. Create a reproducible development environment using an appropriate mechanism such as:

   * Docker;
   * devcontainer;
   * reproducible build scripts;
   * CI workflow.

3. Identify and document the major components involved in:

   * `Galaxy`;
   * `GraphManager`;
   * `LightningGraph`;
   * graph creation and deletion;
   * graph startup and shutdown;
   * transaction lifecycle;
   * storage engine;
   * graph metadata;
   * ACL/security metadata;
   * snapshot creation;
   * backup and restore;
   * HA/Raft implementation;
   * server startup;
   * Bolt/OpenCypher request processing.

4. Build automated benchmark scenarios for:

   * 1 graph;
   * 100 graphs;
   * 1,000 graphs;
   * 4,000 graphs.

5. Record at minimum:

   * process startup time;
   * shutdown time;
   * memory consumption;
   * file descriptor count;
   * storage consumed per empty graph;
   * graph creation latency;
   * graph deletion latency;
   * first-query latency;
   * repeated-query latency;
   * transaction throughput;
   * backup duration;
   * restore duration.

6. Create automated correctness tests covering:

   * graph creation;
   * graph deletion;
   * graph reopening;
   * CRUD operations;
   * transactions;
   * indexes;
   * schemas;
   * ACL;
   * restart recovery.

7. Add failure tests for:

   * process termination;
   * server restart;
   * partially completed operations where practical.

8. Produce an architecture report identifying components that are likely to become bottlenecks when increasing graph count to 10,000, 100,000, and eventually beyond.

### Deliverables

Produce:

* reproducible build environment;
* CI workflow;
* benchmark suite;
* correctness/integration test suite;
* baseline benchmark results;
* architecture document;
* identified technical risks;
* list of components likely to require modification in later phases.

### Success criteria

This phase is complete only when:

* a clean environment can build TuGraph without undocumented manual intervention;
* all existing upstream unit tests pass;
* newly added integration tests pass;
* benchmarks can automatically run against 1, 100, 1,000, and 4,000 graphs;
* startup time, RAM, disk, and file descriptor usage have been recorded for each test size;
* restart/recovery tests complete without data corruption;
* the architecture document clearly explains graph lifecycle and HA/Raft paths;
* all benchmark results can be reproduced by another developer.

Do not begin optimization work unless the baseline results demonstrate an issue that prevents completion of this phase.

---

# Phase 1 — Remove the 4096 Graph Limit

### Project demand

TuGraph currently has an explicit graph-count restriction around 4096 graphs.

Modify the system so graph count is no longer constrained by a hard-coded 4096 constant.

The limit should become configurable.

The system must initially support at least 10,000 graphs without correctness regressions.

This phase is about eliminating artificial restrictions, not yet redesigning graph lifecycle for hundreds of thousands of graphs.

### Methodology

1. Find every graph-count limit in the codebase.

2. Identify:

   * constants;
   * validation functions;
   * API checks;
   * CLI checks;
   * import paths;
   * backup/restore paths;
   * management APIs;
   * tests;
   * documentation.

3. Replace hard-coded graph-count restrictions with configuration.

Recommended behavior:

`max_graphs = N`

where:

* `N > 0` specifies an explicit limit;
* optionally, `0` may mean no application-level limit.

4. Ensure graph-count validation is implemented through one authoritative code path.

5. Investigate and fix inconsistent or off-by-one validation.

6. Confirm that no persistent storage format, graph ID format, bitmap, integer field, or serialization format assumes a maximum of 4096 graphs.

7. Add automated tests for:

   * 4095 graphs;
   * 4096 graphs;
   * 4097 graphs;
   * 10,000 graphs;
   * configured limit exceeded;
   * configured unlimited mode if implemented.

8. Verify:

   * creation;
   * opening;
   * listing;
   * deletion;
   * ACL;
   * restart;
   * backup;
   * restore.

9. Update documentation.

### Deliverables

Produce:

* configurable graph-count limit;
* removal of hard-coded 4096 assumptions;
* consistent graph-count validation;
* tests covering boundary cases;
* documentation;
* benchmark comparison against Phase 0.

### Success criteria

This phase is complete only when:

* 10,000 graphs can be created successfully;
* the server can restart with 10,000 graphs;
* every graph remains addressable after restart;
* CRUD works correctly against randomly selected graphs;
* graph creation and deletion remain correct;
* ACL behavior remains correct;
* backup and restore succeed;
* graph count is configurable;
* no remaining hard-coded 4096 graph limit exists in production code;
* no storage-format dependency on 4096 has been found;
* all existing and new tests pass.

Performance degradation may exist at 10,000 graphs, but correctness must not.

---

# Phase 2 — Scalable Graph Lifecycle and 100K Graph Support

### Project demand

Refactor TuGraph so the server can manage very large numbers of logical graphs without opening every graph at startup.

Target:

* at least 100,000 registered logical graphs;
* only a configurable subset physically open at one time;
* bounded memory consumption;
* bounded file descriptor consumption;
* startup cost primarily dependent on graph metadata rather than opening every storage environment.

Introduce lazy graph loading and automatic inactive-graph eviction.

### Methodology

1. Analyze the current `GraphManager` and graph startup behavior.

2. Separate the concepts of:

   * graph metadata;
   * registered graph;
   * open graph;
   * active graph;
   * graph with running transactions.

3. Introduce a lightweight graph catalog containing metadata required to locate and authorize graphs without opening their underlying storage.

4. Implement lazy opening:

When a request targets graph X:

* locate X in the catalog;
* check whether X is open;
* open X if necessary;
* return a safe graph handle.

5. Implement an active graph cache.

Use an appropriate eviction strategy such as LRU or segmented LRU.

6. Introduce configuration such as:

* `max_open_graphs`;
* `graph_idle_timeout`;
* optional memory-based limits.

7. Ensure active graphs cannot be evicted while:

   * transactions are running;
   * iterators exist;
   * procedures are executing;
   * backups/snapshots are using them.

Use reference counting, leases, guards, or another safe ownership mechanism.

8. Make eviction thread-safe.

9. Test concurrent graph access while eviction is occurring.

10. Avoid loading all graph schemas/index metadata if unnecessary for startup.

11. Add metrics:

* registered graph count;
* open graph count;
* graph cache hits;
* graph cache misses;
* graph opens;
* graph closes;
* eviction count;
* average cold-open latency.

12. Test increasingly large graph populations:

* 10,000;
* 50,000;
* 100,000.

### Deliverables

Produce:

* lightweight graph catalog;
* lazy graph opening;
* safe graph closing;
* configurable active-graph cache;
* eviction policy;
* concurrency-safe graph leases;
* lifecycle metrics;
* 100K graph benchmark;
* migration documentation for any behavior change.

### Success criteria

This phase is complete only when:

* 100,000 logical graphs can be registered;
* server startup does not physically open every graph;
* physical open-graph count respects configured limits;
* file descriptor usage remains bounded;
* idle memory use remains bounded relative to configured open graph count;
* cold graphs open successfully on first access;
* active transactions are never interrupted by eviction;
* concurrent access and eviction tests pass;
* server restart with 100,000 registered graphs succeeds;
* randomly selected graphs remain readable/writable after restart;
* graph catalog metadata is not corrupted under concurrent operations;
* startup time is materially less dependent on total graph count than before the refactor.

---

# Phase 3 — Production-Grade Replication and High Availability

### Project demand

TuGraph already contains Raft/HA functionality.

The goal of this phase is not to invent a new consensus system.

The goal is to turn the existing HA implementation into a well-tested, observable, production-capable three-node replica group.

The intended topology is:

leader + two followers.

Writes acknowledged to clients must survive leader failure according to clearly documented durability semantics.

### Methodology

1. Fully trace the existing HA implementation:

   * `HaStateMachine`;
   * braft integration;
   * leader election;
   * log replication;
   * snapshots;
   * follower catch-up;
   * node membership;
   * request forwarding;
   * Bolt/OpenCypher paths;
   * import paths;
   * schema modifications;
   * plugin/procedure operations.

2. Define durability semantics.

Example desired guarantee:

A write is acknowledged only after reaching the required Raft commit condition.

3. Build an automated three-node test cluster.

4. Test:

   * leader election;
   * follower restart;
   * leader restart;
   * follower lag;
   * snapshot installation;
   * node rejoin;
   * network partition;
   * temporary packet loss;
   * process crash;
   * machine-level kill;
   * disk-full behavior where feasible.

5. Verify every write path is replicated consistently.

Include:

* Cypher/Bolt writes;
* schema modifications;
* graph creation/deletion where applicable;
* bulk import;
* ACL changes;
* stored procedure metadata if relevant.

6. Add cluster observability:

   * leader identity;
   * term;
   * commit index;
   * applied index;
   * replication lag;
   * node health;
   * snapshot progress;
   * election count.

7. Implement automated chaos testing.

Repeatedly:

* run write/read workload;
* terminate random nodes;
* restart nodes;
* partition communication;
* restore communication;
* validate database state.

8. Test recovery against expected state using checksums or deterministic workload validation.

### Deliverables

Produce:

* reproducible 3-node HA deployment;
* HA test suite;
* failure-injection suite;
* replication metrics;
* documented consistency/durability guarantees;
* fixed replication gaps;
* operational runbook;
* recovery documentation.

### Success criteria

This phase is complete only when:

* a 3-node cluster starts reliably;
* exactly one leader accepts writes;
* followers replicate committed changes;
* killing the leader results in automatic election of a new leader;
* acknowledged committed writes remain available after leader loss;
* a restarted node automatically catches up;
* a stale node can recover from snapshot;
* tested network partitions do not result in two independently writable leaders;
* all major write paths are verified under replication;
* 24–72 hour chaos tests run without acknowledged-write loss or data divergence;
* metrics allow operators to detect unhealthy replication.

---

# Phase 4 — Whole-Graph Horizontal Sharding

### Project demand

Introduce horizontal sharding at the graph level.

A graph is the atomic placement unit.

Each logical graph must belong to exactly one shard at a time.

A shard is itself a replicated TuGraph deployment from Phase 3.

Queries must never need to execute across multiple shards during this phase.

The client should address a graph by logical name without knowing its physical shard.

### Methodology

1. Introduce a cluster metadata model.

At minimum store:

* graph name or graph ID;
* shard ID;
* replica group;
* placement version;
* graph state.

2. Introduce a query/router service or routing layer.

For each request:

* identify target graph;
* resolve graph-to-shard mapping;
* identify current shard leader if necessary;
* route request.

3. Design metadata consistency carefully.

Graph placement changes must be versioned and atomic.

4. Define shard abstraction.

Example:

* shard-001 = replica group A;
* shard-002 = replica group B;
* shard-003 = replica group C.

5. Add cluster management operations:

   * register shard;
   * remove shard;
   * create graph;
   * assign graph;
   * list graphs;
   * inspect graph placement;
   * inspect shard health.

6. Implement placement strategy.

Initial options may include:

* round-robin;
* least graph count;
* least disk utilization;
* weighted capacity.

7. Make routing transparent to application clients.

8. Cache routing metadata, but use versioning so stale routers cannot silently route writes to obsolete locations.

9. Add router HA or make routers stateless enough to scale horizontally.

10. Build integration tests across at least three shards.

### Deliverables

Produce:

* graph-to-shard catalog;
* routing service;
* shard registration;
* graph placement logic;
* cluster administration API;
* routing cache/version model;
* multi-shard deployment tooling;
* observability for placement and routing.

### Success criteria

This phase is complete only when:

* at least 3 independent shards operate simultaneously;
* each shard is a replicated group;
* at least 30,000 graphs can be distributed across the cluster;
* clients access graphs by logical graph identifier;
* clients do not need shard addresses;
* reads and writes are routed to the correct shard;
* graph creation automatically chooses a shard;
* placement metadata survives restart;
* stale routing metadata is detected safely;
* failure of a router does not affect persistent graph state;
* shard-level failover remains functional;
* no cross-shard query execution is required.

---

# Phase 5 — Online Graph Migration and Automatic Rebalancing

### Project demand

Add the ability to move a complete graph from one shard to another with minimal service interruption.

This enables:

* adding capacity;
* draining servers;
* balancing disk usage;
* balancing graph count;
* evacuating unhealthy hardware.

Migration must preserve acknowledged writes and prevent divergent writable copies.

### Methodology

1. Define a migration state machine.

Suggested states:

* scheduled;
* snapshotting;
* copying;
* catching-up;
* preparing-cutover;
* cutover;
* validating;
* cleanup;
* completed;
* failed;
* rollback.

2. Implement migration using a safe sequence such as:

* allocate destination;
* snapshot source graph;
* transfer snapshot;
* restore destination;
* stream subsequent changes;
* catch destination up;
* briefly quiesce writes if necessary;
* perform final synchronization;
* atomically update placement metadata;
* route new requests to destination;
* validate destination;
* retain source temporarily;
* delete old copy after safety window.

3. Guarantee that only one location is writable after cutover.

4. Make migration restartable and idempotent.

5. Support interruption during:

   * snapshot copy;
   * log catch-up;
   * cutover;
   * cleanup.

6. Implement manual controls:

   * `moveGraph`;
   * `cancelMigration`;
   * `retryMigration`;
   * `drainShard`.

7. Implement basic automatic balancing using:

   * disk utilization;
   * graph count;
   * configured shard weights;
   * optional workload information.

8. Add migration metrics:

   * bytes transferred;
   * progress;
   * lag;
   * downtime;
   * failures;
   * retries.

9. Test large graphs and small graphs.

10. Test migration while clients continuously read and write.

### Deliverables

Produce:

* migration state machine;
* graph snapshot/copy process;
* incremental synchronization;
* atomic placement cutover;
* rollback/retry;
* shard draining;
* automatic rebalancer;
* operational APIs;
* migration metrics.

### Success criteria

This phase is complete only when:

* an active graph can move from shard A to shard B;
* acknowledged writes are not lost;
* clients automatically begin using the destination after cutover;
* source and destination cannot independently accept writes after cutover;
* migration can recover from process crashes;
* interrupted transfers can resume or restart safely;
* failed cutovers can be rolled back where still safe;
* a shard can be drained of graphs;
* the rebalancer can reduce intentionally created shard imbalance;
* source data is removed only after destination validation;
* migration history is auditable.

---

# Phase 6 — Production Hardening and Version 1.0

### Project demand

Turn the horizontally scalable graph platform from Phases 0–5 into an operationally supportable production system.

The focus is reliability, observability, backup, disaster recovery, security, upgrades, quotas, and documented service-level objectives.

No major new storage architecture should be introduced during this phase.

### Methodology

1. Define production SLOs.

At minimum consider:

* service availability;
* graph availability;
* failover time;
* RPO;
* RTO;
* routing latency;
* migration interruption window.

2. Implement cluster-wide metrics and dashboards covering:

   * routers;
   * shards;
   * replicas;
   * replication lag;
   * disk usage;
   * graph count;
   * active/open graph count;
   * graph placement;
   * migration progress;
   * request latency;
   * request error rate.

3. Implement alerting recommendations.

4. Build backup strategy covering:

   * individual graph;
   * shard;
   * cluster metadata;
   * point-in-time requirements if supported.

5. Test restores regularly.

6. Implement rolling upgrade procedure.

7. Define version compatibility rules between:

   * router;
   * metadata/control plane;
   * shard nodes.

8. Implement quotas if required:

   * graph count per tenant;
   * disk usage;
   * query concurrency;
   * connection count;
   * optional CPU/memory limits.

9. Harden authentication and authorization around cluster-level operations.

10. Test failure scenarios:

* entire shard unavailable;
* router loss;
* metadata service interruption;
* network partition;
* disk full;
* corrupted replica;
* interrupted migration;
* rolling upgrade failure.

11. Create production runbooks for:

* failover;
* recovery;
* replacement of replica;
* replacement of shard;
* backup restore;
* migration troubleshooting;
* capacity expansion.

12. Run long-duration workload and chaos tests.

### Deliverables

Produce:

* production deployment architecture;
* monitoring dashboards;
* alerts;
* backup/restore system;
* rolling upgrade procedure;
* cluster security controls;
* operational runbooks;
* capacity planning documentation;
* chaos/fault test results;
* defined SLO/SLA assumptions.

### Success criteria

This phase is complete only when:

* production SLOs are explicitly documented;
* critical metrics are observable;
* alerts identify replica/shard/router failures;
* backups are automatically generated;
* restore procedures have been successfully tested;
* a cluster can be upgraded without full-cluster downtime;
* failed nodes can be replaced using documented procedures;
* graph migrations survive expected operational failures;
* cluster metadata can be recovered;
* capacity can be increased by adding new shards;
* security controls prevent unauthorized cluster-level operations;
* prolonged load/chaos testing demonstrates stable operation;
* the system is judged ready for a production 1.0 deployment.

---

# Phase 7 — Optional Intra-Graph Sharding

### Project demand

Implement distributed storage for a single logical graph whose data exceeds the capacity or performance envelope of one shard.

This phase is fundamentally different from whole-graph sharding.

Vertices, edges, indexes, and queries may span multiple machines.

Treat this as a separate distributed-database program.

Do not compromise the working whole-graph architecture from Phases 0–6.

### Methodology

Implement this phase incrementally.

#### Stage 7A — Static partitioning

Create a deterministic mapping:

`vertex -> partition`

Initially use a simple partitioning mechanism such as:

* hash partitioning;
* range partitioning.

Define edge ownership rules.

Document where:

* source vertex;
* destination vertex;
* outgoing adjacency;
* incoming adjacency;
* edge properties

are stored.

#### Stage 7B — Cross-shard reads

Allow direct access to remote vertices and edges.

Introduce location metadata and remote fetch mechanisms.

#### Stage 7C — Distributed traversal

Extend query execution so graph traversals can continue across partition boundaries.

Implement:

* distributed execution context;
* remote expansion;
* result aggregation;
* cancellation;
* timeout handling;
* backpressure.

#### Stage 7D — Cross-shard writes

Support mutations involving vertices and edges located on different partitions.

Clearly define failure behavior.

#### Stage 7E — Distributed transactions

Implement transaction coordination for operations touching multiple shards.

Determine consistency requirements.

Possible implementation may require:

* transaction coordinator;
* prepare/commit protocol;
* recovery logs;
* transaction timeout;
* participant recovery;
* deadlock strategy.

#### Stage 7F — Online repartitioning

Support moving partitions or ranges while the graph remains online.

#### Stage 7G — Automatic balancing

Build automated placement and balancing based on:

* storage;
* traffic;
* graph topology;
* hotspot detection.

5. Preserve query semantics wherever practical.

6. Develop extensive correctness tests before optimizing performance.

7. Benchmark cross-shard traversal separately from local traversal.

8. Make local-shard operations remain efficient.

### Deliverables

Produce:

* graph partitioning model;
* edge ownership model;
* partition metadata;
* distributed read execution;
* distributed traversal;
* distributed writes;
* distributed transactions;
* repartitioning;
* automatic balancing;
* distributed query metrics;
* failure recovery mechanisms;
* architecture and consistency documentation.

### Success criteria

Phase 7 should be considered complete only when:

* one logical graph can be stored across at least 3 storage shards;
* vertex lookup works regardless of physical location;
* cross-shard edges are supported correctly;
* multi-hop graph traversal can cross shard boundaries;
* query results match equivalent single-node execution;
* cross-shard writes survive node failures according to documented semantics;
* distributed transactions recover from coordinator or participant crashes;
* partitions can be moved online;
* shard imbalance can be corrected automatically;
* distributed execution exposes sufficient metrics for debugging;
* failure tests demonstrate no silent divergence;
* performance characteristics and limitations are documented.

The project should preserve a fast path for queries that remain within a single partition.

Phase 7 should not be considered successful merely because data can be split across servers. It is successful only when distributed query execution, mutation semantics, recovery, and operational behavior are reliable enough for production use.

