/* --------------------------------------------------------------------*/
/* File: eosd.c                                                        */
/* Author: Elvin-Alin Sindrilaru / Andreas-Joachim Peters - CERN       */
/* --------------------------------------------------------------------*/

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2011 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your option) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.

  gcc -Wall `pkg-config fuse --cflags --libs` hello_ll.c -o hello_ll
*/


#define FUSE_USE_VERSION 26

#include <fuse/fuse_lowlevel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>

#include "xrdposix.hh"

int isdebug = 0;
// mount hostport;
char mounthostport[1024];

// mount prefix 
char mountprefix[1024];
double entrycachetime = 5.0;
double attrcachetime = 5.0;
double readopentime = 5.0;


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_readlink(fuse_req_t req, fuse_ino_t ino)
{
  struct stat stbuf;
  char fullpath[16384];
  {
    xrd_lock_r_p2i(); // =>
    const char* name = xrd_path((unsigned long long)ino);

    if (!name) {
      fuse_reply_err(req, ENXIO);
      xrd_unlock_r_p2i(); // <=
      return;
    }
    
    sprintf(fullpath,"root://%s@%s/%s/%s",xrd_mapuser(req->ctx.uid),mounthostport,mountprefix,name);
    if (isdebug) fprintf(stderr,"[%s]: inode=%lld path=%s\n", __FUNCTION__,(long long)ino,fullpath);  
    xrd_unlock_r_p2i(); // <=
  }

  char linkbuffer[8912];
  
  int retc = xrd_readlink(fullpath, linkbuffer, sizeof(linkbuffer));
  if (!retc) {
    fuse_reply_readlink(req,linkbuffer);
    return;
  } else {
    fuse_reply_err(req, errno);
    return;
  }
}


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_getattr(fuse_req_t req, fuse_ino_t ino,
				struct fuse_file_info *fi)
{
  struct stat stbuf;
  memset(&stbuf,0,sizeof(struct stat));
  char fullpath[16384];

  {
    xrd_lock_r_p2i(); // =>
    const char* name = xrd_path((unsigned long long)ino);

    if (!name) {
      fuse_reply_err(req, ENXIO);
      xrd_unlock_r_p2i(); // <=
      return;
    }
    
    sprintf(fullpath,"root://%s@%s/%s/%s",xrd_mapuser(req->ctx.uid),mounthostport,mountprefix,name);
    if (isdebug) fprintf(stderr,"[%s]: inode=%lld path=%s\n", __FUNCTION__,(long long)ino,fullpath);
    xrd_unlock_r_p2i(); // <=
  }
  
  int retc = xrd_stat(fullpath,&stbuf);

