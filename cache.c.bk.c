//
// Created by Seokin Hong and Prashant Nair on 3/29/18.
//

#include "cache.h"
#include "math.h"
#include "stdlib.h"
#include "assert.h"
#include "stdio.h"
#include <string.h>


void init_cache(MCache* c, uns sets, uns assocs, uns repl_policy, uns linesize, int compression_enabled, int compression_mode, int max_blocks_per_line)
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
    
    //for compression
    c->compression_enabled = compression_enabled;
    c->compression_mode = compression_mode;
    c->max_blocks_per_line = max_blocks_per_line;
    assert(max_blocks_per_line <= 4); // By design you cant have more than 4 blocks per line
}

int isHit(MCache *c, Addr addr, Flag is_write, uns comp_size)
{
    Addr tag = addr;
    int isHit = 0;
    
    if(c->compression_enabled == 1){ // is compressed
        if(c->compression_mode == 1) // using YACC
            isHit=mcache_access_yacc(c,tag,is_write, comp_size);
        else if(c->compression_mode == 2) // using Random
            isHit=mcache_access_rand(c,tag,is_write, comp_size);
    }
    else // Not compressed
        isHit=mcache_access(c,tag,is_write);
    
    if(is_write)
        c->s_write++;
    else
        c->s_read++;
    
    return isHit;
}


MCache_Entry expanded_line_victims_rand(MCache *c, Addr addr)
{
    uns64 offset = c->lineoffset;
    Addr  tag = addr >> offset; // full tag
    uns   set  = mcache_get_index(c,tag);
    uns   start = set * c->assocs;
    uns   end   = start + c->assocs;
    uns   ii;
    MCache_Entry evicted_entry;
    uns flag = 0;
    uns found = 0;
    
    if(c->compression_enabled == 0){ //Double check if the cache is configured for compression
        fprintf(stderr,"This cache is not configured for compression. \n Therefore, this function cannot be called! Abort! \n");
        exit(-1);
    }
    
    assert(c->compression_mode==2);
    
    //Initialize the evicted entry
    evicted_entry.valid = FALSE;
    evicted_entry.dirty = FALSE;
    evicted_entry.tag = 0;
    evicted_entry.pc = 0;
    evicted_entry.ripctr = 0;
    evicted_entry.last_access = 0;
    
    for(int k = 0; k < c->max_blocks_per_line; k++){
        evicted_entry.block_valid[k]=FALSE;
        evicted_entry.block_dirty[k]=FALSE;
        evicted_entry.block_cnt[k]=0;
        evicted_entry.block_overwritten[k]=0;
        evicted_entry.tag_rand[k]=0;
    }
    
    for (ii=start; ii<end; ii++){
        MCache_Entry *entry = &c->entries[ii];
        
        for(int i = 0; i < c->max_blocks_per_line; i++){
            if(entry->valid && (entry->tag_rand[i] == tag) && (entry->block_valid[i]==1)){  //This is the victim cacheline
                found = 1;
            }
        }
        if(found == 1){
            for(int i = 0; i < c->max_blocks_per_line; i++){
                evicted_entry = c->entries[ii];
                if(entry->block_overwritten[i]==1){
                    entry->block_overwritten[i]=0;
                    entry->block_valid[i]=0;
                    entry->block_dirty[i]=0;
                    entry->block_cnt[i]=0;
                    entry->tag_rand[i]=0;
                    flag = 1;
                }
            }
        }
        if(flag == 1 && found == 1)
        {
            return evicted_entry;
        }
    }
    fprintf(stderr,"The tag address %llx must be present in set %u with atleast one overwritten block\n", addr, set);
    exit(-1);
}

