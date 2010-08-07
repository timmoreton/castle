#include "castle_public.h"
#include "castle_compile.h"
#include "castle.h"
#include "castle_da.h"
#include "castle_utils.h"
#include "castle_btree.h"
#include "castle_cache.h"
#include "castle_versions.h"
#include "castle_freespace.h"
#include "castle_rxrpc.h"

//#define DEBUG
#ifndef DEBUG
#define debug(_f, ...)          ((void)0)
#else
#define debug(_f, _a...)        (printk("%s:%.4d: " _f, __FILE__, __LINE__ , ##_a))
#endif

   
static const uint32_t OBJ_TOMBSTONE = ((uint32_t)-1);

#define KEY_DIMENSION_NEXT_FLAG             (1 << 0)                                              
#define KEY_DIMENSION_MINUS_INFINITY_FLAG   (1 << 1)                                              
#define KEY_DIMENSION_UNUSED2_FLAG          (1 << 2)                                              
#define KEY_DIMENSION_UNUSED3_FLAG          (1 << 3)                                              
#define KEY_DIMENSION_UNUSED4_FLAG          (1 << 4)                                              
#define KEY_DIMENSION_UNUSED5_FLAG          (1 << 5)                                              
#define KEY_DIMENSION_UNUSED6_FLAG          (1 << 6)                                              
#define KEY_DIMENSION_UNUSED7_FLAG          (1 << 7)                                              
#define KEY_DIMENSION_FLAGS_SHIFT           (8)
#define KEY_DIMENSION_FLAGS_MASK           ((1 << KEY_DIMENSION_FLAGS_SHIFT) - 1) 
#define KEY_DIMENSION_FLAGS(_dim_head)      ((_dim_head) &  KEY_DIMENSION_FLAGS_MASK)
#define KEY_DIMENSION_OFFSET(_dim_head)     ((_dim_head) >> KEY_DIMENSION_FLAGS_SHIFT)
#define KEY_DIMENSION_HEADER(_off, _flags)  (((_off)  << KEY_DIMENSION_FLAGS_SHIFT) |     \
                                             ((_flags) & KEY_DIMENSION_FLAGS_MASK))


static inline uint32_t castle_object_btree_key_dim_length(c_vl_bkey_t *key, int dim)
{
    uint32_t end_offset;

    end_offset = (dim+1 < key->nr_dims) ? KEY_DIMENSION_OFFSET(key->dim_head[dim+1]) :
                                          key->length + 4;

    return end_offset - KEY_DIMENSION_OFFSET(key->dim_head[dim]);
}

static inline char* castle_object_btree_key_dim_get(c_vl_bkey_t *key, int dim)
{
    return (char *)key + KEY_DIMENSION_OFFSET(key->dim_head[dim]);
}

static inline uint32_t castle_object_btree_key_dim_flags_get(c_vl_bkey_t *key, int dim)
{
    return KEY_DIMENSION_FLAGS(key->dim_head[dim]);
}

/* Constructs btree key, taking dimensions < okey_first_dim from the src_bkey, and
   dimensions >= okey_first_dim from src_okey. */
static c_vl_bkey_t* castle_object_btree_key_construct(c_vl_bkey_t *src_bkey,
                                                      c_vl_okey_t *src_okey,
                                                      int okey_first_dim)
{
    uint32_t key_len, first_okey_offset, payload_offset;
    int i, nr_dims;
    c_vl_bkey_t *btree_key;

    /* Sanity checks */
    BUG_ON(!src_okey);
    BUG_ON(okey_first_dim > 0 && !src_bkey);
    BUG_ON(okey_first_dim == 0 && src_bkey);
    BUG_ON(src_bkey && src_okey && (src_bkey->nr_dims != src_okey->nr_dims));
    BUG_ON(okey_first_dim >= src_okey->nr_dims);

    nr_dims = src_okey->nr_dims;
    /* Work the length of the btree key. okey_first_dim > 0, work out how much space the
       dimensions < okey_first_dim take up first. */ 
    if(okey_first_dim > 0)
    {
        /* The length of the header + dimensions < okey_first_dim can be easily worked
           out by looking at the offset for the okey_first_dim in the src_bkey */ 
        first_okey_offset = castle_object_btree_key_dim_get(src_bkey, okey_first_dim) - 
                                (char *)src_bkey;
        key_len = first_okey_offset;
    }
    else
    {
        /* Work out the header size (including the dim_head array) */
        key_len = sizeof(c_vl_bkey_t) + 4 * nr_dims;
    }

    /* Add the size of dimensions >= okey_first_dim */
    for(i=okey_first_dim; i<nr_dims; i++)
        key_len += src_okey->dims[i]->length;

    /* Allocate the single-dimensional key */
    btree_key = kzalloc(key_len, GFP_KERNEL);
    if(!btree_key)
        return NULL;

    /* Work out where should the first_okey_dim be put. Copy the relevant bits from src_bkey. */
    if(okey_first_dim > 0)
    {
        payload_offset = first_okey_offset;
        memcpy(btree_key, src_bkey, payload_offset);
    }
    else
        payload_offset = sizeof(c_vl_bkey_t) + 4 * nr_dims;

    /* Construct the key. */
    btree_key->length  = key_len - 4; /* Length doesn't include length field */
    btree_key->nr_dims = nr_dims;
    /* Go through all okey dimensions and write them in. */ 
    for(i=okey_first_dim; i<nr_dims; i++)
    {
        btree_key->dim_head[i] = KEY_DIMENSION_HEADER(payload_offset, 0);
        memcpy((char *)btree_key + payload_offset, src_okey->dims[i]->key, src_okey->dims[i]->length);
        payload_offset += src_okey->dims[i]->length;
    }
    BUG_ON(payload_offset != key_len);

    return btree_key;
}

