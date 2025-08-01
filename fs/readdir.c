// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/readdir.c
 *
 *  Copyright (C) 1995  Linus Torvalds
 */

#include <linux/stddef.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/time.h>
#include <linux/mm.h>
#include <linux/errno.h>
#include <linux/stat.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fsnotify.h>
#include <linux/dirent.h>
#include <linux/security.h>
#include <linux/syscalls.h>
#include <linux/unistd.h>
#include <linux/compat.h>

#include <linux/uaccess.h>

#ifdef CONFIG_KSU_SUSFS_SUS_PATH
#include <linux/susfs_def.h>
extern bool susfs_is_inode_sus_path(struct inode *inode);
extern bool susfs_is_sus_android_data_d_name_found(const char *d_name);
extern bool susfs_is_sus_sdcard_d_name_found(const char *d_name);
extern bool susfs_is_base_dentry_android_data_dir(struct dentry* base);
extern bool susfs_is_base_dentry_sdcard_dir(struct dentry* base);
#endif

int iterate_dir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	bool shared = false;
	int res = -ENOTDIR;
	if (file->f_op->iterate_shared)
		shared = true;
	else if (!file->f_op->iterate)
		goto out;

	res = security_file_permission(file, MAY_READ);
	if (res)
		goto out;

	if (shared)
		res = down_read_killable(&inode->i_rwsem);
	else
		res = down_write_killable(&inode->i_rwsem);
	if (res)
		goto out;

	res = -ENOENT;
	if (!IS_DEADDIR(inode)) {
		ctx->pos = file->f_pos;
		if (shared)
			res = file->f_op->iterate_shared(file, ctx);
		else
			res = file->f_op->iterate(file, ctx);
		file->f_pos = ctx->pos;
		fsnotify_access(file);
		file_accessed(file);
	}
	if (shared)
		inode_unlock_shared(inode);
	else
		inode_unlock(inode);
out:
	return res;
}
EXPORT_SYMBOL(iterate_dir);

/*
 * POSIX says that a dirent name cannot contain NULL or a '/'.
 *
 * It's not 100% clear what we should really do in this case.
 * The filesystem is clearly corrupted, but returning a hard
 * error means that you now don't see any of the other names
 * either, so that isn't a perfect alternative.
 *
 * And if you return an error, what error do you use? Several
 * filesystems seem to have decided on EUCLEAN being the error
 * code for EFSCORRUPTED, and that may be the error to use. Or
 * just EIO, which is perhaps more obvious to users.
 *
 * In order to see the other file names in the directory, the
 * caller might want to make this a "soft" error: skip the
 * entry, and return the error at the end instead.
 *
 * Note that this should likely do a "memchr(name, 0, len)"
 * check too, since that would be filesystem corruption as
 * well. However, that case can't actually confuse user space,
 * which has to do a strlen() on the name anyway to find the
 * filename length, and the above "soft error" worry means
 * that it's probably better left alone until we have that
 * issue clarified.
 */
static int verify_dirent_name(const char *name, int len)
{
	if (!len)
		return -EIO;
	if (memchr(name, '/', len))
		return -EIO;
	return 0;
}

/*
 * Traditional linux readdir() handling..
 *
 * "count=1" is a special case, meaning that the buffer is one
 * dirent-structure in size and that the code can't handle more
 * anyway. Thus the special "fillonedir()" function for that
 * case (the low-level handlers don't need to care about this).
 */

#ifdef __ARCH_WANT_OLD_READDIR

struct old_linux_dirent {
	unsigned long	d_ino;
	unsigned long	d_offset;
	unsigned short	d_namlen;
	char		d_name[1];
};

struct readdir_callback {
	struct dir_context ctx;
	struct old_linux_dirent __user * dirent;
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	struct super_block *sb;
	bool is_base_dentry_android_data_root_dir;
	bool is_base_dentry_sdcard_root_dir;
#endif
	int result;
};

