/* FreeTDS - Library of routines accessing Sybase and Microsoft databases
 * Copyright (C) 1998, 1999, 2000, 2001, 2002, 2003, 2004, 2005  Brian Bruns
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef _tdsguard_aoIoAtf9UfNLxwNuH19vDb_
#define _tdsguard_aoIoAtf9UfNLxwNuH19vDb_

#include <freetds/pushvis.h>

#ifdef __cplusplus
extern "C"
{
#if 0
}
#endif
#endif

typedef enum
{
	  _DB_RES_INIT
	, _DB_RES_RESULTSET_EMPTY
	, _DB_RES_RESULTSET_ROWS
	, _DB_RES_NEXT_RESULT
	, _DB_RES_NO_MORE_RESULTS
	, _DB_RES_SUCCEED
} DB_RESULT_STATE;

struct tds_dblib_loginrec
{
	TDSLOGIN *tds_login;
	bool network_auth;
};

struct dblib_buffer_row;

typedef struct
{
	int received;	  	/* how many rows have been received for this result set */
	int head;	  	/* queue insertion point */
	int tail;	  	/* oldest item in queue	*/
	int current;		/* dbnextrow() reads this row */
	int capacity;		/* how many elements the queue can hold  */
	struct dblib_buffer_row *rows;		/* pointer to the row storage */
} DBPROC_ROWBUF;

typedef struct
{
	int host_column;
	TDS_SERVER_TYPE datatype;
	int prefix_len;
	DBINT column_len;
	BYTE *terminator;
	int term_len;
	int tab_colnum;
	int column_error;
} BCP_HOSTCOLINFO;

typedef struct
{
	TDS_CHAR *hostfile;
	TDS_CHAR *errorfile;
	FILE *bcp_errfileptr;
	TDS_INT host_colcount;
	BCP_HOSTCOLINFO **host_columns;
	TDS_INT firstrow;
	TDS_INT lastrow;
	TDS_INT maxerrs;
	TDS_INT batch;
} BCP_HOSTFILEINFO;

/* linked list of rpc parameters */

typedef struct DBREMOTE_PROC_PARAM
{
	struct DBREMOTE_PROC_PARAM *next;

	char *name;
	BYTE status;
	TDS_SERVER_TYPE type;
	DBINT maxlen;
	DBINT datalen;
	BYTE *value;
} DBREMOTE_PROC_PARAM;

typedef struct DBREMOTE_PROC
{
	struct DBREMOTE_PROC *next;

	char *name;
	DBSMALLINT options;
	DBREMOTE_PROC_PARAM *param_list;
} DBREMOTE_PROC;

typedef struct dboption
{
	const char *text;
	DBSTRING *param;
	DBBOOL factive;
} DBOPTION;

typedef struct
{
	const BYTE *bindval;
	unsigned int len;
} NULLREP;

struct tds_dblib_dbprocess
{
	TDSSOCKET *tds_socket;

	STATUS row_type;
	DBPROC_ROWBUF row_buf;

	int noautofree;
	int more_results;	/* boolean.  Are we expecting results? */
	DB_RESULT_STATE dbresults_state;
	int dbresults_retcode;
	BYTE *user_data;	/* see dbsetuserdata() and dbgetuserdata() */
	unsigned char *dbbuf;	/* is dynamic!                   */
	int dbbufsz;
	int command_state;
	TDS_INT text_size;
	TDS_INT text_sent;
	DBTYPEINFO typeinfo;
	unsigned char avail_flag;
	DBOPTION *dbopts;
	DBSTRING *dboptcmd;
	BCP_HOSTFILEINFO *hostfileinfo;
	TDSBCPINFO *bcpinfo;
	DBREMOTE_PROC *rpc;
	DBUSMALLINT envchange_rcv;
	char dbcurdb[DBMAXNAME + 1];
	char servcharset[DBMAXNAME + 1];
	FILE *ftos;
	DB_DBCHKINTR_FUNC chkintr;
	DB_DBHNDLINTR_FUNC hndlintr;

	/** boolean use ms behaviour */
	int msdblib;

	int ntimeouts;

	/** default null values **/
	NULLREP		nullreps[MAXBINDTYPES];
};

enum {
#if MSDBLIB
	dblib_msdblib = 1
#else
	dblib_msdblib = 0
#endif
};

/*
 * internal prototypes
 */
RETCODE dbgetnull(DBPROCESS *dbproc, int bindtype, int varlen, BYTE* varaddr);
void copy_data_to_host_var(DBPROCESS * dbproc, TDS_SERVER_TYPE srctype, const BYTE * src, DBINT srclen,
			   BYTE * dest, DBINT destlen,
			   int bindtype, DBINT *indicator);

int dbperror (DBPROCESS *dbproc, DBINT msgno, long errnum, ...);
int _dblib_handle_info_message(const TDSCONTEXT * ctxptr, TDSSOCKET * tdsptr, TDSMESSAGE* msgptr);
int _dblib_handle_err_message(const TDSCONTEXT * ctxptr, TDSSOCKET * tdsptr, TDSMESSAGE* msgptr);
int _dblib_check_and_handle_interrupt(void * vdbproc);

void _dblib_setTDS_version(TDSLOGIN * tds_login, DBINT version);
void _dblib_convert_err(DBPROCESS * dbproc, TDS_INT len);

DBINT _convert_char(int srctype, BYTE * src, int destype, BYTE * dest, DBINT destlen);
DBINT _convert_intn(int srctype, BYTE * src, int destype, BYTE * dest, DBINT destlen);

RETCODE _bcp_clear_storage(DBPROCESS * dbproc);
RETCODE _bcp_get_prog_data(DBPROCESS * dbproc);

extern MHANDLEFUNC _dblib_msg_handler;
extern EHANDLEFUNC _dblib_err_handler;

#define CHECK_PARAMETER(x, msg, ret)	if (!(x)) { dbperror(dbproc, (msg), 0); return ret; }
#define CHECK_NULP(x, func, param_num, ret)	if (!(x)) { dbperror(dbproc, SYBENULP, 0, func, (int) param_num); return ret; }
#define CHECK_PARAMETER_NOPROC(x, msg)	if (!(x)) { dbperror(NULL, (msg), 0); return FAIL; }
#define DBPERROR_RETURN(x, msg)	if (x) { dbperror(dbproc, (msg), 0); return FAIL; }
#define DBPERROR_RETURN3(x, msg, a, b, c)	if (x) { dbperror(dbproc, (msg), 0, a, b, c); return FAIL; }
#define CHECK_CONN(ret) do { CHECK_PARAMETER(dbproc, SYBENULL, (ret)); \
	if (IS_TDSDEAD(dbproc->tds_socket)) { dbperror(dbproc, SYBEDDNE, 0); return (ret); } } while(0)


#ifdef __cplusplus
#if 0
{
#endif
}
#endif

#include <freetds/popvis.h>

#endif
