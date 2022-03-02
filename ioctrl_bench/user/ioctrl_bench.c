#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

#include "../ioctrl_bench.h"


struct timespec diff(struct timespec start, struct timespec end){
	struct timespec temp;
	if ((end.tv_nsec-start.tv_nsec)<0) {
		temp.tv_sec = end.tv_sec-start.tv_sec-1;
		temp.tv_nsec = 1000000000+end.tv_nsec-start.tv_nsec;
	} else {
		temp.tv_sec = end.tv_sec-start.tv_sec;
		temp.tv_nsec = end.tv_nsec-start.tv_nsec;
	}
	return temp;
}

double timespec_to_double(struct timespec time){
    return (double) time.tv_nsec/1000000000LU + time.tv_sec;
}


int main(int argc, char **argv) {
    int fd, arg_int = 0, ret;
    struct timespec start, end;
    unsigned long count = 0;

    fd = open("/sys/kernel/debug/ioctl_bench/f", O_RDONLY);
    if (fd == -1) {
        printf("open error");
        return EXIT_FAILURE;
    }

    clock_gettime(CLOCK_MONOTONIC, &start);
    for(unsigned long i=0; i<5000LU; i++){ /* reduced from 500000000LU */
        ret = ioctl(fd, LKMC_IOCTL_INC, &arg_int);
        count++;

        if (ret == -1) {
            printf("ioctl error");
            return EXIT_FAILURE;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double time_taken = timespec_to_double( diff(start, end) );
    double throughput = (double)count/time_taken;

    printf("%f s, %f /s\n", time_taken, throughput);

    close(fd);
    return EXIT_SUCCESS;
}
