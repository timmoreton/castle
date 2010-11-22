#include <linux/list.h>
#include <linux/slab.h>
#include <linux/hardirq.h>
#include <linux/sched.h>

#include "castle_public.h"
#include "castle_utils.h"
#include "castle.h"
#include "castle_da.h"
#include "castle_versions.h"
#include "castle_sysfs.h"
#include "castle_cache.h"
#include "castle_events.h"
#include "castle_ctrl.h"

//#define DEBUG
#ifndef DEBUG
#define debug(_f, ...)  ((void)0)
#else
#define debug(_f, _a...)  (printk("%s:%.4d: " _f, __FILE__, __LINE__ , ##_a))
#endif

static int castle_versions_process(void);

static struct kmem_cache *castle_versions_cache  = NULL;

#define CASTLE_VERSIONS_HASH_SIZE       (1000)
static struct list_head  *castle_versions_hash   = NULL;
static          LIST_HEAD(castle_versions_init_list);

static version_t          castle_versions_last   = INVAL_VERSION;
static c_mstore_t        *castle_versions_mstore = NULL;

#define CV_INITED_BIT             (0)
#define CV_INITED_MASK            (1 << CV_INITED_BIT)
#define CV_ATTACHED_BIT           (1)
#define CV_ATTACHED_MASK          (1 << CV_ATTACHED_BIT)
#define CV_SNAP_BIT               (2)
#define CV_SNAP_MASK              (1 << CV_SNAP_BIT)
#define CV_FTREE_LOCKED_BIT       (3)
#define CV_FTREE_LOCKED_MASK      (1 << CV_FTREE_LOCKED_BIT)
struct castle_version {
    /* Various tree links */
    version_t                  version;
    union {
        version_t              parent_v;  /* Vaild if !inited */
        struct castle_version *parent;    /* Vaild if  inited */
    };
    struct castle_version     *first_child;
    struct castle_version     *next_sybling;

    /* Aux data */
    version_t        o_order;
    version_t        r_order;
    da_id_t          da_id;
    c_byte_off_t     size;

    /* Lists for storing versions the hash table & the init list*/
    struct list_head hash_list; 
    unsigned long    flags;
    struct list_head init_list;
};

enum {
    NEW_VER,
    UPDATE_VER,
    REM_VER,
};

DEFINE_HASH_TBL(castle_versions, castle_versions_hash, CASTLE_VERSIONS_HASH_SIZE, struct castle_version, hash_list, version_t, version);

static int castle_version_hash_remove(struct castle_version *v, void *unused) 
{
    list_del(&v->hash_list);
    kmem_cache_free(castle_versions_cache, v);

    return 0;
}

static void castle_versions_hash_destroy(void)
{
    castle_versions_hash_iterate(castle_version_hash_remove, NULL); 
    castle_free(castle_versions_hash);
}

static void castle_versions_init_add(struct castle_version *v)
{
    v->flags &= (~CV_INITED_MASK);
    list_add(&v->init_list, &castle_versions_init_list);
}

version_t castle_version_max_get(void)
{
    return castle_versions_last + 1;
}

static void castle_versions_drop(struct castle_version *p);
static int castle_version_writeback(struct castle_version *v, void *unused);

static struct castle_version * castle_version_delete(struct castle_version *v)
{
    struct castle_version *parent;

    if (!v)
        return NULL;

    /* Sanity check flags. */
    BUG_ON(test_bit(CV_ATTACHED_BIT, &v->flags));
    BUG_ON(!test_bit(CV_INITED_BIT, &v->flags));

    parent = v->parent;

    /* Remove version from hash. */
    castle_sysfs_version_del(v->version);
    castle_versions_drop(v);
    __castle_versions_hash_remove(v);
    kmem_cache_free(castle_versions_cache, v);
    
    /* raise event */
    castle_events_version_destroy(v->version);

    return parent;
}

int castle_version_tree_delete(version_t version)
{
    struct castle_version *v, *cur;
    int ret = 0;

    v = castle_versions_hash_get(version);
    if (!v)
    {
        printk("Asked to delete a non-existent version: %u\n", version);
        ret = -EINVAL;
        goto error_out;
    }

    spin_lock_irq(&castle_versions_hash_lock);
    BUG_ON(!(v->flags & CV_INITED_MASK));
    cur = v;
    while (1)
    {
        /* Check if the version is leaf. */
        if (!cur->first_child)
        {
            int done = 0;

            /* If the node to be deleted is cur, then exit. */
            if (cur == v)
                done = 1;

            /* Delete version and handle Parent. castle_version_delete()
             * returns parent of the deleted node. */
            cur = castle_version_delete(cur);
            if (cur == NULL)
            {
                ret = -EINVAL;
                goto error_out;
            }

            if (done)
                break;
            else
                continue;
        }

        /* For non-leaf nodes, delete first child. */
        cur = cur->first_child;
    }
    spin_unlock_irq(&castle_versions_hash_lock);

    /* Run processing to re-calculate the version ordering. */
    castle_versions_process();

error_out:
    return ret;
}

