#include "animaniac.h"

#define AN_FACE_NAME_LIMIT 64

/* Object definition.
 */
 
struct an_animator {

  // Constantish source data.
  // Frames are not required to be within the image -- we always check and repair at the last moment.
  struct png_image *image;
  struct an_face {
    char *name;
    int namec;
    int rate; // in frames, zero if unspecified
    int w,h; // zero if unspecified
    int anchor; // CTR by default
    struct an_frame {
      // Optional fields (w,h,delay,anchor) are filled in at decode.
      int x,y,w,h;
      int delay; // frames
      int anchor;
    } *framev;
    int framec,framea;
  } *facev;
  int facec,facea;
  
  // Running state.
  int faceid;
  int framep;
  int dirty; // Report a change on the next update regardless of clock (eg image changed).
  int delay; // Counts down to next frame.
  
  // Image buffer for padding.
  uint8_t *buf;
  int bufa;
};

/* Cleanup.
 */
 
static void an_face_cleanup(struct an_face *face) {
  if (face->name) free(face->name);
  if (face->framev) free(face->framev);
}

void an_animator_del(struct an_animator *animator) {
  if (!animator) return;
  
  png_image_del(animator->image);
  
  if (animator->facev) {
    while (animator->facec-->0) an_face_cleanup(animator->facev+animator->facec);
    free(animator->facev);
  }
  
  if (animator->buf) free(animator->buf);

  free(animator);
}

/* New.
 */

struct an_animator *an_animator_new() {
  struct an_animator *animator=calloc(1,sizeof(struct an_animator));
  if (!animator) return 0;
  
  animator->faceid=0;
  
  return animator;
}

/* Replace image.
 */

int an_animator_set_image(struct an_animator *animator,const void *src,int srcc,const char *path) {

  struct png_image *image=png_decode(src,srcc);
  if (!image) {
    fprintf(stderr,"%s: Failed to decode PNG.\n",path);
    return -1;
  }
  
  // Image must be 32-bit RGBA.
  if ((image->colortype!=PNG_COLORTYPE_RGBA)||(image->depth!=8)) {
    struct png_image *rgba=png_image_new();
    if (!rgba||(png_image_convert(rgba,8,PNG_COLORTYPE_RGBA,image)<0)) {
      fprintf(stderr,"%s: Failed to convert image to RGBA.\n",path);
      png_image_del(image);
      png_image_del(rgba);
      return -1;
    }
    png_image_del(image);
    image=rgba;
  }
  
  // Commit the change.
  png_image_del(animator->image);
  animator->image=image;
  animator->dirty=1;
  
  return 0;
}

/* Finish decoding config.
 * Apply frame defaults.
 * Try to restore the previous selected face.
 */
 
static int an_animator_finish_config(
  struct an_animator *animator,
  const char *name,int namec,
  const char *path
) {
  if (animator->facec<1) {
    fprintf(stderr,"%s: Must declare at least one face.\n",path);
    return -1;
  }
  struct an_face *face=animator->facev;
  int faceid=0;
  for (;faceid<animator->facec;face++,faceid++) {
  
    if (face->framec<1) {
      fprintf(stderr,"%s: Face '%.*s' must declare at least one frame.\n",path,face->namec,face->name);
      return -1;
    }
  
    // Is this the restore face?
    if ((face->namec==namec)&&!memcmp(face->name,name,namec)) {
      animator->faceid=faceid;
    }
    
    // If anchor unset, it defaults to CTR.
    if (!face->anchor) face->anchor=AN_ANCHOR_CTR;
    
    // If size unset, use the largest from frames (each axis independently).
    if (!face->w||!face->h) {
      struct an_frame *frame=face->framev;
      int framei=face->framec;
      for (;framei-->0;frame++) {
        if (frame->w>face->w) face->w=frame->w;
        if (frame->h>face->h) face->h=frame->h;
      }
      // Frames might all be unset too -- that is an error.
      if (!face->w||!face->h) {
        fprintf(stderr,"%s: Unable to infer dimensions for face '%.*s'.\n",path,face->namec,face->name);
        return -1;
      }
    }
    
    // Rate is allowed to be unset, but then each frame must declare it explicitly.
    
    // Copy face defaults to anything unset in each frame.
    struct an_frame *frame=face->framev;
    int framei=face->framec;
    for (;framei-->0;frame++) {
      if (!frame->w) frame->w=face->w;
      if (!frame->h) frame->h=face->h;
      if (!frame->delay) {
        if (!face->rate) {
          fprintf(stderr,"%s: Face '%.*s' has a frame with no delay, and no default rate.\n",path,face->namec,face->name);
          return -1;
        }
        frame->delay=face->rate;
      }
      if (!frame->anchor) frame->anchor=face->anchor;
    }
  }
  return 0;
}

