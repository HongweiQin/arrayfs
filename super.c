/*
 * Arrayfs is a simple file system using an array as back storage.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/statfs.h>
#include <linux/buffer_head.h>
#include <linux/backing-dev.h>
#include <linux/kthread.h>
#include <linux/parser.h>
#include <linux/mount.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/random.h>
#include <linux/exportfs.h>
#include <linux/blkdev.h>
#include <linux/quotaops.h>
#include <linux/sysfs.h>
#include <linux/quota.h>
#include <linux/pagevec.h>
#include <linux/mm_inline.h>
#include <linux/uio.h>
#include <linux/sched/signal.h>

/* Eight directory inodes */
#define ARRAY_FS_NR_DIRINODES (8)

#define ARRAYFS_NR_INODES (32)
#define ARRAYFS_NR_PGS_PER_FILE (8)


struct arrayfs_sb {
	spinlock_t m_lock;
	int mounted;
	struct super_block *sb;
	spinlock_t inode_bmlock;
	unsigned long inode_bm;
	spinlock_t cp_lock;
};

struct arrayfs_inode {
	struct inode vfs_inode;
};

struct arrayfs_disk_inode {
	umode_t mode;
	unsigned long size;
};

struct arrayfs_dir_entry {
	char name[32];
	u32 ino;
};

struct arrayfs_dir_data {
	unsigned long bitmap;
	struct arrayfs_dir_entry entries[64];
};

static struct inode *arrayfs_iget(struct super_block *sb, unsigned long ino);
const struct inode_operations arrayfs_dir_iops;
const struct inode_operations arrayfs_file_iops;
const struct file_operations arrayfs_dir_operations;
const struct file_operations arrayfs_file_operations;
const struct address_space_operations arrayfs_file_aops;


/* These are in-memory inodes. So we don't need to allocate them dynamically. */
struct arrayfs_inode memory_inodes[ARRAYFS_NR_INODES];

/* These are data storage */
struct arrayfs_sb global_sb;
struct arrayfs_disk_inode global_inodes[ARRAYFS_NR_INODES];
char global_data[ARRAYFS_NR_INODES][ARRAYFS_NR_PGS_PER_FILE][PAGE_SIZE];
unsigned long disk_inode_bm;

static inline struct arrayfs_inode *ARRAYFS_I(struct inode *inode)
{
	return container_of(inode, struct arrayfs_inode, vfs_inode);
}

static inline struct arrayfs_sb *ARRAYFS_I_SB(struct inode *inode)
{
	return &global_sb;
}

static struct inode *arrayfs_new_inode(struct inode *dir, umode_t mode)
{
	struct arrayfs_sb *sbi = ARRAYFS_I_SB(dir);
	unsigned long ino;
	struct inode *inode;
	int err;
	struct arrayfs_disk_inode *di;

	inode = new_inode(dir->i_sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	spin_lock(&sbi->cp_lock);
	ino = find_first_zero_bit(&disk_inode_bm, ARRAYFS_NR_INODES);
	if (ino == ARRAYFS_NR_INODES) {
		spin_unlock(&sbi->cp_lock);
		err = -ENOSPC;
		goto fail;
	}
	set_bit(ino, &disk_inode_bm);
	spin_unlock(&sbi->cp_lock);

	pr_notice("%s, allocate new disk inode, pa=%lu\n",
					__func__, ino);
	di = &global_inodes[ino];
	di->mode = mode;
	di->size = 0;

	inode_init_owner(inode, dir, mode);

	inode->i_ino = ino;
	inode->i_mtime = inode->i_atime = inode->i_ctime =
			current_time(inode);

	err = insert_inode_locked(inode);
	if (err) {
		err = -EINVAL;
		goto failfree;
	}

	return inode;
failfree:
	spin_lock(&sbi->cp_lock);
	clear_bit(ino, &disk_inode_bm);
	spin_unlock(&sbi->cp_lock);
fail:
	iput(inode);
	return ERR_PTR(err);
}


static int arrayfs_create(struct inode *dir, struct dentry *dentry, umode_t mode,
						bool excl)
{
	struct inode *inode;
	unsigned long ino = 0;
	unsigned long dirino = dir->i_ino;
	struct arrayfs_dir_data *dir_data;
	unsigned long index;

	if (dirino >= ARRAYFS_NR_INODES)
		return -EINVAL;

	//TODO: competition here
	dir_data = (struct arrayfs_dir_data *)global_data[dirino][0];
	index = find_first_zero_bit(&dir_data->bitmap, 64);
	if (index == 64) {
		pr_err("%s, not enough space for dir. ino = %lu\n",
					__func__, dirino);
		return -ENOSPC;
	}
	set_bit(index, &dir_data->bitmap);

	inode = arrayfs_new_inode(dir, mode);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	inode->i_op = &arrayfs_file_iops;
	inode->i_fop = &arrayfs_file_operations;
	inode->i_mapping->a_ops = &arrayfs_file_aops;
	ino = inode->i_ino;

	d_instantiate(dentry, inode);
	unlock_new_inode(inode);

	strcpy(dir_data->entries[index].name, dentry->d_name.name);
	dir_data->entries[index].ino = ino;

	return 0;
}

static int arrayfs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	unsigned long ino = 0;
	unsigned long dirino = dir->i_ino;
	struct arrayfs_dir_data *dir_data;
	unsigned long index;

