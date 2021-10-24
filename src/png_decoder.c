#include "animaniac.h"
#include <stdarg.h>
#include <zlib.h>

/* Decoder object.
 */
 
#define PNG_PSTATUS_SIGNATURE 0
#define PNG_PSTATUS_HEADER    1
#define PNG_PSTATUS_BODY      2
#define PNG_PSTATUS_IDAT      3
#define PNG_PSTATUS_CRC       4
#define PNG_PSTATUS_VERIFY    5
 
struct png_decoder {

  struct png_image *image;
  int status;
  char *message;
  
  // Private status.
  int pstatus;
  int have_IHDR;
  int have_IEND;
  
  // Intake buffer. Long enough to hold signature, chunk header, or CRC.
  uint8_t inv[8];
  int inc;
  
  // Incoming chunk. Reallocated when we process the header, and should be moved somewhere after.
  // All chunks except IDAT use this.
  uint32_t chunkid;
  uint8_t *chunkv;
  int chunkc; // amount actually read
  int chunka; // expected total length
  int idatexpect; // counts down while receiving IDAT body
  
  // Image decode state.
  uint8_t *rowbuf;
  int rowbufc; // includes filter byte
  int y;
  int xstride; // bytes pixel-to-pixel for filter purposes
  z_stream *z;
};

void png_decoder_del(struct png_decoder *decoder) {
  if (!decoder) return;
  png_image_del(decoder->image);
  if (decoder->message) free(decoder->message);
  if (decoder->chunkv) free(decoder->chunkv);
  if (decoder->rowbuf) free(decoder->rowbuf);
  if (decoder->z) {
    inflateEnd(decoder->z);
    free(decoder->z);
  }
  free(decoder);
}

struct png_decoder *png_decoder_new() {
  struct png_decoder *decoder=calloc(1,sizeof(struct png_decoder));
  if (!decoder) return 0;
  
  decoder->pstatus=PNG_PSTATUS_SIGNATURE;
  
  return decoder;
}

/* Accessors.
 */
 
struct png_image *png_decoder_get_image(const struct png_decoder *decoder) {
  return decoder->image;
}

int png_decoder_get_status(const struct png_decoder *decoder) {
  return decoder->status;
}

const char *png_decoder_get_error_message(const struct png_decoder *decoder) {
  return decoder->message;
}

/* Set error message and status.
 * Always returns -1.
 */
 
static int png_fail(struct png_decoder *decoder,const char *fmt,...) {
  if (fmt&&fmt[0]) {
    int na=256;
    char *nv=malloc(na);
    if (nv) {
      va_list vargs;
      va_start(vargs,fmt);
      int nc=vsnprintf(nv,na,fmt,vargs);
      if ((nc>0)&&(nc<na)) {
        if (decoder->message) free(decoder->message);
        decoder->message=nv;
      } else {
        free(nv);
      }
    }
  }
  decoder->status=PNG_DECODER_ERROR;
  return -1;
}

/* Free chunkv if present, and zero its associated content.
 */
 
static void png_decoder_clear_chunkv(struct png_decoder *decoder) {
  if (decoder->chunkv) free(decoder->chunkv);
  decoder->chunkv=0;
  decoder->chunkc=0;
  decoder->chunka=0;
  decoder->chunkid=0;
}

/* Create the image object if we haven't yet.
 */
 
static int png_decoder_require_image(struct png_decoder *decoder) {
  if (decoder->image) return 0;
  if (!(decoder->image=png_image_new())) return -1;
  return 0;
}

/* Unfilter one row, no context.
 */
 
static inline uint8_t png_paeth(uint8_t a,uint8_t b,uint8_t c) {
  int p=a+b-c;
  int pa=a-p; if (pa<0) pa=-pa;
  int pb=b-p; if (pb<0) pb=-pb;
  int pc=c-p; if (pc<0) pc=-pc;
  if ((pa<=pb)&&(pa<=pc)) return a;
  if (pb<=pc) return b;
  return c;
}
 