  if (!retc) {
    fuse_reply_attr(req, &stbuf, attrcachetime);
  } else {
    fuse_reply_err(req, EIO);
  }
}


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
                             int to_set, struct fuse_file_info *fi)
{
  int retc=0;
  char fullpath[16384];

  {
    xrd_lock_r_p2i(); // =>
    const char* name = xrd_path((unsigned long long)ino);
    // the root is inode 1

    if (!name) {
      fuse_reply_err(req, ENXIO);
      xrd_unlock_r_p2i(); // <=
      return;
    }
    
    sprintf(fullpath,"root://%s@%s/%s/%s",xrd_mapuser(req->ctx.uid),mounthostport,mountprefix,name);
    if (isdebug) fprintf(stderr,"[%s]: inode=%lld path=%s\n", __FUNCTION__,(long long)ino,fullpath);  
    xrd_unlock_r_p2i(); // <=
  }
  
  if (to_set & FUSE_SET_ATTR_MODE) {
    if (isdebug) fprintf(stderr,"[%s]: set attr mode ino=%lld\n", __FUNCTION__,(long long)ino);
    retc = xrd_chmod(fullpath, attr->st_mode);
  }
  if ( (to_set & FUSE_SET_ATTR_UID) && (to_set & FUSE_SET_ATTR_GID) ) {
    if (isdebug) fprintf(stderr,"[%s]: set attr uid  ino=%lld\n", __FUNCTION__,(long long)ino);
    if (isdebug) fprintf(stderr,"[%s]: set attr gid  ino=%lld\n", __FUNCTION__,(long long)ino);
    // f.t.m. we fake it works !
    //    fuse_reply_err(req,EPERM);
    //    return;
  }
  
  if (to_set & FUSE_SET_ATTR_SIZE) {
    if (fi) {

      if (isdebug) fprintf(stderr,"[%s]: truncate\n",__FUNCTION__);
      if (fi->fh) {
        retc = xrd_truncate(fi->fh, attr->st_size, ino);
      } else {
        if (isdebug) fprintf(stderr,"[%s]: set attr size=%lld ino=%lld\n", __FUNCTION__,(long long)attr->st_size, (long long)ino);
        int fd;
        if ((fd = xrd_open(fullpath, O_WRONLY , S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH))>=0) {
          retc = xrd_truncate(fd,attr->st_size, ino);
          xrd_close(fd, ino);
        } else {
          retc = -1;
        }
      }
    } else {

      if (isdebug) fprintf(stderr,"[%s]: set attr size=%lld ino=%lld\n", __FUNCTION__,(long long)attr->st_size, (long long)ino);
      int fd;
      if ((fd = xrd_open(fullpath, O_WRONLY , S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH))>=0) {
        retc = xrd_truncate(fd, attr->st_size, ino);
        xrd_close(fd, ino);
      }
    }
  }
  
  if ( (to_set & FUSE_SET_ATTR_ATIME) && (to_set & FUSE_SET_ATTR_MTIME) ) {  
    struct timespec tvp[2];
    tvp[0].tv_sec = attr->st_atime;
    tvp[0].tv_nsec = 0;
    tvp[1].tv_sec = attr->st_mtime;
    tvp[1].tv_nsec = 0;
    if (isdebug) fprintf(stderr,"[%s]: set attr atime ino=%lld time=%ld\n", __FUNCTION__,(long long)ino, (long)attr->st_atime);
    if (isdebug) fprintf(stderr,"[%s]: set attr mtime ino=%lld time=%ld\n", __FUNCTION__,(long long)ino, (long)attr->st_mtime);
    retc = xrd_utimes(fullpath, tvp);
  }
  if (isdebug) fprintf(stderr,"[%s]: return code =%d\n",__FUNCTION__, retc);

  struct stat newattr;
  memset(&newattr,0,sizeof(struct stat));
  if (!retc) {
    int retc = xrd_stat(fullpath,&newattr);
    if (!retc) {
      fuse_reply_attr(req, &newattr,attrcachetime);
    } else {
      fuse_reply_err(req, errno);
    }
  } else {
    fuse_reply_err(req, errno);
  }
}


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
  int entry_found = 0;
  unsigned long long entry_inode;
  const char* parentpath = NULL;
  char fullpath[16384];
  char ifullpath[16384];

  {
    xrd_lock_r_p2i(); // =>
    parentpath = xrd_path((unsigned long long)parent);
        
    if (!parentpath) {
      fuse_reply_err(req, ENXIO);
      xrd_unlock_r_p2i(); // <=
      return;
    }
    
    if (name[0] == '/')
      sprintf(ifullpath, "%s%s", parentpath, name);
    else 
      sprintf(ifullpath, "%s/%s", parentpath, name);
    
    sprintf(fullpath,"root://%s@%s%s/%s/%s", xrd_mapuser(req->ctx.uid), mounthostport, mountprefix, parentpath,name);
    
    if (isdebug) {
      fprintf(stderr,"[%s]: parent=%lld path=%s uid=%d\n",
              __FUNCTION__, (long long)parent, fullpath, req->ctx.uid);
    }
    xrd_unlock_r_p2i(); // <=
  }

  //get subentry inode value
  xrd_lock_r_p2i();   // =>
  entry_inode = xrd_inode(ifullpath);
  xrd_unlock_r_p2i(); // <=

  if (entry_inode) {
    //try to get entry from cache
    entry_found = xrd_dir_cache_get_entry(req, parent, entry_inode, ifullpath);
  }

  if ( !entry_found ) {
    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    if (isdebug) fprintf(stderr, "[%s] subentry_found = %i \n", __FUNCTION__, entry_found);
    
    //add entry to dir
    e.attr_timeout = attrcachetime;
    e.entry_timeout = entrycachetime;
    int retc = xrd_stat(fullpath, &e.attr);
    if (!retc) {
      if (isdebug) {
        fprintf(stderr,"[%s]: storeinode=%lld path=%s\n",
                __FUNCTION__,(long long) e.attr.st_ino,ifullpath);
      }
      
      e.ino = e.attr.st_ino;
      xrd_store_p2i(e.attr.st_ino, ifullpath);
      fuse_reply_entry(req, &e);
      
      //add entry to cached dir
      xrd_dir_cache_add_entry(parent, e.attr.st_ino, &e);
    } else {
      if (errno == EFAULT) {
        e.ino = 0;
        fuse_reply_entry(req, &e);
      } else {
          fuse_reply_err(req, errno);
      }
    }
  }
}

#define min(x, y) ((x) < (y) ? (x) : (y))


//--------------------------------------------------------------------------------------------------
static int reply_buf_limited(fuse_req_t req, const char *buf, size_t bufsize,
			     off_t off, size_t maxsize)
{
  if (off < bufsize)
    return fuse_reply_buf(req, buf + off,
			  min(bufsize - off, maxsize));
  else 
    return fuse_reply_buf(req, NULL, 0);
}


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_opendir(fuse_req_t req, fuse_ino_t ino,
			      struct fuse_file_info *fi)
{
  char fullpath[16384];
  {
    xrd_lock_r_p2i(); // =>
    const char* name = xrd_path((unsigned long long)ino);

    if (!name) {
      fuse_reply_err(req, ENXIO);
      xrd_unlock_r_p2i(); // <=
      return;
    }
    
    sprintf(fullpath,"root://%s@%s/%s/%s",xrd_mapuser(req->ctx.uid),mounthostport,mountprefix,name);
    if (isdebug) fprintf(stderr,"[%s]: inode=%lld path=%s\n", __FUNCTION__,(long long)ino,fullpath);
    xrd_unlock_r_p2i(); // <=
  }
  
  DIR* dir ;
  dir = xrd_opendir(fullpath);

#if __SIZEOF_POINTER__ == 8
  fi->fh = (uint64_t) dir;
#else
  fi->fh = (unsigned long) dir;
#endif

  if (dir) {
    fuse_reply_open(req, fi);
  } else {
    fuse_reply_err(req, EIO);
  }
}


