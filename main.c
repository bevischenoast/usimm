#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<assert.h>

#include "processor.h"
#include "configfile.h"
#include "memory_controller.h"
#include "scheduler.h"
#include "params.h"
#include "randomize.h"
#include "cache.h"
#include "global_types.h"
#include "os.h"
#include "map.h"
#include "zlib.h"


#define MAXTRACELINESIZE 1024

#define HASHTABLE_SIZE 1024*1024

//Set this for table length of randomizer
#define MAX_TABLE_LEN 1024
char ff_done_global = 0;
long long int refetched_cacheline_cnt = 0;
long long int new_cacheline_cnt = 0;
long long int victim_dirty = 0;
long long int victim_clean = 0;
long long int llc_read_miss = 0;
long long int llc_write_miss = 0;
long long int llc_read_access = 0;
//////////////////////////////////////////
long long int fast_write = 0, slow_write = 0;
long long int fill_invalidation = 0, write_back_hit_invalidation = 0 ;
long long int dirty_deadblock_write_back = 0;
long long int dirty_deadblock_invalidation = 0,  clean_dead_block_invalidation = 0;
long long int dirty_victim_write_back = 0;
long long int fill_in_invalided_block = 0;
//////////////////////////////////////////

//////////////////////////////////////////
long long int llc_write_access = 0;
long long int pp_changed = 0;
long long int pp_unchanged = 0;
long long int G_pp_changed = 0;
long long int G_pp_unchanged = 0;
long long int no_metadata = 0;
long long int n_to_w = 0;
long long int n_to_r = 0;
long long int w_to_n = 0;
long long int w_to_r = 0;
long long int r_to_n = 0;
long long int r_to_w = 0;
long long int access_sum = 0;
long long int num_ret_read = 0;
long long int access_clean = 0;
long long int access_dirty = 0;

//Memory Statistics
unsigned long long int read_traffic = 0;
unsigned long long int write_traffic = 0;
unsigned long long int write_traffic_dirty = 0;
unsigned long long int write_traffic_clean = 0;


typedef struct node {
    // element data;
    struct node *left_link;
    struct node *right_link;
} node;

typedef node *nodePointer;
nodePointer head = NULL;

Hashtable *cl_table = NULL;

Element *insertnode(uns64 addr, char optype) {
    Element *elm = lookup(cl_table, addr);

    //lookup the key
    //if find the key, update the rd_cnt and wr_cnt
    if (elm) {
        if (optype == 'R') {
            elm->rd_cnt += 1;
        } else if (optype == 'W') {
            elm->wr_cnt += 1;
        }
        return elm;
    } else //if not, insert the key and value
    {
        Element *new_elm = createElement();
        if (optype == 'R') {
            new_elm->rd_cnt += 1;
        } else if (optype == 'W') {
            new_elm->wr_cnt += 1;
        }

        new_elm->intensity = 0;
        new_elm->pp_changed = 0;
        new_elm->pp_unchanged = 0;
        new_elm->is_metadata = 0;

        insert(cl_table, addr, new_elm);
        return NULL;
    }
}


// The most important variable, CYCLE_VAL keeps a global counter for all elements to tick.
long long int CYCLE_VAL = 0;

long long int get_current_cycle() {
    return CYCLE_VAL;
}

//The ROB to simulate OOO behaviour
struct robstructure *ROB;

//A big number that is used to avoid long stalls and let the ROB proceed ahead
long long int BIGNUM = 1000000;

//Stall counters for Statistics
long long int robf_stalls = 0;
long long int wrqf_stalls = 0;
long long int robn_stalls = 0;
long long int wrqn_stalls = 0;

//Logging counters for Statistics of time done and core power
long long int *time_done;
long long int total_time_done;
float core_power = 0;

long long int page_counter = 0;
//Set this variable to notify if the sims are done
int expt_done = 0;

/*Indicates how many config params exist and their types*/
int config_param = 0;
gzFile *tif = NULL;  /* The handles to the trace input files. */
FILE *config_file = NULL;
FILE *vi_file = NULL;
int *prefixtable; /* For (multi-threaded) MT workloads only */


MCache_Entry llc_miss_handler(Addr addr, char optype, Addr instrpc, char is_write,
                              MCache *L3Cache, long long int *llc_available_cycle,
                              int banknum,int numc, int ROB_tail, char* cachedata, int cachedata_size) {
    MCache_Entry victim;
    
    victim = install(L3Cache, addr, instrpc, is_write, cachedata, cachedata_size);  //addr[numc] is byte address here
    int write_latency = 0;

    if(PI_ENABLED)
    {
	if(victim.valid)
	{
      	  	write_latency = L3_LATENCY_WRITE_SLOW;
        
        	slow_write++;
		MCache_Entry deadblock = find_and_invalid_a_dead_block(L3Cache, addr);
        	fill_invalidation++;
		if (deadblock.dirty) {
		    write_back_a_dirty_dead_block(deadblock, numc, ROB_tail, CYCLE_VAL);
		    dirty_deadblock_invalidation++;
		    dirty_deadblock_write_back++;
		} else
		    clean_dead_block_invalidation++;
    	} else {
        	write_latency = L3_LATENCY_WRITE_FAST;
        	fast_write++;
        	fill_in_invalided_block++;
    	}	
    }
    else if(IDEAL_MODE){
		write_latency=L3_LATENCY_WRITE_FAST;
		fast_write++;
    }
    else{
		write_latency=L3_LATENCY_WRITE_SLOW;
		slow_write++;
	}
    
    llc_available_cycle[banknum] = CYCLE_VAL + write_latency;
    return victim;
}