static struct castle_version* castle_version_add(version_t version, 
                                                 version_t parent, 
                                                 da_id_t da_id,
                                                 c_byte_off_t size)
{
    struct castle_version *v;
    static atomic_t version_cnt = ATOMIC(0);
    
    if(atomic_inc_return(&version_cnt) > 900)
    {
        printk("Beta cannot create more than 900 versions.\n");
        return NULL;
    }

    v = kmem_cache_alloc(castle_versions_cache, GFP_KERNEL);
    if (!v)
        goto out_dealloc;
    
    debug("Adding: (v, p)=(%d,%d)\n", version, parent);
    
    v->version      = version;
    v->parent_v     = parent;
    v->first_child  = NULL; 
    v->next_sybling = NULL; 
    v->o_order      = INVAL_VERSION;
    v->r_order      = INVAL_VERSION;
    v->da_id        = da_id;
    v->size         = size; 
    v->flags        = 0;
    INIT_LIST_HEAD(&v->hash_list);
    INIT_LIST_HEAD(&v->init_list);

    /* Initialise version 0 fully, defer full init of all other versions by 
       putting it on the init list. */ 
    if(v->version == 0)
    {
        if(castle_sysfs_version_add(v->version))
            goto out_dealloc;

        v->parent       = NULL;
        v->first_child  = NULL; /* This will be updated later */
        v->next_sybling = NULL;
        v->flags       |= CV_INITED_MASK;

        castle_versions_hash_add(v);
    } else
    {
        /* Defer the initialisation until all the ancestral nodes are
           available. */
        castle_versions_init_add(v);
        castle_versions_hash_add(v);
    }

    return v;

out_dealloc:
    kmem_cache_free(castle_versions_cache, v);

    return NULL;
}

da_id_t castle_version_da_id_get(version_t version)
{
    struct castle_version *v;
    da_id_t da_id;

    spin_lock_irq(&castle_versions_hash_lock);
    v = __castle_versions_hash_get(version);
    /* Sanity checks */
    BUG_ON(!v);
    BUG_ON(!(v->flags & CV_INITED_MASK));
    da_id = v->da_id;
    spin_unlock_irq(&castle_versions_hash_lock);
 
    return da_id; 
}

/* TODO who should handle errors in writeback? */
static int castle_version_writeback(struct castle_version *v, void *unused)
{
    struct castle_vlist_entry mstore_ventry;
    
    debug("Writing back version %d\n", v->version);

    mstore_ventry.version_nr = v->version;
    mstore_ventry.parent     = (v->parent ? v->parent->version : 0);
    mstore_ventry.size       = v->size;
    mstore_ventry.da_id      = v->da_id;

    spin_unlock_irq(&castle_versions_hash_lock);
    castle_mstore_entry_insert(castle_versions_mstore, &mstore_ventry);
    spin_lock_irq(&castle_versions_hash_lock);

    return 0;
}

int castle_versions_writeback(void)
{ /* Should be called with castle_ctrl_lock() held. */
    BUG_ON(castle_versions_mstore);

    castle_versions_mstore = 
        castle_mstore_init(MSTORE_VERSIONS_ID, sizeof(struct castle_vlist_entry));
    if(!castle_versions_mstore)
        return -ENOMEM;
    
    /* Writeback new copy. */
    castle_versions_hash_iterate(castle_version_writeback, NULL);

    castle_mstore_fini(castle_versions_mstore);
    castle_versions_mstore = NULL;

    return 0;
}