static int png_unfilter_row(
  uint8_t *dst,
  const uint8_t *src,
  const uint8_t *pv, // OPTIONAL
  uint8_t filter,
  int len,
  int xstride
) {
  switch (filter) {
    case 0: {
        memcpy(dst,src,len);
      } return 0;
    case 1: {
        memcpy(dst,src,xstride);
        int i=xstride; for (;i<len;i++) {
          dst[i]=src[i]+dst[i-xstride];
        }
      } return 0;
    case 2: if (pv) {
        int i=len; for (;i-->0;dst++,src++,pv++) {
          *dst=(*src)+(*pv);
        }
      } else {
        memcpy(dst,src,len);
      } return 0;
    case 3: if (pv) {
        int i=0;
        for (;i<xstride;i++) dst[i]=src[i]+(pv[i]>>1);
        for (;i<len;i++) dst[i]=src[i]+((pv[i]+dst[i-xstride])>>1);
      } else {
        int i=0;
        for (;i<xstride;i++) dst[i]=src[i];
        for (;i<len;i++) dst[i]=src[i]+(dst[i-xstride]>>1);
      } return 0;
    case 4: if (pv) {
        int i=0;
        for (;i<xstride;i++) dst[i]=src[i]+pv[i];
        for (;i<len;i++) dst[i]=src[i]+png_paeth(dst[i-xstride],pv[i],pv[i-xstride]);
      } else {
        int i=0;
        for (;i<xstride;i++) dst[i]=src[i];
        for (;i<len;i++) dst[i]=src[i]+dst[i-xstride];
      } return 0;
  }
  return -1;
}

/* Unfilter (decoder->rowbuf) and add it to the image.
 */
 
static int png_receive_filtered_row(struct png_decoder *decoder) {
  if (decoder->y<decoder->image->h) {
    const uint8_t *src=decoder->rowbuf+1;
    uint8_t *dst=((uint8_t*)decoder->image->pixels)+decoder->y*decoder->image->stride;
    uint8_t *pv=0;
    if (decoder->y) pv=dst-decoder->image->stride;
    if (png_unfilter_row(dst,src,pv,decoder->rowbuf[0],decoder->image->stride,decoder->xstride)<0) {
      return png_fail(decoder,"Unexpected filter byte 0x%02x at row %d",decoder->rowbuf[0],decoder->y);
    }
    decoder->y++;
  }
  return 0;
}

/* Decode IDAT chunk.
 */
 
static int png_decode_IDAT(struct png_decoder *decoder,const uint8_t *src,int srcc) {
  
  if (!decoder->have_IHDR) return png_fail(decoder,"IDAT before IHDR");
  
  decoder->z->next_in=(Bytef*)src;
  decoder->z->avail_in=srcc;
  while (decoder->z->avail_in>0) {
  
    if (!decoder->z->avail_out) {
      if (png_receive_filtered_row(decoder)<0) return -1;
      decoder->z->next_out=(Bytef*)decoder->rowbuf;
      decoder->z->avail_out=decoder->rowbufc;
    }
    
    int err=inflate(decoder->z,Z_NO_FLUSH);
    if (err<0) return png_fail(decoder,"inflate: error %d",err);
  }
  
  return 0;
}

/* Finish decoding: Drain the decompressor and confirm we have everything expected.
 */
 
static int png_decode_finish(struct png_decoder *decoder) {
  if (!decoder->have_IHDR) return png_fail(decoder,"No IHDR");
  if (!decoder->z->total_in) return png_fail(decoder,"Missing or empty IDAT");
  
  while (decoder->y<decoder->image->h) {

    if (!decoder->z->avail_out) {
      if (png_receive_filtered_row(decoder)<0) return -1;
      if (decoder->y>=decoder->image->h) break;
      decoder->z->next_out=(Bytef*)decoder->rowbuf;
      decoder->z->avail_out=decoder->rowbufc;
    }
    
    int err=inflate(decoder->z,Z_FINISH);
    if (err<0) return png_fail(decoder,"inflate: error %d",err);
  }
  
  decoder->status=PNG_DECODER_COMPLETE;
  return 0;
}

/* Decode IHDR chunk.
 */
 
