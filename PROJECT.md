    Branch timeseries — Time-Series Properties in TuGraph

    New clone, new branch timeseries, based on performance @ 488ad42cc
    (not including the uncommitted Phase 3 HA work).

    Provenance correction (2026-09-21 review): the named base 488ad42cc is not
    present in this clone; the local merge base with performance is 384dc7da.
    No HA dependency was found; the discrepancy is record-keeping, not a
    branch-management failure.

    This feature is developed in parallel with the other work streams (scaling, HA), not
    sequenced after them. The design below is chosen purely on engineering merit — where a
    change belongs in a hot file (transaction.cpp, lightning_graph.cpp, schema.cpp,
    procedure.cpp, graph_manager.cpp), it goes there. Overlapping edits are resolved by
    rebasing and merging; they are not a design constraint.

    Constraints confirmed by reading the code

    ┌──────────────────────────────────────────────┬──────────────────────────────────────────────────────────┬────────────────────────────────────────────────┐
    │                     Fact                     │                          Source                          │                  Consequence                   │
    ├──────────────────────────────────────────────┼──────────────────────────────────────────────────────────┼────────────────────────────────────────────────┤
    │ Build uses -Wall -Werror                     │ Options.cmake:9                                          │ Adding a FieldType enumerator turns every      │
    │                                              │                                                          │ exhaustive switch without default: into a      │
    │                                              │                                                          │ compile error                                  │
    ├──────────────────────────────────────────────┼──────────────────────────────────────────────────────────┼────────────────────────────────────────────────┤
    │ IsBufType() is a range test >= STRING && <   │ include/lgraph/lgraph_types.h:1305-1307                  │ A new enumerator as 17 is not memory-managed;  │
    │ FLOAT_VECTOR                                 │                                                          │ dtor :520-523 would leak it                    │
    ├──────────────────────────────────────────────┼──────────────────────────────────────────────────────────┼────────────────────────────────────────────────┤
    │ ~731 case FieldType::… labels tree-wide; 124 │ see include/lgraph/lgraph_types.h,                       │ New type = sweeping change across every        │
    │ alone in the public header                   │ src/core/field_data_helper.h                             │ surface                                        │
    ├──────────────────────────────────────────────┼──────────────────────────────────────────────────────────┼────────────────────────────────────────────────┤
    │ MAX_KEY_SIZE 480 B, MAX_PROP_SIZE 16 MiB     │ src/core/data_type.h:308,312                             │ Series data must never pass through indexed    │
    │                                              │                                                          │ keys or normal property paths                  │
    ├──────────────────────────────────────────────┼──────────────────────────────────────────────────────────┼────────────────────────────────────────────────┤
    │ mdb_env_set_maxdbs(10000); only 2            │ src/core/lmdb_store.cpp:67, src/core/lmdb_table.cpp:53   │ One DBI, not one per series field, avoids DBI  │
    │ mdb_dbi_open sites                           │                                                          │ exhaustion                                     │
    ├──────────────────────────────────────────────┼──────────────────────────────────────────────────────────┼────────────────────────────────────────────────┤
    │ BlobManager already does handle-in-record +  │ src/core/blob_manager.h:91-137                           │ Proves the out-of-record-table pattern we are  │
    │ spill                                        │                                                          │ copying                                        │
    ├──────────────────────────────────────────────┼──────────────────────────────────────────────────────────┼────────────────────────────────────────────────┤
    │ Fulltext index buffers writes and flushes    │ src/core/transaction.cpp:362-393                         │ Anti-pattern: our writes must be atomic with   │
    │ after commit; Abort() just discards          │                                                          │ the KV txn                                     │
    ├──────────────────────────────────────────────┼──────────────────────────────────────────────────────────┼────────────────────────────────────────────────┤
    │ Dotted function names already parse, no      │ src/cypher/grammar/Lcypher.g4:357,371; flattened at      │ series.range(...) works today                  │
    │ grammar change                               │ cypher_base_visitor_v2.cpp:1786-1796                     │                                                │
    ├──────────────────────────────────────────────┼──────────────────────────────────────────────────────────┼────────────────────────────────────────────────┤
    │ Per-graph auxiliary-structure inventory      │ src/core/lightning_graph.cpp:3026-3097                   │ Single place to open the series table          │
    └──────────────────────────────────────────────┴──────────────────────────────────────────────────────────┴────────────────────────────────────────────────┘

    ---
    Parallel development model

    - Branch timeseries is cut from performance @ 488ad42cc and rebased onto performance
    regularly so the other streams' changes are absorbed continuously rather than in one
    big merge.
    - Work is organized so each stage lands as a coherent commit touching a predictable set of
    files; stages are ordered so that new-file work (S0, S1) lands first and the deeper edits
    to shared files (S2–S4) come later, which keeps rebases cheap early on even though we are
    not avoiding those files.
    - Where this plan edits a file another stream is also touching
    (src/core/transaction.cpp, src/core/lightning_graph.cpp, src/core/schema.cpp,
    src/cypher/procedure/procedure.cpp, src/restful/server/rest_server.cpp), the edits are
    small and localized to one function or one switch, and never reformat surrounding code —
    that keeps any conflict trivially mechanical to resolve.
    - No dependence on the uncommitted Phase 3 HA work: everything here builds and passes on
    488ad42cc alone.

    Decision 1 — do NOT add a new FieldType (engineering cost, not conflict avoidance)

    A series field is declared as BLOB type + a new series modifier on FieldSpec
    (not a new enum value). Consequences:

    - No record-layout change, no RefreshLayout() semantics change
    (src/core/schema.cpp:955-1029), no new FieldTypeNames/Sizes/IsFixedLength entries,
    no dtor/leak trap, no -Werror switch cascade.
    - The record physically stores nothing for a series field — it is always the null bit.
    All layout, null-handling, import-type and export machinery already understands
    "optional BLOB" and need no change.
    - Series data lives in one dedicated KV table, addressed by (element key, field_id, time),
    so no handle needs to be materialised into the record at all.

    This is a judgement about cost and risk, not about avoiding shared files — with parallel
    development we are free to touch the type system if it earns its keep. It does not, yet: the
    change buys nicer syntax (n.prices returning a real series value, functions taking a value
    instead of an element + name) at the price of ~40-60 files, both record extractors, four
    serialization switches and the memory-management traps listed above. Ship the store first,
    promote it to a first-class type only once the semantics are settled — see S6 below,
    which is now explicitly in scope as a follow-on rather than excluded.

    Cost of this choice: RETURN n.prices cannot print a whole series (there are no bytes in
    the record). It returns a cheap summary map instead; real access goes through the
    series.* function family.

    Decision 2 — return Cypher-native types, so serialization is free

    Every read function returns LIST<MAP<STRING, scalar>> (and scalars: INT64/DOUBLE/
    DATETIME). The Cypher value model already has MAP/ARRAY with existing JSON, Bolt and
    Python conversions, so REST, Bolt and the Python client need no changes — only much
    smaller decisions around the summary map. This is the single biggest reason to avoid a new
    field type: it collapses surface (c) of the requested scope to near zero work.

    Correction: serialization was not quite free. The result model stored
    LIST/MAP columns as JSON but flattened them with ToString() on insertion,
    so one shared result-path change was needed (Record::Insert for
    vector/map json, CollectAggCtx, empty-container nulls). As part of that,
    scalar conversion in the recursive path is now type-preserving: a STRING
    that looks like JSON stays a string. Functions whose contract is a
    container but whose implementation produces text parse it themselves -
    properties() now returns real MAPs (unparseable dumps, e.g. bare DATETIME
    or NUL values, stay strings as before). Map member access returns the whole
    member, so WITH s ... RETURN s.measures works for lists too.

    ---
    Storage layout

    One new table per graph: SERIES_TABLE = "_tseries_" added to src/core/defs.h alongside
    src/core/defs.h:87-110, opened as a normal DBI inside the existing per-graph env at
    src/core/lightning_graph.cpp:3026-3097 (mirroring BlobManager::OpenTable,
    src/core/blob_manager.h:30-36) with the default ComparatorDesc::BYTE_SEQ
    (src/core/kv_table_comparators.h:25).

    Because LMDB values carry [8-byte txn-id][payload] automatically
    (src/core/lmdb_table.cpp:162-168), nothing extra is needed for MVCC.

    Key — big-endian so BYTE_SEQ ordering is correct, with the sign bit
    flipped so byte order agrees with numeric order over the whole int64
    range (plain big-endian two's complement sorts every negative value after
    every positive one and would break pre-1970 scans and lookups).

    vertex:  [0x00][vid:5B][field_id:2B][bucket_start_i64:8B]   = 16 B
    edge:    [0x01][euid:24B][field_id:2B][bucket_start_i64:8B] = 35 B
             (EUID = vid5 + lid2 + tid8 + vid5 + eid4, data_type.h:299-300)

    All buckets for one (element, field) are contiguous and ordered by bucket_start, so a
    time range is one lower-bound seek plus a short forward scan. Both keys are far under the
    480 B MAX_KEY_SIZE.

    Value — one bucket

    Header (little-endian)
      magic        u32   "TSB1"
      version      u16   1
      flags        u16   bit0 = any nulls present
      count        u32   points in this bucket  (<= bucket_max_points, default 1000)
      n_measures   u16
      measure_ids  u16[n_measures]   // index into FieldSpec::series.measures
      first_ts     i64   exact µs timestamp of point[0]  (same domain as DATETIME)

    Column blocks, in order:
      TS column:      u32 byte_len | delta-of-delta varint bitstream
      per measure m:  u32 byte_len | u8 encoding_code | ceil(count/8) null bitmap | payload
                      encoding_code: 0 = RAW64, 1 = GORILLA_XOR, 2 = ZIGZAG_VARINT64

    - Timestamps — delta-of-delta with the 4-way control-bit scheme
    (0→14 bits, 10→7, 110→9, 1110→12, 1111→32 bits), zigzag encoded, bit-packed.
    The widest escape is 64 bits, not 32: timestamps are microseconds, so a
    one-day gap (8.64e10) does not fit in 32 bits. Constant cadence still costs
    ~1 bit/point.
    - DOUBLE measures — Gorilla XOR against the previous value: equal ⇒ single 0 bit;
    otherwise control 1 + 6-bit leading-zero count + 6-bit meaningful-bit length +
    trailing-zero-trimmed payload. The leading-zero count is 6 bits, not 5: it can
    reach 63 for two doubles in the same binade. Good fit for slowly-moving prices.
    - INT64 measures (volume, share counts) — zigzag delta varint.
    - Nulls — one bitmap per measure; histocial gaps (a suspended trading day) cost 1 bit.

    Bucket policy

    - Close a bucket on point count (series_bucket_max_points, default 1000).
    - Optional span cap series_bucket_max_span_us, default 0 = disabled. Rationale: with
    daily bars a wall-clock cap would produce one-point buckets; with intraday ticks the
    count cap alone already bounds the rewrite cost.
    - Hard cap series_bucket_max_bytes (default 1 MiB) with a defensive check — bounds the
    copy-on-write rewrite cost and stays far under MAX_PROP_SIZE.
    - Defaulted (FieldSpec::set_default_value) and the schema version byte:
    SCHEMA_VERSION at src/core/data_type.h:314 governs the extension read below.

    ---
    Schema / DDL

    FieldSpec (include/lgraph/lgraph_types.h:1331+) gains:

    struct SeriesMeasureSpec { std::string name; FieldType type; /* DOUBLE | INT64 */ };
    struct SeriesSpec {
        std::vector<SeriesMeasureSpec> measures;
        uint32_t bucket_max_points = 1000;
        uint64_t bucket_max_span_us = 0;
        uint32_t bucket_max_bytes = 1 << 20;
    };
    bool series = false;          // FieldSpec gains this modifier
    SeriesSpec series_spec;       // valid only when series == true

    Serialization must stay backward compatible. Schema::StoreSchema()/LoadSchema()
    (src/core/schema.cpp, impl src/core/schema.h:715-760) currently write a fixed sequence
    per FieldSpec. Write the series block trailing at the end of StoreSchema and read it
    in LoadSchema only if bytes remain — so old databases still load, and new ones round-trip.

    Validation: a series field must be optional, BLOB, and may not be the primary field or
    be indexed. Enforce in Schema construction/RefreshLayout().
    Measure names must be unique, non-empty, DOUBLE or INT64 — and may not be
    `ts`, which keys the timestamp in every returned point map.

    Identity rules (added after the P1-1 finding: bucket keys use the record
    field id, but packed-layout ids are positional, so deleting a field
    compacts every id behind it):
    - AlterLabelDelFields drops the buckets of deleted series fields and
    re-keys (SeriesStore::MigrateField) the buckets of surviving series fields
    whose id compacts, all in the same transaction as the schema change.
    Fast-alter ids are stable, so that path only needs the drop.
    - AlterLabelModFields rejects measure-set and series-flag changes on a
    series field (buckets decode with the schema's measures); bucket-cap
    retuning stays allowed.
    - DropAllVertex clears the whole series table; DelLabel drops the label's
    vertex and incident-edge buckets while its elements are still enumerable,
    in the same transaction. Transaction::DeleteVertex cleans incident-edge
    buckets in its callback (the direct DeleteEdge path already did).
    - A failed bucket split encodes all replacements before writing anything,
    so a false return leaves the transaction unchanged.

    Declaration is available two ways, both landing in src/cypher/procedure/procedure.cpp.
    Shipped in S3 is the standalone form (it covers every label, old and new,
    so the inline sugar is deferred):

    CALL db.createSeriesField('Company', 'prices', [{name:'open',...}], {bucket_max_points:1000})
    CALL db.createEdgeSeriesField(<same>)      // same table, key kind 0x01
    CALL db.dropSeriesField('vertex', 'Company', 'prices')   // drops schema entry AND all buckets

    Deferred: inline in the existing label DDL — db.createVertexLabel /
    db.createEdgeLabel gaining an optional trailing series spec, so a label
    and its series are declared in one call.

    Schema metadata is surfaced in the REST list-field serialization
    (src/restful/server/json_convert.h, ValueToJson over field extractors):
    every field carries "series" (bool), and series fields additionally carry
    "measures" ([{name, type}]). Additive keys; old responses are unchanged.

    ---
    Read path — Cypher function family

    Registered in ArithOpNode::RegisterFuncs() (src/cypher/arithmetic/arithmetic_expression.h:556-629)
    with BuiltinFunction declarations at :170+ and implementations in
    src/cypher/arithmetic/arithmetic_expression.cpp. No grammar change is required.

    ┌──────────────────────────────────────────────────────┬─────────────┬────────────────────────────────────────┐
    │                       Function                       │   Returns   │                 Notes                  │
    ├──────────────────────────────────────────────────────┼─────────────┼────────────────────────────────────────┤
    │ series.range(e, 'f', t0, t1)                         │ LIST<MAP>   │ canonical access; points ordered by ts │
    ├──────────────────────────────────────────────────────┼─────────────┼────────────────────────────────────────┤
    │ series.at(e, 'f', t)                                 │ MAP or null │ exact timestamp or null                │
    ├──────────────────────────────────────────────────────┼─────────────┼────────────────────────────────────────┤
    │ series.latest(e, 'f') / series.earliest(e, 'f')      │ MAP         │ reads only one bucket end              │
    ├──────────────────────────────────────────────────────┼─────────────┼────────────────────────────────────────┤
    │ series.count(e, 'f'[, t0, t1])                       │ INT64       │ header-only where possible             │
    ├──────────────────────────────────────────────────────┼─────────────┼────────────────────────────────────────┤
    │ series.mean/min/max/sum(e, 'f', 'measure'[, t0, t1]) │ scalar      │ window aggregate over one series       │
    └──────────────────────────────────────────────────────┴─────────────┴────────────────────────────────────────┘

    The functions take the graph element and the field name, not the property value —
    this is what lets us avoid a FieldType (and it is why they stay lazy).

    RETURN n.prices (i.e. Entry::GetEntityField, src/cypher/resultset/record.cpp:22-61)
    returns a summary map computed by ProbeVertexSeries (count over the range,
    earliest, latest):

    {count: 1234, first: datetime(...), last: datetime(...),
     measures: ['open','high','low','close','volume']}

    It never materialises the series, but it does walk the history: the
    bucket headers carry each bucket's own count and first timestamp, not a
    series-wide total or last timestamp, so no header-only summary exists
    without new transactional metadata (deferred; see S5). The actual
    complexity contract, enforced by an operation-count test:
    - series.at/latest/earliest and narrow range/count decode O(1) buckets;
      field validation and measure naming read schema only, never buckets.
    - The summary is the documented exception: count/first/last cost work
      proportional to the series.
    - Aggregates walk the selected points client-side (no store aggregate
    entry point yet); each aggregate accumulates only what it needs, so
    min/max never touch a sum. Integer sums are overflow-checked and refuse
    rather than wrap; integer means accumulate in double; double sums keep
    IEEE semantics.

    Writing to a series field through ordinary SET raises a
    clear error pointing at series.append / series.update.

    Verify early: Lcypher.g4 accepts dotted names, but the vendored GEAX path is
    functionName : identifier (deps/geax-front-end/.../GqlParser.g4:646-648) and may not.
    Test series.range through the GQL parser in Stage 2; if it does not lex, ship both
    series.range and an underscore alias series_range.

    Verified: the dotted form does not lex on the GQL path (pinned syntax
    error in the GQL golden), so every read function and every write
    procedure is registered twice (series.range + series_range, series.append
    + series_append, ...). Two further GQL limits are pinned, not fixed: map
    literals arrive as an unsupported MkRecord node, and CREATE has no viable
    alternative — so point writes stay Cypher-only and the GQL S3 suite covers
    scalar-argument DDL (dropSeriesField and its validation errors).

    Write path

    All writes go through ordinary KV operations inside the existing transaction, so they are
    atomic, WAL-logged and MVCC-correct — deliberately not the fulltext post-commit pattern.

    SeriesStore::Upsert(txn, elem_key, field_id, ts, values[], op):

    1. Seek the last key with prefix (elem, field) whose bucket_start <= ts
    (lower-bound seek + one step back).
    2. None ⇒ create a bucket starting at ts.
    3. Decode the bucket's ts column, binary-search ts:
      - found ⇒ overwrite the measure values for that row (idempotent correction/restatement);
      - not found ⇒ insert in order; if the bucket is full, split into two buckets
    (re-encode both, new bucket keyed by the midpoint timestamp).
    4. Re-encode the whole bucket and SetValue.

    Whole-bucket re-encode is fine because buckets are ≤1000 points; it is what makes in-place
    correction possible with variable-length encodings.

    Hook points in src/core/transaction.cpp (mirroring existing index maintenance in
    AddVertex :1345-1371):

    ┌─────────────────────┬─────────────────────┬─────────────────────────────────────────────────────────────────────────────────────┐
    │      Operation      │        Site         │                                   Required action                                   │
    ├─────────────────────┼─────────────────────┼─────────────────────────────────────────────────────────────────────────────────────┤
    │ AddVertex / AddEdge │ :1345, :1412        │ none (no bytes in record)                                                           │
    ├─────────────────────┼─────────────────────┼─────────────────────────────────────────────────────────────────────────────────────┤
    │ SetVertexProperty   │ :1033-1036          │ if field is series ⇒ clear InputError naming series.append / series.update          │
    │ SetEdgeProperty   │ analogous           │ (a plain SET carries no timestamp or per-measure values, so there is              │
    │                     │                     │ nothing to funnel; points go through the series procedures only)                   │
    ├─────────────────────┼─────────────────────┼─────────────────────────────────────────────────────────────────────────────────────┤
    │ DeleteVertex        │ :407-497            │ delete all buckets with the element prefix before removing the record,              │
    │                     │                     │ plus incident-edge buckets in the callback (DeleteEdge alone is not enough)         │
    ├─────────────────────┼─────────────────────┼─────────────────────────────────────────────────────────────────────────────────────┤
    │ DeleteEdge          │ :528+               │ same                                                                                │
    ├─────────────────────┼─────────────────────┼─────────────────────────────────────────────────────────────────────────────────────┤
    │ DropAllVertex /     │ lightning_graph.cpp │ clear the series table / the label's element prefixes in the same                 │
    │ DelLabel            │                     │ transaction (bucket keys carry no label)                                            │
    ├─────────────────────┼─────────────────────┼─────────────────────────────────────────────────────────────────────────────────────┤
    │ rollback            │ existing KV Abort() │ nothing added — atomicity is inherited                                              │
    └─────────────────────┴─────────────────────┴─────────────────────────────────────────────────────────────────────────────────────┘

    Cypher write surface (all as-built; see procedure.cpp):

    CALL db.createSeriesField('Company', 'prices',
         [{name:'open',type:'DOUBLE'},...,{name:'volume',type:'INT64'}],
         {bucket_max_points:1000}) YIELD field RETURN field;
    CALL db.createEdgeSeriesField(<same for edges>)
    CALL db.dropSeriesField('vertex'|'edge', 'Company', 'prices')   // drops schema entry AND all buckets;
         // refuses ordinary fields
    MATCH (n:Company {id:1})
    CALL series.append(n, 'prices', {ts: datetime('2024-01-02'), open:1.0, ..., volume:100})
         YIELD written RETURN written;   // missing measures become null, unknown keys are rejected
    CALL series.update(n, 'prices', 'close', datetime('2024-01-02'), 1.5)   // single-measure upsert;
         // reads the row first so the other measures survive
    CALL series.clear(n, 'prices') YIELD cleared RETURN cleared;   // yields the removed point count

    Implemented as procedures reusing the same SeriesStore entry points; the
    write procedures run in the query's own transaction (read_only=false,
    separate_txn=false), so a MATCH+CALL writes per matched row and commits
    atomically with the query, while DDL stays standalone (separate_txn=true)
    through the access-controlled DB, preserving auth.

    Naming correction: the plan's series.set is series.update — CALL
    series.set does not parse (SET is a Cypher keyword). series_set is kept as
    an alias, as are series_append/series_clear (and every dotted read name)
    for the GQL path, whose lexer has no dot in identifiers.

    Concurrency: two writers touching the same bucket conflict at LMDB level and one aborts
    into the existing optimistic retry loop. Since daily/quarterly append workloads are
    single-writer per series, this is acceptable; measure the abort rate in Stage 5 and, if
    needed, revisit with a tail-append fast path.

    Backup / snapshot / HA — nearly free, because it is one DBI in data.mdb

    - LMDBKvStore::Backup uses mdb_env_copy2 (src/core/lmdb_store.cpp:211-222) ⇒ automatic.
    - LightningGraph::Snapshot (:2979-2990) copies data.mdb ⇒ automatic
    (and unlike the fulltext directory, nothing is missed).
    - WAL records KvPut/KvDel generically (src/core/wal.h:71-86) ⇒ durability is automatic.
    - HA: both the legacy braft path (HaStateMachine::ReplicateAndApplyRequest,
    src/server/ha_state_machine.cpp:224-245) and Bolt HA (re-executes Cypher text,
    src/server/bolt_handler.cpp:115-163) replicate requests, so each replica writes the
    same series deterministically. No new replication plumbing.
    - Only code change needed: open the DBI in the per-graph inventory
    (src/core/lightning_graph.cpp:3026-3097) so old databases gain it on first open.

    Surface checklist

    ┌────────────────┬───────────────────────────────────────────────────────────────────────────────────────────────┬─────────────────────────────────────────┐
    │    Surface     │                                             Files                                             │                  Work                   │
    ├────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────┼─────────────────────────────────────────┤
    │ Cypher read    │ src/cypher/arithmetic/arithmetic_expression.{h,cpp}                                           │ new registrations + impls               │
    ├────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────┼─────────────────────────────────────────┤
    │ Cypher write / │ src/cypher/procedure/procedure.cpp; REST schema listing in                                    │ extend existing label DDL + new         │
    │ DDL            │ src/restful/server/rest_server.cpp:848,866 + src/restful/server/json_convert.h:551-556,878    │ procedures + schema metadata in REST    │
    ├────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────┼─────────────────────────────────────────┤
    │ Schema         │ src/core/schema.cpp, src/core/schema.h:715-760                                                │ FieldSpec.series + trailing-extension   │
    │ persistence    │                                                                                               │ serialization                           │
    ├────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────┼─────────────────────────────────────────┤
    │ REST           │ src/restful/server/json_convert.h:383-468                                                     │ summary-map case only                   │
    ├────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────┼─────────────────────────────────────────┤
    │ Server result  │ src/server/json_convert.h:25-157                                                              │ summary-map case only                   │
    │ JSON           │                                                                                               │                                         │
    ├────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────┼─────────────────────────────────────────┤
    │ Bolt           │ src/lgraph_api/lgraph_types.cpp:20-58                                                         │ none — we return LIST/MAP/scalars       │
    ├────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────┼─────────────────────────────────────────┤
    │ Python         │ src/python/python_api.cpp:90-119                                                              │ none for query results; summary map     │
    │                │                                                                                               │ only                                    │
    ├────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────┼─────────────────────────────────────────┤
    │ Bulk import    │ src/import/import_config_parser.h:35-124, src/import/column_parser.h:513-625,                 │ new SERIES keyword + row-ingestion path │
    │                │ src/import/import_v2.cpp, import_v3.cpp                                                       │                                         │
    └────────────────┴───────────────────────────────────────────────────────────────────────────────────────────────┴─────────────────────────────────────────┘

    Bulk import: add a SERIES column kind to the import config so a long CSV of
    (company_pk, ts, open, high, low, close, volume) streams straight into buckets, bypassing
    per-point Cypher. Reuse planner utilities in src/import/import_planner.h.

    New files:
    src/core/series_encoding.{h,cpp}, src/core/series_store.{h,cpp},
    src/core/series_types.h, test/test_series_encoding.cpp, test/test_series_store.cpp,
    test/resource/cases/suite/cypher/series.{test,result},
    test/integration/test_timeseries.py.

    ---
    Milestones (status 2026-09-21: S0–S3 done and gated; S4–S5 outstanding)

    Stage: S0 Encoding primitives — DONE
    Delivers: series_encoding + fuzz/round-trip unit tests
    Done criteria: unit tests green; no DB dependency
    ────────────────────────────────────────
    Stage: S1 Core store + schema — DONE, hardened past the plan
    Delivers: series_store, _tseries_ table, FieldSpec.series, Transaction API for vertices; schema persists and reloads
    Done criteria: unit tests cover upsert/overwrite/split/range/delete-element, abort leaves no residue, data survives reopen
    Plus: atomic split (collect-then-write), field migration + bucket cleanup
    on schema edits, DropAllVertex/DelLabel/incident-edge cleanup, measure
    redefinition guard, ts reservation — all with regression tests.
    ────────────────────────────────────────
    Stage: S2 Cypher reads — DONE, hardened past the plan
    Delivers: all read functions + summary map; golden Cypher cases
    Done criteria: series.test/.result passes on both Lcypher and GQL paths (or aliased)
    Plus: type-preserving result conversion (properties() returns real MAPs),
    checked integer sums, schema-only resolution with an operation-count test,
    nested map access, YAGO_SERIES fixture isolation, nested goldens.
    ────────────────────────────────────────
    Stage: S3 Writes + edges — DONE
    Delivers: series.append/update/clear procedures, label DDL procedures, edge series (kind=0x01)
    Done criteria: round-trip through Cypher only; idempotent repeat produces identical bytes
    Evidence: series_write goldens on both parsers (declare → write →
    correct-one-measure → idempotent repeat → clear → drop → error paths,
    vertex and edge); C++ edge round-trip and byte-identical-repeat tests;
    series gate green. Two deviations: series.set shipped as series.update
    (SET is a Cypher keyword; series_set kept as alias), and only standalone
    DDL shipped (inline label-DDL sugar deferred).
    ────────────────────────────────────────
    Stage: S4 Bulk import
    Delivers: CSV/JSON series ingestion
    Done criteria: 10M-point file imports; byte-for-byte equal to equivalent Cypher ingest
    Note: the byte-for-byte criterion needs canonical bucket partitioning.
    Splits today depend on insertion order (descending backfill fragments
    into one-point buckets), so either ingest sorted batches or explicitly
    relax to logical equivalence before building S4 around repeated
    one-point upserts — build it around bucket batches instead.
    ────────────────────────────────────────
    Stage: S5 Hardening
    Delivers: concurrent writers, restart, backup/restore, integration pytest, docs
    Done criteria: integration suite green; abort rate and Δ compression ratio recorded
    Shipped early: the series acceptance gate (ci/phase0/run_series_tests.sh
    + .github/workflows/series.yml) — nonzero exit on any failure/crash/
    missing output, with commit/worktree/image/flags recorded together.
    Outstanding: server restart and durable recovery, backup/restore,
    graph eviction/reopen, 8 writers on distinct series, same-series
    contention/retries, the 500-symbol OHLCV workload, and an actual v4.5.2
    database fixture (loading a new-writer schema is not compatibility proof).
    ────────────────────────────────────────
    Stage: S6 First-class FieldType::SERIES (optional)
    Delivers: promote the BLOB+modifier field to a real type: fix IsBufType() range test (include/lgraph/lgraph_types.h:1305-1307) and the dtor (:520-523), extend
    the three constexpr arrays in src/core/field_data_helper.h:65-118, both record extractors, and the four serialization switches — so n.prices is a real value
    and series.range(n.prices, t0, t1) takes a value
    Done criteria: full unit + integration suite still green; no new -Werror switch fallout; old databases still load

    Each stage lands independently and is valuable on its own; S0–S1 are useful even if the
    Cypher surface is never finished. S6 is deferred only because it is expensive, not because
    it is blocked — it becomes attractive once the semantics have settled.

    Deferred (explicitly out of scope)

    Retention/TTL and downsampling/rollups; continuous/derived series; cross-vertex and
    cross-series aggregation (series.group_mean) — these need a plansharing aggregates
    operator; string measures; partial/regular-time alignment and gap-filling semantics;
    secondary indexing of series values (blocked by the 480 B key cap); interaction with Phase 4
    horizontal sharding (series travel with their vertex, so whole-graph migration is
    presumably safe, but this must be re-checked when Phase 4 lands).

    Not deferred: a first-class FieldType::SERIES is S6, scheduled once v1 has shipped.

    Test strategy

    - Unit: test_series_encoding.cpp (round-trip all three column encodings, randomised
    point counts, random null patterns, single-point and boundary cases);
    test_series_store.cpp (upsert, in-place correction, bucket split, out-of-order insert,
    range interpretation, element deletion cleanup, txn abort, reopen persistence,
    failed-split atomicity); test_series_transaction.cpp (vertex and edge round
    trips, sibling-series deletion, field migration, DDL guards, DropAll/DelLabel/
    incident-edge cleanup, byte-identical repeat, narrow-lookup decode counts).
    - Cypher golden: series.test + series.result and nested.test + nested.result on
    both parser paths, plus series_write goldens covering the whole S3 workflow
    through Cypher only (per-file YAGO_SERIES fixture via test/test_query.cpp).
    - Gate: ci/phase0/run_series_tests.sh allowlists the stream's suites and
    REJECTs (nonzero) on any failure, crash, or missing output; .github/
    workflows/series.yml runs it on master/performance/timeseries.
    - Integration: test/integration/test_timeseries.py — server restart retains series;
    mdb backup/restore round-trip equality; 8 concurrent writers to distinct series; one
    realistic workload (2 years daily OHLCV for 500 symbols) asserting point counts and
    compression ratio.

    Risks

    1. Parallel-stream merge discipline. We share files with other streams
    (transaction.cpp, lightning_graph.cpp, schema.cpp, procedure.cpp); conflicts are
    expected and handled by rebasing, not avoided. Keep every edit local to one function or
    one switch, never reformat surrounding code, and rebase onto performance at the start of
    each stage so merges stay mechanical. New unit tests must not depend on in-flight changes
    from other streams.
    2. No timers, no background tasks. The fork recently shipped a real use-after-free in a
    RecurringTask eviction callback (see the segfault diagnosis in phase0-logs/). Series
    code is strictly synchronous with the transaction and with graph open/close.
    3. Write amplification when correcting an old point — bounded by bucket_max_bytes.
    4. -Wall -Werror — new code must compile warning-clean. This bites hardest in S6, where
    a new enumerator makes every exhaustive switch without a default: a build failure; run
    the ASAN/test build from Options.cmake:43 before declaring that stage done.
    5. Phase 2 lazy loading/eviction — the series table opens inside the existing per-graph
    Open block, so it inherits lazy open and close; confirm no accessor outlives Close.
    6. Schema backward compatibility is the one change that touches persisted format; the
    trailing-extension read is what protects existing databases — test it against a v4.5.2 dump.


