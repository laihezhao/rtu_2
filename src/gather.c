#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <mqueue.h>
#include <sys/types.h>
#include <pthread.h>
#include <modbus/modbus.h>

#include "log.h"
#include "error.h"
#include "config.h"
#include "packet.h"
#include "pols.h"
#include "local.h"
#include "modbus.h"
#include "server.h"
#include "sitesbs.h"
#include "devices.h"
#include "sbparam.h"
#include "database.h"
#include "siteconf.h"
#include "sitepols.h"
#include "polsalarm.h"
#include "ctlparam.h"
#include "ntpclient.h"

#define ERROR_TIMES		(100)
#define RECV_ALARM_TIME		(15)
#define SEND_ALARM_TIME 	(15)

#define CMD_SET_ANALOG_VALUE  	"3022"
#define CMD_SET_LOCALTIME	"1012"
#define CMD_GET_MINUTEDATA	"2051"
#define CMD_GET_HOURDATA	"2061"
#define CMD_GET_DAYDATA		"2031"

#define RESPOND_ALARM_OK        "2072"
#define RESPOND_DAY_STATUS_DATA "2041"

static char check_time[32];
static int check_time_flag = 0;

static int modbus_retry_times = 0;

static struct sigaction act;
static struct sigaction act_rtmin1;
static struct sigaction act_rtmin2;

static int N_VALUE_SEC;
static int N_VALUE_MIN;
static int N_VALUE_HOUR;
static int N_VALUE_DAY;

static timer_t timerid;
static timer_t timerid_sec;
static timer_t timerid_min;
static timer_t timerid_hour;
static timer_t timerid_day;

static struct itimerspec it;
static struct itimerspec it_sec;
static struct itimerspec it_min;
static struct itimerspec it_hour;
static struct itimerspec it_day;

static pthread_mutex_t mutex;
static pthread_mutex_t mutex_db;
static pthread_mutex_t mutex_ctx;

static pthread_t thr_timer;
static pthread_t thr_receiver;
static pthread_t thr_recvalarm;
static pthread_t thr_sendalarm;

static int servernum;
static int devicesnum;

modbus_t *ctx[MAXDEVS] = {NULL};

static struct mq_attr cli_attr[MAXSERVER + 1];
static struct mq_attr rev_attr[MAXSERVER + 1];

static mqd_t mq[MAXSERVER + 1];
static mqd_t mqtest[MAXSERVER + 1];
static int gather_stop[MAXSERVER + 1] = {0};
static char *client_mqfile[MAXSERVER + 1] = {"/mq_data0", "/mq_data1", "/mq_data2", "/mq_data3", \
						"/mq_data4", "/mq_data5", "/mq_data6"};

static char *mqfile[MAXSERVER + 1] = {"/mqtest0", "/mqtest1", "/mqtest2", "/mqtest3", \
					"/mqtest4", "/mqtest5", "/mqtest6"};
pols_t *p_pols           = NULL;
server_t *p_server       = NULL;
sitesbs_t *p_sitesbs     = NULL;
devices_t *p_devices     = NULL;
sbparam_t *p_sbparam     = NULL;
siteconf_t *p_siteconf   = NULL;
sitepols_t *p_sitepols   = NULL;
polsalarm_t *p_polsalarm = NULL;
ctlparam_t *p_ctlparam   = NULL;

typedef void (*sigfunc_t)(int, siginfo_t *, void *);

static int is_in(char *str)
{
	int i;
	int len;
	char *p;

	p = str;
	len = strlen(str);

	for (i = 0; i < len; i++) {
		if (p[i] >= 'a' && p[i] <= 'z')
			return -1;
	}

	return 0;
}

static void sig_rtmin1_handler(int sig, siginfo_t *info, void *ucontext)
{
	log_printf(2, __FILE__, __FUNCTION__, __LINE__, "Catch SIGRTMIN+1 signal.");
	gather_stop[info->si_value.sival_int] = 1;
}

static void sig_rtmin2_handler(int sig, siginfo_t *info, void *ucontext)
{
	log_printf(2, __FILE__, __FUNCTION__, __LINE__, "Catch SIGRTMIN+2 signal.");
	gather_stop[info->si_value.sival_int] = 0;
}

static void set_block_signal(void)
{
	sigfillset(&act.sa_mask);
	sigprocmask(SIG_BLOCK, &act.sa_mask, NULL);
}

static sigfunc_t set_sigrtmin1_handler(void)
{
	struct sigaction oact;

	act_rtmin1.sa_sigaction = sig_rtmin1_handler;
	sigemptyset(&act_rtmin1.sa_mask);
	sigaddset(&act_rtmin1.sa_mask, SIGRTMIN+1);
	act_rtmin1.sa_flags = SA_SIGINFO;
	act_rtmin1.sa_flags |= SA_RESTART;
	
	if (sigaction(SIGRTMIN+1, &act_rtmin1, &oact) < 0)
		return NULL;

	return (oact.sa_sigaction);
}

static sigfunc_t set_sigrtmin2_handler(void)
{
	struct sigaction oact;

	act_rtmin2.sa_sigaction = sig_rtmin2_handler;
	sigemptyset(&act_rtmin2.sa_mask);
	sigaddset(&act_rtmin2.sa_mask, SIGRTMIN+2);
	act_rtmin2.sa_flags = SA_SIGINFO;
	act_rtmin2.sa_flags |= SA_RESTART;
	
	if (sigaction(SIGRTMIN+2, &act_rtmin2, &oact) < 0)
		return NULL;

	return (oact.sa_sigaction);
}

static void respond_alarm_handler(const char *qn, const int flag)
{
	int r;
	char sql[256];

	memset(sql, 0, sizeof(sql));	
	snprintf(sql, 256, "update alarm set centre%d='1' where alarmtime='%s';", flag, qn);

	pthread_mutex_lock(&mutex_db);
	r = db_exec(sql, NULL, 0);
	if (r != 0) {
		pthread_mutex_unlock(&mutex_db);
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "db_exec error.");
	}
	pthread_mutex_unlock(&mutex_db);
}

static void respond_minutedata_handler(char *buf, const int flag)
{
	int r;
	char qn[32];
	char sql[256];

	reverse_time(qn, buf + 4);
	
	memset(sql, 0, sizeof(sql));
	snprintf(sql, 256, "update minutedata set centre%d='0' where datatime='%s';", flag, qn);

	pthread_mutex_lock(&mutex_db);
	r = db_exec(sql, NULL, 0);
	if (r != 0) {
		pthread_mutex_unlock(&mutex_db);
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "db_exec error.");
	}
	pthread_mutex_unlock(&mutex_db);
}

static void respond_hourdata_handler(const char *buf, const int flag)
{
	int r;
	char qn[32];
	char sql[256];

	reverse_time(qn, buf + 4);
	
	memset(sql, 0, sizeof(sql));
	snprintf(sql, 256, "update houlyrdata set centre%d='0' where datatime='%s';", flag, qn);

	pthread_mutex_lock(&mutex_db);
	r = db_exec(sql, NULL, 0);
	if (r != 0) {
		pthread_mutex_unlock(&mutex_db);
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "db_exec error.");
	}
	pthread_mutex_unlock(&mutex_db);
}

static void respond_daydata_handler(const char *buf, const int flag)
{
	int r;
	char qn[32];
	char sql[256];

	reverse_time(qn, buf + 4);
	
	memset(sql, 0, sizeof(sql));
	snprintf(sql, 256, "update dailydata set centre%d='0' where datatime='%s';", flag, qn);

	pthread_mutex_lock(&mutex_db);
	r = db_exec(sql, NULL, 0);
	if (r != 0) {
		pthread_mutex_unlock(&mutex_db);
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "db_exec error.");
	}
	pthread_mutex_unlock(&mutex_db);
}

static int set_localtime(mqd_t mq, const char *rbuf, const char *data)
{
	int n;
	char buf[256];

	if ((!rbuf) || (!data))
		return -1;

	n = create_pkt_response(buf, data);
	mq_send(mq, buf, n, 0);

	memset(check_time, 0, 32);
	sscanf((rbuf + 2 + 4), "%*75sSystemTime=%14s", check_time);
	check_time_flag = 1;

	n = create_pkt_exec(buf, data);
	mq_send(mq, buf, n, 0);

	return 0;
}

static int set_analog_value(mqd_t mq, const char *pkt_r, const char *qn)
{
	int n;
	int r;
	int i;
	int j;
	char *token1;
	char *token2;
	char *token3;
	char *temp1;
	char *temp2;
	char *temp3;
	char *saveptr1;
	char *saveptr2;
	char *saveptr3;
	char sbid[128];
	char sb[100][24];
	char value[100][24];
	char buf[256] = {0};
	const char *split1 = ";";
	const char *split2 = "=";
	const char *split3 = "-";

	if ((!pkt_r) || (!qn))
		return -1;

	n = create_pkt_response(buf, qn);
   	mq_send(mq, buf, n, 0);

	n = create_pkt_exec(buf, qn);
	mq_send(mq, buf, n, 0);
	
	sscanf((pkt_r + 2 + 4), "%*75s%[^&]", sbid);
	
	for (i = 0, temp1 = sbid; ; i++, temp1 = NULL) {
		token1 = strtok_r(temp1, split1, &saveptr1);
		if (token1 == NULL)
			break;

		temp2 = token1;
		token2 = strtok_r(temp2, split2, &saveptr2);
		if (token2 == NULL)
			break;

		strcpy(&value[i][0], saveptr2);

		temp3 = token2;
		token3 = strtok_r(temp3, split3, &saveptr3);
		if (token3 == NULL)
			break;

		strcpy(&sb[i][0], token3);
	}

	ctlparam_info_t *pctlparam;
	pctlparam = p_ctlparam->info;

	devices_info_t *pdevices;
	pdevices = p_devices->info;

	sitesbs_info_t *psitesbs;
	psitesbs = p_sitesbs->info;

	int u, w;

	for (j = 0; j < i; j++) {
		int c = 0;
		psitesbs = p_sitesbs->info;
		for (u = 0; u < p_sitesbs->num; u++, psitesbs++) {
			if (!strcmp(psitesbs->sbid, &sb[j][0])) {
				c = 1;	
				break;
			}
		}

		if (!c) {
			log_printf(2, __FILE__, __FUNCTION__, __LINE__, "SBID error.");
			continue;
		}
		
		pctlparam = p_ctlparam->info;
		for (u = 0; u < p_ctlparam->num; u++, pctlparam++) {
			if ((!strcmp(pctlparam->sbid, &sb[j][0])) && (!strcasecmp(pctlparam->paramname, "SP"))) {
				
				pdevices = p_devices->info;
				for (w = 0; w < devicesnum; w++, pdevices++) {
					
					/* serial communication */
					if ((!strcmp(pdevices->devname, pctlparam->devname)) && (pdevices->devaddr == pctlparam->devaddr) \
							&& (pdevices->devtype == 1)) {

						if (strncmp(pctlparam->funcode, "06", 2)) {
							log_printf(2, __FILE__, __FUNCTION__, __LINE__, "check funcode.");
							continue;
						}
						
						if (pctlparam->datatype != 3) {
							log_printf(2, __FILE__, __FUNCTION__, __LINE__, "check datatype.");
							continue;
						}

						if (pctlparam->reglen != 2) {
							log_printf(2, __FILE__, __FUNCTION__, __LINE__, "check reglen.");
							continue;
						}

						float f;
						uint16_t dest[24];

						f = atof(&value[j][0]);
						modbus_get_uint16_sw(f, dest);

						pthread_mutex_lock(&mutex);
						r = modbus_write_registers_retry(ctx[w], pctlparam->regaddr-40000, pctlparam->reglen, dest);
						if (r == -1) {
							pthread_mutex_unlock(&mutex);
							log_printf(2, __FILE__, __FUNCTION__, __LINE__, "set SP parameter error.");
							continue;
						}
						pthread_mutex_unlock(&mutex);
					}
				}

				c = 1;
			} else
				c = 0;
		}	
		
		if (!c) {
			log_printf(2, __FILE__, __FUNCTION__, __LINE__, "SBID or PARAMNAME error.");
			continue;
		}	
	}

	n = create_pkt_exec(buf, qn);
	mq_send(mq, buf, n, 0);

	return 0;
}

