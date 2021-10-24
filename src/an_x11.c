/* an_wm.c
 * Implementation of our "wm" interface.
 * I'm not building this for multiple platforms, but allowing that we might want that in the future.
 * If so, cut here.
 */

#include "animaniac.h"
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>

#define KeyRepeat (LASTEvent+2)
#define AN_X11_KEY_REPEAT_INTERVAL 10

#define AN_X11_SCALE_LIMIT 16

/* Type definition. 
 */
 
struct an_wm {
  int winw,winh; // total output (client) area
  
  void *userdata;
  int (*cb_close)(void *userdata);
  
  Display *dpy;
  int screen;
  Window win;
  GC gc;
  
  // If (dstdirty), we need to recalculate image size and position.
  // That could mean destroying and rebuilding the image object.
  // This image is sized to the logical output: fb<=image<=win
  XImage *image;
  int dstx,dsty;
  int dstdirty;
  int rshift,gshift,bshift;
  int scale;
  int srcw,srch; // Size of most recent image (ie image->(w,h)/scale)
  uint32_t bgcolor;
  
  Atom atom_WM_PROTOCOLS;
  Atom atom_WM_DELETE_WINDOW;
  Atom atom__NET_WM_STATE;
  Atom atom__NET_WM_STATE_FULLSCREEN;
  Atom atom__NET_WM_STATE_ADD;
  Atom atom__NET_WM_STATE_REMOVE;
  Atom atom__NET_WM_ICON;
};

/* Cleanup.
 */
  
void an_wm_del(struct an_wm *wm) {
  if (!wm) return;
  
  if (wm->dpy) {
    if (wm->image) XDestroyImage(wm->image);
    if (wm->gc) XFreeGC(wm->dpy,wm->gc);
    XCloseDisplay(wm->dpy);
  }
  
  free(wm);
}

/* Init.
 */
 
static int an_wm_init(struct an_wm *wm) {
  
  if (!(wm->dpy=XOpenDisplay(0))) return -1;
  wm->screen=DefaultScreen(wm->dpy);

  #define GETATOM(tag) wm->atom_##tag=XInternAtom(wm->dpy,#tag,0);
  GETATOM(WM_PROTOCOLS)
  GETATOM(WM_DELETE_WINDOW)
  GETATOM(_NET_WM_STATE)
  GETATOM(_NET_WM_STATE_FULLSCREEN)
  GETATOM(_NET_WM_STATE_ADD)
  GETATOM(_NET_WM_STATE_REMOVE)
  GETATOM(_NET_WM_ICON)
  #undef GETATOM
  
  XSetWindowAttributes wattr={
    .background_pixel=0x80808080,
    .event_mask=
      StructureNotifyMask|
      KeyPressMask|KeyReleaseMask|
    0,
  };
  
  wm->winw=640;
  wm->winh=480;
  
  if (!(wm->win=XCreateWindow(
    wm->dpy,RootWindow(wm->dpy,wm->screen),
    0,0,wm->winw,wm->winh,0,
    DefaultDepth(wm->dpy,wm->screen),InputOutput,CopyFromParent,
    CWBackPixel|CWBorderPixel|CWColormap|CWEventMask,&wattr
  ))) return -1;
  
  XMapWindow(wm->dpy,wm->win);
  
  XSync(wm->dpy,0);
  
  XSetWMProtocols(wm->dpy,wm->win,&wm->atom_WM_DELETE_WINDOW,1);
  
  if (!(wm->gc=XCreateGC(wm->dpy,wm->win,0,0))) return -1;
  
  XStoreName(wm->dpy,wm->win,"Animaniac");
  
  return 0;
}

/* New.
 */
 
struct an_wm *an_wm_new(
  int (*cb_close)(void *userdata),
  void *userdata
) {
  struct an_wm *wm=calloc(1,sizeof(struct an_wm));
  if (!wm) return 0;
  
  wm->cb_close=cb_close;
  wm->userdata=userdata;
  
  wm->dstdirty=1;
  
  if (an_wm_init(wm)<0) {
    an_wm_del(wm);
    return 0;
  }
  
  return wm;
}

/* Select framebuffer's output bounds.
 */

static int an_wm_recalculate_output_bounds(struct an_wm *wm) {

  /* First decide the scale factor:
   *  - At least 1.
   *  - At most AN_X11_SCALE_LIMIT.
   *  - Or lesser of winw/srcw,winh/srch.
   */
  int scalex=wm->winw/wm->srcw;
  int scaley=wm->winh/wm->srch;
  wm->scale=(scalex<scaley)?scalex:scaley;
  if (wm->scale<1) wm->scale=1;
  else if (wm->scale>AN_X11_SCALE_LIMIT) wm->scale=AN_X11_SCALE_LIMIT;
  
  /* From there, size and position are trivial:
   */
  int dstw=wm->srcw*wm->scale;
  int dsth=wm->srch*wm->scale;
  wm->dstx=(wm->winw>>1)-(dstw>>1);
  wm->dsty=(wm->winh>>1)-(dsth>>1);
  
  /* If the image is not yet created, or doesn't match the calculated size, rebuild it.
   */
  if (!wm->image||(wm->image->width!=dstw)||(wm->image->height!=dsth)) {
    if (wm->image) {
      XDestroyImage(wm->image);
      wm->image=0;
    }
    void *pixels=malloc(dstw*4*dsth);
    if (!pixels) return -1;
    if (!(wm->image=XCreateImage(
      wm->dpy,DefaultVisual(wm->dpy,wm->screen),24,ZPixmap,0,pixels,dstw,dsth,32,dstw*4
    ))) {
      free(pixels);
      return -1;
    }
    
    // And recalculate channel shifts...
    if (!wm->image->red_mask||!wm->image->green_mask||!wm->image->blue_mask) return -1;
    uint32_t m;
    wm->rshift=0; m=wm->image->red_mask;   for (;!(m&1);m>>=1,wm->rshift++) ; if (m!=0xff) return -1;
    wm->gshift=0; m=wm->image->green_mask; for (;!(m&1);m>>=1,wm->gshift++) ; if (m!=0xff) return -1;
    wm->bshift=0; m=wm->image->blue_mask;  for (;!(m&1);m>>=1,wm->bshift++) ; if (m!=0xff) return -1;
    wm->bgcolor=(0x80<<wm->rshift)|(0x80<<wm->gshift)|(0x80<<wm->bshift);
  }
  
  return 0;
}

