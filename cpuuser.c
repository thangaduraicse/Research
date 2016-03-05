#include <stdio.h>
#include <unistd.h>
#include <getopt.h>

struct {
	unsigned long usage;		/* %age cpu to use */
	unsigned long window;		/* Window of time in us over which CPU is used */
	unsigned long mhz;	/* CPU speed in Mhz */
} options;

const char *PROGNAME = NULL;

/* Read Timestamp Counter - Taken from include/asm-i386/msr.h */
#define rdtsc(high,low) \
     __asm__ __volatile__("rdtsc" : "=a" (low), "=d" (high))

void default_options()
{
	options.usage = 10;
	options.window = 1000000; /* 1 second */
	options.mhz = 0;
}

/* Time elapsed in terms of CPU timestamp counter increments */
unsigned long long time_elapsed(unsigned ohigh, unsigned olow)
{
	unsigned nhigh, nlow;
	unsigned hi, lo, borrow;
	rdtsc(nhigh, nlow);
	lo = nlow - olow;
	borrow = lo > nlow;
	hi = nhigh - ohigh - borrow;
	/* Yes, the (1 << 30) * 4 is intentional
	 * (1 << 32) or (1 << 31) * 2 will NOT do */
	return (unsigned long long)hi * (1 << 30) * 4 + lo;
}

void usage()
{
	printf("Usage: %s [options]\n", PROGNAME);
	printf("Where options are:\n");
	printf("  --use, -u USAGE       CPU usage of process should be USAGE\%\n"
               "  --window, -w WINDOW   Process will use CPU and sleep and repeat\n"
	       "                        this over a time period of WINDOWus\n"
	       "  --mhz, -m MHZ         CPU speed in MHz, 0 for autodetect\n"
	       "  --help, -h            This usage message\n");
}

void process_args(int argc, char **argv)
{
	static struct option long_options[] = {
		{"use",    1, 0, 'u'},
		{"window", 1, 0, 'w'},
		{"mhz",    1, 0, 'm'},
		{"help",   1, 0, 'h'},
		{ 0,       0, 0,  0 }
	};
	int c;
	int index = 0;
	while ((c = getopt_long(argc, argv, "u:w:m:h", long_options, &index)) != -1) {
		switch(c) {
		case 'w':
			options.window = strtoul(optarg, NULL, 10);
			break;
		case 'u':
			options.usage = strtoul(optarg, NULL, 10);
			break;
		case 'm':
			options.mhz = strtoul(optarg, NULL, 10);
			break;
		case 'h':
			usage();
			exit(0);
		case '?':
			printf("Unknown option encountered\n");
			break;
		default:
			printf("?? getopt returned character code 0%o ??\n", c);
		}
		index = 0;
	}
}

void print_options()
{
	printf("PID         = %d\n"
	       "CPU speed   = %luMHz\n"
	       "CPU usage   = %lu\%\n"
	       "time window = %luus\n", 
	       getpid(),
	       options.mhz,
	       options.usage, 
	       options.window);
}

void calculate_mhz()
{
	unsigned int low, high;
	unsigned long MHZ;
	int sleep_time = 10; /* seconds */

	printf("Calculating CPU speed in MHz, will take %d seconds\n", sleep_time);
	rdtsc(high, low);
	sleep(sleep_time);
	options.mhz = (unsigned long)(time_elapsed(high, low) / (sleep_time * 1e6));
}

void use_cpu()
{
	unsigned long long use = (unsigned long long)options.usage * options.window * options.mhz / 100; /* # of counter increments to use CPU */
	unsigned long usleep_time = (100 - options.usage) * options.window / 100;
	unsigned int low, high;
	rdtsc(high, low);
	printf("Beginning infinite loop: Work for %llu counter increments, sleep for %luus\n", use, usleep_time);	
	while (1) {
		if (usleep_time && time_elapsed(high, low) >= use) {
			if (usleep(usleep_time)) {
				perror("usleep():");
				break;
			}
			rdtsc(high, low);
		}
	}
}

int main(int argc, char **argv)
{
	PROGNAME = argv[0];
	default_options();
	process_args(argc, argv);
	if (!options.mhz) 
		calculate_mhz();
	if (options.usage > 100) {
		printf("ERROR! Usage cannot be greater than 100%\n");
		exit(-1);
	}
	print_options();
	use_cpu();
}