static int get_minute_data(mqd_t mq, const char *pkt_r, const char *qn)
{
	int i;
	int j;
	int r;
	int n;
	char buf[256];
	char begt[32];
	char endt[32];

	if ((!pkt_r) || (!qn))
		return -1;

	n = create_pkt_response(buf, qn);
	mq_send(mq, buf, n, 0);

	sscanf((pkt_r + 2 + 4), "%*75sBeginTime=%14s,EndTime=%[^&]", begt, endt);
	
	char begt1[32];
	char endt1[32];
	
	snprintf(begt1, 32, "%s000", begt);
	snprintf(endt1, 32, "%s000", endt);

	char begintime[32];
	char endtime[32];

	reverse_time(begintime, begt1);
	reverse_time(endtime, endt1);

	int row;
	int col;
	char sql[256];
	char **result;
	char timedata[32][32];

	memset(sql, 0, 256);
	snprintf(sql, 256, "select distinct datatime from minutedata where datatime>=\"%s\" and datatime<=\"%s\";", begintime, endtime);

	r = db_get_table(sql, &result, &row, &col);
	if (r != 0) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "db_get_table error.");
		return -1;
	}

	if ((!row) || (!col)) {
		log_printf(2, __FILE__, __FUNCTION__, __LINE__, "no minutedata.");
		return -1;
	}
	
	for (i = 0; i < row; i++) {
		strncpy(timedata[i], result[i+1], 32);
	}
	
	db_free_table(result);

	for (i = 0; i < row; i++) {
		int arow;
		int acol;
		char asql[256];
		char **aresult;
	
		memset(asql, 0, 256);
		snprintf(asql, 256, "select polid, pol_min, pol_avg, pol_max, pol_col from minutedata where datatime=\"%s\";", timedata[i]);

		r = db_get_table(asql, &aresult, &arow, &acol);
		if (r != 0) {
			log_printf(3, __FILE__, __FUNCTION__, __LINE__, "db_get_table error.");
			continue;
			//return -1;
		}

		if ((!arow) || (!acol)) {
			log_printf(2, __FILE__, __FUNCTION__, __LINE__, "no minutedata.");
			continue;
			//return -1;
		}

		char str[1024];
		char *temp = str;
		
		memset(str, 0, 1024);
		for (j = 0; j < arow; j++) {
			n = snprintf(temp, 512, "%s-Min=%s,%s-Avg=%s,%s-Max=%s,%s-Cou=%s;", \
					*(aresult+acol+j*acol+0), *(aresult+acol+j*acol+1), \
					*(aresult+acol+j*acol+0), *(aresult+acol+j*acol+2),\
					*(aresult+acol+j*acol+0), *(aresult+acol+j*acol+3),\
					*(aresult+acol+j*acol+0), *(aresult+acol+j*acol+4));
			temp += n;
		}
	
		db_free_table(aresult);

		char qn[32];
		char ttime[32];
		char pkt[1024];
		char abuf[1024];
		char data[1024];

		memset(pkt, 0, 1024);
	        memset(abuf, 0, 1024);
		memset(data, 0, 1024);

		parse_time(ttime, timedata[i]);
		get_sys_time(qn, NULL);
		snprintf(pkt, 1024, "DataTime=%s000;%s", ttime, str);
		snprintf(abuf, 1024, "QN=%s;%s", qn, pkt);		
		
		n = create_pkt_minute_data(data, abuf);
		mq_send(mq, data, n, 0);

		usleep(500000);
	}

	n = create_pkt_exec(buf, qn);
	mq_send(mq, buf, n, 0);

	return 0;
}

static int get_hour_data(mqd_t mq, const char *pkt_r, const char *qn)
{
	int i;
	int j;
	int r;
	int n;
	char buf[256];
	char begt[32];
	char endt[32];

	if ((!pkt_r) || (!qn))
		return -1;

	n = create_pkt_response(buf, qn);
	mq_send(mq, buf, n, 0);

	sscanf((pkt_r + 2 + 4), "%*75sBeginTime=%14s,EndTime=%[^&]", begt, endt);
	
	char begt1[32];
	char endt1[32];
	
	snprintf(begt1, 32, "%s000", begt);
	snprintf(endt1, 32, "%s000", endt);

	char begintime[32];
	char endtime[32];

	reverse_time(begintime, begt1);
	reverse_time(endtime, endt1);

	int row;
	int col;
	char sql[256];
	char **result;
	char timedata[32][32];

	memset(sql, 0, 256);
	snprintf(sql, 256, "select distinct datatime from hourlydata where datatime>=\"%s\" and datatime<=\"%s\";", begintime, endtime);

	r = db_get_table(sql, &result, &row, &col);
	if (r != 0) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "db_get_table error.");
		return -1;
	}

	if ((!row) || (!col)) {
		log_printf(2, __FILE__, __FUNCTION__, __LINE__, "no hourlydata.");
		return -1;
	}
	
	for (i = 0; i < row; i++) {
		strncpy(timedata[i], result[i+1], 32);
	}
	
	db_free_table(result);

	for (i = 0; i < row; i++) {
		int arow;
		int acol;
		char asql[256];
		char **aresult;
	
		memset(asql, 0, 256);
		snprintf(asql, 256, "select polid, pol_min, pol_avg, pol_max, pol_col from hourlydata where datatime=\"%s\";", timedata[i]);

		r = db_get_table(asql, &aresult, &arow, &acol);
		if (r != 0) {
			log_printf(3, __FILE__, __FUNCTION__, __LINE__, "db_get_table error.");
			continue;
			//return -1;
		}

		if ((!arow) || (!acol))
			continue;
			//return -1;

		char str[1024];
		char *temp = str;
		
		memset(str, 0, 1024);
		for (j = 0; j < arow; j++) {
			n = snprintf(temp, 512, "%s-Min=%s,%s-Avg=%s,%s-Max=%s,%s-Cou=%s;", \
					*(aresult+acol+j*acol+0), *(aresult+acol+j*acol+1), \
					*(aresult+acol+j*acol+0), *(aresult+acol+j*acol+2),\
					*(aresult+acol+j*acol+0), *(aresult+acol+j*acol+3),\
					*(aresult+acol+j*acol+0), *(aresult+acol+j*acol+4));
			temp += n;
		}
	
		db_free_table(aresult);

		char qn[32];
		char ttime[32];
		char pkt[1024];
		char abuf[1024];
		char data[1024];

		memset(pkt, 0, 1024);
	        memset(abuf, 0, 1024);
		memset(data, 0, 1024);

		parse_time(ttime, timedata[i]);
		get_sys_time(qn, NULL);
		snprintf(pkt, 1024, "DataTime=%s000;%s", ttime, str);
		snprintf(abuf, 1024, "QN=%s;%s", qn, pkt);		
		
		n = create_pkt_hourly_data(data, abuf);
		mq_send(mq, data, n, 0);

		usleep(500000);
	}

	n = create_pkt_exec(buf, qn);
	mq_send(mq, buf, n, 0);

	return 0;
}