static int fillonedir(struct dir_context *ctx, const char *name, int namlen,
		      loff_t offset, u64 ino, unsigned int d_type)
{
	struct readdir_callback *buf =
		container_of(ctx, struct readdir_callback, ctx);
	struct old_linux_dirent __user * dirent;
	unsigned long d_ino;
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	struct inode *inode;
#endif

	if (buf->result)
		return -EINVAL;
	buf->result = verify_dirent_name(name, namlen);
	if (buf->result < 0)
		return buf->result;
	d_ino = ino;
	if (sizeof(d_ino) < sizeof(ino) && d_ino != ino) {
		buf->result = -EOVERFLOW;
		return -EOVERFLOW;
	}
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	if (buf->is_base_dentry_android_data_root_dir) {
		if (susfs_is_sus_android_data_d_name_found(name)) {
			return true;
		}
	} else if (buf->is_base_dentry_sdcard_root_dir) {
		if (susfs_is_sus_sdcard_d_name_found(name)) {
			return true;
		}
	}

	inode = ilookup(buf->sb, ino);
	if (!inode) {
		goto orig_flow;
	}
	if (susfs_is_inode_sus_path(inode)) {
		iput(inode);
		return true;
	}
	iput(inode);
orig_flow:
#endif
	buf->result++;
	dirent = buf->dirent;
	if (!access_ok(VERIFY_WRITE, dirent,
			(unsigned long)(dirent->d_name + namlen + 1) -
				(unsigned long)dirent))
		goto efault;
	if (	__put_user(d_ino, &dirent->d_ino) ||
		__put_user(offset, &dirent->d_offset) ||
		__put_user(namlen, &dirent->d_namlen) ||
		__copy_to_user(dirent->d_name, name, namlen) ||
		__put_user(0, dirent->d_name + namlen))
		goto efault;
	return 0;
efault:
	buf->result = -EFAULT;
	return -EFAULT;
}

SYSCALL_DEFINE3(old_readdir, unsigned int, fd,
		struct old_linux_dirent __user *, dirent, unsigned int, count)
{
	int error;
	struct fd f = fdget_pos(fd);
	struct readdir_callback buf = {
		.ctx.actor = fillonedir,
		.dirent = dirent
	};
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	struct inode *inode;
#endif

	if (!f.file)
		return -EBADF;

#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	buf.sb = f.file->f_inode->i_sb;
	inode = f.file->f_path.dentry->d_inode;
	if (f.file->f_path.dentry && inode) {
		if (susfs_is_base_dentry_android_data_dir(f.file->f_path.dentry))
		{
			buf.is_base_dentry_android_data_root_dir = true;
			buf.is_base_dentry_sdcard_root_dir = false;
			goto orig_flow;
		}
		if (susfs_is_base_dentry_sdcard_dir(f.file->f_path.dentry))
		{
			buf.is_base_dentry_sdcard_root_dir = true;
			buf.is_base_dentry_android_data_root_dir = false;
			goto orig_flow;
		}
	}
	buf.is_base_dentry_android_data_root_dir = false;
	buf.is_base_dentry_sdcard_root_dir = false;
orig_flow:
#endif

	error = iterate_dir(f.file, &buf.ctx);
	if (buf.result)
		error = buf.result;

	fdput_pos(f);
	return error;
}

#endif /* __ARCH_WANT_OLD_READDIR */

/*
 * New, all-improved, singing, dancing, iBCS2-compliant getdents()
 * interface. 
 */
struct linux_dirent {
	unsigned long	d_ino;
	unsigned long	d_off;
	unsigned short	d_reclen;
	char		d_name[1];
};

struct getdents_callback {
	struct dir_context ctx;
	struct linux_dirent __user * current_dir;
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	struct super_block *sb;
	bool is_base_dentry_android_data_root_dir;
	bool is_base_dentry_sdcard_root_dir;
#endif
	struct linux_dirent __user * previous;
	int count;
	int error;
};

