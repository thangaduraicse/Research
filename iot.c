/* Simple multi-threaded I/O benchmark program.
 *
 * To compile:
 *   gcc -o iot iot.c -lpthread
 * To run:
 *   Either with disk device or with pre-existing file.
 *    ./iot [options] filename
 *   Filename is the file or device to test on.
 *   By default it uses 8Kb I/O blocks and does sequential read test
 *   until interrupted.
 *   To indicate when to stop:
 *     -t sec - run for this many seconds, say, 30, to eliminate random
 *       noise.
 *     -i num - perform this many I/O operations
 *   To indicate R/W mode:
 *     -wn, -Wn, -rn, -Rn --
 *       perform linear or random write (note: all data will be lost!),
 *       or linear or random read, using given number of threads (n).
 *   I/O modes:
 *    -s - syncronous write (O_SYNC)
 *    -d - direct I/O (O_DIRECT)
 *    -b bs - block size in bytes
 *   And finally:
 *    -h - display usage.
 * Example:
 *   ./iot -t30 -W4 -R4 -d -b8192 /dev/sdb
 * perform random read/write test (4 readers and 4 writers)
 * for 30 seconds using direct I/O and block size of 8Kb.
 *
 * Note: for small blocksize (<64Kb at least) and using direct random I/O,
 * nowadays drives sometimes gives transfer rates below 1Mb/sec - this is
 * expectable, don't be afraid of so low numbers.  The reason is simple:
 * in order to access a given block of data, a disk drive has to seek to the
 * right track (average seek time) and wait for the right sector to be near
 * the head (rotation latency).  Sum up the two, and divide 1 sec to the
 * result -- you'll have max number of requests/sec a drive can perform,
 * not counting the actual data transfer (which reduces this number further).
 * With, say, 5ms seek time + rotation latency, we'll have 200 requests/sec,
 * which, with 4Kb request size, will be about 800Kb/sec - which is below
 * 1Mb/sec, not counting the actual transfer...
 *
 */

#define _GNU_SOURCE
#define _BSD_SOURCE
#define _LARGEFILE_SOURCE
#define _FILE_OFFSET_BITS 64

//#define USE_DEV_URANDOM	/* was not a good idea */

#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <pthread.h>
#include <string.h>

#ifndef BLKGETSIZE64
#define BLKGETSIZE64 _IOR(0x12,114,size_t)
 /* linux-specific. return device size in bytes (u64 *arg) */
#endif

static void edie(const char *what) {
  fprintf(stderr, "%s: %m \n", what);
  exit(1);
}

#ifdef USE_DEV_URANDOM
static int randfd;
#endif
static int oflags;		// open flags
static char *fn;		// filename
static unsigned bs = 8192;	// block size
static unsigned bc;		// block count (device size in blocks)
static unsigned bm;		// blocks to do

#define MFrnd   1
#define MFwrt   2
#define LinRd	0
#define RndRd	MFrnd
#define LinWr	MFwrt
#define RndWr	(MFrnd|MFwrt)

struct state {
  int fd;
  char *buf;
  unsigned ioc;		// I/O count
  int (*workfn)(struct state *, unsigned blocknr);
  unsigned (*posfn)(struct state *s);
  unsigned opi;		// operation index
  unsigned i;		// curidx
  double stime;		// start time
  unsigned bn;		// current block number for linear i/o
};

static unsigned tioc;	// total i/o count
static struct state *states;
static unsigned nt[4];
static unsigned ntt;
static volatile unsigned running;
static const char *const ion[4] = { "LinRd", "RndRd", "LinWr", "RndWr" };

static pthread_mutex_t rnmtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rncond = PTHREAD_COND_INITIALIZER;

static double curtime(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + tv.tv_usec / 1000000.0;
}

static unsigned randpos(struct state *s) {
  unsigned n;
#ifdef USE_DEV_URANDOM
  read(randfd, &n, sizeof(n));
#else
  n = lrand48();
#endif
  s = s;
  return n % bc;
}

static unsigned linpos(struct state *s) {
  if (s->bn >= bc)
    s->bn = 0;
  return s->bn++;
}

static int wwriter(struct state *s, unsigned b) {
  return pwrite(s->fd, s->buf, bs, (off_t)b * bs);
}
static int wreader(struct state *s, unsigned b) {
  return pread(s->fd, s->buf, bs, (off_t)b * bs);
}

static void pst(FILE *f) {
  double ct = curtime();
  double r[4] = { 0, 0, 0, 0 };
  unsigned c[4] = { 0, 0, 0, 0 };
  unsigned i;
  double d;
  for(i = 0; i < ntt; ++i) {
    d = ct - states[i].stime;
    r[states[i].opi] += states[i].ioc / d;
    c[states[i].opi] += states[i].ioc;
  }
//#if 1
//  for(i = 0; i < 4; ++i)
//    if (c[i])
//      fprintf(f, " %s %u %.2f", ion[i], c[i], r[i] * bs / 1024 / 1024);
//#endif
}