void victim_block_handler(MCache_Entry victim, int lineoffset, int numc, int ROB_tail) {

    Addr wb_addr = 0;

    //record the property of victim block
    if (victim.valid) {
        //write back a victim block
        if (victim.dirty) {
            access_dirty += victim.access_count;
            victim_dirty++;
            write_traffic++;
            write_traffic_dirty++;

            wb_addr = victim.tag << lineoffset;
            insert_write(wb_addr, CYCLE_VAL, numc, ROB_tail);
            dirty_victim_write_back++;
        } else {
            access_clean += victim.access_count;
            victim_clean++;
        }
    }
}


int main(int argc, char *argv[]) {

    printf("---------------------------------------------\n");
    printf("-- USIMM: the Utah SImulated Memory Module --\n");
    printf("--              Version: 1.3               --\n");
    printf("---------------------------------------------\n");

    int numc = 0;
    int num_ret = 0;
    int num_fetch = 0;
    int num_done = 0;
    int writeqfull = 0;
    int fnstart;
    int currMTapp;
    long long int maxtd;
    int maxcr;
    long long int temp_inst_cntr = 0;
    long long int temp_inst_cntr2 = 0;
    char newstr[MAXTRACELINESIZE];
    long long int *nonmemops;
    char *opertype;
    long long int *addr;
    long long int *llc_available_cycle;

    long long int *instrpc;
    int *compressedSize1Line;
    int *compressedSize2Line;
    long long int **cachedata;
    char cachedata_buf[64];

    int chips_per_rank = -1;
    long long int total_inst_fetched = 0;
    int fragments = 1;

    //OS parameters
    unsigned long long int os_pages = 2097152; //2 Million pages
    OS_PAGESIZE = 4096; // 4KB is the size of each page
    OS_NUM_RND_TRIES = 4; // Random page mapping
    OS *os;

    //Cache Parameters
    MCache *L3Cache; //The L3 Cache model
    CACHE_SIZE = 8; // 8 MB
    CACHE_WAYS = 16; // 16 Ways
    CACHE_REPL = 0; // Default is LRU



    CACHE_BANKS = 8;
    L3_LATENCY_READ = 20;
    L3_LATENCY_WRITE = 49;
    L3_LATENCY_WRITE_FAST = 20;
    L3_LATENCY_WRITE_SLOW = 49;


    //To keep track of how much is done
    unsigned long long int inst_comp = 0;
    /* Initialization code. */
    printf("Initializing.\n");
    // added by choucc
    // add an argument for refreshing selection
    if (argc < 4) {
        fprintf(stderr, "Need at least one input configuration file and one trace file as argument.  Quitting.\n");
        return -3;
    }

    config_param = atoi(argv[1]);


    config_file = fopen(argv[2], "r");
    if (!config_file) {
        fprintf(stderr, "Missing system configuration file.  Quitting. \n");
        return -4;
    }

    NUMCORES = argc - 3;

    read_config_file(config_file);

    ROB = (struct robstructure *) calloc(sizeof(struct robstructure), NUMCORES);
    tif = (gzFile *) malloc(sizeof(gzFile) * NUMCORES);
    committed = (long long int *) calloc(sizeof(long long int), NUMCORES);
    fetched = (long long int *) calloc(sizeof(long long int), NUMCORES);
    ff_fetched = (long long int *) calloc(sizeof(long long int), NUMCORES);
    ff_done = (char *) calloc(sizeof(char), NUMCORES);
    time_done = (long long int *) calloc(sizeof(long long int), NUMCORES);
    nonmemops = (long long int *) calloc(sizeof(long long int), NUMCORES);
    opertype = (char *) calloc(sizeof(char), NUMCORES);
    addr = (long long int *) calloc(sizeof(long long int), NUMCORES);
    instrpc = (long long int *) calloc(sizeof(long long int), NUMCORES);
    compressedSize1Line = (int *) calloc(sizeof(int), NUMCORES);
    compressedSize2Line = (int *) calloc(sizeof(int), NUMCORES);
    llc_available_cycle = (long long int *) malloc(sizeof(long long int) * CACHE_BANKS);
    for (int i = 0; i < CACHE_BANKS; i++)
        llc_available_cycle[i] = 0;
    
    cachedata= (long long int**) calloc(sizeof(long long int*), NUMCORES);
    
    for (int i=0; i<NUMCORES; i++)
        cachedata[i]=(long long int*) calloc(sizeof(long long int), 8);
    
    prefixtable = (int *) malloc(sizeof(int) * NUMCORES);
    currMTapp = -1;

    // add an argument for selecting input traces
    for (numc = 0; numc < NUMCORES; numc++) {
        tif[numc] = gzopen(argv[numc + 3], "r");
        if (!tif[numc]) {
            fprintf(stderr, "Missing input trace file %d.  Quitting. \n", numc);
            return -5;
        }

        /* The addresses in each trace are given a prefix that equals
         their core ID.  If the input trace starts with "MT", it is
         assumed to be part of a multi-threaded app.  The addresses
         from this trace file are given a prefix that equals that of
         the last seen input trace file that starts with "MT0".  For
         example, the following is an acceptable set of inputs for
         multi-threaded apps CG (4 threads) and LU (2 threads):
         usimm 1channel.cfg MT0CG MT1CG MT2CG MT3CG MT0LU MT1LU */
        prefixtable[numc] = numc;

        /* Find the start of the filename.  It's after the last "/". */
        for (fnstart = strlen(argv[numc + 3]); fnstart >= 0; fnstart--) {
            if (argv[numc + 3][fnstart] == '/') {
                break;
            }
        }
        fnstart++;  /* fnstart is either the letter after the last / or the 0th letter. */

        if ((strlen(argv[numc + 3]) - fnstart) > 2) {
            if ((argv[numc + 3][fnstart + 0] == 'M') && (argv[numc + 3][fnstart + 1] == 'T')) {
                if (argv[numc + 3][fnstart + 2] == '0') {
                    currMTapp = numc;
                } else {
                    if (currMTapp < 0) {
                        fprintf(stderr,
                                "Poor set of input parameters.  Input file %s starts with \"MT\", but there is no preceding input file starting with \"MT0\".  Quitting.\n",
                                argv[numc + 3]);
                        return -6;
                    } else
                        prefixtable[numc] = currMTapp;
                }
            }
        }
        printf("Core %d: Input trace file %s : Addresses will have prefix %d\n", numc, argv[numc + 3],
               prefixtable[numc]);

        committed[numc] = 0;
        fetched[numc] = 0;
        ff_done[numc] = 0;
        time_done[numc] = 0;
        ROB[numc].head = 0;
        ROB[numc].tail = 0;
        ROB[numc].inflight = 0;
        ROB[numc].tracedone = 0;
    }


    vi_file = fopen("../input/8Gb_x8.vi", "r");
    chips_per_rank = 8;
    printf("Reading vi file: 8Gb_x8.vi\t\n%d Chips per Rank\n", chips_per_rank);

    if (!vi_file) {
        fprintf(stderr, "Missing DRAM chip parameter file.  Quitting. \n");
        return -5;
    }


    assert((log_base2(NUM_CHANNELS) + log_base2(NUM_RANKS) + log_base2(NUM_BANKS) + log_base2(NUM_ROWS) +
            log_base2(NUM_COLUMNS) + log_base2(CACHE_LINE_SIZE)) == ADDRESS_BITS);
    read_config_file(vi_file);
    fragments = 1;
    T_RFC = T_RFC / fragments;

    printf("Fragments: %d of length %d\n", fragments, T_RFC);

    print_params();

    for (int i = 0; i < NUMCORES; i++) {
        ROB[i].comptime = (long long int *) calloc(sizeof(long long int), ROBSIZE);
        ROB[i].mem_address = (long long int *) calloc(sizeof(long long int), ROBSIZE);
        ROB[i].instrpc = (long long int *) calloc(sizeof(long long int), ROBSIZE);
        ROB[i].optype = (int *) calloc(sizeof(int), ROBSIZE);
        ROB[i].issue_time = (long long int *) calloc(sizeof(long long int), ROBSIZE);
    }
    long long int cache_size = CACHE_SIZE * 1024 * 1024;//LLC Size (SHARED)
    uns assoc = CACHE_WAYS;
    uns block_size = 64;
    uns sets = cache_size / (assoc * block_size);
    uns repl = CACHE_REPL;

    L3Cache = (MCache *) calloc(1, sizeof(MCache));
    init_cache(L3Cache, sets, assoc, repl, block_size);

    os_pages = ((unsigned long long) 1 << ADDRESS_BITS) / OS_PAGESIZE;

    printf("os_pages: %lld\n", os_pages);
    os = os_new(os_pages, NUMCORES);

    init_memory_controller_vars();
    init_scheduler_vars();


    cl_table = createTable(HASHTABLE_SIZE);

    /* --------------- Done initializing ------------------ */

    /* Must start by reading one line of each trace file. */
    printf("Start Fast Forwarding!!\n");
    fflush(stdout);
    temp_inst_cntr2 = 0;
    char is_write = 0;
    int L3Hit = 0;

    while (ff_done_global == 0) {
        for (numc = 0; numc < NUMCORES; numc++) {
            if (ff_done[numc] == 0 && gzgets(tif[numc], newstr, MAXTRACELINESIZE)) {
                inst_comp++;
                long long int tmp;
                if (sscanf(newstr, "%lld %c", &nonmemops[numc], &opertype[numc]) > 0) {
                    if (opertype[numc] == 'R') {
                        if (sscanf(newstr, "%lld %c %llx %d %d %llx %llx %llx %llx %llx %llx %llx %llx", &nonmemops[numc], &opertype[numc], &addr[numc],
                                   &compressedSize1Line[numc], &compressedSize2Line[numc], 
                                   &cachedata[numc][0],
                                   &cachedata[numc][1],
                                   &cachedata[numc][2],
                                   &cachedata[numc][3],
                                   &cachedata[numc][4],
                                   &cachedata[numc][5],
                                   &cachedata[numc][6],
                                   &cachedata[numc][7]
                                   ) < 1) {
                            fprintf(stderr, "[1]Panic.  Poor trace format.%s\n", newstr);
                            return -4;
                        }
                    } else {
                        if (opertype[numc] == 'W') {
                            if (sscanf(newstr, "%lld %c %llx %d %d %llx %llx %llx %llx %llx %llx %llx %llx", &nonmemops[numc], &opertype[numc], &addr[numc],
                                   &compressedSize1Line[numc], &compressedSize2Line[numc], 
                                   &cachedata[numc][0],
                                   &cachedata[numc][1],
                                   &cachedata[numc][2],
                                   &cachedata[numc][3],
                                   &cachedata[numc][4],
                                   &cachedata[numc][5],
                                   &cachedata[numc][6],
                                   &cachedata[numc][7]
                                )<1){          
                            fprintf(stderr, "[2]Panic.  Poor trace format.%s\n", newstr);
                                return -3;
                            }
                        } else {
                            fprintf(stderr, "[3]Panic.  Poor trace format.%s\n", newstr);
                            return -2;
                        }
                    }
                } else {
                    fprintf(stderr, "[4]Panic.  Poor trace format. %s\n", newstr);
                    return -1;
                }
                //Insert the OS here to do a Virtual to Physical Translation since we are using a virtual address trace
                Addr phy_lineaddr = os_v2p_lineaddr(os, addr[numc] >> L3Cache->lineoffset, numc);
                addr[numc] = phy_lineaddr << (L3Cache->lineoffset);  //convert the line address to byte address
                if (opertype[numc] == 'R')
                    is_write = 0;
                else
                    is_write = 1;

                //fill cache during the fast-forwording
                L3Hit = isHit(L3Cache, addr[numc], is_write); //addr[numc] is byte address here
                if (L3Hit == 0) {
                    
                    for(int i=0; i<8;i++)
                        memcpy(&cachedata_buf[i*8],&cachedata[numc][i],8);
                    MCache_Entry victim = install(L3Cache, addr[numc], instrpc[numc],
                                                  is_write,cachedata_buf,64);  //addr[numc] is byte address here
                    uns64 addr_tmp = addr[numc] >> 6;
                    insertnode(addr_tmp, opertype[numc]);
                }
                ff_fetched[numc]++;
                ff_fetched[numc] += nonmemops[numc];


                if (ff_done[numc] == 0 && ff_fetched[numc] > FF_INST) {
                    printf("[FF Phase] Core:%d, ff done, fetched instruction:%lld\n", numc, ff_fetched[numc]);
                    fflush(stdout);

                    ff_done[numc] = 1;
                    int ff_done_cnt = 0;
                    for (int i = 0; i < NUMCORES; i++)
                        if (ff_done[i] == 1)
                            ff_done_cnt++;

                    if (ff_done_cnt == NUMCORES)
                        ff_done_global = 1;
                    else
                        ff_done_global = 0;
                }
            }
        }
    }

    //print_cache_stats(L3Cache);

    printf("Starting simulation.\n");
    fflush(stdout);
    temp_inst_cntr2 = 0;

    while (!expt_done) {

        /* For each core, retire instructions if they have finished. */
        for (numc = 0; numc < NUMCORES; numc++) {
            num_ret = 0;
            while ((num_ret < MAX_RETIRE) && ROB[numc].inflight) {
                /* Keep retiring until retire width is consumed or ROB is empty. */
                if (ROB[numc].comptime[ROB[numc].head] < CYCLE_VAL) {//todo comptime - issuetime
                    /* Keep retiring instructions if they are done. */
                    ROB[numc].head = (ROB[numc].head + 1) % ROBSIZE;
                    ROB[numc].inflight--;
                    committed[numc]++;
                    num_ret++;
                    if (ROB[numc].optype[ROB[numc].head] == 'R') {
                        access_sum += ROB[numc].comptime[ROB[numc].head] - ROB[numc].issue_time[ROB[numc].head];
                        num_ret_read++;
                    }
                } else  /* Instruction not complete.  Stop retirement for this core. */
                    break;
            }  /* End of while loop that is retiring instruction for one core. */
        }  /* End of for loop that is retiring instructions for all cores. */


        if (CYCLE_VAL % PROCESSOR_CLK_MULTIPLIER == 0) {
            /* Execute function to find ready instructions. */
            update_memory();

            /* Execute user-provided function to select ready instructions for issue. */
            /* Based on this selection, update DRAM data structures and set
             instruction completion times. */
            for (int c = 0; c < NUM_CHANNELS; c++) {
                schedule(c);
                gather_stats(c);
            }
        }

        /* For each core, bring in new instructions from the trace file to
         fill up the ROB. */
        writeqfull = 0;
        for (int c = 0; c < NUM_CHANNELS; c++) {
            if (write_queue_length[c] >= WQ_CAPACITY) {
                writeqfull = 1;
                break;
            }
        }

        for (numc = 0; numc < NUMCORES; numc++) {
            if (!ROB[numc].tracedone) { /* Try to fetch if EOF has not been encountered. */
                num_fetch = 0;
                while ((num_fetch < MAX_FETCH) && (ROB[numc].inflight != ROBSIZE) && (!writeqfull)) {
                    /* Keep fetching until fetch width or ROB capacity or WriteQ are fully consumed. */
                    /* Read the corresponding trace file and populate the tail of the ROB data structure. */
                    /* If Memop, then populate read/write queue.  Set up completion time. */

                    if (nonmemops[numc]) {  /* Have some non-memory-ops to consume. */
                        ROB[numc].optype[ROB[numc].tail] = 'N';
                        ROB[numc].comptime[ROB[numc].tail] = CYCLE_VAL + PIPELINEDEPTH;
                        nonmemops[numc]--;
                        ROB[numc].tail = (ROB[numc].tail + 1) % ROBSIZE;
                        ROB[numc].inflight++;
                        fetched[numc]++;
                        temp_inst_cntr++;
                        num_fetch++;
                    } else { /* Done consuming non-memory-ops.  Must now consume the memory rd or wr. */

                        //determine the bank number
                        int banknum = (addr[numc] >> 6) % CACHE_BANKS;

                        //check if a bank is available for the current cache access
                        if (llc_available_cycle[banknum] > CYCLE_VAL)
                            break;

                        //read
                        if (opertype[numc] == 'R') {
                            ROB[numc].mem_address[ROB[numc].tail] = addr[numc];
                            ROB[numc].optype[ROB[numc].tail] = opertype[numc];
                            ROB[numc].comptime[ROB[numc].tail] = CYCLE_VAL + BIGNUM;
                            ROB[numc].instrpc[ROB[numc].tail] = instrpc[numc];
                            ROB[numc].issue_time[ROB[numc].tail] = CYCLE_VAL;

                            int L3Hit = 0;

                            llc_read_access++;
                            //access cache

                            L3Hit = isHit(L3Cache, addr[numc], false); //addr[numc] is byte address here

                            // Cache hit
                            if (L3Hit == 1) {
                                llc_available_cycle[banknum] = CYCLE_VAL + L3_LATENCY_READ;
                                ROB[numc].comptime[ROB[numc].tail] = CYCLE_VAL + L3_LATENCY_READ + PIPELINEDEPTH;
                            } else // Cache miss
                            {
                                llc_read_miss++;
                                
                                //update LLC
                                for(int i=0; i<8;i++)
                                    memcpy(&cachedata_buf[i*8],&cachedata[numc][i],8);

                                MCache_Entry victim = llc_miss_handler(addr[numc], opertype[numc],
                                                                       instrpc[numc], false, L3Cache,
                                                                       llc_available_cycle, banknum,
                                                                       numc, ROB[numc].tail, cachedata_buf, 64);

                                //handling victim block
                                victim_block_handler(victim, L3Cache->lineoffset, numc, ROB[numc].tail);

                                // Check to see if the read is for buffered data in write queue -
                                // return constant latency if match in WQ
                                // add in read queue otherwise
                                int lat = read_matches_write_or_read_queue(addr[numc]);

                                // Check to see if the read is for buffered data in write queue -
                                // return constant latency if match in WQ
                                // add in read queue otherwise
                                if (lat) {
                                    ROB[numc].comptime[ROB[numc].tail] = CYCLE_VAL + lat + PIPELINEDEPTH;
                                } else {
                                    read_traffic++;
                                    insert_read(addr[numc], CYCLE_VAL, numc, ROB[numc].tail, instrpc[numc]);
                                }
                            }
                        }
                            // write
                        else {  /* This must be a 'W'.  We are confirming that while reading the trace. */
                            if (opertype[numc] == 'W') {
                                ROB[numc].mem_address[ROB[numc].tail] = addr[numc];
                                ROB[numc].optype[ROB[numc].tail] = opertype[numc];
                                ROB[numc].comptime[ROB[numc].tail] = CYCLE_VAL + PIPELINEDEPTH;
                                /* Also, add this to the write queue. */
                                int L3Hit = 0;
                                llc_write_access++;
                                L3Hit = isHit(L3Cache, addr[numc], true); //addr[numc] is byte address here

                                if (L3Hit == 1) {
                                    /*MCache_Entry deadblock = find_and_invalid_a_dead_block(L3Cache, addr[numc]);
                                    write_back_hit_invalidation++;
                                    if (deadblock.dirty) {
                                        write_back_a_dirty_dead_block(deadblock, numc, ROB[numc].tail, CYCLE_VAL);
                                        dirty_deadblock_write_back++;
                                        dirty_deadblock_invalidation++;
                                    } else {
                                        clean_dead_block_invalidation++;
                                    } */
                                    llc_available_cycle[banknum] = CYCLE_VAL + L3_LATENCY_WRITE_SLOW;

                                } else if (L3Hit == 0) {
                                    llc_write_miss++;
                                    //update LLC
                                    for(int i=0; i<8;i++)
                                        memcpy(&cachedata_buf[i*8],&cachedata[numc][i],8);
                                    MCache_Entry victim = llc_miss_handler(addr[numc], opertype[numc],
                                                                           instrpc[numc], true, L3Cache,
                                                                           llc_available_cycle, banknum,
                                                                           numc, ROB[numc].tail, cachedata_buf, 64);
                                    //handing victim block
                                    victim_block_handler(victim, L3Cache->lineoffset, numc, ROB[numc].tail);
                                    // Check to see if the read is for buffered data in write queue -
                                    // return constant latency if match in WQ
                                    // add in read queue otherwise
                                    int lat = read_matches_write_or_read_queue(addr[numc]);
                                    // Check to see if the read is for buffered data in write queue -
                                    // return constant latency if match in WQ
                                    // add in read queue otherwise
                                    if (lat) {
                                        ROB[numc].comptime[ROB[numc].tail] = CYCLE_VAL + lat + PIPELINEDEPTH;
                                    } else {
                                        read_traffic++;
                                        insert_read(addr[numc], CYCLE_VAL, numc, ROB[numc].tail, instrpc[numc]);
                                    }
                                }
                            } else {
                                fprintf(stderr, "Panic.  Poor trace format. \n");
                                return -1;
                            }
                        }

                        ROB[numc].tail = (ROB[numc].tail + 1) % ROBSIZE;
                        ROB[numc].inflight++;
                        fetched[numc]++;
                        temp_inst_cntr++;
                        num_fetch++;

                        /* Done consuming one line of the trace file.  Read in the next. */
                        if (gzgets(tif[numc], newstr, MAXTRACELINESIZE)) {
                            inst_comp++;
                            if (sscanf(newstr, "%lld %c", &nonmemops[numc], &opertype[numc]) > 0) {
                                if (opertype[numc] == 'R') {
                                    if (sscanf(newstr, "%lld %c %llx %d %d %llx %llx %llx %llx %llx %llx %llx %llx", &nonmemops[numc], &opertype[numc], &addr[numc],
                                            &compressedSize1Line[numc], &compressedSize2Line[numc], 
                                               &cachedata[numc][0],
                                               &cachedata[numc][1],
                                               &cachedata[numc][2],
                                               &cachedata[numc][3],
                                               &cachedata[numc][4],
                                               &cachedata[numc][5],
                                               &cachedata[numc][6],
                                               &cachedata[numc][7]
                                               ) < 1) {
                                        fprintf(stderr, "[1]Panic.  Poor trace format.%s\n", newstr);
                                        return -4;
                                    }
                                } else {
                                    if (opertype[numc] == 'W') {
                                        if (sscanf(newstr, "%lld %c %llx %d %d %llx %llx %llx %llx %llx %llx %llx %llx", &nonmemops[numc], &opertype[numc], &addr[numc],
                                            &compressedSize1Line[numc], &compressedSize2Line[numc], 
                                               &cachedata[numc][0],
                                               &cachedata[numc][1],
                                               &cachedata[numc][2],
                                               &cachedata[numc][3],
                                               &cachedata[numc][4],
                                               &cachedata[numc][5],
                                               &cachedata[numc][6],
                                               &cachedata[numc][7]
                                            )<1){          
                                        fprintf(stderr, "[2]Panic.  Poor trace format.%s\n", newstr);
                                            return -3;
                                        }
                                    } else {
                                        fprintf(stderr, "[3]Panic.  Poor trace format.%s\n", newstr);
                                        return -2;
                                    }
                                }
                            } else {
                                fprintf(stderr, "Panic.  Poor trace format.\n");
                                return -1;
                            }

                            //Insert the OS here to do a Virtual to Physical Translation since we are using a virtual address trace
                            Addr phy_lineaddr = os_v2p_lineaddr(os, addr[numc] >> L3Cache->lineoffset, numc);
                            addr[numc] = phy_lineaddr << L3Cache->lineoffset;
                        } else {
                            gzrewind(tif[numc]);
                        }

                    }  /* Done consuming the next rd or wr. */

                } /* One iteration of the fetch while loop done. */
                if ((fetched[numc] >= MAX_INST) && (time_done[numc] == 0)) {
                    num_done++;
                    time_done[numc] = CYCLE_VAL;
                }
            } /* Closing brace for if(trace not done). */
        } /* End of for loop that goes through all cores. */


        if (num_done == NUMCORES) {
            expt_done = 1;
        }

        CYCLE_VAL++;  /* Advance the simulation cycle. */
        if (fetched[0] > 100000000 * (1 + temp_inst_cntr2)) {
            temp_inst_cntr2++;
            fflush(stdout);
            printf(".");
            printf(" - %lld00Million Instructions\n", fetched[0] / 100000000);
            fflush(stdout);
            temp_inst_cntr = 0;
        }
    }

    fprintf(stderr, "sim exit!\n");

    /* Code to make sure that the write queue drain time is included in
     the execution time of the thread that finishes last. */
    maxtd = time_done[0];
    maxcr = 0;
    for (numc = 1; numc < NUMCORES; numc++) {
        if (time_done[numc] > maxtd) {
            maxtd = time_done[numc];
            maxcr = numc;
        }
    }
    time_done[maxcr] = CYCLE_VAL;

    core_power = 0;
    for (numc = 0; numc < NUMCORES; numc++) {
        /* A core has peak power of 10 W in a 4-channel config.  Peak power is consumed while the thread is running, else the core is perfectly power gated. */
        core_power = core_power + (10 * ((float) time_done[numc] / (float) CYCLE_VAL));
    }
    if (NUM_CHANNELS == 1) {
        /* The core is more energy-efficient in our single-channel configuration. */
        core_power = core_power / 2.0;
    }


    printf("Done with loop. Printing stats.\n");
    printf("Cycles %lld\n", CYCLE_VAL);
    total_time_done = 0;

    for (numc = 0; numc < NUMCORES; numc++) {
        printf("Done: Core %d: Fetched %lld : Committed %lld : At time : %lld\n", numc, fetched[numc], committed[numc],
               time_done[numc]);
        total_time_done += time_done[numc];
        total_inst_fetched = total_inst_fetched + fetched[numc];
    }
    printf("\nUSIMM_CYCLES          \t : %lld\n", CYCLE_VAL);
    printf("\nUSIMM_INST            \t : %lld\n", total_inst_fetched);
    printf("\nUSIMM_IPC             \t : %f\n", ((double) total_inst_fetched) / CYCLE_VAL);
    printf("\nUSIMM_ROBF_STALLS     \t : %lld\n", robf_stalls);
    printf("\nUSIMM_ROBN_STALLS     \t : %lld\n", robn_stalls);
    printf("\nUSIMM_WRQF_STALLS     \t : %lld\n", wrqf_stalls);
    printf("\nUSIMM_WRQN_STALLS     \t : %lld\n", wrqn_stalls);
    printf("Num reads merged: %lld\n", num_read_merge);
    printf("Num writes merged: %lld\n\n", num_write_merge);
    printf("\n\n ---- MEMORY TRAFFIC STAT ---- \n");
    printf("\nUSIMM_MEM_READS       \t : %lld\n", read_traffic);
    printf("\nUSIMM_MEM_WRITES_DIRTY      \t : %lld\n", write_traffic_dirty);
    printf("\nUSIMM_MEM_WRITES_CLEAN      \t : %lld\n", write_traffic_clean);
    printf("\nUSIMM_MEM_TRAFFIC     \t : %lld\n", read_traffic + write_traffic_dirty + write_traffic_clean);


    printf("\n\n ---- Per-Core Stat ---- \n");
    //Per core stat
    for (numc = 0; numc < NUMCORES; numc++) {
        printf("\nCORE%d_INST            \t : %lld\n", numc, fetched[numc]);
        printf("\nCORE%d_CYCLES          \t : %lld\n", numc, CYCLE_VAL);
        printf("\nCORE%d_IPC             \t : %f\n", numc, ((double) fetched[numc]) / CYCLE_VAL);
    }

    /* Print all other memory system stats. */
    scheduler_stats();
    //print_cache_stats(L3Cache);


    printf("L3Cache statistics\n");
    print_cache_stats(L3Cache);

    print_stats();
    os_print_stats(os);

    /*Print Cycle Stats*/
    for (int c = 0; c < NUM_CHANNELS; c++)
        for (int r = 0; r < NUM_RANKS; r++)
            calculate_power(c, r, 0, chips_per_rank);

    printf("\n#-------------------------------------- Power Stats ----------------------------------------------\n");
    printf("Note:  1. termRoth/termWoth is the power dissipated in the ODT resistors when Read/Writes terminate \n");
    printf("          in other ranks on the same channel\n");
    printf("#-------------------------------------------------------------------------------------------------\n\n");


    /*Print Power Stats*/
    float total_system_power = 0;
    for (int c = 0; c < NUM_CHANNELS; c++)
        for (int r = 0; r < NUM_RANKS; r++)
            total_system_power += calculate_power(c, r, 1, chips_per_rank);

    printf("\n#-------------------------------------------------------------------------------------------------\n");
    if (NUM_CHANNELS == 4) {  /* Assuming that this is 4channel.cfg  */
        printf("Total memory system power = %f W\n", total_system_power / 1000);
        printf("Miscellaneous system power = 40 W  # Processor uncore power, disk, I/O, cooling, etc.\n");
        printf("Processor core power = %f W  # Assuming that each core consumes 10 W when running\n", core_power);
        printf("Total system power = %f W # Sum of the previous three lines\n",
               40 + core_power + total_system_power / 1000);
        printf("Energy Delay product (EDP) = %2.9f J.s\n",
               (40 + core_power + total_system_power / 1000) * (float) ((double) CYCLE_VAL / (double) 3200000000) *
               (float) ((double) CYCLE_VAL / (double) 3200000000));
    } else {  /* Assuming that this is 1channel.cfg  */
        printf("Total memory system power = %f W\n", total_system_power / 1000);
        printf("Miscellaneous system power = 10 W  # Processor uncore power, disk, I/O, cooling, etc.\n");  /* The total 40 W misc power will be split across 4 channels, only 1 of which is being considered in the 1-channel experiment. */
        printf("Processor core power = %f W  # Assuming that each core consumes 5 W\n",
               core_power);  /* Assuming that the cores are more lightweight. */
        printf("Total system power = %f W # Sum of the previous three lines\n",
               10 + core_power + total_system_power / 1000);
        printf("Energy Delay product (EDP) = %2.9f J.s\n",
               (10 + core_power + total_system_power / 1000) * (float) ((double) CYCLE_VAL / (double) 3200000000) *
               (float) ((double) CYCLE_VAL / (double) 3200000000));
    }
    printf("\n#--------------------------------- result of read&write count ------------------------------------\n");

    //printnode();
    destroyTable(cl_table);
    printf("REFETCH NEW %lld %lld\n", refetched_cacheline_cnt, new_cacheline_cnt);
    printf("DIRTY CLEAN %lld %lld\n", victim_dirty, victim_clean);
    printf("AVERAGE_HIT %.2f %.2f\n", (double) access_dirty / (double) victim_dirty,
           (double) access_clean / (double) victim_clean);
    printf("TT_R_WD_WC %lld\t%lld\t%lld\t%lld\n", read_traffic + write_traffic_dirty + write_traffic_clean,
           read_traffic, write_traffic_dirty, write_traffic_clean);

    printf("MISS_TYPE %lld\t%lld\t%lld\t%lld\n", llc_read_miss, llc_write_miss, llc_read_access, llc_write_access);

    printf("LLC_READ_MISS： %lld\n", llc_read_miss);
    printf("LLC_WRITE_MISS： %lld\n", llc_write_miss);
    printf("LLC_READ_ACCESS： %lld\n", llc_read_access);
    printf("LLC_WRITE_ACCESS： %lld\n", llc_write_access);

    printf("LLC_FAST_WRITE\t : %lld\n", fast_write);
    printf("LLC_SLOW_WRITE\t : %lld\n", slow_write);
    printf("LLC_FAST_WRITE_PERCENTAGE \t : %.2f\n", (double)fast_write/((double)fast_write+(double)slow_write) );
    printf("LLC_SLOW_WRITE_PERCENTAGE \t : %.2f\n", (double)slow_write/((double)fast_write+(double)slow_write));

    printf("LLC_FILL_TO_VALID_BLOCK\t : %lld\n", fill_invalidation);
    printf("LLC_FILL_TO_INVALID_BLOCK\t : %lld\n", fill_in_invalided_block);
    printf("LLC_WRITE_BACK_HIT_INVALIDATION\t : %lld\n", write_back_hit_invalidation);
    printf("LLC_FILL_INVALIDATION_PERCENTAGE \t : %.2f\n", (double)fill_invalidation/((double)fill_invalidation+(double)write_back_hit_invalidation));
    printf("LLC_WRITE_BACK_HIT_INVALIDATION_PERCENTAGE \t : %.2f\n", (double)write_back_hit_invalidation/((double)fill_invalidation+(double)write_back_hit_invalidation));

    printf("DIRTY_DEADBLOCK_INVALIDATION\t : %lld\n", dirty_deadblock_invalidation);
    printf("CLEAN_DEADBLOCK_INVALIDATION\t : %lld\n", clean_dead_block_invalidation);
    printf("DIRTY_DEADBLOCK_INVALIDATION_PERCENTAGE \t : %.2f\n", (double)dirty_deadblock_invalidation/((double)dirty_deadblock_invalidation+(double)clean_dead_block_invalidation));
    printf("CLEAN_DEADBLOCK_INVALIDATION_PERCENTAGE \t : %.2f\n", (double)clean_dead_block_invalidation/((double)dirty_deadblock_invalidation+(double)clean_dead_block_invalidation));

    printf("DIRTY_DEADBLOCK_WRITE_BACK\t : %lld\n", dirty_deadblock_write_back);
    printf("DIRTY_VICTIM_WRITE_BACK\t : %lld\n", dirty_victim_write_back);
    printf("DIRTY_DEADBLOCK_WRITE_BACK_PERCENTAGE \t : %.2f\n", (double)dirty_deadblock_write_back/((double)dirty_deadblock_write_back+(double)dirty_victim_write_back));
    printf("DIRTY_VICTIM_WRITE_BACK_PERCENTAGE \t : %.2f\n", (double)dirty_victim_write_back/((double)dirty_deadblock_write_back+(double)dirty_victim_write_back));

    printf("PERCENTAGE_OF_FILL_IN_PREINVALIDED_BLOCK \t : %.2f\n", (double)fill_in_invalided_block/((double)fill_invalidation+(double)write_back_hit_invalidation));

    //check_table();
    printf("PP_CHANGE %lld\t%lld\n", pp_changed, pp_unchanged);
    printf("SPECIFIC_pp_change %lld\t%lld\t%lld\t%lld\t%lld\t%lld\n", n_to_w, n_to_r, w_to_n, w_to_r, r_to_n, r_to_w);

    printf("AVERAGE_ACCESS_TIME %f\n", (double) access_sum / (double) num_ret);


    //Freeing all used memory
    for (int i = 0; i < NUMCORES; i++) {
        //fclose(tif[i]);
        gzclose(tif[i]);
        free(ROB[i].comptime);
        free(ROB[i].mem_address);
        free(ROB[i].instrpc);
        free(ROB[i].optype);
    }
    free(tif);
    free(ROB);
    free(committed);
    free(fetched);
    free(ff_fetched);
    free(ff_done);
    free(time_done);
    free(nonmemops);
    free(opertype);
    free(addr);
    free(instrpc);
    free(compressedSize1Line);
    free(compressedSize2Line);
    free(L3Cache);
    free(prefixtable);

    return 0;
}