/* Begin decoding a new face.
 */

static struct an_face *an_animator_begin_face(
  struct an_animator *animator,
  const char *src,int srcc
) {
  
  if (animator->facec>=animator->facea) {
    int na=animator->facea+4;
    if (na>INT_MAX/sizeof(struct an_face)) return 0;
    void *nv=realloc(animator->facev,sizeof(struct an_face)*na);
    if (!nv) return 0;
    animator->facev=nv;
    animator->facea=na;
  }
  
  if ((srcc<2)||(src[0]!='[')) return 0;
  src++; srcc--;
  if (src[srcc-1]==']') srcc--; // technically optional
  if (srcc>AN_FACE_NAME_LIMIT) {
    fprintf(stderr,"Limit %d bytes for face names. (found %d)\n",AN_FACE_NAME_LIMIT,srcc);
    return 0;
  }
  int i=srcc; while (i-->0) {
    if ((src[i]<0x20)||(src[i]>0x7e)) {
      fprintf(stderr,"Illegal byte 0x%02x in face name.\n",(uint8_t)src[i]);
      return 0;
    }
  }
  int namec=srcc;
  char *name=malloc(namec+1);
  if (!name) return 0;
  memcpy(name,src,namec);
  name[namec]=0;
  
  struct an_face *face=animator->facev+animator->facec++;
  memset(face,0,sizeof(struct an_face));
  face->name=name;
  face->namec=namec;
  
  return face;
}

/* Decode face fields.
 */
 
static int an_animator_parse_face_field(
  void *kpp,int *kc,void *vpp,int *vc,
  const char *src,int srcc
) {
  if ((srcc<1)||(src[0]!='=')) return -1;
  int srcp=1;
  while ((srcp<srcc)&&((unsigned char)src[srcp]<=0x20)) srcp++;
  *(const char**)kpp=src+srcp;
  *kc=0;
  while ((srcp<srcc)&&((unsigned char)src[srcp]>0x20)) { srcp++; (*kc)++; }
  while ((srcp<srcc)&&((unsigned char)src[srcp]<=0x20)) srcp++;
  *(const char**)vpp=src+srcp;
  *vc=srcc-srcp;
  return 0;
}

static int an_face_set_field(
  struct an_face *face,
  const char *k,int kc,
  const char *v,int vc
) {

  if ((kc==4)&&!memcmp(k,"rate",4)) {
    int n=an_eval_rate(v,vc);
    if (n<0) {
      fprintf(stderr,"Failed to evaluate '%.*s' as rate. Must be 'Nhz', 'Nms', or 'Nf'.\n",vc,v);
      return -1;
    }
    face->rate=n;
    return 0;
  }
  
  if ((kc==4)&&!memcmp(k,"size",4)) {
    int w=0,h=0,err;
    if (
      ((err=an_eval_int(&w,v,vc))<1)||
      ((err=an_eval_int(&h,v+err,vc-err))<1)||
      (w<1)||(h<1)
    ) {
      fprintf(stderr,"err=%d w=%d h=%d\n",err,w,h);
      // We're not detecting trailing tokens, whatever.
      fprintf(stderr,"Failed to evaluate '%.*s' as size. Must be 'W H', >0.\n",vc,v);
      return -1;
    }
    face->w=w;
    face->h=h;
    return 0;
  }
  
  if ((kc==6)&&!memcmp(k,"anchor",6)) {
    int n=an_eval_anchor(v,vc);
    if (n<0) {
      fprintf(stderr,"Expected after 'anchor' one of: NW N NE W CTR E SW S SE\n");
      return -1;
    }
    face->anchor=n;
    return 0;
  }
  
  fprintf(stderr,"Unknown face field '%.*s', expected 'rate', 'size', or 'anchor'.\n",kc,k);
  return -1;
}