static int png_decode_IHDR(struct png_decoder *decoder,const uint8_t *src,int srcc) {
  
  if (decoder->have_IHDR) return png_fail(decoder,"Multiple IHDR.");
  if (srcc<13) return png_fail(decoder,"Short IHDR (%d<13).",srcc);
  
  int w=(src[0]<<24)|(src[1]<<16)|(src[2]<<8)|src[3];
  int h=(src[4]<<24)|(src[5]<<16)|(src[6]<<8)|src[7];
  if ((w<1)||(h<1)) return png_fail(decoder,"Invalid dimensions %dx%d.",w,h);
  
  if (src[10]||src[11]||src[12]) {
    return png_fail(decoder,
      "Unsupported compression/filter/interlace: %d/%d/%d",
      src[10],src[11],src[12]
    );
  }
  
  if (png_decoder_require_image(decoder)<0) return -1;
  if (png_image_allocate_pixels(decoder->image,w,h,src[8],src[9])<0) return -1;
  
  if (decoder->rowbuf||decoder->z) return -1;
  
  decoder->rowbufc=1+decoder->image->stride;
  if (!(decoder->rowbuf=malloc(decoder->rowbufc))) return -1;
  decoder->xstride=decoder->image->pixelsize>>3;
  if (!decoder->xstride) decoder->xstride=1;
  
  if (!(decoder->z=calloc(1,sizeof(z_stream)))) return -1;
  if (inflateInit(decoder->z)<0) return -1;
  
  decoder->y=0;
  decoder->z->next_out=(Bytef*)decoder->rowbuf;
  decoder->z->avail_out=decoder->rowbufc;
  
  decoder->have_IHDR=1;
  return 0;
}

/* Receive chunk header.
 * Input is always 8 bytes.
 * Changes (pstatus).
 */
 
static int png_decode_chunk_begin(struct png_decoder *decoder,const uint8_t *src) {
  int len=(src[0]<<24)|(src[1]<<16)|(src[2]<<8)|src[3];
  uint32_t chunkid=(src[4]<<24)|(src[5]<<16)|(src[6]<<8)|src[7];
  if (len<0) return png_fail(decoder,"Improbable chunk length 0x%08x",len);
  if (!len) {
    decoder->pstatus=PNG_PSTATUS_CRC;
    if (chunkid==PNG_ID('I','E','N','D')) {
      decoder->have_IEND=1;
    }
    if ((chunkid!=PNG_ID('I','D','A','T'))&&(chunkid!=PNG_ID('I','H','D','R'))&&(chunkid!=PNG_ID('I','E','N','D'))) {
      if (png_decoder_require_image(decoder)<0) return -1;
      if (png_image_add_chunk_copy(decoder->image,chunkid,0,0)<0) return -1;
    }
  } else if (chunkid==PNG_ID('I','D','A','T')) {
    decoder->pstatus=PNG_PSTATUS_IDAT;
    decoder->idatexpect=len;
  } else {
    decoder->pstatus=PNG_PSTATUS_BODY;
    if (!(decoder->chunkv=malloc(len?len:1))) return -1;
    decoder->chunka=len;
    decoder->chunkid=chunkid;
  }
  return 0;
}

/* Receive input, internal.
 * This does not need to consume the entire input but must consume at least one byte.
 */
 
