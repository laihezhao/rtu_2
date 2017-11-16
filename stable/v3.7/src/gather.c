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

/* new add */
#include "polsalarm.h"
#include "ctlparam.h"

#define __DBG__

#define ERROR_TIMES		(100)
#define RECV_ALARM_TIME		(5)
#define SEND_ALARM_TIME 	(10)

#define CMD_SET_ANALOG_VALUE  	"3022"
#define RESPOND_ALARM_OK        "2072"
#define RESPOND_DAY_STATUS_DATA "2041"

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

static pthread_mutex_t mutex;
static pthread_mutex_t mutex_db;
static pthread_mutex_t mutex_ctx;

static pthread_t thr_receiver;
static pthread_t thr_recvalarm;
static pthread_t thr_sendalarm;

modbus_t *ctx[MAXDEVS] = {NULL};

static int servernum;
static int devicesnum;
static long int msgsize;
static mqd_t mq[MAXSERVER + 1];
static mqd_t mqtest[MAXSERVER + 1];
static int gather_stop[MAXSERVER + 1] = {0};
static char *client_mqfile[MAXSERVER + 1] = {"/mq_data0", "/mq_data1", "/mq_data2", "/mq_data3", \
						"/mq_data4", "/mq_data5", "/mq_data6"};
static char *mqfile[MAXSERVER + 1] = {"/mqtest0", "/mqtest1", "/mqtest2", "/mqtest3", \
					"/mqtest4", "/mqtest5", "/mqtest6"};
pols_t *p_pols         = NULL;
server_t *p_server     = NULL;
sitesbs_t *p_sitesbs   = NULL;
devices_t *p_devices   = NULL;
sbparam_t *p_sbparam   = NULL;
siteconf_t *p_siteconf = NULL;
sitepols_t *p_sitepols = NULL;

/* new add */
polsalarm_t *p_polsalarm = NULL;
ctlparam_t *p_ctlparam   = NULL;

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
	printf("catch SIGRTMIN+1 signal\n");
	gather_stop[info->si_value.sival_int] = 1;
}

static void sig_rtmin2_handler(int sig, siginfo_t *info, void *ucontext)
{
	printf("catch SIGRTMIN+2 signal\n");
	gather_stop[info->si_value.sival_int] = 0;
}

static void set_block_signal(void)
{
	sigfillset(&act.sa_mask);
	sigprocmask(SIG_BLOCK, &act.sa_mask, NULL);
}

static void set_sigrtmin1_handler(void)
{
	act_rtmin1.sa_sigaction = sig_rtmin1_handler;
	sigemptyset(&act_rtmin1.sa_mask);
	sigaddset(&act_rtmin1.sa_mask, SIGRTMIN+1);
	act_rtmin1.sa_flags = SA_SIGINFO;
	sigaction(SIGRTMIN+1, &act_rtmin1, NULL);
}

static void set_sigrtmin2_handler(void)
{
	act_rtmin2.sa_sigaction = sig_rtmin2_handler;
	sigemptyset(&act_rtmin2.sa_mask);
	sigaddset(&act_rtmin2.sa_mask, SIGRTMIN+2);
	act_rtmin2.sa_flags = SA_SIGINFO;
	sigaction(SIGRTMIN+2, &act_rtmin2, NULL);
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
		fprintf(stderr, "failed: %s\n", sql);
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
		fprintf(stderr, "failed: %s\n", sql);
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
		fprintf(stderr, "failed: %s\n", sql);
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
		fprintf(stderr, "failed: %s\n", sql);
	}
	pthread_mutex_unlock(&mutex_db);
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
			fprintf(stderr, "SBID ERROR\n");
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
							fprintf(stderr, "FUNCODE ERROR\n");
							continue;
						}
						
						if (pctlparam->datatype != 3) {
							fprintf(stderr, "DATATYPE ERROR\n");
							continue;
						}

						if (pctlparam->reglen != 2) {
							fprintf(stderr, "REGLEN ERROR\n");
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
							fprintf(stderr, "set sp parameter failed\n");
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
			fprintf(stderr, "SBID or PARAMETER ERROR\n");
			continue;
		}	
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
									fprintf(stderr, "Please check reglen if it equals 2\n");
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
								fprintf(stderr, "datatype parameter error\n");
								continue;
							}
						
						/* read input regsiter */
						} else if (!strncmp(psitepols->funcode, "04", 2)) {

							continue;
						} else {
							fprintf(stderr, "funcode parameter error\n");
							continue;
						}
