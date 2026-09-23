/*
 * Phase 0 experiment: prove the ~1000-graph ceiling is a pthread TLS-key limit
 * in LMDB, not a TuGraph graph-count limit.
 *
 * Background
 * ----------
 * TuGraph opens one LMDB environment per graph. The default build does NOT pass
 * MDB_NOTLS (src/core/lmdb_store.cpp:60-65; the flag is only added when built
 * with ENABLE_SHARE_DIR). Without MDB_NOTLS, LMDB calls
 * pthread_key_create(&env->me_txkey, ...) once per environment
 * (src/core/lmdb/mdb.c:5211-5212), and glibc caps process-wide TLS keys at
 * PTHREAD_KEYS_MAX = 1024, which is not an adjustable rlimit.
 *
 * This program opens environments in a loop using exactly the flags TuGraph
 * uses, with and without MDB_NOTLS, and reports where it fails.
 *
 * Build and run:
 *   dev/phase0/experiments/run_lmdb_tls_limit.sh
 *
 * Expected result on glibc (verified on the pinned arm64 image):
 *   TLS    failed at env #1025 with EAGAIN
 *   NOTLS  opened 5000 environments without failure
 *
 * The 1025-vs-998 difference is the ~25 TLS keys already consumed by glibc,
 * brpc, cpprestsdk and the Python runtime in the real server process.
 *
 * Build with the vendored LMDB:
 *   gcc -O0 -I<repo>/src/core/lmdb -o lmdb_tls_limit lmdb_tls_limit.c \
 *       <repo>/src/core/lmdb/mdb.c <repo>/src/core/lmdb/midl.c -lpthread
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "lmdb.h"

#define MAX_ENVS 5000
#define MAP_SIZE ((size_t)1 << 30)

int main(int argc, char **argv) {
    int notls = (argc > 1 && strcmp(argv[1], "notls") == 0);
    const char *tag = notls ? "NOTLS" : "TLS";
    int n;
    int last_ok = 0;

    for (n = 1; n <= MAX_ENVS; n++) {
        char path[256];
        MDB_env *env = NULL;
        MDB_txn *txn = NULL;
        int rc;

        snprintf(path, sizeof(path), "/tmp/lmdb_tls_envs/%s_%d", tag, n);
        mkdir(path, 0777);

        rc = mdb_env_create(&env);
        if (!rc) {
            mdb_env_set_mapsize(env, MAP_SIZE);
            mdb_env_set_maxdbs(env, 10000);
            mdb_env_set_maxreaders(env, 1200);
        }
        if (!rc) {
            /* Same flags as LMDBKvStore::Open, plus optional MDB_NOTLS. */
            unsigned int flags = MDB_NOMEMINIT | MDB_NORDAHEAD | MDB_NOSYNC;
            if (notls) flags |= MDB_NOTLS;
            rc = mdb_env_open(env, path, flags, 0664);
        }
        if (!rc) rc = mdb_txn_begin(env, NULL, MDB_RDONLY, &txn);

        if (rc) {
            printf("%-5s FAILED opening environment #%d: rc=%d (%s)\n",
                   tag, n, rc, mdb_strerror(rc));
            break;
        }
        mdb_txn_abort(txn);
        last_ok = n;
        if (n % 1000 == 0) {
            printf("%-5s %d environments opened OK...\n", tag, n);
            fflush(stdout);
        }
    }

    if (n > MAX_ENVS) {
        printf("%-5s opened %d environments without failure\n", tag, MAX_ENVS);
    }
    printf("%-5s highest successful environment index: %d\n", tag, last_ok);
    return 0;
}
