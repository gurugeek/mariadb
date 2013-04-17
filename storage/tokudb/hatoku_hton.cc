/* -*- mode: C; c-basic-offset: 4 -*- */
#define MYSQL_SERVER 1
#include "toku_mysql_priv.h"
#include <db.h>

extern "C" {
#include "stdint.h"
#if defined(_WIN32)
#include "misc.h"
#endif
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include "toku_os.h"
#include "toku_time.h"
}


/* We define DTRACE after mysql_priv.h in case it disabled dtrace in the main server */
#ifdef HAVE_DTRACE
#define _DTRACE_VERSION 1
#else
#endif


#include <mysql/plugin.h>
#include "hatoku_hton.h"
#include "hatoku_defines.h"
#include "ha_tokudb.h"


#undef PACKAGE
#undef VERSION
#undef HAVE_DTRACE
#undef _DTRACE_VERSION

#define TOKU_METADB_NAME "tokudb_meta"

typedef struct savepoint_info {
    DB_TXN* txn;
    tokudb_trx_data* trx;
    bool in_sub_stmt;
} *SP_INFO, SP_INFO_T;


static inline void *thd_data_get(THD *thd, int slot) {
    return thd->ha_data[slot].ha_ptr;
}

static inline void thd_data_set(THD *thd, int slot, void *data) {
    thd->ha_data[slot].ha_ptr = data;
}



static uchar *tokudb_get_key(TOKUDB_SHARE * share, size_t * length, my_bool not_used __attribute__ ((unused))) {
    *length = share->table_name_length;
    return (uchar *) share->table_name;
}

static handler *tokudb_create_handler(handlerton * hton, TABLE_SHARE * table, MEM_ROOT * mem_root);
static MYSQL_THDVAR_BOOL(commit_sync, PLUGIN_VAR_THDLOCAL, "sync on txn commit", 
                         /* check */ NULL, /* update */ NULL, /* default*/ TRUE);
static MYSQL_THDVAR_ULONGLONG(write_lock_wait,
  0,
  "time waiting for write lock",
  NULL, 
  NULL, 
  5000, // default
  0, // min?
  ULONGLONG_MAX, // max
  1 // blocksize
  );
static MYSQL_THDVAR_ULONGLONG(read_lock_wait,
  0,
  "time waiting for read lock",
  NULL, 
  NULL, 
  4000, // default
  0, // min?
  ULONGLONG_MAX, // max
  1 // blocksize
  );
static MYSQL_THDVAR_UINT(pk_insert_mode,
  0,
  "set the primary key insert mode",
  NULL, 
  NULL, 
  1, // default
  0, // min?
  2, // max
  1 // blocksize
  );
static MYSQL_THDVAR_BOOL(load_save_space,
  0,
  "if on, intial loads are slower but take less space",
  NULL, 
  NULL, 
  FALSE
  );
static MYSQL_THDVAR_BOOL(disable_slow_alter,
  0,
  "if on, alter tables that require copy are disabled",
  NULL, 
  NULL, 
  FALSE
  );
static MYSQL_THDVAR_BOOL(create_index_online,
  0,
  "if on, create index done online",
  NULL, 
  NULL, 
  FALSE
  );
static MYSQL_THDVAR_BOOL(prelock_empty,
  0,
  "Tokudb Prelock Empty Table",
  NULL, 
  NULL, 
  TRUE
  );
static MYSQL_THDVAR_UINT(block_size,
  0,
  "fractal tree block size",
  NULL, 
  NULL, 
  4<<20, // default
  4096,  // min
  ~0L,   // max
  1      // blocksize???
  );
static MYSQL_THDVAR_UINT(read_block_size,
  0,
  "fractal tree read block size",
  NULL, 
  NULL, 
  128*1024, // default
  4096,  // min
  ~0L,   // max
  1      // blocksize???
  );

void tokudb_checkpoint_lock(THD * thd);
void tokudb_checkpoint_unlock(THD * thd);

static
void
tokudb_checkpoint_lock_update(
    THD* thd,
    struct st_mysql_sys_var* var,
    void* var_ptr,
    const void* save) 
{
    my_bool* val = (my_bool *) var_ptr;
    *val= *(my_bool *) save ? TRUE : FALSE;
    if (*val) {
        tokudb_checkpoint_lock(thd);
    }
    else {
        tokudb_checkpoint_unlock(thd);
    }
}
  
static MYSQL_THDVAR_BOOL(checkpoint_lock,
  0,
  "Tokudb Checkpoint Lock",
  NULL, 
  tokudb_checkpoint_lock_update, 
  FALSE
  );


static void tokudb_print_error(const DB_ENV * db_env, const char *db_errpfx, const char *buffer);
static void tokudb_cleanup_log_files(void);
static int tokudb_end(handlerton * hton, ha_panic_function type);
static bool tokudb_flush_logs(handlerton * hton);
static bool tokudb_show_status(handlerton * hton, THD * thd, stat_print_fn * print, enum ha_stat_type);
static int tokudb_close_connection(handlerton * hton, THD * thd);
static int tokudb_commit(handlerton * hton, THD * thd, bool all);
static int tokudb_rollback(handlerton * hton, THD * thd, bool all);
#if defined(HA_GENERAL_ONLINE)
static uint tokudb_alter_table_flags(uint flags);
#endif
static int tokudb_rollback_to_savepoint(handlerton * hton, THD * thd, void *savepoint);
static int tokudb_savepoint(handlerton * hton, THD * thd, void *savepoint);
static int tokudb_release_savepoint(handlerton * hton, THD * thd, void *savepoint);
static int tokudb_discover(handlerton *hton, THD* thd, const char *db, 
                        const char *name,
                        uchar **frmblob, 
                        size_t *frmlen);
handlerton *tokudb_hton;

const char *ha_tokudb_ext = ".tokudb";
char *tokudb_data_dir;
ulong tokudb_debug;
DB_ENV *db_env;
DB* metadata_db;
HASH tokudb_open_tables;
pthread_mutex_t tokudb_mutex;
pthread_mutex_t tokudb_meta_mutex;


//my_bool tokudb_shared_data = FALSE;
static u_int32_t tokudb_init_flags = 
    DB_CREATE | DB_THREAD | DB_PRIVATE | 
    DB_INIT_LOCK | 
    DB_INIT_MPOOL |
    DB_INIT_TXN | 
    DB_INIT_LOG |
    DB_RECOVER;
static u_int32_t tokudb_env_flags = 0;
// static u_int32_t tokudb_lock_type = DB_LOCK_DEFAULT;
// static ulong tokudb_log_buffer_size = 0;
// static ulong tokudb_log_file_size = 0;
static ulonglong tokudb_cache_size = 0;
static ulonglong tokudb_max_lock_memory = 0;
static char *tokudb_home;
static char *tokudb_tmp_dir;
static char *tokudb_log_dir;
// static long tokudb_lock_scan_time = 0;
// static ulong tokudb_region_size = 0;
// static ulong tokudb_cache_parts = 1;
static const char tokudb_hton_name[] = "TokuDB";
static const int tokudb_hton_name_length = sizeof(tokudb_hton_name) - 1;
static u_int32_t tokudb_checkpointing_period;
u_int32_t tokudb_write_status_frequency;
u_int32_t tokudb_read_status_frequency;
#ifdef TOKUDB_VERSION
char *tokudb_version = (char*) TOKUDB_VERSION;
#else
 char *tokudb_version;
#endif
static int tokudb_fs_reserve_percent;  // file system reserve as a percentage of total disk space

#if defined(_WIN32)
extern "C" {
#include "ydb.h"
}
#endif