/* Converts 'object key' (i.e. multidimensional key) to btree key (single dimensional) */
c_vl_bkey_t* castle_object_key_convert(c_vl_okey_t *obj_key)
{
    return castle_object_btree_key_construct(NULL, obj_key, 0);
}

c_vl_okey_t* castle_object_btree_key_convert(c_vl_bkey_t *btree_key)
{
    c_vl_okey_t *obj_key;
    c_vl_key_t *dim;
    uint32_t dim_len;
    int i;

    obj_key = kzalloc(sizeof(c_vl_okey_t) + sizeof(c_vl_key_t *) * btree_key->nr_dims, GFP_KERNEL);
    if(!obj_key)
        return NULL;

    obj_key->nr_dims = btree_key->nr_dims;
    for(i=0; i<btree_key->nr_dims; i++)
    {
        dim_len = castle_object_btree_key_dim_length(btree_key, i);
        dim = kmalloc(dim_len + 4, GFP_KERNEL);
        if(!dim)
            goto err_out;
        dim->length = dim_len;
        memcpy(dim->key, castle_object_btree_key_dim_get(btree_key, i), dim_len);
        obj_key->dims[i] = dim; 
    }

    return obj_key;

err_out:
    for(i--; i>0; i--)
        kfree(obj_key->dims[i]);
    kfree(obj_key);

    return NULL;
}

static inline int castle_object_key_dim_compare(char *dim_a, uint32_t dim_a_len, uint32_t dim_a_flags,
                                                char *dim_b, uint32_t dim_b_len, uint32_t dim_b_flags)
{
    int cmp, dim_a_next_flag, dim_b_next_flag;

     /* Lexicographic comparison of the two dims (min length) */
    cmp = memcmp(dim_a, dim_b, ((dim_a_len > dim_b_len) ? dim_b_len : dim_a_len));
    if(cmp)
        return cmp;
    /* If the common part of the keys the same, check which one is shorter */
    if(dim_a_len != dim_b_len)
        return (dim_a_len > dim_b_len) ? 1 : -1;

    /* Identical dimension, check if either of the keys has NEXT_FLAG set */ 
    dim_a_next_flag = dim_a_flags & KEY_DIMENSION_NEXT_FLAG;
    dim_b_next_flag = dim_b_flags & KEY_DIMENSION_NEXT_FLAG;
    /* We should never compare two non-btree keys */
    BUG_ON(dim_a_next_flag && dim_b_next_flag);
    if(dim_a_next_flag)
        return 1;
    if(dim_b_next_flag)
        return -1;
 
    return 0;
}

int castle_object_btree_key_compare(c_vl_bkey_t *key1, c_vl_bkey_t *key2)
{
    int dim;

    /* Compare dimensions first */
    if(key1->nr_dims != key2->nr_dims)
        return key1->nr_dims > key2->nr_dims ? 1 : -1;

    /* Number of dimensions is the same, go through them one by one */ 
    for(dim=0; dim<key1->nr_dims; dim++)
    {
        int cmp;

        /* Lexicographic comparison of the two dims (min length) */
        cmp = castle_object_key_dim_compare(castle_object_btree_key_dim_get(key1, dim),
                                            castle_object_btree_key_dim_length(key1, dim),
                                            castle_object_btree_key_dim_flags_get(key1, dim),
                                            castle_object_btree_key_dim_get(key2, dim),
                                            castle_object_btree_key_dim_length(key2, dim),
                                            castle_object_btree_key_dim_flags_get(key2, dim));
        if(cmp)
            return cmp;
        /* This dimension is identical in every way for the two keys. Move on to the next one */
    }

    /* All dimensions identical in every way for the two keys => keys identical */
    return 0;
}
    
static void castle_object_btree_key_dim_inc(c_vl_bkey_t *key, int dim)
{
    uint32_t flags = KEY_DIMENSION_FLAGS(key->dim_head[dim]);
    uint32_t offset = KEY_DIMENSION_OFFSET(key->dim_head[dim]);

    key->dim_head[dim] = KEY_DIMENSION_HEADER(offset, flags | KEY_DIMENSION_NEXT_FLAG);
}

void *castle_object_btree_key_next(c_vl_bkey_t *key)
{
    c_vl_bkey_t *new_key;
    uint32_t key_length;

    /* Duplicate the key first */
    key_length = key->length + 4;
    /* TODO: memory leak, fix all clients */
    new_key = kmalloc(key_length, GFP_KERNEL);
    if(!new_key)
        return NULL;
    memcpy(new_key, key, key_length);

    /* Increment the least significant dimension */
    castle_object_btree_key_dim_inc(new_key, new_key->nr_dims-1);

    return new_key;
}

/* Checks if the btree key is within the bounds imposed by start/end object keys.
   Returns 1 if the most significant dimension is greater than the end, -1 if it is
   less then start, or 0 if the key is within bounds. Optionally, the function can
   be queried about which dimension offeneded */