MCache_Entry expanded_line_victims_yacc(MCache *c, Addr addr)
{
    uns64 offset = c->lineoffset;
    Addr  sb_tag = addr >> offset; // full tag
    uns   block_id = sb_tag % c->max_blocks_per_line;
    sb_tag = sb_tag - block_id;
    uns   set  = mcache_get_index(c,sb_tag);
    uns   start = set * c->assocs;
    uns   end   = start + c->assocs;
    uns   ii;
    MCache_Entry evicted_entry;
    uns flag = 0;
    
    if(c->compression_enabled == 0){ //Double check if the cache is configured for compression
        fprintf(stderr,"This cache is not configured for compression. \n Therefore, this function cannot be called! Abort! \n");
        exit(-1);
    }
    assert(c->compression_mode==1);
    
    //Initialize the evicted entry
    evicted_entry.valid = FALSE;
    evicted_entry.dirty = FALSE;
    evicted_entry.tag = 0;
    evicted_entry.pc = 0;
    evicted_entry.ripctr = 0;
    evicted_entry.last_access = 0;
    
    for(int k = 0; k < c->max_blocks_per_line; k++){
        evicted_entry.block_valid[k]=FALSE;
        evicted_entry.block_dirty[k]=FALSE;
        evicted_entry.block_cnt[k]=0;
        evicted_entry.block_overwritten[k]=0;
        evicted_entry.tag_rand[k]=0;
    }
    
    for (ii=start; ii<end; ii++){
        MCache_Entry *entry = &c->entries[ii];
        
        if(entry->valid && (entry->tag == sb_tag) && (entry->block_valid[block_id]==1)){  //This is the victim cacheline
            evicted_entry = c->entries[ii];
            for(int i = 0; i < c->max_blocks_per_line; i++){
                if(entry->block_overwritten[i]==1){
                    entry->block_overwritten[i]=0;
                    entry->block_valid[i]=0;
                    entry->block_dirty[i]=0;
                    entry->block_cnt[i]=0;
                    flag = 1;
                }
            }
            if(flag == 1)
            {
                return evicted_entry;
            }
        }
    }
    fprintf(stderr,"The tag address %llx must be present in set %u with atleast one overwritten block\n", addr, set);
    exit(-1);
}

MCache_Entry install(MCache *c, Addr addr, Addr pc, Flag is_write, uns comp_size)
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
    
    for(int k = 0; k < c->max_blocks_per_line; k++){
        victim.block_valid[k]=FALSE;
        victim.block_dirty[k]=FALSE;
        victim.block_cnt[k]=0;
        victim.block_overwritten[k]=0;
    }
    
    if(c->compression_enabled == 1){ // is compressed
        if(comp_size<=16)
            comp_size=16;
        else if(comp_size<=32)
            comp_size=32;
        else if(comp_size<=48)
            comp_size=48;
        else
            comp_size=64;
        
        if(c->compression_mode == 1){ // using YACC
            victim = mcache_install_yacc(c,tag,pc,is_write,comp_size);
        }
        else
            victim = mcache_install_rand(c,tag,pc,is_write,comp_size);
        
        return victim;
    }
    else{
        victim = mcache_install(c,tag,pc,is_write);
        return victim;
    }
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

