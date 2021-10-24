/* an_inmgr.c
 * Manages stdin, signals, and inotify.
 * Likely to change if we port to some other platform -- inotify is a Linux thing.
 */

#include "animaniac.h"
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/poll.h>

typedef void (*sighandler_t)(int);

#define AN_INMGR_DIRTY_TIME 10

/* Object definition.
 */
 
struct an_inmgr {
  int (*cb_file)(const char *path,void *userdata);
  int (*cb_stdin)(const void *src,int srcc,void *userdata);
  void *userdata;
  
  int infd;
  int stdinfd;
  sighandler_t sigintpv;
  int dirtytime;
  
  struct an_inmgr_file {
    char *path;
    int pathc,dirc;
    int wd;
    // Ready to report, but we sit on it a few frames to reduce frequency.
    // Before this feature, we were zany to the max, but now we sit back and relax.
    int dirty;
  } *filev;
  int filec,filea;
};

static volatile int an_inmgr_sigc=0;

/* Signal handler.
 */
 
static void an_inmgr_rcvsig(int sigid) {
  switch (sigid) {
    case SIGINT: if (++an_inmgr_sigc>=3) {
        fprintf(stderr,"Too many unprocessed signals.\n");
        exit(1);
      } break;
  }
}

/* Delete.
 */
 
// This does not remove the inotify watch. (that will happen implicitly when we close the inotify fd).
static void an_inmgr_file_cleanup(struct an_inmgr_file *file) {
  if (file->path) free(file->path);
}

void an_inmgr_del(struct an_inmgr *inmgr) {
  if (!inmgr) return;
  
  if (inmgr->infd>=0) close(inmgr->infd);
  
  // if (inmgr->stdinfd>=0) close(inmgr->stdinfd); // assume it's the real stdin and don't close it.
  
  if (inmgr->sigintpv) signal(SIGINT,inmgr->sigintpv);
  
  if (inmgr->filev) {
    while (inmgr->filec-->0) an_inmgr_file_cleanup(inmgr->filev+inmgr->filec);
    free(inmgr->filev);
  }
  
  free(inmgr);
}

/* Init.
 */
 
static int an_inmgr_init_inotify(struct an_inmgr *inmgr) {
  if ((inmgr->infd=inotify_init())<0) return -1;
  return 0;
}

static int an_inmgr_init_signals(struct an_inmgr *inmgr) {
  inmgr->sigintpv=signal(SIGINT,an_inmgr_rcvsig);
  return 0;
}
 
static int an_inmgr_init(struct an_inmgr *inmgr) {
  if (inmgr->cb_file) {
    if (an_inmgr_init_inotify(inmgr)<0) return -1;
  }
  if (inmgr->cb_stdin) {
    // Signals and stdin.
    inmgr->stdinfd=STDIN_FILENO;
    if (an_inmgr_init_signals(inmgr)<0) return -1;
  }
  return 0;
}

/* New.
 */

struct an_inmgr *an_inmgr_new(
  int (*cb_file)(const char *path,void *userdata),
  int (*cb_stdin)(const void *src,int srcc,void *userdata),
  void *userdata
) {
  struct an_inmgr *inmgr=calloc(1,sizeof(struct an_inmgr));
  if (!inmgr) return 0;
  
  inmgr->cb_file=cb_file;
  inmgr->cb_stdin=cb_stdin;
  inmgr->userdata=userdata;
  inmgr->infd=-1;
  inmgr->stdinfd=-1;
  
  if (an_inmgr_init(inmgr)<0) {
    an_inmgr_del(inmgr);
    return 0;
  }
  return inmgr;
}

/* Add file for inotify.
 */

int an_inmgr_add_file(struct an_inmgr *inmgr,const char *path) {
  if (inmgr->infd<0) return -1;
  
  int pathc=0,dirc=0;
  while (path[pathc]) {
    if (path[pathc]=='/') dirc=pathc;
    pathc++;
  }
  
  if (inmgr->filec>=inmgr->filea) {
    int na=inmgr->filea+2;
    if (na>INT_MAX/sizeof(struct an_inmgr_file)) return -1;
    void *nv=realloc(inmgr->filev,sizeof(struct an_inmgr_file)*na);
    if (!nv) return -1;
    inmgr->filev=nv;
    inmgr->filea=na;
  }
  
  int wd=-1;
  const struct an_inmgr_file *q=inmgr->filev;
  int i=inmgr->filec;
  for (;i-->0;q++) {
    if ((q->dirc==dirc)&&!memcmp(q->path,path,dirc)) {
      wd=q->wd;
      break;
    }
  }
  if (wd<0) {
    char dirname[1024];
    if (dirc>=sizeof(dirname)) return -1;
    if (!dirc) strcpy(dirname,".");
    else {
      memcpy(dirname,path,dirc);
      dirname[dirc]=0;
    }
    if ((wd=inotify_add_watch(inmgr->infd,dirname,IN_CLOSE_WRITE))<0) {
      fprintf(stderr,"%s: Failed to add inotify watch.\n",dirname);
      return -1;
    }
  }
  
  struct an_inmgr_file *file=inmgr->filev+inmgr->filec++;
  memset(file,0,sizeof(struct an_inmgr_file));
  if (!(file->path=malloc(pathc+1))) {
    inmgr->filec--;
    return -1;
  }
  memcpy(file->path,path,pathc);
  file->pathc=pathc;
  file->dirc=dirc;
  file->wd=wd;
  file->dirty=1; // New files get an initial report whether they change or not.
  inmgr->dirtytime=AN_INMGR_DIRTY_TIME;
  
  //fprintf(stderr,"%s: Watching file via wd %d\n",path,file->wd);
  
  return 0;
}