static int castle_object_btree_key_bounds_check(c_vl_bkey_t *key,
                                                c_vl_okey_t *start,
                                                c_vl_okey_t *end,
                                                int *offending_dim_p)
{
    int dim;

    if((key->nr_dims != start->nr_dims) || (key->nr_dims != end->nr_dims))
    {
        printk("Nonmatching # of dimensions: key=%d, start_key=%d, end_key=%d\n",
                key->nr_dims, start->nr_dims, end->nr_dims);
        BUG();
    }
    /* Go through each dimension checking if they are within bounds */
    for(dim=0; dim<key->nr_dims; dim++)
    {
        uint32_t key_dim_len, key_dim_flags, start_dim_len, end_dim_len;
        char *key_dim, *start_dim, *end_dim;
        int cmp;

        key_dim_len   = castle_object_btree_key_dim_length(key, dim);
        key_dim       = castle_object_btree_key_dim_get(key, dim);
        key_dim_flags = castle_object_btree_key_dim_flags_get(key, dim);

        start_dim_len = start->dims[dim]->length;
        start_dim     = start->dims[dim]->key;

        end_dim_len   = end->dims[dim]->length;
        end_dim       = end->dims[dim]->key;

        cmp = castle_object_key_dim_compare(key_dim, 
                                            key_dim_len,
                                            key_dim_flags,
                                            start_dim,
                                            start_dim_len,
                                            0);
        /* We expect the key to be >= than the start key. Therefore, exit when it is not. */
        if(cmp < 0)
        {
            if(offending_dim_p) *offending_dim_p = dim;
            return -1;
        }

        cmp = castle_object_key_dim_compare(key_dim, 
                                            key_dim_len,
                                            key_dim_flags,
                                            end_dim,
                                            end_dim_len,
                                            0);
        /* We expect the key to be <= than the end key. */ 
        if(cmp > 0)
        {
            if(offending_dim_p) *offending_dim_p = dim;
            return 1;
        }
    }

    return 0;
}

static c_vl_bkey_t* castle_object_btree_key_skip(c_vl_bkey_t *old_key, 
                                                 c_vl_okey_t *start, 
                                                 int offending_dim,
                                                 int bigger)
{
    c_vl_bkey_t *new_key;

    new_key = castle_object_btree_key_construct(old_key,
                                                start,
                                                offending_dim);
    if(!new_key)
        return NULL;

    /* If the offending dimension was bigger than the bounds, we need to set 
       the NEXT_FLAG for it */ 
    if(bigger)
        castle_object_btree_key_dim_inc(new_key, offending_dim);

    return new_key;
}

void castle_object_key_free(c_vl_okey_t *obj_key)
{
    int i;

    for(i=0; i < obj_key->nr_dims; i++)
        kfree(obj_key->dims[i]);
    kfree(obj_key);
}

/**********************************************************************************************/
/* Iterator(s) */

typedef struct castle_objects_rq_iterator {
    /* Filled in by the client */
    da_id_t             da_id;
    version_t           version;
    c_vl_okey_t        *start_okey;
    c_vl_okey_t        *end_okey;

    /* Rest */
    int                 err;
    c_vl_bkey_t        *start_bkey;
    c_vl_bkey_t        *end_bkey;
    c_da_rq_iter_t      da_rq_iter;
    /* Cached entry, guaranteed to fall in the hypercube */
    int                 cached;
    void               *cached_k;
    version_t           cached_v;
    c_val_tup_t         cached_cvt;
} c_obj_rq_iter_t;

#if 0 
typedef struct castle_da_rq_iterator {
    int                       nr_cts;
    int                       err;
    struct castle_btree_type *btree;
    void                     *min_key;
    c_merged_iter_t           merged_iter;

    struct ct_rq {
        struct castle_component_tree *ct;
        c_rq_enum_t                   ct_rq_iter; 
    } *ct_rqs;
} c_da_rq_iter_t;
#endif

static void castle_objects_rq_iter_next(c_obj_rq_iter_t *iter, 
                                        void **k, 
                                        version_t *v, 
                                        c_val_tup_t *cvt) 
{
    BUG_ON(!iter->cached);
    if(k)   *k   = iter->cached_k;
    if(v)   *v   = iter->cached_v;
    if(cvt) *cvt = iter->cached_cvt;
    iter->cached = 0;
}

static int castle_objects_rq_iter_has_next(c_obj_rq_iter_t *iter)
{
    void *k, *next_key;
    version_t v;
    c_val_tup_t cvt;
    int offending_dim, bigger;

    while(1)
    {
        if(iter->cached)
            return 1;
        /* Nothing cached, check if da_rq_iter has anything */
        if(!castle_da_rq_iter.has_next(&iter->da_rq_iter))
            return 0;
        /* Nothing cached, but there is something in the da_rq_iter.
           Check if that's within the rq hypercube */
        castle_da_rq_iter.next(&iter->da_rq_iter, &k, &v, &cvt);
        bigger = castle_object_btree_key_bounds_check(k, 
                                                      iter->start_okey, 
                                                      iter->end_okey,
                                                      &offending_dim);
        //printk("Got the following key from da_rq iterator. Is in range: %d, offending_dim=%d\n", 
        //        bigger, offending_dim);
        //vl_bkey_print(k);
        if(bigger)
        {
            /* We are outside of the rq hypercube, find next intersection point
               and skip to that */
            next_key = castle_object_btree_key_skip(k, 
                                                    iter->start_okey, 
                                                    offending_dim,
                                                    bigger);
            //printk("Skipping to:\n");
            //vl_bkey_print(next_key);
            /* TODO: memory leak for next keys! FIX that */
            castle_da_rq_iter.skip(&iter->da_rq_iter, next_key);
        }    
        else 
        {
            /* Found something to cache, save */
            iter->cached_k = k;
            iter->cached_v = v;
            iter->cached_cvt = cvt;
            iter->cached = 1;
        }
    }

    /* We should never get here */
    BUG();
}

