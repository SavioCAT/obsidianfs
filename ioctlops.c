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
    switch (cmd) {
        case OBSIDIAN_IOC_PROTECT:
            int err;
            oi->flagsProtected = true;
            mark_inode_dirty(inode);
            err = write_inode_now(inode, 1);
            if (err) {
                pr_info("[OBSIDIANFS] write of the inode %lu failed\n", inode->i_ino);
                return err;
            }
            pr_info("[OBSIDIANFS] inode %lu protected\n", inode->i_ino);
            return 0;

        case OBSIDIAN_IOC_GET_FLAGS_PROTECTED: {
            char value = oi->flagsProtected ? 1 : 0;
            if (copy_to_user((char __user *)arg, &value, sizeof(char)))
                return -EFAULT;
            return 0;
        }
        case OBSIDIAN_IOC_REVERT: {
            struct hlist_node *p;
            unsigned long prev_ino = oi->i_previous_inode;
            unsigned short count_dentry = 0;

            if (prev_ino == 0) {
            	pr_err("[OBSIDIANFS] revert: no previous inode for ino %lu\n", inode->i_ino);
		        return -EINVAL;
            }

            pr_info("[OBSIDIANFS REVERT] ino=%lu -> prev_ino=%lu (i_next=%lu)\n",
                    inode->i_ino, prev_ino, (unsigned long)oi->i_next_inode);

            spin_lock(&inode->i_lock);
            hlist_for_each(p, &inode->i_dentry) {
        	    count_dentry++; // Counter of dentry pointing to the inode
    	    }

            struct dentry **dentry_list = kcalloc(count_dentry, sizeof(struct dentry *), GFP_ATOMIC);
            if (!dentry_list) {
                spin_unlock(&inode->i_lock);
                return -ENOMEM;
            }

            unsigned short i = 0;
            hlist_for_each(p, &inode->i_dentry) {
        	    dentry_list[i] = container_of(p, struct dentry, d_u.d_alias);
                dget(dentry_list[i]);
                i++;
    	    }
            spin_unlock(&inode->i_lock);

            for (short j = 0; j < count_dentry; j++) {
                obsidianfs_update_dir_entry(d_inode(dentry_list[j]->d_parent), &dentry_list[j]->d_name, prev_ino);
                d_drop(dentry_list[j]);
                dput(dentry_list[j]);
            }

            kfree(dentry_list);

            set_nlink(inode, 1);
            mark_inode_dirty(inode);

            struct inode *prev_inode = obsidianfs_iget(inode->i_sb, prev_ino);
            if (!IS_ERR(prev_inode)) {
                set_nlink(prev_inode, count_dentry);
                mark_inode_dirty(prev_inode);
                iput(prev_inode);
            }

            return 0;
        }
        case OBSIDIAN_IOC_FORWARD: {
            struct hlist_node *p;
            unsigned long next_ino = oi->i_next_inode;
            unsigned short count_dentry = 0;

            if (next_ino == 0) {
            	pr_err("[OBSIDIANFS] revert: no next inode for ino %lu\n", inode->i_ino);
		        return -EINVAL;
            }

            spin_lock(&inode->i_lock);
            hlist_for_each(p, &inode->i_dentry) {
        	    count_dentry++; // Counter of dentry pointing to the inode
    	    }

            struct dentry **dentry_list = kcalloc(count_dentry, sizeof(struct dentry *), GFP_ATOMIC);
            if (!dentry_list) {
                spin_unlock(&inode->i_lock);
                return -ENOMEM;
            }

            unsigned short i = 0;
            hlist_for_each(p, &inode->i_dentry) {
        	    dentry_list[i] = container_of(p, struct dentry, d_u.d_alias);
                dget(dentry_list[i]);
                i++;
    	    }
            spin_unlock(&inode->i_lock);

            for (short j = 0; j < count_dentry; j++) {
                obsidianfs_update_dir_entry(d_inode(dentry_list[j]->d_parent), &dentry_list[j]->d_name, next_ino);
                d_drop(dentry_list[j]);
                dput(dentry_list[j]);
            }

            kfree(dentry_list);

            set_nlink(inode, 1);
            mark_inode_dirty(inode);

            struct inode *next_inode = obsidianfs_iget(inode->i_sb, next_ino);
            if (!IS_ERR(next_inode)) {
                pr_info("[OBSIDIANFS] forward: ino %lu -> next_ino %lu (mode=0%o size=%lld nlink=%u)\n",
                        inode->i_ino, next_ino, next_inode->i_mode,
                        next_inode->i_size, next_inode->i_nlink);
                set_nlink(next_inode, count_dentry);
                mark_inode_dirty(next_inode);
                iput(next_inode);
            } else {
                pr_err("[OBSIDIANFS] forward: iget(%lu) failed: %ld\n",
                       next_ino, PTR_ERR(next_inode));
            }

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
