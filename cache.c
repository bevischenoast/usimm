//
// Created by Seokin Hong and Prashant Nair on 3/29/18.
//

#include "cache.h"
#include "memory_controller.h"
#include "math.h"
#include "stdlib.h"
#include "assert.h"
#include "stdio.h"
#include <string.h>
#include <immintrin.h>

//#define DEBUG_CACHEDATA 1
void init_cache(MCache* c, uns sets, uns assocs, uns repl_policy, uns linesize)
{
    c->sets    = sets;
    c->assocs  = assocs;
    c->linesize = linesize;
    c->lineoffset=log2(linesize);
    c->repl_policy = (MCache_ReplPolicy)repl_policy;
    c->index_policy = 0;
    c->entries  = (MCache_Entry *) calloc (sets * assocs, sizeof(MCache_Entry));
    c->fifo_ptr  = (uns *) calloc (sets, sizeof(uns));

    //for drrip or dip
    mcache_select_leader_sets(c,sets);
    c->psel=(MCACHE_PSEL_MAX+1)/2;

}

int isHit(MCache *c, Addr addr, Flag is_write)
{
    Addr tag = addr;
    int isHit = 0;

    isHit=mcache_access(c,tag,is_write);

    if(is_write)
        c->s_write++;
    else
        c->s_read++;

    return isHit;
}

MCache_Entry install(MCache *c, Addr addr, Addr pc, Flag is_write, char* data,int datasize)
{
    Addr tag = addr;
    MCache_Entry victim;

    //Initialize the victim
    victim.valid = FALSE;
    victim.dirty = FALSE;
    victim.tag = 0;
    victim.pc = 0;
    victim.ripctr = 0;
    victim.last_access = 0;

    victim = mcache_install(c,tag,pc,is_write, data, datasize);
    return victim;
}
////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

void mcache_select_leader_sets(MCache *c, uns sets){
    uns done=0;

    c->is_leader_p0  = (Flag *) calloc (sets, sizeof(Flag));
    c->is_leader_p1  = (Flag *) calloc (sets, sizeof(Flag));

    while(done <= MCACHE_LEADER_SETS){
        uns randval=rand()%sets;
        if( (c->is_leader_p0[randval]==FALSE)&&(c->is_leader_p1[randval]==FALSE)){
            c->is_leader_p0[randval]=TRUE;
            done++;
        }
    }

    done=0;
    while(done <= MCACHE_LEADER_SETS){
        uns randval=rand()%sets;
        if( (c->is_leader_p0[randval]==FALSE)&&(c->is_leader_p1[randval]==FALSE)){
            c->is_leader_p1[randval]=TRUE;
            done++;
        }
    }
}



int mcache_access(MCache *c, Addr addr, Flag dirty)
{
    uns64 offset = c->lineoffset;
    Addr  tag  = addr >> offset; // full tags
    uns   set  = mcache_get_index(c,tag);
    uns   start = set * c->assocs;
    uns   end   = start + c->assocs;
    uns   ii;
    c->s_count++;

    for (ii=start; ii<end; ii++){
        MCache_Entry *entry = &c->entries[ii];
        if(entry->valid && (entry->tag == tag))
        {
            entry->last_access  = c->s_count;
            entry->ripctr       = MCACHE_SRRIP_MAX;
            entry->access_count++;

            if(dirty) entry->wr_count++;
            else entry->rd_count++;

            c->touched_wayid = (ii-start);
            c->touched_setid = set;
            c->touched_lineid = ii;
            if(dirty==TRUE) //If the operation is a WB then mark it as dirty
            {
                mcache_mark_dirty(c,tag);
            }
            return 1;
        }
    }
    //even on a miss, we need to know which set was accessed
    c->touched_wayid = 0;
    c->touched_setid = set;
    c->touched_lineid = start;

    c->s_miss++;
    return 0;
}


