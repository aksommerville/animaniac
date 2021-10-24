#ifndef ANIMANIAC_H
#define ANIMANIAC_H

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>

/* App configuration.
 ***********************************************************/
 
struct an_config {
  const char *exename;
  const char *pngpath;
  const char *cfgpath;
  int rate;
};

// Logs errors.
int an_config_init(struct an_config *config,int argc,char **argv);

/* The main business: Reads the PNG and config file and tracks animation state.
 ************************************************************/
 
#define AN_ANCHOR_NW  1
#define AN_ANCHOR_N   2
#define AN_ANCHOR_NE  3
#define AN_ANCHOR_W   4
#define AN_ANCHOR_CTR 5
#define AN_ANCHOR_E   6
#define AN_ANCHOR_SW  7
#define AN_ANCHOR_S   8
#define AN_ANCHOR_SE  9
 
struct an_animator;

void an_animator_del(struct an_animator *animator);

struct an_animator *an_animator_new();

/* Replace file content.
 * We make an effort to do this on the fly, without interrupting playback.
 */
int an_animator_set_image(struct an_animator *animator,const void *src,int srcc,const char *path);
int an_animator_set_config(struct an_animator *animator,const char *src,int srcc,const char *path);

/* faceid are 0..c-1.
 * Each face has a name, and you can borrow it.
 */
int an_animator_count_faces(const struct an_animator *animator);
int an_animator_get_face_name(char **name,const struct an_animator *animator,int faceid);
int an_animator_use_face(struct an_animator *animator,int faceid);
int an_animator_use_face_by_name(struct an_animator *animator,const char *name,int namec);

/* Call at 60 Hz.
 * Returns >0 if the image changed, 0 if no change, or <0 for unlikely errors.
 */
int an_animator_update(struct an_animator *animator);

/* Borrow a pointer to the current image.
 * an_animator_set_image() may invalidate this pointer.
 * It's formatted to plug right in to an_wm_set_image().
 */
int an_animator_get_image(
  void *rgbapp,int *w,int *h,int *stride,
  struct an_animator *animator
);

/* (rate,anchor) expect a cut token and return the result or <0.
 * (int) consumes leading and trailing space and returns length consumed, or <0 if no int present.
 */
int an_eval_rate(const char *src,int srcc); // => frames
int an_eval_int(int *dst,const char *src,int srcc);
int an_eval_anchor(const char *src,int srcc);

/* Timing.
 ***********************************************************/
 
struct an_clock;

void an_clock_del(struct an_clock *clock);

struct an_clock *an_clock_new(int ratehz);

int an_clock_update(struct an_clock *clock);

/* Filesystem.
 * Copied this all from my 'bits' collection... we only actually use an_file_read().
 ************************************************************/
 
int an_file_read(void *dstpp,const char *path);
int an_file_write(const char *path,const void *src,int srcc);
int an_dir_read(
  const char *path,
  int (*cb)(const char *path,const char *base,char type,void *userdata),
  void *userdata
);
char an_file_get_type(const char *path);

/* Inotify/stdin.
 ***********************************************************/
 
struct an_inmgr;

void an_inmgr_del(struct an_inmgr *inmgr);

/* Callbacks will only fire during an_inmgr_update().
 * cb_file() when a file added via an_inmgr_add_file() changes.
 * Files will always be reported via update after you add them. (not necessarily the very next update).
 * cb_stdin() when input received via stdin. If empty, stdin closed.
 * If you provide a stdin callback, we also listen for SIGINT and report it the same as stdin closure.
 */
struct an_inmgr *an_inmgr_new(
  int (*cb_file)(const char *path,void *userdata),
  int (*cb_stdin)(const void *src,int srcc,void *userdata),
  void *userdata
);

int an_inmgr_add_file(struct an_inmgr *inmgr,const char *path);

int an_inmgr_update(struct an_inmgr *inmgr);

/* Window manager.
 *************************************************************/
 
struct an_wm;

void an_wm_del(struct an_wm *wm);

struct an_wm *an_wm_new(
  int (*cb_close)(void *userdata),
  void *userdata
);

int an_wm_update(struct an_wm *wm);

/* Replace the currently displayed content.
 * We don't borrow the pointer or anything, once this returns we're done with it.
 */
int an_wm_set_image(
  struct an_wm *wm,
  const void *rgba,
  int w,int h,int stride
);

/* PNG decoder and image type.
 ************************************************************/