/***** External functions *****/
static struct castle_version* castle_version_new_create(int snap_or_clone,
                                                        version_t parent,
                                                        da_id_t da_id,
                                                        c_byte_off_t size)
{
    struct castle_version *v, *p;
    c_byte_off_t parent_size;
    version_t version;

    /* Read ftree root from the parent (also, make sure parent exists) */
    p = castle_versions_hash_get(parent);
    if(!p)
    {
        printk("Asked to create a child of non-existant parent: %d\n",
            parent);
        return NULL;
    }
    
    parent_size = p->size;

    /* Allocate a new version number. */
    BUG_ON(VERSION_INVAL(castle_versions_last));
    version = ++castle_versions_last;
    BUG_ON(VERSION_INVAL(castle_versions_last));

    /* Try to add it to the hash. Use the da_id provided or the parent's */
    BUG_ON(!DA_INVAL(da_id) && !DA_INVAL(p->da_id));
    da_id = DA_INVAL(da_id) ? p->da_id : da_id;
    v = castle_version_add(version, parent, da_id, size);
    if(!v) 
        return NULL;
    
    /* If our parent has the size set, inherit it (ignores the size argument) */
    if(parent_size != 0)
        v->size = parent_size;
    
    /* Set clone/snap bit in flags */ 
    if(snap_or_clone)
        v->flags |= CV_SNAP_MASK;
    else
        v->flags &= ~CV_SNAP_MASK;

    /* Run processing (which will thread the new version into the tree,
       and recalculate the order numbers) */
    castle_versions_process(); 
    
    /* Check if the version got initialised */
    if(!(v->flags & CV_INITED_MASK))
    {
        spin_lock_irq(&castle_versions_hash_lock);
        __castle_versions_hash_remove(v);
        spin_unlock_irq(&castle_versions_hash_lock);
        kmem_cache_free(castle_versions_cache, v);

        return NULL;
    }

    castle_events_version_create(version);

    return v;
}

version_t castle_version_new(int snap_or_clone,
                             version_t parent,
                             da_id_t da_id,
                             c_byte_off_t size)
{
    struct castle_version *v;
    
    debug("New version: snap_or_clone=%d, parent=%d, size=%lld\n",
            snap_or_clone, parent, size);
    /* Get a new version number */
    v = castle_version_new_create(snap_or_clone,
                                  parent,
                                  da_id,
                                  size);
        
    /* Return if we couldn't create the version correctly
       (possibly because we trying to clone attached version,
        or because someone asked for more than one snapshot to
        an attached version */
    if(!v)
        return INVAL_VERSION;

    /* We've succeeded at creating a new version number.
       Let's find where to store it on the disk. */

    return v->version; 
}

int castle_version_attach(version_t version) 
{
    struct castle_version *v;

    v = castle_versions_hash_get(version);
    if(!v)
        return -EINVAL;

    if(test_and_set_bit(CV_ATTACHED_BIT, &v->flags))
        return -EAGAIN;

    return 0;
}

int castle_version_read(version_t version, 
                        da_id_t *da,
                        version_t *parent,
                        c_byte_off_t *size,
                        int *leaf)
{
    struct castle_version *v;

    v = castle_versions_hash_get(version);
    if(!v)
        return -EINVAL;
    
    /* Set these even if we fail to set the attached bit */
    if(da)     *da     =  v->da_id;
    if(size)   *size   =  v->size;
    if(parent) *parent =  v->parent ? v->parent->version : 0;
    if(leaf)   *leaf   = (v->first_child == NULL);

    return 0;
} 

void castle_version_detach(version_t version)
{
    struct castle_version *v;

    v = castle_versions_hash_get(version);
    BUG_ON(!v);
    BUG_ON(!test_and_clear_bit(CV_ATTACHED_BIT, &v->flags));
}

static void castle_versions_insert(struct castle_version *p,
                                   struct castle_version *v)
{
    struct castle_version *sybling_list;
    struct castle_version **pprev;

    /* We know who our parent is */
    v->parent = p;
    /* Sybling list starts with whatever the parent is pointing at. pprev will point to
       the address where sybling list was stored. This allows us to update it without
       special casing first child etc. */
    pprev = &p->first_child;
    sybling_list = *pprev;
    while(sybling_list && (sybling_list->version > v->version))
    {
        pprev = &sybling_list->next_sybling;
        sybling_list = *pprev;
    }
    BUG_ON(!pprev);
    v->next_sybling = sybling_list;
    *pprev = v;
}

static void castle_versions_drop(struct castle_version *v)
{
    struct castle_version *sybling_list, *prev, *p;

    if (!v)
        return;

    p = v->parent;
    prev = NULL;
    sybling_list = p->first_child;
    while(sybling_list)
    {
        if (sybling_list == v)
        {
            if (prev)
                prev->next_sybling = v->next_sybling;
            else
                p->first_child = v->next_sybling;
        }
        prev = sybling_list;
        sybling_list = sybling_list->next_sybling;
    }
    v->next_sybling = v->parent = NULL;
}