/* Decode and add a new frame.
 * - X Y (W H) (DURATION) (ANCHOR)
 */
 
static int an_animator_decode_frame(
  struct an_face *face,
  const char *src,int srcc
) {
  if ((srcc<1)||(src[0]!='-')) return -1;
  int srcp=1,x,y,w=0,h=0,delay=0,anchor=0,err;
  
  // (x,y) are required, nice and easy.
  if ((err=an_eval_int(&x,src+srcp,srcc-srcp))<1) return -1; srcp+=err;
  if ((err=an_eval_int(&y,src+srcp,srcc-srcp))<1) return -1; srcp+=err;
  
  // If the next token is a plain integer, must be (w,h).
  if ((err=an_eval_int(&w,src+srcp,srcc-srcp))>0) {
    srcp+=err;
    if ((err=an_eval_int(&h,src+srcp,srcc-srcp))<1) return -1;
    srcp+=err;
  } else w=0;
  
  // If the next token begins with a digit, it's DURATION.
  if ((srcp<srcc)&&(src[srcp]>='0')&&(src[srcp]<='9')) {
    const char *token=src+srcp;
    int tokenc=0;
    while ((srcp<srcc)&&((unsigned char)src[srcp]>0x20)) { srcp++; tokenc++; }
    if ((delay=an_eval_rate(token,tokenc))<1) return -1;
    while ((srcp<srcc)&&((unsigned char)src[srcp]<=0x20)) srcp++;
  }
  
  // Anything left must be ANCHOR.
  if (srcp<srcc) {
    if ((anchor=an_eval_anchor(src+srcp,srcc-srcp))<0) return -1;
  }
  
  if (face->framec>=face->framea) {
    int na=face->framea+4;
    if (na>INT_MAX/sizeof(struct an_frame)) return -1;
    void *nv=realloc(face->framev,sizeof(struct an_frame)*na);
    if (!nv) return -1;
    face->framev=nv;
    face->framea=na;
  }
  
  struct an_frame *frame=face->framev+face->framec++;
  memset(frame,0,sizeof(struct an_frame));
  frame->x=x;
  frame->y=y;
  frame->w=w;
  frame->h=h;
  frame->delay=delay;
  frame->anchor=anchor;
  
  return 0;
}

/* Replace config.
 */
 
int an_animator_set_config(struct an_animator *animator,const char *src,int srcc,const char *path) {

  // Record the name of the current face so we can try to restore it at the end.
  char pvfacename[AN_FACE_NAME_LIMIT];
  int pvfacenamec=0;
  if ((animator->faceid>=0)&&(animator->faceid<animator->facec)) {
    struct an_face *face=animator->facev+animator->faceid;
    memcpy(pvfacename,face->name,face->namec);
    pvfacenamec=face->namec;
  }
  
  // Drop all the existing face definitions.
  while (animator->facec>0) {
    animator->facec--;
    an_face_cleanup(animator->facev+animator->facec);
  }
  
  // Read input linewise.
  struct an_face *face=0;
  int srcp=0,lineno=0;
  while (srcp<srcc) {
  
    // Measure the next line, and strip whitespace.
    lineno++;
    const char *line=src+srcp;
    int linec=0;
    while (srcp<srcc) {
      if (src[srcp++]==0x0a) break;
      linec++;
    }
    while (linec&&((unsigned char)line[linec-1]<=0x20)) linec--;
    while (linec&&((unsigned char)line[0]<=0x20)) { linec--; line++; }
    
    // Ignore empties and comments.
    if (!linec||(line[0]=='#')) continue;
    
    // '[' introduces a new face.
    if (line[0]=='[') {
      if (!(face=an_animator_begin_face(animator,line,linec))) {
        fprintf(stderr,"%s:%d: Failed to begin new face. Is the name valid?\n",path,lineno);
        return -1;
      }
      continue;
    }
    
    // Everything else requires a face first.
    if (!face) {
      fprintf(stderr,"%s:%d: Expected face introducer on its own line: [NAME]\n",path,lineno);
      return -1;
    }
    
    // '=' sets a face field.
    if (line[0]=='=') {
      const char *k=0,*v=0;
      int kc=0,vc=0;
      if (an_animator_parse_face_field(&k,&kc,&v,&vc,line,linec)<0) {
        fprintf(stderr,"%s:%d: Expected '= KEY VALUE...'\n",path,lineno);
        return -1;
      }
      if (an_face_set_field(face,k,kc,v,vc)<0) {
        fprintf(stderr,"%s:%d: Failed to assign face field '%.*s' = '%.*s'\n",path,lineno,kc,k,vc,v);
        return -1;
      }
      continue;
    }
    
    // '-' defines a new frame.
    if (line[0]=='-') {
      if (an_animator_decode_frame(face,line,linec)<0) {
        fprintf(stderr,"%s:%d: Failed to decode frame.\n",path,lineno);
        return -1;
      }
      continue;
    }
    
    // Anything else is undefined.
    fprintf(stderr,"%s:%d: Line must begin with one of: '[' '=' '-'\n",path,lineno,linec,line);
    return -1;
  }

  return an_animator_finish_config(animator,pvfacename,pvfacenamec,path);
}