/* Scale user's RGBA image into our final-size buffer.
 * Input dimensions must already be stored as (wm->srcw,wm->srch).
 * Size of (wm->image) must already agree with (scale,dstw,dsth,srcw,srch).
 */
 
static void an_wm_scale_image(struct an_wm *wm,const void *src,int stride) {
  const uint8_t *srcrow=src;
  uint32_t *dst=(void*)wm->image->data;
  int cpc=wm->image->width*4;
  int yi=wm->srch;
  for (;yi-->0;srcrow+=stride) {
    const uint8_t *srcp=srcrow;
    uint32_t *dststart=dst;
    int xi=wm->srcw;
    for (;xi-->0;srcp+=4) {
      // Nonzero alpha becomes fully opaque, zero alpha becomes background color.
      uint32_t pixel;
      if (srcp[3]) {
        pixel=(srcp[0]<<wm->rshift)|(srcp[1]<<wm->gshift)|(srcp[2]<<wm->bshift);
      } else {
        pixel=wm->bgcolor;
      }
      int ri=wm->scale;
      for (;ri-->0;dst++) *dst=pixel;
    }
    int ri=wm->scale-1;
    for (;ri-->0;dst+=wm->image->width) memcpy(dst,dststart,cpc);
  }
}

/* Send new image.
 */
 
int an_wm_set_image(
  struct an_wm *wm,
  const void *rgba,
  int w,int h,int stride
) {
  if (!rgba||(w<1)||(h<1)||(stride<w<<2)) return -1;
  if (wm->dstdirty||(w!=wm->srcw)||(h!=wm->srch)) {
    wm->srcw=w;
    wm->srch=h;
    if (an_wm_recalculate_output_bounds(wm)<0) return -1;
    wm->dstdirty=0;
    XClearWindow(wm->dpy,wm->win);
  }
  an_wm_scale_image(wm,rgba,stride);
  XPutImage(wm->dpy,wm->win,wm->gc,wm->image,0,0,wm->dstx,wm->dsty,wm->image->width,wm->image->height);
  return 0;
}

/* Process one event.
 */
 
static int an_wm_receive_event(struct an_wm *wm,XEvent *evt) {
  if (!evt) return -1;
  switch (evt->type) {
  
    case KeyPress: 
    case KeyRelease: {
        KeySym keysym=XkbKeycodeToKeysym(wm->dpy,evt->xkey.keycode,0,0);
        switch (keysym) {
          //TODO pick face, etc
          case XK_Escape: if (wm->cb_close) return wm->cb_close(wm->userdata); return 0;
        }
      } break;
    
    case ClientMessage: {
        if (evt->xclient.message_type==wm->atom_WM_PROTOCOLS) {
          if (evt->xclient.format==32) {
            if (evt->xclient.data.l[0]==wm->atom_WM_DELETE_WINDOW) {
              if (wm->cb_close) return wm->cb_close(wm->userdata);
            }
          }
        }
      } break;
    
    case ConfigureNotify: {
        int nw=evt->xconfigure.width,nh=evt->xconfigure.height;
        if ((nw!=wm->winw)||(nh!=wm->winh)) {
          wm->winw=nw;
          wm->winh=nh;
          wm->dstdirty=1;
        }
      } break;
    
  }
  return 0;
}

/* Update.
 */
 
int an_wm_update(struct an_wm *wm) {
  int evtc=XEventsQueued(wm->dpy,QueuedAfterFlush);
  while (evtc-->0) {
    XEvent evt={0};
    XNextEvent(wm->dpy,&evt);
    
    /* If we detect an auto-repeated key, drop one of the events, and turn the other into KeyRepeat.
     * This is a hack to force single events for key repeat.
     */
    if ((evtc>0)&&(evt.type==KeyRelease)) {
      XEvent next={0};
      XNextEvent(wm->dpy,&next);
      evtc--;
      if ((next.type==KeyPress)&&(evt.xkey.keycode==next.xkey.keycode)&&(evt.xkey.time>=next.xkey.time-AN_X11_KEY_REPEAT_INTERVAL)) {
        evt.type=KeyRepeat;
        if (an_wm_receive_event(wm,&evt)<0) return -1;
      } else {
        if (an_wm_receive_event(wm,&evt)<0) return -1;
        if (an_wm_receive_event(wm,&next)<0) return -1;
      }
    } else {
      if (an_wm_receive_event(wm,&evt)<0) return -1;
    }
  }
  return 1;
}
