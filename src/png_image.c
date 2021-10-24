#include "animaniac.h"

/* Object lifecycle.
 */
 
void png_image_cleanup(struct png_image *image) {
  if (!image) return;
  if (image->pixels) free(image->pixels);
  if (image->chunkv) {
    struct png_chunk *chunk=image->chunkv;
    int i=image->chunkc;
    for (;i-->0;chunk++) {
      if (chunk->v) free(chunk->v);
    }
    free(image->chunkv);
  }
  memset(image,0,sizeof(struct png_image));
}

void png_image_del(struct png_image *image) {
  if (!image) return;
  if (image->refc) {
    if (image->refc-->0) return;
    png_image_cleanup(image);
    free(image);
  } else {
    png_image_cleanup(image);
  }
}

int png_image_ref(struct png_image *image) {
  if (!image) return -1;
  if (image->refc<1) return -1;
  if (image->refc==INT_MAX) return -1;
  image->refc++;
  return 0;
}

struct png_image *png_image_new() {
  struct png_image *image=calloc(1,sizeof(struct png_image));
  if (!image) return 0;
  image->refc=1;
  return image;
}

/* Convert from indexed image with PLTE.
 */
 
static void png_image_convert_from_index(
  struct png_image *dst,
  const struct png_image *src,
  const uint8_t *plte,int pltec,
  const uint8_t *trns,int trnsc
) {

  // (pltec) is in bytes. Rephrase as pixels.
  pltec/=3;
  
  uint8_t *dstrow=dst->pixels;
  const uint8_t *srcrow=src->pixels;
  int y=dst->h;
  
  /* Optimize for rgb8 and rgba8 from i8.
   * The format of PLTE is always rgb8, that's why these formats are privileged.
   */
  if (
    (dst->depth==8)&&(src->depth==8)&&
    ((dst->colortype==PNG_COLORTYPE_RGB)||(dst->colortype==PNG_COLORTYPE_RGBA))
  ) {
    for (;y-->0;dstrow+=dst->stride,srcrow+=src->stride) {
      uint8_t *dstp=dstrow;
      const uint8_t *srcp=srcrow;
      int x=dst->w;
      if (dst->colortype==PNG_COLORTYPE_RGBA) {
        for (;x-->0;dstp+=4,srcp++) {
          if (*srcp>=pltec) dstp[0]=dstp[1]=dstp[2]=0x00;
          else memcpy(dstp,plte+(*srcp)*3,3);
          if (*srcp>=trnsc) dstp[3]=0xff;
          else dstp[3]=trns[*srcp];
        }
      } else {
        for (;x-->0;dstp+=3,srcp++) {
          if (*srcp>=pltec) dstp[0]=dstp[1]=dstp[2]=0x00;
          else memcpy(dstp,plte+(*srcp)*3,3);
        }
      }
    }
    return;
  }

  /* Use a generic writer, but implement reading 4 times for the 4 legal input depths.
   * We do not provide a generic reader for indexed pixels.
   */
  png_pxwr_fn wr=png_get_pxwr(dst->depth,dst->colortype);
  if (!wr) return;
  switch (src->depth) {
  
    case 1: {
        uint8_t tmpplte[6]={0};
        if (pltec==1) memcpy(tmpplte,plte,3);
        else if (pltec>=2) memcpy(tmpplte,plte,6);
        plte=tmpplte;
        uint8_t tmptrns[2];
        if (trnsc<1) { tmptrns[0]=tmptrns[1]=0xff; trns=tmptrns; }
        else if (trnsc<2) { tmptrns[0]=trns[0]; tmptrns[1]=0xff; trns=tmptrns; }
        for (;y-->0;dstrow+=dst->stride,srcrow+=src->stride) {
          const uint8_t *srcp=srcrow;
          uint8_t mask=0x80;
          int x=0;
          for (;x<dst->w;x++) {
            int ix=((*srcp)&mask)?1:0;
            uint8_t a=trns[ix];
            ix*=3;
            uint32_t rgba=(plte[ix]<<24)|(plte[ix+1]<<16)|(plte[ix+2]<<8)|a;
            wr(dstrow,x,rgba);
            if (!(mask>>=1)) {
              mask=0x80;
              srcp++;
            }
          }
        }
      } break;
      
    case 2: {
        for (;y-->0;dstrow+=dst->stride,srcrow+=src->stride) {
          const uint8_t *srcp=srcrow;
          int shift=6;
          int x=0;
          for (;x<dst->w;x++) {
            int ix=((*srcp)>>shift)&3;
            uint8_t a=(ix<trnsc)?trns[ix]:0xff;
            uint32_t rgb=0;
            if (ix<pltec) {
              ix*=3;
              rgb=(plte[ix]<<24)|(plte[ix+1]<<16)|(plte[ix+2]<<8);
            }
            wr(dstrow,x,rgb|a);
            if ((shift-=2)<0) {
              shift=6;
              srcp++;
            }
          }
        }
      } break;
      
    case 4: {
        for (;y-->0;dstrow+=dst->stride,srcrow+=src->stride) {
          const uint8_t *srcp=srcrow;
          int shift=4;
          int x=0;
          for (;x<dst->w;x++) {
            int ix=((*srcp)>>shift)&15;
            uint8_t a=(ix<trnsc)?trns[ix]:0xff;
            uint32_t rgb=0;
            if (ix<pltec) {
              ix*=3;
              rgb=(plte[ix]<<24)|(plte[ix+1]<<16)|(plte[ix+2]<<8);
            }
            wr(dstrow,x,rgb|a);
            if (shift) shift=0;
            else { shift=4; srcp++; }
          }
        }
      } break;
      
    case 8: {
        for (;y-->0;dstrow+=dst->stride,srcrow+=src->stride) {
          const uint8_t *srcp=srcrow;
          int x=0;
          for (;x<dst->w;x++,srcp++) {
            int ix=*srcp;
            uint8_t a=(ix<trnsc)?trns[ix]:0xff;
            uint32_t rgb=0;
            if (ix<pltec) {
              ix*=3;
              rgb=(plte[ix]<<24)|(plte[ix+1]<<16)|(plte[ix+2]<<8);
            }
            wr(dstrow,x,rgb|a);
          }
        }
      } break;
  }
}