//--------------------------------------------------------------------------------------------------
static void dirbuf_add(fuse_req_t req, struct dirbuf *b, const char *name,
		fuse_ino_t ino)
{
  struct stat stbuf;
  size_t oldsize = b->size;
  b->size += fuse_add_direntry(req, NULL, 0, name, NULL, 0);
  b->p = (char *) realloc(b->p, b->size);
  memset(&stbuf, 0, sizeof(stbuf));
  stbuf.st_ino = ino;
  fuse_add_direntry(req, b->p + oldsize, b->size - oldsize, name, &stbuf,
                    b->size);
}


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
			      off_t off, struct fuse_file_info *fi)
{
  struct stat stbuf;
  char fullpath[16384];
  char dirfullpath[16384];

  int retc = 0;
  int dir_status;
  size_t cnt = 0;
  char* namep;
  unsigned long long in;
  struct dirbuf* b;
  struct stat attr;
  
  xrd_lock_r_p2i(); // =>
  char* name = 0;
  const char* tmpname = xrd_path((unsigned long long)ino);

  if (tmpname) 
    name = strdup(tmpname);

  if (!name) {
    fuse_reply_err(req, ENXIO);
    xrd_unlock_r_p2i(); // <=
    return;
  }

  xrd_unlock_r_p2i(); // <=  
  sprintf(fullpath,"root://%s@%s//proc/user/?mgm.cmd=fuse&mgm.subcmd=inodirlist&mgm.path=%s/%s",
          xrd_mapuser(req->ctx.uid),mounthostport,mountprefix,name);
  
  if (isdebug) {
    fprintf(stderr, "[%s]: inode=%lld path=%s size=%lld off=%lld\n",
            __FUNCTION__, (long long)ino, fullpath, (long long)size, (long long)off);
  }
  
  sprintf(dirfullpath, "root://%s@%s%s/%s", xrd_mapuser(req->ctx.uid), mounthostport, mountprefix, name);

  // try to use the cache for directories
  retc = xrd_stat(dirfullpath, &attr);
  dir_status = xrd_dir_cache_get(ino, attr.st_mtim, fullpath, &b);

  if (isdebug) fprintf(stderr, "[%s] directory status is: %i. \n",  __FUNCTION__, dir_status);

  if (dir_status == dError){
    fuse_reply_err(req, EPERM);
    return;    
  }
  else if (dir_status != dValid) {
    // dir not in cache or invalid
    if (!xrd_dirview_entry(ino, 0)) {
      // there is no listing yet, create one!
      retc = xrd_inodirlist((unsigned long long)ino, fullpath);
      if (retc) {
        fuse_reply_err(req, retc);
        if (name)
          free(name);
        return;
      }
      
      b = xrd_dirview_getbuffer((unsigned long long)ino);
      if (!b) {
        fuse_reply_err(req, EPERM);
        if (name)
          free(name);
        return;
      }
      b->p = NULL;
      b->size = 0;
    
      xrd_lock_r_dirview(); // =>

      while ((in = xrd_dirview_entry(ino, cnt))) {
        char ifullpath[16384];
        if ((namep = xrd_basename(in))) {
          if (cnt == 0) {
            // this is the '.' directory
            namep = ".";
          } else {
            if (cnt == 1) {
              // this is the '..' directory
              namep = "..";
            } else {
              sprintf(ifullpath,"%s/%s",name,xrd_basename(in));	 
              if (isdebug) fprintf(stderr,"[%s]: add entry fullpath=%s name=%s\n", __FUNCTION__, ifullpath, namep);
            } 
          }
	
          dirbuf_add(req, b, namep, (fuse_ino_t) in);
          cnt++;
        } else {
          if (isdebug) fprintf(stderr,"[%s]: lookup failed for inode=%llu\n", __FUNCTION__, in);
          cnt++;
        }
      
        // add directory to cache or update it
        xrd_dir_cache_sync(ino, dirfullpath, cnt, attr.st_mtim, b);           
      }

      xrd_unlock_r_dirview(); // <=
    } else {
      xrd_lock_r_dirview(); // =>
      b = xrd_dirview_getbuffer((unsigned long long)ino);
      xrd_unlock_r_dirview(); // <=
    }
  }

  if (name) {
    free(name);
  }

  if (isdebug) {
    fprintf(stderr,"[%s]: return size=%lld ptr=%lld\n",
            __FUNCTION__, (long long)b->size, (long long)b->p);
  }
  
  reply_buf_limited(req, b->p, b->size, off, size);
}


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_releasedir (fuse_req_t req, fuse_ino_t ino,
				  struct fuse_file_info *fi)
{
  if (fi->fh) {
    xrd_closedir((DIR*)fi->fh);
  }