/* Search for file.
 */
 
static struct an_inmgr_file *an_inmgr_find_file(
  const struct an_inmgr *inmgr,
  int wd,const char *base,int basec
) {
  struct an_inmgr_file *file=inmgr->filev;
  int i=inmgr->filec;
  for (;i-->0;file++) {
    if (file->wd!=wd) continue;
    if (basec>file->pathc) continue;
    if (memcmp(file->path+file->pathc-basec,base,basec)) continue;
    return file;
  }
  return 0;
}

/* Receive input from inotify.
 */
 
static int an_inmgr_read_inotify(struct an_inmgr *inmgr,const char *src,int srcc) {
  int srcp=0;
  while (srcp<srcc-(int)sizeof(struct inotify_event)) {
    struct inotify_event *event=(struct inotify_event*)(src+srcp);
    srcp+=sizeof(struct inotify_event);
    if (srcp>srcc-event->len) break;
    srcp+=event->len;
    const char *base=event->name;
    int basec=0;
    while ((basec<event->len)&&base[basec]) basec++;
    // We watch the parent dir. not found, no big deal.
    struct an_inmgr_file *file=an_inmgr_find_file(inmgr,event->wd,base,basec);
    if (!file) continue;
    // Mark it for later reporting.
    file->dirty=1;
    inmgr->dirtytime=AN_INMGR_DIRTY_TIME;
  }
  return 0;
}

/* Receive input from stdin.
 */
 
static int an_inmgr_read_stdin(struct an_inmgr *inmgr,const char *src,int srcc) {
  if (inmgr->cb_stdin) return inmgr->cb_stdin(src,srcc,inmgr->userdata);
  return 0;
}

/* Close file.
 */
 
static int an_inmgr_file_closed(struct an_inmgr *inmgr,int fd) {
  if (fd==inmgr->infd) {
    close(inmgr->infd);
    inmgr->infd=-1;
    return 0;
  }
  if (fd==inmgr->stdinfd) {
    inmgr->stdinfd=-1;
    if (inmgr->cb_stdin) return inmgr->cb_stdin(0,0,inmgr->userdata);
    return 0;
  }
  return 0; // ?
}

/* File ready for reading, or closed.
 */
 
static int an_inmgr_read_file(struct an_inmgr *inmgr,int fd) {
  char buf[1024];
  int bufc=read(fd,buf,sizeof(buf));
  if (bufc<=0) return an_inmgr_file_closed(inmgr,fd);
  if (fd==inmgr->infd) return an_inmgr_read_inotify(inmgr,buf,bufc);
  if (fd==inmgr->stdinfd) return an_inmgr_read_stdin(inmgr,buf,bufc);
  return -1;
}

/* Update.
 */

int an_inmgr_update(struct an_inmgr *inmgr) {

  if (an_inmgr_sigc) {
    an_inmgr_sigc=0;
    if (!inmgr->cb_stdin) return -1;
    int err=inmgr->cb_stdin(0,0,inmgr->userdata);
    if (err<0) return err;
  }
  
  // Tick the dirty clock and notify user if it strikes zero.
  if (inmgr->dirtytime>0) {
    if (!--(inmgr->dirtytime)) {
      struct an_inmgr_file *file=inmgr->filev;
      int i=inmgr->filec;
      for (;i-->0;file++) {
        if (file->dirty) {
          if (inmgr->cb_file(file->path,inmgr->userdata)<0) return -1;
          file->dirty=0;
        }
      }
    }
  }

  // There can be no more than 2 files to poll, so I'm not going dynamic with it.
  struct pollfd pollfdv[2]={0};
  int pollfdc=0;
  if (inmgr->infd>=0) {
    pollfdv[pollfdc].fd=inmgr->infd;
    pollfdv[pollfdc].events=POLLIN|POLLERR|POLLHUP;
    pollfdc++;
  }
  if (inmgr->stdinfd>=0) {
    pollfdv[pollfdc].fd=inmgr->stdinfd;
    pollfdv[pollfdc].events=POLLIN|POLLERR|POLLHUP;
    pollfdc++;
  }
  
  if (!pollfdc) return 0;
  if (poll(pollfdv,pollfdc,0)<1) return 0;
  
  if (pollfdv[0].revents) {
    if (an_inmgr_read_file(inmgr,pollfdv[0].fd)<0) return -1;
  }
  if (pollfdv[1].revents) { // zero if c<2, don't worry
    if (an_inmgr_read_file(inmgr,pollfdv[1].fd)<0) return -1;
  }

  return 0;
}