static int get_day_data(mqd_t mq, const char *pkt_r, const char *qn)
{
	int i;
	int j;
	int r;
	int n;
	char buf[256];
	char begt[32];
	char endt[32];

	if ((!pkt_r) || (!qn))
		return -1;

	n = create_pkt_response(buf, qn);
	mq_send(mq, buf, n, 0);

	sscanf((pkt_r + 2 + 4), "%*75sBeginTime=%14s,EndTime=%[^&]", begt, endt);
	
	char begt1[32];
	char endt1[32];
	
	snprintf(begt1, 32, "%s000", begt);
	snprintf(endt1, 32, "%s000", endt);

	char begintime[32];
	char endtime[32];

	reverse_time(begintime, begt1);
	reverse_time(endtime, endt1);

	int row;
	int col;
	char sql[256];
	char **result;
	char timedata[32][32];

	memset(sql, 0, 256);
	snprintf(sql, 256, "select distinct datatime from dailydata where datatime>=\"%s\" and datatime<=\"%s\";", begintime, endtime);

	r = db_get_table(sql, &result, &row, &col);
	if (r != 0) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "db_get_table error.");
		return -1;
	}

	if ((!row) || (!col)) {
		log_printf(2, __FILE__, __FUNCTION__, __LINE__, "no dailydata.");
		return -1;
	}
	
	for (i = 0; i < row; i++) {
		strncpy(timedata[i], result[i+1], 32);
	}
	
	db_free_table(result);

	for (i = 0; i < row; i++) {
		int arow;
		int acol;
		char asql[256];
		char **aresult;
	
		memset(asql, 0, 256);
		snprintf(asql, 256, "select polid, pol_min, pol_avg, pol_max, pol_col from dailydata where datatime=\"%s\";", timedata[i]);

		r = db_get_table(asql, &aresult, &arow, &acol);
		if (r != 0) {
			log_printf(3, __FILE__, __FUNCTION__, __LINE__, "db_get_table error.");
			continue;
			//return -1;
		}

		if ((!arow) || (!acol))
			continue;
			//return -1;

		char str[1024];
		char *temp = str;
		
		memset(str, 0, 1024);
		for (j = 0; j < arow; j++) {
			n = snprintf(temp, 512, "%s-Min=%s,%s-Avg=%s,%s-Max=%s,%s-Cou=%s;", \
					*(aresult+acol+j*acol+0), *(aresult+acol+j*acol+1), \
					*(aresult+acol+j*acol+0), *(aresult+acol+j*acol+2),\
					*(aresult+acol+j*acol+0), *(aresult+acol+j*acol+3),\
					*(aresult+acol+j*acol+0), *(aresult+acol+j*acol+4));
			temp += n;
		}
	
		db_free_table(aresult);

		char qn[32];
		char ttime[32];
		char pkt[1024];
		char abuf[1024];
		char data[1024];

		memset(pkt, 0, 1024);
	        memset(abuf, 0, 1024);
		memset(data, 0, 1024);

		parse_time(ttime, timedata[i]);
		get_sys_time(qn, NULL);
		snprintf(pkt, 1024, "DataTime=%s000;%s", ttime, str);
		snprintf(abuf, 1024, "QN=%s;%s", qn, pkt);		
		
		n = create_pkt_daily_data(data, abuf);
		mq_send(mq, data, n, 0);

		usleep(500000);
	}

	n = create_pkt_exec(buf, qn);
	mq_send(mq, buf, n, 0);

	return 0;
}

static int gather_sitepols(const char *datatime, const char *temptime)
{
	int i, j, r, n;
	char estr[1024];
	char fstr[1024];
	char jstr[1024];

	memset(estr, 0, sizeof(estr));
	memset(fstr, 0, sizeof(fstr));
	memset(jstr, 0, sizeof(jstr));

	if ((!p_devices) || (!p_sitepols))
		return -1;

	devices_info_t *pdevices;
	pdevices = p_devices->info;

	sitepols_info_t *psitepols;
	psitepols = p_sitepols->info;

	for (i = 0; i < devicesnum; i++, pdevices++) {
		if (!ctx[i])
			continue;
		
		char cstr[1024];
		memset(cstr, 0, sizeof(cstr));
		
		if (pdevices->devtype == 1) {
			/* serial communication */
			if (pdevices->comtype == 2) {
				/*485*/
				char *temp;
				char astr[256];
				
				temp = astr;
				memset(astr, 0, sizeof(astr));
				
				psitepols = p_sitepols->info;
				for (j = 0; j < p_sitepols->num; j++, psitepols++) {
					if ((!strcmp(psitepols->devname, pdevices->devname)) && (psitepols->devaddr == pdevices->devaddr)) {
						char str[32];
						char pkt[256];
	
						memset(str, 0, strlen(str));
						memset(pkt, 0, strlen(pkt));
						/* read coil */
						if (!strncmp(psitepols->funcode, "01", 2)) {
						
							continue;
						/* read input status */
						} else if (!strncmp(psitepols->funcode, "02", 2)) {
							continue;
						/* read holding regsiter */
						} else if (!strncmp(psitepols->funcode, "03", 2)) {
							uint16_t dest[24] = {0};
							if (psitepols->datatype == 2) {
#if 0
								if (psitepols->reglen != 1) {
									fprintf(stderr, "Please check reglen if it equals 1\n");
									continue;
								}

								r = modbus_read_registers_retry(ctx[i], psitepols->regaddr-40000, psitepols->reglen, dest);
								if (r != -1) {
									sprintf(str, "%.1f", dest[0] * psitepols->K + psitepols->B);
									/* save polvalue */
								//	psitepols->polvalue = dest[0]*psitepols->K + psitepols->B;
								} else
									continue;
#endif
							} else if (psitepols->datatype == 3) {
								if (psitepols->reglen != 2) {
									log_printf(2, __FILE__, __FUNCTION__, __LINE__, "check reglen.");
									continue;
								}
							
								pthread_mutex_lock(&mutex_ctx);	
								r = modbus_read_registers_retry(ctx[i], psitepols->regaddr-40000, psitepols->reglen, dest);
								if (r != -1) {
									sprintf(str, "%.3f", modbus_get_float_sw(dest + 0) * psitepols->K + psitepols->B);
									/*
									if (!strncmp(str, "inf", 3)) {
										pthread_mutex_unlock(&mutex_ctx);
										continue;
									}
									*/

									int ret = is_in(str);
									if (ret == -1) {
										pthread_mutex_unlock(&mutex_ctx);
										continue;
									}

									/* save polvalue */
									psitepols->polvalue = modbus_get_float_sw(dest + 0) * psitepols->K + psitepols->B;
								} else {
									modbus_retry_times++;
									pthread_mutex_unlock(&mutex_ctx);
									continue;
								}
								pthread_mutex_unlock(&mutex_ctx);

							} else if (psitepols->datatype == 4) {
#if 0
								if (psitepols->reglen != 1) {
									fprintf(stderr, "Please check reglen if it equals 1\n");
									continue;
								}
								
								if ((psitepols->databit < 0) || (psitepols->databit > 15)) {
									fprintf(stderr, "Bit parameter should be 0-15\n");
									continue;
								}
								
								r = modbus_read_registers_retry(ctx[i], psitepols->regaddr-40000, psitepols->reglen, dest);
								if (r != -1) {
									sprintf(str, "%f", (((uint16_t)1 << psitepols->databit) & dest[0]));
									/* save polvalue */
								//	psitepols->polvalue = dest[0]*psitepols->K + psitepols->B;
								} else
									continue;
#endif
							} else {
								log_printf(2, __FILE__, __FUNCTION__, __LINE__, "check datatype.");
								continue;
							}
						
						/* read input regsiter */
						} else if (!strncmp(psitepols->funcode, "04", 2)) {

							continue;
						} else {
							log_printf(2, __FILE__, __FUNCTION__, __LINE__, "check funcode.");
							continue;
						}
#if 1
						snprintf(pkt, 256, "insert into rtdata (datatime, polid, polname, pol_rtd, flag) "
								"values('%.19s','%s','%s','%s','n');", temptime, psitepols->polid, psitepols->polname, str);

						pthread_mutex_lock(&mutex_db);
						r = db_exec(pkt, NULL, 0);
						if (r != 0) {
							pthread_mutex_unlock(&mutex_db);
							log_printf(3, __FILE__, __FUNCTION__, __LINE__, "insert into rtdata table failed.");
							continue;
						}
						pthread_mutex_unlock(&mutex_db);
#endif
						n = snprintf(temp, 100, ";%s-Rtd=%s",psitepols->polid, str);
						temp += n;
					}
				}

				strncat(cstr, astr, strlen(astr));

			} else {
				log_printf(2, __FILE__, __FUNCTION__, __LINE__, "check comtype.");
				continue;
			}
		} else {
			log_printf(2, __FILE__, __FUNCTION__, __LINE__, "check devtype.");
			continue;
		}
	
		strncat(estr, cstr, sizeof(cstr));
	
	}
 
	/* if all datatype parameter error */
	if (strlen(estr) != 0) {
		snprintf(fstr, 1024, "DataTime=%.14s000%s", datatime, estr);
		n = create_pkt_realtime_data(jstr, fstr);

		pthread_mutex_lock(&mutex);
		for (i = 1; i < (servernum + 1); i++) {
			if (!gather_stop[i]) {
				r = mq_send(mq[i], jstr, n, 0);
				if (r == -1)
					log_printf(3, __FILE__, __FUNCTION__, __LINE__, "mq_send: %s." ,strerror(errno));
			}
		}
		pthread_mutex_unlock(&mutex);

	}

	return 0;
}