  xrd_dirview_delete(ino);
  fuse_reply_err(req, 0);
}


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_statfs(fuse_req_t req, fuse_ino_t ino)
{
  struct statvfs svfs,svfs2;
  
  char* path=NULL;
  char rootpath[16384];

  xrd_lock_r_p2i(); // =>
  const char* tmppath = xrd_path((unsigned long long) ino);
  if (tmppath)
    path = strdup(tmppath);
  xrd_unlock_r_p2i(); // <=
  
  
  if (!path) {
    svfs.f_bsize=128*1024;
    svfs.f_blocks=1000000000ll;
    svfs.f_bfree=1000000000ll;
    svfs.f_bavail=1000000000ll;
    svfs.f_files=1000000;
    svfs.f_ffree=1000000;
    
    fuse_reply_statfs(req, &svfs);
    return;
  }
  
  sprintf(rootpath,"root://%s@%s/%s",xrd_mapuser(req->ctx.uid),mounthostport,mountprefix);
  
  int res = xrd_statfs(rootpath, path, &svfs2);
  
  free(path);
  
  if (res == -1) {
    svfs.f_bsize=128*1024;
    svfs.f_blocks=1000000000ll;
    svfs.f_bfree=1000000000ll;
    svfs.f_bavail=1000000000ll;
    svfs.f_files=1000000;
    svfs.f_ffree=1000000;
    fuse_reply_statfs(req, &svfs);
  } else {
    fuse_reply_statfs(req, &svfs2);
  }
  return;
}


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_mknod(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode,
			    dev_t rdev)
{
  int res;

  if (S_ISREG(mode)) {
    const char* parentpath=NULL;
    char fullpath[16384];
    char fullparentpath[16384];

    xrd_lock_r_p2i(); // =>
    parentpath = xrd_path((unsigned long long)parent);

    if (!parentpath) {
      fuse_reply_err(req, ENXIO);
      xrd_unlock_r_p2i(); // <=
      return;
    }
    
    sprintf(fullpath,"root://%s@%s/%s%s/%s",xrd_mapuser(req->ctx.uid),mounthostport,mountprefix,parentpath,name);
    sprintf(fullparentpath,"root://%s@%s/%s%s",xrd_mapuser(req->ctx.uid),mounthostport, mountprefix,parentpath);
    if (isdebug) fprintf(stderr,"[%s]: parent=%lld path=%s uid=%d\n", __FUNCTION__,(long long)parent,fullpath,req->ctx.uid);
    
    xrd_unlock_r_p2i(); // <=
    
    res = xrd_open(fullpath, O_CREAT | O_EXCL | O_RDWR, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
    if (res == -1) {
      fuse_reply_err(req, errno);
      return;
    }
    
    char ifullpath[16384];
    sprintf(ifullpath,"%s/%s",parentpath,name);
    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.attr_timeout = attrcachetime;
    e.entry_timeout = entrycachetime;
    struct stat newbuf;

    // update the entry
    int retc = xrd_stat(fullpath,&e.attr);
    e.ino = e.attr.st_ino;

    if (retc == -1) {
      fuse_reply_err(req,errno);
      return;
    } else {
      xrd_add_open_fd(res, (unsigned long long) e.ino, req->ctx.uid);
      xrd_get_open_fd((unsigned long long) e.ino, req->ctx.uid);
      xrd_store_p2i((unsigned long long)e.ino,ifullpath);
      if (isdebug) fprintf(stderr,"[%s]: storeinode=%lld path=%s\n", __FUNCTION__,(long long) e.ino,ifullpath);
      fuse_reply_entry(req,&e);
      return;
    }
  }    
  fuse_reply_err(req,EINVAL);
}


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode)
{
  const char* parentpath=NULL;
  char fullpath[16384];

  xrd_lock_r_p2i(); // =>

  parentpath = xrd_path((unsigned long long)parent);

  if (!parentpath) {
    fuse_reply_err(req, ENXIO);
    xrd_unlock_r_p2i(); // <=
    return;
  }
  char ifullpath[16384];
  sprintf(ifullpath,"%s/%s",parentpath,name);
  sprintf(fullpath,"root://%s@%s/%s%s/%s",xrd_mapuser(req->ctx.uid),mounthostport,mountprefix,parentpath,name);

  xrd_unlock_r_p2i();

  if (isdebug) fprintf(stderr,"[%s]: path=%s\n", __FUNCTION__,fullpath);
  int retc = xrd_mkdir(fullpath,mode);
  if (!retc) {
    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.attr_timeout = attrcachetime;
    e.entry_timeout = entrycachetime;
    struct stat newbuf;
    int retc = xrd_stat(fullpath,&e.attr);

    e.ino = e.attr.st_ino;

    if (retc) {
      fuse_reply_err(req,errno);
      return;
    } else {
      xrd_store_p2i((unsigned long long)e.attr.st_ino,ifullpath);
      fuse_reply_entry(req,&e);
      return;
    }
  } else {
    fuse_reply_err(req, errno);
    return;
  }
 }


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_unlink(fuse_req_t req, fuse_ino_t parent, const char *name)
{
  const char* parentpath=NULL;
  char fullpath[16384];

  xrd_lock_r_p2i(); // =>
  parentpath = xrd_path((unsigned long long)parent);

  if (!parentpath) {
    fuse_reply_err(req, ENXIO);
    xrd_unlock_r_p2i(); // <=
    return;
  }

  sprintf(fullpath,"root://%s@%s/%s%s/%s",xrd_mapuser(req->ctx.uid),mounthostport,mountprefix,parentpath,name);
  if (isdebug) fprintf(stderr,"[%s]: path=%s\n", __FUNCTION__,fullpath);

  xrd_unlock_r_p2i(); // <=

  int retc = xrd_unlink(fullpath);

  if (!retc) {
    fuse_reply_buf(req, NULL, 0);
    return;
  } else {
    fuse_reply_err(req, errno);
    return;
  }
}


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name)
{
  const char* parentpath=NULL;
  char fullpath[16384];

  xrd_lock_r_p2i(); // =>
  parentpath = xrd_path( (unsigned long long) parent );
  
  if (!parentpath) {
    fuse_reply_err(req, ENXIO);
    return;
  }
  
  sprintf(fullpath,"root://%s@%s/%s%s/%s",xrd_mapuser(req->ctx.uid),mounthostport,mountprefix,parentpath,name);
  if (isdebug) fprintf(stderr,"[%s]: path=%s\n", __FUNCTION__,fullpath);

  xrd_unlock_r_p2i(); // <=

  int retc = xrd_rmdir(fullpath);

  if (!retc) {
    fuse_reply_err(req, 0);
    return;
  } else {
    if (errno == ENOSYS) {
      fuse_reply_err(req, ENOTEMPTY);
    } else {
      fuse_reply_err(req, errno);
    }
    return;
  }
}


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_symlink(fuse_req_t req, const char *link, fuse_ino_t parent, 
			      const char *name)
{
  char linksource[16384];
  char linkdest[16384];
  const char* parentpath=NULL;
  char fullpath[16384];
  char fulllinkpath[16384];

  xrd_lock_r_p2i(); // =>
  parentpath = xrd_path( (unsigned long long) parent);

  if (!parentpath) {
    fuse_reply_err(req, ENXIO);
    xrd_unlock_r_p2i(); // <=
    return;
  }

  sprintf(fullpath,"root://%s@%s/%s/%s",xrd_mapuser(req->ctx.uid),mounthostport,parentpath,name);
  if (isdebug) fprintf(stderr,"[%s]: path=%s\n", __FUNCTION__,fullpath);

  sprintf(linksource,"%s/%s",parentpath,name);
  sprintf(linkdest,"%s/%s",parentpath,link);

  sprintf(fulllinkpath,"root://%s@%s/%s/%s",xrd_mapuser(req->ctx.uid),mounthostport,parentpath,link);

  xrd_unlock_r_p2i(); // <=

  if (isdebug) fprintf(stderr,"[%s]: sourcepath=%s link=%s\n", __FUNCTION__,linksource,link);
  int retc = xrd_symlink(fullpath,linksource,link);
  if (!retc) {
    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.attr_timeout = attrcachetime;
    e.entry_timeout = entrycachetime;
    int retc = xrd_stat(fullpath,&e.attr);
    if (!retc) {
      if (isdebug) fprintf(stderr,"[%s]: storeinode=%lld path=%s\n", __FUNCTION__,(long long)e.attr.st_ino,linksource);
      e.ino = e.attr.st_ino;
      xrd_store_p2i((unsigned long long)e.attr.st_ino,linksource);
      fuse_reply_entry(req, &e);
      return;
    } else {
      fuse_reply_err(req, errno);
      return;
    }
  } else {
    fuse_reply_err(req, errno);
    return;
  }
}


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_rename(fuse_req_t req, fuse_ino_t parent, const char *name,
			     fuse_ino_t newparent, const char *newname)
{
  const char* parentpath=NULL;
  const char* newparentpath=NULL;
  char fullpath[16384];
  char newfullpath[16384];

  xrd_lock_r_p2i(); // =>

  parentpath = xrd_path( (unsigned long long ) parent );

  if (!parentpath) {
    fuse_reply_err(req, ENXIO);
    xrd_unlock_r_p2i(); // <=
    return;
  }

  newparentpath = xrd_path( (unsigned long long ) newparent );

  if (!newparentpath) {
    fuse_reply_err(req, ENXIO);
    xrd_unlock_r_p2i(); // <=
    return;
  }

  sprintf(fullpath,"root://%s@%s/%s/%s",xrd_mapuser(req->ctx.uid),mounthostport,parentpath,name);
  sprintf(newfullpath,"root://%s@%s/%s/%s",xrd_mapuser(req->ctx.uid),mounthostport,newparentpath,newname);

  xrd_unlock_r_p2i(); // <=

  struct stat stbuf;
  int retcold = xrd_stat(fullpath, &stbuf);
  if (isdebug) fprintf(stderr,"[%s]: path=%s inode=%lu [%d]\n", __FUNCTION__,fullpath,stbuf.st_ino,retcold);
  if (isdebug) fprintf(stderr,"[%s]: path=%s newpath=%s\n", __FUNCTION__,fullpath,newfullpath);

  int retc = xrd_rename(fullpath,newfullpath);

  if (!retc) {
    // update the inode store
    if (!retcold) {
      char iparentpath[16384];
      sprintf(iparentpath,"%s/%s",newparentpath,newname);
      if (isdebug) fprintf(stderr,"[%s]: forgetting inode=%lu \n",__FUNCTION__,stbuf.st_ino);
      xrd_forget_p2i((unsigned long long) stbuf.st_ino);
      xrd_store_p2i((unsigned long long) stbuf.st_ino,iparentpath);
    }
    
    fuse_reply_err(req,0);
    return;
  } else {
    fuse_reply_err(req,EOPNOTSUPP);
    return;
  }
}


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t parent,
			   const char *name)
{
  char linkdest[16384];
  const char* parentpath=NULL;
  char fullpath[16384];
  const char* sourcepath=NULL;

  xrd_lock_r_p2i(); // =>
  parentpath = xrd_path( (unsigned long long) parent);

  if (!parentpath) {
    fuse_reply_err(req, ENXIO);
    xrd_unlock_r_p2i(); // <=
    return;
  }
  sourcepath = xrd_path((unsigned long long)ino);
  if (!sourcepath) {
    fuse_reply_err(req, ENXIO);
    xrd_unlock_r_p2i(); // <=
    return;
  }
  
  sprintf(fullpath,"root://%s@%s/%s/%s",xrd_mapuser(req->ctx.uid),mounthostport,parentpath,name);
  if (isdebug) fprintf(stderr,"[%s]: path=%s\n", __FUNCTION__,fullpath);

  sprintf(linkdest,"%s/%s",parentpath,name);

  xrd_unlock_r_p2i(); // <=

  if (isdebug) fprintf(stderr,"[%s]: sourcepath=%s link=%s\n", __FUNCTION__,linkdest,sourcepath);

  int retc = xrd_link(fullpath,linkdest,sourcepath);

  if (!retc) {
    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.attr_timeout = attrcachetime;
    e.entry_timeout = entrycachetime;
    int retc = xrd_stat(fullpath,&e.attr);
    if (!retc) {
      if (isdebug) fprintf(stderr,"[%s]: storeinode=%lld path=%s\n", __FUNCTION__,(long long)e.attr.st_ino,linkdest);
      e.ino = e.attr.st_ino;
      xrd_store_p2i((unsigned long long)e.attr.st_ino,linkdest);
      fuse_reply_entry(req, &e);
      return;
    } else {
      fuse_reply_err(req, errno);
      return;
    }
  } else {
    fuse_reply_err(req, errno);
    return;
  }
}


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_access(fuse_req_t req, fuse_ino_t ino, int mask)
{
  char fullpath[16384];
  xrd_lock_r_p2i(); // =>
  const char* name = xrd_path((unsigned long long)ino);

  if (!name) {
    fuse_reply_err(req, ENXIO);
    xrd_unlock_r_p2i(); // <=
    return;
  }
  
  sprintf(fullpath,"root://%s@%s/%s/%s",xrd_mapuser(req->ctx.uid),mounthostport,mountprefix,name);
  if (isdebug) fprintf(stderr,"[%s]: inode=%lld path=%s\n", __FUNCTION__,(long long)ino,fullpath);
  
  xrd_unlock_r_p2i(); // <=

  int retc = xrd_access(fullpath,mask);
  if (!retc) {
    fuse_reply_err(req, 0);
  } else {
    fuse_reply_err(req, errno);
  }
}


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_open( fuse_req_t             req,
                           fuse_ino_t             ino,
                           struct fuse_file_info *fi )
{
  struct stat stbuf;
  char fullpath[16384];

