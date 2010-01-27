#include <linux/module.h>
#include <linux/bio.h>
#include <linux/kobject.h>
#include <linux/device-mapper.h>
#include <linux/blkdev.h>
#include <asm/semaphore.h>

#include "castle.h"
#include "castle_block.h"
#include "castle_btree.h"
#include "castle_ctrl.h"
#include "castle_sysfs.h"

struct castle               castle;
struct castle_volumes       castle_volumes;
struct castle_slaves        castle_slaves;
struct castle_devices       castle_devices;
int                         castle_fs_inited;
struct castle_fs_superblock castle_fs_super;


static void castle_fs_superblock_print(struct castle_fs_superblock *fs_sb)
{
    printk("Magic1: %.8x\n"
           "Magic2: %.8x\n"
           "Magic3: %.8x\n"
           "Salt:   %x\n"
           "Pepper: %x\n"
           "F_t_d1: %x\n"
           "F_t_b1: %x\n"
           "F_t_d2: %x\n"
           "F_t_b2: %x\n"
           "R_t_d1: %x\n"
           "R_t_b1: %x\n"
           "R_t_d2: %x\n"
           "R_t_b2: %x\n",
           fs_sb->magic1,
           fs_sb->magic2,
           fs_sb->magic3,
           fs_sb->salt,
           fs_sb->peper,
           fs_sb->fwd_tree_disk1,
           fs_sb->fwd_tree_block1,
           fs_sb->fwd_tree_disk2,
           fs_sb->fwd_tree_block2,
           fs_sb->rev_tree_disk1,
           fs_sb->rev_tree_block1,
           fs_sb->rev_tree_disk2,
           fs_sb->rev_tree_block2);
}

static int castle_fs_superblock_validate(struct castle_fs_superblock *fs_sb)
{
    if(fs_sb->magic1 != 0x19731121) return -1;
    if(fs_sb->magic2 != 0x19880624) return -2;
    if(fs_sb->magic3 != 0x19821120) return -3;

    return 0;
}

static int castle_fs_superblock_read(struct castle_slave *cs,
                                     struct castle_fs_superblock *fs_sb) 
{
    int err;
    
    err = castle_sub_block_read(cs,
                                fs_sb,
                                C_BLK_SIZE, 
                                sizeof(struct castle_fs_superblock));
    if(err)
    {
        printk("Failed to read fs superblock.\n");
        return err;
    }

    castle_fs_superblock_print(fs_sb); 
    err = castle_fs_superblock_validate(fs_sb);
    if(err)
    {
        printk("Invalid superblock.\n");
        return err;
    }

    return 0;
}

static void castle_version_node_destroy(struct castle_vtree_node *v_node)
{
    int i;

    for(i=0; i<v_node->used; i++)
        if(v_node->slots[i].type == VTREE_SLOT_NODE)
            castle_version_node_destroy(v_node->children[i]);
    kfree(v_node);
}

static int castle_version_tree_read(c_disk_blk_t cdb, struct castle_vtree_node **v_node)
{
    struct castle_slave *cs;
    struct castle_vtree_node *vtree_node;
    int i, ret;

    vtree_node = kmalloc(sizeof(struct castle_vtree_node), GFP_KERNEL);
    if(vtree_node == NULL) return -ENOMEM;
    memset(vtree_node, 0, sizeof(struct castle_vtree_node));

    cs = castle_slave_find_by_block(cdb);
    if(cs == NULL) return -ENODEV; 

    ret = castle_sub_block_read(cs,
                                vtree_node,
                                disk_blk_to_offset(cdb),
                                NODE_HEADER); 
    if(ret)
    {
        printk("Could not read version tree root.\n");
        return ret;
    }

    if((vtree_node->capacity > VTREE_NODE_SLOTS) ||
       (vtree_node->used > vtree_node->capacity))
    {
        printk("Invalid vtree root capacity or/and used: (%d, %d)\n",
               vtree_node->capacity, vtree_node->used);
        return ret;
    }
    ret = castle_sub_block_read(cs,
                               &vtree_node->slots,
                                disk_blk_to_offset(cdb) + NODE_HEADER,
                                vtree_node->used * sizeof(struct castle_vtree_slot)); 
    if(ret)
    {
        printk("Could not read version slots.\n");
        return ret;
    }
    for(i=0; i<vtree_node->used; i++)
    {
        if((vtree_node->slots[i].type == VTREE_SLOT_NODE) ||
           (vtree_node->slots[i].type == VTREE_SLOT_NODE_LAST))
        {
            /* Read the child node */
            ret = castle_version_tree_read(vtree_node->slots[i].node.cdb, 
                                           &vtree_node->children[i]);
            /* If failed, cleanup all children read so far */
            if(ret < 0)
            {
                while(i > 0)
                {
                    i--;
                    /* This will decend recursively into sub-children */
                    castle_version_node_destroy(vtree_node->children[i]);
                }
                /* All children freed, free our node */
                kfree(vtree_node);
                return ret;
            }
        } else
        {
            printk("Version slot[%d]: ty= 0x%x\n"
                   "                  vn= 0x%x\n"
                   "                  di= 0x%x\n"
                   "                  bl= 0x%x\n",
                   i,
                   vtree_node->slots[i].type,
                   vtree_node->slots[i].leaf.version_nr,
                   vtree_node->slots[i].leaf.cdb.disk,
                   vtree_node->slots[i].leaf.cdb.block);
        }
    }

    if(v_node) *v_node = vtree_node;
    return 0;
}

