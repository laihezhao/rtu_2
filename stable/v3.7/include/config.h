/* config file */

/* heartbeat time */
#define HEARTBEAT_TIME	(120)

/* database file */
#define FILENAME	"./site.db"

/* support the server numbers */
#define MAXSERVER	(6)

/* watchdog time */
#define WATCHDOG_TIME	(120)

/* timer type */
#define CLOCKID CLOCK_REALTIME

/* history data resend time */
#define RETRY_TIME_MIN   (60)
#define RETRY_TIME_HOUR  (60)
#define RETRY_TIME_DAY   (60)

/* history data resend count */
#define RETRY_COUNT_MIN	 (3)
#define RETRY_COUNT_HOUR (3)
#define RETRY_COUNT_DAY  (3)

/* support the device numbers */
#define MAXDEVS     	(32)

/* read modbus data failed waiting for DELAY and retry */
#define DELAY	(1)

/* modbus retry times */
#define RETRY_TIMES	(1)

/* the time of gathering data */
#define N_INTERNAL_SEC  (10)
#define	N_INTERNAL_MIN 	(60 * 5)
#define N_INTERNAL_HOUR	(60 * 60)
#define N_INTERNAL_DAY	(60 * 60 * 24)

/**/
#define MAX_V(a,b)	(((a) >= (b)) ? (a) : (b))
#define MIN_V(a,b)	(((a) <= (b)) ? (a) : (b))

/**/
#define APPROACH(a,b)   (((((a) + ((b) - 1)) / (b)) * (b)) - (a))