static int filldir(struct dir_context *ctx, const char *name, int namlen,
		   loff_t offset, u64 ino, unsigned int d_type)
{
	struct linux_dirent __user * dirent;
	struct getdents_callback *buf =
		container_of(ctx, struct getdents_callback, ctx);
	unsigned long d_ino;
	int reclen = ALIGN(offsetof(struct linux_dirent, d_name) + namlen + 2,
		sizeof(long));

#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	struct inode *inode;
#endif
	buf->error = verify_dirent_name(name, namlen);
	if (unlikely(buf->error))
		return buf->error;
	buf->error = -EINVAL;	/* only used if we fail.. */
	if (reclen > buf->count)
		return -EINVAL;
	d_ino = ino;
	if (sizeof(d_ino) < sizeof(ino) && d_ino != ino) {
		buf->error = -EOVERFLOW;
		return -EOVERFLOW;
	}
	dirent = buf->previous;
	if (dirent) {
		if (signal_pending(current))
			return -EINTR;
		if (__put_user(offset, &dirent->d_off))
			goto efault;
	}

#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	if (buf->is_base_dentry_android_data_root_dir) {
		if (susfs_is_sus_android_data_d_name_found(name)) {
			return true;
		}
	} else if (buf->is_base_dentry_sdcard_root_dir) {
		if (susfs_is_sus_sdcard_d_name_found(name)) {
			return true;
		}
	}

	inode = ilookup(buf->sb, ino);
	if (!inode) {
		goto orig_flow;
	}
	if (susfs_is_inode_sus_path(inode)) {
		iput(inode);
		return true;
	}
	iput(inode);
orig_flow:
#endif
	dirent = buf->current_dir;
	if (__put_user(d_ino, &dirent->d_ino))
		goto efault;
	if (__put_user(reclen, &dirent->d_reclen))
		goto efault;
	if (copy_to_user(dirent->d_name, name, namlen))
		goto efault;
	if (__put_user(0, dirent->d_name + namlen))
		goto efault;
	if (__put_user(d_type, (char __user *) dirent + reclen - 1))
		goto efault;
	buf->previous = dirent;
	dirent = (void __user *)dirent + reclen;
	buf->current_dir = dirent;
	buf->count -= reclen;
	return 0;
efault:
	buf->error = -EFAULT;
	return -EFAULT;
}

SYSCALL_DEFINE3(getdents, unsigned int, fd,
		struct linux_dirent __user *, dirent, unsigned int, count)
{
	struct fd f;
	struct linux_dirent __user * lastdirent;
	struct getdents_callback buf = {
		.ctx.actor = filldir,
		.count = count,
		.current_dir = dirent
	};
	int error;
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	struct inode *inode;
#endif

	if (!access_ok(VERIFY_WRITE, dirent, count))
		return -EFAULT;

#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	buf.sb = f.file->f_inode->i_sb;
	inode = f.file->f_path.dentry->d_inode;
	if (f.file->f_path.dentry && inode) {
		if (susfs_is_base_dentry_android_data_dir(f.file->f_path.dentry))
		{
			buf.is_base_dentry_android_data_root_dir = true;
			buf.is_base_dentry_sdcard_root_dir = false;
			goto orig_flow;
		}
		if (susfs_is_base_dentry_sdcard_dir(f.file->f_path.dentry))
		{
			buf.is_base_dentry_sdcard_root_dir = true;
			buf.is_base_dentry_android_data_root_dir = false;
			goto orig_flow;
		}
	}
	buf.is_base_dentry_android_data_root_dir = false;
	buf.is_base_dentry_sdcard_root_dir = false;
orig_flow:
#endif

	f = fdget_pos(fd);
	if (!f.file)
		return -EBADF;

	error = iterate_dir(f.file, &buf.ctx);
	if (error >= 0)
		error = buf.error;
	lastdirent = buf.previous;
	if (lastdirent) {
		if (put_user(buf.ctx.pos, &lastdirent->d_off))
			error = -EFAULT;
		else
			error = count - buf.count;
	}
	fdput_pos(f);
	return error;
}

struct getdents_callback64 {
	struct dir_context ctx;
	struct linux_dirent64 __user * current_dir;
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	struct super_block *sb;
	bool is_base_dentry_android_data_root_dir;
	bool is_base_dentry_sdcard_root_dir;
#endif
	struct linux_dirent64 __user * previous;
	int count;
	int error;
};

