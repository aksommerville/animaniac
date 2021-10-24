#include "animaniac.h"
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

// This is for MS Windows compatibility.
// Though... we're using '/' literally as path separator, so it will still be a challenge.
#ifndef O_BINARY
  #define O_BINARY 0
#endif

// Seekless read is technically unbounded, so we need to enforce some crazy high limit.
#define FS_SEEKLESS_SANITY_LIMIT 0x10000000

/* Read from open file into new buffer until EOF.
 */
 
static int file_read_seekless(void *dstpp,int fd) {
  int dstc=0,dsta=8192;
  char *dst=malloc(dsta);
  if (!dst) return -1;
  while (1) {
    if (dstc>=dsta) {
      if (dsta>=FS_SEEKLESS_SANITY_LIMIT) break;
      dsta<<=1;
      char *nv=malloc(dsta);
      if (!nv) {
        free(dst);
        return -1;
      }
      dst=nv;
    }
    int err=read(fd,dst+dstc,dsta-dstc);
    if (err<0) {
      free(dst);
      return -1;
    }
    if (!err) {
      *(void**)dstpp=dst;
      return dstc;
    }
    dstc+=err;
  }
}

/* Read file.
 */
 
int an_file_read(void *dstpp,const char *path) {
  if (!dstpp||!path) return -1;
  int fd=open(path,O_RDONLY|O_BINARY);
  if (fd<0) return -1;
  
  off_t flen=lseek(fd,0,SEEK_END);
  if (flen<0) {
    int err=file_read_seekless(dstpp,fd);
    close(fd);
    return err;
  }
  if ((flen>INT_MAX)||lseek(fd,0,SEEK_SET)) {
    close(fd);
    return -1;
  }
  
  char *dst=malloc(flen?flen:1);
  if (!dst) {
    close(fd);
    return -1;
  }
  int dstc=0;
  while (dstc<flen) {
    int err=read(fd,dst+dstc,flen-dstc);
    if (err<=0) {
      close(fd);
      free(dst);
      return -1;
    }
    dstc+=err;
  }
  close(fd);
  *(void**)dstpp=dst;
  return dstc;
}

/* Write file.
 */

int an_file_write(const char *path,const void *src,int srcc) {
  if (!path||(srcc<0)||(srcc&&!src)) return -1;
  int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC|O_BINARY,0666);
  if (fd<0) return -1;
  int srcp=0;
  while (srcp<srcc) {
    int err=write(fd,(char*)src+srcp,srcc-srcp);
    if (err<=0) {
      close(fd);
      unlink(path);
      return -1;
    }
    srcp+=err;
  }
  close(fd);
  return 0;
}

/* Call (cb) for each file immediately under directory (path).
 * (type) is zero if dirent doesn't provide it, otherwise see file_get_type().
 * Terminates if (cb) returns nonzero, and returns the same.
 */

int an_dir_read(
  const char *path,
  int (*cb)(const char *path,const char *base,char type,void *userdata),
  void *userdata
) {
  if (!path||!path[0]) return -1;
  DIR *dir=opendir(path);
  if (!dir) return -1;
  
  char subpath[1024];
  int pathc=0;
  while (path[pathc]) pathc++;
  if (pathc>=sizeof(subpath)) {
    closedir(dir);
    return -1;
  }
  memcpy(subpath,path,pathc);
  if (subpath[pathc-1]!='/') subpath[pathc++]='/';
  
  struct dirent *de;
  while (de=readdir(dir)) {
    const char *base=de->d_name;
    int basec=0;
    while (base[basec]) basec++;
    
    if (!basec) continue;
    if ((basec==1)&&(base[0]=='.')) continue;
    if ((basec==2)&&(base[0]=='.')&&(base[1]=='.')) continue;
    
    if (pathc>=sizeof(subpath)-basec) {
      closedir(dir);
      return -1;
    }
    memcpy(subpath+pathc,base,basec+1);
    
    char type=0;
    switch (de->d_type) {
      case DT_REG: type='f'; break;
      case DT_DIR: type='d'; break;
      case DT_CHR: type='c'; break;
      case DT_BLK: type='b'; break;
      case DT_LNK: type='l'; break;
      case DT_SOCK: type='s'; break;
      default: type='?';
    }
    
    int err=cb(subpath,base,type,userdata);
    if (err) {
      closedir(dir);
      return err;
    }
  }
  closedir(dir);
  return 0;
}

/* File type.
 */
 
char an_file_get_type(const char *path) {
  struct stat st={0};
  if (stat(path,&st)<0) return 0;
  if (S_ISREG(st.st_mode)) return 'f';
  if (S_ISDIR(st.st_mode)) return 'd';
  if (S_ISCHR(st.st_mode)) return 'c';
  if (S_ISBLK(st.st_mode)) return 'b';
  if (S_ISSOCK(st.st_mode)) return 's';
  if (S_ISLNK(st.st_mode)) return 'l';
  return '?';
}