static int tokudb_init_func(void *p) {
    TOKUDB_DBUG_ENTER("tokudb_init_func");
    int r;
#if defined(_WIN64)
        r = toku_ydb_init();
        if (r) {
            printf("got error %d\n", r);
            goto error;
        }
#endif
    db_env = NULL;
    metadata_db = NULL;

    tokudb_hton = (handlerton *) p;

    pthread_mutex_init(&tokudb_mutex, MY_MUTEX_INIT_FAST);
    pthread_mutex_init(&tokudb_meta_mutex, MY_MUTEX_INIT_FAST);
    (void) my_hash_init(&tokudb_open_tables, table_alias_charset, 32, 0, 0, (my_hash_get_key) tokudb_get_key, 0, 0);

    tokudb_hton->state = SHOW_OPTION_YES;
    // tokudb_hton->flags= HTON_CAN_RECREATE;  // QQQ this came from skeleton
    tokudb_hton->flags = HTON_CLOSE_CURSORS_AT_COMMIT;
#ifdef DB_TYPE_TOKUDB
    tokudb_hton->db_type = DB_TYPE_TOKUDB;
#else
    tokudb_hton->db_type = DB_TYPE_UNKNOWN;
#endif

    tokudb_hton->create = tokudb_create_handler;
    tokudb_hton->close_connection = tokudb_close_connection;

    tokudb_hton->savepoint_offset = sizeof(SP_INFO_T);
    tokudb_hton->savepoint_set = tokudb_savepoint;
    tokudb_hton->savepoint_rollback = tokudb_rollback_to_savepoint;
    tokudb_hton->savepoint_release = tokudb_release_savepoint;

    tokudb_hton->discover = tokudb_discover;
    
    tokudb_hton->commit = tokudb_commit;
    tokudb_hton->rollback = tokudb_rollback;
    tokudb_hton->panic = tokudb_end;
    tokudb_hton->flush_logs = tokudb_flush_logs;
    tokudb_hton->show_status = tokudb_show_status;
#if defined(HA_GENERAL_ONLINE)
    tokudb_hton->alter_table_flags = tokudb_alter_table_flags;
#endif
    if (!tokudb_home)
        tokudb_home = mysql_real_data_home;
    DBUG_PRINT("info", ("tokudb_home: %s", tokudb_home));
#if 0
    if (!tokudb_log_buffer_size) { // QQQ
        tokudb_log_buffer_size = max(table_cache_size * 512, 32 * 1024);
        DBUG_PRINT("info", ("computing tokudb_log_buffer_size %ld\n", tokudb_log_buffer_size));
    }
    tokudb_log_file_size = tokudb_log_buffer_size * 4;
    tokudb_log_file_size = MY_ALIGN(tokudb_log_file_size, 1024 * 1024L);
    tokudb_log_file_size = max(tokudb_log_file_size, 10 * 1024 * 1024L);
    DBUG_PRINT("info", ("computing tokudb_log_file_size: %ld\n", tokudb_log_file_size));
#endif
    if ((r = db_env_create(&db_env, 0))) {
        DBUG_PRINT("info", ("db_env_create %d\n", r));
        goto error;
    }

    DBUG_PRINT("info", ("tokudb_env_flags: 0x%x\n", tokudb_env_flags));
    r = db_env->set_flags(db_env, tokudb_env_flags, 1);
    if (r) { // QQQ
        if (tokudb_debug & TOKUDB_DEBUG_INIT) 
            TOKUDB_TRACE("%s:WARNING: flags=%x r=%d\n", __FUNCTION__, tokudb_env_flags, r); 
        // goto error;
    }

    // config error handling
    db_env->set_errcall(db_env, tokudb_print_error);
    db_env->set_errpfx(db_env, "TokuDB");

    //
    // set default comparison functions
    //
    r = db_env->set_default_bt_compare(db_env, tokudb_cmp_dbt_key);
    if (r) {
        DBUG_PRINT("info", ("set_default_bt_compare%d\n", r));
        goto error; 
    }

    {
    char *tmp_dir = tokudb_tmp_dir;
    char *data_dir = tokudb_data_dir;
    if (data_dir == 0) {
        data_dir = mysql_data_home;
    }
    if (tmp_dir == 0) {
        tmp_dir = data_dir;
    }
    DBUG_PRINT("info", ("tokudb_data_dir: %s\n", data_dir));
    db_env->set_data_dir(db_env, data_dir);

    DBUG_PRINT("info", ("tokudb_tmp_dir: %s\n", tmp_dir));
    db_env->set_tmp_dir(db_env, tmp_dir);
    }

    if (tokudb_log_dir) {
        DBUG_PRINT("info", ("tokudb_log_dir: %s\n", tokudb_log_dir));
        db_env->set_lg_dir(db_env, tokudb_log_dir);
    }

    // config the cache table size to min(1/2 of physical memory, 1/8 of the process address space)
    if (tokudb_cache_size == 0) {
        uint64_t physmem, maxdata;
        physmem = toku_os_get_phys_memory_size();
        tokudb_cache_size = physmem / 2;
        r = toku_os_get_max_process_data_size(&maxdata);
        if (r == 0) {
            if (tokudb_cache_size > maxdata / 8)
                tokudb_cache_size = maxdata / 8;
        }
    }
    if (tokudb_cache_size) {
        DBUG_PRINT("info", ("tokudb_cache_size: %lld\n", tokudb_cache_size));
        r = db_env->set_cachesize(db_env, (u_int32_t)(tokudb_cache_size >> 30), (u_int32_t)(tokudb_cache_size % (1024L * 1024L * 1024L)), 1);
        if (r) {
            DBUG_PRINT("info", ("set_cachesize %d\n", r));
            goto error; 
        }
    }
    if (tokudb_max_lock_memory == 0) {
        tokudb_max_lock_memory = tokudb_cache_size/8;
    }
    if (tokudb_max_lock_memory) {
        DBUG_PRINT("info", ("tokudb_max_lock_memory: %lld\n", tokudb_max_lock_memory));
        r = db_env->set_lk_max_memory(db_env, tokudb_max_lock_memory);
        if (r) {
            DBUG_PRINT("info", ("set_lk_max_memory %d\n", r));
            goto error; 
        }
    }
    
    u_int32_t gbytes, bytes; int parts;
    r = db_env->get_cachesize(db_env, &gbytes, &bytes, &parts);
    if (r == 0) 
        if (tokudb_debug & TOKUDB_DEBUG_INIT) 
            TOKUDB_TRACE("%s:tokudb_cache_size=%lld\n", __FUNCTION__, ((unsigned long long) gbytes << 30) + bytes);

#if 0
    // QQQ config the logs
    DBUG_PRINT("info", ("tokudb_log_file_size: %ld\n", tokudb_log_file_size));
    db_env->set_lg_max(db_env, tokudb_log_file_size);
    DBUG_PRINT("info", ("tokudb_log_buffer_size: %ld\n", tokudb_log_buffer_size));
    db_env->set_lg_bsize(db_env, tokudb_log_buffer_size);
    // DBUG_PRINT("info",("tokudb_region_size: %ld\n", tokudb_region_size));
    // db_env->set_lg_regionmax(db_env, tokudb_region_size);
#endif

    // config the locks
#if 0 // QQQ no lock types yet
    DBUG_PRINT("info", ("tokudb_lock_type: 0x%lx\n", tokudb_lock_type));
    db_env->set_lk_detect(db_env, tokudb_lock_type);
#endif
    r = db_env->set_lk_max_locks(db_env, 0xffffffff);
    if (r) {
        DBUG_PRINT("info", ("tokudb_set_max_locks %d\n", r));
        goto error;
    }

    if (db_env->set_redzone) {
        r = db_env->set_redzone(db_env, tokudb_fs_reserve_percent);
        if (r && (tokudb_debug & TOKUDB_DEBUG_INIT))
            TOKUDB_TRACE("%s:%d r=%d\n", __FUNCTION__, __LINE__, r);
    }

    if (tokudb_debug & TOKUDB_DEBUG_INIT) TOKUDB_TRACE("%s:env open:flags=%x\n", __FUNCTION__, tokudb_init_flags);

    r = db_env->set_generate_row_callback_for_put(db_env,generate_row_for_put);
    assert(!r);
    r = db_env->set_generate_row_callback_for_del(db_env,generate_row_for_del);
    assert(!r);
#if defined(HA_GENERAL_ONLINE)
    db_env->set_update(db_env, tokudb_update_fun);
#endif
    r = db_env->open(db_env, tokudb_home, tokudb_init_flags, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH);

    if (tokudb_debug & TOKUDB_DEBUG_INIT) TOKUDB_TRACE("%s:env opened:return=%d\n", __FUNCTION__, r);

    if (r) {
        DBUG_PRINT("info", ("env->open %d\n", r));
        goto error;
    }

    r = db_env->checkpointing_set_period(db_env, tokudb_checkpointing_period);
    assert(!r);

    r = db_create(&metadata_db, db_env, 0);
    if (r) {
        DBUG_PRINT("info", ("failed to create metadata db %d\n", r));
        goto error;
    }
    

    r= metadata_db->open(metadata_db, NULL, TOKU_METADB_NAME, NULL, DB_BTREE, DB_THREAD, 0);
    if (r) {
        if (r != ENOENT) {
            sql_print_error("Got error %d when trying to open metadata_db", r);
            goto error;
        }
        r = metadata_db->close(metadata_db,0);
        assert(r == 0);
        r = db_create(&metadata_db, db_env, 0);
        if (r) {
            DBUG_PRINT("info", ("failed to create metadata db %d\n", r));
            goto error;
        }

        r= metadata_db->open(metadata_db, NULL, TOKU_METADB_NAME, NULL, DB_BTREE, DB_THREAD | DB_CREATE | DB_EXCL, my_umask);
        if (r) {
            goto error;
        }
    }


    DBUG_RETURN(FALSE);

error:
    if (metadata_db) {
        int rr = metadata_db->close(metadata_db, 0);
        assert(rr==0);
    }
    if (db_env) {
        int rr= db_env->close(db_env, 0);
        assert(rr==0);
        db_env = 0;
    }
    DBUG_RETURN(TRUE);
}

