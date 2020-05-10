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

//From Intel Skylake 6660U
#define L3_LATENCY 30
#define DECOMPRESSION_LATENCY 1

#define MAXTRACELINESIZE 128

#define HASHTABLE_SIZE 1024*1024

//Set this for table length of randomizer
#define MAX_TABLE_LEN 1024 
char ff_done_global=0;
long long int llc_friendly_cacheline=0;
long long int llc_unfriendly_cacheline=0;
long long int refetched_cacheline_cnt=0;
long long int new_cacheline_cnt=0;
long long int victim_dirty=0;
long long int victim_clean=0;


typedef struct element{
    long long int addr;
    long long int rd_cnt;
    long long int wr_cnt;
	long long int access_count;
	long long int rd_intensive;
	long long int wr_intensive;
	long long int pp_changed;
	long long int pp_unchanged;
}element;

typedef struct node{
    element data;
    struct node* left_link;
    struct node* right_link;
}node;

typedef node* nodePointer;
nodePointer head=NULL;

Hashtable *cl_table = NULL;

Element* insertnode(uns64 addr, char optype, int compressedSize)
{
    Element* elm=lookup(cl_table, addr);

    //lookup the key
    //if find the key, update the rd_cnt and wr_cnt
    if(elm)
    {
        if(optype=='R'){
            elm->rd_cnt+=1;
        }
        else if(optype=='W'){
            elm->wr_cnt+=1;
        }
        elm->compressedSize=compressedSize;
        return elm;
    }
    else //if not, insert the key and value
    {
        Element* new_elm=createElement();
         if(optype=='R'){
            new_elm->rd_cnt+=1;
        }
        else if(optype=='W'){
            new_elm->wr_cnt+=1;
        }
        new_elm->compressedSize=compressedSize;
		new_elm->rd_intensive=0;
		new_elm->wr_intensive=0;
		new_elm->pp_changed=0;
		new_elm->pp_unchanged=0;
		new_elm->is_metadata=0;

        insert(cl_table,addr,new_elm);
        return NULL;
    }
}


void printnode(){
 
    for(int i=0; i<cl_table->size;i++)
    {
        struct ht_node *list = cl_table->list[i];
        struct ht_node *temp = list;
        while(temp)
        {
            printf("CL_ACCESS %lld %lld %lld\n",temp->key,temp->val->rd_cnt,temp->val->wr_cnt);
			printf("PP_changed PP_unchanged %lld %lld\n",temp->val->pp_changed,temp->val->pp_unchanged);
            temp=temp->next;
        }
    }
    printf("Total number of elements installed in the hash table: %lld\n",cl_table->num_elements);
}


nodePointer findNode(nodePointer head, element item){
	nodePointer tmp=NULL;
	while(head){
		if(head->data.addr == item.addr)
			return head;

		tmp=findNode(head->left_link,item);
		if(tmp!=NULL)
			return tmp;

		tmp=findNode(head->right_link,item);
		if(tmp!=NULL)
			return tmp;
		break;
	}
	return NULL;
}

// The most important variable, CYCLE_VAL keeps a global counter for all elements to tick.
long long int CYCLE_VAL=0;

long long int get_current_cycle()
{
    return CYCLE_VAL;
}

//The ROB to simulate OOO behaviour
struct robstructure *ROB;

//A big number that is used to avoid long stalls and let the ROB proceed ahead
long long int BIGNUM = 1000000;

//Stall counters for Statistics
long long int robf_stalls=0;
long long int wrqf_stalls=0;
long long int robn_stalls=0;
long long int wrqn_stalls=0;

//Logging counters for Statistics of time done and core power
long long int *time_done;
long long int total_time_done;
float core_power=0;

long long int page_counter=0;
//Set this variable to notify if the sims are done
int expt_done=0;  

/*Indicates how many config params exist and their types*/
int config_param=0;
gzFile *tif=NULL;  /* The handles to the trace input files. */
FILE *config_file=NULL;
FILE *vi_file=NULL;
int *prefixtable; /* For (multi-threaded) MT workloads only */


uns64 sram_install_cnt=0;
uns64 sram_write_cnt=0;
uns64 sttram_install_cnt=0;
uns64 sttram_write_cnt=0;

uns64 llc_install_cnt=0;
uns64 llc_bypass_cnt=0;
uns64 llc_bypass_read=0;
uns64 llc_bypass_write=0;