static int gather_sbparam(const char *datatime, const char *temptime)
{
	int i, j, k, r, n;
	char cstr[1024];
	char dstr[1024];
	char estr[1024];

	memset(cstr, 0, sizeof(cstr));
	memset(dstr, 0, sizeof(dstr));
	memset(estr, 0, sizeof(estr));

	if ((!p_devices) || (!p_sbparam) || (!p_sitesbs))
		return -1;

	devices_info_t *pdevices;
	pdevices = p_devices->info;

	sitesbs_info_t *psitesbs;
	psitesbs = p_sitesbs->info;

	sbparam_info_t *psbparam;
	psbparam = p_sbparam->info;

	for (i = 0; i < devicesnum; i++, pdevices++) {
		if (!ctx[i])
			continue;

		if (pdevices->devtype == 1) {
			if (pdevices->comtype == 2) {
				/*485*/
				char bstr[1024];

				memset(bstr, 0, sizeof(bstr));
				psitesbs = p_sitesbs->info;
				for (j = 0; j < p_sitesbs->num; j++, psitesbs++) {
					char *temp;
					char astr[512];

					temp = astr;
					memset(astr, 0, sizeof(astr));
					
					/* SB00 and beng */
					if ((psitesbs->sbtype == 0) || (psitesbs->sbtype == 1) || (psitesbs->sbtype == 2)) {
						psbparam = p_sbparam->info;
						for (k = 0; k < p_sbparam->num; k++, psbparam++) {
							char str[32];
							memset(str, 0, sizeof(str));

							if ((!strcmp(psbparam->devname, pdevices->devname)) && (psbparam->devaddr == pdevices->devaddr) \
									&& (!strcmp(psbparam->sbid, psitesbs->sbid))) {
								/* read coil */
								if (!strncmp(psbparam->funcode, "01", 2)) {
									continue;
									/* read input status */
								} else if (!strncmp(psbparam->funcode, "02", 2)) {
									uint8_t dest[24];
									if (psbparam->datatype == 1) {
										if (psbparam->reglen == 1) {

											pthread_mutex_lock(&mutex_ctx);
											r = modbus_read_input_bits_retry(ctx[i], psbparam->regaddr-10001, psbparam->reglen, dest);
											if (r != -1) {
												sprintf(str, "%d", dest[0]);
											//	sprintf(psbparam->polvalue, "%d", dest[0]);
											} else {
												modbus_retry_times++;
												pthread_mutex_unlock(&mutex_ctx);
												continue;
											}
											pthread_mutex_unlock(&mutex_ctx);

										} else {
											log_printf(2, __FILE__, __FUNCTION__, __LINE__, "check reglen.");
											continue;
										}	
									} else {
										log_printf(2, __FILE__, __FUNCTION__, __LINE__, "check datatype.");
										continue;
									}
									/* read holding regsiter */
								} else if (!strncmp(psbparam->funcode, "03", 2)) {
									if (psbparam->datatype == 2) {
										uint16_t dest[32];
#if 0
										if (psbparam->reglen != 1) {
											fprintf(stderr, "Please check reglen if it equals 1\n");
											continue;
										}

										r = modbus_read_registers_retry(ctx[i], psbparam->regaddr-40000, psbparam->reglen, dest);
										if (r != -1) {
											sprintf(str, "%.1f", dest[0] * psbparam->K + psbparam->B);
											/* save polvalue */
											//	psbparam->polvalue = dest[0]*psbparam->K + psbparam->B;
										} else
											continue;
#endif
									} else if (psbparam->datatype == 3) {
										uint16_t dest[24];
#if 1
										if (psbparam->reglen != 2) {
											log_printf(2, __FILE__, __FUNCTION__, __LINE__, "check reglen.");
											continue;
										}
										
										pthread_mutex_lock(&mutex_ctx);
										r = modbus_read_registers_retry(ctx[i], psbparam->regaddr-40000, psbparam->reglen, dest);
										if (r != -1) {
											sprintf(str, "%.3f", modbus_get_float_sw(dest + 0) * psbparam->K + psbparam->B);
											/*
											if (!strncmp(str, "inf", 3)) {
												pthread_mutex_unlock(&mutex_ctx);
												continue;
											}

											*/
											
											int ret = is_in(str);
											if (ret == -1) {
												pthread_mutex_unlock(&mutex_ctx);
												continue;
											}

											/* save polvalue */
											//	psbparam->polvalue = modbus_get_float_sw(dest + 0) * psbparam->K + psbparam->B;
										} else {
											modbus_retry_times++;
											pthread_mutex_unlock(&mutex_ctx);
											continue;
										}
										pthread_mutex_unlock(&mutex_ctx);
#endif
									} else if (psbparam->datatype == 4) {
										int ret = 0;
										uint16_t dest[24];
										uint16_t low_bit, high_bit, value;

										if (psbparam->reglen != 1) {
											log_printf(2, __FILE__, __FUNCTION__, __LINE__, "check reglen.");
											continue;
										}

										if ((psbparam->databit < 0) || (psbparam->databit > 15)) {
											log_printf(2, __FILE__, __FUNCTION__, __LINE__, "check databit.");
											continue;
										}

										pthread_mutex_lock(&mutex_ctx);
										r = modbus_read_registers_retry(ctx[i], psbparam->regaddr-40001, psbparam->reglen, dest);
										if (r != -1) {
											dest[1] = dest[0];
											low_bit = dest[0] >> 8;
											high_bit = (dest[1] & 0xFF) << 8;
											value = low_bit | high_bit;
											
											ret = ((uint16_t)1 << psbparam->databit) & value;
											if (ret > 0)
												strncpy(str, "1", 1);
											else
												strncpy(str, "0", 1);
										} else {
											modbus_retry_times++;
											pthread_mutex_unlock(&mutex_ctx);
											continue;
										}
										pthread_mutex_unlock(&mutex_ctx);

									} else {
										log_printf(2, __FILE__, __FUNCTION__, __LINE__, "check datatype.");
										continue;
									}
									/* read input regsiter */
								} else if (!strncmp(psbparam->funcode, "04", 2)) {

									continue;
								} else {
									log_printf(2, __FILE__, __FUNCTION__, __LINE__, "check funcode.");
									continue;
								}
								
								n = snprintf(temp, 100, "%s-%s=%s,",psbparam->sbid, psbparam->paramname, str);
								temp += n;

							}
						}
					
					} else {
						log_printf(2, __FILE__, __FUNCTION__, __LINE__, "check sbtype.");
						continue;
					}

					astr[strlen(astr) -1] = ';';
					strncat(bstr, astr, strlen(astr));
				}
				
				strncat(cstr, bstr, strlen(bstr));

			} else {
				log_printf(2, __FILE__, __FUNCTION__, __LINE__, "check comtype.");
				continue;
			}	
		} else {
			log_printf(2, __FILE__, __FUNCTION__, __LINE__, "check devtype.");
			continue;
		}
	}

	if (strlen(cstr) != 0) {
		snprintf(dstr, 1024, "DataTime=%.14s000;%s", datatime, cstr);
		n = create_pkt_sbs_status(estr, dstr);

		pthread_mutex_lock(&mutex);
		for (i = 1; i < (servernum + 1); i++) {
			if (!gather_stop[i]) {
				r = mq_send(mq[i], estr, n, 0);
				if (r == -1)
					log_printf(3, __FILE__, __FUNCTION__, __LINE__, "mq_send: %s", strerror(errno));
			}
		}
		pthread_mutex_unlock(&mutex);
	}
	
	return 0;
}

static void timer_thread_sec(union sigval v)
{
	int r;
	char temptime[32];
	char datatime[32];

	r = get_sys_time(datatime, temptime);
	if (r != 0) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "get_sys_time error.");
		return ;
	}

	/**/
	if (modbus_retry_times > ERROR_TIMES) {
		log_printf(2, __FILE__, __FUNCTION__, __LINE__, "the system reboot.");
		system("reboot");
	}

	printf("%.19s, sec gather timer fired, signal: %d\n", temptime, v.sival_int);
	
	if (gather_sitepols(datatime, temptime))
		printf("gather sitepols data failed\n");

	if (gather_sbparam(datatime, temptime))
		printf("gather sbparam data failed\n");
}

static void timer_thread_min(union sigval v)
{
	int i;
	int j;
	int k;
	int r;
	int n;
	char astr[1024];
	char bstr[1024];
	char cstr[1024];
	char datatime[32];
	char temptime[32];

	char *temp = astr;
	memset(bstr, 0, sizeof(bstr));
	memset(cstr, 0, sizeof(cstr));
	memset(astr, 0, sizeof(astr));

	pols_info_t *ppols;
	ppols = p_pols->info;
	
	sitepols_info_t *psitepols;
	psitepols = p_sitepols->info;

	sleep(7);

	r = get_sys_time(datatime, temptime);
	if (r != 0) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "get_sys_time error.");
		return ;
	}
	
	printf("%.19s, minute gather timer fired, signal: %d\n", temptime, v.sival_int);

	for (i = 0; i < p_sitepols->num; i++, psitepols++) {
		/* be careful */
		ppols = p_pols->info;
		for (j = 0; j < p_pols->num; j++, ppols++) {
			/* polname, polid, isstat */
			if ((!strcmp(ppols->polid, psitepols->polid)) && (ppols->isstat == 1)) {
				int row, col;
				char **result;
				char sql[512];

				memset(sql, 0, sizeof(sql));
				snprintf(sql, 512,
						"select min(pol_rtd) as c1,avg(pol_rtd) as c2,"
						"max(pol_rtd) as c3,sum(pol_rtd) as c4 from rtdata where "
						"(julianday(datetime('now','localtime')) - "
						"julianday(datetime(datatime)))*24*60*60<=%d and polid='%s';",
						N_INTERNAL_MIN, psitepols->polid);
			
				pthread_mutex_lock(&mutex_db);	
				r = db_get_table(sql, &result, &row, &col);
				if (r != 0) {
					pthread_mutex_unlock(&mutex_db);
					log_printf(3, __FILE__, __FUNCTION__, __LINE__, "db_get_table error.");
					continue;
				}
				pthread_mutex_unlock(&mutex_db);

				if ((!row) || (!col))
					continue;
				
				if (row > 0) {
					char str[512];
					char ret[24][24];
					int len = (row + 1) * col;

					memset(str, 0, sizeof(str));

					for (k = col; k < len; k++) {
						if (result[k] == NULL) {
							memcpy(ret[k -col], "0.0", strlen("0.0") + 1);
						} else {
							memcpy(ret[k - col], result[k], strlen(result[k]) + 1);
						}
					}
					
					snprintf(str, 512,
							"insert into minutedata(datatime,polid,pol_min,pol_avg,pol_max,pol_col,"
							"centre1,centre2,centre3,centre4,centre5,centre6,flag) "
							"values('%.17s00','%s',%'.1f,%'.1f,%'.1f,%'.1f,1,1,1,1,1,1,0);",
							temptime, psitepols->polid, atof(ret[0]), atof(ret[1]),
							atof(ret[2]), atof(ret[3]));
				
					pthread_mutex_lock(&mutex_db);	
					r = db_exec(str, NULL, 0);
					if (r != 0) {
						pthread_mutex_unlock(&mutex_db);
						log_printf(3, __FILE__, __FUNCTION__, __LINE__, "db_exec error.");
						db_free_table(result);
						continue;
					}
//	
					char qstr[1024];
					memset(qstr, 0, sizeof(qstr));

					snprintf(qstr, sizeof(qstr), "delete from rtdata where "
						"(julianday(datetime('now','localtime')) - "
						"julianday(datetime(datatime)))*24*60*60>60*60 and polid='%s';", psitepols->polid);

					r = db_exec(qstr, NULL, 0);
					if (r != 0) {
						log_printf(3, __FILE__, __FUNCTION__, __LINE__, "db_exec error.");
					}
					pthread_mutex_unlock(&mutex_db);
//
					n = snprintf(temp, 100, ";%s-Min=%'.1f,%s-Avg=%'.1f,%s-Max=%'.1f,%s-Cou=%'.1f", psitepols->polid, \
							atof(ret[0]), psitepols->polid, atof(ret[1]), psitepols->polid, atof(ret[2]), psitepols->polid, atof(ret[3]));
					temp += n;

					db_free_table(result);
				}
			} else {
				continue;
			}
		}	
	}

	/* if all calculate fail */	
	if (strlen(astr) != 0) { 
		snprintf(bstr, 1024, "QN=%.12s00000;DataTime=%.12s00000%s", datatime, datatime, astr);
		n = create_pkt_minute_data(cstr, bstr);

		pthread_mutex_lock(&mutex);
		for (i = 1; i < (servernum + 1); i++) {
			if (!gather_stop[i]) {
				r = mq_send(mq[i], cstr, n, 0);
				if (r == -1)
					log_printf(3, __FILE__, __FUNCTION__, __LINE__, "mq_send: %s.", strerror(errno));
			}
		}
		pthread_mutex_unlock(&mutex);
	}
}