static int tokudb_done_func(void *p) {
    TOKUDB_DBUG_ENTER("tokudb_done_func");
    int error = 0;

    if (tokudb_open_tables.records)
        error = 1;
    my_hash_free(&tokudb_open_tables);
    pthread_mutex_destroy(&tokudb_mutex);
    pthread_mutex_destroy(&tokudb_meta_mutex);
#if defined(_WIN64)
        toku_ydb_destroy();
#endif
    TOKUDB_DBUG_RETURN(0);
}



static handler *tokudb_create_handler(handlerton * hton, TABLE_SHARE * table, MEM_ROOT * mem_root) {
    return new(mem_root) ha_tokudb(hton, table);
}

int tokudb_end(handlerton * hton, ha_panic_function type) {
    TOKUDB_DBUG_ENTER("tokudb_end");
    int error = 0;
    if (metadata_db) {
        int r = metadata_db->close(metadata_db, 0);
        assert(r==0);
    }
    if (db_env) {
        if (tokudb_init_flags & DB_INIT_LOG)
            tokudb_cleanup_log_files();
        error = db_env->close(db_env, 0);       // Error is logged
        assert(error==0);
        db_env = NULL;
    }
    TOKUDB_DBUG_RETURN(error);
}

static int tokudb_close_connection(handlerton * hton, THD * thd) {
    int error = 0;
    tokudb_trx_data* trx = NULL;
    trx = (tokudb_trx_data *) thd_data_get(thd, tokudb_hton->slot);
    if (trx && trx->checkpoint_lock_taken) {
        error = db_env->checkpointing_resume(db_env);
    }
    my_free(trx, MYF(0));
    return error;
}

bool tokudb_flush_logs(handlerton * hton) {
    TOKUDB_DBUG_ENTER("tokudb_flush_logs");
    int error;
    bool result = 0;
    u_int32_t curr_tokudb_checkpointing_period = 0;

    //
    // get the current checkpointing period
    //
    error = db_env->checkpointing_get_period(
        db_env, 
        &curr_tokudb_checkpointing_period
        );
    if (error) {
        my_error(ER_ERROR_DURING_CHECKPOINT, MYF(0), error);
        result = 1;
        goto exit;
    }

    //
    // if the current period is not the same as the variable, that means
    // the user has changed the period and now we need to update it
    //
    if (tokudb_checkpointing_period != curr_tokudb_checkpointing_period) {
        error = db_env->checkpointing_set_period(
            db_env, 
            tokudb_checkpointing_period
            );
        if (error) {
            my_error(ER_ERROR_DURING_CHECKPOINT, MYF(0), error);
            result = 1;
            goto exit;
        }
    }
    
    //
    // take the checkpoint
    //
    error = db_env->txn_checkpoint(db_env, 0, 0, 0);
    if (error) {
        my_error(ER_ERROR_DURING_CHECKPOINT, MYF(0), error);
        result = 1;
        goto exit;
    }

    result = 0;
exit:
    TOKUDB_DBUG_RETURN(result);
}

ulonglong get_write_lock_wait_time (THD* thd) {
    ulonglong ret_val = THDVAR(thd, write_lock_wait);
    return (ret_val == 0) ? ULONGLONG_MAX : ret_val;
}


ulonglong get_read_lock_wait_time (THD* thd) {
    ulonglong ret_val = THDVAR(thd, read_lock_wait);
    return (ret_val == 0) ? ULONGLONG_MAX : ret_val;
}

uint get_pk_insert_mode(THD* thd) {
    return THDVAR(thd, pk_insert_mode);
}

bool get_load_save_space(THD* thd) {
    return (THDVAR(thd, load_save_space) != 0);
}

bool get_disable_slow_alter(THD* thd) {
    return (THDVAR(thd, disable_slow_alter) != 0);
}

bool get_create_index_online(THD* thd) {
    return (THDVAR(thd, create_index_online) != 0);
}

bool get_prelock_empty(THD* thd) {
    return (THDVAR(thd, prelock_empty) != 0);
}

uint get_tokudb_block_size(THD* thd) {
    return THDVAR(thd, block_size);
}

uint get_tokudb_read_block_size(THD* thd) {
    return THDVAR(thd, read_block_size);
}

typedef struct txn_progress_info {
    char status[200];
    THD* thd;
} *TXN_PROGRESS_INFO;


void txn_progress_func(TOKU_TXN_PROGRESS progress, void* extra) {
    TXN_PROGRESS_INFO progress_info = (TXN_PROGRESS_INFO)extra;
    int r;
    if (progress->stalled_on_checkpoint) {
        if (progress->is_commit) {
            r = sprintf(
                progress_info->status, 
                "Writing committed changes to disk, processing commit of transaction, %"PRId64" out of %"PRId64, 
                progress->entries_processed, 
                progress->entries_total
                ); 
            assert(r >= 0);
        }
        else {
            r = sprintf(
                progress_info->status, 
                "Writing committed changes to disk, processing abort of transaction, %"PRId64" out of %"PRId64, 
                progress->entries_processed, 
                progress->entries_total
                ); 
            assert(r >= 0);
        }
    }
    else {
        if (progress->is_commit) {
            r = sprintf(
                progress_info->status, 
                "processing commit of transaction, %"PRId64" out of %"PRId64, 
                progress->entries_processed, 
                progress->entries_total
                ); 
            assert(r >= 0);
        }
        else {
            r = sprintf(
                progress_info->status, 
                "processing abort of transaction, %"PRId64" out of %"PRId64, 
                progress->entries_processed, 
                progress->entries_total
                ); 
            assert(r >= 0);
        }
    }
    thd_proc_info(progress_info->thd, progress_info->status);
}


static void commit_txn_with_progress(DB_TXN* txn, u_int32_t flags, THD* thd) {
    int r;
    struct txn_progress_info info;
    info.thd = thd;
    r = txn->commit_with_progress(txn, flags, txn_progress_func, &info);
    if (r != 0) {
        sql_print_error("tried committing transaction %p and got error code %d", txn, r);
    }
    assert(r == 0);
}