#if 1
						snprintf(pkt, 256, "insert into rtdata (datatime, polid, polname, pol_rtd, flag) "
								"values('%.19s','%s','%s','%s','n');", temptime, psitepols->polid, psitepols->polname, str);

						pthread_mutex_lock(&mutex_db);
						r = db_exec(pkt, NULL, 0);
						if (r != 0) {
							pthread_mutex_unlock(&mutex_db);
							fprintf(stderr, "insert into rtdata table failed!\n");
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
				fprintf(stderr, "comtype parameter error!\n");
				continue;
			}
		} else {
			fprintf(stderr, "devtype parameter error!\n");
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
					fprintf(stderr, "send rtdata to server[%d] failed: %s\n", i, strerror(errno));
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
											fprintf(stderr, "Please check reglen if it equals 1\n");
											continue;
										}	
									} else {
										fprintf(stderr, "Please check datatype if it equals 1\n");
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
											fprintf(stderr, "Please check reglen if it equals 2\n");
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
											fprintf(stderr, "Please check reglen if it equals 1\n");
											continue;
										}

										if ((psbparam->databit < 0) || (psbparam->databit > 15)) {
											fprintf(stderr, "databit parameter should be 0-15\n");
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
										fprintf(stderr, "datatype parameter error\n");
										continue;
									}
									/* read input regsiter */
								} else if (!strncmp(psbparam->funcode, "04", 2)) {

									continue;
								} else {
									fprintf(stderr, "funcode parameter error\n");
									continue;
								}
								
								n = snprintf(temp, 100, "%s-%s=%s,",psbparam->sbid, psbparam->paramname, str);
								temp += n;

							}
						}
					
					} else {
						fprintf(stderr, "sbtype parameter error\n");
						continue;
					}

					astr[strlen(astr) -1] = ';';
					strncat(bstr, astr, strlen(astr));
				}
				
				strncat(cstr, bstr, strlen(bstr));

			} else {
				fprintf(stderr, "comtype parameter error\n");
				continue;
			}	
		} else {
			fprintf(stderr, "devtype parameter error\n");
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
					fprintf(stderr, "send rtdata to server[%d] failed!\n", i);
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
		fprintf(stderr, "get system time failed\n");
		return ;
	}

	/**/
	if (modbus_retry_times > ERROR_TIMES) {
		fprintf(stderr, "too many times read modbus data!\n");
		system("reboot");
	}

	printf("%.19s, sec gather timer fired, signal: %d\n", temptime, v.sival_int);
	
	if (gather_sitepols(datatime, temptime))
		fprintf(stderr, "gather sitepols data failed\n");

	if (gather_sbparam(datatime, temptime))
		fprintf(stderr, "gather sbparam data failed\n");
}

static void timer_thread_min(union sigval v)
{
	int i, j, k, r, n;
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

	r = get_sys_time(datatime, temptime);
	if (r != 0) {
		fprintf(stderr, "get system time failed\n");
		return ;
	}
	
	printf("%.19s, minute gather timer fired, signal: %d\n", temptime, v.sival_int);
	
	sleep(5);

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
						"(julianday(datetime('now')) - "
						"julianday(datetime(datatime)))*24*60*60<=%d and polid='%s';",
						N_INTERNAL_MIN, psitepols->polid);
			
				pthread_mutex_lock(&mutex_db);	
				r = db_get_table(sql, &result, &row, &col);
				if (r != 0) {
					pthread_mutex_unlock(&mutex_db);
					fprintf(stderr, "select rtdata failed!\n");
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
						fprintf(stderr, "error insert into minutedata table failed, at %s: %d\n", __FUNCTION__, __LINE__);
						db_free_table(result);
						continue;
					}