static void timer_thread_hour(union sigval v)
{
	int i;
	int j;
	int k;
	int r;
	int n;
	char astr[1024];
	char bstr[1024];
	char cstr[1024];
	char datatime[32];
	char temptime[32];

	char *temp = astr;
	memset(bstr, 0, sizeof(bstr));
	memset(cstr, 0, sizeof(cstr));
	memset(astr, 0, sizeof(astr));

	pols_info_t *ppols;
	ppols = p_pols->info;
	
	sitepols_info_t *psitepols;
	psitepols = p_sitepols->info;

	sleep(37);

	r = get_sys_time(datatime, temptime);
	if (r != 0) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "get_sys_time error.");
		return ;
	}
	
	printf("%.19s, hourly gather timer fired, signal: %d\n", temptime, v.sival_int);

	for (i = 0; i < p_sitepols->num; i++, psitepols++) {
		/* be careful */
		ppols = p_pols->info;
		for (j = 0; j < p_pols->num; j++, ppols++) {
			/* polname, polid, isstat */
			if ((!strcmp(ppols->polid, psitepols->polid)) && (ppols->isstat == 1)) {
				int row, col;
				char **result;
				char sql[512];

				memset(sql, 0, sizeof(sql));
				snprintf(sql, 512,
						"select min(pol_min) as c1,avg(pol_avg) as c2,"
						"max(pol_max) as c3,sum(pol_col) as c4 from minutedata where "
						"(julianday(datetime('now','localtime')) - "
						"julianday(datetime(datatime)))*24*60*60<=%d and polid='%s';",
						N_INTERNAL_HOUR, psitepols->polid);
			
				pthread_mutex_lock(&mutex_db);	
				r = db_get_table(sql, &result, &row, &col);
				if (r != 0) {
					pthread_mutex_unlock(&mutex_db);
					log_printf(3, __FILE__, __FUNCTION__, __LINE__, "db_get_table error.");
					continue;
				}
				pthread_mutex_unlock(&mutex_db);
				
				if ((!row) || (!col))
					continue;
				
				if (row > 0) {
					char str[512];
					char ret[24][24];
					int len = (row + 1) * col;

					memset(str, 0, sizeof(str));

					for (k = col; k < len; k++) {
						if (result[k] == NULL) {
							memcpy(ret[k -col], "0.0", strlen("0.0") + 1);
						} else {
							memcpy(ret[k - col], result[k], strlen(result[k]) + 1);
						}
					}
					
					snprintf(str, 512,
							"insert into hourlydata(datatime,polid,pol_min,pol_avg,pol_max,pol_col,"
							"centre1,centre2,centre3,centre4,centre5,centre6,flag) "
							"values('%.13s:00:00','%s',%'.1f,%'.1f,%'.1f,%'.1f,1,1,1,1,1,1,0);",
							temptime, psitepols->polid, atof(ret[0]), atof(ret[1]),
							atof(ret[2]), atof(ret[3]));
				
					pthread_mutex_lock(&mutex_db);	
					r = db_exec(str, NULL, 0);
					if (r != 0) {
						pthread_mutex_unlock(&mutex_db);
						log_printf(3, __FILE__, __FUNCTION__, __LINE__, "db_exec error.");
						db_free_table(result);
						continue;
					}
//
					char qstr[1024];
					memset(qstr, 0, sizeof(qstr));

					snprintf(qstr, sizeof(qstr), "delete from minutedata where "
						"(julianday(datetime('now','localtime')) - "
						"julianday(datetime(datatime)))*24*60*60>60*60*24 and polid='%s';",psitepols->polid);

					r = db_exec(qstr, NULL, 0);
					if (r != 0) {
						log_printf(3, __FILE__, __FUNCTION__, __LINE__, "db_exec error.");
					}
					pthread_mutex_unlock(&mutex_db);
//
					n = snprintf(temp, 100, ";%s-Min=%'.1f,%s-Avg=%'.1f,%s-Max=%'.1f,%s-Cou=%'.1f", psitepols->polid, \
							atof(ret[0]), psitepols->polid, atof(ret[1]), psitepols->polid, atof(ret[2]), psitepols->polid, atof(ret[3]));
					temp += n;

					db_free_table(result);
				}
			} else {
				continue;
			}
		}	
	}

	/* if all calculate fail */	
	if (strlen(astr) != 0) { 
		snprintf(bstr, 1024, "QN=%.10s0000000;DataTime=%.10s0000000%s", datatime, datatime, astr);
		n = create_pkt_hourly_data(cstr, bstr);

		pthread_mutex_lock(&mutex);
		for (i = 1; i < (servernum + 1); i++) {
			if (!gather_stop[i]) {
				r = mq_send(mq[i], cstr, n, 0);
				if (r == -1)
					log_printf(3, __FILE__, __FUNCTION__, __LINE__, "mq_send: %s.", strerror(errno));
			}
		}
		pthread_mutex_unlock(&mutex);
	}
}

static void timer_thread_day(union sigval v)
{	
	int i;
	int j;
	int k;
	int r;
	int n;
	char astr[1024];
	char bstr[1024];
	char cstr[1024];
	char datatime[32];
	char temptime[32];

	char *temp = astr;
	memset(bstr, 0, sizeof(bstr));
	memset(cstr, 0, sizeof(cstr));
	memset(astr, 0, sizeof(astr));

	pols_info_t *ppols;
	ppols = p_pols->info;
	
	sitepols_info_t *psitepols;
	psitepols = p_sitepols->info;

	sleep(57);

	r = get_sys_time(datatime, temptime);
	if (r != 0) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "get_sys_time error.");
		return ;
	}

	printf("%.19s, daily gather timer fired, signal: %d\n", temptime, v.sival_int);

	for (i = 0; i < p_sitepols->num; i++, psitepols++) {
		/* be careful */
		ppols = p_pols->info;
		for (j = 0; j < p_pols->num; j++, ppols++) {
			/* polname, polid, isstat */
			if ((!strcmp(ppols->polid, psitepols->polid)) && (ppols->isstat == 1)) {
				int row, col;
				char **result;
				char sql[512];

				memset(sql, 0, sizeof(sql));
				snprintf(sql, 512,
						"select min(pol_min) as c1,avg(pol_avg) as c2,"
						"max(pol_max) as c3,sum(pol_col) as c4 from hourlydata where "
						"(julianday(datetime('now','localtime')) - "
						"julianday(datetime(datatime)))*24*60*60<=%d and polid='%s';",
						N_INTERNAL_DAY, psitepols->polid);
			
				pthread_mutex_lock(&mutex_db);	
				r = db_get_table(sql, &result, &row, &col);
				if (r != 0) {
					pthread_mutex_unlock(&mutex_db);
					log_printf(3, __FILE__, __FUNCTION__, __LINE__, "db_get_table error.");
					continue;
				}
				pthread_mutex_unlock(&mutex_db);
				
				if ((!row) || (!col))
					continue;
				
				if (row > 0) {
					char str[512];
					char ret[24][24];
					int len = (row + 1) * col;

					memset(str, 0, sizeof(str));

					for (k = col; k < len; k++) {
						if (result[k] == NULL) {
							memcpy(ret[k -col], "0.0", strlen("0.0") + 1);
						} else {
							memcpy(ret[k - col], result[k], strlen(result[k]) + 1);
						}
					}
					
					snprintf(str, 512,
							"insert into dailydata(datatime,polid,pol_min,pol_avg,pol_max,pol_col,"
							"centre1,centre2,centre3,centre4,centre5,centre6,flag) "
							"values('%.11s00:00:00','%s',%'.1f,%'.1f,%'.1f,%'.1f,1,1,1,1,1,1,0);",
							temptime, psitepols->polid, atof(ret[0]), atof(ret[1]),
							atof(ret[2]), atof(ret[3]));
				
					pthread_mutex_lock(&mutex_db);	
					r = db_exec(str, NULL, 0);
					if (r != 0) {
						pthread_mutex_unlock(&mutex_db);
						log_printf(3, __FILE__, __FUNCTION__, __LINE__, "db_exec error.");
						db_free_table(result);
						continue;
					}
					pthread_mutex_unlock(&mutex_db);

					n = snprintf(temp, 100, ";%s-Min=%'.1f,%s-Avg=%'.1f,%s-Max=%'.1f,%s-Cou=%'.1f", psitepols->polid, \
							atof(ret[0]), psitepols->polid, atof(ret[1]), psitepols->polid, atof(ret[2]), psitepols->polid, atof(ret[3]));
					temp += n;

					db_free_table(result);
				}
			} else {
				continue;
			}
		}	
	}

	/* if all calculate fail */	
	if (strlen(astr) != 0) { 
		snprintf(bstr, 1024, "QN=%.8s000000000;DataTime=%.8s000000000%s", datatime, datatime, astr);
		n = create_pkt_daily_data(cstr, bstr);

		pthread_mutex_lock(&mutex);
		for (i = 1; i < (servernum + 1); i++) {
			if (!gather_stop[i]) {
				r = mq_send(mq[i], cstr, n, 0);
				if (r == -1)
					log_printf(3, __FILE__, __FUNCTION__, __LINE__, "mq_send: %s.", strerror(errno));
			}
		}
		pthread_mutex_unlock(&mutex);
	}
}

static void timer_thread_watchdog(union sigval v)
{
	int ret;

	ret = mq_send(mq[0], "hello", 6, 0);
	if (ret != 0)
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "mq_send: %s.", strerror(errno));
}

static int create_watchdog_timer()
{
	struct sigevent evp;

	memset(&evp, 0, sizeof(struct sigevent));
	evp.sigev_value.sival_int = 0;
	evp.sigev_notify = SIGEV_THREAD;
	evp.sigev_notify_function = timer_thread_watchdog;

	if (timer_create(CLOCKID, &evp, &timerid) == -1) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "timer_create: %s.", strerror(errno));
		return -1;
	}

	return 0;
}

static int create_gather_timer_sec(void)
{
	struct sigevent evp;

	memset(&evp, 0, sizeof(struct sigevent));
	evp.sigev_value.sival_int = 1;
	evp.sigev_notify = SIGEV_THREAD;
	evp.sigev_notify_function = timer_thread_sec;

	if (timer_create(CLOCKID, &evp, &timerid_sec) == -1) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "timer_create: %s.", strerror(errno));
		return -1;
	}

	return 0;
}