static void abort_txn_with_progress(DB_TXN* txn, THD* thd) {
    int r;
    struct txn_progress_info info;
    info.thd = thd;
    r = txn->abort_with_progress(txn, txn_progress_func, &info);
    if (r != 0) {
        sql_print_error("tried aborting transaction %p and got error code %d", txn, r);
    }
    assert(r == 0);
}

static int tokudb_commit(handlerton * hton, THD * thd, bool all) {
    TOKUDB_DBUG_ENTER("tokudb_commit");
    DBUG_PRINT("trans", ("ending transaction %s", all ? "all" : "stmt"));
    u_int32_t syncflag = THDVAR(thd, commit_sync) ? 0 : DB_TXN_NOSYNC;
    tokudb_trx_data *trx = (tokudb_trx_data *) thd_data_get(thd, hton->slot);
    DB_TXN **txn = all ? &trx->all : &trx->stmt;
    if (*txn) {
        if (tokudb_debug & TOKUDB_DEBUG_TXN) {
            TOKUDB_TRACE("doing txn commit:%d:%p\n", all, *txn);
        }
        commit_txn_with_progress(*txn, syncflag, thd);
        if (*txn == trx->sp_level) {
            trx->sp_level = 0;
        }
        *txn = 0;
        trx->sub_sp_level = NULL;
    } 
    else if (tokudb_debug & TOKUDB_DEBUG_TXN) {
        TOKUDB_TRACE("nothing to commit %d\n", all);
    }
    reset_stmt_progress(&trx->stmt_progress);
    TOKUDB_DBUG_RETURN(0);
}

static int tokudb_rollback(handlerton * hton, THD * thd, bool all) {
    TOKUDB_DBUG_ENTER("tokudb_rollback");
    DBUG_PRINT("trans", ("aborting transaction %s", all ? "all" : "stmt"));
    tokudb_trx_data *trx = (tokudb_trx_data *) thd_data_get(thd, hton->slot);
    DB_TXN **txn = all ? &trx->all : &trx->stmt;
    if (*txn) {
        if (tokudb_debug & TOKUDB_DEBUG_TXN) {
            TOKUDB_TRACE("rollback:%p\n", *txn);
        }
        abort_txn_with_progress(*txn, thd);
        if (*txn == trx->sp_level) {
            trx->sp_level = 0;
        }
        *txn = 0;
        trx->sub_sp_level = NULL;
    } 
    else {
        if (tokudb_debug & TOKUDB_DEBUG_TXN) {
            TOKUDB_TRACE("abort0\n");
        }
    }
    reset_stmt_progress(&trx->stmt_progress);
    TOKUDB_DBUG_RETURN(0);
}


static int tokudb_savepoint(handlerton * hton, THD * thd, void *savepoint) {
    TOKUDB_DBUG_ENTER("tokudb_savepoint");
    int error;
    SP_INFO save_info = (SP_INFO)savepoint;
    tokudb_trx_data *trx = (tokudb_trx_data *) thd_data_get(thd, hton->slot);
    if (thd->in_sub_stmt) {
        assert(trx->stmt);
        error = db_env->txn_begin(db_env, trx->sub_sp_level, &(save_info->txn), DB_INHERIT_ISOLATION);
        if (error) {
            goto cleanup;
        }
        trx->sub_sp_level = save_info->txn;
        save_info->in_sub_stmt = true;
    }
    else {
        error = db_env->txn_begin(db_env, trx->sp_level, &(save_info->txn), DB_INHERIT_ISOLATION);
        if (error) {
            goto cleanup;
        }
        trx->sp_level = save_info->txn;
        save_info->in_sub_stmt = false;
    }
    save_info->trx = trx;
    error = 0;
cleanup:
    TOKUDB_DBUG_RETURN(error);
}

static int tokudb_rollback_to_savepoint(handlerton * hton, THD * thd, void *savepoint) {
    TOKUDB_DBUG_ENTER("tokudb_rollback_to_savepoint");
    int error;
    SP_INFO save_info = (SP_INFO)savepoint;
    DB_TXN* parent = NULL;
    DB_TXN* txn_to_rollback = save_info->txn;

    tokudb_trx_data *trx = (tokudb_trx_data *) thd_data_get(thd, hton->slot);
    parent = txn_to_rollback->parent;
    if (!(error = txn_to_rollback->abort(txn_to_rollback))) {
        if (save_info->in_sub_stmt) {
            trx->sub_sp_level = parent;
        }
        else {
            trx->sp_level = parent;
        }
        error = tokudb_savepoint(hton, thd, savepoint);
    }
    TOKUDB_DBUG_RETURN(error);
}

static int tokudb_release_savepoint(handlerton * hton, THD * thd, void *savepoint) {
    TOKUDB_DBUG_ENTER("tokudb_release_savepoint");
    int error;

    SP_INFO save_info = (SP_INFO)savepoint;
    DB_TXN* parent = NULL;
    DB_TXN* txn_to_commit = save_info->txn;

    tokudb_trx_data *trx = (tokudb_trx_data *) thd_data_get(thd, hton->slot);
    parent = txn_to_commit->parent;
    if (!(error = txn_to_commit->commit(txn_to_commit, 0))) {
        if (save_info->in_sub_stmt) {
            trx->sub_sp_level = parent;
        }
        else {
            trx->sp_level = parent;
        }
        save_info->txn = NULL;
    }
    TOKUDB_DBUG_RETURN(error);
}

static int
smart_dbt_callback_verify_frm (DBT const *key, DBT  const *row, void *context) {
    DBT* stored_frm = (DBT *)context;
    stored_frm->size = row->size;
    stored_frm->data = (uchar *)my_malloc(row->size, MYF(MY_WME));
    assert(stored_frm->data);
    memcpy(stored_frm->data, row->data, row->size);
    return 0;
}
static int tokudb_discover(handlerton *hton, THD* thd, const char *db, 
                        const char *name,
                        uchar **frmblob, 
                        size_t *frmlen)
{
    TOKUDB_DBUG_ENTER("tokudb_discover");
    int error;
    DB* status_db = NULL;
    DB_TXN* txn = NULL;
    char path[FN_REFLEN + 1];
    HA_METADATA_KEY curr_key = hatoku_frm_data;
    DBT key, value;    
    bzero(&key, sizeof(key));
    bzero(&value, sizeof(&value));
    
    error = db_env->txn_begin(db_env, 0, &txn, 0);
    if (error) { goto cleanup; }

    build_table_filename(path, sizeof(path) - 1, db, name, "", 0);
    error = open_status_dictionary(&status_db, path, txn);
    if (error) { goto cleanup; }

    key.data = &curr_key;
    key.size = sizeof(curr_key);

    error = status_db->getf_set(
        status_db, 
        txn,
        0,
        &key, 
        smart_dbt_callback_verify_frm,
        &value
        );
    if (error) {
        goto cleanup;
    }

    *frmblob = (uchar *)value.data;
    *frmlen = value.size;

    error = 0;
cleanup:
    if (status_db) {
        status_db->close(status_db,0);
    }
    if (txn) {
        commit_txn(txn, 0);
    }
    TOKUDB_DBUG_RETURN(error);    
}

static int smart_dbt_do_nothing (DBT const *key, DBT  const *row, void *context) {
  return 0;
}