int castle_fs_init(void)
{
    struct list_head *lh;
    struct castle_slave *cs;
    struct castle_fs_superblock fs_sb;
    int ret, first;
    // TODO: Temporary, replace with c_disk_blk_t in fs_superblock
    c_disk_blk_t blk;

    if(castle_fs_inited)
        return -EEXIST;

    if(list_empty(&castle_slaves.slaves))
        return -ENOENT;

    first = 1;
    list_for_each(lh, &castle_slaves.slaves)
    {
        cs = list_entry(lh, struct castle_slave, list);
        ret = castle_fs_superblock_read(cs, &fs_sb);  
        if(ret)
        {
            // TODO: invalidate/rebuild the slave! 
            printk("Invaild superblock on slave uuid=0x%x, id=%d, err=%d\n",
                    cs->cs_sb.uuid, cs->id, ret);
            continue;
        }
        /* Save fs superblock if the first slave. */
        if(first)
        {
            memcpy(&castle_fs_super, &fs_sb, sizeof(struct castle_fs_superblock));
            first = 0;
        }
        else
        /* Check if fs superblock the save as the one we already know about */
        {
            if(memcmp(&castle_fs_super, &fs_sb, 
                      sizeof(struct castle_fs_superblock)) != 0)
            {
                printk("Castle fs supreblocks do not match!\n");
                return -EINVAL;
            }
        }
    }

    /* If first is True, we've not found a single cs with valid fs superblock */
    if(first)
        return -ENOENT;

    blk.disk  = castle_fs_super.fwd_tree_disk1;
    blk.block = castle_fs_super.fwd_tree_block1;
    ret = castle_version_tree_read(blk, NULL);
    if(ret)
        return -EINVAL;

    printk("Castle FS inited.\n");
    castle_fs_inited = 1;

    return 0;
}

static void castle_slave_superblock_print(struct castle_slave_superblock *cs_sb)
{
    printk("Magic1: %.8x\n"
           "Magic2: %.8x\n"
           "Magic3: %.8x\n"
           "Uuid:   %x\n"
           "Free:   %x\n"
           "Size:   %x\n",
           cs_sb->magic1,
           cs_sb->magic2,
           cs_sb->magic3,
           cs_sb->uuid,
           cs_sb->free,
           cs_sb->size);
}

static int castle_slave_superblock_validate(struct castle_slave_superblock *cs_sb)
{
    if(cs_sb->magic1 != 0x02061985) return -1;
    if(cs_sb->magic2 != 0x16071983) return -2;
    if(cs_sb->magic3 != 0x16061981) return -3;

    return 0;
}

static int castle_slave_superblock_read(struct castle_slave *cs) 
{
    struct castle_slave_superblock *cs_sb = &cs->cs_sb;
    int err;
    
    err = castle_sub_block_read(cs,
                                cs_sb,
                                0,
                                sizeof(struct castle_slave_superblock));
    if(err) 
    {
        printk("Failed to read superblock.\n");
        return err;
    }

    err = castle_slave_superblock_validate(cs_sb);
    if(err)
    {
        printk("Invalid superblock.\n");
        return err;
    }
    
    return 0;
}