static int create_gather_timer_min(void)
{
	struct sigevent evp;

	memset(&evp, 0, sizeof(struct sigevent));
	evp.sigev_value.sival_int = 2;
	evp.sigev_notify = SIGEV_THREAD;
	evp.sigev_notify_function = timer_thread_min;

	if (timer_create(CLOCKID, &evp, &timerid_min) == -1) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "timer_create: %s.", strerror(errno));
		return -1;
	}

	return 0;
}

static int create_gather_timer_hour(void)
{
	struct sigevent evp;

	memset(&evp, 0, sizeof(struct sigevent));
	evp.sigev_value.sival_int = 4;
	evp.sigev_notify = SIGEV_THREAD;
	evp.sigev_notify_function = timer_thread_hour;

	if (timer_create(CLOCKID, &evp, &timerid_hour) == -1) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "timer_create: %s.", strerror(errno));
		return -1;
	}

	return 0;
}

static int create_gather_timer_day(void)
{
	struct sigevent evp;

	memset(&evp, 0, sizeof(struct sigevent));
	evp.sigev_value.sival_int = 8;
	evp.sigev_notify = SIGEV_THREAD;
	evp.sigev_notify_function = timer_thread_day;

	if (timer_create(CLOCKID, &evp, &timerid_day) == -1) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "timer_create: %s.", strerror(errno));
		return -1;
	}

	return 0;
}

static int start_watchdog_timer(void)
{
	it.it_interval.tv_sec = WATCHDOG_TIME;
	it.it_interval.tv_nsec = 0;
	it.it_value.tv_sec = WATCHDOG_TIME-2;
	it.it_value.tv_nsec = 0;

	if (timer_settime(timerid, 0, &it, NULL) == -1) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "timer_settime: %s.", strerror(errno));
		return -1;
	}

	return 0;
}

static int start_gather_timer_sec(void)
{
	it_sec.it_interval.tv_sec = N_INTERNAL_SEC;
	it_sec.it_interval.tv_nsec = 0;
	it_sec.it_value.tv_sec = N_VALUE_SEC;
	it_sec.it_value.tv_nsec = 0;

	if (timer_settime(timerid_sec, 0, &it_sec, NULL) == -1) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "timer_settime: %s.", strerror(errno));
		return -1;
	}

	return 0;
}

static int start_gather_timer_min(void)
{
	it_min.it_interval.tv_sec = N_INTERNAL_MIN;
	it_min.it_interval.tv_nsec = 0;
	it_min.it_value.tv_sec = N_VALUE_MIN;
	it_min.it_value.tv_nsec = 0;

	if (timer_settime(timerid_min, 0, &it_min, NULL) == -1) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "timer_settime: %s.", strerror(errno));
		return -1;
	}

	return 0;
}

static int start_gather_timer_hour(void)
{
	it_hour.it_interval.tv_sec = N_INTERNAL_HOUR;
	it_hour.it_interval.tv_nsec = 0;
	it_hour.it_value.tv_sec = N_VALUE_HOUR;
	it_hour.it_value.tv_nsec = 0;

	if (timer_settime(timerid_hour, 0, &it_hour, NULL) == -1) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "timer_settime: %s.", strerror(errno));
		return -1;
	}

	return 0;
}

static int start_gather_timer_day(void)
{
	it_day.it_interval.tv_sec = N_INTERNAL_DAY;
	it_day.it_interval.tv_nsec = 0;
	it_day.it_value.tv_sec = N_VALUE_DAY;
	it_day.it_value.tv_nsec = 0;

	if (timer_settime(timerid_day, 0, &it_day, NULL) == -1) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "timer_settime: %s.", strerror(errno));
		return -1;
	}

	return 0;
}

static int stop_watchdog_timer(void)
{
	it.it_interval.tv_sec = 0;
	it.it_interval.tv_nsec = 0;
	it.it_value.tv_sec = 0;
	it.it_value.tv_nsec = 0;

	if (timer_settime(timerid, 0, &it, NULL) == -1) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "timer_settime: %s.", strerror(errno));
		return -1;
	}

	return 0;
}

static int stop_gather_timer_sec(void)
{
	it_sec.it_interval.tv_sec = 0;
	it_sec.it_interval.tv_nsec = 0;
	it_sec.it_value.tv_sec = 0;
	it_sec.it_value.tv_nsec = 0;

	if (timer_settime(timerid_sec, 0, &it_sec, NULL) == -1) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "timer_settime: %s.", strerror(errno));
		return -1;
	}

	return 0;
}

static int stop_gather_timer_min(void)
{
	it_min.it_interval.tv_sec = 0;
	it_min.it_interval.tv_nsec = 0;
	it_min.it_value.tv_sec = 0;
	it_min.it_value.tv_nsec = 0;

	if (timer_settime(timerid_min, 0, &it_min, NULL) == -1) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "timer_settime: %s.", strerror(errno));
		return -1;
	}

	return 0;
}

static int stop_gather_timer_hour(void)
{
	it_hour.it_interval.tv_sec = 0;
	it_hour.it_interval.tv_nsec = 0;
	it_hour.it_value.tv_sec = 0;
	it_hour.it_value.tv_nsec = 0;

	if (timer_settime(timerid_hour, 0, &it_hour, NULL) == -1) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "timer_settime: %s.", strerror(errno));
		return -1;
	}

	return 0;
}

static int stop_gather_timer_day(void)
{
	it_day.it_interval.tv_sec = 0;
	it_day.it_interval.tv_nsec = 0;
	it_day.it_value.tv_sec = 0;
	it_day.it_value.tv_nsec = 0;

	if (timer_settime(timerid_day, 0, &it_day, NULL) == -1) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "timer_settime: %s.", strerror(errno));
		return -1;
	}

	return 0;
}

static int delete_watchdog_timer(void)
{
	int r;

	r = timer_delete(timerid);
	if (r == -1) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "timer_delete: %s.", strerror(errno));
		return -1;
	}

	return 0;
}

static int delete_sec_timer(void)
{
	int r;

	r = timer_delete(timerid_sec);
	if (r == -1) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "timer_delete: %s.", strerror(errno));
		return -1;
	}

	return 0;
}

static int delete_min_timer(void)
{
	int r;

	r = timer_delete(timerid_min);
	if (r == -1) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "timer_delete: %s.", strerror(errno));
		return -1;
	}

	return 0;
}

static int delete_hour_timer(void)
{
	int r;

	r = timer_delete(timerid_hour);
	if (r == -1) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "timer_delete: %s.", strerror(errno));
		return -1;
	}

	return 0;
}

static int delete_day_timer(void)
{
	int r;
	
	r = timer_delete(timerid_day);
	if (r == -1) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "timer_delete: %s.", strerror(errno));
		return -1;
	}

	return 0;
}

static void parse_recv_respond(const char *pkt_r, const int flag)
{
	char qn[32];
	char cn[32];

	memset(qn, 0, sizeof(qn));
	memset(cn, 0, sizeof(cn));

	if (strncmp(pkt_r + 2 + 4, "ST", 2) != 0)
		return ;
	
	sscanf(pkt_r + 2 + 4, "%*54sQN=%17s", qn);
	sscanf(pkt_r + 2 + 4, "%*75sCN=%4s",  cn);

	/* 2072 */
	if (!strcmp(RESPOND_ALARM_OK, cn)) {
		respond_alarm_handler(qn, flag);
		printf("receive respond = %s\n", cn);
	}
}

static void parse_recv_command(const char *pkt_r, const int flag)
{
	char qn[32];	
	char cn[32];

	memset(qn, 0, sizeof(qn));
	memset(cn, 0, sizeof(cn));

	if (strncmp(pkt_r + 2 + 4, "QN", 2) != 0)
		return ;

	sscanf(pkt_r + 2 + 4, "QN=%17s", qn);
	sscanf(pkt_r + 2 + 4, "%*27sCN=%4s", cn);

	/* 3022 */
	if (strcmp(CMD_SET_ANALOG_VALUE, cn) == 0) {
		printf("receive comand : %s\n", cn);
		set_analog_value(mq[flag], pkt_r, qn);
	}
	/* 1012 */
	if (strcmp(CMD_SET_LOCALTIME, cn) == 0) {
		printf("receive comand : %s\n", cn);
		set_localtime(mq[flag], pkt_r, qn);
	}
	/* 2051 */
	if (strcmp(CMD_GET_MINUTEDATA, cn) == 0) {
		printf("receive comand : %s\n", cn);
		get_minute_data(mq[flag], pkt_r, qn);
	}
	/* 2061 */
	if (strcmp(CMD_GET_HOURDATA, cn) == 0) {
		printf("receive comand : %s\n", cn);
		get_hour_data(mq[flag], pkt_r, qn);
	}
	/* 2031 */
	if (strcmp(CMD_GET_DAYDATA, cn) == 0) {
		printf("receive comand : %s\n", cn);
		get_day_data(mq[flag], pkt_r, qn);
	}
}

static void *receiver_routine(void *arg)
{
	int r;
	int i;
	char buf[MAX_DATA_PACKET];

	if (!p_server)
		return NULL;

	while (1) {	
		for (i = 1; i < (servernum + 1); i++) {
			memset(buf, 0, MAX_DATA_PACKET);
			r = mq_receive(mqtest[i], buf, MAX_DATA_PACKET, NULL);
			if (r > 0) {
				if (!strncmp(buf, "2051", 4)) {
					respond_minutedata_handler(buf, i);
					continue;
				}

				if (!strncmp(buf, "2061", 4)) {
					respond_hourdata_handler(buf, i);
					continue;
				}

				if (!strncmp(buf, "2051", 4)) {
					respond_daydata_handler(buf, i);
					continue;
				}

				parse_recv_command(buf, i);
				parse_recv_respond(buf, i);

			} else {
				if ((errno == EAGAIN) || (errno == EINTR))
					continue;
				else
					log_printf(3, __FILE__, __FUNCTION__, __LINE__, "mq_receive: %s.", strerror(errno));
			}
		} 
	}

	return NULL;	
}

static void *sendalarm_routine(void *arg)
{
	int r;
	int i;
	int j;
	int n;
	int row;
	int col;
	char **result;
	char str[256];
	char pkt[512];
	char sql[512];
	char datatime[32];

	if (!p_server)
		return NULL;

	r = get_sys_time(datatime, NULL); 
	if (r != 0) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "get_sys_time error.");
		return NULL;
	}