//	
					char qstr[1024];
					memset(qstr, 0, sizeof(qstr));

					snprintf(qstr, sizeof(qstr), "delete from rtdata where "
						"(julianday(datetime('now')) - "
						"julianday(datetime(datatime)))*24*60*60>300 and polid='%s';", psitepols->polid);

					r = db_exec(qstr, NULL, 0);
					if (r != 0) {
						fprintf(stderr, "error delete rtdata table failed, at %s: %d\n", __FUNCTION__, __LINE__);
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
					fprintf(stderr, "send minute data to server[%d] failed!\n", i);
			}
		}
		pthread_mutex_unlock(&mutex);

	}

}

static void timer_thread_hour(union sigval v)
{
	int i, j, k, r, n;
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

	r = get_sys_time(datatime, temptime);
	if (r != 0) {
		fprintf(stderr, "get system time failed\n");
		return ;
	}
	
	printf("%.19s, hourly gather timer fired, signal: %d\n", temptime, v.sival_int);

	sleep(10);

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
						"(julianday(datetime('now')) - "
						"julianday(datetime(datatime)))*24*60*60<=%d and polid='%s';",
						N_INTERNAL_HOUR, psitepols->polid);
			
				pthread_mutex_lock(&mutex_db);	
				r = db_get_table(sql, &result, &row, &col);
				if (r != 0) {
					pthread_mutex_unlock(&mutex_db);
					fprintf(stderr, "select minutedata failed!\n");
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
						fprintf(stderr, "error insert into hourlydata table failed, at %s: %d\n", __FUNCTION__, __LINE__);
						db_free_table(result);
						continue;
					}
//
					char qstr[1024];
					memset(qstr, 0, sizeof(qstr));

					snprintf(qstr, sizeof(qstr), "delete from minutedata where "
						"(julianday(datetime('now')) - "
						"julianday(datetime(datatime)))*24*60*60>3600 and polid='%s';",psitepols->polid);

					r = db_exec(qstr, NULL, 0);
					if (r != 0) {
						fprintf(stderr, "error delete minutedata table failed, at %s: %d\n", __FUNCTION__, __LINE__);
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
					fprintf(stderr, "send hourly data to server[%d] failed!\n", i);
			}
		}
		pthread_mutex_unlock(&mutex);

	}
}

static void timer_thread_day(union sigval v)
{
	
	int i, j, k, r, n;
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

	r = get_sys_time(datatime, temptime);
	if (r != 0) {
		fprintf(stderr, "get system time failed\n");
		return ;
	}

	printf("%.19s, daily gather timer fired, signal: %d\n", temptime, v.sival_int);

	sleep(15);

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
						"(julianday(datetime('now')) - "
						"julianday(datetime(datatime)))*24*60*60<=%d and polid='%s';",
						N_INTERNAL_DAY, psitepols->polid);
			
				pthread_mutex_lock(&mutex_db);	
				r = db_get_table(sql, &result, &row, &col);
				if (r != 0) {
					pthread_mutex_unlock(&mutex_db);
					fprintf(stderr, "select hourlydata failed!\n");
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
						fprintf(stderr, "error insert into dailydata table failed, at %s: %d\n", __FUNCTION__, __LINE__);
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
					fprintf(stderr, "send daily data to server[%d] failed!\n", i);
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
		fprintf(stderr, "gather process send 'hello' to main process failed: %s\n", \
				strerror(errno));
}