////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////
Flag mcache_probe    (MCache *c, Addr addr)
{
    uns64 offset = c->lineoffset;
    Addr  tag  = addr>>offset; // full tags
    uns   set  = mcache_get_index(c,tag);
    uns   start = set * c->assocs;
    uns   end   = start + c->assocs;
    uns   ii;

    for (ii=start; ii<end; ii++){
        MCache_Entry *entry = &c->entries[ii];
        if(entry->valid && (entry->tag == tag))
        {
            return TRUE;
        }
    }

    return FALSE;
}

////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

Flag mcache_invalidate    (MCache *c, Addr addr)
{
    uns64 offset = c->lineoffset;
    Addr  tag  = addr>>offset; // full tags
    uns   set  = mcache_get_index(c,tag);
    uns   start = set * c->assocs;
    uns   end   = start + c->assocs;
    uns   ii;

    for (ii=start; ii<end; ii++){
        MCache_Entry *entry = &c->entries[ii];
        if(entry->valid && (entry->tag == tag))
        {
            entry->valid = FALSE;
            return TRUE;
        }
    }

    return FALSE;
}


////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

void mcache_swap_lines(MCache *c, uns set, uns way_ii, uns way_jj)
{
    uns   start = set * c->assocs;
    uns   loc_ii   = start + way_ii;
    uns   loc_jj   = start + way_jj;

    MCache_Entry tmp = c->entries[loc_ii];
    c->entries[loc_ii] = c->entries[loc_jj];
    c->entries[loc_jj] = tmp;

}

////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

Flag mcache_mark_dirty    (MCache *c, Addr tag)
{
    //uns64 offset = c->lineoffset;
    uns   set  = mcache_get_index(c,tag);
    uns   start = set * c->assocs;
    uns   end   = start + c->assocs;
    uns   ii;

    for (ii=start; ii<end; ii++){
        MCache_Entry *entry = &c->entries[ii];
        if(entry->valid && (entry->tag == tag))
        {
            entry->dirty = TRUE;
            return TRUE;
        }
    }

    return FALSE;
}
////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////
MCache_Entry mcache_install(MCache *c, Addr addr, Addr pc, Flag dirty, char* data, int datasize)
{
    uns64 offset = c->lineoffset;
    Addr  tag  = addr>>offset; // full tags
    uns   set  = mcache_get_index(c,tag);
    uns   start = set * c->assocs;
    uns   end   = start + c->assocs;
    uns   ii, victim;

    Flag update_lrubits=TRUE;

    MCache_Entry *entry;
    MCache_Entry evicted_entry;

    for (ii=start; ii<end; ii++){
        entry = &c->entries[ii];
        if(entry->valid && (entry->tag == tag)){
            fprintf(stderr,"Installed entry already with addr:%llx present in set:%u\n", addr, set);
            exit(-1);
        }
    }

    // find victim and install entry
    victim = mcache_find_victim(c, set);

    entry = &c->entries[victim];
    evicted_entry =c->entries[victim];
    if(entry->valid){
        c->s_evict++;
        if(entry->dirty)
            c->s_writeback++;
    }

    //udpate DRRIP info and select value of ripctr
    uns ripctr_val=MCACHE_SRRIP_INIT;

    if(c->repl_policy==REPL_DRRIP){
        ripctr_val=mcache_drrip_get_ripctrval(c,set);
    }

    if(c->repl_policy==REPL_DIP){
        update_lrubits=mcache_dip_check_lru_update(c,set);
    }

    //put new information in
    entry->tag   = tag;
    entry->valid = TRUE;
    entry->dead = 0;
    entry->pc = pc;
    entry->address = addr;
    entry->access_count = 0;
    if (datasize != 64) {
        fprintf(stderr, "block size should be 64\n");
        exit(-1);
    }

    uns64 *current_data;
    uns64 *new_data;

    current_data = (uns64*)&entry->data;
    new_data = (uns64*)&data;

    for (uns8 i = datasize - 1; i >= 0; i--) {
        uns8 sr_tmp_current = (*current_data >> i) & 1;
        uns8 sr_tmp_new = (*new_data >> i) & 1;

        if (sr_tmp_current != sr_tmp_new) {
            c->flip_cnt++;

            if (sr_tmp_current == 1) {
                c->p_ap_trs++;
            } else {
                c->ap_p_trs++;
            }
        } else if (sr_tmp_current == 0 && sr_tmp_new == 0) {
            c->p_p_trs++;
        }
    }

    memcpy(entry->data, data, datasize);

#ifdef DEBUG_CACHEDATA
    for(int i=0; i<16;i++)
    {
        printf("%x\t",*((int*)entry->data+i));
    }
    printf("\n");
#endif

    if(dirty==TRUE)
        entry->dirty=TRUE;
    else
        entry->dirty = FALSE;
    entry->ripctr  = ripctr_val;

    if(update_lrubits){
        entry->last_access  = c->s_count;
    }

    c->fifo_ptr[set] = (c->fifo_ptr[set]+1)%c->assocs; // fifo update

    c->touched_lineid=victim;
    c->touched_setid=set;
    c->touched_wayid=victim-(set*c->assocs);

    return evicted_entry;
}