	if (dirino >= ARRAYFS_NR_INODES)
		return -EINVAL;

	//TODO: competition here
	dir_data = (struct arrayfs_dir_data *)global_data[dirino][0];
	index = find_first_zero_bit(&dir_data->bitmap, 64);
	if (index == 64) {
		pr_err("%s, not enough space for dir. ino = %lu\n",
					__func__, dirino);
		return -ENOSPC;
	}
	set_bit(index, &dir_data->bitmap);

	inode = arrayfs_new_inode(dir, mode);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	inode->i_op = &arrayfs_dir_iops;
	inode->i_fop = &arrayfs_dir_operations;
	ino = inode->i_ino;

	d_instantiate(dentry, inode);
	unlock_new_inode(inode);

	strcpy(dir_data->entries[index].name, dentry->d_name.name);
	dir_data->entries[index].ino = ino;

	return 0;
}

static int str_same(const char *a, const char *b)
{
	int i;

	for (i = 0; i < 32; i++) {
		if (a[i] != b[i])
			return 0;
		if (!a[i])
			return 1;
	}
	return 1;
}

static struct dentry *arrayfs_lookup(struct inode *dir, struct dentry *dentry,
		unsigned int flags)
{
	unsigned long dir_ino = dir->i_ino;
	unsigned long child_ino;
	struct arrayfs_dir_data *dirdata;
	int index = -1;
	struct inode *child_inode = NULL;
	struct dentry *newdentry;

	pr_notice("%s, findname=%s\n",
				__func__, dentry->d_name.name);


	if (dir_ino >= ARRAYFS_NR_INODES)
		return ERR_PTR(-EINVAL);

	dirdata = (struct arrayfs_dir_data *)global_data[dir_ino][0];

	for (;;) {
		index = find_next_bit(&dirdata->bitmap, 64, index + 1);
		if (index >= 64)
			break;

		if (str_same(dirdata->entries[index].name, dentry->d_name.name)) {
			//found
			child_ino = dirdata->entries[index].ino;
			child_inode = arrayfs_iget(ARRAYFS_I_SB(dir)->sb, child_ino);
			if (IS_ERR(child_inode)) {
				pr_err("%s, Can't get inode %lu\n",
							__func__, child_ino);
				return ERR_PTR(-EIO);
			}
			goto outSplice;
		}
	}
	//not found
outSplice:
	newdentry = d_splice_alias(child_inode, dentry);
	return newdentry;
}


const struct inode_operations arrayfs_dir_iops = {
	.create 	= arrayfs_create,
	.mkdir		= arrayfs_mkdir,
	.lookup 	= arrayfs_lookup,
};

const struct inode_operations arrayfs_file_iops = {

};