  xrd_lock_r_p2i(); // =>
  const char* name = xrd_path((unsigned long long)ino);

  if (!name) {
    fuse_reply_err(req, ENXIO);
    xrd_unlock_r_p2i(); // <=
    return;
  }

  sprintf(fullpath,"root://%s@%s/%s%s",xrd_mapuser(req->ctx.uid),mounthostport,mountprefix,name);
  
  xrd_unlock_r_p2i(); // <=

  int res;
  if ( fi->flags & ( O_RDWR | O_WRONLY | O_CREAT) ) {
    if ( (res = xrd_get_open_fd((unsigned long long)ino, req->ctx.uid)) > 0) {

      if (isdebug) {
        fprintf(stderr,"[%s]: inode=%llu path=%s attaching to res=%d\n",
                __FUNCTION__,(long long)ino,fullpath,res);
      }

      xrd_lease_open_fd(ino, req->ctx.uid);
    } else {
      res = xrd_open(fullpath, fi->flags,S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
    }
  } else {
      res = xrd_open(fullpath, fi->flags,0);
  }

  if (isdebug) fprintf(stderr,"[%s]: inode=%lld path=%s res=%d\n", __FUNCTION__,(long long)ino,fullpath,res);
  if (res == -1) {
    fuse_reply_err(req, errno);
    return;
  }

  fi->fh = res;

  
  if (getenv("EOS_FUSE_KERNELCACHE") && (!strcmp(getenv("EOS_FUSE_KERNELCACHE"),"1"))) {
    // TODO: this should be improved
    if (strstr(fullpath,"/proc/")) {
      fi->keep_cache = 0;
    } else {
      fi->keep_cache = 1;
    }
  } else {
    fi->keep_cache = 0;
  }
    
  if (getenv("EOS_FUSE_DIRECTIO") && (!strcmp(getenv("EOS_FUSE_DIRECTIO"),"1"))) {
    fi->direct_io=1;
  } else {
    fi->direct_io=0;
  }

  fuse_reply_open(req, fi);
}


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_read(fuse_req_t req, fuse_ino_t ino, size_t size,
			   off_t off, struct fuse_file_info *fi)
{
  if (fi->fh) {
    char* buf = xrd_attach_read_buffer(fi->fh,size);
    if (isdebug) fprintf(stderr,"[%s]: inode=%lld size=%lld off=%lld buf=%lld fh=%lld\n", __FUNCTION__,(long long)ino,(long long)size,(long long)off,(long long)buf,(long long)fi->fh);
    int res = xrd_pread(fi->fh, buf, size, off, ino);
    if (res == -1) {
      // map file system errors to IO errors!
      if (errno == ENOSYS)
        errno = EIO;

      fuse_reply_err(req, errno);
      return;
    }
    
    fuse_reply_buf(req, buf, res);
    return;
  } else {
    fuse_reply_err(req, ENXIO);
  }
}


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_write(fuse_req_t req, fuse_ino_t ino, const char *buf, size_t size,
			    off_t off, struct fuse_file_info *fi)
{
  if (fi->fh) {
    if (isdebug) fprintf(stderr,"[%s]: inode=%lld size=%lld off=%lld buf=%lld fh=%lld\n", 
                         __FUNCTION__, (long long)ino,(long long)size,(long long)off,
                         (long long)buf,(long long)fi->fh);

    int res = xrd_pwrite(fi->fh, buf, size, off, ino);
    if (res == -1) {
      fuse_reply_err(req, errno);
    }
    
    fuse_reply_write(req, res);
  } else {
    fuse_reply_err(req, ENXIO);
  }
}


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_release(fuse_req_t req, fuse_ino_t ino,
			    struct fuse_file_info *fi)
{
  if (fi->fh) {
    if (isdebug) fprintf(stderr,"[%s]: inode=%lld fh=%lld\n", __FUNCTION__,(long long)ino,(long long)fi->fh);
    int fd = (int)fi->fh;

    xrd_release_read_buffer(fi->fh);
    
    int res=0;

    xrd_lease_open_fd((unsigned long long) ino, req->ctx.uid);

    res = xrd_close(fd, ino);

    fi->fh = 0;
    if (res == -1) {
      fuse_reply_err(req, errno);
      return;
    }
  }
  fuse_reply_err(req,0);
}


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
			    struct fuse_file_info *fi)
{
  if (fi->fh) {
    if (isdebug) fprintf(stderr,"[%s]: inode=%lld fh=%lld\n", __FUNCTION__,(long long)ino,(long long)fi->fh);
    int res = xrd_fsync(fi->fh, ino);
    if (res == -1) {
      fuse_reply_err(req, errno);
    }
  }
  fuse_reply_err(req,0);
}


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_forget (fuse_req_t req, fuse_ino_t ino, unsigned long nlookup)
{
  xrd_forget_p2i((unsigned long long) ino);
  fuse_reply_none(req);
}


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_flush (fuse_req_t req, fuse_ino_t ino,
			     struct fuse_file_info *fi) 
{
  fuse_reply_err(req,0);
}