static void incc() {
  if (!(++tioc % 1000))
    pthread_cond_signal(&rncond);
}
static void decnr() {
  pthread_mutex_lock(&rnmtx);
  --running;
  pthread_mutex_unlock(&rnmtx);
  pthread_cond_broadcast(&rncond);
}

static volatile int term;

void *worker(void *arg) {
  struct state *s = arg;
  s->workfn = s->opi & MFwrt ? wwriter : wreader;
  s->posfn  = s->opi & MFrnd ? randpos : linpos;
  s->fd = open(fn, (s->opi & MFwrt ? O_WRONLY : O_RDONLY) | oflags);
  if (s->fd < 0) {
    int e = errno;
    decnr();
    errno = e;
    edie(fn);
  }
  s->stime = curtime();
  for(;;) {
    if (term) break;
    if (s->workfn(s, s->posfn(s)) < 0) {
      perror(ion[s->opi]);
      break;
    }
    ++s->ioc;
    incc();
    if (bm && s->ioc >= bm) break;
  }
  decnr();
  return 0;
}

static void sig(int s) {
  term = s;
}

int main(int argc, char **argv) {
  int c;
  unsigned i, j;
  unsigned tm = 0;
  struct state *s;
  char *buf;

  while((c = getopt(argc, argv, "r::R::w::W::dsb:n:i:t:h")) != EOF) switch(c) {
  case 'r': nt[LinRd] = optarg ? atoi(optarg) : 1; break;
  case 'R': nt[RndRd] = optarg ? atoi(optarg) : 1; break;
  case 'w': nt[LinWr] = optarg ? atoi(optarg) : 1; break;
  case 'W': nt[RndWr] = optarg ? atoi(optarg) : 1; break;
  case 'd': oflags |= O_DIRECT; break;
  case 's': oflags |= O_SYNC; break;
  case 'b': bs = atoi(optarg); break;
  case 'n': bc = atoi(optarg); break;
  case 'i': bm = atoi(optarg); break;
  case 't': tm = atoi(optarg); break;
  case 'h':
    puts(
"iotest: perform I/O speed test\n"
"Usage is: iotest [options] device-or-file\n"
"options:\n"
" -r[n] - linear read test (n readers)\n"
" -R[n] - random read test (n readers)\n"
" -w[n] - linear write test (n writers)\n"
" -W[n] - random write test (n writers)\n"
" -d - use direct I/O (O_DIRECT)\n"
" -s - use syncronous I/O (O_SYNC)\n"
" -b bs - blocksize (default is 8192)\n"
" -n bc - block count (default is whole device/file)\n"
" -i nb - number of I/O iterations to perform\n"
" -t sec - time to spend on all I/O\n"
" -h - this help\n"
"It's ok to specify all, one or some of -r,-R,-w and -W\n"
);
    return 0;
  default: fprintf(stderr, "try `iotest -h' for help\n"); exit(1);
  }

  if (optind + 1 != argc) {
    fprintf(stderr, "exactly one device/file argument expected\n");
    return 1;
  }
  fn = argv[optind];

  ntt = nt[0] + nt[1] + nt[2] + nt[3];
  if (!ntt)
    nt[LinRd] = ntt = 1;

  c = open(fn, (nt[LinWr] + nt[RndWr] ? O_RDWR : O_RDONLY) | oflags);
  if (c < 0) edie(fn);
  if (!bc) {
    unsigned long long sz;
    struct stat st;
    fstat(c, &st);
    if (st.st_size) sz = st.st_size;
    else ioctl(c, BLKGETSIZE64, &sz);
    bc = sz / bs;
//    fprintf(stderr, "size = %lld (%u blocks)\n", sz, bc);
  }
  close(c);
  if (nt[RndRd] || nt[RndWr]) {
#ifdef USE_DEV_URANDOM
    randfd = open("/dev/urandom", O_RDONLY);
    if (randfd < 0) edie("/dev/urandom");
#else
#if 0
    struct timeval tv;
    gettimeofday(&tv, NULL);
    srand48(tv.tv_usec ^ getpid());
#else
    srand48(0xfeda432);	// arbitrary, to get repeated values on repeated runs
#endif
#endif
  }

  states = calloc(ntt, sizeof(*states));
  s = states;
  buf = valloc(ntt * bs);
  if (tm) {
    signal(SIGALRM, sig);
    alarm(tm);
  }
  running = ntt;
  for(j = 0; j < 4; ++j)
    for(i = 0; i < nt[j]; ++i) {
      pthread_t t;
      s->buf = buf; buf += bs;
      s->opi = j;
      s->i = i;
      pthread_create(&t, NULL, worker, s++);
    }
  while(running) {
    pthread_cond_wait(&rncond, &rnmtx);
    putc('\r', stderr);
    pst(stderr);
  }

  putc('\r', stderr);
  pst(stdout);
  putc('\n', stdout);

  return 0;
}