/* Produce a default image, when we've really got nothing.
 */
 
static int an_animator_get_image_default(
  void *rgbapp,int *w,int *h,int *stride,
  const struct an_animator *animator
) {
  fprintf(stderr,"...default\n");
  *(void**)rgbapp="\0\0\0\0";
  *w=1;
  *h=1;
  *stride=4;
  return 0;
}

/* Copy to our private image buffer due to size mismatches or OOB.
 */
 
static int an_animator_get_image_pad(
  void *rgbapp,int *w,int *h,int *stride,
  struct an_animator *animator,
  const struct an_face *face,
  const struct an_frame *frame
) {
  fprintf(stderr,"...pad\n");

  // Calculate output size, require buffer, and zero it.
  int dstw=face->w;
  int dsth=face->h;
  if (dstw<1) dstw=1;
  if (dsth<1) dsth=1;
  int dststride=dstw*4;
  int dstsize=dststride*dsth;
  if (dstsize>animator->bufa) {
    void *nv=realloc(animator->buf,dstsize);
    if (!nv) return -1;
    animator->buf=nv;
    animator->bufa=dstsize;
  }
  memset(animator->buf,0,dstsize);
  
  // Calculate and clip bounds.
  int dstx=0;
  int dsty=0;
  int srcx=frame->x;
  int srcy=frame->y;
  int srcw=frame->w;
  int srch=frame->h;
  if (dstw!=srcw) switch (frame->anchor) {
    case AN_ANCHOR_NW: case AN_ANCHOR_W: case AN_ANCHOR_SW: dstx=0; break;
    case AN_ANCHOR_NE: case AN_ANCHOR_E: case AN_ANCHOR_SE: dstx=dstw-srcw; break;
    default: dstx=(dstw>>1)-(srcw>>1);
  }
  if (dsth!=srch) switch (frame->anchor) {
    case AN_ANCHOR_NW: case AN_ANCHOR_N: case AN_ANCHOR_NE: dsty=0; break;
    case AN_ANCHOR_SW: case AN_ANCHOR_S: case AN_ANCHOR_SE: dsty=dsth-srch; break;
    default: dsty=(dsth>>1)-(srch>>1);
  }
  if (srcx<0) { dstx-=srcx; srcw+=srcx; srcx=0; }
  if (srcy<0) { dsty-=srcy; srch+=srcy; srcy=0; }
  if (dstx<0) { srcx-=dstx; srcw+=dstx; dstx=0; }
  if (dsty<0) { srcy-=dsty; srch+=dsty; dsty=0; }
  if (srcx+srcw>animator->image->w) srcw=animator->image->w-srcx;
  if (srcy+srch>animator->image->h) srch=animator->image->h-srcy;
  if (dstx+srcw>dstw) srcw=dstw-dstx;
  if (dsty+srch>dsth) srch=dsth-dsty;
  
  // Copy the valid range.
  int cpc=srcw<<2;
  const uint8_t *srcrow=((uint8_t*)animator->image->pixels)+srcy*animator->image->stride+(srcx<<2);
  uint8_t *dstrow=animator->buf+dsty*dststride+(dstx<<2);
  int yi=srch;
  for (;yi-->0;srcrow+=animator->image->stride,dstrow+=dststride) {
    memcpy(dstrow,srcrow,cpc);
  }
  
  *(void**)rgbapp=animator->buf;
  *w=dstw;
  *h=dsth;
  *stride=dststride;
  return 0;
}