//---------------------------------------------------------------------------------------------------
static void eosfs_ll_getxattr (fuse_req_t req, fuse_ino_t ino, const char *xattr_name, size_t size) 
{
  //filter out specific requests
  if ((!strcmp(xattr_name, "system.posix_acl_access")) || 
      (!strcmp(xattr_name, "system.posix_acl_default") ||
       (!strcmp(xattr_name, "security.capability")))) 
  {
    fuse_reply_err(req, ENODATA);
    return;
  }
 
  int retc = 0;
  size_t init_size = size;
  char fullpath[16384];

  xrd_lock_r_p2i(); // =>
  const char* name = xrd_path((unsigned long long)ino);

  if (!name) {
    fuse_reply_err(req, ENXIO);
    xrd_unlock_r_p2i(); // <=
    return;
  }

  if ((!strcmp(xattr_name, "system.posix_acl_access")) || 
      (!strcmp(xattr_name, "system.posix_acl_default"))) 
    {
      fuse_reply_err(req, ENODATA);
      xrd_unlock_r_p2i(); // <=
      return;
    }

  sprintf(fullpath,"root://%s@%s/%s/%s", xrd_mapuser(req->ctx.uid), mounthostport, mountprefix, name);
  if (isdebug) fprintf(stderr,"[%s]: inode=%lld path=%s\n", __FUNCTION__, (long long)ino, fullpath);  

  xrd_unlock_r_p2i(); // <=

  char* xattr_value = NULL;
  retc = xrd_getxattr(fullpath, xattr_name, &xattr_value, &size);

  if (retc)
    fuse_reply_err(req, ENODATA);
  else {
    if (init_size) {
      if (init_size < size)
        fuse_reply_err(req, ERANGE);
      else 
        fuse_reply_buf(req, xattr_value, size);
    }
    else 
      fuse_reply_xattr(req, size);
  }

  if (xattr_value)
    free(xattr_value);
  return;
}


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_listxattr (fuse_req_t req, fuse_ino_t ino, size_t size)
{

  int retc = 0;
  size_t init_size = size;
  char fullpath[16384];
  
  xrd_lock_r_p2i(); // =>
  const char* name = xrd_path((unsigned long long)ino);

  if (!name) {
    fuse_reply_err(req, ENXIO);
    xrd_unlock_r_p2i(); // <=
    return;
  }

  sprintf(fullpath,"root://%s@%s/%s/%s", xrd_mapuser(req->ctx.uid), mounthostport, mountprefix, name);
  if (isdebug) fprintf(stderr,"[%s]: inode=%lld path=%s\n", __FUNCTION__, (long long)ino, fullpath);  

  xrd_unlock_r_p2i(); // <=

  char* xattr_list = NULL;
  retc = xrd_listxattr(fullpath, &xattr_list, &size);
  if (retc)
    fuse_reply_err(req, retc);
  else {
    if (init_size) {
      if (init_size < size)
        fuse_reply_err(req, ERANGE);
      else 
        fuse_reply_buf(req, xattr_list, size + 1);
    }
    else 
      fuse_reply_xattr(req, size);
  }
  
  if (xattr_list)
    free(xattr_list);

  return;
}


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_removexattr (fuse_req_t req, fuse_ino_t ino, const char *xattr_name)
{
  int retc = 0;
  char fullpath[16384];

  xrd_lock_r_p2i(); // =>
  const char* name = xrd_path((unsigned long long)ino);

  if (!name) {
    fuse_reply_err(req, ENXIO);
    xrd_unlock_r_p2i(); // <=
    return;
  }

  sprintf(fullpath,"root://%s@%s/%s/%s", xrd_mapuser(req->ctx.uid), mounthostport, mountprefix, name);
  if (isdebug) fprintf(stderr,"[%s]: inode=%lld path=%s\n", __FUNCTION__, (long long)ino, fullpath);  

  xrd_unlock_r_p2i(); // <=

  retc = xrd_rmxattr(fullpath, xattr_name);
  fuse_reply_err(req, retc);

  return;
}