/* Convert.
 */

int png_image_convert(
  struct png_image *dst,
  uint8_t depth,uint8_t colortype,
  const struct png_image *src
) {
  if (!dst||!src) return -1;
  if (png_image_allocate_pixels(dst,src->w,src->h,depth,colortype)<0) return -1;
  
  // If format is the same, memcpy the whole thing, it's way cheaper.
  // This means we're being used as "copy image", which is sane, there is no literal "copy image".
  // Check stride too. Images produced by us will always have minimal stride, but user-supplied ones maybe not.
  if ((dst->colortype==src->colortype)&&(dst->depth==src->depth)) {
    if (dst->stride==src->stride) {
      memcpy(dst->pixels,src->pixels,dst->stride*dst->h);
    } else {
      uint8_t *dstrow=dst->pixels;
      const uint8_t *srcrow=src->pixels;
      int i=dst->h;
      for (;i-->0;dstrow+=dst->stride,srcrow+=src->stride) {
        memcpy(dstrow,srcrow,dst->stride);
      }
    }
    return 0;
  }
  
  // Indexed pixels are a mess, of course.
  if (src->colortype==PNG_COLORTYPE_INDEX) {
    const void *srcplte=0,*srctrns=0;
    int srcpltec=png_image_get_chunk_by_id(&srcplte,src,PNG_ID('P','L','T','E'));
    if (srcpltec>=0) { // PLTE exists so we're doing this...
      int srctrnsc=png_image_get_chunk_by_id(&srctrns,src,PNG_ID('t','R','N','S'));
      if (srctrnsc<0) srctrnsc=0;
      png_image_convert_from_index(dst,src,srcplte,srcpltec,srctrns,srctrnsc);
      return 0;
    }
    // No PLTE, proceed and let the input behave like gray.
  }
  
  // Expensive generic conversion with accessor functions.
  png_pxrd_fn rd=png_get_pxrd(src->depth,src->colortype);
  if (!rd) return -1;
  png_pxwr_fn wr=png_get_pxwr(dst->depth,dst->colortype);
  if (!wr) return -1;
  uint8_t *dstrow=dst->pixels;
  const uint8_t *srcrow=src->pixels;
  int y=dst->h;
  for (;y-->0;dstrow+=dst->stride,srcrow+=src->stride) {
    int x=dst->w;
    while (x-->0) wr(dstrow,x,rd(srcrow,x));
  }
  return 0;
}