static int castle_versions_process(void)
{
    struct castle_version *v, *p, *n;
    LIST_HEAD(sysfs_list); 
    version_t id;
    int children_first, ret;
    int err = 0;

    spin_lock_irq(&castle_versions_hash_lock);
    /* Start processing elements from the init list, one at the time */
    while(!list_empty(&castle_versions_init_list))
    {
        v = list_first_entry(&castle_versions_init_list, 
                              struct castle_version,
                              init_list);
process_version:        
        /* Remove the element from the list */
        list_del(&v->init_list);
        BUG_ON(v->flags & CV_INITED_MASK);

        /* Find it's parent, and check if it's been inited already */
        p = __castle_versions_hash_get(v->parent_v);
        BUG_ON(!p);
        debug("Processing version: %d, parent: %d\n", v->version, p->version);
        /* We can only snapshot leaf nodes */ 
        if((v->flags & CV_SNAP_MASK) &&   /* version is a snapshot    */
           (p->first_child != NULL))      /* there already is a child */
        {
            printk("Warn: ignoring snapshot: %d, parent: %d has a child %d already.\n",
                    v->version, p->version, p->first_child->version);
            err = -1;
            continue;
        }
        /* Clones can only be made if the parent isn't attached writeably
           Which is the same as to say that the parent is a leaf */
        if(!(v->flags & CV_SNAP_MASK) &&        /* version is a clone */
            (p->flags & CV_ATTACHED_MASK) &&    /* parent is attached */
            (p->first_child == NULL))           /* parent is a leaf   */
        {
            printk("Warn: ignoring clone: %d, parent: %d is a leaf.\n",
                    v->version, p->version);
            err = -2;
            continue;
        }
        /* If the parent hasn't been initialised yet, initialise it instead */
        if(!(p->flags & CV_INITED_MASK))
        {
            /* Re-add v back to the init list.
               Because the element is added to the front of the list O(n) is guaranteed.
               This is because after following parent pointers up to the root of the tree,
               we will come back down initialising all children on the path. */
            list_add(&v->init_list, &castle_versions_init_list);
            debug("Changing version to parent.\n");
            /* Set v to the parent */
            v = p;
            /* Retry processing, this time starting with the parent. 
               This has to succeed at some point because version 0 
               is guaranteed to be inited */
            goto process_version;
        }
        /* If we got here we know that the parent has been inited */
        debug(" Parent initialised, (v,p)=(%d,%d)\n", v->version, p->version);
        /* Insert v at the start of the sybling list. */
        castle_versions_insert(p, v);
        list_add(&v->init_list, &sysfs_list);

        /* We are done setting this version up. */
        v->flags |= CV_INITED_MASK;
    }
    debug("Done with tree init.\n");

    /* Now, once the tree has been built, assign the order to the nodes
       We assign two id's to each node. o_order is based on when is the node 
       visited first time in DFS, r_order when the node is visited last. 
       The code below implements non-recursive DFS (we don't have enough stack for
       potentialy deep recursion */  
    v = __castle_versions_hash_get(0); 
    BUG_ON(!v);
    BUG_ON(!(v->flags & CV_INITED_MASK));
    BUG_ON(v->parent);
    id = 0;
    children_first = 1;
    
    while(v)
    {
        debug("Looking at version: %d\n", v->version);
        n = NULL;
        /* If going down the tree select the next node in the following order
           of preference:
           - first child
           - next sybling
           - parent
           On the way up select:
           - next sybling
           - parent
           Note that the next sybling & parent cases are common to both cases.
           Also, if the parent is selected, make sure 'children_first' is not set */
        if(children_first)
        {
            v->o_order = ++id;
            debug("Assigned version=%d o_order %d\n", v->version, v->o_order);
            /* Only attempt to go to the child on the way down the tree */
            n = v->first_child;
            /* Special case for leaf nodes: r_order = o_order */
            if(!n) v->r_order = v->o_order;
        } else
        {
            /* Assign the r order first (the id of the last decendant) */
            v->r_order = id;
            debug("Assigned version=%d r_order %d\n", v->version, v->r_order);
        }
        children_first = 1;
        if(!n) 
            n = v->next_sybling;
        if(!n) {
            n = v->parent;
            children_first = 0;
        }
        if(n) debug("Next version is: %d\n", n->version);
        v = n;
    }
    spin_unlock_irq(&castle_versions_hash_lock);

    while(!list_empty(&sysfs_list))
    {
        v = list_first_entry(&sysfs_list, 
                              struct castle_version,
                              init_list);
        list_del(&v->init_list);
        /* Now that we are done setting the version up, try to add it to sysfs. */
        ret = castle_sysfs_version_add(v->version);
        if(ret)
        {
            printk("Could not add version %d to sysfs. Errno=%d.\n", v->version, ret);
            err = -3;
            continue; 
        }
    }
 
    /* Done. */
    return err;
}

