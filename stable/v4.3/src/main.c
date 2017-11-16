#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <mqueue.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>

#include "config.h"
#include "server.h"
#include "database.h"
#include "siteconf.h"

#define __DBG__

typedef struct client_info {
	char num[32];
	char ip[32];
	char port[32];
	char mqfile[64];
} client_info_t;

client_info_t client[MAXSERVER + 1];

pid_t pid[MAXSERVER + 1];
mqd_t mq[MAXSERVER + 1];

server_t *p_server = NULL;
siteconf_t *p_siteconf = NULL;

static int servernum;
static long int msgsize;
static volatile int flag = -1;

static mode_t mode = 0777;
static int oflag = O_RDWR | O_CREAT | O_NONBLOCK;

static char *gather_watchdog_mqfile = "/mq_watchdog";
static char *client_watchdog_mqfile[MAXSERVER + 1] = {"/mq_watchdog0", "/mq_watchdog1", "/mq_watchdog2", \
				"/mq_watchdog3", "/mq_watchdog4", "/mq_watchdog5", "/mq_watchdog6"};

void *watchdog_routine(void *arg)
{
	int i;
	int r;
	char buf[32];

	while (1) {
		sleep(WATCHDOG_TIME);
		for (i = 0; i < (servernum + 1); i++) {
			memset(buf, 0, sizeof(buf));
			r = mq_receive(mq[i], buf, msgsize, NULL);
			if (r == -1) {
				fprintf(stderr, "children < %d > died!!!!!!\n\n", i);
				sleep(5);
				flag = i;
				continue;
			} else
				printf("read buf = %s, from %d\n", buf, i);
		}
	}
}

int main(int argc, char *argv[])
{
	int i, r;
	char gather_pid[32];
	struct mq_attr attr;
	server_info_t *pserver;
	
	db_open(FILENAME);
	
	get_server_settings();
	if (p_server) {
		pserver = p_server->info;
		servernum = MIN_V(p_server->num, MAXSERVER);
	}

	for (i = 1; i < (servernum + 1); i++, pserver++) {
		snprintf(client[i].num,    32, "%d", i);
		snprintf(client[i].ip,     32, "%s", pserver->ip);
		snprintf(client[i].port,   32, "%d", pserver->port);
		snprintf(client[i].mqfile, 64, "%s", client_watchdog_mqfile[i]);
	}

	/* for gather process */
	mq[0] = mq_open(gather_watchdog_mqfile, oflag, mode, NULL);
	if (mq[0] == ((mqd_t) -1)) {
		fprintf(stderr, "create gather process mqueue failed: %s\n", strerror(errno));
		return 1;
	}

	/* for client process */	
	for (i = 1; i < (servernum + 1); i++) {
		mq[i] = mq_open(client_watchdog_mqfile[i], oflag, mode, NULL);
		if (mq[i] == ((mqd_t) -1)) {
			fprintf(stderr, "create client[%d] process watchdog mqueue failed: %s\n", i, strerror(errno));
			return 2;
		}
	}

	mq_getattr(mq[0], &attr);
	msgsize = attr.mq_msgsize;

	pid[0] = fork();
	if (pid[0] == -1) {
		fprintf(stderr, "fork gather process failed: %s\n", strerror(errno));
		return 3;
	} 
	
	snprintf(gather_pid, 32, "%d", pid[0]);

	if (pid[0] == 0) {
		execl("./gather", gather_watchdog_mqfile, NULL);
		exit(0);
	}

	/* parent process runs later */
	sleep(1);
	
	db_close();
	put_server_settings();

	for (i = 1; i < (servernum + 1); i++) {
		pid[i] = fork();
		if (pid[i] == -1) {
			fprintf(stderr, "fork [%d] children process failed: %s\n", i, strerror(errno));
			continue;	
		}

		if (pid[i] == 0) {
			execl("./client", client[i].num, client[i].ip, client[i].port, client[i].mqfile, gather_pid, NULL);
			exit(0);
		}

		/* parent process do to loop next */
	}	

	pthread_t thr_watchdog;
	r = pthread_create(&thr_watchdog, NULL, watchdog_routine, NULL);
	if (r != 0) {
		fprintf(stderr, "create watchdog_routine failed: %s\n", strerror(errno));
		return 4;
	}
	pthread_detach(thr_watchdog);

	while (1) {
		if (flag >= 0) {
			if (flag == 0) {
				/* if gather process died, can not update gather_pid argument, so must reboot system */
				fprintf(stderr, "gather process dead!!!!\n");
				system("reboot");			
			} else {

				pid[flag] = fork();
				if (pid[flag] == -1) {
					fprintf(stderr, "fork client [%d] process failed: %s\n", flag, strerror(errno));
					continue;	
				}

				if (pid[flag] == 0) {
					execl("./client", client[flag].num, client[flag].ip, client[flag].port, client[flag].mqfile, gather_pid, NULL);
					exit(0);
				}

				flag = -1;
				continue;
			}			
		}
	}

	return 0;
}