static int tokudb_get_user_data_size(THD *thd, bool exact, u_int64_t *data_size_ret) {
    int error;
    u_int64_t num_bytes_in_db = 0;
    DB* curr_db = NULL;
    DB_TXN* txn = NULL;
    DBC* tmp_cursor = NULL;
    DBC* tmp_table_cursor = NULL;
    DBT curr_key;
    DBT curr_val;
    DB_TXN* tmp_txn = NULL;
    memset(&curr_key, 0, sizeof curr_key); 
    memset(&curr_val, 0, sizeof curr_val);
    pthread_mutex_lock(&tokudb_meta_mutex);

    error = db_env->txn_begin(db_env, 0, &txn, DB_READ_UNCOMMITTED);
    if (error) {
        goto cleanup;
    }
    error = metadata_db->cursor(metadata_db, txn, &tmp_cursor, 0);
    if (error) {
        goto cleanup;
    }
    while (error == 0) {
        tmp_txn = NULL;
        //
        // here, and in other places, check if process has been killed
        // if so, get out of function so user is not stalled
        //
        if (thd->killed) {
            break;
        }
        error = db_env->txn_begin(db_env, 0, &tmp_txn, DB_READ_UNCOMMITTED);
        if (error) {
            goto cleanup;
        }

        //
        // do not need this to be super fast, so use old simple API
        //
        error = tmp_cursor->c_get(
            tmp_cursor, 
            &curr_key, 
            &curr_val, 
            DB_NEXT
            );
        if (!error) {
            char* name = (char *)curr_key.data;
            char* newname = NULL;
            u_int64_t curr_num_bytes = 0;
            DB_BTREE_STAT64 dict_stats;

            newname = (char *)my_malloc(
                get_max_dict_name_path_length(name),
                MYF(MY_WME|MY_ZEROFILL)
                );
            if (newname == NULL) { 
                error = ENOMEM;
                goto cleanup;
            }

            make_name(newname, name, "main");

            error = db_create(&curr_db, db_env, 0);
            if (error) { goto cleanup; }
            
            error = curr_db->open(curr_db, tmp_txn, newname, NULL, DB_BTREE, DB_THREAD, 0);
            if (error == ENOENT) { error = 0; continue; }
            if (error) { goto cleanup; }

            if (exact) {
                //
                // flatten if exact is required
                //
                uint curr_num_items = 0;                
                error = curr_db->cursor(curr_db, tmp_txn, &tmp_table_cursor, 0);
                if (error) {
                    tmp_table_cursor = NULL;
                    goto cleanup;
                }
                while (error != DB_NOTFOUND) {
                    error = tmp_table_cursor->c_getf_next(tmp_table_cursor, 0, smart_dbt_do_nothing, NULL);
                    if (error && error != DB_NOTFOUND) {
                        goto cleanup;
                    }
                    curr_num_items++;
                    //
                    // allow early exit if command has been killed
                    //
                    if ( (curr_num_items % 1000) == 0 && thd->killed) {
                        goto cleanup;
                    }
                }
                error = tmp_table_cursor->c_close(tmp_table_cursor);
                assert(error==0);
                tmp_table_cursor = NULL;
            }

            error = curr_db->stat64(
                curr_db, 
                tmp_txn, 
                &dict_stats
                );
            if (error) { goto cleanup; }

            curr_num_bytes = dict_stats.bt_dsize;
            if (*(uchar *)curr_val.data) {
                //
                // in this case, we have a hidden primary key, do not
                // want to report space taken up by the hidden primary key to the user
                //
                u_int64_t hpk_space = TOKUDB_HIDDEN_PRIMARY_KEY_LENGTH*dict_stats.bt_ndata;
                curr_num_bytes = (hpk_space > curr_num_bytes) ? 0 : curr_num_bytes - hpk_space;
            }
            else {
                //
                // one infinity byte per key needs to be subtracted
                //
                u_int64_t inf_byte_space = dict_stats.bt_ndata;
                curr_num_bytes = (inf_byte_space > curr_num_bytes) ? 0 : curr_num_bytes - inf_byte_space;
            }

            num_bytes_in_db += curr_num_bytes;

            {
                int r = curr_db->close(curr_db, 0);
                assert(r==0);
                curr_db = NULL;
            }
            my_free(newname,MYF(MY_ALLOW_ZERO_PTR));
        }

        if (tmp_txn) {
            commit_txn(tmp_txn, 0);
            tmp_txn = NULL;
        }
    }

    *data_size_ret = num_bytes_in_db;

    error = 0;

cleanup:
    if (tmp_cursor) {
        int r = tmp_cursor->c_close(tmp_cursor);
        assert(r==0);
    }
    if (tmp_table_cursor) {
        int r = tmp_table_cursor->c_close(tmp_table_cursor);
        assert(r==0);
    }
    if (curr_db) {
        int r = curr_db->close(curr_db, 0);
        assert(r==0);
    }
    if (tmp_txn) {
        commit_txn(tmp_txn, 0);
    }
    if (txn) {
        commit_txn(txn, 0);
    }
    if (error) {
        sql_print_error("got an error %d in show_data_size\n", error);
    }
    pthread_mutex_unlock(&tokudb_meta_mutex);
    return error;
}

/*** to be used when timestamps are sent in engine status as u_int64_t instead of as char strings
static void
format_time(u_int64_t time64, char *buf) {
    time_t timer = (time_t) time64;
    ctime_r(&timer, buf);
    size_t len = strlen(buf);
    assert(len < 26);
    char end;

    assert(len>=1);
    end = buf[len-1];
    while (end == '\n' || end == '\r') {
        buf[len-1] = '\0';
        len--;
        assert(len>=1);
        end = buf[len-1];
    }
}
***/

#define STATPRINT(legend, val) stat_print(thd, \
                                          tokudb_hton_name, \
                                          tokudb_hton_name_length, \
                                          legend, \
                                          strlen(legend), \
                                          val, \
                                          strlen(val))