static int create_watchdog_timer()
{
	struct sigevent evp;
	struct itimerspec it;

	memset(&evp, 0, sizeof(struct sigevent));
	evp.sigev_value.sival_int = 0;
	evp.sigev_notify = SIGEV_THREAD;
	evp.sigev_notify_function = timer_thread_watchdog;

	if (timer_create(CLOCKID, &evp, &timerid) == -1) {
		perror("gather timer_create");
		return -1;
	}

	/* be sure that data is sended ahead of */
	it.it_interval.tv_sec = WATCHDOG_TIME;
	it.it_interval.tv_nsec = 0;
	it.it_value.tv_sec = WATCHDOG_TIME -2;
	it.it_value.tv_nsec = 0;

	if (timer_settime(timerid, 0, &it, NULL) == -1) {
		perror("gather timer_settime");
		return -1;
	}

	return 0;
}
static int create_gather_timer_sec()
{
	struct sigevent evp;
	struct itimerspec it;

	memset(&evp, 0, sizeof(struct sigevent));
	evp.sigev_value.sival_int = 1;
	evp.sigev_notify = SIGEV_THREAD;
	evp.sigev_notify_function = timer_thread_sec;

	if (timer_create(CLOCKID, &evp, &timerid_sec) == -1) {
		perror("sec timer_create");
		return -1;
	}

	it.it_interval.tv_sec = N_INTERNAL_SEC;
	it.it_interval.tv_nsec = 0;
	it.it_value.tv_sec = N_VALUE_SEC;
	it.it_value.tv_nsec = 0;

	if (timer_settime(timerid_sec, 0, &it, NULL) == -1) {
		perror("sec timer_settime");
		return -1;
	}

	return 0;
}

static int create_gather_timer_min()
{
	struct sigevent evp;
	struct itimerspec it;

	memset(&evp, 0, sizeof(struct sigevent));

	evp.sigev_value.sival_int = 2;
	evp.sigev_notify = SIGEV_THREAD;
	evp.sigev_notify_function = timer_thread_min;

	if (timer_create(CLOCKID, &evp, &timerid_min) == -1) {
		perror("min timer_create");
		return -1;
	}

	it.it_interval.tv_sec = N_INTERNAL_MIN;
	it.it_interval.tv_nsec = 0;
	it.it_value.tv_sec = N_VALUE_MIN;
	it.it_value.tv_nsec = 0;

	if (timer_settime(timerid_min, 0, &it, NULL) == -1) {
		perror("min timer_settime(1min)");
		return -1;
	}

	return 0;
}

static int create_gather_timer_hour()
{
	struct sigevent evp;
	struct itimerspec it;
	memset(&evp, 0, sizeof(struct sigevent));

	evp.sigev_value.sival_int = 4;
	evp.sigev_notify = SIGEV_THREAD;
	evp.sigev_notify_function = timer_thread_hour;

	if (timer_create(CLOCKID, &evp, &timerid_hour) == -1) {
		perror("hour timer_create_hour");
		return -1;
	}

	it.it_interval.tv_sec = N_INTERNAL_HOUR;
	it.it_interval.tv_nsec = 0;
	it.it_value.tv_sec = N_VALUE_HOUR;
	it.it_value.tv_nsec = 0;

	if (timer_settime(timerid_hour, 0, &it, NULL) == -1) {
		perror("hour timer_settime");
		return -1;
	}	

	return 0;
}

static int create_gather_timer_day()
{
	struct sigevent evp;
	struct itimerspec it;
	memset(&evp, 0, sizeof(struct sigevent));

	evp.sigev_value.sival_int = 8;
	evp.sigev_notify = SIGEV_THREAD;
	evp.sigev_notify_function = timer_thread_day;

	if (timer_create(CLOCKID, &evp, &timerid_day) == -1) {
		perror("day timer_create_day");
		return -1;
	}

	it.it_interval.tv_sec = N_INTERNAL_DAY;
	it.it_interval.tv_nsec = 0;
	it.it_value.tv_sec = N_VALUE_DAY;
	it.it_value.tv_nsec = 0;

	if (timer_settime(timerid_day, 0, &it, NULL) == -1) {
		perror("day timer_settime");
		return -1;
	}

	return 0;
}