static void castle_objects_rq_iter_init(c_obj_rq_iter_t *iter)
{
    BUG_ON(!iter->start_okey || !iter->end_okey);

    iter->err = 0;
    iter->cached = 0;
    /* Construct the btree keys for range-query */
    iter->start_bkey = castle_object_key_convert(iter->start_okey);
    iter->end_bkey   = castle_object_key_convert(iter->end_okey);
    printk("====================== RQ start keys =======================\n");
    vl_okey_print(iter->start_okey);
    vl_bkey_print(iter->start_bkey);
    printk("======================= RQ end keys ========================\n");
    vl_okey_print(iter->end_okey);
    vl_bkey_print(iter->end_bkey);
    printk("============================================================\n");

    /* Check if we managed to initialise the btree keys correctly */
    if(!iter->start_bkey || !iter->end_bkey)
    {
        iter->err = -ENOMEM;
        return;
    }

    castle_da_rq_iter_init(&iter->da_rq_iter, 
                            iter->version, 
                            iter->da_id, 
                            iter->start_bkey, 
                            iter->end_bkey);
    if(iter->da_rq_iter.err)
    {
        iter->err = iter->da_rq_iter.err;
        return;
    }
}

struct castle_iterator_type castle_objects_rq_iter = {
    .has_next = (castle_iterator_has_next_t)castle_objects_rq_iter_has_next,
    .next     = (castle_iterator_next_t)    castle_objects_rq_iter_next,
    .skip     = NULL, 
};

/**********************************************************************************************/
/* High level interface functions */
static void castle_object_replace_cvt_get(c_bvec_t    *c_bvec,
                                          c_val_tup_t  prev_cvt,
                                          c_val_tup_t *cvt)
{
    struct castle_rxrpc_call *call = c_bvec->c_bio->rxrpc_call;
    int tombstone = c_bvec_data_del(c_bvec); 
    int nr_blocks;

    /* We should be handling a write (possibly a tombstone write). */
    BUG_ON(c_bvec_data_dir(c_bvec) != WRITE); 
    /* Some sanity checks */
    BUG_ON(CVT_TOMB_STONE(prev_cvt) && (prev_cvt.length != 0));

    /* Allocate space for new value, in or out of line */ 
    if(!tombstone)
    {
        /* The packet will now contain the length of the data payload */
        cvt->length = castle_rxrpc_uint32_get(call);
        /* Decide whether to use inline, or out-of-line value on the 
           basis of this length. */
        if (cvt->length <= MAX_INLINE_VAL_SIZE)
        {
            cvt->type = CVT_TYPE_INLINE;
            cvt->val  = kmalloc(cvt->length, GFP_NOIO);
            /* TODO: Work out how to handle this */
            BUG_ON(!cvt->val);
            /* We should not inline values which do not fit in a packet */
            BUG_ON(castle_rxrpc_packet_length(call) < cvt->length);
            castle_rxrpc_str_copy(call, cvt->val, cvt->length, 0 /* not partial */); 
        }
        else
        {
            nr_blocks = (cvt->length - 1) / C_BLK_SIZE + 1; 
            /* Arbitrary limits on the size of the objets (freespace code cannot handle
               huge objects ATM) */
            BUG_ON(nr_blocks > 100); 
            cvt->type   = CVT_TYPE_ONDISK;
            cvt->cdb    = castle_freespace_block_get(c_bvec->version, nr_blocks); 
            /* TODO: Again, work out how to handle failed allocations */ 
            BUG_ON(DISK_BLK_INVAL(cvt->cdb));
         }

    } else
    /* For tombstones, construct the cvt and exit. */
    {
        CVT_TOMB_STONE_SET(*cvt);
    }

    /* If there was an out-of-line object stored under this key, release it. */
    if (CVT_ONDISK(prev_cvt))
    {
        nr_blocks = (prev_cvt.length - 1) / C_BLK_SIZE + 1; 
        castle_freespace_block_free(prev_cvt.cdb,
                                    c_bvec->version,
                                    nr_blocks);
    }
    BUG_ON(CVT_INVALID(*cvt));
}

#define OBJ_IO_MAX_BUFFER_SIZE      (10)    /* In C_BLK_SIZE blocks */

static c_disk_blk_t castle_object_write_next_cdb(struct castle_rxrpc_call *call,
                                                 c_disk_blk_t old_cdb,
                                                 uint32_t data_length)
{
    uint32_t data_c2b_length;
    c_disk_blk_t new_data_cdb;
    int nr_blocks;

    /* Work out how large buffer to allocate */
    data_c2b_length = data_length > OBJ_IO_MAX_BUFFER_SIZE * C_BLK_SIZE ?
                                    OBJ_IO_MAX_BUFFER_SIZE * C_BLK_SIZE :
                                    data_length;
    nr_blocks = (data_c2b_length - 1) / C_BLK_SIZE + 1; 
    debug("Allocating new buffer of size %d blocks, for data_length=%d\n",
        nr_blocks, data_length);
    new_data_cdb.disk  = old_cdb.disk; 
    new_data_cdb.block = old_cdb.block + nr_blocks; 

    return new_data_cdb;
}

