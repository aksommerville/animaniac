#include "animaniac.h"
#include <sys/time.h>
#include <unistd.h>

// Add so many microseconds to each sleep, to improve the odds of actually reaching the next frame.
#define AN_CLOCK_SLEEP_EXTRA 100

/* Object definition.
 */
 
struct an_clock {
  int rate; // hz
  int period; // us
  int framec;
  int skipc;
  int64_t starttime;
  int64_t nexttime;
};

/* Current absolute time in microseconds.
 */
 
static int64_t an_now() {
  struct timeval tv={0};
  gettimeofday(&tv,0);
  return (int64_t)tv.tv_sec*1000000ll+tv.tv_usec;
}

/* Delete.
 */
 
void an_clock_del(struct an_clock *clock) {
  if (!clock) return;
  free(clock);
}

/* New.
 */

struct an_clock *an_clock_new(int ratehz) {
  if ((ratehz<1)||(ratehz>1000)) return 0;
  struct an_clock *clock=calloc(1,sizeof(struct an_clock));
  if (!clock) return 0;
  
  clock->rate=ratehz;
  clock->period=1000000/ratehz;
  
  clock->starttime=an_now();
  clock->nexttime=clock->starttime;
  
  return clock;
}

/* Update.
 */

int an_clock_update(struct an_clock *clock) {
  clock->framec++;
  while (1) {
    int64_t now=an_now();
    if (now>=clock->nexttime) {
      clock->nexttime+=clock->period;
      if (clock->nexttime<now) {
        clock->skipc++;
        clock->nexttime=now+clock->period;
      }
      return 0;
    }
    if (now<clock->nexttime-2000000) {
      // We've fallen behind by more than 2 seconds. Something is broken. Reset.
      clock->skipc++;
      clock->nexttime=now+clock->period;
      return 0;
    }
    usleep(clock->nexttime-now+AN_CLOCK_SLEEP_EXTRA);
  }
  return 0;
}