static int delete_watchdog_timer(void)
{
	int r;

	r = timer_delete(timerid);
	if (r == -1) {
		fprintf(stderr, "watchdog timer_delete: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int delete_sec_timer(void)
{
	int r;

	r = timer_delete(timerid_sec);
	if (r == -1) {
		fprintf(stderr, "sec timer_delete: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int delete_min_timer(void)
{
	int r;

	r = timer_delete(timerid_min);
	if (r == -1) {
		fprintf(stderr, "min timer_delete: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int delete_hour_timer(void)
{
	int r;

	r = timer_delete(timerid_hour);
	if (r == -1) {
		fprintf(stderr, "hour timer_delete: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int delete_day_timer(void)
{
	int r;
	
	r = timer_delete(timerid_day);
	if (r == -1) {
		fprintf(stderr, "day timer_delete: %s\n", strerror(errno));
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
		set_analog_value(mq[flag], pkt_r, qn);
		printf("receive comand : %s\n", cn);
	}
}

static void *receiver_routine(void *arg)
{
	int r;
	int i;
	char buf[msgsize];

	if (!p_server)
		return NULL;

	while (1) {	
		for (i = 1; i < (servernum + 1); i++) {
			memset(buf, 0, sizeof(buf));
			r = mq_receive(mqtest[i], buf, msgsize, NULL);
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
					perror("mq_receive");
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
		fprintf(stderr, "get system time failed\n");
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
			fprintf(stderr, "select polsalarm table failed\n");
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
					fprintf(stderr, "send alarm type = 0 failed\n");
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
					fprintf(stderr, "select alarm table failed\n");
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
						fprintf(stderr, "send alarm data to server[%d] failed!\n", i);
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
						fprintf(stderr, "error: %s-%d funcode parameter error\n", __FUNCTION__, __LINE__);
						continue;
					}
					
					/* funcode = 0x03H */
					if (ppolsalarm->datatype == 2) {
						continue;
					} else if (ppolsalarm->datatype == 3) {
						continue;
					} else if (ppolsalarm->datatype != 4) {
						fprintf(stderr, "error: %s-%d datatype parameter error\n", __FUNCTION__, __LINE__);
						continue;
					}
					
					/* datatype = 4 */
					int ret = 0;
					uint16_t dest[24];
					uint16_t low_bit, high_bit, value;

					if (ppolsalarm->reglen != 1) {
						fprintf(stderr, "Please check reglen if it equals 1\n");
						continue;
					}

					if ((ppolsalarm->databit < 0) || (ppolsalarm->databit > 15)) {
						fprintf(stderr, "Bit parameter should be 0-15\n");
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
							fprintf(stderr, "get system time failed\n");
							continue;
						}

						psitepols = p_sitepols->info;
						for (k = 0; k < p_sitepols->num; k++, psitepols++) {
							if (!strcmp(psitepols->polid, ppolsalarm->polid))
								break;
						}

						if (k == p_sitepols->num) {
							fprintf(stderr, "Can not find the polid in sitepols table\n");
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
							fprintf(stderr, "failed: %s\n", str);
						}
						pthread_mutex_unlock(&mutex_db);
					}

					if (ret == 0) {
						if (ppolsalarm->flag == 1) {
							char str[512];
							memset(str, 0, sizeof(str));
							
							r = get_sys_time(datatime, NULL);
							if (r != 0) {
								fprintf(stderr, "get system time failed\n");
								continue;
							}

							psitepols = p_sitepols->info;
							for (k = 0; k < p_sitepols->num; k++, psitepols++) {
								if (!strcmp(psitepols->polid, ppolsalarm->polid))
									break;
							}

							if (k == p_sitepols->num) {
								fprintf(stderr, "Can not find the polid in sitepols table\n");
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
								fprintf(stderr, "failed: %s\n", str);
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
	get_siteconf_settings();
	get_server_settings();
	get_devices_settings();
	get_sitesbs_settings();
	get_pols_settings();
	get_sitepols_settings();
	get_sbparam_settings();
	
	/* new add */
	get_polsalarm_settings();
	get_ctlparam_settings();
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

	/* new add */
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

int main(int argc, char *argv[])
{
	int i, r;	
	struct tm *ptm;
	struct timeval tv;
	struct mq_attr attr;
	
	if ((argc != 1) || (!argv[0])) {
		fprintf(stderr, "gather paremeter error!\n");
		return 1;
	}

	db_open(FILENAME);

	db_table_init();

	modbus_device_init();

	set_block_signal();

	/* watchdog mqueue */
	mq[0] = mq_open(argv[0], O_RDWR);
	if (mq[0] == ((mqd_t) -1)) {
		fprintf(stderr, "open gather process watchdog mqueue failed: %s\n", strerror(errno));
		return 2;		
	}
	
	mq_getattr(mq[0], &attr);
	msgsize = attr.mq_msgsize;
	
	if (p_server)
		servernum = MIN_V(p_server->num, MAXSERVER);
		
	if (p_devices)
		devicesnum = MIN_V(p_devices->num, MAXDEVS);
	
	/* client mqueue */
	for (i = 1; i < (servernum + 1); i++) {
		mq[i] = mq_open(client_mqfile[i], O_CREAT | O_RDWR, 0777, NULL);
		if (mq[i] == ((mqd_t) -1)) {
			fprintf(stderr, "create client[%d] process mqueue failed: %s\n", i, strerror(errno));
			return 2;
		}
	}

	/* gather and client communication */
	for (i = 1; i < (servernum + 1); i++) {
		mqtest[i] = mq_open(mqfile[i], O_CREAT | O_RDWR | O_NONBLOCK, 0777, NULL);
		if (mqtest[i] == ((mqd_t) -1)) {
			fprintf(stderr, "create client[%d] process and gather process communication mqueue failed: %s\n", \
					i, strerror(errno));
			return 2;
		}
	}

	create_watchdog_timer();
	
	set_sigrtmin1_handler();
	set_sigrtmin2_handler();

	pthread_mutex_init(&mutex, NULL);
	pthread_mutex_init(&mutex_db, NULL);
	pthread_mutex_init(&mutex_ctx, NULL);

	/* client first runs, be sure the system time correct */
	sleep(5);

	r = gettimeofday(&tv, NULL);
	if (r == -1) {
		fprintf(stderr, "gettimeofday: %s\n", strerror(errno));
		return 2;
	}

	ptm = localtime(&(tv.tv_sec));

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

	r = create_gather_timer_sec();
	if (r != 0) {
		fprintf(stderr, "create gather timer sec failed\n");
		return 3;
	}

	r = create_gather_timer_min();
	if (r != 0) {
		fprintf(stderr, "create gather timer min failed\n");
		return 3;
	}
	
	r = create_gather_timer_hour();
	if (r != 0) {
		fprintf(stderr, "create gather timer hour failed\n");
		return 3;
	}
	
	r = create_gather_timer_day();
	if (r != 0) {
		fprintf(stderr, "create gather timer day failed\n");
		return 3;
	}

	r = pthread_create(&thr_receiver,  NULL, receiver_routine,  NULL);
	if (r != 0) {
	       fprintf(stderr, "create receiver routine failed: %s\n", strerror(errno));
		return 3;
	}

	r = pthread_create(&thr_recvalarm, NULL, recvalarm_routine, NULL);
	if (r != 0) {
		fprintf(stderr, "create recvalarm routine failed: %s\n", strerror(errno));
		return 3;
	}

	r = pthread_create(&thr_sendalarm, NULL, sendalarm_routine, NULL);
	if (r != 0) {
		fprintf(stderr, "create sendalarm routine failed: %s\n", strerror(errno));
		return 3;
	}

	r = pthread_join(thr_receiver,  NULL);
	if (r != 0) {
		fprintf(stderr, "join receiver routine failed: %s\n", strerror(errno));
		return 4;
	}

	r = pthread_join(thr_recvalarm, NULL);
	if (r != 0) {
		fprintf(stderr, "join recvalarm routine failed: %s\n", strerror(errno));
		return 4;
	}

	r = pthread_join(thr_sendalarm, NULL);
	if (r != 0) {
		fprintf(stderr, "join sendalarm routine failed: %s\n", strerror(errno));
		return 4;
	}

	return 0;
}