/* Allocate pixels.
 */

int png_image_allocate_pixels(
  struct png_image *image,
  int32_t w,int32_t h,
  uint8_t depth,uint8_t colortype
) {
  if (!image) return -1;
  
  // Ensure sane format and dimensions.
  if ((w<1)||(h<1)) return -1;
  int pixelsize=png_pixelsize_for_format(depth,colortype);
  if (pixelsize<1) return -1;
  if (pixelsize>(INT_MAX-7)/w) return -1;
  int stride=(pixelsize*w+7)>>3;
  if (stride<1) return -1; // i think not possible? meh let's check and be safe.
  if (stride>INT_MAX/h) return -1;
  
  void *pixels=calloc(stride,h);
  if (!pixels) return -1;
  if (image->pixels) free(image->pixels);
  image->pixels=pixels;
  image->stride=stride;
  image->pixelsize=pixelsize;
  image->w=w;
  image->h=h;
  image->depth=depth;
  image->colortype=colortype;
  
  return 0;
}

/* Add chunk.
 */
 
static int png_image_chunkv_require(struct png_image *image) {
  if (!image) return -1;
  if (image->chunkc<image->chunka) return 0;
  int na=image->chunka+8;
  if (na>INT_MAX/sizeof(struct png_chunk)) return -1;
  void *nv=realloc(image->chunkv,sizeof(struct png_chunk)*na);
  if (!nv) return -1;
  image->chunkv=nv;
  image->chunka=na;
  return 0;
}

int png_image_add_chunk_handoff(struct png_image *image,uint32_t id,void *v,int c) {
  if ((c<0)||(c&&!v)) return -1;
  if (png_image_chunkv_require(image)<0) return -1;
  struct png_chunk *chunk=image->chunkv+image->chunkc++;
  chunk->id=id;
  chunk->v=v;
  chunk->c=c;
  return 0;
}

int png_image_add_chunk_copy(struct png_image *image,uint32_t id,const void *v,int c) {
  if ((c<0)||(c&&!v)) return -1;
  if (png_image_chunkv_require(image)<0) return -1;
  void *nv=0;
  if (c) {
    if (!(nv=malloc(c))) return -1;
    memcpy(nv,v,c);
  }
  struct png_chunk *chunk=image->chunkv+image->chunkc++;
  chunk->id=id;
  chunk->v=nv;
  chunk->c=c;
  return 0;
}

/* Find chunk.
 */

int png_image_get_chunk_by_id(void *dstpp,const struct png_image *image,uint32_t id) {
  if (!image) return -1;
  struct png_chunk *chunk=image->chunkv;
  int i=image->chunkc;
  for (;i-->0;chunk++) {
    if (chunk->id==id) {
      if (dstpp) *(void**)dstpp=chunk->v;
      return chunk->c;
    }
  }
  return -1;
}

/* Validate pixel format.
 */

int png_pixelsize_for_format(uint8_t depth,uint8_t colortype) {
  switch (colortype) {
    case PNG_COLORTYPE_GRAY: switch (depth) {
        case 1: case 2: case 4: case 8: case 16: return depth;
      } break;
    case PNG_COLORTYPE_RGB: switch (depth) {
        case 8: case 16: return depth*3;
      } break;
    case PNG_COLORTYPE_INDEX: switch (depth) {
        case 1: case 2: case 4: case 8: return depth;
      } break;
    case PNG_COLORTYPE_GRAYA: switch (depth) {
        case 8: case 16: return depth<<1;
      } break;
    case PNG_COLORTYPE_RGBA: switch (depth) {
        case 8: case 16: return depth<<2;
      } break;
  }
  return 0;
}