/* Get current image.
 */
 
int an_animator_get_image(
  void *rgbapp,int *w,int *h,int *stride,
  struct an_animator *animator
) {

  // Is there a valid frame we can return? If not, use the default empty image.
  if (!animator->image) return an_animator_get_image_default(rgbapp,w,h,stride,animator);
  if ((animator->faceid<0)||(animator->faceid>=animator->facec)) {
    return an_animator_get_image_default(rgbapp,w,h,stride,animator);
  }
  const struct an_face *face=animator->facev+animator->faceid;
  if ((animator->framep<0)||(animator->framep>=face->framec)) {
    return an_animator_get_image_default(rgbapp,w,h,stride,animator);
  }
  const struct an_frame *frame=face->framev+animator->framep;
  
  // If the frame box is smaller than the shared box, draw a fresh copy of it at the face's size.
  // Our output size is always constant within one face, wm kind of depends on it.
  if ((frame->w!=face->w)||(frame->h!=face->h)) {
    return an_animator_get_image_pad(rgbapp,w,h,stride,animator,face,frame);
  }
  
  // Likewise, if the frame's box exceeds the image, use pad to create a sensible size.
  if ((frame->x<0)||(frame->y<0)||(frame->x+frame->w>animator->image->w)||(frame->y+frame->h>animator->image->h)) {
    return an_animator_get_image_pad(rgbapp,w,h,stride,animator,face,frame);
  }
  
  // OK normal cases, we can return a pointer into the source image.
  *(void**)rgbapp=((uint8_t*)(animator->image->pixels))+frame->y*animator->image->stride+(frame->x<<2);
  *w=frame->w;
  *h=frame->h;
  *stride=animator->image->stride;
  return 0;
}

/* Public access to face list.
 */

int an_animator_count_faces(const struct an_animator *animator) {
  return animator->facec;
}

int an_animator_get_face_name(char **name,const struct an_animator *animator,int faceid) {
  if ((faceid<0)||(faceid>=animator->facec)) return -1;
  const struct an_face *face=animator->facev+faceid;
  if (name) *name=face->name;
  return face->namec;
}

int an_animator_use_face(struct an_animator *animator,int faceid) {
  if ((faceid<0)||(faceid>=animator->facec)) return -1;
  if (faceid==animator->faceid) return 0;
  struct an_face *face=animator->facev+faceid;
  if (face->framec<1) return -1;
  animator->faceid=faceid;
  animator->framep=0;
  animator->delay=face->framev[0].delay;
  animator->dirty=1;
  return 0;
}

int an_animator_use_face_by_name(struct an_animator *animator,const char *name,int namec) {
  if (!name) return -1;
  if (namec<0) { namec=0; while (name[namec]) namec++; }
  int faceid=0;
  const struct an_face *face=animator->facev;
  for (;faceid<animator->facec;faceid++,face++) {
    if (face->namec!=namec) continue;
    if (memcmp(name,face->name,namec)) continue;
    return an_animator_use_face(animator,faceid);
  }
  return -1;
}

/* Update.
 */

int an_animator_update(struct an_animator *animator) {

  // We have some delay pending, so just tick it off.
  if (animator->delay>1) {
    animator->delay--;
    if (animator->dirty) {
      animator->dirty=0;
      return 1;
    }
    return 0;
  }
  
  // Advance to the next frame.
  if ((animator->faceid<0)||(animator->faceid>=animator->facec)) return 0;
  struct an_face *face=animator->facev+animator->faceid;
  if (face->framec<1) {
    fprintf(stderr,"Face '%.*s' somehow has no frames.\n",face->namec,face->name);
    return -1;
  }
  animator->framep++;
  if (animator->framep>=face->framec) animator->framep=0;
  animator->delay=face->framev[animator->framep].delay;

  return 1;
}