static int filldir64(struct dir_context *ctx, const char *name, int namlen,
		     loff_t offset, u64 ino, unsigned int d_type)
{
	struct linux_dirent64 __user *dirent;
	struct getdents_callback64 *buf =
		container_of(ctx, struct getdents_callback64, ctx);
	int reclen = ALIGN(offsetof(struct linux_dirent64, d_name) + namlen + 1,
		sizeof(u64));

#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	struct inode *inode;
#endif
	buf->error = verify_dirent_name(name, namlen);
	if (unlikely(buf->error))
		return buf->error;
	buf->error = -EINVAL;	/* only used if we fail.. */
	if (reclen > buf->count)
		return -EINVAL;
	dirent = buf->previous;
	if (dirent) {
		if (signal_pending(current))
			return -EINTR;
		if (__put_user(offset, &dirent->d_off))
			goto efault;
	}

#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	if (buf->is_base_dentry_android_data_root_dir) {
		if (susfs_is_sus_android_data_d_name_found(name)) {
			return true;
		}
	} else if (buf->is_base_dentry_sdcard_root_dir) {
		if (susfs_is_sus_sdcard_d_name_found(name)) {
			return true;
		}
	}

	inode = ilookup(buf->sb, ino);
	if (!inode) {
		goto orig_flow;
	}
	if (susfs_is_inode_sus_path(inode)) {
		iput(inode);
		return true;
	}
	iput(inode);
orig_flow:
#endif
	dirent = buf->current_dir;
	if (__put_user(ino, &dirent->d_ino))
		goto efault;
	if (__put_user(0, &dirent->d_off))
		goto efault;
	if (__put_user(reclen, &dirent->d_reclen))
		goto efault;
	if (__put_user(d_type, &dirent->d_type))
		goto efault;
	if (copy_to_user(dirent->d_name, name, namlen))
		goto efault;
	if (__put_user(0, dirent->d_name + namlen))
		goto efault;
	buf->previous = dirent;
	dirent = (void __user *)dirent + reclen;
	buf->current_dir = dirent;
	buf->count -= reclen;
	return 0;
efault:
	buf->error = -EFAULT;
	return -EFAULT;
}

int ksys_getdents64(unsigned int fd, struct linux_dirent64 __user *dirent,
		    unsigned int count)
{
	struct fd f;
	struct linux_dirent64 __user * lastdirent;
	struct getdents_callback64 buf = {
		.ctx.actor = filldir64,
		.count = count,
		.current_dir = dirent
	};
	int error;
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	struct inode *inode;
#endif

	if (!access_ok(VERIFY_WRITE, dirent, count))
		return -EFAULT;

	f = fdget_pos(fd);
	if (!f.file)
		return -EBADF;

#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	buf.sb = f.file->f_inode->i_sb;
	inode = f.file->f_path.dentry->d_inode;
	if (f.file->f_path.dentry && inode) {
		if (susfs_is_base_dentry_android_data_dir(f.file->f_path.dentry))
		{
			buf.is_base_dentry_android_data_root_dir = true;
			buf.is_base_dentry_sdcard_root_dir = false;
			goto orig_flow;
		}
		if (susfs_is_base_dentry_sdcard_dir(f.file->f_path.dentry))
		{
			buf.is_base_dentry_sdcard_root_dir = true;
			buf.is_base_dentry_android_data_root_dir = false;
			goto orig_flow;
		}
	}
	buf.is_base_dentry_android_data_root_dir = false;
	buf.is_base_dentry_sdcard_root_dir = false;
orig_flow:
#endif
	error = iterate_dir(f.file, &buf.ctx);
	if (error >= 0)
		error = buf.error;
	lastdirent = buf.previous;
	if (lastdirent) {
		typeof(lastdirent->d_off) d_off = buf.ctx.pos;
		if (__put_user(d_off, &lastdirent->d_off))
			error = -EFAULT;
		else
			error = count - buf.count;
	}
	fdput_pos(f);
	return error;
}


SYSCALL_DEFINE3(getdents64, unsigned int, fd,
		struct linux_dirent64 __user *, dirent, unsigned int, count)
{
	return ksys_getdents64(fd, dirent, count);
}

#ifdef CONFIG_COMPAT
struct compat_old_linux_dirent {
	compat_ulong_t	d_ino;
	compat_ulong_t	d_offset;
	unsigned short	d_namlen;
	char		d_name[1];
};