/* Pixel access functions.
 */
 
#define SRC ((const uint8_t*)src)
#define DST ((uint8_t*)dst)
 
static uint32_t png_pxrd_y1(const void *src,int x) {
  return (SRC[x>>3]&(0x80>>(x&7)))?0xffffffff:0x000000ff;
}
static void png_pxwr_y1(void *dst,int x,uint32_t src) {
  uint8_t r=src>>24,g=src>>16,b=src>>8;
  if (r+g+b>=0x180) DST[x>>3]|=(0x80>>(x&7));
  else DST[x>>3]&=~(0x80>>(x&7));
}

static uint32_t png_pxrd_y2(const void *src,int x) {
  uint8_t y=(SRC[x>>2]>>(6-((x&3)<<1)))&3;
  y|=y<<2;
  y|=y<<4;
  return (y<<24)|(y<<16)|(y<<8)|0xff;
}
static void png_pxwr_y2(void *dst,int x,uint32_t src) {
  uint8_t r=src>>24,g=src>>16,b=src>>8;
  uint8_t y=(r+g+b)/3;
  y>>=6;
  dst=DST+(x>>2);
  int shift=(6-((x&3)<<1));
  uint8_t mask=~(3<<shift);
  *DST=((*DST)&mask)|(y<<shift);
}

static uint32_t png_pxrd_y4(const void *src,int x) {
  uint8_t y=(x&1)?(SRC[x>>1]&0xf):(SRC[x>>1]>>4);
  y|=y<<4;
  return (y<<24)|(y<<16)|(y<<8)|0xff;
}
static void png_pxwr_y4(void *dst,int x,uint32_t src) {
  uint8_t r=src>>24,g=src>>16,b=src>>8;
  uint8_t y=(r+g+b)/3;
  dst=DST+(x>>1);
  if (x&1) *DST=((*DST)&0xf0)|(y>>4);
  else *DST=((*DST)&0x0f)|(y&0xf0);
}

static uint32_t png_pxrd_y8(const void *src,int x) {
  uint8_t y=SRC[x];
  return (y<<24)|(y<<16)|(y<<8)|0xff;
}
static void png_pxwr_y8(void *dst,int x,uint32_t src) {
  uint8_t r=src>>24,g=src>>16,b=src>>8;
  uint8_t y=(r+g+b)/3;
  DST[x]=y;
}

static uint32_t png_pxrd_y16(const void *src,int x) {
  src=SRC+(x<<1);
  uint8_t y=SRC[0];
  return (y<<24)|(y<<16)|(y<<8)|0xff;
}
static void png_pxwr_y16(void *dst,int x,uint32_t src) {
  uint8_t r=src>>24,g=src>>16,b=src>>8;
  uint8_t y=(r+g+b)/3;
  dst=DST+(x<<1);
  DST[0]=y;
  DST[1]=y;
}

static uint32_t png_pxrd_ya8(const void *src,int x) {
  src=SRC+(x<<1);
  uint8_t y=SRC[0],a=SRC[1];
  return (y<<24)|(y<<16)|(y<<8)|a;
}
static void png_pxwr_ya8(void *dst,int x,uint32_t src) {
  uint8_t r=src>>24,g=src>>16,b=src>>8,a=src;
  uint8_t y=(r+g+b)/3;
  dst=DST+(x<<1);
  DST[0]=y;
  DST[1]=a;
}

static uint32_t png_pxrd_ya16(const void *src,int x) {
  src=SRC+(x<<2);
  uint8_t y=SRC[0];
  uint8_t a=SRC[2];
  return (y<<24)|(y<<16)|(y<<8)|a;
}
static void png_pxwr_ya16(void *dst,int x,uint32_t src) {
  uint8_t r=src>>24,g=src>>16,b=src>>8,a=src;
  uint8_t y=(r+g+b)/3;
  dst=DST+(x<<2);
  DST[0]=y;
  DST[1]=y;
  DST[2]=a;
  DST[3]=a;
}