/* Evaluate a digit (a..z = 10..35).
 */
 
static inline int an_digit_eval(char src) {
  if ((src>='0')&&(src<='9')) return src-'0';
  if ((src>='a')&&(src<='z')) return src-'a'+10;
  if ((src>='A')&&(src<='Z')) return src-'A'+10;
  return -1;
}

/* Evaluate rate: Nhz Nms Nf
 */
 
int an_eval_rate(const char *src,int srcc) {
  
  char unit; // 'h','m','f'
  if ((srcc>=2)&&!memcmp(src+srcc-2,"hz",2)) { unit='h'; srcc-=2; }
  else if ((srcc>=2)&&!memcmp(src+srcc-2,"ms",2)) { unit='m'; srcc-=2; }
  else if ((srcc>=1)&&(src[srcc-1]=='f')) { unit='f'; srcc-=1; }
  else return -1;
  
  int n;
  if (an_eval_int(&n,src,srcc)!=srcc) return -1;
  
  switch (unit) {
    case 'h': n=60/n; if (n<1) return 1; return n;
    case 'm': n=(n*1000+8333)/16666; if (n<1) return 1; return n;
    case 'f': if (n<1) return 1; return n;
  }
  return -1;
}

/* Measure and evaluate integer with leading and trailing space.
 */
 
int an_eval_int(int *dst,const char *src,int srcc) {

  int srcp=0,positive=1,base=10;
  while ((srcp<srcc)&&((unsigned char)src[srcp]<=0x20)) srcp++;
  if (srcp>=srcc) return -1;
  
  if (src[srcp]=='-') {
    positive=0;
    if (++srcp>=srcc) return -1;
  } else if (src[srcp]=='+') {
    if (++srcp>=srcc) return -1;
  }
  if ((src[srcp]<'0')||(src[srcp]>'9')) return -1;
  
  if ((src[srcp]=='0')&&(srcp<=srcc-3)) switch (src[srcp+1]) {
    case 'x': base=16; srcp+=2; break;
    case 'd': base=10; srcp+=2; break;
    case 'o': base=8; srcp+=2; break;
    case 'b': base=2; srcp+=2; break;
  }
  
  *dst=0;
  int limit;
  if (positive) limit=UINT_MAX/base;
  else limit=INT_MIN/base;
  
  int ok=0;
  while ((srcp<srcc)&&((unsigned char)src[srcp]>0x20)) {
    ok=1;
    int digit=an_digit_eval(src[srcp++]);
    if ((digit<0)||(digit>=base)) return -1;
    if (positive) {
      if (((unsigned int)*dst)>limit) return -1;
      (*dst)*=base;
      if (((unsigned int)*dst)>INT_MAX-digit) return -1;
      (*dst)+=digit;
    } else {
      if (*dst<limit) return -1;
      (*dst)*=base;
      if (*dst<INT_MIN+digit) return -1;
      (*dst)-=digit;
    }
  }
  if (!ok) return -1;
  
  while ((srcp<srcc)&&((unsigned char)src[srcp]<=0x20)) srcp++;
  return srcp;
}

/* Evaluate anchor: NW N NE W CTR E SW S SE
 */
 
int an_eval_anchor(const char *src,int srcc) {
  switch (srcc) {
    case 1: switch (src[0]) {
        case 'N': return AN_ANCHOR_N;
        case 'W': return AN_ANCHOR_W;
        case 'E': return AN_ANCHOR_E;
        case 'S': return AN_ANCHOR_S;
      } break;
    case 2: {
        if (!memcmp(src,"NW",2)) return AN_ANCHOR_NW;
        if (!memcmp(src,"NE",2)) return AN_ANCHOR_NE;
        if (!memcmp(src,"SW",2)) return AN_ANCHOR_SW;
        if (!memcmp(src,"SE",2)) return AN_ANCHOR_SE;
      } break;
    case 3: {
        if (!memcmp(src,"CTR",3)) return AN_ANCHOR_CTR;
      } break;
  }
  return -1;
}