static bool tokudb_show_engine_status(THD * thd, stat_print_fn * stat_print) {
    TOKUDB_DBUG_ENTER("tokudb_show_engine_status");
    int error;
    const int bufsiz = 1024;
    char buf[bufsiz] = {'\0'};

    ENGINE_STATUS engstat;

    error = db_env->get_engine_status(db_env, &engstat, buf, bufsiz);
    if (strlen(buf)) {
	STATPRINT("Environment panic string", buf);
    }
    if (error == 0) {
	if (engstat.env_panic) {
	    snprintf(buf, bufsiz, "%" PRIu64, engstat.env_panic);
	    STATPRINT("Environment panic", buf);
	}
	if (engstat.logger_panic) {
	    snprintf(buf, bufsiz, "%" PRIu64, engstat.logger_panic);
	    STATPRINT("logger panic", buf);
	    snprintf(buf, bufsiz, "%" PRIu64, engstat.logger_panic_errno);
	    STATPRINT("logger panic errno", buf);
	}


      if(engstat.enospc_threads_blocked) {
	  STATPRINT("*** URGENT WARNING ***", "FILE SYSTEM IS COMPLETELY FULL");
	  snprintf(buf, bufsiz, "FILE SYSTEM IS COMPLETELY FULL");
      }
      else if (engstat.enospc_state == 0) {
	  snprintf(buf, bufsiz, "more than %d percent of total file system space", 2*tokudb_fs_reserve_percent);
      }
      else if (engstat.enospc_state == 1) {
	  snprintf(buf, bufsiz, "*** WARNING *** FILE SYSTEM IS GETTING FULL (less than %d percent free)", 2*tokudb_fs_reserve_percent);
      } 
      else if (engstat.enospc_state == 2){
	  snprintf(buf, bufsiz, "*** WARNING *** FILE SYSTEM IS GETTING VERY FULL (less than %d percent free): INSERTS ARE PROHIBITED", tokudb_fs_reserve_percent);
      }
      else {
	  snprintf(buf, bufsiz, "information unavailable %" PRIu64, engstat.enospc_state);
      }
      STATPRINT ("disk free space", buf);

      STATPRINT("time of environment creation", engstat.creationtime);
      STATPRINT("time of engine startup", engstat.startuptime);
      STATPRINT("time now", engstat.now);

      snprintf(buf, bufsiz, "%" PRIu32, engstat.checkpoint_period);
      STATPRINT("checkpoint period", buf);
      snprintf(buf, bufsiz, "%" PRIu32, engstat.checkpoint_footprint);
      STATPRINT("checkpoint status code (0 = idle)", buf);
      STATPRINT("last checkpoint began ", engstat.checkpoint_time_begin);
      STATPRINT("last complete checkpoint began ", engstat.checkpoint_time_begin_complete);
      STATPRINT("last complete checkpoint ended ", engstat.checkpoint_time_end);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.checkpoint_last_lsn);
      STATPRINT("last complete checkpoint LSN ", buf);
      snprintf(buf, bufsiz, "%" PRIu32, engstat.checkpoint_count);
      STATPRINT("checkpoints taken  ", buf);
      snprintf(buf, bufsiz, "%" PRIu32, engstat.checkpoint_count_fail);
      STATPRINT("checkpoints failed", buf);

      snprintf(buf, bufsiz, "%" PRIu64, engstat.txn_begin);
      STATPRINT("txn begin", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.txn_commit);
      STATPRINT("txn commits", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.txn_abort);
      STATPRINT("txn aborts", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.txn_close);
      STATPRINT("txn close (commit+abort)", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.txn_oldest_live);
      STATPRINT("txn oldest live", buf);
      STATPRINT("txn oldest starttime", engstat.txn_oldest_live_starttime);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.next_lsn);
      STATPRINT("next LSN", buf);

      snprintf(buf, bufsiz, "%" PRIu64, engstat.inserts);
      STATPRINT("dictionary inserts", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.inserts_fail);
      STATPRINT("dictionary inserts fail", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.deletes);
      STATPRINT("dictionary deletes", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.deletes_fail);
      STATPRINT("dictionary deletes fail", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.updates);
      STATPRINT("dictionary updates", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.updates_fail);
      STATPRINT("dictionary updates fail", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.updates_broadcast);
      STATPRINT("dictionary broadcast updates", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.updates_broadcast_fail);
      STATPRINT("dictionary broadcast updates fail", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.le_updates);
      STATPRINT("leafentry updates", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.le_updates_broadcast);
      STATPRINT("leafentry broadcast updates", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.descriptor_set);
      STATPRINT("descriptor_set", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.partial_fetch_hit);
      STATPRINT("partial_fetch_hit", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.partial_fetch_miss);
      STATPRINT("partial_fetch_miss", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.partial_fetch_compressed);
      STATPRINT("partial_fetch_compressed", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.msn_discards);
      STATPRINT("msn_discards", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.max_workdone);
      STATPRINT("max_workdone", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.dsn_gap);
      STATPRINT("dsn_gap", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.multi_inserts);
      STATPRINT("dictionary inserts multi", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.multi_inserts_fail);
      STATPRINT("dictionary inserts multi fail", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.multi_deletes);
      STATPRINT("dictionary deletes multi", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.multi_deletes_fail);
      STATPRINT("dictionary deletes multi fail", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.multi_updates);
      STATPRINT("dictionary updates multi", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.multi_updates_fail);
      STATPRINT("dictionary updates multi fail", buf);

      snprintf(buf, bufsiz, "%" PRIu64, engstat.point_queries);
      STATPRINT("dictionary point queries", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.sequential_queries);
      STATPRINT("dictionary sequential queries", buf);

      snprintf(buf, bufsiz, "%" PRIu64, engstat.le_max_committed_xr);
      STATPRINT("le_max_committed_xr", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.le_max_provisional_xr);
      STATPRINT("le_max_provisional_xr", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.le_max_memsize);
      STATPRINT("le_max_memsize", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.le_expanded);
      STATPRINT("le_expanded", buf);

      const char * lockstat = (engstat.ydb_lock_ctr & 0x01) ? "Locked" : "Unlocked";
      u_int64_t lockctr     =  engstat.ydb_lock_ctr >> 1;   // lsb indicates if locked
      snprintf(buf, bufsiz, "%" PRIu64, lockctr);  
      STATPRINT("ydb lock", lockstat);
      STATPRINT("ydb lock counter", buf);

      snprintf(buf, bufsiz, "%" PRIu32, engstat.num_waiters_now);
      STATPRINT("num_waiters_now", buf);
      snprintf(buf, bufsiz, "%" PRIu32, engstat.max_waiters);
      STATPRINT("max_waiters", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.total_sleep_time);
      STATPRINT("total_sleep_time", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.max_time_ydb_lock_held);
      STATPRINT("max_time_ydb_lock_held", buf);
      snprintf(buf, bufsiz, "%.6f", tokutime_to_seconds(engstat.total_time_ydb_lock_held));
      STATPRINT("total_time_ydb_lock_held", buf);
      snprintf(buf, bufsiz, "%.6f", tokutime_to_seconds(engstat.total_time_since_start));
      STATPRINT("total_time_since_start", buf);


      snprintf(buf, bufsiz, "%" PRIu64, engstat.cachetable_lock_taken);  
      STATPRINT("cachetable lock taken", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.cachetable_lock_released);  
      STATPRINT("cachetable lock released", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.cachetable_hit);  
      STATPRINT("cachetable hit", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.cachetable_miss);  
      STATPRINT("cachetable miss", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.cachetable_misstime);  
      STATPRINT("cachetable misstime", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.cachetable_waittime);  
      STATPRINT("cachetable waittime", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.cachetable_wait_reading);  
      STATPRINT("cachetable wait reading", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.cachetable_wait_writing);  
      STATPRINT("cachetable wait writing", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.cachetable_wait_checkpoint);  
      STATPRINT("cachetable wait checkpoint", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.puts);  
      STATPRINT("cachetable puts (new nodes)", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.prefetches);  
      STATPRINT("cachetable prefetches", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.maybe_get_and_pins);  
      STATPRINT("cachetable maybe_get_and_pins", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.maybe_get_and_pin_hits);  
      STATPRINT("cachetable maybe_get_and_pin_hits", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.cachetable_size_current);  
      STATPRINT("cachetable size_current", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.cachetable_size_limit);  
      STATPRINT("cachetable size_limit", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.cachetable_size_writing);  
      STATPRINT("cachetable size_writing", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.get_and_pin_footprint);  
      STATPRINT("cachetable get_and_pin_footprint", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.local_checkpoint);  
      STATPRINT("local checkpoint", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.local_checkpoint_files);  
      STATPRINT("local checkpoint files", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.local_checkpoint_during_checkpoint);  
      STATPRINT("local checkpoint during checkpoint", buf);

      snprintf(buf, bufsiz, "%" PRIu32, engstat.range_locks_max);
      STATPRINT("max range locks", buf);
      snprintf(buf, bufsiz, "%" PRIu32, engstat.range_locks_curr);
      STATPRINT("range locks in use", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.range_locks_max_memory);
      STATPRINT("memory available for range locks", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.range_locks_curr_memory);
      STATPRINT("memory in use for range locks", buf);
      snprintf(buf, bufsiz, "%" PRIu32, engstat.range_lock_escalation_successes);
      STATPRINT("range lock escalation successes", buf);
      snprintf(buf, bufsiz, "%" PRIu32, engstat.range_lock_escalation_failures);
      STATPRINT("range lock escalation failures", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.range_read_locks);
      STATPRINT("range read locks acquired", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.range_read_locks_fail);
      STATPRINT("range read locks unable to be acquired", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.range_out_of_read_locks);
      STATPRINT("range read locks exhausted", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.range_write_locks);
      STATPRINT("range write locks acquired", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.range_write_locks_fail);
      STATPRINT("range write locks unable to be acquired", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.range_out_of_write_locks);
      STATPRINT("range write locks exhausted", buf);

      snprintf(buf, bufsiz, "%" PRIu64, engstat.directory_read_locks);
      STATPRINT("directory_read_locks", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.directory_read_locks_fail);
      STATPRINT("directory_read_locks_fail", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.directory_write_locks);
      STATPRINT("directory_write_locks", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.directory_write_locks_fail);
      STATPRINT("directory_write_locks_fail", buf);

      snprintf(buf, bufsiz, "%" PRIu64, engstat.fsync_count);
      STATPRINT("fsync count", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.fsync_time);
      STATPRINT("fsync time", buf);

      snprintf(buf, bufsiz, "%" PRIu64, engstat.logger_ilock_ctr);
      STATPRINT("logger ilock count", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.logger_olock_ctr);
      STATPRINT("logger olock count", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.logger_swap_ctr);
      STATPRINT("logger swap count", buf);

      STATPRINT("most recent disk full", engstat.enospc_most_recent);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.enospc_threads_blocked);
      STATPRINT("threads currently blocked by full disk", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.enospc_ctr);
      STATPRINT("ENOSPC blocked count", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.enospc_redzone_ctr);
      STATPRINT("ENOSPC reserve count (redzone)", buf);

      snprintf(buf, bufsiz, "%" PRIu64, engstat.loader_create);
      STATPRINT("loader create (success)", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.loader_create_fail);
      STATPRINT("loader create fail", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.loader_put);
      STATPRINT("loader put", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.loader_put_fail);
      STATPRINT("loader put_fail", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.loader_close);
      STATPRINT("loader close (success)", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.loader_close_fail);
      STATPRINT("loader close fail", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.loader_abort);
      STATPRINT("loader abort", buf);
      snprintf(buf, bufsiz, "%" PRIu32, engstat.loader_current);
      STATPRINT("loaders current", buf);
      snprintf(buf, bufsiz, "%" PRIu32, engstat.loader_max);
      STATPRINT("loader max", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.logsuppress);
      STATPRINT("log suppress (success) ", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.logsuppressfail);
      STATPRINT("log suppress fail", buf);

      snprintf(buf, bufsiz, "%" PRIu64, engstat.indexer_create);
      STATPRINT("indexer create (success)", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.indexer_create_fail);
      STATPRINT("indexer create fail", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.indexer_build);
      STATPRINT("indexer build (success)", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.indexer_build_fail);
      STATPRINT("indexer build fail", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.indexer_close);
      STATPRINT("indexer close (success)", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.indexer_close_fail);
      STATPRINT("indexer close fail", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.indexer_abort);
      STATPRINT("indexer abort", buf);
      snprintf(buf, bufsiz, "%" PRIu32, engstat.indexer_current);
      STATPRINT("indexers current", buf);
      snprintf(buf, bufsiz, "%" PRIu32, engstat.indexer_max);
      STATPRINT("indexer max", buf);

      snprintf(buf, bufsiz, "%" PRIu64, engstat.upgrade_env_status);
      STATPRINT("upgrade env status", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.upgrade_header);
      STATPRINT("upgrade header", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.upgrade_nonleaf);
      STATPRINT("upgrade nonleaf", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.upgrade_leaf);
      STATPRINT("upgrade leaf", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.optimized_for_upgrade);
      STATPRINT("optimized for upgrade", buf);

      snprintf(buf, bufsiz, "%" PRIu64, engstat.original_ver);
      STATPRINT("original version", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.ver_at_startup);
      STATPRINT("version at startup", buf);
      snprintf(buf, bufsiz, "%" PRIu64, engstat.last_lsn_v13);
      STATPRINT("last LSN of version 13", buf);      
      STATPRINT("time of upgrade to version 14", engstat.upgrade_v14_time);
    }
    if (error) { my_errno = error; }
    TOKUDB_DBUG_RETURN(error);
}