static uint32_t png_pxrd_rgb8(const void *src,int x) {
  src=SRC+(x*3);
  return (SRC[0]<<24)|(SRC[1]<<16)|(SRC[2]<<8)|0xff;
}
static void png_pxwr_rgb8(void *dst,int x,uint32_t src) {
  dst=DST+(x*3);
  DST[0]=src>>24;
  DST[1]=src>>16;
  DST[2]=src>>8;
}

static uint32_t png_pxrd_rgb16(const void *src,int x) {
  src=SRC+(x*6);
  return (SRC[0]<<24)|(SRC[2]<<16)|(SRC[4]<<8)|0xff;
}
static void png_pxwr_rgb16(void *dst,int x,uint32_t src) {
  dst=DST+(x*6);
  DST[0]=DST[1]=src>>24;
  DST[2]=DST[3]=src>>16;
  DST[4]=DST[5]=src>>8;
}

static uint32_t png_pxrd_rgba8(const void *src,int x) {
  src=SRC+(x<<2);
  return (SRC[0]<<24)|(SRC[1]<<16)|(SRC[2]<<8)|SRC[3];
}
static void png_pxwr_rgba8(void *dst,int x,uint32_t src) {
  dst=DST+(x<<2);
  DST[0]=src>>24;
  DST[1]=src>>16;
  DST[2]=src>>8;
  DST[3]=src;
}

static uint32_t png_pxrd_rgba16(const void *src,int x) {
  src=SRC+(x<<3);
  return (SRC[0]<<24)|(SRC[2]<<16)|(SRC[4]<<8)|SRC[6];
}
static void png_pxwr_rgba16(void *dst,int x,uint32_t src) {
  dst=DST+(x<<3);
  DST[0]=DST[1]=src>>24;
  DST[2]=DST[3]=src>>16;
  DST[4]=DST[5]=src>>8;
  DST[6]=DST[7]=src;
}

#undef SRC
#undef DST

/* Pixel access functions TOC.
 */
 
png_pxrd_fn png_get_pxrd(uint8_t depth,uint8_t colortype) {
  switch (colortype) {
    case PNG_COLORTYPE_GRAY:
    case PNG_COLORTYPE_INDEX: switch (depth) {
        case 1: return png_pxrd_y1;
        case 2: return png_pxrd_y2;
        case 4: return png_pxrd_y4;
        case 8: return png_pxrd_y8;
        case 16: return png_pxrd_y16;
      } break;
    case PNG_COLORTYPE_RGB: switch (depth) {
        case 8: return png_pxrd_rgb8;
        case 16: return png_pxrd_rgb16;
      } break;
    case PNG_COLORTYPE_GRAYA: switch (depth) {
        case 8: return png_pxrd_ya8;
        case 16: return png_pxrd_ya16;
      } break;
    case PNG_COLORTYPE_RGBA: switch (depth) {
        case 8: return png_pxrd_rgba8;
        case 16: return png_pxrd_rgba16;
      } break;
  }
  return 0;
}

png_pxwr_fn png_get_pxwr(uint8_t depth,uint8_t colortype) {
  switch (colortype) {
    case PNG_COLORTYPE_GRAY:
    case PNG_COLORTYPE_INDEX: switch (depth) {
        case 1: return png_pxwr_y1;
        case 2: return png_pxwr_y2;
        case 4: return png_pxwr_y4;
        case 8: return png_pxwr_y8;
        case 16: return png_pxwr_y16;
      } break;
    case PNG_COLORTYPE_RGB: switch (depth) {
        case 8: return png_pxwr_rgb8;
        case 16: return png_pxwr_rgb16;
      } break;
    case PNG_COLORTYPE_GRAYA: switch (depth) {
        case 8: return png_pxwr_ya8;
        case 16: return png_pxwr_ya16;
      } break;
    case PNG_COLORTYPE_RGBA: switch (depth) {
        case 8: return png_pxwr_rgba8;
        case 16: return png_pxwr_rgba16;
      } break;
  }
  return 0;
}
