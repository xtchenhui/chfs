/*
  CHFS: An Incremenatl Filesystem in Userspace
  Copyright (C) 2015       Hui Chen <hchen46@lsu.edu>

  gcc -Wall fusech_fh.c `pkg-config fuse --cflags --libs` -lulockmgr -o fusech_fh
*/

#define FUSE_USE_VERSION 26

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE

#include <time.h>
#include <fuse.h>
#include <ulockmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include <sys/file.h> /* flock(2) */

int LIMIT= 1<<10;

//static void log_msg(const char* str, const char* content);
//static void log_int(const char* str, int content);

static void log_msg(const char* str, const char* content)
{
    FILE *f=fopen("/tmp/fuselog","a");
    fputs(str,f);
    fputs(content,f);
    putc('\n',f);
    fclose(f);
}

static void log_int(const char* str, int content)
{
    FILE *f=fopen("/tmp/fuselog","a");
    fprintf(f,"%s : %d\n",str,content);
    fclose(f);
}


static char* nameold(const char* path,off_t offset)
{
    char str[50];
    time_t t;
    struct tm *tmp;
    t = time(NULL);
    tmp = localtime(&t);
    strftime(str,sizeof(str),"%y%m%d%H%M%S",tmp);

    char newname[]=".";
    char *lastdot=strrchr(path,'.');
    char *oldname=strrchr(path,'/');
    if(lastdot!=NULL){
        *lastdot='\0';
        strcat(newname,oldname+2);
    }
    else
        strcat(newname,oldname+1);
    char *res=malloc(strlen(newname)+1);
    strcpy(res,newname);
    strcat(newname,str);
    int num=(oldname-path)>0?(oldname-path+1):(path-oldname+1);
    char *dir=malloc(num+1);
    memcpy(dir,path,num);
    dir[num]='\0';
    char *prefix=malloc(strlen(dir)+1);
    strcpy(prefix,dir);
    strcat(prefix,newname);
    strcat(dir,res);
    
    log_msg("old name:",prefix);
    log_msg("normal name:",dir);
    log_int("offset:", offset);
    log_int("access:", access(dir,F_OK));
    if(offset==0 && access(dir,F_OK)!=-1){
        log_msg("exist:",dir); 
	rename(dir,prefix);
    }

    return dir;
}

static int copy(const char* path, const char* buf, size_t size,off_t offset)
{
    char *dir=nameold(path,offset);

    int fd=open(dir, O_WRONLY|O_CREAT, 0644);
    pwrite(fd,buf,size,offset);
	return close(fd);
}


static int ch_getattr(const char *path, struct stat *stbuf)
{
	int res;

	res = lstat(path, stbuf);
	if (res == -1)
		return -errno;

	return 0;
}

static int ch_fgetattr(const char *path, struct stat *stbuf,
			struct fuse_file_info *fi)
{
	int res;

	(void) path;

	res = fstat(fi->fh, stbuf);
	if (res == -1)
		return -errno;

	return 0;
}


static int ch_access(const char *path, int mask)
{
	int res;

	res = access(path, mask);
	if (res == -1)
		return -errno;

	return 0;
}

static int ch_readlink(const char *path, char *buf, size_t size)
{
	int res;

	res = readlink(path, buf, size - 1);
	if (res == -1)
		return -errno;

	buf[res] = '\0';
	return 0;
}

struct ch_dirp {
	DIR *dp;
	struct dirent *entry;
	off_t offset;
};

static int ch_opendir(const char *path, struct fuse_file_info *fi)
{
	int res;
	struct ch_dirp *d = malloc(sizeof(struct ch_dirp));
	if (d == NULL)
		return -ENOMEM;

	d->dp = opendir(path);
	if (d->dp == NULL) {
		res = -errno;
		free(d);
		return res;
	}
	d->offset = 0;
	d->entry = NULL;

	fi->fh = (unsigned long) d;
	return 0;
}

static inline struct ch_dirp *get_dirp(struct fuse_file_info *fi)
{
	return (struct ch_dirp *) (uintptr_t) fi->fh;
}

static int ch_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		       off_t offset, struct fuse_file_info *fi)
{
	struct ch_dirp *d = get_dirp(fi);

	(void) path;
	if (offset != d->offset) {
		seekdir(d->dp, offset);
		d->entry = NULL;
		d->offset = offset;
	}
	while (1) {
		struct stat st;
		off_t nextoff;

		if (!d->entry) {
			d->entry = readdir(d->dp);
			if (!d->entry)
				break;
		}

		memset(&st, 0, sizeof(st));
		st.st_ino = d->entry->d_ino;
		st.st_mode = d->entry->d_type << 12;
		nextoff = telldir(d->dp);
                char *name=d->entry->d_name;
                if(strcmp(path,"/")==0)
                {
                  if(strcmp("fuse",name)==0&&filler(buf, name, &st, nextoff)==0)
                        break;
                }
                else{
                  if(strchr(name,'.')==NULL)
                     if(filler(buf, d->entry->d_name, &st, nextoff))
                        break;
                }

                d->entry = NULL;
        	d->offset = nextoff;
	}

	return 0;
}