int main(int argc, char * argv[])
{
    
    printf("---------------------------------------------\n");
    printf("-- USIMM: the Utah SImulated Memory Module --\n");
    printf("--              Version: 1.3               --\n");
    printf("---------------------------------------------\n");
    
    int numc=0;
    int num_ret=0;
    int num_fetch=0;
    int num_done=0;
    //int numch=0;
    int writeqfull=0;
    int fnstart;
    int currMTapp;
    long long int maxtd;
    int maxcr;
    long long int temp_inst_cntr=0;
    long long int temp_inst_cntr2=0;
    char newstr[MAXTRACELINESIZE];
    long long int *nonmemops;
    char *opertype;
    long long int *addr;
	long long int *llc_available_cycle;
    
    long long int *instrpc;
    int *compressedSize1Line;
    int *compressedSize2Line;
    
    //Memory Statistics
    unsigned long long int read_traffic=0;
    unsigned long long int write_traffic_dirty=0;
	unsigned long long int write_traffic_clean=0;
    
    int chips_per_rank=-1;
    long long int total_inst_fetched = 0;
    int fragments=1;
    
    //OS parameters
    unsigned long long int os_pages = 2097152; //2 Million pages
    OS_PAGESIZE = 4096; // 4KB is the size of each page
    OS_NUM_RND_TRIES = 4; // Random page mapping
    OS *os;
    
    //Cache Parameters
    MCache *L3Cache; //The L3 Cache model
    MCache *L3Cache_SRAM; //The L3 Cache model
    MCache *L3Cache_STTRAM; //The L3 Cache model
    CACHE_SIZE = 4; // 4 MB
    CACHE_SIZE_SRAM = 4;  //todo : need to update so that SRAM and STTRAM have difference cache size
    CACHE_SIZE_STTRAM = 12;  //todo 
    CACHE_WAYS = 8; // 8 Ways
    CACHE_REPL = 0; // Default is LRU
	CACHE_SRAM_WAYS= 8;
	CACHE_STTRAM_WAYS= 8;
    COMPRESSION_ENABLED = 0; // 0 by default (not enabled)
    COMPRESSION_MODE = 0; // 0 by default (no mode)
    MAX_BLOCKS_PER_LINE = 4; // 4 compressed blocks by default
    IDEAL_COMPRESSOR_ENABLED = 0; //0 by default (not enabled). if enabled all cacheline will be compressed to 8B.
    FASTMEM_ENABLED = 0; //0 by default(not enabled). in enabled, the latency of all memory requests is 1.
	CACHE_BANKS=32;
	L3_LATENCY_READ=30;
	L3_LATENCY_WRITE=60;
	L3_LATENCY_WRITE_SRAM=30;
	L3_LATENCY_WRITE_STTRAM=60;
	ME_mode=0;
	base_hybrid_mode=0;
	ME_bypassing_mode=0;
    
    //To keep track of how much is done
    unsigned long long int inst_comp=0;
    /* Initialization code. */
    printf("Initializing.\n");
    // added by choucc
    // add an argument for refreshing selection
    if (argc < 4) {
        fprintf(stderr,"Need at least one input configuration file and one trace file as argument.  Quitting.\n");
        return -3;
    }
    
    config_param = atoi(argv[1]);
    
    
    config_file = fopen(argv[2], "r");
    if (!config_file) {
        fprintf(stderr,"Missing system configuration file.  Quitting. \n");
        return -4;
    }
    
    NUMCORES = argc-3;
    
 	read_config_file(config_file);
   
    ROB = (struct robstructure *)calloc(sizeof(struct robstructure),NUMCORES);
    tif = (gzFile *)malloc(sizeof(gzFile)*NUMCORES);
    committed = (long long int *)calloc(sizeof(long long int),NUMCORES);
	fetched = (long long int *)calloc(sizeof(long long int),NUMCORES);
    ff_fetched = (long long int *)calloc(sizeof(long long int),NUMCORES);
	ff_done = (char*)calloc(sizeof(char),NUMCORES);
    time_done = (long long int *)calloc(sizeof(long long int),NUMCORES);
    nonmemops = (long long int *)calloc(sizeof(long long int),NUMCORES);
    opertype = (char *)calloc(sizeof(char),NUMCORES);
    addr = (long long int *)calloc(sizeof(long long int),NUMCORES);
    instrpc = (long long int *)calloc(sizeof(long long int),NUMCORES);
    compressedSize1Line = (int *)calloc(sizeof(int),NUMCORES);
    compressedSize2Line = (int *)calloc(sizeof(int),NUMCORES);
	llc_available_cycle = (long long int *)malloc(sizeof(long long int)*CACHE_BANKS);
	for(int i=0;i<CACHE_BANKS;i++)
		llc_available_cycle[i]=0;
    
    prefixtable = (int *)malloc(sizeof(int)*NUMCORES);
    currMTapp = -1;
    
    // add an argument for selecting input traces
    for (numc=0; numc < NUMCORES; numc++) {
        tif[numc] = gzopen(argv[numc+3], "r");
        if (!tif[numc]) {
            fprintf(stderr,"Missing input trace file %d.  Quitting. \n",numc);
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
        for (fnstart = strlen(argv[numc+3]) ; fnstart >= 0; fnstart--) {
            if (argv[numc+3][fnstart] == '/') {
                break;
            }
        }
        fnstart++;  /* fnstart is either the letter after the last / or the 0th letter. */
        
        if ((strlen(argv[numc+3])-fnstart) > 2) {
            if ((argv[numc+3][fnstart+0] == 'M') && (argv[numc+3][fnstart+1] == 'T')) {
                if (argv[numc+3][fnstart+2] == '0') {
                    currMTapp = numc;
                }
                else {
                    if (currMTapp < 0) {
                        fprintf(stderr,"Poor set of input parameters.  Input file %s starts with \"MT\", but there is no preceding input file starting with \"MT0\".  Quitting.\n", argv[numc+3]);
                        return -6;
                    }
                    else
                        prefixtable[numc] = currMTapp;
                }
            }
        }
        printf("Core %d: Input trace file %s : Addresses will have prefix %d\n", numc, argv[numc+3], prefixtable[numc]);
        
        committed[numc]=0;
        fetched[numc]=0;
		ff_done[numc]=0;
        time_done[numc]=0;
        ROB[numc].head=0;
        ROB[numc].tail=0;
        ROB[numc].inflight=0;
        ROB[numc].tracedone=0;
    }
    
    
    
    vi_file = fopen("../input/8Gb_x8.vi", "r");
    chips_per_rank= 8;
    printf("Reading vi file: 8Gb_x8.vi\t\n%d Chips per Rank\n",chips_per_rank);
    
    if (!vi_file) {
        fprintf(stderr,"Missing DRAM chip parameter file.  Quitting. \n");
        return -5;
    }
    
    
    assert((log_base2(NUM_CHANNELS) + log_base2(NUM_RANKS) + log_base2(NUM_BANKS) + log_base2(NUM_ROWS) + log_base2(NUM_COLUMNS) + log_base2(CACHE_LINE_SIZE)) == ADDRESS_BITS );
    read_config_file(vi_file);
    fragments=1;
    T_RFC=T_RFC/fragments;
    
    printf("Fragments: %d of length %d\n",fragments, T_RFC);
    
    print_params();
    
    for(int i=0; i<NUMCORES; i++)
    {
        ROB[i].comptime = (long long int*)calloc(sizeof(long long int),ROBSIZE);
        ROB[i].mem_address = (long long int*)calloc(sizeof(long long int),ROBSIZE);
        ROB[i].instrpc = (long long int*)calloc(sizeof(long long int),ROBSIZE);
        ROB[i].optype = (int*)calloc(sizeof(int),ROBSIZE);
    }
    long long int cache_size = CACHE_SIZE*1024*1024;//LLC Size (SHARED)
    uns assoc = CACHE_WAYS;
    uns assoc_sram = CACHE_SRAM_WAYS;
    uns assoc_sttram = CACHE_STTRAM_WAYS;
    uns block_size = 64;
    uns sets = cache_size/(assoc*block_size);
    uns repl = CACHE_REPL;

    long long int cache_sram_size = CACHE_SIZE_SRAM*1024*1024;//LLC Size (SHARED)
    uns sets_sram = cache_sram_size/(assoc_sram*block_size);
    
    long long int cache_sttram_size = CACHE_SIZE_STTRAM*1024*1024;//LLC Size (SHARED)
    uns sets_sttram = cache_sttram_size/(assoc_sttram*block_size);
    
    L3Cache = (MCache*) calloc(1, sizeof(MCache));
    L3Cache_SRAM = (MCache*) calloc(1, sizeof(MCache));
    L3Cache_STTRAM = (MCache*) calloc(1, sizeof(MCache));
    init_cache(L3Cache, sets, assoc, repl, block_size, COMPRESSION_ENABLED);
    init_cache(L3Cache_SRAM, sets_sram, assoc_sram, repl, block_size, COMPRESSION_ENABLED);
    init_cache(L3Cache_STTRAM, sets_sttram, assoc_sttram, repl, block_size, COMPRESSION_ENABLED);
    
    os_pages =((unsigned long long)1<<ADDRESS_BITS)/OS_PAGESIZE;
    
    printf("os_pages: %lld\n",os_pages);
    os = os_new(os_pages,NUMCORES);
    
    init_memory_controller_vars();
    init_scheduler_vars();


    cl_table = createTable(HASHTABLE_SIZE);
    
    /* --------------- Done initializing ------------------ */
    
    /* Must start by reading one line of each trace file. */
	printf("Start Fast Forwarding!!\n");
	fflush(stdout);
    temp_inst_cntr2=0;

	while(ff_done_global==0)
	{
		for(numc=0; numc<NUMCORES; numc++)
		{
			if (ff_done[numc]==0&&gzgets(tif[numc],newstr,MAXTRACELINESIZE)) {
				inst_comp++;
				if (sscanf(newstr,"%lld %c",&nonmemops[numc],&opertype[numc]) > 0) {
					if (opertype[numc] == 'R') {
						if (sscanf(newstr,"%lld %c %llx %d %d",&nonmemops[numc],&opertype[numc],&addr[numc],&compressedSize1Line[numc],&compressedSize2Line[numc]) < 1) {
							fprintf(stderr,"[1]Panic.  Poor trace format.%s\n",newstr);
							return -4;
						}
					}
					else {
						if (opertype[numc] == 'W') {
							if (sscanf(newstr,"%lld %c %llx %d %d",&nonmemops[numc],&opertype[numc],&addr[numc],&compressedSize1Line[numc],&compressedSize2Line[numc]) < 1) {
								fprintf(stderr,"[2]Panic.  Poor trace format.%s\n",newstr);
								return -3;
							}
						}
						else {
							fprintf(stderr,"[3]Panic.  Poor trace format.%s\n",newstr);
							return -2;
						}
					}
                         
				}
				else {
					fprintf(stderr,"[4]Panic.  Poor trace format. %s\n",newstr);
					return -1;
				}
				//Insert the OS here to do a Virtual to Physical Translation since we are using a virtual address trace
				Addr phy_lineaddr=os_v2p_lineaddr(os,addr[numc]>>L3Cache->lineoffset,numc);
				addr[numc]=phy_lineaddr<<(L3Cache->lineoffset);  //convert the line address to byte address

				//fill cache during the fast-forwording
				int L3Hit = isHit(L3Cache, addr[numc], false, compressedSize1Line[numc]); //addr[numc] is byte address here
                
                MCache_Entry victim;
                
				if(L3Hit==0)
                     victim=install(L3Cache, addr[numc], instrpc[numc], false,compressedSize1Line[numc]);  //addr[numc] is byte address here
                     
                
                uns64 addr_tmp=addr[numc]>>6;
                insertnode(addr_tmp, opertype[numc],compressedSize1Line[numc]);


                //update access history in the hash table
                if(L3Hit==1)
                {
                    Element* elm=lookup(cl_table, addr_tmp);
					if(elm)
                    	elm->access_count++;
			    }
                else if(victim.valid)
                {
                    Element* elm=lookup(cl_table, victim.tag);
					if(elm)
                    	elm->access_count=victim.access_count;
                }
            }
			else {
                  //gzrewind(tif[numc]);
				  //printf("rewind for core %d\n", numc);
			}


			ff_fetched[numc]++;
            ff_fetched[numc]+=nonmemops[numc];


			if(ff_done[numc]==0 && ff_fetched[numc]>FF_INST)
			{
				printf("[FF Phase] Core:%d, ff done, fetched instruction:%lld\n",numc,ff_fetched[numc]);
				fflush(stdout);

				ff_done[numc]=1;
				int ff_done_cnt=0;
				for(int i=0;i<NUMCORES;i++)
					if(ff_done[i]==1)
						ff_done_cnt++;

				if(ff_done_cnt==NUMCORES)
					ff_done_global=1;
				else
					ff_done_global=0;
			}
		}
    }

    //print_cache_stats(L3Cache);
    
    printf("Starting simulation.\n");
	fflush(stdout);
    temp_inst_cntr2=0;

    while (!expt_done) {
        
        /* For each core, retire instructions if they have finished. */
        for (numc = 0; numc < NUMCORES; numc++) {
            num_ret = 0;
            while ((num_ret < MAX_RETIRE) && ROB[numc].inflight) {
                /* Keep retiring until retire width is consumed or ROB is empty. */
                if (ROB[numc].comptime[ROB[numc].head] < CYCLE_VAL) {
                    /* Keep retiring instructions if they are done. */
                    ROB[numc].head = (ROB[numc].head + 1) % ROBSIZE;
                    ROB[numc].inflight--;
                    committed[numc]++;
                    num_ret++;
                }
                else  /* Instruction not complete.  Stop retirement for this core. */
                    break;
            }  /* End of while loop that is retiring instruction for one core. */
        }  /* End of for loop that is retiring instructions for all cores. */
        
        
        if(CYCLE_VAL%PROCESSOR_CLK_MULTIPLIER == 0)
        {
            /* Execute function to find ready instructions. */
            update_memory();
            
            /* Execute user-provided function to select ready instructions for issue. */
            /* Based on this selection, update DRAM data structures and set
             instruction completion times. */
            for(int c=0; c < NUM_CHANNELS; c++)
            {
                schedule(c);
                gather_stats(c);
            }
        }
        
        /* For each core, bring in new instructions from the trace file to
         fill up the ROB. */
        writeqfull =0;
        for(int c=0; c<NUM_CHANNELS; c++){
            if(write_queue_length[c] >= WQ_CAPACITY)
            {
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
                        ROB[numc].comptime[ROB[numc].tail] = CYCLE_VAL+PIPELINEDEPTH;
                        nonmemops[numc]--;
                        ROB[numc].tail = (ROB[numc].tail +1) % ROBSIZE;
                        ROB[numc].inflight++;
                        fetched[numc]++;
                        temp_inst_cntr++;
                        num_fetch++;
                    }
                    else{ /* Done consuming non-memory-ops.  Must now consume the memory rd or wr. */
						
						int banknum =  (addr[numc]>>6)%CACHE_BANKS;
						int installed_sram=0;
						
						if(llc_available_cycle[banknum]> CYCLE_VAL)
							break;
						
                        //lookup the cache line in the hash table
                        Element* elm=insertnode(addr[numc]>>6,opertype[numc],compressedSize1Line[numc]);
						
                        if (opertype[numc] == 'R') 
                        {
                            ROB[numc].mem_address[ROB[numc].tail] = addr[numc];
                            ROB[numc].optype[ROB[numc].tail] = opertype[numc];
                            ROB[numc].comptime[ROB[numc].tail] = CYCLE_VAL + BIGNUM;
                            ROB[numc].instrpc[ROB[numc].tail] = instrpc[numc];
                            long long int wb_addr = 0;
                            //long long int wb_inst_addr = 0;
                            MCache_Entry victim;
                            victim.valid=0;
                            int L3Hit = 0;
                            
							if(ME_mode || base_hybrid_mode)
							{
								L3Hit = isHit(L3Cache_SRAM, addr[numc], false, compressedSize1Line[numc]); //addr[numc] is byte address here
								if(!L3Hit)
									L3Hit = isHit(L3Cache_STTRAM, addr[numc], false, compressedSize1Line[numc]); //addr[numc] is byte address here
							}
							else
								L3Hit = isHit(L3Cache, addr[numc], false, compressedSize1Line[numc]); //addr[numc] is byte address here
                           
							if(L3Hit==0)
							{
                                if(elm)
                                    refetched_cacheline_cnt++;
                                else
                                    new_cacheline_cnt++;

								if(ME_mode)
								{
									int is_write_intensive=0;
									int is_llc_friendly=1;

									//determine whether this cache block is write intensive or read intensive
									if(elm)
									{
										if((float)elm->wr_cnt/((float)elm->wr_cnt+(float)elm->rd_cnt)>0.1)
											is_write_intensive=1;

										if(elm->access_count==0)
											is_llc_friendly=0;
                                        else
                                            is_llc_friendly=1;
									}


									//todo: need to check implementation of ME bypassing mode
									/*if(ME_bypassing_mode)
									{
										if((is_llc_friendly && elm && elm->is_metadata==1) || compressedSize1Line[numc]>61){
											if(is_write_intensive)
											{
												victim=install(L3Cache_SRAM, addr[numc], instrpc[numc], false,compressedSize1Line[numc]);  //addr[numc] is byte address here
												installed_sram=1;
											}
											else
											{
												victim=install(L3Cache_STTRAM, addr[numc], instrpc[numc], false,compressedSize1Line[numc]);  //addr[numc] is byte address here
												installed_sram=0;
											}
                                            llc_install_cnt++;
										}
                                        else{
											llc_bypass_read++;
                                            llc_bypass_cnt++;
										}
									}
									else*/
									{
                                        if(!elm) //if the cache block is new
                                        {
                                            victim=install(L3Cache_STTRAM, addr[numc], instrpc[numc], false,compressedSize1Line[numc]);  //addr[numc] is byte address here
                                            installed_sram=0; // install it into the sttram because the current miss is read miss.
                                        }
                                        else if(elm->is_metadata==0) // this cache block is refetched cache line, but it does not have metadata
                                        {
                                            victim=install(L3Cache_STTRAM, addr[numc], instrpc[numc], false,compressedSize1Line[numc]);  //addr[numc] is byte address here
                                            installed_sram=0; //install it into the sttram because the current miss is read miss.
                                        }
                                        else if(elm->is_metadata==1) // this cache block is refetched cache block, and it does not have metadata
                                        {
											if(is_write_intensive)
											{
												victim=install(L3Cache_SRAM, addr[numc], instrpc[numc], false,compressedSize1Line[numc]);  //addr[numc] is byte address here
												installed_sram=1;
											}
											else
											{
												victim=install(L3Cache_STTRAM, addr[numc], instrpc[numc], false,compressedSize1Line[numc]);  //addr[numc] is byte address here
												installed_sram=0;
											}
									    }
								    }
                                }
								else if(base_hybrid_mode)
                                {
									victim=install(L3Cache_STTRAM, addr[numc], instrpc[numc], false,compressedSize1Line[numc]);  //addr[numc] is byte address here
                                    installed_sram=0; //install new cache block in sttram
                                }
								else
                                {
									victim=install(L3Cache, addr[numc], instrpc[numc], false,compressedSize1Line[numc]);  //addr[numc] is byte address here
                                    installed_sram=1; //install new cache block in sram
                                }
								
                                if(victim.valid)
                                {
                                    if(victim.access_count>0)
                                        llc_friendly_cacheline++;
                                    else
                                        llc_unfriendly_cacheline++;
									
                                
                                    //record the number of access count
                                    Element * elm_tmp=lookup(cl_table, victim.tag);
                                    elm_tmp->access_count=victim.access_count;

									if(victim.dirty){
										victim_dirty++;
										if(victim.)
										elm_tmp->is_metadata=1;
									}else{
										victim_clean++;
									}

									//check the intensive property of cachelines
									if((float)elm_tmp->wr_cnt/((float)elm_tmp->wr_cnt+(float)elm_tmp->rd_cnt)>0.1){
										if(elm_tmp->wr_intensive==0 && elm_tmp->rd_intensive==0)
											elm_tmp->wr_intensive=1;
										else if(elm_tmp->wr_intensive==0 && elm_tmp->rd_intensive==1){
											elm_tmp->pp_changed++;
											elm_tmp->rd_intensive=0;
											elm_tmp->wr_intensive=1;
										}
										else if(elm_tmp->wr_intensive==1 && elm_tmp->rd_intensive==0)
											elm_tmp->pp_unchanged++;
									}
									else{
										if(elm_tmp->rd_intensive==0 && elm_tmp->wr_intensive==0)
											elm_tmp->rd_intensive=1;
										else if(elm_tmp->rd_intensive==0 && elm_tmp->wr_intensive==1){
											elm_tmp->pp_changed++;
											elm_tmp->wr_intensive=0;
											elm_tmp->rd_intensive=1;
										}
										else if(elm_tmp->rd_intensive==1 && elm_tmp->wr_intensive==0)
											elm_tmp->pp_unchanged++;
									}
									elm_tmp->wr_cnt=0;
									elm_tmp->rd_cnt=0;

                                    
                                    int write_latency=0;
								    if(installed_sram){
									    write_latency=L3_LATENCY_WRITE_SRAM;
                                        sram_install_cnt++;
                                    }
								    else
                                    {
									    write_latency=L3_LATENCY_WRITE_STTRAM;
                                        sttram_install_cnt++;
                                    }

				    		    	llc_available_cycle[banknum]=CYCLE_VAL+write_latency;
	
                                }
							}
                            
                            // Check to see if the read is for buffered data in write queue -
                            // return constant latency if match in WQ
                            // add in read queue otherwise
                            int lat = read_matches_write_or_read_queue(addr[numc]);
                            
                            if(FASTMEM_ENABLED ==1)
                                lat = 1;
							
							if(L3Hit==1){
				    				llc_available_cycle[banknum]=CYCLE_VAL+L3_LATENCY_READ;
                                    ROB[numc].comptime[ROB[numc].tail] = CYCLE_VAL+L3_LATENCY_READ+PIPELINEDEPTH;
                            }else{
                                //evict all compressed cache lines in the victim data block
								 if(victim.valid){
								 	Element * elm_tmp=lookup(cl_table, victim.tag);
                                 	elm_tmp->access_count=victim.access_count;
								 
	                         
                                 	if(victim.dirty)
                                 	{
                                    	write_traffic_dirty++;
                                     	wb_addr = victim.tag << L3Cache->lineoffset;
                                     	insert_write(wb_addr, CYCLE_VAL, numc, ROB[numc].tail);
                                 	}
                              	 	if(!victim.dirty && (victim.access_count>0) && !elm_tmp->is_metadata)
                                 	{
										elm_tmp->is_metadata=1;
                                     	write_traffic_clean++;
                                     	wb_addr = victim.tag << L3Cache->lineoffset;
                                     	insert_write(wb_addr, CYCLE_VAL, numc, ROB[numc].tail);
                                	 }
								 }
                                // Check to see if the read is for buffered data in write queue -
                                // return constant latency if match in WQ
                                // add in read queue otherwise
                                if(lat) {
                                    if(FASTMEM_ENABLED){
                                        read_traffic++;
                                    }
                                    ROB[numc].comptime[ROB[numc].tail] = CYCLE_VAL+lat+PIPELINEDEPTH;
                                }
                                else {
                                    if(!FASTMEM_ENABLED){
                                        read_traffic++;
                                    }
                                    insert_read(addr[numc], CYCLE_VAL, numc, ROB[numc].tail, instrpc[numc]);
                                }
                            }
                        }
                        else{  /* This must be a 'W'.  We are confirming that while reading the trace. */
                            if (opertype[numc] == 'W') 
                            {
                                ROB[numc].mem_address[ROB[numc].tail] = addr[numc];
                                ROB[numc].optype[ROB[numc].tail] = opertype[numc];
                                ROB[numc].comptime[ROB[numc].tail] = CYCLE_VAL+PIPELINEDEPTH;
                                /* Also, add this to the write queue. */
                                long long int wb_addr = 0;
                                MCache_Entry victim;
                                victim.valid=0;
                                
								int L3Hit=0;
                                int is_hit_sram=0;

								if(ME_mode ||base_hybrid_mode)
								{
									L3Hit = isHit(L3Cache_SRAM, addr[numc], false, compressedSize1Line[numc]); //addr[numc] is byte address here
									if(!L3Hit)
                                    {
										L3Hit = isHit(L3Cache_STTRAM, addr[numc], false, compressedSize1Line[numc]); //addr[numc] is byte address here
                                        is_hit_sram=0;    
								    }
                                    else
                                        is_hit_sram=1;
                                }
								else{
									L3Hit = isHit(L3Cache, addr[numc], false, compressedSize1Line[numc]); //addr[numc] is byte address here
                                    is_hit_sram=1;
                                }

                                
                                if(L3Hit==0)
                                {
                                   if(elm)
                                        refetched_cacheline_cnt++;
                                    else
                                        new_cacheline_cnt++;

									if(ME_mode)
									{
										int is_write_intensive=0;
										int is_llc_friendly=1;

										if(elm)
										{
											if((float)elm->wr_cnt/((float)elm->wr_cnt+(float)elm->rd_cnt)>0.1)
												is_write_intensive=1;

											if(elm->access_count==0)
												is_llc_friendly=0;
                                            else
                                                is_llc_friendly=1;
										}
										
										if(ME_bypassing_mode)
										{
											if((is_llc_friendly && elm && elm->is_metadata==1) || compressedSize1Line[numc]>61 ){
												if(is_write_intensive)
                                                {
													victim=install(L3Cache_SRAM, addr[numc], instrpc[numc], true,compressedSize1Line[numc]);  //addr[numc] is byte address here
                                                    installed_sram=1;
                                                }
												else
                                                {
													victim=install(L3Cache_STTRAM, addr[numc], instrpc[numc], true,compressedSize1Line[numc]);  //addr[numc] is byte address here
                                                    installed_sram=0;
											    }
                                                llc_install_cnt++;
										    }
                                            else{
												wb_addr = victim.tag << L3Cache->lineoffset;
                                       			if(!FASTMEM_ENABLED){
                                            	insert_write(wb_addr, CYCLE_VAL, numc, ROB[numc].tail);
												}
												llc_bypass_write++;
                                                llc_bypass_cnt++;
											}
                                        }
										else
										{
                                            if(!elm || compressedSize1Line[numc]>61)// if the cache line is new, install it into the sram because the miss is write miss
                                            {
		                                    	victim=install(L3Cache_SRAM, addr[numc], instrpc[numc], true,compressedSize1Line[numc]);  //addr[numc] is byte address here
                                                installed_sram=1;
                                            }
                                            else
                                            {
												if(is_write_intensive)
                                                {
													victim=install(L3Cache_SRAM, addr[numc], instrpc[numc], true,compressedSize1Line[numc]);  //addr[numc] is byte address here
                                                    installed_sram=1;
                                                }
												else
                                                {
													victim=install(L3Cache_STTRAM, addr[numc], instrpc[numc], true,compressedSize1Line[numc]);  //addr[numc] is byte address here
                                                    installed_sram=0;
										        }
									        }
                                        }
                                    }
									else if(base_hybrid_mode)
                                    {
										victim=install(L3Cache_SRAM, addr[numc], instrpc[numc], true,compressedSize1Line[numc]);  //addr[numc] is byte address here
                                        installed_sram=1;
                                    }
									else
										victim=install(L3Cache, addr[numc], instrpc[numc], true,compressedSize1Line[numc]);  //addr[numc] is byte address here
									

                                    if(victim.valid)
                                    {
                                        if(victim.access_count>0)
                                            llc_friendly_cacheline++;
                                        else
                                            llc_unfriendly_cacheline++;

                                    
                                        //record the number of access count
                                        Element * elm_tmp=lookup(cl_table, victim.tag);
                                        elm_tmp->access_count=victim.access_count;
	                            		
										if(victim.dirty){
											victim_dirty++;
											elm_tmp->is_metadata=1;//move to write on memory
										}else{
											victim_clean++;
										}
                                    
										if((float)elm_tmp->wr_cnt/((float)elm_tmp->wr_cnt+(float)elm_tmp->rd_cnt)>0.1){
											if(elm_tmp->wr_intensive==0 && elm_tmp->rd_intensive==0)
												elm_tmp->wr_intensive=1;
											else if(elm_tmp->wr_intensive==0 && elm_tmp->rd_intensive==1){
												elm_tmp->pp_changed++;
												elm_tmp->rd_intensive=0;
												elm_tmp->wr_intensive=1;
											}
											else if(elm_tmp->wr_intensive==1 && elm_tmp->rd_intensive==0)
												elm_tmp->pp_unchanged++;
										}
										else{
											if(elm_tmp->rd_intensive==0 && elm_tmp->wr_intensive==0)
												elm_tmp->rd_intensive=1;
											else if(elm_tmp->rd_intensive==0 && elm_tmp->wr_intensive==1){
												elm_tmp->pp_changed++;
												elm_tmp->wr_intensive=0;
												elm_tmp->rd_intensive=1;
											}
											else if(elm_tmp->rd_intensive==1 && elm_tmp->wr_intensive==0)
												elm_tmp->pp_unchanged++;
									}
									elm_tmp->wr_cnt=0;
									elm_tmp->rd_cnt=0;

     
                                        int write_latency=0;
								        if(installed_sram)
                                        {
								         	write_latency=L3_LATENCY:_WRITE_SRAM;
                                            sram_install_cnt++;
                                        }
								        else
                                        {
								         	write_latency=L3_LATENCY_WRITE_STTRAM;
                                            sttram_install_cnt++;
                                        }
    				    				llc_available_cycle[banknum]=CYCLE_VAL+write_latency;
                                     }
                                }
                                
                                if(L3Hit==1){
                                    if(is_hit_sram)
                                    {
				    				    llc_available_cycle[banknum]=CYCLE_VAL+L3_LATENCY_WRITE_SRAM;
                                        sram_write_cnt++;
                                    }
                                    else
                                    {
                                        sttram_write_cnt++;
                                        llc_available_cycle[banknum]=CYCLE_VAL+L3_LATENCY_WRITE_STTRAM;
                                    }
                                }
                                else if(L3Hit == 0){
                                    //evict all compressed cache lines in the victim data block
									if(victim.valid){	
										Element * elm_tmp=lookup(cl_table, victim.tag);
                                   	 	elm_tmp->access_count=victim.access_count;
	                         
                                    	if(victim.dirty)
                                    	{
                                        	write_traffic_dirty++;
                                        	wb_addr = victim.tag << L3Cache->lineoffset;
                                        	if(!FASTMEM_ENABLED){
                                            	insert_write(wb_addr, CYCLE_VAL, numc, ROB[numc].tail);
                                        	}
                                    	}
										if(!victim.dirty &&(victim.access_count>0)&& !elm_tmp->is_metadata)//todo: change to else if
                                    	{
											elm_tmp->is_metadata=1;
                                        	write_traffic_clean++;
                                        	wb_addr = victim.tag << L3Cache->lineoffset;
                                        	if(!FASTMEM_ENABLED){
                                            	insert_write(wb_addr, CYCLE_VAL, numc, ROB[numc].tail);
                                        	}
                                    	}
									}


                                }
                                
                                for(int c=0; c<NUM_CHANNELS; c++){
                                    if(write_queue_length[c] >= WQ_CAPACITY)
                                    {
                                        writeqfull = 1;
                                        break;
                                    }
                                }

                            }
                            else {
                                fprintf(stderr,"Panic.  Poor trace format. \n");
                                return -1;
                            }
                        }


                       	//insertnode(item,opertype[numc]);
                        ROB[numc].tail = (ROB[numc].tail +1) % ROBSIZE;
                        ROB[numc].inflight++;
                        fetched[numc]++;
                        temp_inst_cntr++;
                        num_fetch++;
                        
                        /* Done consuming one line of the trace file.  Read in the next. */
                        if (gzgets(tif[numc],newstr,MAXTRACELINESIZE)) {
                            inst_comp++;
                            if (sscanf(newstr,"%lld %c",&nonmemops[numc],&opertype[numc]) > 0) {
                                if (opertype[numc] == 'R') {
                                    if (sscanf(newstr,"%lld %c %llx %d %d",&nonmemops[numc],&opertype[numc],&addr[numc],&compressedSize1Line[numc],&compressedSize2Line[numc]) < 1) {
                                        fprintf(stderr,"Panic.  Poor trace format.\n");
                                        return -4;
                                    }
                                }
                                else {
                                    if (opertype[numc] == 'W') {
                                        if (sscanf(newstr,"%lld %c %llx %d %d",&nonmemops[numc],&opertype[numc],&addr[numc],&compressedSize1Line[numc],&compressedSize2Line[numc]) < 1) {
                                            fprintf(stderr,"Panic.  Poor trace format.\n");
                                            return -3;
                                        }
                                    }
                                    else {
                                        fprintf(stderr,"Panic.  Poor trace format.\n");
                                        return -2;
                                    }
                                }
                            }
                            else {
                                fprintf(stderr,"Panic.  Poor trace format.\n");
                                return -1;
                            }

                            //Insert the OS here to do a Virtual to Physical Translation since we are using a virtual address trace
                            Addr phy_lineaddr=os_v2p_lineaddr(os,addr[numc]>>L3Cache->lineoffset,numc);
                            addr[numc]=phy_lineaddr<<L3Cache->lineoffset;
                        }
                        else {
                        	gzrewind(tif[numc]);
                        }
                        
                    }  /* Done consuming the next rd or wr. */
                    
                } /* One iteration of the fetch while loop done. */
                if((fetched[numc] >= MAX_INST) && (time_done[numc] == 0)){
                    num_done++;
                    time_done[numc] = CYCLE_VAL;
                }
            } /* Closing brace for if(trace not done). */
        } /* End of for loop that goes through all cores. */
        
        
        if (num_done == NUMCORES) {
            expt_done = 1;
        }

        CYCLE_VAL++;  /* Advance the simulation cycle. */
        if(fetched[0]>100000000*(1+temp_inst_cntr2))
        {
            temp_inst_cntr2++;
            fflush(stdout);
            printf(".");
            printf(" - %lld00Million Instructions\n", fetched[0]/100000000);
			fflush(stdout);
            temp_inst_cntr=0;
        }
    }
    
    fprintf(stderr,"sim exit!\n");
    
    /* Code to make sure that the write queue drain time is included in
     the execution time of the thread that finishes last. */
    maxtd = time_done[0];
    maxcr = 0;
    for (numc=1; numc < NUMCORES; numc++) {
        if (time_done[numc] > maxtd) {
            maxtd = time_done[numc];
            maxcr = numc;
        }
    }
    time_done[maxcr] = CYCLE_VAL;
    
    core_power = 0;
    for (numc=0; numc < NUMCORES; numc++) {
        /* A core has peak power of 10 W in a 4-channel config.  Peak power is consumed while the thread is running, else the core is perfectly power gated. */
        core_power = core_power + (10*((float)time_done[numc]/(float)CYCLE_VAL));
    }
    if (NUM_CHANNELS == 1) {
        /* The core is more energy-efficient in our single-channel configuration. */
        core_power = core_power/2.0 ;
    }
    
	
    printf("Done with loop. Printing stats.\n");
    printf("Cycles %lld\n", CYCLE_VAL);
    total_time_done = 0;
    
    for (numc=0; numc < NUMCORES; numc++) {
        printf("Done: Core %d: Fetched %lld : Committed %lld : At time : %lld\n", numc, fetched[numc], committed[numc], time_done[numc]);
        total_time_done += time_done[numc];
        total_inst_fetched = total_inst_fetched + fetched[numc];
    }
    printf("\nUSIMM_CYCLES          \t : %lld\n",CYCLE_VAL);
    printf("\nUSIMM_INST            \t : %lld\n",total_inst_fetched);
    printf("\nUSIMM_IPC             \t : %f\n",((double)total_inst_fetched)/CYCLE_VAL);
    printf("\nUSIMM_ROBF_STALLS     \t : %lld\n",robf_stalls);
    printf("\nUSIMM_ROBN_STALLS     \t : %lld\n",robn_stalls);
    printf("\nUSIMM_WRQF_STALLS     \t : %lld\n",wrqf_stalls);
    printf("\nUSIMM_WRQN_STALLS     \t : %lld\n",wrqn_stalls);
    printf("Num reads merged: %lld\n",num_read_merge);
    printf("Num writes merged: %lld\n\n",num_write_merge);
    printf("\n\n ---- MEMORY TRAFFIC STAT ---- \n");
    printf("\nUSIMM_MEM_READS       \t : %lld\n",read_traffic);
    printf("\nUSIMM_MEM_WRITES_DIRTY      \t : %lld\n",write_traffic_dirty);
    printf("\nUSIMM_MEM_WRITES_CLEAN      \t : %lld\n",write_traffic_clean);
    printf("\nUSIMM_MEM_TRAFFIC     \t : %lld\n",read_traffic+write_traffic_dirty+write_traffic_clean);
    
    printf("\nSRAM_INSTALL_COUNT     \t : %lld\n",sram_install_cnt);
    printf("\nSTTRAM_INSTALL_COUNT     \t : %lld\n",sttram_install_cnt);
    printf("\nSRAM_WRITE_COUNT     \t : %lld\n",sram_write_cnt);
    printf("\nSTTRAM_WRITE_COUNT     \t : %lld\n",sttram_write_cnt);
    
    printf("\nLLC_BYPASS_COUNT    \t : %lld\n",llc_bypass_cnt);
	printf("\nTOBY_RD_WR %lld\t%lld\t%lld\n",llc_bypass_cnt,llc_bypass_read,llc_bypass_write);
    printf("\nLLC_INSTALL_COUNT     \t : %lld\n",llc_install_cnt);
    
    
    printf("\n\n ---- Per-Core Stat ---- \n");
    //Per core stat
    for (numc=0; numc < NUMCORES; numc++) {
        printf("\nCORE%d_INST            \t : %lld\n",numc,fetched[numc]);
        printf("\nCORE%d_CYCLES          \t : %lld\n",numc,CYCLE_VAL);
        printf("\nCORE%d_IPC             \t : %f\n",numc,((double)fetched[numc])/CYCLE_VAL);
    }
    
    /* Print all other memory system stats. */
    scheduler_stats();
    print_cache_stats(L3Cache);

    printf("L3Cache_SRAM statistics");
    print_cache_stats(L3Cache_SRAM);
    printf("L3Cache_STTRAM statistics");
    print_cache_stats(L3Cache_STTRAM);

    print_stats();
    os_print_stats(os);
    
    /*Print Cycle Stats*/
    for(int c=0; c<NUM_CHANNELS; c++)
        for(int r=0; r<NUM_RANKS ;r++)
            calculate_power(c,r,0,chips_per_rank);
    
    printf ("\n#-------------------------------------- Power Stats ----------------------------------------------\n");
    printf ("Note:  1. termRoth/termWoth is the power dissipated in the ODT resistors when Read/Writes terminate \n");
    printf ("          in other ranks on the same channel\n");
    printf ("#-------------------------------------------------------------------------------------------------\n\n");
    
    
    /*Print Power Stats*/
    float total_system_power =0;
    for(int c=0; c<NUM_CHANNELS; c++)
        for(int r=0; r<NUM_RANKS ;r++)
            total_system_power += calculate_power(c,r,1,chips_per_rank);
    
    printf ("\n#-------------------------------------------------------------------------------------------------\n");
    if (NUM_CHANNELS == 4) {  /* Assuming that this is 4channel.cfg  */
        printf ("Total memory system power = %f W\n",total_system_power/1000);
        printf("Miscellaneous system power = 40 W  # Processor uncore power, disk, I/O, cooling, etc.\n");
        printf("Processor core power = %f W  # Assuming that each core consumes 10 W when running\n",core_power);
        printf("Total system power = %f W # Sum of the previous three lines\n", 40 + core_power + total_system_power/1000);
        printf("Energy Delay product (EDP) = %2.9f J.s\n", (40 + core_power + total_system_power/1000)*(float)((double)CYCLE_VAL/(double)3200000000) * (float)((double)CYCLE_VAL/(double)3200000000));
    }
    else {  /* Assuming that this is 1channel.cfg  */
        printf ("Total memory system power = %f W\n",total_system_power/1000);
        printf("Miscellaneous system power = 10 W  # Processor uncore power, disk, I/O, cooling, etc.\n");  /* The total 40 W misc power will be split across 4 channels, only 1 of which is being considered in the 1-channel experiment. */
        printf("Processor core power = %f W  # Assuming that each core consumes 5 W\n",core_power);  /* Assuming that the cores are more lightweight. */
        printf("Total system power = %f W # Sum of the previous three lines\n", 10 + core_power + total_system_power/1000);
        printf("Energy Delay product (EDP) = %2.9f J.s\n", (10 + core_power + total_system_power/1000)*(float)((double)CYCLE_VAL/(double)3200000000) * (float)((double)CYCLE_VAL/(double)3200000000));
    }
    printf ("\n#--------------------------------- result of read&write count ------------------------------------\n");
    
    //printnode();
    destroyTable(cl_table);
	printf("FRIENDLY UNFRIENDLY %lld %lld\n",llc_friendly_cacheline,llc_unfriendly_cacheline);
	printf("REFETCH NEW %lld %lld\n",refetched_cacheline_cnt,new_cacheline_cnt);
	printf("DIRTY CLEAN %lld %lld\n",victim_dirty,victim_clean);
	printf("TT_R_WD_WC %lld\t%lld\t%lld\t%lld\n",read_traffic+write_traffic_dirty+write_traffic_clean,read_traffic,write_traffic_dirty,write_traffic_clean);
    
    //Freeing all used memory
    for(int i = 0; i < NUMCORES; i++){
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