void tokudb_checkpoint_lock(THD * thd) {
    int error;
    tokudb_trx_data* trx = NULL;
    char status_msg[200]; //buffer of 200 should be a good upper bound.
    trx = (tokudb_trx_data *) thd_data_get(thd, tokudb_hton->slot);
    if (!trx) {
        error = create_tokudb_trx_data_instance(&trx);
        //
        // can only fail due to memory allocation, so ok to assert
        //
        assert(!error);
        thd_data_set(thd, tokudb_hton->slot, trx);
    }
    
    if (trx->checkpoint_lock_taken) {
        goto cleanup;
    }
    //
    // This can only fail if environment is not created, which is not possible
    // in handlerton
    //
    sprintf(status_msg, "Trying to grab checkpointing lock.");
    thd_proc_info(thd, status_msg);
    error = db_env->checkpointing_postpone(db_env);
    assert(!error);

    trx->checkpoint_lock_taken = true;
cleanup:
    return;
}

void tokudb_checkpoint_unlock(THD * thd) {
    int error;
    char status_msg[200]; //buffer of 200 should be a good upper bound.
    tokudb_trx_data* trx = NULL;
    trx = (tokudb_trx_data *) thd_data_get(thd, tokudb_hton->slot);
    if (!trx) {
        error = 0;
        goto  cleanup;
    }
    if (!trx->checkpoint_lock_taken) {
        error = 0;
        goto  cleanup;
    }
    //
    // at this point, we know the checkpoint lock has been taken
    //
    sprintf(status_msg, "Trying to release checkpointing lock.");
    thd_proc_info(thd, status_msg);
    error = db_env->checkpointing_resume(db_env);
    assert(!error);

    trx->checkpoint_lock_taken = false;
    
cleanup:
    return;
}




bool tokudb_show_status(handlerton * hton, THD * thd, stat_print_fn * stat_print, enum ha_stat_type stat_type) {
    switch (stat_type) {
    case HA_ENGINE_STATUS:
        return tokudb_show_engine_status(thd, stat_print);
        break;
    default:
        break;
    }
    return FALSE;
}

static void tokudb_print_error(const DB_ENV * db_env, const char *db_errpfx, const char *buffer) {
    sql_print_error("%s:  %s", db_errpfx, buffer);
}

void tokudb_cleanup_log_files(void) {
    TOKUDB_DBUG_ENTER("tokudb_cleanup_log_files");
    char **names;
    int error;

    if ((error = db_env->txn_checkpoint(db_env, 0, 0, 0)))
        my_error(ER_ERROR_DURING_CHECKPOINT, MYF(0), error);

    if ((error = db_env->log_archive(db_env, &names, 0)) != 0) {
        DBUG_PRINT("error", ("log_archive failed (error %d)", error));
        db_env->err(db_env, error, "log_archive");
        DBUG_VOID_RETURN;
    }

    if (names) {
        char **np;
        for (np = names; *np; ++np) {
#if 1
            if (tokudb_debug)
                TOKUDB_TRACE("%s:cleanup:%s\n", __FUNCTION__, *np);
#else
            my_delete(*np, MYF(MY_WME));
#endif
        }

        free(names);
    }

    DBUG_VOID_RETURN;
}

#if defined(HA_GENERAL_ONLINE)
//
// *******NOTE*****
// If the flags HA_ONLINE_DROP_INDEX and HA_ONLINE_DROP_UNIQUE_INDEX
// are ever added, prepare_drop_index and final_drop_index will need to be modified
// so that the actual deletion of DB's is done in final_drop_index and not prepare_drop_index
//
static uint tokudb_alter_table_flags(uint flags)
{
    return (HA_ONLINE_ADD_INDEX_NO_WRITES| HA_ONLINE_DROP_INDEX_NO_WRITES |
            HA_ONLINE_ADD_UNIQUE_INDEX_NO_WRITES| HA_ONLINE_DROP_UNIQUE_INDEX_NO_WRITES|HA_GENERAL_ONLINE);

}
#endif


// options flags
//   PLUGIN_VAR_THDLOCAL  Variable is per-connection
//   PLUGIN_VAR_READONLY  Server variable is read only
//   PLUGIN_VAR_NOSYSVAR  Not a server variable
//   PLUGIN_VAR_NOCMDOPT  Not a command line option
//   PLUGIN_VAR_NOCMDARG  No argument for cmd line
//   PLUGIN_VAR_RQCMDARG  Argument required for cmd line
//   PLUGIN_VAR_OPCMDARG  Argument optional for cmd line
//   PLUGIN_VAR_MEMALLOC  String needs memory allocated


// system variables

static MYSQL_SYSVAR_ULONGLONG(cache_size, tokudb_cache_size, PLUGIN_VAR_READONLY, "TokuDB cache table size", NULL, NULL, 0, 0, ~0LL, 0);

