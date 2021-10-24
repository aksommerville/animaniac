#include "animaniac.h"

/* If we only got path to the PNG file, make up a config path.
 */
 
static char an_cfgpath_storage[1024]={0};
 
static int an_config_synthesize_cfgpath(struct an_config *config,const char *src) {
  int srcc=0,dotp=-1;
  while (src[srcc]) {
    if (src[srcc]=='/') dotp=-1;
    else if (src[srcc]=='.') dotp=srcc;
    srcc++;
  }
  int dstc=snprintf(an_cfgpath_storage,sizeof(an_cfgpath_storage),"%.*s.cfg",(dotp>=0)?dotp:srcc,src);
  if ((dstc<1)||(dstc>=sizeof(an_cfgpath_storage))) return -1;
  config->cfgpath=an_cfgpath_storage;
  return 0;
}

/* Options.
 */
 
static void an_print_help(const char *exename) {
  fprintf(stderr,"\nUsage: %s [OPTIONS] PNGFILE\n\n",exename);
  fprintf(stderr,
    "OPTIONS:\n"
    "  --help            Print this message and exit.\n"
    "  --config=PATH     Use this config file instead of guessing.\n"
    "\n"
  );
}

static int an_config_short_option(struct an_config *config,char option) {
  fprintf(stderr,"%s: Unknown short option '%c'.\n",config->exename,option);
  return -1;
}

static int an_config_long_option(struct an_config *config,const char *k,int kc,const char *v) {
  if (!k) kc=0; else if (kc<0) { kc=0; while (k[kc]) kc++; }
  int vc=0; if (v) while (v[vc]) vc++;
  
  if ((kc==4)&&!memcmp(k,"help",4)) {
    an_print_help(config->exename);
    return -1;
  }
  
  if ((kc==6)&&!memcmp(k,"config",6)) {
    if (config->cfgpath) {
      fprintf(stderr,"%s: Multiple config paths '%s' and '%s'.\n",config->exename,config->cfgpath,v);
      return -1;
    }
    config->cfgpath=v;
    return 0;
  }
  
  fprintf(stderr,"%s: Unknown long option '%.*s' = '%.*s'.\n",config->exename,kc,k,vc,v);
  return -1;
}

/* Read argv, env, etc.
 */
 
int an_config_init(struct an_config *config,int argc,char **argv) {
  memset(config,0,sizeof(struct an_config));
  
  config->rate=60;//TODO configurable
  
  if (argc>=1) config->exename=argv[0];
  else config->exename="animaniac";
  
  int argp=1;
  while (argp<argc) {
    const char *arg=argv[argp++];
    
    // Empty or null? Invalid.
    if (!arg||!arg[0]) goto _invalid_;
    
    // No dash? pngpath
    if (arg[0]!='-') {
      if (config->pngpath) {
        fprintf(stderr,"%s: Multiple input files not supported.\n",config->exename);
        return -1;
      }
      config->pngpath=arg;
      continue;
    }
    
    // Single dash alone? Reserved.
    if (!arg[1]) goto _invalid_;
    
    // Single dash? Short options.
    if (arg[1]!='-') {
      arg++;
      for (;*arg;arg++) {
        if (an_config_short_option(config,*arg)<0) return -1;
      }
      continue;
    }
    
    // Double dash alone? Reserved.
    if (!arg[2]) goto _invalid_;
    
    // Long option.
    const char *k=arg+2;
    while (k[0]=='-') k++;
    const char *v=0;
    int kc=0;
    while (k[kc]&&(k[kc]!='=')) kc++;
    if (k[kc]=='=') v=k+kc+1;
    else if ((argp<argc)&&(argv[argp][0]!='-')) v=argv[argp++];
    else v="1";
    if (an_config_long_option(config,k,kc,v)<0) return -1;
    continue;
    
   _invalid_:;
    fprintf(stderr,"%s: Unexpected argument '%s'.\n",config->exename,arg);
    return -1;
  }
  
  // PNG path required.
  if (!config->pngpath) {
    //fprintf(stderr,"%s: Input PNG file required.\n",config->exename);
    an_print_help(config->exename);
    return -1;
  }
  
  if (!config->cfgpath) {
    if (an_config_synthesize_cfgpath(config,config->pngpath)<0) {
      fprintf(stderr,"%s: Failed to guess config path for '%s'.\n",config->exename,config->pngpath);
      return -1;
    }
  }
  
  return 0;
}