static int arrayfs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	unsigned long ino = inode->i_ino;
	long long pos = ctx->pos;
	struct arrayfs_dir_data *data;
	unsigned long index;
	unsigned int child_ino;
	unsigned type;
	
	if (ino >= ARRAYFS_NR_INODES)
		return -EINVAL;

	pr_notice("%s, pos=%lld\n",
				__func__, pos);

	data = (struct arrayfs_dir_data *)global_data[ino][0];
	for (;;) {
		index = find_next_bit(&data->bitmap, 64, pos);
		if (index == 64) {
			ctx->pos = pos = 64;
			break;
		} else {
			child_ino = data->entries[index].ino;
			if (child_ino >= ARRAYFS_NR_INODES)
				return 1;
			if (S_ISREG(global_inodes[child_ino].mode))
				type = DT_REG;
			else
				type = DT_DIR;
			pr_notice("%s, diremit, name[%s]\n",
				__func__, data->entries[index].name);
			if (!dir_emit(ctx, data->entries[index].name, strlen(data->entries[index].name),
					child_ino, type))
				return 1;
			ctx->pos = pos = index + 1;
		}
	}
	return 0;
}

static int arrayfs_dir_open(struct inode *inode, struct file *filp)
{
	pr_notice("%s\n", __func__);
	return 0;
}


const struct file_operations arrayfs_dir_operations = {
	.iterate_shared	= arrayfs_readdir,
	.open		= arrayfs_dir_open,
};

loff_t arrayfs_file_llseek(struct file *file, loff_t offset, int whence)
{
	pr_notice("%s\n",
			__func__);
	return generic_file_llseek(file, offset, whence);
}

int arrayfs_file_open(struct inode * inode, struct file * filp)
{
	pr_notice("%s\n",
			__func__);
	return generic_file_open(inode, filp);
}

int arrayfs_file_fsync(struct file *file, loff_t start, loff_t end,
		       int datasync)
{
	struct inode *inode = file->f_mapping->host;
	int err;

	pr_notice("%s\n",
			__func__);

	err = __generic_file_fsync(file, start, end, datasync);
	if (err)
		return err;
	return 0;
}


const struct file_operations arrayfs_file_operations = {
	.llseek		= arrayfs_file_llseek,
	.read_iter	= generic_file_read_iter,
	.write_iter	= generic_file_write_iter,
	.open		= arrayfs_file_open,
	.fsync		= arrayfs_file_fsync,
};

static int arrayfs_read_datapage(struct file *file, struct page *page)
{
	struct inode *inode = page->mapping->host;
	unsigned long ino = inode->i_ino;
	unsigned long index = page->index;

	if (index >= ARRAYFS_NR_PGS_PER_FILE) {
		pr_warning("%s, index=%lu\n",
					__func__, index);
		return 0;
	}
	
	if (ino >= ARRAYFS_NR_INODES) {
		pr_warning("%s, ino=%lu\n",
					__func__, ino);
		return 0;
	}

	memcpy(page_to_virt(page), global_data[ino][index], PAGE_SIZE);
	SetPageUptodate(page);
	pr_notice("%s, ino=%lu, index=%lu, pageflags=0x%lx\n",
				__func__, ino, index, page->flags);
	return 0;
}


static int arrayfs_read_data_pages(struct file *file,
			struct address_space *mapping,
			struct list_head *pages, unsigned nr_pages)
{
	unsigned page_idx;
	gfp_t gfp = mapping_gfp_mask(mapping);

	pr_notice("%s, nr_pages=%u\n",
			__func__, nr_pages);

	for (page_idx = 0; page_idx < nr_pages; page_idx++) {
		struct page *page = lru_to_page(pages);

		list_del(&page->lru);
		if (!add_to_page_cache_lru(page, mapping, page->index, gfp))
			arrayfs_read_datapage(file, page);
		put_page(page);
	}
	return 0;
}

static int arrayfs_write_datapage(struct page *page,
					struct writeback_control *wbc)
{
	struct inode *inode = page->mapping->host;
	unsigned long index = page->index;
	unsigned long ino = inode->i_ino;

	if (index >= ARRAYFS_NR_PGS_PER_FILE) {
		pr_warning("%s, index=%lu\n",
					__func__, index);
		return 0;
	}
	
