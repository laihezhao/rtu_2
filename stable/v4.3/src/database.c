#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sqlite3.h>
#include <pthread.h>
#include <unistd.h>

#define __DBG__

static sqlite3 *db;

int db_open(const char *name)
{
	int r;
	int retry = 0;

	if (name == NULL) {
		fprintf(stderr, "error: database's name can't be NULL\n");
		exit(-1);
	}

	while (1) {
		retry++;
		if (retry > 10) {
			fprintf(stderr, "open database failed\n");
			exit(-1);
		}

		r = sqlite3_open(name, &db);
		if (r) {
			sleep(1);
			continue;
		} else {
			printf("open %s database ok\n", name);
			break;
		}
	}

	return r;
}

int db_exec(const char *sql,
	    int (*callback) (void *, int, char **, char **), void *arg)
{
	int r;
	char *zErr;

	if (!sql) {
		fprintf(stderr, "sql can't be NULL\n");
		return -1;
	}

	r = sqlite3_exec(db, sql, callback, arg, &zErr);
	if (r != SQLITE_OK) {
		if (zErr != NULL) {
			fprintf(stderr, "sqlite3_exec: %s\n", zErr);
			sqlite3_free(zErr);
			return -1;
		}
	}
	
	return 0;
}

int db_get_table(const char *zSql, char ***pazResult, int *pnRow, int *pnColumn)
{
	int r;
	char *zErr;

	r = sqlite3_get_table(db, zSql, pazResult, pnRow, pnColumn, &zErr);
	if (r != SQLITE_OK) {
		fprintf(stderr, "sqlite3_get_table: %s\n", zErr);
		sqlite3_free(zErr);
		sqlite3_free_table(*pazResult);
		return -1;
	}
	
	return 0;
}

void db_free_table(char **result)
{
	sqlite3_free_table(result);
}

void db_close(void)
{
	sqlite3_close(db);
}