static c2_block_t* castle_object_write_buffer_alloc(struct castle_rxrpc_call *call,
                                                    c_disk_blk_t new_data_cdb,
                                                    uint32_t data_length)
{
    uint32_t data_c2b_length;
    c2_block_t *new_data_c2b;
    int nr_blocks;

    /* Work out how large the buffer is */
    data_c2b_length = data_length > OBJ_IO_MAX_BUFFER_SIZE * C_BLK_SIZE ?
                                    OBJ_IO_MAX_BUFFER_SIZE * C_BLK_SIZE :
                                    data_length;
    nr_blocks = (data_c2b_length - 1) / C_BLK_SIZE + 1; 
    new_data_c2b = castle_cache_block_get(new_data_cdb, nr_blocks);
    lock_c2b(new_data_c2b);
    set_c2b_uptodate(new_data_c2b);
#ifdef CASTLE_DEBUG        
    /* Poison the data block */
    memset(c2b_buffer(new_data_c2b), 0xf4, nr_blocks * C_BLK_SIZE);
#endif
 
    return new_data_c2b;
}

static int castle_object_data_write(struct castle_rxrpc_call *call)
{
    c2_block_t *data_c2b;
    uint32_t data_c2b_offset, data_c2b_length, data_length, packet_length;
    c2_block_t *new_data_c2b;
    c_disk_blk_t new_data_cdb;

    /* Work out how much data we've got, and how far we've got so far */
    castle_rxrpc_replace_call_get(call, &data_c2b, &data_c2b_offset, &data_length);
    debug("Data write. call=%p, data_c2b=%p, data_c2b_offset=%d, data_length=%d\n",
        call, data_c2b, data_c2b_offset, data_length);
    data_c2b_length = data_c2b->nr_pages * C_BLK_SIZE;
    packet_length = castle_rxrpc_packet_length(call);

    debug("Packet length=%d, data_length=%d\n", packet_length, data_length);
    BUG_ON(packet_length <= 0);
    do {
        char *data_c2b_buffer;
        int copy_length;
        int last_copy;

        BUG_ON(data_c2b_offset >= data_c2b_length);
        data_c2b_buffer = (char *)c2b_buffer(data_c2b) + data_c2b_offset; 
        copy_length = data_c2b_length - data_c2b_offset >= packet_length ? 
                                           packet_length :
                                           data_c2b_length - data_c2b_offset;
        debug("Could copy %d bytes.\n", copy_length);
        last_copy = 0;
        if(copy_length >= data_length)
        {
            debug("data_length=%d is smaller than copy_length=%d, resetting copy_length.\n", 
                    data_length, copy_length);
            last_copy = 1;
            copy_length = data_length;
        }
        castle_rxrpc_str_copy(call, 
                              data_c2b_buffer,
                              copy_length,
                              last_copy ? 0 : 1);

        data_length     -= copy_length;
        data_c2b_offset += copy_length;
        /* For last copy more bytes might have been pulled, work out how many */
        if(last_copy)
            copy_length += (copy_length % 4 == 0 ? 0 : 4 - copy_length % 4);
        debug("Read %d bytes from the packet.\n", copy_length);
        packet_length   -= copy_length;


        /* Allocate a new buffer if there will be more data (either in the current
           packet, or in future packets). */
        if((data_c2b_offset == data_c2b_length) && (data_length > 0))
        {
            debug("Run out of buffer space, allocating a new one.\n");
            new_data_cdb = castle_object_write_next_cdb(call, data_c2b->cdb, data_length); 
            new_data_c2b = castle_object_write_buffer_alloc(call, new_data_cdb, data_length); 
            data_c2b_length = new_data_c2b->nr_pages * C_BLK_SIZE;
            data_c2b_offset = 0;
            /* Release the (old) buffer */
            dirty_c2b(data_c2b);
            unlock_c2b(data_c2b);
            put_c2b(data_c2b);
            /* Swap the new buffer in, if one was initialised. */
            data_c2b = new_data_c2b;
        } 
    }
    while((packet_length > 0) && (data_length > 0));

    debug("Exiting data_write with data_c2b_offset=%d, data_length=%d, data_c2b=%p\n", 
            data_c2b_offset, data_length, data_c2b);
    castle_rxrpc_replace_call_set(call, data_c2b, data_c2b_offset, data_length);

    return (data_length == 0);
}
                                     

void castle_object_replace_complete(struct castle_bio_vec *c_bvec,
                                    int err,
                                    c_val_tup_t cvt)
{
    struct castle_rxrpc_call *call = c_bvec->c_bio->rxrpc_call;
    c_bio_t *c_bio = c_bvec->c_bio;
    c2_block_t *c2b = NULL;
    int complete_write;

    /* Sanity checks on the bio */
    BUG_ON(c_bvec_data_dir(c_bvec) != WRITE); 
    BUG_ON(atomic_read(&c_bio->count) != 1);
    BUG_ON(c_bio->err != 0);

    /* Free the key */
    kfree(c_bvec->key);

    /* Deal with error case first */
    if(err)
    {
        castle_rxrpc_replace_complete(call, err);
        castle_utils_bio_free(c_bio);
        return;
    }

    /* Otherwise, write the entry out. */
    BUG_ON(CVT_INVALID(cvt));
    if(CVT_ONDISK(cvt))
    {
        BUG_ON(c_bvec_data_del(c_bvec));
        c2b = castle_object_write_buffer_alloc(call, cvt.cdb, cvt.length); 
        castle_rxrpc_replace_call_set(call, c2b, 0, cvt.length); 
        complete_write = castle_object_data_write(call);
    }
    else 
    if(CVT_INLINE(cvt))
    {
        complete_write = 1;
        kfree(cvt.val);
    }
        
        
    /* Unlock buffers, and complete the call if we are done already */
    if(complete_write)
    {
        debug("Completing the write. c2b=%p\n", c2b);
        if(c2b)
        {
            dirty_c2b(c2b);
            unlock_c2b(c2b);
            put_c2b(c2b);
        }
 
        castle_rxrpc_replace_complete(call, 0);
    } else
    /* Complete the packet, so that the client sends us more. */
    {
        debug("Completing the packet, continuing the rest of the write.\n");
        castle_rxrpc_replace_continue(call);
    }

    castle_utils_bio_free(c_bio);
}