	if (ino >= ARRAYFS_NR_INODES) {
		pr_warning("%s, ino=%lu\n",
					__func__, ino);
		return 0;
	}
	
	memcpy(global_data[ino][index], page_to_virt(page), PAGE_SIZE);
	clear_page_dirty_for_io(page);
	pr_notice("%s, ino=%lu, index=%lu, pageflags=0x%lx\n",
				__func__, ino, index, page->flags);
	return 0;
}


static int arrayfs_write_data_pages(struct address_space *mapping,
			    struct writeback_control *wbc)
{
	pgoff_t startpage = wbc->range_start >> PAGE_SHIFT;
	pgoff_t endpage = wbc->range_end >> PAGE_SHIFT;
	int tag = PAGECACHE_TAG_TOWRITE;
	unsigned nrpages;
	struct pagevec pvec;

	if (endpage >= ARRAYFS_NR_PGS_PER_FILE)
		endpage = ARRAYFS_NR_PGS_PER_FILE;

	pr_notice("%s, startpage=%lu, endpage=%lu\n",
			__func__, startpage, endpage);

	pagevec_init(&pvec);
	tag_pages_for_writeback(mapping, startpage, endpage);

	while (startpage <= endpage) {
		int i;

		nrpages = pagevec_lookup_range_tag(&pvec, mapping, &startpage, endpage,
				tag);
		if (nrpages == 0)
			break;

		for (i = 0; i < nrpages; i++) {
			struct page *page = pvec.pages[i];

			lock_page(page);
			arrayfs_write_datapage(page, wbc);
			unlock_page(page);
		}
		pagevec_release(&pvec);
	}
	return 0;
}


const struct address_space_operations arrayfs_file_aops = {
	.readpage	= arrayfs_read_datapage,
	.readpages	= arrayfs_read_data_pages,
	.writepage	= arrayfs_write_datapage,
	.writepages	= arrayfs_write_data_pages,
	.write_begin = simple_write_begin,
	.write_end = simple_write_end,
};

static struct inode *arrayfs_alloc_inode(struct super_block *sb)
{
	struct arrayfs_sb *sbi = sb->s_fs_info;
	int pa;
	struct arrayfs_inode *si;

	spin_lock(&sbi->inode_bmlock);
	pa = find_first_zero_bit(&sbi->inode_bm, ARRAYFS_NR_INODES);
	if (pa == ARRAYFS_NR_INODES) {
		spin_unlock(&sbi->inode_bmlock);
		return NULL;
	}
	set_bit(pa, &sbi->inode_bm);
	spin_unlock(&sbi->inode_bmlock);

	si = &memory_inodes[pa];

	inode_init_once(&si->vfs_inode);
	pr_notice("%s, allocate new in-memory inode, pa=%d\n",
				__func__, pa);

	return &si->vfs_inode;
}

static void arrayfs_destroy_inode(struct inode *inode)
{
	struct arrayfs_inode *si = ARRAYFS_I(inode);
	int pa = si - memory_inodes;
	//struct arrayfs_sb *sbi = ARRAYFS_I_SB(inode);

	pr_notice("%s, %d\n", __func__, pa);

	//spin_lock(&sbi->inode_bmlock);
	//clear_bit(pa, &sbi->inode_bm);
	//spin_unlock(&sbi->inode_bmlock);
}

static void arrayfs_put_super(struct super_block *sb)
{
	spin_lock(&global_sb.m_lock);
	global_sb.mounted = 0;
	spin_unlock(&global_sb.m_lock);
}

static const struct super_operations arrayfs_sops = {
	.alloc_inode	= arrayfs_alloc_inode,
	//.drop_inode	= f2fs_drop_inode,
	.destroy_inode	= arrayfs_destroy_inode,
	//.write_inode	= f2fs_write_inode,
	//.dirty_inode	= f2fs_dirty_inode,
	//.show_options	= f2fs_show_options,
	//.evict_inode	= f2fs_evict_inode,
	.put_super	= arrayfs_put_super,
};