#if 1
	/* first cancel alarm info */
	for (i = 1; i < (servernum + 1); i++) {
		memset(sql, 0, sizeof(sql));
		snprintf(sql, 512, "select * from polsalarm;");

		pthread_mutex_lock(&mutex_db);
		r = db_get_table(sql, &result, &row, &col);
		if (r != 0) {
			pthread_mutex_unlock(&mutex_db);
			log_printf(3, __FILE__, __FUNCTION__, __LINE__, "select polsalarm table failed.");
			continue;
		}
		pthread_mutex_unlock(&mutex_db);

		if ((!row) || (!col))
			continue;

		for (j = 0; j < row; j++) {
			memset(str, 0, sizeof(str));
			memset(pkt, 0, sizeof(pkt));
			
			snprintf(str, 256, "QN=%s;DataTime=%s;%s-Ala=0.0,AlarmType=0", datatime, datatime, *(result + col + j*col + 1));

			n = create_pkt_alarm_data(pkt, str);
			if (!gather_stop[i]) {
				r = mq_send(mq[i], pkt, n, 0);
				if (r == -1)
					log_printf(3, __FILE__, __FUNCTION__, __LINE__, "mq_send: %s.", strerror(errno));
			}
		}

		db_free_table(result);
	}
#endif	
	while (1) {
		sleep(SEND_ALARM_TIME);

		for (i = 1; i < (servernum + 1); i++) {
			if (!gather_stop[i]) {
				memset(sql, 0, sizeof(sql));
				snprintf(sql, 512, "select * from alarm where centre%d='0';", i);
				
				pthread_mutex_lock(&mutex_db);
				r = db_get_table(sql, &result, &row, &col);
				if (r != 0) {
					pthread_mutex_unlock(&mutex_db);
					log_printf(3, __FILE__, __FUNCTION__, __LINE__, "select alarm table failed.");
					continue;
				}
				pthread_mutex_unlock(&mutex_db);

				if ((!row) || (!col))
					continue;
				
				for (j = 0; j < row; j++) {
					memset(str, 0, sizeof(str));
					memset(pkt, 0, sizeof(pkt));

					snprintf(str, 256, "QN=%s;DataTime=%s;%s-Ala=%s,AlarmType=%s", *(result + col + j*col + 1), *(result + col + j*col + 1), \
							*(result + col + j*col + 3), *(result + col + j*col + 4), *(result + col + j*col + 2));

					n = create_pkt_alarm_data(pkt, str);
					r = mq_send(mq[i], pkt, n, 0);
					if (r == -1)
						log_printf(3, __FILE__, __FUNCTION__, __LINE__, "mq_send: %s.", strerror(errno));
				}

				db_free_table(result);
			}
		}
	}

	return NULL;
}

static void *recvalarm_routine(void *arg)
{
	int i;
	int j;
	int k;
	int r;
	char datatime[32];

	if ((!p_devices) || (!p_polsalarm) || (!p_sitepols) || (!p_server))
		return NULL;

	sitepols_info_t *psitepols;
	psitepols = p_sitepols->info;

	devices_info_t *pdevices;
	pdevices = p_devices->info;

	polsalarm_info_t *ppolsalarm;
	ppolsalarm = p_polsalarm->info;

	for (i = 0; i < p_polsalarm->num; i++, ppolsalarm++)
		ppolsalarm->flag = 0;

	while (1) {
		sleep(RECV_ALARM_TIME);
		
		pdevices = p_devices->info;
		for (i = 0; i < devicesnum; i++, pdevices++) {
			if (!ctx[i])
				continue;
			
			ppolsalarm = p_polsalarm->info;
			for (j = 0; j < p_polsalarm->num; j++, ppolsalarm++) {
				char str[32];
				memset(str, 0, sizeof(str));
				if ((!strcmp(ppolsalarm->devname, pdevices->devname)) && (ppolsalarm->devaddr == pdevices->devaddr)) {

					if (!strncmp(ppolsalarm->funcode, "01", 2)) {
						continue;
					} else if (!strncmp(ppolsalarm->funcode, "02", 2)) {
						continue;
					} else if (!strncmp(ppolsalarm->funcode, "04", 2)) {
						continue;
					} else if (strncmp(ppolsalarm->funcode, "03", 2)) {
						log_printf(2, __FILE__, __FUNCTION__, __LINE__, "check funcode.");
						continue;
					}
					
					/* funcode = 0x03H */
					if (ppolsalarm->datatype == 2) {
						continue;
					} else if (ppolsalarm->datatype == 3) {
						continue;
					} else if (ppolsalarm->datatype != 4) {
						log_printf(2, __FILE__, __FUNCTION__, __LINE__, "check datatype.");
						continue;
					}
					
					/* datatype = 4 */
					int ret = 0;
					uint16_t dest[24];
					uint16_t low_bit, high_bit, value;

					if (ppolsalarm->reglen != 1) {
						log_printf(2, __FILE__, __FUNCTION__, __LINE__, "check reglen.");
						continue;
					}

					if ((ppolsalarm->databit < 0) || (ppolsalarm->databit > 15)) {
						log_printf(2, __FILE__, __FUNCTION__, __LINE__, "check databit.");
						continue;
					}

					pthread_mutex_lock(&mutex_ctx);
					r = modbus_read_registers_retry(ctx[i], ppolsalarm->regaddr-40001, ppolsalarm->reglen, dest);
					if (r != -1) {
						dest[1] = dest[0];
						low_bit = dest[0] >> 8;
						high_bit = (dest[1] & 0xFF) << 8;
						value = low_bit | high_bit;
						ret = ((uint16_t)1 << ppolsalarm->databit) & value;
						
						if (ppolsalarm->alarmtype == 2) {
							if (ret > 0)
								ret = 0;
							else
								ret = 1;
						}

					} else {
						pthread_mutex_unlock(&mutex_ctx);
						continue;
					}
					pthread_mutex_unlock(&mutex_ctx);
					
					if (ret > 0) {
						if (ppolsalarm->flag == 0)
							ppolsalarm->flag = 1;
						else if (ppolsalarm->flag == 1)
							continue;
						else if (ppolsalarm->flag == 2)
							ppolsalarm->flag = 1;

						char str[512];
						memset(str, 0, sizeof(str));
					
						r = get_sys_time(datatime, NULL);
						if (r != 0) {
							log_printf(3, __FILE__, __FUNCTION__, __LINE__, "get_sys_time error.");
							continue;
						}

						psitepols = p_sitepols->info;
						for (k = 0; k < p_sitepols->num; k++, psitepols++) {
							if (!strcmp(psitepols->polid, ppolsalarm->polid))
								break;
						}

						if (k == p_sitepols->num) {
							log_printf(2, __FILE__, __FUNCTION__, __LINE__, "no suitable polid.");
							continue;
						}	

						snprintf(str, 512, "insert into alarm(alarmtime,alarmtype,polid,alarmvalue,"
								"centre1,centre2,centre3,centre4,centre5,centre6,flag) "
								"values('%s', '%d', '%s', '%.1f','0','0','0','0','0','0','0');", 
								datatime, ppolsalarm->alarmtype, ppolsalarm->polid, psitepols->polvalue);
						
						pthread_mutex_lock(&mutex_db);
						r = db_exec(str, NULL, 0);
						if (r != 0) {
							pthread_mutex_unlock(&mutex_db);
							log_printf(3, __FILE__, __FUNCTION__, __LINE__, "db_exec: %s.", str);
						}
						pthread_mutex_unlock(&mutex_db);
					}

					if (ret == 0) {
						if (ppolsalarm->flag == 1) {
							char str[512];
							memset(str, 0, sizeof(str));
							
							r = get_sys_time(datatime, NULL);
							if (r != 0) {
								log_printf(2, __FILE__, __FUNCTION__, __LINE__, "get_sys_time error.");
								continue;
							}

							psitepols = p_sitepols->info;
							for (k = 0; k < p_sitepols->num; k++, psitepols++) {
								if (!strcmp(psitepols->polid, ppolsalarm->polid))
									break;
							}

							if (k == p_sitepols->num) {
								log_printf(2, __FILE__, __FUNCTION__, __LINE__, "no suitable polid.");
								continue;
							}	
							
							snprintf(str, 512, "insert into alarm(alarmtime,alarmtype,polid,alarmvalue,"
										"centre1,centre2,centre3,centre4,centre5,centre6,flag) "
										"values('%s', '0', '%s', '%.1f','0','0','0','0','0','0','0');", 
										datatime, ppolsalarm->polid, psitepols->polvalue);

							pthread_mutex_lock(&mutex_db);
							r = db_exec(str, NULL, 0);
							if (r != 0) {
								pthread_mutex_unlock(&mutex_db);
								log_printf(3, __FILE__, __FUNCTION__, __LINE__, "db_exec: %s.", str);
							} else {
								pthread_mutex_unlock(&mutex_db);
								ppolsalarm->flag = 2;
							}
						}
					}
				}
			}
		}
	}

	return NULL;
}

static void db_table_init(void)
{
	int r;

	r = get_siteconf_settings();
	if (r != 0)
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "select siteconf table failed.");

	r = get_server_settings();
	if (r != 0)
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "select server table failed.");

	r = get_devices_settings();
	if (r != 0)
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "select device table failed.");
	
	r = get_sitesbs_settings();
	if (r != 0)
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "select sitesbs table failed.");
	
	r = get_pols_settings();
	if (r != 0)
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "select pols table failed.");
	
	r = get_sitepols_settings();
	if (r != 0)
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "select sitepols table failed.");
	
	r = get_sbparam_settings();
	if (r != 0)
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "select sbparam table failed.");
	
	r = get_polsalarm_settings();
	if (r != 0)
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "select polsalarm table failed.");
	
	r = get_ctlparam_settings();
	if (r != 0)
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "select ctlparam table failed.");
}

static void db_table_uninit(void)
{
	put_siteconf_settings();
	put_server_settings();
	put_devices_settings();
	put_sitesbs_settings();
	put_pols_settings();
	put_sitepols_settings();
	put_sbparam_settings();
	put_polsalarm_settings();
	put_ctlparam_settings();
}

static void modbus_device_init(void)
{
	get_modbus_settings();
}

static void modbus_device_uninit(void)
{
	put_modbus_settings();
}