int castle_object_replace_continue(struct castle_rxrpc_call *call, int last)
{
    int copy_end;

    debug("Replace continue.\n");
    copy_end = castle_object_data_write(call);
    if(copy_end != last)
        printk("Warning packet for completed replace!!.\n");
    if(last)
    {
        c2_block_t *data_c2b;
        uint32_t data_c2b_offset, data_length;

        castle_rxrpc_replace_call_get(call, &data_c2b, &data_c2b_offset, &data_length);
        BUG_ON(data_length != 0);
        dirty_c2b(data_c2b);
        unlock_c2b(data_c2b);
        put_c2b(data_c2b);
        castle_rxrpc_replace_complete(call, 0);
    } else
    {
        castle_rxrpc_replace_continue(call);
    }

    return 0;
}

int castle_object_replace(struct castle_rxrpc_call *call, 
                          struct castle_attachment *attachment,
                          c_vl_okey_t *key, 
                          int tombstone)
{
    c_vl_bkey_t *btree_key;
    c_bvec_t *c_bvec;
    c_bio_t *c_bio;

    btree_key = castle_object_key_convert(key);
    castle_object_key_free(key);
    
    //printk(" value          : %s\n", tombstone ? "tombstone" : "object");
    //printk("Btree key is:");
    //vl_key_print(btree_key);

    /* Single c_bvec for the bio */
    c_bio = castle_utils_bio_alloc(1);
    if(!c_bio)
        return -ENOMEM;
    BUG_ON(!attachment);
    c_bio->attachment    = attachment;
    c_bio->rxrpc_call    = call;
    c_bio->data_dir      = WRITE;
    /* Tombstone & object replace both require a write */
    if(tombstone) 
        c_bio->data_dir |= REMOVE;
    
    c_bvec = c_bio->c_bvecs; 
    c_bvec->key        = btree_key; 
    c_bvec->flags      = 0;
    c_bvec->cvt_get    = castle_object_replace_cvt_get;
    c_bvec->endfind    = castle_object_replace_complete;
    c_bvec->da_endfind = NULL; 
    
    /* TODO: add bios to the debugger! */ 

    castle_double_array_find(c_bvec);

    return 0;
}

int castle_object_slice_get(struct castle_rxrpc_call *call, 
                            struct castle_attachment *attachment, 
                            c_vl_okey_t *start_key, 
                            c_vl_okey_t *end_key)
{
    c_obj_rq_iter_t *iterator;
    int dim;
    char *rsp_buffer;
    uint32_t rsp_buffer_offset;
    int nr_vals;
#define SLICE_RSP_BUFFER_LEN    (C_BLK_SIZE * 256)  /* 1MB buffer */

    if(start_key->nr_dims != end_key->nr_dims)
    {
        printk("Range query with different # of dimensions.\n");
        return -EINVAL;
    }

    /* Cannot handle infinities just yet */
    for(dim=0; dim<start_key->nr_dims; dim++)
    {
        BUG_ON(start_key->dims[dim]->length == 0);
        BUG_ON(end_key->dims[dim]->length == 0);
    }

    rsp_buffer = vmalloc(SLICE_RSP_BUFFER_LEN); 

    iterator = kmalloc(sizeof(c_obj_rq_iter_t), GFP_KERNEL);
    if(!iterator)
        return -ENOMEM;

    /* Initialise the iterator */
    iterator->start_okey = start_key;
    iterator->end_okey   = end_key;
    iterator->version    = attachment->version; 
    iterator->da_id      = castle_version_da_id_get(iterator->version);
    
    printk("rq_iter_init.\n");
    castle_objects_rq_iter_init(iterator);
    if(iterator->err)
    {
        kfree(iterator);
        return iterator->err;
    }
    printk("rq_iter_init done.\n");

    nr_vals = 0;
    rsp_buffer_offset = 0;
    while(castle_objects_rq_iter.has_next(iterator))
    {
        c_vl_bkey_t *k;
        c_vl_okey_t *okey;
        version_t v;
        c_val_tup_t cvt;
        char *value;
        c2_block_t *data_c2b;
        int nr_blocks;
        uint32_t marshalled_len;

        printk("Getting an entry the range query.\n");
        castle_objects_rq_iter.next(iterator, (void **)&k, &v, &cvt);
        printk("Got an entry the range query.\n");

        /* Ignore tombstones, we are not sending these */
        if(CVT_TOMB_STONE(cvt))
            continue;

        /* Now we know we've got something to send. 
           Prepare the key for marshaling */
        okey = castle_object_btree_key_convert(k);
        if(!okey)
        {
            /* TODO: free all the buffers etc! */
            return -ENOMEM;
        }
        /* Prepare the value for marshaling */
        if(CVT_INLINE(cvt))
        {
            value = cvt.val;
        } else
        if(CVT_ONDISK(cvt))
        {
            /* We are not handling large values here for the time being 
               (never, if replaced with iterators?) */
            BUG_ON(cvt.length > C_BLK_SIZE); 
            nr_blocks = (cvt.length - 1) / C_BLK_SIZE + 1;
            data_c2b = castle_cache_block_get(cvt.cdb, nr_blocks);
            lock_c2b(data_c2b);
            if(!c2b_uptodate(data_c2b)) 
                BUG_ON(submit_c2b_sync(READ, data_c2b));
            value = c2b_buffer(data_c2b);
        } else
        {
            /* Unknown cvt type */
            BUG();
        }
        BUG_ON(castle_rxrpc_get_slice_reply_marshall(call, 
                                                     okey, 
                                                     value, 
                                                     cvt.length, 
                                                     rsp_buffer + rsp_buffer_offset,
                                                     SLICE_RSP_BUFFER_LEN - rsp_buffer_offset,
                                                     &marshalled_len));
        rsp_buffer_offset += marshalled_len; 
        nr_vals++;
        /* Unlock c2b if one was taken out */
        if(CVT_ONDISK(cvt))
        {
            unlock_c2b(data_c2b);
            put_c2b(data_c2b);
        }
    }
    printk("Ended the rq iterator in objects, replying with nr_vals: %d, rsp_buffer_offset=%d.\n",
            nr_vals, rsp_buffer_offset);
    /* rsp buffer contains responce payload, send it through */
    castle_rxrpc_get_slice_reply(call, 0, nr_vals, rsp_buffer, rsp_buffer_offset);
    
    return 0;
}

