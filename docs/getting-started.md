# Developer quick start

This builds the fork locally. Published TuGraph runtime images do not contain
GraphFin's merged changes. No public GraphFin runtime image is assumed here.

## Build once

Clone this repository with its submodules. On the documented arm64 development
host, provision the pinned compile image using
[the environment instructions](../ci/phase0/env/README.md). Other architectures
need their own compatible toolchain and evidence; an image-ID override is not
equivalent to reproducing the pinned arm64 environment.

```bash
git clone --recursive https://github.com/ziangziangziang/graphfin.git
cd graphfin
bash ci/phase0/doctor.sh
CLEAN=0 JOBS=2 BUILD_TYPE=RelWithDebInfo bash ci/phase0/build.sh
```

`CLEAN=0` retains objects. A new checkout still needs its first complete build.
Shared-header changes can rebuild many dependents. `JOBS=2` is intentional for
the ~8 GiB baseline Docker VM; do not increase it without a memory budget.

For a server-only development change, use the configured build's existing target:

```bash
bash -c 'source ci/phase0/container.sh
phase0_run_compile cmake --build /workspace/build --target lgraph_server --parallel 2'
```

Run only one build against a build directory. A gtest filter selects executed
tests; it does not create a smaller test executable. Reserve clean builds for
toolchain/configuration changes and final release qualification.

## Start a local server

The following uses the existing compile image as a development runtime and a
named volume for database files. Ports are exposed only on the host loopback.
The legacy `lgraph_*` executable names remain for compatibility.

```bash
docker run --rm --name graphfin-dev \
  -p 127.0.0.1:7070:7070 -p 127.0.0.1:9090:9090 \
  -v "$PWD:/workspace:ro" -v graphfin-dev-data:/var/lib/graphfin \
  -w /workspace/build/output \
  -e LD_LIBRARY_PATH=/workspace/build/output:/usr/local/lib64/lgraph:/usr/local/lib64:/usr/local/lib:/usr/lib/jvm/java-11-openjdk/lib/server \
  tugraph-compile-arm64:phase0 ./lgraph_server \
  -c lgraph_standalone.json --directory /var/lib/graphfin \
  --host 0.0.0.0 --port 7070 --rpc_port 9090 --enable_rpc true
```

The supplied examples use the inherited development credentials. For a deployment,
configure your own account and access policy before exposing the service. These
commands do not start a distributed shard router or qualify an HA deployment.

In another terminal, run either example against a fresh database. Each declares
its schema, writes observations and prints results/exports:

```bash
python3 demo/SeriesTelemetry/telemetry.py --port 7070
python3 demo/SeriesFinancial/financial.py --port 7070
```

Check each script's `--help` for credentials and output options. Repeating a schema
creation example on an already populated database may need a new test database.

## Run merge gates without rebuilding

```bash
bash ci/merge/run.sh unit
bash ci/merge/run.sh smoke
bash ci/merge/run.sh clients
bash ci/merge/run.sh ha
```

Each gate starts an isolated container, mounts source read-only, and places fresh
results under `/tmp/graphfin-merge-results` on the host. Override `MERGE_RESULTS`
to archive elsewhere. Required skipped cases fail. The `clients` gate requires
the real pinned Neo4j driver and never downloads it implicitly. It may be installed
in the compile environment, or in a separate dependency directory using the same
Python environment; set `MERGE_PYTHON_DEPS` to mount that directory read-only.
The gate records its content hash, so dependency provisioning does not require
rebuilding the image. HA starts three servers and needs an appropriate resource budget.

Explicit one-time client dependency provisioning (the test run itself stays offline):

```bash
mkdir -p /tmp/graphfin-python-deps
docker run --rm --platform linux/arm64 \
  -v "$PWD:/workspace:ro" -v /tmp/graphfin-python-deps:/deps \
  tugraph-compile-arm64:phase0 python3 -m pip install --target /deps \
  -r /workspace/ci/merge/requirements-clients.txt
MERGE_PYTHON_DEPS=/tmp/graphfin-python-deps bash ci/merge/run.sh clients
```

Do not run gates while source or binaries are changing: their provenance check
rejects such runs. The gates are subsets of the
[full post-merge qualification plan](testing/post-merge.md).