struct compat_readdir_callback {
	struct dir_context ctx;
	struct compat_old_linux_dirent __user *dirent;
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	struct super_block *sb;
	bool is_base_dentry_android_data_root_dir;
	bool is_base_dentry_sdcard_root_dir;
#endif
	int result;
};

static int compat_fillonedir(struct dir_context *ctx, const char *name,
			     int namlen, loff_t offset, u64 ino,
			     unsigned int d_type)
{
	struct compat_readdir_callback *buf =
		container_of(ctx, struct compat_readdir_callback, ctx);
	struct compat_old_linux_dirent __user *dirent;
	compat_ulong_t d_ino;

#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	struct inode *inode;
#endif

	if (buf->result)
		return -EINVAL;
	buf->result = verify_dirent_name(name, namlen);
	if (buf->result < 0)
		return buf->result;
	d_ino = ino;
	if (sizeof(d_ino) < sizeof(ino) && d_ino != ino) {
		buf->result = -EOVERFLOW;
		return -EOVERFLOW;
	}

#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	if (buf->is_base_dentry_android_data_root_dir) {
		if (susfs_is_sus_android_data_d_name_found(name)) {
			return true;
		}
	} else if (buf->is_base_dentry_sdcard_root_dir) {
		if (susfs_is_sus_sdcard_d_name_found(name)) {
			return true;
		}
	}

	inode = ilookup(buf->sb, ino);
	if (!inode) {
		goto orig_flow;
	}
	if (susfs_is_inode_sus_path(inode)) {
		iput(inode);
		return true;
	}
	iput(inode);
orig_flow:
#endif
	buf->result++;
	dirent = buf->dirent;
	if (!access_ok(VERIFY_WRITE, dirent,
			(unsigned long)(dirent->d_name + namlen + 1) -
				(unsigned long)dirent))
		goto efault;
	if (	__put_user(d_ino, &dirent->d_ino) ||
		__put_user(offset, &dirent->d_offset) ||
		__put_user(namlen, &dirent->d_namlen) ||
		__copy_to_user(dirent->d_name, name, namlen) ||
		__put_user(0, dirent->d_name + namlen))
		goto efault;
	return 0;
efault:
	buf->result = -EFAULT;
	return -EFAULT;
}

COMPAT_SYSCALL_DEFINE3(old_readdir, unsigned int, fd,
		struct compat_old_linux_dirent __user *, dirent, unsigned int, count)
{
	int error;
	struct fd f = fdget_pos(fd);
	struct compat_readdir_callback buf = {
		.ctx.actor = compat_fillonedir,
		.dirent = dirent
	};
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	struct inode *inode;
#endif

	if (!f.file)
		return -EBADF;

#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	buf.sb = f.file->f_inode->i_sb;
	inode = f.file->f_path.dentry->d_inode;
	if (f.file->f_path.dentry && inode) {
		if (susfs_is_base_dentry_android_data_dir(f.file->f_path.dentry))
		{
			buf.is_base_dentry_android_data_root_dir = true;
			buf.is_base_dentry_sdcard_root_dir = false;
			goto orig_flow;
		}
		if (susfs_is_base_dentry_sdcard_dir(f.file->f_path.dentry))
		{
			buf.is_base_dentry_sdcard_root_dir = true;
			buf.is_base_dentry_android_data_root_dir = false;
			goto orig_flow;
		}
	}
	buf.is_base_dentry_android_data_root_dir = false;
	buf.is_base_dentry_sdcard_root_dir = false;
orig_flow:
#endif
	error = iterate_dir(f.file, &buf.ctx);
	if (buf.result)
		error = buf.result;

	fdput_pos(f);
	return error;
}

struct compat_linux_dirent {
	compat_ulong_t	d_ino;
	compat_ulong_t	d_off;
	unsigned short	d_reclen;
	char		d_name[1];
};

struct compat_getdents_callback {
	struct dir_context ctx;
	struct compat_linux_dirent __user *current_dir;
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	struct super_block *sb;
	bool is_base_dentry_android_data_root_dir;
	bool is_base_dentry_sdcard_root_dir;
#endif
	struct compat_linux_dirent __user *previous;
	int count;
	int error;
};