//--------------------------------------------------------------------------------------------------
static void eosfs_ll_setxattr (fuse_req_t req, fuse_ino_t ino, const char *xattr_name,
				const char *xattr_value, size_t size, int flags)
{
  int retc = 0;
  size_t init_size = size;
  char fullpath[16384];

  xrd_lock_r_p2i(); // =>

  const char* name = xrd_path((unsigned long long)ino);

  if (!name) {
    fuse_reply_err(req, ENXIO);
    xrd_unlock_r_p2i(); // <=
    return;
  }

  sprintf(fullpath,"root://%s@%s/%s/%s", xrd_mapuser(req->ctx.uid), mounthostport, mountprefix, name);
  if (isdebug) fprintf(stderr,"[%s]: inode=%lld path=%s\n", __FUNCTION__, (long long)ino, fullpath);  

  xrd_unlock_r_p2i(); // <=

  retc = xrd_setxattr(fullpath, xattr_name, xattr_value, strlen(xattr_value));
  fuse_reply_err(req, retc);

  return;
}

static struct fuse_lowlevel_ops eosfs_ll_oper = {
  .getattr	= eosfs_ll_getattr,
  .lookup	= eosfs_ll_lookup,
  .setattr	= eosfs_ll_setattr,
  .access	= eosfs_ll_access,
  .readlink	= eosfs_ll_readlink,
  .readdir	= eosfs_ll_readdir,
  //  .opendir      = eosfs_ll_opendir,
  .mknod	= eosfs_ll_mknod,
  .mkdir	= eosfs_ll_mkdir,
  .symlink	= eosfs_ll_symlink,
  .unlink	= eosfs_ll_unlink,
  .rmdir	= eosfs_ll_rmdir,
  .rename	= eosfs_ll_rename,
  .link	        = eosfs_ll_link,
  .open	        = eosfs_ll_open,
  .read	        = eosfs_ll_read,
  .write	= eosfs_ll_write,
  .statfs	= eosfs_ll_statfs,
  .release	= eosfs_ll_release,
  .releasedir   = eosfs_ll_releasedir,
  .fsync	= eosfs_ll_fsync,
  .forget       = eosfs_ll_forget,
  .flush        = eosfs_ll_flush,
  .setxattr     = eosfs_ll_setxattr,
  .getxattr     = eosfs_ll_getxattr,
  .listxattr    = eosfs_ll_listxattr,
  .removexattr  = eosfs_ll_removexattr
};