struct castle_slave* castle_slave_find_by_id(uint32_t id)
{
    struct list_head *lh;
    struct castle_slave *slave;

    list_for_each(lh, &castle_slaves.slaves)
    {
        slave = list_entry(lh, struct castle_slave, list);
        if(slave->id == id)
            return slave;
    }

    return NULL;
}

struct castle_slave* castle_slave_find_by_uuid(uint32_t uuid)
{
    struct list_head *lh;
    struct castle_slave *slave;

    list_for_each(lh, &castle_slaves.slaves)
    {
        slave = list_entry(lh, struct castle_slave, list);
        if(slave->cs_sb.uuid == uuid)
            return slave;
    }

    return NULL;
}

struct castle_slave* castle_slave_find_by_block(c_disk_blk_t cdb)
{
    return castle_slave_find_by_uuid(cdb.disk);
}

struct castle_slave* castle_claim(uint32_t new_dev)
{
    dev_t dev;
    struct block_device *bdev;
    int bdev_claimed = 0;
    int err;
    char b[BDEVNAME_SIZE];
    struct castle_slave *cs = NULL;
    static int slave_id = 0;

    if(!(cs = kmalloc(sizeof(struct castle_slave), GFP_KERNEL)))
        goto err_out;
    cs->id = slave_id++;

    dev = new_decode_dev(new_dev);
    bdev = open_by_devnum(dev, FMODE_READ|FMODE_WRITE);
    if (IS_ERR(bdev)) 
    {
        printk("Could not open %s.\n", __bdevname(dev, b));
        goto err_out;
    }
    err = bd_claim(bdev, &castle);
    bdev_claimed = 1;
    if (err) 
    {
        printk("Could not bd_claim %s.\n", bdevname(bdev, b));
        goto err_out;
    }
    cs->bdev = bdev;

    err = castle_slave_superblock_read(cs); 
    if(err)
    {
        printk("Invalid superblock. Not initialised(?)\n");
        goto err_out;
    }

    list_add(&cs->list, &castle_slaves.slaves);
    castle_sysfs_slave_add(cs);

    return cs;
err_out:
    if(bdev_claimed) blkdev_put(bdev);
    if(cs) kfree(cs);
    return NULL;    
}

void castle_release(struct castle_slave *cs)
{
    printk("Releasing slave %x.\n", 
            MKDEV(cs->bdev->bd_disk->major, 
                  cs->bdev->bd_disk->first_minor));
    castle_sysfs_slave_del(cs);
    bd_release(cs->bdev);
    blkdev_put(cs->bdev);
    list_del(&cs->list);
    kfree(cs);
}


static int castle_open(struct inode *inode, struct file *filp)
{
	struct castle_device *dev = inode->i_bdev->bd_disk->private_data;

	filp->private_data = dev;
	spin_lock(&dev->lock);
	if (! dev->users) 
		check_disk_change(inode->i_bdev);
	dev->users++;
	spin_unlock(&dev->lock);
	return 0;
}

static int castle_close(struct inode *inode, struct file *filp)
{
	struct castle_device *dev = inode->i_bdev->bd_disk->private_data;

	spin_lock(&dev->lock);
	dev->users--;
	spin_unlock(&dev->lock);

	return 0;
}

static struct block_device_operations castle_bd_ops = {
	.owner           = THIS_MODULE,
	.open 	         = castle_open,
	.release 	     = castle_close,
	.media_changed   = NULL,
	.revalidate_disk = NULL,
};

static struct block_device* castle_basedisk_claim(dev_t base_dev)
{
    struct block_device *bdev;
    int err;
    char b[BDEVNAME_SIZE];

    bdev = open_by_devnum(base_dev, FMODE_READ|FMODE_WRITE);
    if (IS_ERR(bdev)) {
        printk("Could not open %s.\n", __bdevname(base_dev, b));
        return NULL;
    }
    err = bd_claim(bdev, &castle);
    if (err) {
        printk("Could not bd_claim %s.\n", bdevname(bdev, b));
        blkdev_put(bdev);
        return NULL;
    }

    return bdev;
}