static int png_decode_partial(struct png_decoder *decoder,const uint8_t *src,int srcc) {
  switch (decoder->pstatus) {
  
    case PNG_PSTATUS_SIGNATURE: {
        if (decoder->inc) {
          int cpc=8-decoder->inc;
          if (cpc>srcc) cpc=srcc;
          memcpy(decoder->inv+decoder->inc,src,cpc);
          decoder->inc+=cpc;
          if (decoder->inc>=8) {
            if (memcmp(decoder->inv,"\x89PNG\r\n\x1a\n",8)) {
              return png_fail(decoder,"Signature mismatch.");
            }
            decoder->inc=0;
            decoder->pstatus=PNG_PSTATUS_HEADER;
          }
          return cpc;
        } else if (srcc>=8) {
          if (memcmp(src,"\x89PNG\r\n\x1a\n",8)) {
            return png_fail(decoder,"Signature mismatch.");
          }
          decoder->pstatus=PNG_PSTATUS_HEADER;
          return 8;
        } else {
          memcpy(decoder->inv,src,srcc);
          decoder->inc=srcc;
          return srcc;
        }
      } break;
      
    case PNG_PSTATUS_HEADER: {
        if (decoder->inc) {
          int cpc=8-decoder->inc;
          if (cpc>srcc) cpc=srcc;
          memcpy(decoder->inv+decoder->inc,src,cpc);
          decoder->inc+=cpc;
          if (decoder->inc>=8) {
            if (png_decode_chunk_begin(decoder,decoder->inv)<0) return -1;
            decoder->inc=0;
          }
          return cpc;
        } else if (srcc>=8) {
          if (png_decode_chunk_begin(decoder,src)<0) return -1;
          return 8;
        } else {
          memcpy(decoder->inv,src,srcc);
          decoder->inc=srcc;
          return srcc;
        }
      } break;
    
    case PNG_PSTATUS_CRC: {
        if (decoder->inc) {
          int cpc=4-decoder->inc;
          if (cpc>srcc) cpc=srcc;
          memcpy(decoder->inv+decoder->inc,src,cpc);
          decoder->inc+=cpc;
          if (decoder->inc>=4) {
            // We don't actually do anything with the CRC.
            if (decoder->have_IEND) {
              decoder->status=PNG_DECODER_COMPLETE;
              decoder->pstatus=PNG_PSTATUS_VERIFY;
            } else {
              decoder->pstatus=PNG_PSTATUS_HEADER;
            }
            decoder->pstatus=PNG_PSTATUS_VERIFY;
            decoder->inc=0;
          }
          return cpc;
        } else if (srcc>=4) {
          if (decoder->have_IEND) {
            decoder->status=PNG_DECODER_COMPLETE;
            decoder->pstatus=PNG_PSTATUS_VERIFY;
          } else {
            decoder->pstatus=PNG_PSTATUS_HEADER;
          }
          return 4;
        } else {
          memcpy(decoder->inv,src,srcc);
          decoder->inc=srcc;
          return srcc;
        }
      } break;
      
    case PNG_PSTATUS_BODY: {
        int cpc=decoder->chunka-decoder->chunkc;
        if (cpc>srcc) cpc=srcc;
        memcpy(decoder->chunkv+decoder->chunkc,src,cpc);
        decoder->chunkc+=cpc;
        if (decoder->chunkc>=decoder->chunka) {
          switch (decoder->chunkid) {
            case PNG_ID('I','H','D','R'): {
                if (png_decode_IHDR(decoder,decoder->chunkv,decoder->chunkc)<0) return -1;
                png_decoder_clear_chunkv(decoder);
              } break;
            case PNG_ID('I','E','N','D'): {
                // We're not actually "complete" yet; we still need IEND's CRC.
                decoder->have_IEND=1;
                png_decoder_clear_chunkv(decoder);
              } break;
            default: {
                if (png_decoder_require_image(decoder)<0) return -1;
                if (png_image_add_chunk_handoff(decoder->image,decoder->chunkid,decoder->chunkv,decoder->chunkc)<0) return -1;
                decoder->chunkv=0;
                png_decoder_clear_chunkv(decoder);
              }
          }
          decoder->pstatus=PNG_PSTATUS_CRC;
        }
        return cpc;
      }
      
    case PNG_PSTATUS_IDAT: {
        int cpc=decoder->idatexpect;
        if (cpc>srcc) cpc=srcc;
        if (png_decode_IDAT(decoder,src,cpc)<0) return -1;
        decoder->idatexpect-=cpc;
        if (decoder->idatexpect<=0) {
          decoder->pstatus=PNG_PSTATUS_CRC;
        }
        return cpc;
      }
  }
  return -1;
}

/* Receive input.
 */

int png_decoder_provide_input(struct png_decoder *decoder,const void *src,int srcc) {
  if (!decoder) return -1;
  while (srcc>0) {
    if (decoder->status==PNG_DECODER_ERROR) return -1;
    if (decoder->status==PNG_DECODER_COMPLETE) return 0;
    int err=png_decode_partial(decoder,src,srcc);
    if (err<=0) return png_fail(decoder,0);
    if (decoder->pstatus==PNG_PSTATUS_VERIFY) {
      if (png_decode_finish(decoder)<0) return -1;
      return 0;
    }
    src=(char*)src+err;
    srcc-=err;
  }
  return 0;
}

/* One-shot decode.
 */
 
struct png_image *png_decode(const void *src,int srcc) {
  struct png_decoder *decoder=png_decoder_new();
  if (!decoder) return 0;
  if (png_decoder_provide_input(decoder,src,srcc)<0) {
    png_decoder_del(decoder);
    return 0;
  }
  struct png_image *image=png_decoder_get_image(decoder);
  int err=png_image_ref(image);
  png_decoder_del(decoder);
  if (err<0) return 0;
  return image;
}