static int compat_filldir(struct dir_context *ctx, const char *name, int namlen,
		loff_t offset, u64 ino, unsigned int d_type)
{
	struct compat_linux_dirent __user * dirent;
	struct compat_getdents_callback *buf =
		container_of(ctx, struct compat_getdents_callback, ctx);
	compat_ulong_t d_ino;
	int reclen = ALIGN(offsetof(struct compat_linux_dirent, d_name) +
		namlen + 2, sizeof(compat_long_t));

#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	struct inode *inode;
#endif
	buf->error = -EINVAL;	/* only used if we fail.. */
	if (reclen > buf->count)
		return -EINVAL;
	d_ino = ino;
	if (sizeof(d_ino) < sizeof(ino) && d_ino != ino) {
		buf->error = -EOVERFLOW;
		return -EOVERFLOW;
	}
	dirent = buf->previous;
	if (dirent) {
		if (signal_pending(current))
			return -EINTR;
		if (__put_user(offset, &dirent->d_off))
			goto efault;
	}

#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	if (buf->is_base_dentry_android_data_root_dir) {
		if (susfs_is_sus_android_data_d_name_found(name)) {
			return true;
		}
	} else if (buf->is_base_dentry_sdcard_root_dir) {
		if (susfs_is_sus_sdcard_d_name_found(name)) {
			return true;
		}
	}

	inode = ilookup(buf->sb, ino);
	if (!inode) {
		goto orig_flow;
	}
	if (susfs_is_inode_sus_path(inode)) {
		iput(inode);
		return true;
	}
	iput(inode);
orig_flow:
#endif
	dirent = buf->current_dir;
	if (__put_user(d_ino, &dirent->d_ino))
		goto efault;
	if (__put_user(reclen, &dirent->d_reclen))
		goto efault;
	if (copy_to_user(dirent->d_name, name, namlen))
		goto efault;
	if (__put_user(0, dirent->d_name + namlen))
		goto efault;
	if (__put_user(d_type, (char  __user *) dirent + reclen - 1))
		goto efault;
	buf->previous = dirent;
	dirent = (void __user *)dirent + reclen;
	buf->current_dir = dirent;
	buf->count -= reclen;
	return 0;
efault:
	buf->error = -EFAULT;
	return -EFAULT;
}

COMPAT_SYSCALL_DEFINE3(getdents, unsigned int, fd,
		struct compat_linux_dirent __user *, dirent, unsigned int, count)
{
	struct fd f;
	struct compat_linux_dirent __user * lastdirent;
	struct compat_getdents_callback buf = {
		.ctx.actor = compat_filldir,
		.current_dir = dirent,
		.count = count
	};
	int error;
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	struct inode *inode;
#endif

	if (!access_ok(VERIFY_WRITE, dirent, count))
		return -EFAULT;

	f = fdget_pos(fd);
	if (!f.file)
		return -EBADF;

#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	buf.sb = f.file->f_inode->i_sb;
	inode = f.file->f_path.dentry->d_inode;
	if (f.file->f_path.dentry && inode) {
		if (susfs_is_base_dentry_android_data_dir(f.file->f_path.dentry))
		{
			buf.is_base_dentry_android_data_root_dir = true;
			buf.is_base_dentry_sdcard_root_dir = false;
			goto orig_flow;
		}
		if (susfs_is_base_dentry_sdcard_dir(f.file->f_path.dentry))
		{
			buf.is_base_dentry_sdcard_root_dir = true;
			buf.is_base_dentry_android_data_root_dir = false;
			goto orig_flow;
		}
	}
	buf.is_base_dentry_android_data_root_dir = false;
	buf.is_base_dentry_sdcard_root_dir = false;
orig_flow:
#endif
	error = iterate_dir(f.file, &buf.ctx);
	if (error >= 0)
		error = buf.error;
	lastdirent = buf.previous;
	if (lastdirent) {
		if (put_user(buf.ctx.pos, &lastdirent->d_off))
			error = -EFAULT;
		else
			error = count - buf.count;
	}
	fdput_pos(f);
	return error;
}
#endif