static int castle_device_make_request(struct request_queue *rq, struct bio *bio)
{ 
    struct castle_device *dev = rq->queuedata;

    bio->bi_bdev = dev->bdev;
	generic_make_request(bio);
    return 0;
}

struct castle_device* castle_dev_mirror(dev_t base_dev)
{
    struct castle_device *dev;
    struct request_queue *rq;
    struct gendisk *gd;
    static int minor = 0;

    struct block_device *bdev;

    dev = kmalloc(sizeof(struct castle_device), GFP_KERNEL); 
    if(!dev)
        goto error_out;
    dev->bdev = castle_basedisk_claim(base_dev);
    if(!dev->bdev)
        goto error_out;
	spin_lock_init(&dev->lock);
        
    gd = alloc_disk(1);
    if(!gd)
        goto error_out;

    sprintf(gd->disk_name, "castle-fs-%d", minor);
    gd->major        = castle_devices.major;
    gd->first_minor  = minor++;
	gd->fops         = &castle_bd_ops;
    gd->private_data = dev;

	rq = blk_alloc_queue(GFP_KERNEL);
    if (!rq)
        goto error_out;
	blk_queue_make_request(rq, castle_device_make_request);
	rq->queuedata    = dev;
    gd->queue        = rq;


    list_add(&dev->list, &castle_devices.devices);
    dev->gd = gd;
    set_capacity(gd, get_capacity(dev->bdev->bd_disk));
    add_disk(gd);

    bdev = bdget(MKDEV(gd->major, gd->first_minor));

    return dev;

error_out:
    printk("Failed to mirror device.\n");
    return NULL;    
}

static int castle_slaves_init(void)
{
    memset(&castle_slaves, 0, sizeof(struct castle_slaves));
    INIT_LIST_HEAD(&castle_slaves.slaves);

    return 0;
}

static void castle_slaves_free(void)                                                                 
{                                                                                        
    struct list_head *lh, *th;
    struct castle_slave *slave;

    list_for_each_safe(lh, th, &castle_slaves.slaves)
    {
        slave = list_entry(lh, struct castle_slave, list); 
        castle_release(slave);
    }
}

void castle_device_free(struct castle_device *cd)
{
    bd_release(cd->bdev);
    blkdev_put(cd->bdev);
    del_gendisk(cd->gd);
    put_disk(cd->gd);
    list_del(&cd->list);
    kfree(cd);
}

static int castle_devices_init(void)
{
    int major;

    memset(&castle_devices, 0, sizeof(struct castle_devices));
    INIT_LIST_HEAD(&castle_devices.devices);
    /* Allocate a major for this device */
    if((major = register_blkdev(0, "castle-fs")) < 0) 
    {
        printk("Couldn't register castle device\n");
        return -ENOMEM;
    }
    castle_devices.major = major;

    return 0;
}

static void castle_devices_free(void)                                                                 
{                                                                                        
    struct list_head *lh, *th;
    struct castle_device *dev;

    list_for_each_safe(lh, th, &castle_devices.devices)
    {
        dev = list_entry(lh, struct castle_device, list); 
        castle_device_free(dev);
    }

    if (castle_devices.major)
        unregister_blkdev(castle_devices.major, "castle-fs");
}

static int __init castle_init(void)
{
    int ret;

    printk("Castle FS init ... ");

    castle_fs_inited = 0;
    if((ret = castle_btree_init()))   goto err_out1;
    if((ret = castle_devices_init())) goto err_out2;
    if((ret = castle_slaves_init()))  goto err_out3;
    if((ret = castle_control_init())) goto err_out4;
    if((ret = castle_sysfs_init()))   goto err_out5;

    printk("OK.\n");

    return 0;

    /* Unreachable */
    castle_sysfs_fini();
err_out5:
    castle_control_fini();
err_out4:
    castle_slaves_free();
err_out3:
    castle_devices_free();
err_out2:
    castle_btree_free();
err_out1:

    return ret;
}


static void __exit castle_exit(void)
{
    printk("Castle FS exit ... ");

    castle_sysfs_fini();
    castle_control_fini();
    castle_slaves_free();
    castle_devices_free();
    castle_btree_free();

    printk("done.\n\n\n");
}

module_init(castle_init);
module_exit(castle_exit);

MODULE_LICENSE("GPL");

