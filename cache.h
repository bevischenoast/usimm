//
// Created by Seokin Hong on 3/29/18.
//

#ifndef SRC_CACHE_HPP
#define SRC_CACHE_HPP

#include <stdbool.h>
#include <stdint.h>
#include "global_types.h"

#define FALSE 0
#define TRUE  1

#define HIT   1
#define MISS  0

#define MCACHE_SRRIP_MAX  7
#define MCACHE_SRRIP_INIT 1
#define MCACHE_PSEL_MAX    1023
#define MCACHE_LEADER_SETS  32

typedef struct MCache_Entry {
    Flag valid;
    Flag dirty;
    Flag dead;
    Addr tag;
    //For Random Compression Algorithm
    Addr tag_rand[4];
    Addr address;
    Addr pc;
    uns ripctr;
    uns64 last_access;

    uns yacc_comp;
    uns block_valid[4]; //block id used in YACC
    uns block_dirty[4]; //dirty bit for blocks
    uns block_cnt[4];
    uns block_overwritten[4];
    uns block_queue[4];
    uns map_block_cnt[16];
    uns64 access_count;
    uns64 rd_count;
    uns64 wr_count;
    char data[64];

} MCache_Entry;

typedef enum MCache_ReplPolicy_Enum {
    REPL_LRU = 0,
    REPL_RND = 1,
    REPL_SRRIP = 2,
    REPL_DRRIP = 3,
    REPL_FIFO = 4,
    REPL_DIP = 5,
    NUM_REPL_POLICY = 6
} MCache_ReplPolicy;

typedef struct MCache {
    uns sets;
    uns assocs;
    uns linesize;
    uns64 lineoffset;
    MCache_ReplPolicy repl_policy; //0:LRU  1:RND 2:SRRIP
    uns index_policy; // how to index cache

    Flag *is_leader_p0; // leader SET for D(RR)IP
    Flag *is_leader_p1; // leader SET for D(RR)IP
    uns psel;

    MCache_Entry *entries;
    uns *fifo_ptr; // for fifo replacement (per set)

    uns64 s_count; // number of accesses
    uns64 s_miss; // number of misses
    uns64 s_evict; // number of evictions
    uns64 s_writeback; // number of writeback

    uns64 s_read;
    uns64 s_write;

    uns64 flip_cnt;
    uns64 p_ap_trs;
    uns64 ap_p_trs;
    uns64 p_p_trs;
    uns64 ap_ap_trs;

    uns64 s_sblock_cnt[5]; //1: 1 subblock, 1: 2 subblock, 2: 3 subblock, 3: 4 subblock
    uns64 s_sblock_called;
    uns64 s_sblock_aggr[5];

    uns64 s_access_valid_entry_to_install;
    uns64 s_fail_install_sblock;
    uns64 s_success_install_sblock;
    uns64 s_noroom_in_sblock;
    uns64 s_diff_comp_size;


    int touched_wayid;
    int touched_setid;
    int touched_lineid;


} MCache;


void init_cache(MCache* c, uns sets, uns assocs, uns repl, uns block_size);

int isHit(MCache* c, Addr addr, Flag dirty);

MCache_Entry find_and_invalid_a_dead_block(MCache *c, Addr addr );

void write_back_a_dirty_dead_block(MCache_Entry deadblock,int numc, int ROB_tail, long long int CYCLE_VAL);

MCache_Entry install(MCache* c, Addr addr, Addr pc, Flag dirty, char* data, int datasize);

MCache_Entry mcache_install(MCache *c, Addr addr, Addr pc, Flag dirty, char* data, int datasize);


void mcache_new(MCache* c, uns sets, uns assocs, uns linesize, uns repl);

int mcache_access(MCache *c, Addr addr, Flag dirty);  //true: hit, false: miss

Flag mcache_probe(MCache *c, Addr addr);

Flag mcache_invalidate(MCache *c, Addr addr);

Flag mcache_mark_dirty(MCache *c, Addr addr);


uns mcache_get_index(MCache *c, Addr addr);

uns mcache_find_victim(MCache *c, uns set);

uns mcache_find_victim_lru(MCache *c, uns set);

uns mcache_find_victim_rnd(MCache *c, uns set);

uns mcache_find_victim_srrip(MCache *c, uns set);

uns mcache_find_victim_fifo(MCache *c, uns set);

void mcache_swap_lines(MCache *c, uns set, uns way_i, uns way_j);

void mcache_select_leader_sets(MCache *c, uns sets);

uns mcache_drrip_get_ripctrval(MCache *c, uns set);

Flag mcache_dip_check_lru_update(MCache *c, uns set);

void print_cache_stats(MCache *c);


#endif //SRC_CACHE_HPP
