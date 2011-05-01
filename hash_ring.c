#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "sha1.h"
#include "hash_ring.h"
#include "bubble_sort.h"

hash_ring_t *hash_ring_create(uint32_t numReplicas) {
    hash_ring_t *ring = NULL;
    
    // numReplicas must be greater than or equal to 1
    if(numReplicas <= 0) return NULL;
    
    ring = (hash_ring_t*)malloc(sizeof(hash_ring_t));
    
    ring->numReplicas = numReplicas;
    ring->nodes = NULL;
    ring->items = NULL;
    ring->numNodes = 0;
    ring->numItems = 0;
    
    return ring;
}

void hash_ring_free(hash_ring_t *ring) {
    if(ring == NULL) return;

    // Clean up the nodes
    ll_t *tmp, *cur = ring->nodes;
    while(cur != NULL) {
        free(((hash_ring_node_t*)cur->data)->key);
        tmp = cur;
        cur = tmp->next;
        free(tmp);
    }
    ring->nodes = NULL;
    
    // Clean up the items
    int x;
    for(x = 0; x < ring->numItems; x++) {
        free(ring->items[x]);
    }
    free(ring->items);
    
    free(ring);
}

void hash_ring_print(hash_ring_t *ring) {
    if(ring == NULL) return;
    int x, y;
    
    printf("----------------------------------------\n");
    printf("hash_ring\n\n");
    printf("numReplicas:%8d\n", ring->numReplicas);
    printf("Nodes: \n\n");
    
    ll_t *cur = ring->nodes;
    x = 0;
    while(cur != NULL) {
        printf("%d: ", x);
        
        hash_ring_node_t *node = (hash_ring_node_t*)cur->data;
        
        for(y = 0; y < node->keyLen; y++) {
            printf("%c", node->key[y]);
        }
        printf("\n");
        cur = cur->next;
        x++;
    }
    printf("\n");
    printf("Items (%d): \n\n", ring->numItems);
    
    for(x = 0; x < ring->numItems; x++) {
        hash_ring_item_t *item = ring->items[x];
        printf("%" PRIu64 " : ", item->number);
        for(y = 0; y < item->node->keyLen; y++) {
            printf("%c", item->node->key[y]);
        }
        printf("\n");
    }
    
    printf("\n");
    printf("----------------------------------------\n");
    
}

int hash_ring_add_items(hash_ring_t *ring, hash_ring_node_t *node) {
    int x;
    SHA1Context sha1_ctx;
    char concat_buf[8];
    int concat_len;
    
    hash_ring_item_t *items = (hash_ring_item_t*)malloc(sizeof(hash_ring_item_t) * ring->numReplicas);
    if(items == NULL) {
        return HASH_RING_ERR;
    }

    // Resize the items array
    void *resized = realloc(ring->items, (sizeof(hash_ring_item_t*) * ring->numNodes * ring->numReplicas));
    if(resized == NULL) {
        return HASH_RING_ERR;
    }
    ring->items = (hash_ring_item_t**)resized;
    
    for(x = 0; x < ring->numReplicas; x++) {
        SHA1Reset(&sha1_ctx);
        
        SHA1Input(&sha1_ctx, node->key, node->keyLen);
        
        concat_len = snprintf(concat_buf, sizeof(concat_buf), "%d", x);
        SHA1Input(&sha1_ctx, (uint8_t*)&concat_buf, concat_len);
        
        if(SHA1Result(&sha1_ctx) != 1) {
            free(items);
            return HASH_RING_ERR;
        }
        hash_ring_item_t *item = (hash_ring_item_t*)malloc(sizeof(hash_ring_item_t));
        item->node = node;
        item->number = sha1_ctx.Message_Digest[3];
        item->number <<= 32;
        item->number |= sha1_ctx.Message_Digest[4];
        
        ring->items[(ring->numNodes - 1) * ring->numReplicas + x] = item;
    }

    ring->numItems += ring->numReplicas;
    return HASH_RING_OK;
}

static int item_sort(void *a, void *b) {
    hash_ring_item_t *itemA = a, *itemB = b;

    if(itemA->number < itemB->number) {
        return -1;
    }
    else if(itemA->number > itemB->number) {
        return 1;
    }
    else {
        return 0;
    }
}