int main(int argc, char *argv[])
{
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  struct fuse_chan *ch;
  time_t xcfsatime;

  char *mountpoint;
  int err = -1;
  int i;

  char *epos, *spos, *rdr;

  for (i=0; i< argc; i++) {
    if (!strcmp(argv[i],"-d")) {
      isdebug = 1;
    }
  }

  if (getenv("EOS_SOCKS4_HOST") && getenv("EOS_SOCKS4_PORT")) {
    fprintf(stdout,"EOS_SOCKS4_HOST=%s\n", getenv("EOS_SOCKS4_HOST"));
    fprintf(stdout,"EOS_SOCKS4_PORT=%s\n", getenv("EOS_SOCKS4_PORT"));
    xrd_socks4(getenv("EOS_SOCKS4_HOST"),getenv("EOS_SOCKS4_PORT"));
  }

  xcfsatime=time(NULL);
  for (i=0; i< argc; i++) {
    if ( (spos=strstr(argv[i],"url=root://"))) {
      if ( (epos=strstr(spos+11,"//" )) ) {
      	//*(epos+2) = 0;
	*(spos) = 0;
	if (*(spos-1) == ',') {
	  *(spos-1) = 0;
	}
	setenv("EOS_RDRURL",spos+4,1);
      }
    }
  }

  rdr = getenv("EOS_RDRURL");
  fprintf(stdout,"EOS_RDRURL = %s\n", getenv("EOS_RDRURL"));

  if (! rdr) {
    fprintf(stderr,"error: EOS_RDRURL is not defined or add root://<host>// to the options argument\n");
    exit(-1);
  }

  if (strchr(rdr,'@') ) {
    fprintf(stderr,"error: EOS_RDRURL or url option contains user specification '@' - forbidden\n");
    exit(-1);
  }

  // move the mounthostport starting with the host name
  char* pmounthostport=0;
  char* smountprefix=0;

  pmounthostport = strstr(rdr,"root://");
  if (!pmounthostport) {
    fprintf(stderr,"error: EOS_RDRURL or url option is not valid\n");
    exit(-1);
  }

  pmounthostport += 7;
  strcpy(mounthostport,pmounthostport);

  if (!(smountprefix = strstr(mounthostport,"//"))) {
    fprintf(stderr,"error: EOS_RDRURL or url option is not valid\n");
    exit(-1);
  } else {
    smountprefix++;
    strcpy(mountprefix,smountprefix);
    *smountprefix=0;

    if (mountprefix[strlen(mountprefix)-1] == '/') {
      mountprefix[strlen(mountprefix)-1] = 0;
    }

    if (mountprefix[strlen(mountprefix)-1] == '/') {
      mountprefix[strlen(mountprefix)-1] = 0;
    }
  }
  
  fprintf(stderr,"mounthost=%s mountmountprefix=%s\n", mounthostport, mountprefix);

  if (!isdebug) {
    pid_t m_pid=fork();
    if(m_pid<0) {
      fprintf(stderr,"ERROR: Failed to fork daemon process\n");
      exit(-1);
    } 

    // kill the parent
    if(m_pid>0) {
      exit(0);
    }
    
    umask(0); 

    pid_t sid;
    if((sid=setsid()) < 0) {
      fprintf(stderr,"ERROR: failed to create new session (setsid())\n");
      exit(-1);
    }

    if ((chdir("/")) < 0) {
      /* Log any failure here */
      exit(-1);
    }
    
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    // = > don't close STDERR because we redirect that to a file!
  }

  xrd_init();

  if (fuse_parse_cmdline(&args, &mountpoint, NULL, NULL) != -1 &&
      (ch = fuse_mount(mountpoint, &args)) != NULL) {
    struct fuse_session *se;
    
    se = fuse_lowlevel_new(&args, &eosfs_ll_oper,
			   sizeof(eosfs_ll_oper), NULL);
    if (se != NULL) {
      if (fuse_set_signal_handlers(se) != -1) {
	fuse_session_add_chan(se, ch);
	err = fuse_session_loop(se);
	fuse_remove_signal_handlers(se);
	fuse_session_remove_chan(ch);
      }
      fuse_session_destroy(se);
    }
    fuse_unmount(mountpoint, ch);
  }

  fuse_opt_free_args(&args);
  
  return err ? 1 : 0;
}