static void *timer_routine(void *arg)
{
	int r;
	struct tm *ptm;
	struct timeval tv;
	struct timezone tz;

	/* create timers */
	create_gather_timer_sec();
	create_gather_timer_min();
	create_gather_timer_hour();
	create_gather_timer_day();

	r = gettimeofday(&tv, &tz);
	if (r == -1) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "gettimeofday: %s.", strerror(errno));
		return NULL;
	}

	ptm = localtime(&(tv.tv_sec));
	if (ptm == NULL) {
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "localtime: %s.", strerror(errno));
		return NULL;
	}

	/*if N_VALUE_SEC=0 timer will not work */
	N_VALUE_SEC  = APPROACH(ptm->tm_sec, N_INTERNAL_SEC);
	if (N_VALUE_SEC == 0)
		N_VALUE_SEC = N_INTERNAL_SEC;

	N_VALUE_MIN  = APPROACH(ptm->tm_min * 60 + ptm->tm_sec, N_INTERNAL_MIN);
	if (N_VALUE_MIN == 0)
		N_VALUE_MIN = N_INTERNAL_MIN;

	N_VALUE_HOUR = APPROACH(ptm->tm_hour * 60 * 60 + ptm->tm_min * 60 + ptm->tm_sec, N_INTERNAL_HOUR);
	if (N_VALUE_HOUR == 0)
		N_VALUE_HOUR = N_INTERNAL_HOUR;

	N_VALUE_DAY  = APPROACH(ptm->tm_mday * 60 * 60 * 24 + ptm->tm_hour * 60 * 60 + 
			ptm->tm_min * 60 + ptm->tm_sec, N_INTERNAL_DAY);
	if (N_VALUE_DAY == 0)
		N_VALUE_DAY = N_INTERNAL_DAY;

	start_gather_timer_sec();
	start_gather_timer_min();
	start_gather_timer_hour();
	start_gather_timer_day();
	
	check_time_flag = 0;

	while (1) {
		if(check_time_flag) {	
		
			struct tm time_tm;
			struct timeval tv;	
			struct timeval time_tv;
			time_t timep;  
			
			stop_gather_timer_sec();
			stop_gather_timer_min();
			stop_gather_timer_hour();
			stop_gather_timer_day();

			if (strlen(check_time) <= 0) {
				check_time_flag = 0;
				continue;	
			}

			sscanf(check_time, "%4d%2d%2d%2d%2d%2d", &time_tm.tm_year, &time_tm.tm_mon, &time_tm.tm_mday, &time_tm.tm_hour, \
					&time_tm.tm_min, &time_tm.tm_sec);  

			time_tm.tm_year -= 1900;	
			time_tm.tm_mon -= 1;  
			time_tm.tm_wday = 0;
			time_tm.tm_yday = 0;
			time_tm.tm_isdst = 0;

			memset(check_time, 0, 32);

			timep = mktime(&time_tm);
			time_tv.tv_sec = timep;  
			time_tv.tv_usec = 0;  

			r = settimeofday(&time_tv, NULL);
			if(r != 0) {  
				log_printf(3, __FILE__, __FUNCTION__, __LINE__, "settimeofday: %s.", strerror(errno));
				check_time_flag = 0;
				return NULL;  
			}

			sleep(3);

			r = gettimeofday(&tv, NULL);
			if (r == -1) {
				log_printf(3, __FILE__, __FUNCTION__, __LINE__, "gettimeofday: %s.", strerror(errno));
				check_time_flag = 0;
				return NULL;
			}

			ptm = localtime(&(tv.tv_sec));
			if (ptm == NULL) {
				log_printf(3, __FILE__, __FUNCTION__, __LINE__, "localtime: %s.", strerror(errno));
				check_time_flag = 0;
				return NULL;
			}

			/*if N_VALUE_SEC=0 timer will not work */
			N_VALUE_SEC  = APPROACH(ptm->tm_sec, N_INTERNAL_SEC);
			if (N_VALUE_SEC == 0)
				N_VALUE_SEC = N_INTERNAL_SEC;

			N_VALUE_MIN  = APPROACH(ptm->tm_min * 60 + ptm->tm_sec, N_INTERNAL_MIN);
			if (N_VALUE_MIN == 0)
				N_VALUE_MIN = N_INTERNAL_MIN;

			N_VALUE_HOUR = APPROACH(ptm->tm_hour * 60 * 60 + ptm->tm_min * 60 + ptm->tm_sec, N_INTERNAL_HOUR);
			if (N_VALUE_HOUR == 0)
				N_VALUE_HOUR = N_INTERNAL_HOUR;

			N_VALUE_DAY  = APPROACH(ptm->tm_mday * 60 * 60 * 24 + ptm->tm_hour * 60 * 60 + 
					ptm->tm_min * 60 + ptm->tm_sec, N_INTERNAL_DAY);
			if (N_VALUE_DAY == 0)
				N_VALUE_DAY = N_INTERNAL_DAY;

			start_gather_timer_sec();
			start_gather_timer_min();
			start_gather_timer_hour();
			start_gather_timer_day();

			check_time_flag = 0;
		}
	}

	return NULL;
}

/*
 * Don't forget to unlink the message queue.before running your program again.
 * if you don't unlink it, it will still use the old message queue settings.
 * this happens when you end your program with Ctrl+C.
 *
 */
static void unlink_mqueue(void)
{
	int i;
	int rc;

	for (i = 1; i < (servernum + 1); i++) {
		rc = mq_unlink(client_mqfile[i]);
		if (rc != 0 && errno != ENOENT) {
			log_printf(2, __FILE__, __FUNCTION__, __LINE__, "Message queue %s "
					"remove from system failed.", client_mqfile[i]);
		} else {
#ifdef __DBG__
			printf("Message queue %s remove from system ok\n", client_mqfile[i]);
#endif
		}

		rc = mq_unlink(mqfile[i]);
		if (rc != 0 && errno != ENOENT) {
			log_printf(2, __FILE__, __FUNCTION__, __LINE__, "Message queue %s "
					"remove from system failed.", mqfile[i]);
		} else {
#ifdef __DBG__
			printf("Message queue %s remove from system ok\n", mqfile[i]);
#endif
		}
	}
}

int main(int argc, char *argv[])
{
	int i, r;	
	
	int fd = open(LOG_FILE, O_RDWR | O_CREAT | O_APPEND, 0660);
	if (fd == -1)
		err_sys("open");

	dup2(fd, STDERR_FILENO);

	if ((argc != 1) || (!argv[0]))
		log_printf(4, __FILE__, __FUNCTION__, __LINE__, "gather parameter error.");

	r = db_open(FILENAME);
	if (r != 0)
		log_printf(4, __FILE__, __FUNCTION__, __LINE__, "open database %s failed.", FILENAME);
	
	printf("open database %s ok\n", FILENAME);
	
	db_table_init();

	modbus_device_init();

	set_block_signal();

	/* watchdog mqueue */
	mq[0] = mq_open(argv[0], O_RDWR);
	if (mq[0] == ((mqd_t) -1))
		log_printf(4, __FILE__, __FUNCTION__, __LINE__, "mq_open: %s.", strerror(errno));
	
	if (p_server)
		servernum = MIN_V(p_server->num, MAXSERVER);
		
	if (p_devices)
		devicesnum = MIN_V(p_devices->num, MAXDEVS);

	unlink_mqueue();

	for (i = 1; i < (servernum + 1); i++) {
		memset(&cli_attr[i], 0, sizeof(struct mq_attr));
		cli_attr[i].mq_flags   = 0;
		cli_attr[i].mq_maxmsg  = 10;
		cli_attr[i].mq_msgsize = 1024;
		cli_attr[i].mq_curmsgs = 0;
	}

	/* client mqueue */
	for (i = 1; i < (servernum + 1); i++) {
		mq[i] = mq_open(client_mqfile[i], O_CREAT | O_RDWR, 0777, &cli_attr[i]);
		if (mq[i] == ((mqd_t) -1))
			log_printf(4, __FILE__, __FUNCTION__, __LINE__, "mq_open<%d>: %s.", i, strerror(errno));
	}

	for (i = 1; i < (servernum + 1); i++) {
		memset(&rev_attr[i], 0, sizeof(struct mq_attr));
		rev_attr[i].mq_flags   = 0;
		rev_attr[i].mq_maxmsg  = 10;
		rev_attr[i].mq_msgsize = 1024;
		rev_attr[i].mq_curmsgs = 0;
	}

	/* gather and client communication */
	for (i = 1; i < (servernum + 1); i++) {
		mqtest[i] = mq_open(mqfile[i], O_CREAT | O_RDWR | O_NONBLOCK, 0777, &rev_attr[i]);
		if (mqtest[i] == ((mqd_t) -1))
			log_printf(4, __FILE__, __FUNCTION__, __LINE__, "mq_open<%d>: %s.", i, strerror(errno));
	}

	r = create_watchdog_timer();
	if (r != 0)
		log_printf(4, __FILE__, __FUNCTION__, __LINE__, "create watchdog timer failed.");

	r = start_watchdog_timer();
	if (r != 0)
		log_printf(4, __FILE__, __FUNCTION__, __LINE__, "start watchdog timer failed.");

	set_sigrtmin1_handler();
	set_sigrtmin2_handler();

	pthread_mutex_init(&mutex, NULL);
	pthread_mutex_init(&mutex_db, NULL);
	pthread_mutex_init(&mutex_ctx, NULL);

	sleep(1);

	/* create timer thread */
	r = pthread_create(&thr_timer,  NULL, timer_routine,  NULL);
	if (r != 0)
		log_printf(4, __FILE__, __FUNCTION__, __LINE__, "create timer routine failed.");
	
	/* create threads */
	r = pthread_create(&thr_receiver,  NULL, receiver_routine,  NULL);
	if (r != 0)
		log_printf(4, __FILE__, __FUNCTION__, __LINE__, "create receiver routine failed.");

	r = pthread_create(&thr_recvalarm, NULL, recvalarm_routine, NULL);
	if (r != 0)
		log_printf(4, __FILE__, __FUNCTION__, __LINE__, "create recvalarm routine failed.");
	
	r = pthread_create(&thr_sendalarm, NULL, sendalarm_routine, NULL);
	if (r != 0)
		log_printf(4, __FILE__, __FUNCTION__, __LINE__, "create sendalarm routine failed.");

	/* join threads */
	r = pthread_join(thr_receiver,  NULL);
	if (r != 0)
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "join receiver routine failed.");
	
	r = pthread_join(thr_recvalarm, NULL);
	if (r != 0)
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "join recvalarm routine failed.");

	r = pthread_join(thr_sendalarm, NULL);
	if (r != 0)
		log_printf(3, __FILE__, __FUNCTION__, __LINE__, "join sendalarm routine failed.");
	
	return 0;
}