// TODO - add locking for multithreaded use?
int hash_ring_add_node(hash_ring_t *ring, uint8_t *key, uint32_t keyLen) {
    if(hash_ring_get_node(ring, key, keyLen) != NULL) return HASH_RING_ERR;
    if(key == NULL || keyLen <= 0) return HASH_RING_ERR;

    hash_ring_node_t *node = (hash_ring_node_t*)malloc(sizeof(hash_ring_node_t));
    if(node == NULL) {
        return HASH_RING_ERR;
    }
    
    node->key = (uint8_t*)malloc(keyLen);
    if(node->key == NULL) {
        free(node);
        return HASH_RING_ERR;
    }
    memcpy(node->key, key, keyLen);
    node->keyLen = keyLen;
    
    ll_t *cur = (ll_t*)malloc(sizeof(ll_t));
    if(cur == NULL) {
        free(node->key);
        free(node);
        return HASH_RING_ERR;
    }
    cur->data = node;
    
    // Add the node
    ll_t *tmp = ring->nodes;
    ring->nodes = cur;
    ring->nodes->next = tmp;
    
    ring->numNodes++;
    
    // Add the items for this node
    if(hash_ring_add_items(ring, node) != HASH_RING_OK) {
        hash_ring_remove_node(ring, node->key, node->keyLen);
        return HASH_RING_ERR;
    }

    // Sort the items
    bubble_sort_array((void**)ring->items, ring->numItems, item_sort);
    
    return HASH_RING_OK;
}

int hash_ring_remove_node(hash_ring_t *ring, uint8_t *key, uint32_t keyLen) {
    if(ring == NULL || key == NULL || keyLen <= 0) return HASH_RING_ERR;

    hash_ring_node_t *node;
    ll_t *next, *prev = NULL, *cur = ring->nodes;
    
    while(cur != NULL) {
        node = (hash_ring_node_t*)cur->data;
        if(node->keyLen == keyLen  &&
            memcmp(node->key, key, keyLen) == 0) {
                
                // Node found, remove it
                next = cur->next;
                free(node->key);
                
                if(prev == NULL) {
                    ring->nodes = next;
                }
                else {
                    prev->next = next;
                }
                
                free(node);
                free(cur);
                
                ring->numNodes--;
                
                return HASH_RING_OK;
        }
        
        prev = cur;
        cur = prev->next;
    }
    
    return HASH_RING_ERR;
}

hash_ring_node_t *hash_ring_get_node(hash_ring_t *ring, uint8_t *key, uint32_t keyLen) {
    if(ring == NULL || key == NULL || keyLen <= 0) return NULL;
    
    ll_t *cur = ring->nodes;
    while(cur != NULL) {
        hash_ring_node_t *node = (hash_ring_node_t*)cur->data;
        // Check if the keyLen is the same as well as the key
        if(node->keyLen == keyLen && 
            memcmp(node->key, key, keyLen) == 0) {
                return node;
        }
        cur = cur->next;
    }
    
    return NULL;
}

hash_ring_item_t *hash_ring_find_next_highest_item(hash_ring_t *ring, uint64_t num) {
    if(ring->numItems == 0) return NULL;
    
    int min = 0;
    int max = ring->numItems - 1;
    hash_ring_item_t *item = NULL;

    while(1) {
        if(min > max) {
            if(min == ring->numItems) {
                // Past the end of the ring, return the first item
                return ring->items[0];
            }
            else {
                // Return the next highest item
                return ring->items[min];
            }
        }
        
        int midpointIndex = (min + max) / 2;
        item = ring->items[midpointIndex];

        if(item->number > num) {
            // Key is in the lower half
            max = midpointIndex - 1;
        }
        else if(item->number <= num) {
            // Key is in the upper half
            min = midpointIndex + 1;
        }
    }
    
    return NULL;
}

hash_ring_node_t *hash_ring_find_node(hash_ring_t *ring, uint8_t *key, uint32_t keyLen) {
    if(ring == NULL || key == NULL || keyLen <= 0) return NULL;
    
    SHA1Context sha1_ctx;
    
    SHA1Reset(&sha1_ctx);
    SHA1Input(&sha1_ctx, key, keyLen);
    if(SHA1Result(&sha1_ctx) != 1) {
        return NULL;
    }
    
    uint64_t keyInt = sha1_ctx.Message_Digest[3];
    keyInt <<= 32;
    keyInt |= sha1_ctx.Message_Digest[4];

    hash_ring_item_t *item = hash_ring_find_next_highest_item(ring, keyInt);
    if(item == NULL) {
        return NULL;
    }
    else {
        return item->node;
    }
}