////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

Flag mcache_dip_check_lru_update(MCache *c, uns set){
    Flag update_lru=TRUE;

    if(c->is_leader_p0[set]){
        if(c->psel<MCACHE_PSEL_MAX){
            c->psel++;
        }
        update_lru=FALSE;
        if(rand()%100<5) update_lru=TRUE; // BIP
    }

    if(c->is_leader_p1[set]){
        if(c->psel){
            c->psel--;
        }
        update_lru=1;
    }

    if( (c->is_leader_p0[set]==FALSE)&& (c->is_leader_p1[set]==FALSE)){
        if(c->psel >= (MCACHE_PSEL_MAX+1)/2){
            update_lru=1; // policy 1 wins
        }else{
            update_lru=FALSE; // policy 0 wins
            if(rand()%100<5) update_lru=TRUE; // BIP
        }
    }

    return update_lru;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
uns mcache_drrip_get_ripctrval(MCache *c, uns set){
    uns ripctr_val=MCACHE_SRRIP_INIT;

    if(c->is_leader_p0[set]){
        if(c->psel<MCACHE_PSEL_MAX){
            c->psel++;
        }
        ripctr_val=0;
        if(rand()%100<5) ripctr_val=1; // BIP
    }

    if(c->is_leader_p1[set]){
        if(c->psel){
            c->psel--;
        }
        ripctr_val=1;
    }

    if( (c->is_leader_p0[set]==FALSE)&& (c->is_leader_p1[set]==FALSE)){
        if(c->psel >= (MCACHE_PSEL_MAX+1)/2){
            ripctr_val=1; // policy 1 wins
        }else{
            ripctr_val=0; // policy 0 wins
            if(rand()%100<5) ripctr_val=1; // BIP
        }
    }


    return ripctr_val;
}


////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

uns mcache_find_victim (MCache *c, uns set)
{
    int ii;
    int start = set   * c->assocs;
    int end   = start + c->assocs;

    //search for invalid first
    for (ii = start; ii < end; ii++){
        if(!c->entries[ii].valid){
            return ii;
        }
    }

    switch(c->repl_policy){
        case REPL_LRU:
            return mcache_find_victim_lru(c, set);
        case REPL_RND:
            return mcache_find_victim_rnd(c, set);
        case REPL_SRRIP:
            return mcache_find_victim_srrip(c, set);
        case REPL_DRRIP:
            return mcache_find_victim_srrip(c, set);
        case REPL_FIFO:
            return mcache_find_victim_fifo(c, set);
        case REPL_DIP:
            return mcache_find_victim_lru(c, set);
        default:
            assert(0);
    }

    return -1;

}


////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

uns mcache_find_victim_lru (MCache *c,  uns set)
{
    uns start = set   * c->assocs;
    uns end   = start + c->assocs;
    uns lowest=start;
    uns ii;
    

    for (ii = start; ii < end; ii++){
        if (c->entries[ii].last_access < c->entries[lowest].last_access){
            lowest = ii;
        }
    }

    return lowest;
}
////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
MCache_Entry find_and_invalid_a_dead_block(MCache *c, Addr addr )
{
    uns64   offset = c->lineoffset;
    Addr    tag    = addr>>offset; // full tags
    uns     set    = mcache_get_index(c,tag);
    uns     start  = set   * c->assocs;
    uns     end    = start + c->assocs;
    uns64   lowest_last_access = -1;
    uns     ii;
    uns     deadblock_idx=start;
    Flag    found = 0;
    MCache_Entry deadblock;

    for (ii=start; ii<end; ii++){
        if ( !c->entries[ii].valid ){
            deadblock_idx = ii;
            found = 1;
        }
    }

    if( !found ) {
        //select LRU victim block as dead block
        lowest_last_access = -1;
        for (ii = start; ii < end; ii++) {
            if (c->entries[ii].last_access < lowest_last_access) {
                //select the lru block
                deadblock_idx = ii;

                lowest_last_access = c->entries[ii].last_access;
            }
        }
    }
    c->entries[deadblock_idx].valid = 0;
    deadblock.valid = c->entries[deadblock_idx].valid;
    deadblock.address = c->entries[deadblock_idx].address;
    deadblock.dirty = c->entries[deadblock_idx].dirty;
    c->entries[deadblock_idx].dirty = 0;
    return deadblock;
}
////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
void write_back_a_dirty_dead_block(MCache_Entry deadblock,int numc, int ROB_tail, long long int CYCLE_VAL){
    Addr wb_addr = 0;
    wb_addr = deadblock.address;
    insert_write(wb_addr, CYCLE_VAL, numc, ROB_tail);
}
////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
uns mcache_find_victim_rnd (MCache *c,  uns set)
{
    uns start = set   * c->assocs;
    uns victim = start + rand()%c->assocs;

    return  victim;
}



////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

uns mcache_find_victim_srrip (MCache *c,  uns set)
{
    uns start = set   * c->assocs;
    uns end   = start + c->assocs;
    uns ii;
    uns victim = end; // init to impossible

    while(victim == end){
        for (ii = start; ii < end; ii++){
            if (c->entries[ii].ripctr == 0){
                victim = ii;
                break;
            }
        }

        if(victim == end){
            for (ii = start; ii < end; ii++){
                c->entries[ii].ripctr--;
            }
        }
    }

    return  victim;
}


////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

uns mcache_find_victim_fifo (MCache *c,  uns set)
{
    uns start = set   * c->assocs;
    uns retval = start + c->fifo_ptr[set];
    return retval;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

uns mcache_get_index(MCache *c, Addr addr){
    uns retval;

    switch(c->index_policy){
        case 0:
            retval=(addr>>2)%c->sets;
            break;

        default:
            fprintf(stderr,"unsupported index_policy\n");
            exit(-1);
    }

    return retval;
}


void print_cache_stats(MCache * llcache){
    //uns64 totLookups_type = 0, totMisses_type = 0, totHits_type = 0;
    uns64 totLookups = 0, totMisses = 0, totHits = 0;
    uns64 aggregate_lines = 0;
    uns64 aggregate_block_size = 0;
    uns64 aggregate_block_index = 0;

    printf("==========================================================\n");
    printf("==========            LLC Statistics           ===========\n");
    printf("==========================================================\n");
    printf("Cache Configuration: \n");
    printf("\tCacheSize:      %dK\n", (llcache->sets*llcache->assocs*llcache->linesize/1024));
    printf("\tLineSize:       %dB\n", llcache->linesize);
    printf("\tAssociativity:  %d\n", llcache->assocs);
    printf("\tTotalSets:      %d\n", llcache->sets);


    printf("Cache Statistics: \n\n");
    totLookups=llcache->s_count;
    totMisses=llcache->s_miss;
    totHits=llcache->s_count-llcache->s_miss;

    if( totLookups )
    {
        printf("Overall Cache stat :\n");
        printf("Overall_Accesses : %lld\n", totLookups);
        printf("Overall_Misses :   %lld\n", totMisses);
        printf("Overall_Hits :     %lld\n", totHits);
        printf("Overall_MissRate \t : %5f\n\n", ((double)totMisses/(double)totLookups)*100.0);
    }
}