static int arrayfs_read_inode(struct inode *inode)
{
	unsigned long ino = inode->i_ino;
	struct arrayfs_disk_inode *di;

	if (ino >= ARRAYFS_NR_INODES)
		return -EINVAL;

	di = &global_inodes[ino];
	inode->i_mode = di->mode;
	inode->i_size = di->size;
	return 0;
}

static struct inode *arrayfs_iget(struct super_block *sb, unsigned long ino)
{
	struct inode *inode;
	int ret;

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	if (!(inode->i_state & I_NEW)) {
		return inode;
	}

	ret = arrayfs_read_inode(inode);
	if (ret)
		goto bad_inode;

	if (S_ISREG(inode->i_mode)) {
		inode->i_op = &arrayfs_file_iops;
		inode->i_fop = &arrayfs_file_operations;
		inode->i_mapping->a_ops = &arrayfs_file_aops;
	} else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &arrayfs_dir_iops;
		inode->i_fop = &arrayfs_dir_operations;
	}
	unlock_new_inode(inode);
	return inode;

bad_inode:
	iget_failed(inode);
	return ERR_PTR(ret);
}

static int arrayfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct arrayfs_sb *sbi;
	struct inode *root_inode;
	int err;

	spin_lock(&global_sb.m_lock);
	if (global_sb.mounted) {
		spin_unlock(&global_sb.m_lock);
		pr_err("%s, already mounted\n",
				__func__);
		return -EBUSY;
	}
	global_sb.mounted = 1;
	spin_unlock(&global_sb.m_lock);
	sb->s_fs_info = sbi = &global_sb;
	sbi->sb = sb;
	sbi->inode_bm = 0;
	spin_lock_init(&sbi->inode_bmlock);
	spin_lock_init(&sbi->cp_lock);
	sb->s_op = &arrayfs_sops;

	/* Deal with root inode */
	root_inode = arrayfs_iget(sb, 0);
	if (IS_ERR(root_inode)) {
		pr_notice("%s, Can't get root inode\n",
					__func__);
		err = PTR_ERR(root_inode);
		goto errout;
	}
	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		err = -ENOMEM;
		goto errout; //No need to free anything
	}

	pr_notice("%s, Mount arrayfs succceed!\n",
			__func__);

	return 0;

errout:
	spin_lock(&global_sb.m_lock);
	global_sb.mounted = 0;
	spin_unlock(&global_sb.m_lock);
	return err;
}

static struct dentry *arrayfs_mount(struct file_system_type *fs_type, int flags,
			const char *dev_name, void *data)
{
	return mount_nodev(fs_type, flags, data, arrayfs_fill_super);
}

static void arrayfs_umount(struct super_block *sb)
{
	kill_anon_super(sb);
}

static struct file_system_type arrayfs_type = {
	.owner		= THIS_MODULE,
	.name		= "arrayfs",
	.mount		= arrayfs_mount,
	.kill_sb	= arrayfs_umount,
	.fs_flags	= FS_REQUIRES_DEV,
};
MODULE_ALIAS_FS("arrayfs");

static void mkfs_arrayfs(void)
{
	struct arrayfs_disk_inode *di = &global_inodes[0];
	struct arrayfs_dir_data *dd = (struct arrayfs_dir_data *)global_data[0][0];

	di->mode = S_IFDIR | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
	di->size = 0;
	disk_inode_bm = 0;
	set_bit(0, &disk_inode_bm);
	dd->bitmap = 0;
}

static int __init init_arrayfs(void)
{
	int err;

	mkfs_arrayfs();

	global_sb.mounted = 0;
	spin_lock_init(&global_sb.m_lock);

	err = register_filesystem(&arrayfs_type);
	if (err)
		goto out;
	pr_notice("%s finished\n", __func__);
	return 0;
out:
	return err;
}

static void __exit exit_arrayfs(void)
{
	pr_notice("%s\n", __func__);
	unregister_filesystem(&arrayfs_type);
}

module_init(init_arrayfs)
module_exit(exit_arrayfs)


MODULE_AUTHOR("Hongwei Qin <glqhw@hust.edu.cn>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Array File System");

