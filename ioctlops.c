#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/ioctl.h>
#include <linux/dcache.h>
#include "ioctlops.h"
#include "inode.h"

long obsidianfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct inode *inode = file_inode(file);
    struct obsidianfs_inode_meta *oi = OBSIDIANFS_INODE(inode);
    //struct dentry *dentry = file->f_path.dentry;
    //struct super_block *sb = inode->i_sb;

    pr_info("[OBSIDIANFS] ioctl cmd=0x%x on inode %lu\n", cmd, inode->i_ino);

    switch (cmd) {
        case OBSIDIAN_IOC_PROTECT:
            oi->flagsProtected = true;
            mark_inode_dirty(inode);
            pr_info("[OBSIDIANFS] inode %lu protected\n", inode->i_ino);
            return 0;

        case OBSIDIAN_IOC_GET_FLAGS_PROTECTED: {
            char value = oi->flagsProtected ? 1 : 0;
            if (copy_to_user((char __user *)arg, &value, sizeof(char)))
                return -EFAULT;
            return 0;
        }
        case OBSIDIAN_IOC_REVERT: {
            // WIP REVERT INODE
            return 0;
        }
        case OBSIDIAN_IOC_FORWARD: {
            // WIP FORWARD INODE
            return 0;
        }
        case OBSIDIAN_IOC_DENTRY_TEST: {
        	struct hlist_node *p;
        	
        	spin_lock(&inode->i_lock);
        	hlist_for_each(p, &inode->i_dentry) {
        		struct dentry *dentry_test = container_of(p, struct dentry, d_u.d_alias);
        		pr_err("[OBSIDIANFS] alias found: %s\n", dentry_test->d_name.name);
    		}
        	spin_unlock(&inode->i_lock);
        	
        	return 0;
        }
        default:
            return -ENOTTY;  /* std error for unknow IOCTL */
    }
}