int castle_version_is_ancestor(version_t candidate, version_t version)
{
    struct castle_version *c, *v;
    int ret;

    spin_lock_irq(&castle_versions_hash_lock);
    v = __castle_versions_hash_get(version);
    c = __castle_versions_hash_get(candidate);
    /* Sanity checks */
    BUG_ON(!v);
    BUG_ON(!(v->flags & CV_INITED_MASK));
    BUG_ON(VERSION_INVAL(v->o_order));
    BUG_ON(!c);
    BUG_ON(!(c->flags & CV_INITED_MASK));
    BUG_ON(VERSION_INVAL(c->o_order));
    BUG_ON(VERSION_INVAL(c->r_order));
    /* c is an ancestor of v if v->o_order is in range c->o_order to c->r_order
       inclusive */
    ret = (v->o_order >= c->o_order) && (v->o_order <= c->r_order);
    spin_unlock_irq(&castle_versions_hash_lock);

    return ret;
}

int castle_version_compare(version_t version1, version_t version2)
{
    struct castle_version *v1, *v2;
    int ret;

    spin_lock_irq(&castle_versions_hash_lock);
    v1 = __castle_versions_hash_get(version1);
    v2 = __castle_versions_hash_get(version2);
    /* Sanity checks */
    BUG_ON(!v1);
    BUG_ON(!(v1->flags & CV_INITED_MASK));
    BUG_ON(VERSION_INVAL(v1->o_order));
    BUG_ON(!v2);
    BUG_ON(!(v2->flags & CV_INITED_MASK));
    BUG_ON(VERSION_INVAL(v2->o_order));

    ret = v1->o_order - v2->o_order;
    spin_unlock_irq(&castle_versions_hash_lock);

    return ret;
}

int castle_versions_zero_init(void)
{
    struct castle_version *v;

    debug("Initialising version root.\n");

    v = castle_version_add(0, 0, INVAL_DA, 0);
    if (!v)
    {
        printk("Failed to create verion ZERO\n");
        return -1;
    }
    castle_versions_last = v->version;

    return 0;
}

int castle_versions_read(void)
{
    struct castle_vlist_entry mstore_ventry;
    struct castle_mstore_iter* iterator = NULL;
    struct castle_version* v; 
    c_mstore_key_t key;
    int ret = 0;

    BUG_ON(castle_versions_mstore);
    castle_versions_mstore = 
        castle_mstore_open(MSTORE_VERSIONS_ID, sizeof(struct castle_vlist_entry));

    if(!castle_versions_mstore)
    {
        ret = -ENOMEM;
        goto out;
    }

    iterator = castle_mstore_iterate(castle_versions_mstore);
    if(!iterator)
    {
        ret = -EINVAL;
        goto out;
    }

    while(castle_mstore_iterator_has_next(iterator))
    {
        castle_mstore_iterator_next(iterator, &mstore_ventry, &key);
        v = castle_version_add(mstore_ventry.version_nr, 
                               mstore_ventry.parent, 
                               mstore_ventry.da_id,
                               mstore_ventry.size);
        if(!v)
        {
            ret = -ENOMEM;
            goto out;
        }
        if(VERSION_INVAL(castle_versions_last) || v->version > castle_versions_last)
            castle_versions_last = v->version;
    }
    ret = castle_versions_process(); 

out:
    if (iterator)               castle_mstore_iterator_destroy(iterator);
    if (castle_versions_mstore) castle_mstore_fini(castle_versions_mstore);
    castle_versions_mstore = NULL;

    return ret; 
}

/***** Init/fini functions *****/
int castle_versions_init(void)
{
    int ret;

    ret = -ENOMEM;
    castle_versions_cache = kmem_cache_create("castle_versions",
                                               sizeof(struct castle_version),
                                               0,     /* align */
                                               0,     /* flags */
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,18)
                                               NULL, NULL); /* ctor, dtor */
#else                                               
                                               NULL); /* ctor */
#endif

    if(!castle_versions_cache)
    {
        printk("Could not allocate kmem cache for castle versions.\n");
        goto err_out;
    }
    
    castle_versions_hash = castle_versions_hash_alloc();
    if(!castle_versions_hash)
    {
        printk("Could not allocate versions hash\n");
        goto err_out;
    }
    castle_versions_hash_init();

    return 0;

err_out:
    if(castle_versions_cache)
        kmem_cache_destroy(castle_versions_cache);
    if(castle_versions_hash)
        castle_free(castle_versions_hash);
    return ret;
}

void castle_versions_fini(void)
{
    castle_versions_hash_destroy();
    kmem_cache_destroy(castle_versions_cache);
}