static int ch_releasedir(const char *path, struct fuse_file_info *fi)
{
	struct ch_dirp *d = get_dirp(fi);
	(void) path;
	closedir(d->dp);
	free(d);
	return 0;
}

static int ch_mknod(const char *path, mode_t mode, dev_t rdev)
{
	int res;

	if (S_ISFIFO(mode))
		res = mkfifo(path, mode);
	else
		res = mknod(path, mode, rdev);
	if (res == -1)
		return -errno;

	return 0;
}

static int ch_mkdir(const char *path, mode_t mode)
{
	int res;

	res = mkdir(path, mode);
	if (res == -1)
		return -errno;

	return 0;
}

static int ch_unlink(const char *path)
{
	int res;

	res = unlink(path);
	if (res == -1)
		return -errno;

	return 0;
}

static int ch_rmdir(const char *path)
{
	int res;

	res = rmdir(path);
	if (res == -1)
		return -errno;

	return 0;
}

static int ch_symlink(const char *from, const char *to)
{
	int res;

	res = symlink(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int ch_rename(const char *from, const char *to)
{
	int res;

	res = rename(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int ch_link(const char *from, const char *to)
{
	int res;

	res = link(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int ch_chmod(const char *path, mode_t mode)
{
	int res;

	res = chmod(path, mode);
	if (res == -1)
		return -errno;

	return 0;
}

static int ch_chown(const char *path, uid_t uid, gid_t gid)
{
	int res;

	res = lchown(path, uid, gid);
	if (res == -1)
		return -errno;

	return 0;
}

static int ch_truncate(const char *path, off_t size)
{
	int res;

	res = truncate(path, size);
	if (res == -1)
		return -errno;

	return 0;
}

static int ch_ftruncate(const char *path, off_t size,
			 struct fuse_file_info *fi)
{
	int res;

	(void) path;

	res = ftruncate(fi->fh, size);
	if (res == -1)
		return -errno;

	return 0;
}

static int ch_utimens(const char *path, const struct timespec ts[2])
{
	int res;

	/* don't use utime/utimes since they follow symlinks */
	res = utimensat(0, path, ts, AT_SYMLINK_NOFOLLOW);
	if (res == -1)
		return -errno;

	return 0;
}

static int ch_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	int fd;

	fd = open(path, fi->flags, mode);
	if (fd == -1)
		return -errno;

	fi->fh = fd;
	return 0;
}

static int ch_open(const char *path, struct fuse_file_info *fi)
{
	int fd;

	fd = open(path, fi->flags);
	if (fd == -1)
		return -errno;

	fi->fh = fd;
	return 0;
}

static int ch_read(const char *path, char *buf, size_t size, off_t offset,
		    struct fuse_file_info *fi)
{
	int res;

	(void) path;
	res = pread(fi->fh, buf, size, offset);
	if (res == -1)
		res = -errno;

	return res;
}

static int ch_read_buf(const char *path, struct fuse_bufvec **bufp,
			size_t size, off_t offset, struct fuse_file_info *fi)
{
	struct fuse_bufvec *src;

	(void) path;

	src = malloc(sizeof(struct fuse_bufvec));
	if (src == NULL)
		return -ENOMEM;

	*src = FUSE_BUFVEC_INIT(size);

	src->buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
	src->buf[0].fd = fi->fh;
	src->buf[0].pos = offset;

	*bufp = src;

	return 0;
}

static int ch_write(const char *path, const char *buf, size_t size,
		     off_t offset, struct fuse_file_info *fi)
{
	int res;

	(void) path;
 	res = pwrite(fi->fh, buf, size, offset);
	if (res == -1)
		res = -errno;

//    log_int("offset:",offset);
    if(offset+size<LIMIT)
        copy(path,buf,size,offset);
	return res;
}

static int ch_statfs(const char *path, struct statvfs *stbuf)
{
	int res;

	res = statvfs(path, stbuf);
	if (res == -1)
		return -errno;

	return 0;
}

static int ch_flush(const char *path, struct fuse_file_info *fi)
{
	int res;

	(void) path;
	/* This is called from every close on an open file, so call the
	   close on the underlying filesystem.	But since flush may be
	   called multiple times for an open file, this must not really
	   close the file.  This is important if used on a network
	   filesystem like NFS which flush the data/metadata on close() */
	res = close(dup(fi->fh));
	if (res == -1)
		return -errno;

	return 0;
}

static int ch_release(const char *path, struct fuse_file_info *fi)
{
	(void) path;

   close(fi->fh);
	return 0;
}

static int ch_fsync(const char *path, int isdatasync,
		     struct fuse_file_info *fi)
{
	int res;
	(void) path;

#ifndef HAVE_FDATASYNC
	(void) isdatasync;
#else
	if (isdatasync)
		res = fdatasync(fi->fh);
	else
#endif
		res = fsync(fi->fh);
	if (res == -1)
		return -errno;

	return 0;
}

#ifdef HAVE_POSIX_FALLOCATE
static int ch_fallocate(const char *path, int mode,
			off_t offset, off_t length, struct fuse_file_info *fi)
{
	(void) path;

	if (mode)
		return -EOPNOTSUPP;

	return -posix_fallocate(fi->fh, offset, length);
}
#endif

/* xattr operations are optional and can safely be left unimplemented */
static int ch_setxattr(const char *path, const char *name, const char *value,
			size_t size, int flags)
{
	int res = lsetxattr(path, name, value, size, flags);
	if (res == -1)
		return -errno;
	return 0;
}

static int ch_getxattr(const char *path, const char *name, char *value,
			size_t size)
{
	int res = lgetxattr(path, name, value, size);
	if (res == -1)
		return -errno;
	return res;
}

static int ch_listxattr(const char *path, char *list, size_t size)
{
	int res = llistxattr(path, list, size);
	if (res == -1)
		return -errno;
	return res;
}

static int ch_removexattr(const char *path, const char *name)
{
	int res = lremovexattr(path, name);
	if (res == -1)
		return -errno;
	return 0;
}

static int ch_lock(const char *path, struct fuse_file_info *fi, int cmd,
		    struct flock *lock)
{
	(void) path;

	return ulockmgr_op(fi->fh, cmd, lock, &fi->lock_owner,
			   sizeof(fi->lock_owner));
}

static int ch_flock(const char *path, struct fuse_file_info *fi, int op)
{
	int res;
	(void) path;

	res = flock(fi->fh, op);
	if (res == -1)
		return -errno;

	return 0;
}


static struct fuse_operations ch_oper = {
	.getattr	= ch_getattr,
	.fgetattr	= ch_fgetattr,
	.access		= ch_access,
	.readlink	= ch_readlink,
	.opendir	= ch_opendir,
	.readdir	= ch_readdir,
	.releasedir	= ch_releasedir,
	.mknod		= ch_mknod,
	.mkdir		= ch_mkdir,
	.symlink	= ch_symlink,
	.unlink		= ch_unlink,
	.rmdir		= ch_rmdir,
	.rename		= ch_rename,
	.link		= ch_link,
	.chmod		= ch_chmod,
	.chown		= ch_chown,
	.truncate	= ch_truncate,
	.ftruncate	= ch_ftruncate,
	.utimens	= ch_utimens,
	.create		= ch_create,
	.open		= ch_open,
	.read		= ch_read,
	.read_buf	= ch_read_buf,
	.write		= ch_write,
	.statfs		= ch_statfs,
	.flush		= ch_flush,
	.release	= ch_release,
	.fsync		= ch_fsync,
#ifdef HAVE_POSIX_FALLOCATE
	.fallocate	= ch_fallocate,
#endif
	.setxattr	= ch_setxattr,
	.getxattr	= ch_getxattr,
	.listxattr	= ch_listxattr,
	.removexattr	= ch_removexattr,
	.lock		= ch_lock,
	.flock		= ch_flock,
	.flag_nullpath_ok = 1,
	.flag_utime_omit_ok = 1,
};

void setLimit(char* size)
{
    char c=*size;
    int value=0;
    int num=0;
    int i=0;
    while(c!='\0'){
        i++;
        if(c>='0'&&c<='9')
            value=value*10+(c-'0');
        else if(c=='k'||c=='K')
            num=10;
        else if(c=='m'||c=='M')
            num=20;
        c=*(size+i);
    }
    LIMIT=value<<num;
//    log_int("LIMIT: ",LIMIT);
}


int check(int argc,char *argv[]){
    if(argc<3){
      printf("Usage: chfs Target_DIR LIMIT_SIZE[k|m]...\n");
      return 0;
    }
    char *size=argv[2];
    char c=*size;
    int j=0;
    while(c!='\0'){
      j++;
      char next=*(size+j);
      if(next=='\0'&&c!='k'&&c!='m'&&c!='K'&&c!='M'){
         printf("Usage: chfs Target_DIR LIMIT_SIZE[k|m]...\n");
	 return 0;
      }
      else if(next!='\0'&&(c<'0'||c>'9')){
         printf("Usage: chfs Target_DIR LIMIT_SIZE[k|m]...\n");
	 return 0;
      }
      c=next;
    }
    return 1;
}

int main(int argc, char *argv[])
{
    if(check(argc,argv)==0)
	return 0;
    umask(0);
    int i=2;
    char *size=argv[2];
    while(i<argc-1){
        argv[i]=argv[i+1];
        i++;
    }
    argc--;
    setLimit(size);
    return fuse_main(argc, argv, &ch_oper, NULL);
}
