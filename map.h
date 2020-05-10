#ifndef MAP_H
#define MAP_H
#include "global_types.h"

typedef struct ht_element{
    long long int rd_cnt;
    long long int wr_cnt;
	long long int access_count;
    int compressedSize;
	int rd_intensive;
	int wr_intensive;
	long long int pp_changed;
	long long int pp_unchanged;
	int is_metadata;
}Element;


struct ht_node{
        uns64 key;
        Element* val;
        struct ht_node *next;
};

typedef struct hashtable{
        int size;
        uns64 num_elements;
        struct ht_node **list;
            
}Hashtable;


Hashtable *createTable(int size);
void destroyTable(struct hashtable *t);
int hashCode(Hashtable *t,uns64 key);
int insert(Hashtable *t,uns64 key,Element* val);
Element* lookup(Hashtable*t,uns64 key);
Element* createElement();

#endif