int mcache_access_yacc(MCache *c, Addr addr, Flag dirty, uns comp_size)
{
    uns64 offset = c->lineoffset;
    Addr  sb_tag = addr >> offset; // full tag
    uns   block_id = sb_tag % c->max_blocks_per_line;
    sb_tag = sb_tag - block_id;
    uns   set  = mcache_get_index(c,sb_tag);
    uns   start = set * c->assocs;
    uns   end   = start + c->assocs;
    uns   ii;
    c->s_count++;
    
    int block_cnt = 0;
    int diff_block_cnt = 0;
    int accu_valid_block_cnt = 0;
    
    if(comp_size<=16){
        block_cnt = 1;
    }
    else if(comp_size <= 32){
        block_cnt = 2;
    }
    else if(comp_size <= 48){
        block_cnt = 3;
    }
    else{
        block_cnt = 4;
    }
    
    //printf("cache size:%d\n",comp_size);
    //printf("[access] addr:%llx tag:%lx block_id:%x set:%d\n",addr, sb_tag,block_id, set);
    
    for (ii=start; ii<end; ii++){
        MCache_Entry *entry = &c->entries[ii];
        
        if(entry->valid && (entry->tag == sb_tag) && (entry->block_valid[block_id]==1)){  //check block valid bit
            entry->last_access  = c->s_count;
            entry->ripctr       = MCACHE_SRRIP_MAX;
            c->touched_wayid = (ii-start);
            c->touched_setid = set;
            c->touched_lineid = ii;
            
            if(dirty==FALSE){
                return 1;
            }
            else //If the operation is a WB then mark it as dirty
            {
                mcache_mark_dirty_yacc(c,sb_tag,set,block_id);
                if(entry->block_cnt[block_id] == block_cnt)
                    return 1;
                else{
                    if(entry->block_cnt[block_id] > block_cnt){
                        entry->block_cnt[block_id] = block_cnt;
                        return 1;
                    }
                    else{
                        diff_block_cnt = block_cnt - entry->block_cnt[block_id];
                        
                        int available_room=0;
                        //Check the total size of valid blocks
                        for(int i = 0; i < c->max_blocks_per_line; i++){
                            if(entry->block_valid[i]==1)
                                accu_valid_block_cnt = accu_valid_block_cnt + entry->block_cnt[i];
                        }
                        //Then check for if you can use the additional space for storing the expanded line
                        available_room=4-accu_valid_block_cnt;
                        if(available_room >= diff_block_cnt){
                            entry->block_cnt[block_id] = block_cnt;
                            return 1;
                        }
                        else{//if not remove lines and use the additional space for storing the expanded line
                            //accu_valid_block_cnt = 0;
                            for(int i = 1; i < c->max_blocks_per_line; i++){
                                if(entry->block_valid[(block_id+i)%c->max_blocks_per_line]==1){
                                    available_room+= entry->block_cnt[(block_id+i)%c->max_blocks_per_line];
                                    entry->block_overwritten[(block_id+i)%c->max_blocks_per_line]=1;
                                }
                                assert(available_room<=4);
                                if(available_room >= diff_block_cnt){
                                    entry->block_cnt[block_id] = block_cnt;
                                    return(i+1);
                                }
                            }
                            printf("shouldn't be here!");
                            exit(-1);
                        }
                    }
                }
            }
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
//////////////////////////Random Lines Compressed///////////////////

int mcache_access_rand(MCache *c, Addr addr, Flag dirty, uns comp_size)
{
    uns64 offset = c->lineoffset;
    Addr  tag = addr >> offset; // full tag
    uns   set  = mcache_get_index(c,tag);
    uns block_id =0;
    uns   start = set * c->assocs;
    uns   end   = start + c->assocs;
    uns   ii;
    c->s_count++;
    
    int block_cnt = 0;
    int diff_block_cnt = 0;
    int accu_valid_block_cnt = 0;
    
    if(comp_size<=16){
        block_cnt = 1;
    }
    else if(comp_size <= 32){
        block_cnt = 2;
    }
    else if(comp_size <= 48){
        block_cnt = 3;
    }
    else{
        block_cnt = 4;
    }
    
    for (ii=start; ii<end; ii++){
        MCache_Entry *entry = &c->entries[ii];
        if(entry->valid)
        {
            for(int i = 0; i < c->max_blocks_per_line; i++){
                if (entry->tag_rand[i] == tag){
                    if(entry->block_valid[i]){  //check block valid bit
                        block_id = i;
                        entry->last_access  = c->s_count;
                        entry->ripctr       = MCACHE_SRRIP_MAX;
                        c->touched_wayid = (ii-start);
                        c->touched_setid = set;
                        c->touched_lineid = ii;
                        
                        if(dirty!=TRUE){//return hit
                            return 1;
                        }
                        else{//If the operation is a WB then mark it as dirty
                            mcache_mark_dirty_rand(c,tag,set,block_id);
                            if(entry->block_cnt[block_id] == block_cnt)
                                return 1;
                            else{
                                if(entry->block_cnt[block_id] > block_cnt){
                                    entry->block_cnt[block_id] = block_cnt;
                                    return 1;
                                }
                                else{
                                    diff_block_cnt = block_cnt - entry->block_cnt[block_id];
                                    int available_room=0;
                                    //Check the total size of valid blocks
                                    for(int i = 0; i < c->max_blocks_per_line; i++){
                                        if(entry->block_valid[i]==1)
                                            accu_valid_block_cnt = accu_valid_block_cnt + entry->block_cnt[i];
                                    }
                                    available_room=4-accu_valid_block_cnt;
                                    //Then check for if you can use the additional space for storing the expanded line
                                    if(available_room >= diff_block_cnt){
                                        entry->block_cnt[block_id] = block_cnt;
                                        return 1;
                                    }
                                    else{//if not remove lines and use the additional space for storing the expanded line
                                        for(int i = 1; i < c->max_blocks_per_line; i++){
                                            if(entry->block_valid[(block_id+i)%c->max_blocks_per_line]==1){
                                                available_room+=entry->block_cnt[(block_id+i)%c->max_blocks_per_line];
                                                entry->block_overwritten[(block_id+i)%c->max_blocks_per_line]=1;
                                            }
                                            
                                            assert(available_room<=4);
                                            if(available_room >= diff_block_cnt){
                                                entry->block_cnt[block_id] = block_cnt;
                                                return(i+1);
                                            }
                                        }
                                        printf("shouldn't be here!");
                                        exit(-1);
                                    }
                                }
                            }
                        }
                    }
                }
            }
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


Flag mcache_probe_yacc    (MCache *c, Addr addr)
{
    uns64 offset = c->lineoffset;
    Addr  sb_tag = addr >> offset; // full tag
    uns   block_id = sb_tag % c->max_blocks_per_line;
    sb_tag = sb_tag - block_id;
    uns   set  = mcache_get_index(c,sb_tag);
    uns   start = set * c->assocs;
    uns   end   = start + c->assocs;
    uns   ii;
    
    for (ii=start; ii<end; ii++){
        MCache_Entry *entry = &c->entries[ii];
        if(entry->valid && (entry->tag == sb_tag)
           &&(entry->block_valid[block_id]==1)) //check block valid bit
        {
            return TRUE;
        }
    }
    
    return FALSE;
}

Flag mcache_probe_rand(MCache *c, Addr addr)
{
    uns64 offset = c->lineoffset;
    Addr  tag = addr >> offset; // full tag
    uns   block_id = 0;
    uns   set  = mcache_get_index(c, tag);
    uns   start = set * c->assocs;
    uns   end   = start + c->assocs;
    uns   ii;
    
    for (ii=start; ii<end; ii++){
        MCache_Entry *entry = &c->entries[ii];
        for(int i = 0; i < c->max_blocks_per_line; i++){
            if (entry->tag_rand[i] == tag){
                if(entry->block_valid[i]){  //check block valid bit
                    block_id = i; //check block valid bit
                    return TRUE;
                }
            }
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

Flag mcache_mark_dirty_yacc (MCache *c, Addr tag, Addr set, uns block_id)
{
    uns   start = set * c->assocs;
    uns   end   = start + c->assocs;
    uns   ii;
    
    for (ii=start; ii<end; ii++){
        MCache_Entry *entry = &c->entries[ii];
        if(entry->valid && (entry->tag == tag) && entry->block_valid[block_id])
        {
            entry->block_dirty[block_id] = TRUE;
            return TRUE;
        }
    }
    
    return FALSE;
}

Flag mcache_mark_dirty_rand (MCache *c, Addr tag, Addr set, uns block_id)
{
    uns   start = set * c->assocs;
    uns   end   = start + c->assocs;
    uns   ii;
    
    for (ii=start; ii<end; ii++){
        MCache_Entry *entry = &c->entries[ii];
        if (entry->tag_rand[block_id] == tag){
            if(entry->block_valid[block_id]){  //check block valid bit
                entry->block_dirty[block_id] = TRUE;
                return TRUE;
            }
        }
    }
    return FALSE;
}

////////////////////////////////////////////////////////////////////


////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

MCache_Entry mcache_install(MCache *c, Addr addr, Addr pc, Flag dirty)
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
    entry->pc    = pc;
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


MCache_Entry mcache_install_yacc(MCache *c, Addr addr, Addr pc, Flag dirty, uns comp_size)
{
    uns64 offset = c->lineoffset;
    Addr  sb_tag = addr >> offset; // full tag
    uns   block_id = sb_tag % c->max_blocks_per_line;
    sb_tag = sb_tag - block_id;
    uns   set  = mcache_get_index(c,sb_tag); // use super block tag to get the set number
    uns   start = set * c->assocs;
    uns   end   = start + c->assocs;
    uns   ii, victim;
    
    Flag update_lrubits=TRUE;
    
    MCache_Entry *entry;
    MCache_Entry evicted_entry;
    
    //Initialize the evicted entry
    evicted_entry.valid = FALSE;
    evicted_entry.dirty = FALSE;
    evicted_entry.tag = 0;
    evicted_entry.pc = 0;
    evicted_entry.ripctr = 0;
    evicted_entry.last_access = 0;
    
    for(int k = 0; k < c->max_blocks_per_line; k++){
        evicted_entry.block_valid[k]=FALSE;
        evicted_entry.block_dirty[k]=FALSE;
        evicted_entry.block_cnt[k]=0;
        evicted_entry.block_overwritten[k]=0;
    }
    
    //printf("addr:%llx tag:%lx block_id:%x set:%d\n",addr, sb_tag,block_id, set);
    
    for (ii=start; ii<end; ii++){
        entry = &c->entries[ii];
        if(entry->valid && (entry->tag == sb_tag) && (entry->yacc_comp == (comp_size/16)))
        {
            if(entry->block_valid[block_id]==1){ //YACC need to check the block valid bit
                fprintf(stderr,"YACC Installed entry already with addr:%llx present in set:%u tag:%llx block_id:%u\n", addr, set, sb_tag, block_id);
                exit(-1);
            }
            
            //A super block associated to new cache line is already in the cache, so check whether there is a room in this super block for the cache line
            bool has_room=false;
            int block_cnt = 0;
            
            for(int i = 0; i < c->max_blocks_per_line; i++){
                if(entry->block_valid[i] == TRUE)
                    block_cnt = block_cnt + entry->block_cnt[i];
            }
            
            if(comp_size<=16 && block_cnt<=3)
                has_room=true;
            else if(comp_size<=32 && block_cnt<=2)
                has_room=true;
            else if(comp_size<=48 && block_cnt<=1)
                has_room=true;
            else if (comp_size<=64)
                has_room=false;
            else{
                fprintf(stderr,"unsupported comp size: %d!\n",comp_size);
                exit(-1);
            }
            
            if(has_room==true)
            {
                if(update_lrubits){
                    entry->last_access  = c->s_count;
                }
                entry->ripctr       = MCACHE_SRRIP_MAX;
                c->touched_wayid = (ii-start);
                c->touched_setid = set;
                c->touched_lineid = ii;
                
                entry->block_cnt[block_id] = comp_size/16;
                entry->block_valid[block_id] =1;
                //set the dirty bit if needed
                if(dirty)
                    entry->block_dirty[block_id]=1;
                
                
                //printf("new compressed cache line is installed, addr:%llx tag:%llx set:%d block_id:%d comp_size:%d\n",addr,sb_tag,set, block_id, comp_size);
                return evicted_entry; //we install the compressed cache line in a data block. so just return empty entry (valid bit should be 0)
            }
            else
                continue;
        }
    }
    
    // find victim and install entry
    victim = mcache_find_victim(c, set);
    entry = &c->entries[victim];
    evicted_entry = c->entries[victim];
    
    
    //printf("victim cache line, tag:%llx set:%d\n",entry->tag,set);
    if(entry->valid){
        c->s_evict++;
        
        if(entry->dirty)
            c->s_writeback++;
    }
    
    
    uns ripctr_val=MCACHE_SRRIP_INIT;
    
    if(c->repl_policy==REPL_DRRIP){
        ripctr_val=mcache_drrip_get_ripctrval(c,set);
    }
    
    if(c->repl_policy==REPL_DIP){
        update_lrubits=mcache_dip_check_lru_update(c,set);
    }
    
    
    //put new information in
    entry->tag   = sb_tag;
    entry->valid = TRUE;
    
    for(int k=0; k<c->max_blocks_per_line; k++){
        entry->block_valid[k]=FALSE; // Invalidate all block ids in this new block
        entry->block_dirty[k]=FALSE; // None of these sub-blocks are dirty now
        entry->block_cnt[k]=0;
        entry->block_overwritten[k]=0;
    }
    
    entry->block_valid[block_id]=TRUE;
    entry->pc    = pc;
    //printf("new cache line is installed, addr:%llx tag:%llx set:%d block_id:%d\n",addr,sb_tag,set, block_id);
    
    if(comp_size<=16){
        entry->block_cnt[block_id]=1;
        entry->yacc_comp = 1;
    }
    else if(comp_size<=32){
        entry->block_cnt[block_id]=2;
        entry->yacc_comp = 2;
    }
    else if(comp_size<=48){
        entry->block_cnt[block_id]=3;
        entry->yacc_comp = 4; // There cannot be 48B blocks in original YACC
    }
    else if(comp_size<=64){
        entry->block_cnt[block_id]=4;
        entry->yacc_comp = 4;
    }
    
    
    if(dirty==TRUE)
        entry->block_dirty[block_id] = TRUE;
    else
        entry->block_dirty[block_id] = FALSE;
    
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

MCache_Entry mcache_install_rand(MCache *c, Addr addr, Addr pc, Flag dirty, uns comp_size)
{
    uns64 offset = c->lineoffset;
    Addr  tag = addr >> offset; // full tag
    uns   set  = mcache_get_index(c,tag);
    uns block_id =0;
    uns   start = set * c->assocs;
    uns   end   = start + c->assocs;
    uns   ii, victim;
    
    Flag update_lrubits=TRUE;
    
    MCache_Entry *entry;
    MCache_Entry evicted_entry;
    
    //Initialize the evicted entry
    evicted_entry.valid = FALSE;
    evicted_entry.dirty = FALSE;
    evicted_entry.tag = 0;
    evicted_entry.pc = 0;
    evicted_entry.ripctr = 0;
    evicted_entry.last_access = 0;
    
    for(int k = 0; k < c->max_blocks_per_line; k++){
        evicted_entry.tag_rand[k]=0;
        evicted_entry.block_valid[k]=FALSE;
        evicted_entry.block_dirty[k]=FALSE;
        evicted_entry.block_cnt[k]=0;
        evicted_entry.block_overwritten[k]=0;
    }
    
    for (ii=start; ii<end; ii++){
        entry = &c->entries[ii];
        if(entry->valid)
        {
            for(int j = 0; j < c->max_blocks_per_line; j++){
                block_id = j;
                //if ((entry->tag_rand[ii] == tag) && (entry->block_valid[block_id]==1)){
                if ((entry->tag_rand[block_id] == tag) && (entry->block_valid[block_id]==1)){
                    fprintf(stderr,"RAND Installed entry already with addr:%llx present in set:%u tag:%llx block_id:%u\n", addr, set, tag, block_id);
                    exit(-1);
                }
                if (((entry->tag_rand[block_id]>>2) == (tag>>2)) && (entry->block_valid[block_id]==1)){
                    //A block associated to new cache line is already in the cache, so check whether there is a room in this super block for the cache line
                    bool has_room=false;
                    int block_cnt = 0;

                    for(int i = 0; i < c->max_blocks_per_line; i++){
                        if(entry->block_valid[i] == TRUE)
                            block_cnt = block_cnt + entry->block_cnt[i];
                    }
                    
                    if(comp_size<=16 && block_cnt<=3)
                        has_room=true;
                    else if(comp_size<=32 && block_cnt<=2)
                        has_room=true;
                    else if(comp_size<=48 && block_cnt<=1)
                        has_room=true;
                    else if (comp_size<=64)
                        has_room=false;
                    else{
                        fprintf(stderr,"unsupported comp size: %d!\n",comp_size);
                        exit(-1);
                    }
                    
                    block_id=-1;
                    if(has_room==true){
                        for(int i=0; i<c->max_blocks_per_line; i++){
                            if(entry->block_valid[i] == FALSE)
                            {
                                block_id=i;
                            }
                        }
                        
                        if(update_lrubits){
                            entry->last_access  = c->s_count;
                        }
                        entry->ripctr = MCACHE_SRRIP_MAX;
                        c->touched_wayid = (ii-start);
                        c->touched_setid = set;
                        c->touched_lineid = ii;
                        
                        entry->tag_rand[block_id] = tag;
                        entry->block_cnt[block_id] = comp_size/16;
                        entry->block_valid[block_id] = TRUE;
                        entry->block_overwritten[block_id] = FALSE;
			//set the dirty bit if needed
                        if(dirty)
                            entry->block_dirty[block_id]=1;
                        
                        
                        //printf("new compressed cache line is installed, addr:%llx tag:%llx set:%d block_id:%d comp_size:%d\n",addr,sb_tag,set, block_id, comp_size);
                        return evicted_entry; //we install the compressed cache line in a data block. so just return empty entry (valid bit should be 0)
                        
                    }
                    else{
                        continue;
                    }
                }
            }
        }
    }
    // find victim and install entry
    victim = mcache_find_victim(c, set);
    entry = &c->entries[victim];
    evicted_entry = c->entries[victim];
    
    
    //printf("victim cache line, tag:%llx set:%d\n",entry->tag,set);
    if(entry->valid){
        c->s_evict++;
        
        if(entry->dirty)
            c->s_writeback++;
    }
    
    
    uns ripctr_val=MCACHE_SRRIP_INIT;
    
    if(c->repl_policy==REPL_DRRIP){
        ripctr_val=mcache_drrip_get_ripctrval(c,set);
    }
    
    if(c->repl_policy==REPL_DIP){
        update_lrubits=mcache_dip_check_lru_update(c,set);
    }
    
    
    uns num_blocks_evicted = comp_size/16;
    
    //put new information in
    entry->tag   = tag;
    entry->valid = TRUE;
    
    int available_room=0;
    int tmp_cnt=0;
    int id_size = 4;
    
    block_id=0;
    
    while(block_id < c->max_blocks_per_line)
    {
        if(entry->block_valid[block_id]==TRUE){
            available_room +=entry->block_cnt[block_id];
            assert(available_room <=4);
        }
        
        block_id++;
    }
    
    available_room = 4 - available_room;
    
    if(available_room < num_blocks_evicted){
        block_id=rand()%c->max_blocks_per_line;
        while(1){
            if((entry->block_valid[block_id]==TRUE) && (entry->block_cnt[block_id]==id_size))
            {
                entry->tag_rand[block_id]=0;
                entry->block_valid[block_id]=FALSE; // Invalidate all block ids in this new block
                entry->block_overwritten[block_id]=0;
                
                if(entry->block_dirty[block_id]==TRUE)
                    evicted_entry.block_overwritten[block_id]=1;
                
                available_room +=entry->block_cnt[block_id];
                entry->block_cnt[block_id]=0;
                entry->block_dirty[block_id]=FALSE; // None of these sub-blocks are dirty now

                
                if(available_room>=num_blocks_evicted)
                {
                    break;
                }
            }
            block_id=(block_id+1)%c->max_blocks_per_line;
            tmp_cnt++;
            if(tmp_cnt%4==0)
            {
                if(id_size>0)
                    id_size--;
            }
            if(tmp_cnt>17)
            {
                assert(0);
            }
        }
    }
    else{
        for(int hh=0; hh < c->max_blocks_per_line; hh++)
        {
            evicted_entry.tag_rand[hh]=0;
            evicted_entry.block_valid[hh]=FALSE;
            evicted_entry.block_dirty[hh]=FALSE;
            evicted_entry.block_cnt[hh]=0;
            evicted_entry.block_overwritten[hh]=0;
        }
        for(int hh=0; hh < c->max_blocks_per_line; hh++)
        {
            if(entry->block_valid[hh]!=TRUE)
            {
                block_id=hh;
                break;
            }
        }
    }
    
    entry->block_valid[block_id]=TRUE;
    entry->tag_rand[block_id]=tag;
    entry->block_overwritten[block_id]=FALSE;

    entry->pc = pc;
    //printf("new cache line is installed, addr:%llx tag:%llx set:%d block_id:%d\n",addr,sb_tag,set, block_id);
    
    if(comp_size<=16)
        entry->block_cnt[block_id]=1;
    else if(comp_size<=32)
        entry->block_cnt[block_id]=2;
    else if(comp_size<=48)
        entry->block_cnt[block_id]=3;
    else if(comp_size<=64)
        entry->block_cnt[block_id]=4;
    
    
    if(dirty==TRUE)
        entry->block_dirty[block_id] = TRUE;
    else
        entry->block_dirty[block_id] = FALSE;
    
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
            if(c->compression_enabled)
            {
                if(c->compression_mode==1 || c->compression_mode==2)
                    retval=(addr>>5)%c->sets;
                else
                {
                    fprintf(stderr,"unsupported compression mode:%d\n",c->compression_mode);
                    exit(-1);
                }
            }
            else
                retval=(addr>>5)%c->sets;
            
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
    
    printf("==========================================================\n");
    printf("==========            LLC Statistics           ===========\n");
    printf("==========================================================\n");
    printf("Cache Configuration: \n");
    printf("\tCacheSize:      %dK\n", (llcache->sets*llcache->assocs*llcache->linesize/1024));
    printf("\tLineSize:       %dB\n", llcache->linesize);
    printf("\tAssociativity:  %d\n", llcache->assocs);
    printf("\tTotalSets:      %d\n", llcache->sets);
    printf("\tCompressEnable: %d\n", llcache->compression_enabled);
    printf("\tCompressMode:   %d\n\n", llcache->compression_mode);
    printf("\tMaxBlocks:      %d\n\n", llcache->max_blocks_per_line);
    
    printf("Cache Statistics: \n\n");
    totLookups=llcache->s_count;
    totMisses=llcache->s_miss;
    totHits=llcache->s_count-llcache->s_miss;
    
    if( totLookups )
    {
        printf("Overall Cache stat:\n");
        printf("Overall_Accesses: %lld\n", totLookups);
        printf("Overall_Misses:   %lld\n", totMisses);
        printf("Overall_Hits:     %lld\n", totHits);
        printf("Overall_MissRate \t : %5f\n\n", ((double)totMisses/(double)totLookups)*100.0);
    }
    
    
}


void print_superblock_stat(MCache* llcache)
{
    unsigned long long num_cachelines=llcache->sets * llcache->assocs;
    long long int sblock_cnt[5];
    
    
    for(int i=0; i<=4;i++)
        sblock_cnt[i]=0;
    
    
    for(int i=0; i<num_cachelines;i++)
    {
        MCache_Entry *entry = &(llcache->entries[i]);
        
        int subblock_cnt=0;
        if(entry->valid)
        {
            if(llcache->compression_mode ==0 || llcache->compression_enabled==0)
            {
                subblock_cnt=1;
            }
            else
            {
                for(int block_id=0; block_id<llcache->max_blocks_per_line;block_id++)
                {
                    if(entry->block_valid[block_id]==1){  //check block valid bit
                        subblock_cnt++;
                    }
                }
            }
        }
        assert(subblock_cnt<=4);
        sblock_cnt[subblock_cnt]++;
    }
    
    printf("superblock_cnt:");
    for(int i=1;i<=4;i++)
        printf(" %lld",sblock_cnt[i]);
    printf("\n");
}