#define PNG_COLORTYPE_GRAY    0x00 /* 1 channel at (1,2,4,8,16) bits */
#define PNG_COLORTYPE_RGB     0x02 /* 3 channels at (8,16) bits */
#define PNG_COLORTYPE_INDEX   0x03 /* 1 channel at (1,2,4,8) bits */
#define PNG_COLORTYPE_GRAYA   0x04 /* 2 channels at (8,16) bits */
#define PNG_COLORTYPE_RGBA    0x06 /* 4 channels at (8,16) bits */

// case label does not reduce to an integer constant :(
//#define PNG_ID(str) ((str[0]<<24)|(str[1]<<16)|(str[2]<<8)|str[3])
#define PNG_ID(a,b,c,d) ((a<<24)|(b<<16)|(c<<8)|d)

struct png_decoder;

struct png_image {
  int refc; // 0=immortal

  void *pixels;
  int stride; // bytes
  int pixelsize; // bits, full pixel ie 1..64
  
  // IHDR:
  int32_t w,h;
  uint8_t depth,colortype;
  
  // All chunks except IHDR,IDAT,IEND:
  struct png_chunk {
    uint32_t id; // big-endian
    void *v;
    int c;
  } *chunkv;
  int chunkc,chunka;
};

/* Make a static image by initializing to zero (in particular, (refc) must be zero).
 * You can safely (del) static images same as dynamic ones.
 * Cleanup zeroes the struct, so you won't accidentally do something else with it later.
 * Ref fails for static images.
 */
void png_image_cleanup(struct png_image *image);
void png_image_del(struct png_image *image);
int png_image_ref(struct png_image *image);
struct png_image *png_image_new();

/* Overwrite (dst) with a copy of (src) possibly converting format.
 * Extra chunks will not be copied. In particular, if you ask for INDEX you will *not* get a PLTE.
 */
int png_image_convert(
  struct png_image *dst,
  uint8_t depth,uint8_t colortype,
  const struct png_image *src
);

/* Free existing pixels and replace: pixels,stride,pixelsize,w,h,depth,colortype
 */
int png_image_allocate_pixels(
  struct png_image *image,
  int32_t w,int32_t h,
  uint8_t depth,uint8_t colortype
);

int png_image_add_chunk_handoff(struct png_image *image,uint32_t id,void *v,int c);
int png_image_add_chunk_copy(struct png_image *image,uint32_t id,const void *v,int c);

// Return WEAK the first chunk matching (id).
int png_image_get_chunk_by_id(void *dstpp,const struct png_image *image,uint32_t id);

// Full pixel size in bits (1..64), or zero if invalid.
int png_pixelsize_for_format(uint8_t depth,uint8_t colortype);

/* Helpers to read or write one pixel using normalized 32-bit RGBA.
 * Accessors exist for all valid formats.
 * INDEX behaves like GRAY (NB It does normalize values).
 * 16-bit channels work, but there can be some data loss.
 */
typedef uint32_t (*png_pxrd_fn)(const void *src,int x);
typedef void (*png_pxwr_fn)(void *dst,int x,uint32_t src);
png_pxrd_fn png_get_pxrd(uint8_t depth,uint8_t colortype);
png_pxwr_fn png_get_pxwr(uint8_t depth,uint8_t colortype);

// Convenience so you don't have to deal with a decoder, if you've got the full serial data.
struct png_image *png_decode(const void *src,int srcc);
 
void png_decoder_del(struct png_decoder *decoder);
struct png_decoder *png_decoder_new();

/* Give some input to a decoder.
 * You can give it the whole file at once, or one byte at a time, or anything in between.
 * At each call to this function, we advance the decode process as far as possible.
 * This will always fail if we have ERROR status.
 */
int png_decoder_provide_input(struct png_decoder *decoder,const void *src,int srcc);

/* Image is accessible after we've processed the IHDR chunk.
 * Its pixels are incomplete until we process the last IDAT.
 * Returns a WEAK but retainable image.
 */
struct png_image *png_decoder_get_image(const struct png_decoder *decoder);

#define PNG_DECODER_ERROR      -1 /* Failed, no more progress possible. */
#define PNG_DECODER_IHDR        0 /* Initialized, awaiting IHDR. */
#define PNG_DECODER_IDAT        1 /* Awaiting or in the middle of IDATs. */
#define PNG_DECODER_IEND        2 /* Pixels complete, file not yet terminated. */
#define PNG_DECODER_COMPLETE    3 /* EOF */
int png_decoder_get_status(const struct png_decoder *decoder);
const char *png_decoder_get_error_message(const struct png_decoder *decoder);

#endif
