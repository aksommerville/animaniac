#include "animaniac.h"

/* Context.
 */
 
struct an_app {
  struct an_config config;
  struct an_clock *clock;
  struct an_inmgr *inmgr;
  struct an_wm *wm;
  struct an_animator *animator;
  int quit;
};

static void an_app_cleanup(struct an_app *app) {
  an_clock_del(app->clock);
  an_inmgr_del(app->inmgr);
  an_wm_del(app->wm);
  an_animator_del(app->animator);
}

/* File changed.
 */
 
static int an_read_image(struct an_app *app,const char *path) {
  void *src=0;
  int srcc=an_file_read(&src,path);
  if (srcc<0) {
    fprintf(stderr,"%s: Failed to read image file.\n",path);
    return -1;
  }
  int err=an_animator_set_image(app->animator,src,srcc,path);
  free(src);
  if (err<0) {
    fprintf(stderr,"%s: Failed to decode or apply image file.\n",path);
    return -1;
  }
  return 0;
}
 
static int an_read_config(struct an_app *app,const char *path) {
  void *src=0;
  int srcc=an_file_read(&src,path);
  if (srcc<0) {
    fprintf(stderr,"%s: Failed to read config file.\n",path);
    return -1;
  }
  int err=an_animator_set_config(app->animator,src,srcc,path);
  free(src);
  if (err<0) {
    fprintf(stderr,"%s: Failed to decode or apply config file.\n",path);
    return -1;
  }
  return 0;
}
 
static int cb_file(const char *path,void *userdata) {
  struct an_app *app=userdata;
  if (!strcmp(path,app->config.pngpath)) return an_read_image(app,path);
  if (!strcmp(path,app->config.cfgpath)) return an_read_config(app,path);
  return 0;
}

/* Receive content via stdin.
 */
 
static int cb_stdin(const void *src,int srcc,void *userdata) {
  struct an_app *app=userdata;
  if (srcc<=0) {
    app->quit=1;
    return 0;
  }
  //TODO I originally imagined accepting config adjustments via stdin. Want to do that?
  return 0;
}

/* Window closed.
 */
 
static int cb_close(void *userdata) {
  struct an_app *app=userdata;
  app->quit=1;
  return 0;
}

/* Main.
 */
 
int main(int argc,char **argv) {
  struct an_app app={0};

  if (an_config_init(&app.config,argc,argv)<0) return 1;
  
  if (
    !(app.inmgr=an_inmgr_new(cb_file,cb_stdin,&app))||
    (an_inmgr_add_file(app.inmgr,app.config.pngpath)<0)||
    (an_inmgr_add_file(app.inmgr,app.config.cfgpath)<0)
  ) {
    fprintf(stderr,"%s: Failed to initialize input.\n",app.config.exename);
    an_app_cleanup(&app);
    return 1;
  }
  
  if (
    !(app.wm=an_wm_new(cb_close,&app))
  ) {
    fprintf(stderr,"%s: Failed to initialize window manager.\n",app.config.exename);
    an_app_cleanup(&app);
    return 1;
  }
  
  if (!(app.animator=an_animator_new())) {
    an_app_cleanup(&app);
    return 1;
  }
  
  if (!(app.clock=an_clock_new(app.config.rate))) {
    fprintf(stderr,"%s: Failed to create clock for rate %d Hz.\n",app.config.exename,app.config.rate);
    an_app_cleanup(&app);
    return 1;
  }
  
  while (!app.quit) {
    an_clock_update(app.clock);
    
    int err=an_animator_update(app.animator);
    if (err<0) {
      fprintf(stderr,"%s: Internal error updating animation.\n",app.config.exename);
      an_app_cleanup(&app);
      return 1;
    }
    if (err>0) {
      const void *rgba=0;
      int w=0,h=0,stride=0;
      if (
        (an_animator_get_image(&rgba,&w,&h,&stride,app.animator)<0)||
        (an_wm_set_image(app.wm,rgba,w,h,stride)<0)
      ) {
        fprintf(stderr,"%s: Failed to retrieve or apply image.\n",app.config.exename);
        an_app_cleanup(&app);
        return 1;
      }
    }
    
    if (an_inmgr_update(app.inmgr)<0) {
      fprintf(stderr,"%s: Failed to update input.\n",app.config.exename);
      an_app_cleanup(&app);
      return 1;
    }
    if (an_wm_update(app.wm)<0) {
      fprintf(stderr,"%s: Failed to update window manager.\n",app.config.exename);
      an_app_cleanup(&app);
      return 1;
    }
  }
  
  an_app_cleanup(&app);
  return 0;
}