static MYSQL_SYSVAR_ULONGLONG(max_lock_memory, tokudb_max_lock_memory, PLUGIN_VAR_READONLY, "TokuDB max memory for locks", NULL, NULL, 0, 0, ~0LL, 0);
static MYSQL_SYSVAR_ULONG(debug, tokudb_debug, 0, "TokuDB Debug", NULL, NULL, 0, 0, ~0L, 0);

static MYSQL_SYSVAR_STR(log_dir, tokudb_log_dir, PLUGIN_VAR_READONLY, "TokuDB Log Directory", NULL, NULL, NULL);

static MYSQL_SYSVAR_STR(data_dir, tokudb_data_dir, PLUGIN_VAR_READONLY, "TokuDB Data Directory", NULL, NULL, NULL);

static MYSQL_SYSVAR_STR(version, tokudb_version, PLUGIN_VAR_READONLY, "TokuDB Version", NULL, NULL, NULL);

static MYSQL_SYSVAR_UINT(init_flags, tokudb_init_flags, PLUGIN_VAR_READONLY, "Sets TokuDB DB_ENV->open flags", NULL, NULL, tokudb_init_flags, 0, ~0, 0);

static MYSQL_SYSVAR_UINT(checkpointing_period, tokudb_checkpointing_period, 0, "TokuDB Checkpointing period", NULL, NULL, 60, 0, ~0L, 0);
static MYSQL_SYSVAR_UINT(write_status_frequency, tokudb_write_status_frequency, 0, "TokuDB frequency that show processlist updates status of writes", NULL, NULL, 1000, 0, ~0L, 0);
static MYSQL_SYSVAR_UINT(read_status_frequency, tokudb_read_status_frequency, 0, "TokuDB frequency that show processlist updates status of reads", NULL, NULL, 10000, 0, ~0L, 0);
static MYSQL_SYSVAR_INT(fs_reserve_percent, tokudb_fs_reserve_percent, PLUGIN_VAR_READONLY, "TokuDB file system space reserve (percent free required)", NULL, NULL, 5, 0, 100, 0);
static MYSQL_SYSVAR_STR(tmp_dir, tokudb_tmp_dir, PLUGIN_VAR_READONLY, "Tokudb Tmp Dir", NULL, NULL, NULL);

static struct st_mysql_sys_var *tokudb_system_variables[] = {
    MYSQL_SYSVAR(cache_size),
    MYSQL_SYSVAR(max_lock_memory),
    MYSQL_SYSVAR(data_dir),
    MYSQL_SYSVAR(log_dir),
    MYSQL_SYSVAR(debug),
    MYSQL_SYSVAR(commit_sync),
    MYSQL_SYSVAR(write_lock_wait),
    MYSQL_SYSVAR(read_lock_wait),
    MYSQL_SYSVAR(pk_insert_mode),
    MYSQL_SYSVAR(load_save_space),
    MYSQL_SYSVAR(disable_slow_alter),
    MYSQL_SYSVAR(create_index_online),
    MYSQL_SYSVAR(version),
    MYSQL_SYSVAR(init_flags),
    MYSQL_SYSVAR(checkpointing_period),
    MYSQL_SYSVAR(prelock_empty),
    MYSQL_SYSVAR(checkpoint_lock),
    MYSQL_SYSVAR(write_status_frequency),
    MYSQL_SYSVAR(read_status_frequency),
    MYSQL_SYSVAR(fs_reserve_percent),
    MYSQL_SYSVAR(tmp_dir),
    MYSQL_SYSVAR(block_size),
    MYSQL_SYSVAR(read_block_size),
    NULL
};

struct st_mysql_storage_engine tokudb_storage_engine = { MYSQL_HANDLERTON_INTERFACE_VERSION };

static ST_FIELD_INFO tokudb_user_data_field_info[] = {
    {"User Data Size", 8, MYSQL_TYPE_LONGLONG, 0, 0, "user data size", SKIP_OPEN_TABLE },
    {NULL, 0, MYSQL_TYPE_NULL, 0, 0, NULL, SKIP_OPEN_TABLE}
};

static int tokudb_user_data_fill_table(THD *thd, TABLE_LIST *tables, COND *cond) {
    TABLE *table = tables->table;
    uint64_t data_size;
    int error = tokudb_get_user_data_size(thd, false, &data_size);
    if (error == 0) {
        table->field[0]->store(data_size, false);
        error = schema_table_store_record(thd, table);
    }
    return error;
}

static int tokudb_user_data_init(void *p) {
    ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *) p;
    schema->fields_info = tokudb_user_data_field_info;
    schema->fill_table = tokudb_user_data_fill_table;
    return 0;
}

static int tokudb_user_data_done(void *p) {
    return 0;
}

struct st_mysql_information_schema tokudb_user_data_information_schema = { MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };

static ST_FIELD_INFO tokudb_user_data_exact_field_info[] = {
    {"User Data Size", 8, MYSQL_TYPE_LONGLONG, 0, 0, "user data size", SKIP_OPEN_TABLE },
    {NULL, 0, MYSQL_TYPE_NULL, 0, 0, NULL, SKIP_OPEN_TABLE}
};

static int tokudb_user_data_exact_fill_table(THD *thd, TABLE_LIST *tables, COND *cond) {
    TABLE *table = tables->table;
    uint64_t data_size;
    int error = tokudb_get_user_data_size(thd, true, &data_size);
    if (error == 0) {
        table->field[0]->store(data_size, false);
        error = schema_table_store_record(thd, table);
    }
    return error;
}

static int tokudb_user_data_exact_init(void *p) {
    ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *) p;
    schema->fields_info = tokudb_user_data_exact_field_info;
    schema->fill_table = tokudb_user_data_exact_fill_table;
    return 0;
}

static int tokudb_user_data_exact_done(void *p) {
    return 0;
}

struct st_mysql_information_schema tokudb_user_data_exact_information_schema = { MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };

enum { TOKUDB_PLUGIN_VERSION = 0x0400 };

mysql_declare_plugin(tokudb) 
{
    MYSQL_STORAGE_ENGINE_PLUGIN, 
    &tokudb_storage_engine, 
    "TokuDB", 
    "Tokutek Inc", 
    "Tokutek TokuDB Storage Engine with Fractal Tree(tm) Technology",
    PLUGIN_LICENSE_GPL,
    tokudb_init_func,          /* plugin init */
    tokudb_done_func,          /* plugin deinit */
    TOKUDB_PLUGIN_VERSION,     /* 4.0.0 */
    NULL,                      /* status variables */
    tokudb_system_variables,   /* system variables */
    NULL                       /* config options */
},
{
    MYSQL_INFORMATION_SCHEMA_PLUGIN, 
    &tokudb_user_data_information_schema, 
    "TokuDB_user_data", 
    "Tokutek Inc", 
    "Tokutek TokuDB Storage Engine with Fractal Tree(tm) Technology",
    PLUGIN_LICENSE_GPL,
    tokudb_user_data_init,     /* plugin init */
    tokudb_user_data_done,     /* plugin deinit */
    TOKUDB_PLUGIN_VERSION,     /* 4.0.0 */
    NULL,                      /* status variables */
    NULL,                      /* system variables */
    NULL                       /* config options */
},
{
    MYSQL_INFORMATION_SCHEMA_PLUGIN, 
    &tokudb_user_data_exact_information_schema, 
    "TokuDB_user_data_exact", 
    "Tokutek Inc", 
    "Tokutek TokuDB Storage Engine with Fractal Tree(tm) Technology",
    PLUGIN_LICENSE_GPL,
    tokudb_user_data_exact_init,     /* plugin init */
    tokudb_user_data_exact_done,     /* plugin deinit */
    TOKUDB_PLUGIN_VERSION,     /* 4.0.0 */
    NULL,                      /* status variables */
    NULL,                      /* system variables */
    NULL                       /* config options */
}
mysql_declare_plugin_end;

