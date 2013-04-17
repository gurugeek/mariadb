/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "Copyright (c) 2007 Tokutek Inc.  All rights reserved."
#ident "$Id: perf_nop.cc 45903 2012-07-19 13:06:39Z leifwalsh $"
#include "test.h"

#include <stdio.h>
#include <stdlib.h>

#include <toku_pthread.h>
#include <unistd.h>
#include <memory.h>
#include <sys/stat.h>
#include <db.h>

#include "threaded_stress_test_helpers.h"

// The intent of this test is to measure the throughput of the test infrastructure executing a nop
// on multiple threads.

DB_TXN** txns;
int num_txns;

static int commit_and_create_txn(
    DB_TXN* UU(txn), 
    ARG arg, 
    void* UU(operation_extra), 
    void* UU(stats_extra)
    ) 
{
    int rand_txn_id = random() % num_txns;
    int r = txns[rand_txn_id]->commit(txns[rand_txn_id], 0);
    CKERR(r);
    r = arg->env->txn_begin(arg->env, 0, &txns[rand_txn_id], arg->txn_type); 
    CKERR(r);
    return 0;
}

static void
stress_table(DB_ENV* env, DB** dbp, struct cli_args *cli_args) {
    if (verbose) printf("starting running of stress\n");

    num_txns = cli_args->txn_size;
    XCALLOC_N(num_txns, txns);
    for (int i = 0; i < num_txns; i++) {
        int r = env->txn_begin(env, 0, &txns[i], DB_TXN_SNAPSHOT); 
        CKERR(r);
    }
    
    struct arg myarg;
    arg_init(&myarg, dbp, env, cli_args);
    myarg.operation = commit_and_create_txn;
    
    run_workers(&myarg, 1, cli_args->time_of_test, false, cli_args);

    for (int i = 0; i < num_txns; i++) {
        int chk_r = txns[i]->commit(txns[i], 0);
        CKERR(chk_r);
    }
    toku_free(txns);
    num_txns = 0;
}

int
test_main(int argc, char *const argv[]) {
    num_txns = 0;
    txns = NULL;
    struct cli_args args = get_default_args_for_perf();
    parse_stress_test_args(argc, argv, &args);
    args.single_txn = true;
    // this test is all about transactions, make the DB small
    args.num_elements = 1;
    args.num_DBs= 1;    
    stress_test_main(&args);
    return 0;
}