void castle_object_get_continue(struct castle_bio_vec *c_bvec,
                                struct castle_rxrpc_call *call,
                                c_disk_blk_t data_cdb,
                                uint32_t data_length);
void __castle_object_get_complete(struct work_struct *work)
{
    c_bvec_t *c_bvec = container_of(work, c_bvec_t, work);
    struct castle_rxrpc_call *call = c_bvec->c_bio->rxrpc_call;
    c2_block_t *c2b;
    uint32_t data_c2b_length, data_length;
    c_disk_blk_t cdb;
    int first, last;

    castle_rxrpc_get_call_get(call, &c2b, &data_c2b_length, &data_length, &first);
    debug("Get complete for call %p, first=%d, c2b->cdb=(0x%x, 0x%x), "
           "data_c2b_length=%d, data_length=%d\n", 
        call, first, c2b->cdb.disk, c2b->cdb.block, data_c2b_length, data_length);
    /* Deal with error case first */
    if(!c2b_uptodate(c2b))
    {
        debug("Not up to date.\n");
        if(first)
            castle_rxrpc_get_reply_start(call, -EIO, 0, NULL, 0);
        else
            castle_rxrpc_get_reply_continue(call, -EIO, NULL, 0, 1 /* last */);
        goto out;
    }
    
    /* If data_length is zero, it means we are supposed to finish this get call */
    last = (data_length == 0);
    debug("Last=%d\n", last);
    if(first)
        castle_rxrpc_get_reply_start(call, 
                                     0,
                                     data_c2b_length + data_length,
                                     c2b_buffer(c2b), 
                                     data_c2b_length);
    else
        castle_rxrpc_get_reply_continue(call, 
                                        0, 
                                        c2b_buffer(c2b), 
                                        data_c2b_length,
                                        last);

    if(last)
        goto out;
        
    BUG_ON(data_c2b_length != OBJ_IO_MAX_BUFFER_SIZE * C_BLK_SIZE);
    cdb.disk  = c2b->cdb.disk;
    cdb.block = c2b->cdb.block + OBJ_IO_MAX_BUFFER_SIZE;
    debug("Continuing for cdb=(0x%x, 0x%x)\n", cdb.disk, cdb.block);   
    /* TODO: Work out if we don't go into unbound recursion here */
    castle_rxrpc_get_call_set(call, c2b, data_c2b_length, data_length, 0 /* not first any more */);
    castle_object_get_continue(c_bvec,
                               call,
                               cdb,
                               data_length);
    return;

out:    
    debug("Finishing with call %p, putting c2b->cdb=(0x%x, 0x%x)\n",
        call, c2b->cdb.disk, c2b->cdb.block);
    unlock_c2b(c2b);
    put_c2b(c2b);

    castle_utils_bio_free(c_bvec->c_bio);
}

void castle_object_get_io_end(c2_block_t *c2b, int uptodate)
{
    c_bvec_t *c_bvec = c2b->private;
    struct castle_rxrpc_call *call = c_bvec->c_bio->rxrpc_call;
#ifdef CASTLE_DEBUG    
    c2_block_t *data_c2b;
    uint32_t data_length, data_c2b_length;
    int first;

    castle_rxrpc_get_call_get(call, &data_c2b, &data_c2b_length, &data_length, &first); 
    BUG_ON(c2b != data_c2b);
#endif
    debug("IO end for cdb (0x%x, 0x%x), uptodate=%d\n", 
            c2b->cdb.disk, c2b->cdb.block, uptodate);
    if(uptodate)
        set_c2b_uptodate(c2b);

    INIT_WORK(&c_bvec->work, __castle_object_get_complete);
    queue_work(castle_wq, &c_bvec->work); 
}

