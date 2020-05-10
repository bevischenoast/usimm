#include <stdio.h>
#include <stdlib.h>
#include "global_types.h"
#include "map.h"


Hashtable *createTable(int size){
        Hashtable *t = (Hashtable*)malloc(sizeof(Hashtable));
        t->size = size;
        t->list = (struct ht_node**)malloc(sizeof(struct ht_node*)*size);
        int i;
        for(i=0;i<size;i++)
            t->list[i] = NULL;
        return t;
                            
}

void deleteNode(struct ht_node* node)
{
    if(node->next)
    {
        node=node->next;
        deleteNode(node);
    }
    else
    {
        free(node->val);
        free(node);
    }
    return;
}

void destroyTable(Hashtable *t)
{
     for(int i=0; i<t->size;i++)
     {
         struct ht_node *list = t->list[i];
         struct ht_node *temp = list;
         if(temp)
            deleteNode(temp);
    }
}

int hashCode(Hashtable *t,uns64 key){
        if(key<0)
              return -(key%t->size);
        return key%t->size;
            
}

//insert new element
//return 1 on success,  0 if exists
int insert(Hashtable *t,uns64 key,Element* val){
        int pos = hashCode(t,key);
        struct ht_node *list = t->list[pos];
        struct ht_node *newNode = (struct ht_node*)malloc(sizeof(struct ht_node));
        struct ht_node *temp = list;
        while(temp){
                if(temp->key==key){
                     temp->val = val;
                     return FALSE;
                }
                temp = temp->next;
        }
        
        newNode->key = key;
        newNode->val = val;
        newNode->next = list;
        t->list[pos] = newNode;
        t->num_elements++;

        return TRUE;
}


//lookup key
Element* lookup(Hashtable *t,uns64  key){
        int pos = hashCode(t,key);
        struct ht_node *list = t->list[pos];
        struct ht_node *temp = list;
        while(temp){
            if(temp->key==key){
                return temp->val;
            }
            temp = temp->next;
        }
        return NULL;
}

Element* createElement()
{

    Element *elm = (Element*)malloc(sizeof(Element));

    elm->rd_cnt=0;
    elm->wr_cnt=0;
    elm->access_count=0;
    return elm;
 
}
/*
main()
{
    Hashtable *tb = createTable(5);
    Element* elm= createElement();

    for(int i=0;i<10;i++)
    {
        Element* elm= createElement();
        elm->rd_cnt=i;
        insert(tb, i, elm);
    }
    
    elm= lookup(tb, 5);

    printf("test:%d\n", elm->rd_cnt);
    destroyTable(tb);
        
}
*/