void castle_object_get_continue(struct castle_bio_vec *c_bvec,
                                struct castle_rxrpc_call *call,
                                c_disk_blk_t data_cdb,
                                uint32_t data_length)
{
    c2_block_t *c2b, *old_c2b;
    int nr_blocks, first;
    uint32_t data_c2b_length, old_data_length;
    
    BUG_ON(c_bvec->c_bio->rxrpc_call != call);
    castle_rxrpc_get_call_get(call, &old_c2b, &data_c2b_length, &old_data_length, &first);
    debug("get_continue for call %p, data_c2b_length=%d, "
           "old_data_length=%d, data_length=%d, first=%d\n", 
        call, data_c2b_length, old_data_length, data_length, first);
    BUG_ON(data_length != old_data_length);
    /* If old_c2b exists, we must have completed a MAX chunk */
    BUG_ON( old_c2b &&
           (old_c2b->cdb.disk != data_cdb.disk) &&
           (old_c2b->cdb.block + OBJ_IO_MAX_BUFFER_SIZE != data_cdb.block));

    nr_blocks = (data_length - 1) / C_BLK_SIZE + 1; 
    debug("Nr blocks required for entire data: %d\n", nr_blocks);
    /* Work out if we can read the (remaining part of the) object in full,
       or if we are going to be reading just a part of it */
    if(nr_blocks > OBJ_IO_MAX_BUFFER_SIZE)
    {
        nr_blocks = OBJ_IO_MAX_BUFFER_SIZE;
        data_c2b_length = nr_blocks * C_BLK_SIZE;
        debug("Too many blocks required, reducing to %d\n", nr_blocks);
    } else
    {
        data_c2b_length = data_length;
    }
    debug("data_c2b_length=%d, data_length=%d\n", data_c2b_length, data_length);
    data_length -= data_c2b_length; 
    
    debug("Locking cdb (0x%x, 0x%x)\n", data_cdb.disk, data_cdb.block);
    c2b = castle_cache_block_get(data_cdb, nr_blocks);
    lock_c2b(c2b);
    castle_rxrpc_get_call_set(call, c2b, data_c2b_length, data_length, first);
    /* Unlock the old c2b if we had one */
    if(old_c2b)
    {
        debug("Unlocking old_cdb (0x%x, 0x%x)\n", old_c2b->cdb.disk, old_c2b->cdb.block);
        unlock_c2b(old_c2b);
        put_c2b(old_c2b);
    }

    debug("c2b uptodate: %d\n", c2b_uptodate(c2b));
    if(!c2b_uptodate(c2b))
    {
        /* If the buffer doesn't contain up to date data, schedule the IO */
        c2b->private = c_bvec;
        c2b->end_io = castle_object_get_io_end;
        BUG_ON(submit_c2b(READ, c2b));
    } else
    {
        __castle_object_get_complete(&c_bvec->work);
    }
}

void castle_object_get_complete(struct castle_bio_vec *c_bvec, 
                                int err,
                                c_val_tup_t cvt)
{
    struct castle_rxrpc_call *call = c_bvec->c_bio->rxrpc_call;
    c_bio_t *c_bio = c_bvec->c_bio;

    debug("Returned from btree walk with value of type 0x%x and length %u\n", 
          cvt.type, cvt.length);
    /* Sanity checks on the bio */
    BUG_ON(c_bvec_data_dir(c_bvec) != READ); 
    BUG_ON(atomic_read(&c_bio->count) != 1);
    BUG_ON(c_bio->err != 0);

    /* Free the key */
    kfree(c_bvec->key);

    /* Deal with error case, or non-existant value. */
    if(err || CVT_INVALID(cvt) || CVT_TOMB_STONE(cvt))
    {
        debug("Error, invalid or tombstone.\n");
        castle_rxrpc_get_reply_start(call, err, 0, NULL, 0);
        castle_utils_bio_free(c_bvec->c_bio);
        return;
    }

    /* Next, handle inline values, since we already have them in memory */
    if(CVT_INLINE(cvt))
    {
        debug("Inline.\n");
        castle_rxrpc_get_reply_start(call, 0, cvt.length, cvt.val, cvt.length);
        kfree(cvt.val);
        castle_utils_bio_free(c_bvec->c_bio);
        return;
    }

    debug("Out of line.\n");
    /* Finally, out of line values */
    BUG_ON(!CVT_ONDISK(cvt));
    /* Init the variables stored in the call correctly, so that _continue() doesn't
       get confused */
    castle_rxrpc_get_call_set(call, NULL, 0, cvt.length, 1 /* first */);
    castle_object_get_continue(c_bvec, call, cvt.cdb, cvt.length);
}

int castle_object_get(struct castle_rxrpc_call *call, 
                      struct castle_attachment *attachment, 
                      c_vl_okey_t *key)
{
    c_vl_bkey_t *btree_key;
    c_bvec_t *c_bvec;
    c_bio_t *c_bio;

    btree_key = castle_object_key_convert(key);
    castle_object_key_free(key);

    /* Single c_bvec for the bio */
    c_bio = castle_utils_bio_alloc(1);
    if(!c_bio)
        return -ENOMEM;
    BUG_ON(!attachment);
    c_bio->attachment    = attachment; 
    c_bio->rxrpc_call    = call;
    c_bio->data_dir      = READ;

    c_bvec = c_bio->c_bvecs; 
    c_bvec->key        = btree_key; 
    /* Callback cvt_get() is not required for READ */
    c_bvec->cvt_get    = NULL;
    c_bvec->endfind    = castle_object_get_complete;
    c_bvec->da_endfind = NULL; 
    
    /* TODO: add bios to the debugger! */ 

    castle_double_array_find(c_bvec);

    return 0;
}
