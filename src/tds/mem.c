/* FreeTDS - Library of routines accessing Sybase and Microsoft databases
 * Copyright (C) 1998, 1999, 2000, 2001, 2002, 2003, 2004, 2005  Brian Bruns
 * Copyright (C) 2005-2015 Frediano Ziglio
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

#include <config.h>

#include <stdlib.h>

#if HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */

#include <assert.h>

#include <freetds/tds.h>
#include <freetds/iconv.h>
#include <freetds/tls.h>
#include <freetds/checks.h>
#include <freetds/utils/string.h>
#include <freetds/replacements.h>
#include <freetds/enum_cap.h>
#include <freetds/utils.h>

#if HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_LOCALE_H
#include <locale.h>
#endif /* HAVE_LOCALE_H */

#ifdef HAVE_LANGINFO_H
#include <langinfo.h>
#endif /* HAVE_LANGINFO_H */

static void tds_free_env(TDSCONNECTION * conn);
static void tds_free_compute_results(TDSSOCKET * tds);
static void tds_free_compute_result(TDSCOMPUTEINFO * comp_info);

#undef TEST_MALLOC
#define TEST_MALLOC(dest,type) \
	{if (!(dest = (type*)calloc(1, sizeof(type)))) goto Cleanup;}

#undef TEST_CALLOC
#define TEST_CALLOC(dest,type,n) \
	{if (!(dest = (type*)calloc((n), sizeof(type)))) goto Cleanup;}

/**
 * \ingroup libtds
 * \defgroup mem Memory allocation
 * Allocate or free resources. Allocation can fail only on out of memory. 
 * In such case they return NULL and leave the state as before call.
 * Mainly function names are in the form tds_alloc_XX or tds_free_XXX.
 * tds_alloc_XXX functions allocate structures and return pointer to allocated
 * data while tds_free_XXX take structure pointers and free them. Some functions
 * require additional parameters to initialize structure correctly.
 * The main exception are structures that use reference counting. These structures
 * have tds_alloc_XXX functions but instead of tds_free_XXX use tds_release_XXX.
 */

/**
 * \addtogroup mem
 * @{
 */

static volatile unsigned int inc_num = 1;

/**
 * Get an id for dynamic query based on TDS information
 * \param conn state information for the connection and the TDS protocol
 * \return TDS_FAIL or TDS_SUCCESS
 */
static char *
tds_get_dynid(TDSCONNECTION * conn, char *id)
{
	unsigned long n;
	int i;
	char *p;
	char c;

	CHECK_CONN_EXTRA(conn);

	inc_num = (inc_num + 1) & 0xffff;
	/* some version of Sybase require length <= 10, so we code id */
	n = (unsigned long) (TDS_INTPTR) conn;
	p = id;
	*p++ = (char) ('a' + (n % 26u));
	n /= 26u;
	for (i = 0; i < 9; ++i) {
		c = (char) ('0' + (n % 36u));
		*p++ = (c < ('0' + 10)) ? c : c + ('a' - '0' - 10);
		/* tdsdump_log(TDS_DBG_INFO1, "%d -> %d(%c)\n", (int) (n%36u),p[-1],p[-1]); */
		n /= 36u;
		if (i == 4)
			n += 3u * inc_num;
	}
	*p = 0;
	return id;
}

#include <freetds/pushvis.h>
extern const TDSCOLUMNFUNCS tds_invalid_funcs;
#include <freetds/popvis.h>

static TDSCOLUMN *
tds_alloc_column(void)
{
	TDSCOLUMN *col;

	TEST_MALLOC(col, TDSCOLUMN);
	tds_dstr_init(&col->table_name);
	tds_dstr_init(&col->column_name);
	tds_dstr_init(&col->table_column_name);
	col->funcs = &tds_invalid_funcs;

      Cleanup:
	return col;
}

static void
tds_free_column(TDSCOLUMN *col)
{
	tds_dstr_free(&col->table_name);
	tds_dstr_free(&col->column_name);
	tds_dstr_free(&col->table_column_name);
	free(col);
}


/**
 * \fn TDSDYNAMIC *tds_alloc_dynamic(TDSCONNECTION *conn, const char *id)
 * \brief Allocate a dynamic statement.
 * \param conn the connection within which to allocate the statement.
 * \param id a character label identifying the statement.
 * \return a pointer to the allocated structure (NULL on failure).
 *
 * tds_alloc_dynamic is used to implement placeholder code under TDS 5.0
 */
TDSDYNAMIC *
tds_alloc_dynamic(TDSCONNECTION * conn, const char *id)
{
	TDSDYNAMIC *dyn;
	char tmp_id[30];

	if (id) {
		/* check to see if id already exists (shouldn't) */
		if (tds_lookup_dynamic(conn, id))
			return NULL;
	} else {
		unsigned int n;
		id = tmp_id;

		for (n = 0;;) {
			if (!tds_lookup_dynamic(conn, tds_get_dynid(conn, tmp_id)))
				break;
			if (++n == 256)
				return NULL;
		}
	}

	TEST_MALLOC(dyn, TDSDYNAMIC);

	/* take into account pointer in list */
	dyn->ref_count = 2;

	/* insert into list */
	dyn->next = conn->dyns;
	conn->dyns = dyn;

	strlcpy(dyn->id, id, TDS_MAX_DYNID_LEN);

	return dyn;

      Cleanup:
	return NULL;
}

/**
 * \fn void tds_free_input_params(TDSDYNAMIC *dyn)
 * \brief Frees all allocated input parameters of a dynamic statement.
 * \param dyn the dynamic statement whose input parameter are to be freed
 *
 * tds_free_input_params frees all parameters for the give dynamic statement
 */
void
tds_free_input_params(TDSDYNAMIC * dyn)
{
	TDSPARAMINFO *info;

	info = dyn->params;
	if (info) {
		tds_free_param_results(info);
		dyn->params = NULL;
	}
}

/*
 * Called when dynamic got deallocated from server
 */
void
tds_dynamic_deallocated(TDSCONNECTION *conn, TDSDYNAMIC *dyn)
{
	TDSDYNAMIC **victim;

	tdsdump_log(TDS_DBG_FUNC, "tds_dynamic_deallocated() : freeing dynamic_id %s\n", dyn->id);

	victim = &conn->dyns;
	while (*victim != dyn) {
		if (*victim == NULL) {
			tdsdump_log(TDS_DBG_FUNC, "tds_dynamic_deallocated() : cannot find id %s\n", dyn->id);
			return;
		}
		victim = &(*victim)->next;
	}

	/* remove from list */
	*victim = dyn->next;
	dyn->next = NULL;

	/* assure there is no id left */
	dyn->num_id = 0;

	tds_release_dynamic(&dyn);
}


/**
 * \fn void tds_release_dynamic(TDSDYNAMIC **pdyn)
 * \brief Frees dynamic statement
 * \param pdyn pointer to dynamic statement to be freed.
 */
void
tds_release_dynamic(TDSDYNAMIC ** pdyn)
{
	TDSDYNAMIC *dyn;

	dyn = *pdyn;
	*pdyn = NULL;
	if (!dyn || --dyn->ref_count > 0)
		return;

	tds_detach_results(dyn->res_info);

	tds_free_results(dyn->res_info);
	tds_free_input_params(dyn);
	free(dyn->query);
	free(dyn);
}

/**
 * \fn TDSPARAMINFO *tds_alloc_param_result(TDSPARAMINFO *old_param)
 * \brief Adds a output parameter to TDSPARAMINFO.
 * \param old_param a pointer to the TDSPARAMINFO structure containing the 
 * current set of output parameter, or NULL if none exists.
 * \return a pointer to the new TDSPARAMINFO structure.
 *
 * tds_alloc_param_result() works a bit differently than the other alloc result
 * functions.  Output parameters come in individually with no total number 
 * given in advance, so we simply call this func every time with get a
 * TDS_PARAM_TOKEN and let it realloc the columns struct one bigger. 
 * tds_free_all_results() usually cleans up after us.
 */
TDSPARAMINFO *
tds_alloc_param_result(TDSPARAMINFO * old_param)
{
	TDSPARAMINFO *param_info;
	TDSCOLUMN *colinfo;

	/* parameters cannot have row associated */
	if (old_param && (old_param->current_row || old_param->row_free))
		return NULL;

	colinfo = tds_alloc_column();
	if (!colinfo)
		return NULL;

	param_info = old_param;
	if (!param_info) {
		TEST_MALLOC(param_info, TDSPARAMINFO);
		param_info->ref_count = 1;
	}

	if (!TDS_RESIZE(param_info->columns, param_info->num_cols + 1u))
		goto Cleanup;

	param_info->columns[param_info->num_cols++] = colinfo;
	return param_info;

      Cleanup:
	if (!old_param)
		free(param_info);
	free(colinfo);
	return NULL;
}

/**
 * Delete latest parameter
 */
void
tds_free_param_result(TDSPARAMINFO * param_info)
{
	TDSCOLUMN *col;

	if (!param_info || param_info->num_cols <= 0)
		return;

	col = param_info->columns[--param_info->num_cols];
	if (col->column_data && col->column_data_free)
		col->column_data_free(col);

	if (param_info->num_cols == 0)
		TDS_ZERO_FREE(param_info->columns);

	/*
	 * NOTE some information should be freed too but when this function
	 * is called are not used. I hope to remove the need for this
	 * function ASAP
	 * A better way is to support different way to allocate and get
	 * parameters
	 * -- freddy77
	 */
	tds_free_column(col);
}

static void
tds_free_tvp_row(TDS_TVP_ROW *row)
{
	tds_free_param_results(row->params);
}

static void
tds_param_free(TDSCOLUMN *col)
{
	if (!col->column_data)
		return;

	if (col->column_type == SYBMSTABLE) {
		tds_deinit_tvp((TDS_TVP *) col->column_data);
	} else if (is_blob_col(col)) {
		TDSBLOB *blob = (TDSBLOB *) col->column_data;
		free(blob->textvalue);
	}

	TDS_ZERO_FREE(col->column_data);
}

/**
 * Allocate data for a parameter.
 * @param curparam parameter to retrieve size information
 * @return NULL on failure or new data
 */
void *
tds_alloc_param_data(TDSCOLUMN * curparam)
{
	TDS_INT data_size;
	void *data;

	CHECK_COLUMN_EXTRA(curparam);

	data_size = curparam->funcs->row_len(curparam);

	/* allocate data */
	if (curparam->column_data && curparam->column_data_free)
		curparam->column_data_free(curparam);
	curparam->column_data_free = tds_param_free;

	data = malloc(data_size);
	curparam->column_data = (unsigned char*) data;
	if (!data)
		return NULL;
	/* if is a blob reset buffer */
	if (is_blob_col(curparam) || curparam->column_type == SYBMSTABLE)
		memset(data, 0, data_size);

	return data;
}

/**
 * Allocate memory for storing compute info
 * return NULL on out of memory
 */

static TDSCOMPUTEINFO *
tds_alloc_compute_result(TDS_USMALLINT num_cols, TDS_USMALLINT by_cols)
{
	TDS_USMALLINT col;
	TDSCOMPUTEINFO *info;

	TEST_MALLOC(info, TDSCOMPUTEINFO);
	info->ref_count = 1;

	TEST_CALLOC(info->columns, TDSCOLUMN *, num_cols);

	info->num_cols = num_cols;
	for (col = 0; col < num_cols; col++)
		if (!(info->columns[col] = tds_alloc_column()))
			goto Cleanup;

	if (by_cols) {
		TEST_CALLOC(info->bycolumns, TDS_SMALLINT, by_cols);
		info->by_cols = by_cols;
	}

	return info;
      Cleanup:
	tds_free_compute_result(info);
	return NULL;
}

TDSCOMPUTEINFO **
tds_alloc_compute_results(TDSSOCKET * tds, TDS_USMALLINT num_cols, TDS_USMALLINT by_cols)
{
	TDS_UINT n;
	TDSCOMPUTEINFO **comp_info;
	TDSCOMPUTEINFO *cur_comp_info;

	tdsdump_log(TDS_DBG_FUNC, "alloc_compute_result. num_cols = %d bycols = %d\n", num_cols, by_cols);
	tdsdump_log(TDS_DBG_FUNC, "alloc_compute_result. num_comp_info = %d\n", tds->num_comp_info);

	cur_comp_info = tds_alloc_compute_result(num_cols, by_cols);
	if (!cur_comp_info)
		return NULL;

	n = tds->num_comp_info;
	comp_info = (TDSCOMPUTEINFO **) TDS_RESIZE(tds->comp_info, n + 1u);
	if (!comp_info) {
		tds_free_compute_result(cur_comp_info);
		return NULL;
	}

	tds->comp_info = comp_info;
	comp_info[n] = cur_comp_info;
	tds->num_comp_info = n + 1u;

	tdsdump_log(TDS_DBG_FUNC, "alloc_compute_result. num_comp_info = %d\n", tds->num_comp_info);

	return comp_info;
}

TDSRESULTINFO *
tds_alloc_results(TDS_USMALLINT num_cols)
{
	TDSRESULTINFO *res_info;
	TDS_USMALLINT col;

	TEST_MALLOC(res_info, TDSRESULTINFO);
	res_info->ref_count = 1;
	if (num_cols)
		TEST_CALLOC(res_info->columns, TDSCOLUMN *, num_cols);
	for (col = 0; col < num_cols; col++)
		if (!(res_info->columns[col] = tds_alloc_column()))
			goto Cleanup;
	res_info->num_cols = num_cols;
	res_info->row_size = 0;
	return res_info;
      Cleanup:
	tds_free_results(res_info);
	return NULL;
}

void
tds_set_current_results(TDSSOCKET *tds, TDSRESULTINFO *info)
{
	tds_detach_results(info);
	if (tds->current_results)
		tds->current_results->attached_to = NULL;
	if (info)
		info->attached_to = tds;
	tds->in_row = (info != NULL);
	tds->current_results = info;
}

/**
 * Detach result info from it current socket
 */
void
tds_detach_results(TDSRESULTINFO *info)
{
	if (info && info->attached_to) {
		info->attached_to->current_results = NULL;
		info->attached_to->in_row = false;
		info->attached_to = NULL;
	}
}

static void
tds_row_free(TDSRESULTINFO *res_info, unsigned char *row)
{
	int i;
	const TDSCOLUMN *col;

	if (!res_info || !row)
		return;

	for (i = 0; i < res_info->num_cols; ++i) {
		col = res_info->columns[i];
		
		if (is_blob_col(col)) {
			TDSBLOB *blob = (TDSBLOB *) &row[col->column_data - res_info->current_row];
			if (blob->textvalue)
				TDS_ZERO_FREE(blob->textvalue);
		}
	}

	free(row);
}

/**
 * Allocate space for row store
 * return NULL on out of memory
 */
TDSRET
tds_alloc_row(TDSRESULTINFO * res_info)
{
	int i, num_cols = res_info->num_cols;
	unsigned char *ptr;
	TDSCOLUMN *col;
	TDS_UINT row_size;

	/* compute row size */
	row_size = 0;
	for (i = 0; i < num_cols; ++i) {
		col = res_info->columns[i];

		col->column_data_free = NULL;

		row_size += col->funcs->row_len(col);
		row_size += (TDS_ALIGN_SIZE - 1);
		row_size -= row_size % TDS_ALIGN_SIZE;
	}
	res_info->row_size = row_size;

	ptr = tds_new0(unsigned char, row_size ? row_size : 1);
	res_info->current_row = ptr;
	if (!ptr)
		return TDS_FAIL;
	res_info->row_free = tds_row_free;

	/* fill column_data */
	row_size = 0;
	for (i = 0; i < num_cols; ++i) {
		col = res_info->columns[i];

		col->column_data = ptr + row_size;

		row_size += col->funcs->row_len(col);
		row_size += (TDS_ALIGN_SIZE - 1);
		row_size -= row_size % TDS_ALIGN_SIZE;
	}

	return TDS_SUCCESS;
}

TDSRET
tds_alloc_compute_row(TDSCOMPUTEINFO * res_info)
{
	return tds_alloc_row(res_info);
}

void
tds_free_param_results(TDSPARAMINFO * param_info)
{
	tds_free_results(param_info);
}

static void
tds_free_compute_result(TDSCOMPUTEINFO * comp_info)
{
	tds_free_results(comp_info);
}

static void
tds_free_compute_results(TDSSOCKET * tds)
{
	TDSCOMPUTEINFO ** comp_info = tds->comp_info;
	TDS_UINT i, num_comp = tds->num_comp_info;

	tds->comp_info = NULL;
	tds->num_comp_info = 0;

	for (i = 0; i < num_comp; i++) {
		if (comp_info && comp_info[i]) {
			tds_detach_results(comp_info[i]);
			tds_free_compute_result(comp_info[i]);
		}
	}
	if (num_comp)
		free(comp_info);
}

void
tds_free_row(TDSRESULTINFO * res_info, unsigned char *row)
{
	assert(res_info);
	if (!row || !res_info->row_free)
		return;

	res_info->row_free(res_info, row);
}

void
tds_free_results(TDSRESULTINFO * res_info)
{
	int i;
	TDSCOLUMN *curcol;

	if (!res_info)
		return;

	if (--res_info->ref_count != 0)
		return;

	tds_detach_results(res_info);

	if (res_info->num_cols && res_info->columns) {
		for (i = 0; i < res_info->num_cols; i++)
			if ((curcol = res_info->columns[i]) != NULL) {
				if (curcol->bcp_terminator)
					TDS_ZERO_FREE(curcol->bcp_terminator);
				tds_free_bcp_column_data(curcol->bcp_column_data);
				curcol->bcp_column_data = NULL;
				if (curcol->column_data && curcol->column_data_free)
					curcol->column_data_free(curcol);
			}
	}

	if (res_info->current_row && res_info->row_free)
		res_info->row_free(res_info, res_info->current_row);

	if (res_info->num_cols && res_info->columns) {
		for (i = 0; i < res_info->num_cols; i++)
			if ((curcol = res_info->columns[i]) != NULL)
				tds_free_column(curcol);
		free(res_info->columns);
	}

	free(res_info->bycolumns);

	free(res_info);
}

void
tds_free_all_results(TDSSOCKET * tds)
{
	tdsdump_log(TDS_DBG_FUNC, "tds_free_all_results()\n");
	tds_detach_results(tds->res_info);
	tds_free_results(tds->res_info);
	tds->res_info = NULL;
	tds_detach_results(tds->param_info);
	tds_free_param_results(tds->param_info);
	tds->param_info = NULL;
	tds_free_compute_results(tds);
	tds->has_status = false;
	tds->in_row = false;
	tds->ret_status = 0;
	if (tds->cur_dyn)
		tds_detach_results(tds->cur_dyn->res_info);
}

/*
 * Return true if winsock is initialized, else false.
 */
static bool
winsock_initialized(void)
{
#ifdef _WIN32
	static bool initialized = false;
	static tds_mutex mtx = TDS_MUTEX_INITIALIZER;

	int erc;

	if (initialized)
		return true;

	tds_mutex_lock(&mtx);
	/* same check inside the mutex */
	if (initialized) {
		tds_mutex_unlock(&mtx);
		return true;
	}

	/* initialize the socket layer */
	erc = tds_socket_init();
	initialized = (erc == 0);
	tds_mutex_unlock(&mtx);

	if (erc != 0) {
		char *errstr = sock_strerror(erc);
		tdsdump_log(TDS_DBG_ERROR, "tds_init_winsock: WSAStartup failed with %d (%s)\n", erc, errstr);
		sock_strerror_free(errstr);
		return false;
	}
#endif
	return true;
}

TDSCONTEXT *
tds_alloc_context(void * parent)
{
	TDSCONTEXT *context;
	TDSLOCALE *locale;

	if (!winsock_initialized())
		return NULL;

	if ((locale = tds_get_locale()) == NULL)
		return NULL;

	if ((context = tds_new0(TDSCONTEXT, 1)) == NULL) {
		tds_free_locale(locale);
		return NULL;
	}
	context->locale = locale;
	context->parent = parent;
	context->money_use_2_digits = false;

	return context;
}

void
tds_free_context(TDSCONTEXT * context)
{
	if (!context)
		return;

	tds_free_locale(context->locale);
	free(context);
}

TDSLOCALE *
tds_alloc_locale(void)
{
	TDSLOCALE *locale;

	TEST_MALLOC(locale, TDSLOCALE);

	return locale;

      Cleanup:
	tds_free_locale(locale);
	return NULL;
}

/*
 * Default capabilities.
 */
#undef REQ
#define SUPPORTED_REQ_CAP(i) \
	REQ(i,LANG) REQ(i,RPC) REQ(i,EVT) REQ(i,MSTMT) REQ(i,BCP) REQ(i,CURSOR) REQ(i,DYNF) \
	REQ(i,MSG) REQ(i,PARAM) REQ(i,DATA_INT1) REQ(i,DATA_INT2) REQ(i,DATA_INT4) REQ(i,DATA_BIT) \
	REQ(i,DATA_CHAR) REQ(i,DATA_VCHAR) REQ(i,DATA_BIN) REQ(i,DATA_VBIN) REQ(i,DATA_MNY8) \
	REQ(i,DATA_MNY4) REQ(i,DATA_DATE8) REQ(i,DATA_DATE4) REQ(i,DATA_FLT4) REQ(i,DATA_FLT8) \
	REQ(i,DATA_NUM) REQ(i,DATA_TEXT) REQ(i,DATA_IMAGE) REQ(i,DATA_DEC) REQ(i,DATA_LCHAR) \
	REQ(i,DATA_LBIN) REQ(i,DATA_INTN) REQ(i,DATA_DATETIMEN) REQ(i,DATA_MONEYN) \
	REQ(i,CSR_PREV) REQ(i,CSR_FIRST) REQ(i,CSR_LAST) REQ(i,CSR_ABS) REQ(i,CSR_REL) \
	REQ(i,CSR_MULTI) REQ(i,CON_INBAND) REQ(i,PROTO_TEXT) REQ(i,PROTO_BULK) \
	REQ(i,DATA_SENSITIVITY) REQ(i,DATA_BOUNDARY) REQ(i,PROTO_DYNPROC) REQ(i,DATA_FLTN) \
	REQ(i,DATA_BITN) REQ(i,DATA_INT8) REQ(i,DOL_BULK) REQ(i,WIDETABLE) \
	REQ(i,DATA_UINT2) REQ(i,DATA_UINT4) REQ(i,DATA_UINT8) REQ(i,DATA_UINTN) REQ(i,LARGEIDENT) \
	REQ(i,SRVPKTSIZE) REQ(i,DATA_DATE) REQ(i,DATA_TIME) REQ(i,DATA_BIGTIME) REQ(i,DATA_BIGDATETIME)
#define REQ(i,n) |(((TDS_REQ_ ## n / 8) == i)?(1<<(TDS_REQ_ ## n & 7)):0)
#define REQB(i) 0 SUPPORTED_REQ_CAP(i)

#undef RES
#define SUPPORTED_RES_CAP(i) \
	RES(i,CON_NOOOB) RES(i,PROTO_NOTEXT) RES(i,PROTO_NOBULK) RES(i,NOTDSDEBUG)
#define RES(i,n) |(((TDS_RES_ ## n / 8) == i)?(1<<(TDS_RES_ ## n & 7)):0)
#define RESB(i) 0 SUPPORTED_RES_CAP(i)

static const TDS_CAPABILITIES defaultcaps = { {
     /* type,  len, data, data, data, data, data, data, data, data, data, data, data, data, data, data (14 bytes) */
	{ 1, 14, { REQB(13), REQB(12), REQB(11), REQB(10), REQB(9), REQB(8), REQB(7),
	           REQB(6),  REQB(5),  REQB(4),  REQB(3),  REQB(2), REQB(1), REQB(0) } },
	{ 2, 14, { RESB(13), RESB(12), RESB(11), RESB(10), RESB(9), RESB(8), RESB(7),
	           RESB(6),  RESB(5),  RESB(4),  RESB(3),  RESB(2), RESB(1), RESB(0) } }
} };
/* check we match the values size */
TDS_COMPILE_CHECK(tds_values_len, sizeof(defaultcaps.types[0].values) == 14);
/* check we match the default size */
TDS_COMPILE_CHECK(tds_cap_len, sizeof(defaultcaps) == TDS_MAX_CAPABILITY);

/**
 * Initialize login structure with locale information and other stuff for connection
 * @param locale locale information (copied to configuration information)
 * @result login structure or NULL if initialization error
 */
TDSLOGIN*
tds_init_login(TDSLOGIN *login, TDSLOCALE * locale)
{
	char hostname[128];
#if HAVE_NL_LANGINFO && defined(CODESET)
	const char *charset;
#elif !defined(_WIN32)
	char *lc_all, *tok = NULL;
#endif
#ifdef _WIN32
	char cp[128];
#endif

	/*
	 * TDS 7.0:
	 * ODBC driver settings.
	 * Change to initial language must succeed.
	 */
	login->option_flag2 = TDS_INIT_LANG_REQUIRED|TDS_ODBC_ON;
	login->tds_version = TDS_DEFAULT_VERSION;
	login->block_size = 0;

#if HAVE_NL_LANGINFO && defined(CODESET)
	charset = nl_langinfo(CODESET);
	if (strcmp(tds_canonical_charset_name(charset), "US-ASCII") == 0)
		charset = "ISO-8859-1";
	if (!tds_dstr_copy(&login->client_charset, charset))
		return NULL;
#else
	if (!tds_dstr_copy(&login->client_charset, "ISO-8859-1"))
		return NULL;

#ifdef _WIN32
	/* for Windows uses GetLocaleInfoA */
	strcpy(cp, "CP");
	if (GetLocaleInfoA(GetThreadLocale(), LOCALE_IDEFAULTANSICODEPAGE, cp + 2, sizeof(cp) - 2) > 0)
		if (!tds_dstr_copy(&login->client_charset, cp))
			return NULL;
#else
	if ((lc_all = strdup(setlocale(LC_ALL, NULL))) == NULL)
		return NULL;

	if (strtok_r(lc_all, ".", &tok)) {
		char *encoding = strtok_r(NULL, "@", &tok);
		if (encoding) {
			if (!tds_dstr_copy(&login->client_charset, encoding)) {
				free(lc_all);
				return NULL;
			}
		}
	}
	free(lc_all);
#endif
#endif

	if (locale) {
		if (locale->language)
			if (!tds_dstr_copy(&login->language, locale->language))
				return NULL;
		if (locale->server_charset)
			if (!tds_dstr_copy(&login->server_charset, locale->server_charset))
				return NULL;
	}
	if (tds_dstr_isempty(&login->language)) {
		if (!tds_dstr_copy(&login->language, TDS_DEF_LANG))
			return NULL;
	}
	memset(hostname, '\0', sizeof(hostname));
	gethostname(hostname, sizeof(hostname));
	hostname[sizeof(hostname) - 1] = '\0';	/* make sure it's terminated */
	if (!tds_dstr_copy(&login->client_host_name, hostname))
		return NULL;

	login->valid_configuration = 1;
	login->check_ssl_hostname = 1;

	return login;
}

TDSCURSOR *
tds_alloc_cursor(TDSSOCKET *tds, const char *name, size_t namelen, const char *query, size_t querylen)
{
	TDSCURSOR *cursor;
	TDSCURSOR *pcursor;

	TEST_MALLOC(cursor, TDSCURSOR);
	cursor->ref_count = 1;

	cursor->type = TDS_CUR_TYPE_KEYSET;
	cursor->concurrency = TDS_CUR_CONCUR_OPTIMISTIC;

	TEST_CALLOC(cursor->cursor_name, char, namelen + 1);
	memcpy(cursor->cursor_name, name, namelen);

	TEST_CALLOC(cursor->query, char, querylen + 1);
	memcpy(cursor->query, query, querylen);

	if (tds->conn->cursors == NULL) {
		tds->conn->cursors = cursor;
	} else {
		for (pcursor = tds->conn->cursors; pcursor->next; pcursor = pcursor->next)
			continue;
		pcursor->next = cursor;
	}
	/* take into account reference in connection list */
	++cursor->ref_count;

	return cursor;

      Cleanup:
	tds_release_cursor(&cursor);
	return NULL;
}

/*
 * Called when cursor got deallocated from server
 */
void
tds_cursor_deallocated(TDSCONNECTION *conn, TDSCURSOR *cursor)
{
	TDSCURSOR **victim;

	tdsdump_log(TDS_DBG_FUNC, "tds_cursor_deallocated() : freeing cursor_id %d\n", cursor->cursor_id);

	victim = &conn->cursors;
	while (*victim != cursor) {
		if (*victim == NULL) {
			tdsdump_log(TDS_DBG_FUNC, "tds_cursor_deallocated() : cannot find cursor_id %d\n", cursor->cursor_id);
			return;
		}
		victim = &(*victim)->next;
	}

	/* remove from list */
	*victim = cursor->next;
	cursor->next = NULL;

	tds_release_cursor(&cursor);
}

/*
 * Decrement reference counter and free if necessary.
 * Called internally by libTDS and by upper library when you don't need 
 * cursor reference anymore
 */
void
tds_release_cursor(TDSCURSOR **pcursor)
{
	TDSCURSOR *cursor = *pcursor;
	*pcursor = NULL;
	if (!cursor || --cursor->ref_count > 0)
		return;

	tdsdump_log(TDS_DBG_FUNC, "tds_release_cursor() : freeing cursor_id %d\n", cursor->cursor_id);

	tdsdump_log(TDS_DBG_FUNC, "tds_release_cursor() : freeing cursor results\n");
	tds_detach_results(cursor->res_info);
	tds_free_results(cursor->res_info);

	if (cursor->cursor_name) {
		tdsdump_log(TDS_DBG_FUNC, "tds_release_cursor() : freeing cursor name\n");
		free(cursor->cursor_name);
	}

	if (cursor->query) {
		tdsdump_log(TDS_DBG_FUNC, "tds_release_cursor() : freeing cursor query\n");
		free(cursor->query);
	}

	tdsdump_log(TDS_DBG_FUNC, "tds_release_cursor() : cursor_id %d freed\n", cursor->cursor_id);
	free(cursor);
}

TDSLOGIN *
tds_alloc_login(bool use_environment)
{
	TDSLOGIN *login = NULL;
	const char *server_name = TDS_DEF_SERVER;

	TEST_MALLOC(login, TDSLOGIN);
	login->check_ssl_hostname = 1;
	login->use_utf16 = 1;
	login->bulk_copy = 1;
	tds_dstr_init(&login->server_name);
	tds_dstr_init(&login->language);
	tds_dstr_init(&login->server_charset);
	tds_dstr_init(&login->client_host_name);
	tds_dstr_init(&login->server_host_name);
	tds_dstr_init(&login->app_name);
	tds_dstr_init(&login->user_name);
	tds_dstr_init(&login->password);
	tds_dstr_init(&login->library);
	tds_dstr_init(&login->new_password);

	login->ip_addrs = NULL;

	tds_dstr_init(&login->database);
	login->dump_file = NULL;
	tds_dstr_init(&login->client_charset);
	tds_dstr_init(&login->instance_name);
	tds_dstr_init(&login->server_realm_name);
	tds_dstr_init(&login->server_spn);
	tds_dstr_init(&login->cafile);
	tds_dstr_init(&login->crlfile);
	tds_dstr_init(&login->certificate_host_name);
	tds_dstr_init(&login->db_filename);
	tds_dstr_init(&login->openssl_ciphers);
	tds_dstr_init(&login->routing_address);

	if (use_environment) {
		const char *s;
		if ((s=getenv("DSQUERY")) != NULL)
			server_name = s;

		if ((s=getenv("TDSQUERY")) != NULL)
			server_name = s;
	}

	if (!tds_dstr_copy(&login->server_name, server_name)) {
		free(login);
		return NULL;
	}

	login->capabilities = defaultcaps;
	login->use_ntlmv2_specified = 0;
	login->use_ntlmv2 = 1;

Cleanup:
	return login;
}

void
tds_free_login(TDSLOGIN * login)
{
	if (!login)
		return;

	/* for security reason clear memory */
	tds_dstr_zero(&login->password);
	tds_dstr_free(&login->password);
	tds_dstr_zero(&login->new_password);
	tds_dstr_free(&login->new_password);
	tds_dstr_free(&login->server_name);
	tds_dstr_free(&login->language);
	tds_dstr_free(&login->server_charset);
	tds_dstr_free(&login->client_host_name);
	tds_dstr_free(&login->app_name);
	tds_dstr_free(&login->user_name);
	tds_dstr_free(&login->library);
	tds_dstr_free(&login->client_charset);
	tds_dstr_free(&login->server_host_name);

	if (login->ip_addrs != NULL)
		freeaddrinfo(login->ip_addrs);

	tds_dstr_free(&login->database);
	free(login->dump_file);
	tds_dstr_free(&login->instance_name);
	tds_dstr_free(&login->server_realm_name);
	tds_dstr_free(&login->server_spn);
	tds_dstr_free(&login->cafile);
	tds_dstr_free(&login->crlfile);
	tds_dstr_free(&login->certificate_host_name);
	tds_dstr_free(&login->db_filename);
	tds_dstr_free(&login->openssl_ciphers);
	tds_dstr_free(&login->routing_address);
	free(login);
}

TDSPACKET *
tds_alloc_packet(void *buf, unsigned len)
{
	TDSPACKET *packet = (TDSPACKET *) malloc(len + TDS_OFFSET(TDSPACKET, buf));
	if (TDS_LIKELY(packet)) {
		tds_packet_zero_data_start(packet);
		packet->data_len = 0;
		packet->capacity = len;
		packet->sid = 0;
		packet->next = NULL;
		if (buf) {
			memcpy(packet->buf, buf, len);
			packet->data_len = len;
		}
	}
	return packet;
}

TDSPACKET *
tds_realloc_packet(TDSPACKET *packet, unsigned len)
{
	if (packet->capacity < len) {
		packet = (TDSPACKET *) realloc(packet, len + TDS_OFFSET(TDSPACKET, buf));
		if (TDS_LIKELY(packet))
			packet->capacity = len;
	}
	return packet;
}

void
tds_free_packets(TDSPACKET *packet)
{
	TDSPACKET *next;
	for (; packet; packet = next) {
		next = packet->next;
		free(packet);
	}
}

static void
tds_deinit_connection(TDSCONNECTION *conn)
{
	if (conn->authentication)
		conn->authentication->free(conn, conn->authentication);
	conn->authentication = NULL;
	while (conn->dyns)
		tds_dynamic_deallocated(conn, conn->dyns);
	while (conn->cursors)
		tds_cursor_deallocated(conn, conn->cursors);
	tds_ssl_deinit(conn);
	/* close connection and free inactive sockets */
	tds_connection_close(conn);
	tds_wakeup_close(&conn->wakeup);
	tds_iconv_free(conn);
	free(conn->product_name);
	free(conn->server);
	tds_free_env(conn);
	tds_free_packets(conn->packet_cache);
	tds_mutex_free(&conn->list_mtx);
#if ENABLE_ODBC_MARS
	tds_free_packets(conn->packets);
	tds_free_packets(conn->recv_packet);
	tds_free_packets(conn->send_packets);
	free(conn->sessions);
#endif
}

static TDSCONNECTION *
tds_init_connection(TDSCONNECTION *conn, TDSCONTEXT *context, unsigned int bufsize)
{
	conn->env.block_size = bufsize;
	conn->s = INVALID_SOCKET;
	conn->use_iconv = 1;
	conn->tds_ctx = context;
	conn->ncharsize = 1;
	conn->unicharsize = 1;

	if (tds_wakeup_init(&conn->wakeup))
		goto Cleanup;

	if (tds_iconv_alloc(conn))
		goto Cleanup;

	if (tds_mutex_init(&conn->list_mtx))
		goto Cleanup;

#if ENABLE_ODBC_MARS
	TEST_CALLOC(conn->sessions, TDSSOCKET*, 64);
	conn->num_sessions = 64;
#endif
	return conn;

Cleanup:
	tds_wakeup_close(&conn->wakeup);
	tds_iconv_free(conn);
	return NULL;
}

static TDSSOCKET *
tds_init_socket(TDSSOCKET * tds_socket, unsigned int bufsize)
{
	TDSPACKET *pkt;

	tds_socket->parent = NULL;

	tds_socket->recv_packet = tds_alloc_packet(NULL, bufsize);
	if (!tds_socket->recv_packet)
		goto Cleanup;
	tds_socket->in_buf = tds_socket->recv_packet->buf;

	pkt = tds_alloc_packet(NULL, bufsize + TDS_ADDITIONAL_SPACE);
	if (!pkt)
		goto Cleanup;
	tds_set_current_send_packet(tds_socket, pkt);

	tds_socket->out_buf_max = bufsize;

	/* Jeff's hack, init to no timeout */
	tds_socket->query_timeout = 0;
	tds_init_write_buf(tds_socket);
	tds_socket->state = TDS_DEAD;
	tds_socket->env_chg_func = NULL;
	if (tds_mutex_init(&tds_socket->wire_mtx))
		goto Cleanup;

#ifdef ENABLE_ODBC_MARS
	tds_socket->sid = 0;
	if (tds_cond_init(&tds_socket->packet_cond))
		goto Cleanup;

	tds_socket->recv_seq = 0;
	tds_socket->send_seq = 0;
	tds_socket->recv_wnd = 4;
	tds_socket->send_wnd = 4;
#endif
	return tds_socket;

      Cleanup:
	return NULL;
}


#if ENABLE_ODBC_MARS
static void
tds_free_connection(TDSCONNECTION *conn)
{
	if (!conn) return;
	assert(conn->in_net_tds == NULL);
	tds_deinit_connection(conn);
	free(conn);
}

static TDSCONNECTION *
tds_alloc_connection(TDSCONTEXT *context, unsigned int bufsize)
{
	TDSCONNECTION *conn;

	TEST_MALLOC(conn, TDSCONNECTION);
	if (!tds_init_connection(conn, context, bufsize))
		goto Cleanup;
	return conn;

Cleanup:
	tds_free_connection(conn);
	return NULL;
}

static TDSSOCKET *
tds_alloc_socket_base(unsigned int bufsize)
{
	TDSSOCKET *tds_socket;

	TEST_MALLOC(tds_socket, TDSSOCKET);
	if (!tds_init_socket(tds_socket, bufsize))
		goto Cleanup;
	return tds_socket;

      Cleanup:
	tds_free_socket(tds_socket);
	return NULL;
}

TDSSOCKET *
tds_alloc_socket(TDSCONTEXT * context, unsigned int bufsize)
{
	TDSCONNECTION *conn = tds_alloc_connection(context, bufsize);
	TDSSOCKET *tds;

	if (!conn)
		return NULL;

	tds = tds_alloc_socket_base(bufsize);
	if (tds) {
		conn->sessions[0] = tds;
		tds->conn = conn;
		return tds;
	}
	tds_free_connection(conn);
	return NULL;
}

static bool
tds_alloc_new_sid(TDSSOCKET *tds)
{
	uint16_t sid;
	TDSCONNECTION *conn = tds->conn;

	tds_mutex_lock(&conn->list_mtx);
	for (sid = 1; sid < conn->num_sessions; ++sid)
		if (!conn->sessions[sid])
			break;
	if (sid == conn->num_sessions) {
		/* extend array */
		TDSSOCKET **s = (TDSSOCKET **) TDS_RESIZE(conn->sessions, sid+64);
		if (!s)
			goto error;
		memset(s + conn->num_sessions, 0, sizeof(*s) * 64);
		conn->num_sessions += 64;
	}
	conn->sessions[sid] = tds;
	tds->sid = sid;
error:
	tds_mutex_unlock(&conn->list_mtx);
	return tds->sid != 0;
}

TDSSOCKET *
tds_alloc_additional_socket(TDSCONNECTION *conn)
{
	TDSSOCKET *tds;
	if (!IS_TDS72_PLUS(conn) || !conn->mars)
		return NULL;

	tds = tds_alloc_socket_base(sizeof(TDS72_SMP_HEADER) + conn->env.block_size);
	if (!tds)
		return NULL;
	tds->send_packet->data_start = sizeof(TDS72_SMP_HEADER);
	tds->out_buf = tds->send_packet->buf + sizeof(TDS72_SMP_HEADER);
	tds->out_buf_max -= sizeof(TDS72_SMP_HEADER);

	tds->conn = conn;
	if (!tds_alloc_new_sid(tds))
		goto Cleanup;

	tds->state = TDS_IDLE;
	if (TDS_FAILED(tds_append_syn(tds)))
		goto Cleanup;

	return tds;

      Cleanup:
	tds_free_socket(tds);
	return NULL;
}
#else /* !ENABLE_ODBC_MARS */
TDSSOCKET *
tds_alloc_socket(TDSCONTEXT * context, unsigned int bufsize)
{
	TDSSOCKET *tds_socket;

	TEST_MALLOC(tds_socket, TDSSOCKET);
	if (!tds_init_connection(tds_socket->conn, context, bufsize))
		goto Cleanup;
	if (!tds_init_socket(tds_socket, bufsize))
		goto Cleanup;
	return tds_socket;

      Cleanup:
	tds_free_socket(tds_socket);
	return NULL;
}
#endif /* !ENABLE_ODBC_MARS */

TDSSOCKET *
tds_realloc_socket(TDSSOCKET * tds, unsigned int bufsize)
{
	TDSPACKET *packet;
#if ENABLE_ODBC_MARS
	size_t smp_hdr_len = tds->conn->mars ? sizeof(TDS72_SMP_HEADER) : 0;
#else
	enum { smp_hdr_len = 0 };
#endif

	assert(tds && tds->out_buf && tds->send_packet);

	if (bufsize < 512)
		bufsize = 512;

	/* prevent nasty memory conditions, server should send the request at
	 * the beginning only */
	if (tds->out_pos > bufsize || tds->frozen)
		return NULL;

	tds->conn->env.block_size = bufsize;

	packet = tds_realloc_packet(tds->send_packet, smp_hdr_len + bufsize + TDS_ADDITIONAL_SPACE);
	if (packet == NULL)
		return NULL;

#if ENABLE_ODBC_MARS
	packet->data_start = smp_hdr_len;
#endif
	tds->out_buf_max = bufsize;
	tds_set_current_send_packet(tds, packet);
	return tds;
}

#if ENABLE_ODBC_MARS
static void
tds_connection_remove_socket(TDSCONNECTION *conn, TDSSOCKET *tds)
{
	unsigned n;
	bool must_free_connection = true;
	tds_mutex_lock(&conn->list_mtx);
	if (tds->sid < conn->num_sessions)
		conn->sessions[tds->sid] = NULL;
	for (n = 0; n < conn->num_sessions; ++n)
		if (TDSSOCKET_VALID(conn->sessions[n])) {
			must_free_connection = false;
			break;
		}
	if (!must_free_connection) {
		/* tds use connection member so must be valid */
		tds_append_fin(tds);
	}
	tds_mutex_unlock(&conn->list_mtx);

	/* detach entirely */
	tds->conn = NULL;

	if (must_free_connection)
		tds_free_connection(conn);
}
#else
static void inline
tds_connection_remove_socket(TDSCONNECTION *conn, TDSSOCKET *tds TDS_UNUSED)
{
	tds_deinit_connection(conn);
}
#endif

void
tds_free_socket(TDSSOCKET * tds)
{
#if ENABLE_EXTRA_CHECKS
	TDSDYNAMIC *dyn;
	TDSCURSOR *cur;
#endif

	if (!tds)
		return;

	/* detach this socket */
	tds_release_cur_dyn(tds);
	tds_release_cursor(&tds->cur_cursor);
	tds_detach_results(tds->current_results);
#if ENABLE_EXTRA_CHECKS
	for (dyn = tds->conn->dyns; dyn; dyn = dyn->next) {
		if (dyn->res_info && dyn->res_info->attached_to == tds) {
			assert(0);
		}
	}
	for (cur = tds->conn->cursors; cur; cur = cur->next) {
		if (cur->res_info && cur->res_info->attached_to == tds) {
			assert(0);
		}
	}
#endif
	tds_free_all_results(tds);
#if ENABLE_ODBC_MARS
	tds_cond_destroy(&tds->packet_cond);
#endif

	tds_connection_remove_socket(tds->conn, tds);
	tds_free_packets(tds->recv_packet);
	if (tds->frozen_packets)
		tds_free_packets(tds->frozen_packets);
	else
		tds_free_packets(tds->send_packet);
	free(tds);
}

void
tds_free_locale(TDSLOCALE * locale)
{
	if (!locale)
		return;

	free(locale->language);
	free(locale->server_charset);
	free(locale->datetime_fmt);
	free(locale->date_fmt);
	free(locale->time_fmt);
	free(locale);
}

static void
tds_free_env(TDSCONNECTION* conn)
{
	if (conn->env.language)
		TDS_ZERO_FREE(conn->env.language);
	if (conn->env.charset)
		TDS_ZERO_FREE(conn->env.charset);
	if (conn->env.database)
		TDS_ZERO_FREE(conn->env.database);
}

void
tds_free_msg(TDSMESSAGE * message)
{
	if (message) {
		message->priv_msg_type = 0;
		message->msgno = 0;
		message->state = 0;
		message->severity = 0;
		message->line_number = 0;
		TDS_ZERO_FREE(message->message);
		TDS_ZERO_FREE(message->server);
		TDS_ZERO_FREE(message->proc_name);
		TDS_ZERO_FREE(message->sql_state);
	}
}

#define SQLS_ENTRY(number,state) case number: p = state; break

char *
tds_alloc_client_sqlstate(int msgno)
{
	const char *p = NULL;

	switch (msgno) {
		SQLS_ENTRY(17000, "S1T00");	/* timeouts ??? */
		SQLS_ENTRY(20004, "08S01");	/* Communication link failure */
		SQLS_ENTRY(20006, "08S01");
		SQLS_ENTRY(20009, "08S01");
		SQLS_ENTRY(20020, "08S01");
		SQLS_ENTRY(20019, "24000");	/* Invalid cursor state */
		SQLS_ENTRY(20014, "28000");	/* Invalid authorization specification */
		SQLS_ENTRY(2400, "42000");	/* Syntax error or access violation */
		SQLS_ENTRY(2401, "42000");
		SQLS_ENTRY(2403, "42000");
		SQLS_ENTRY(2404, "42000");
		SQLS_ENTRY(2402, "S1000");	/* General error */
	}

	if (p != NULL)
		return strdup(p);
	else
		return NULL;
}

char *
tds_alloc_lookup_sqlstate(TDSSOCKET * tds, int msgno)
{
	const char *p = NULL;
	char *q = NULL;

	if (TDS_IS_MSSQL(tds)) {
		switch (msgno) {	/* MSSQL Server */

			SQLS_ENTRY(3621,"01000");
			SQLS_ENTRY(8153,"01003");	/* Null in aggregate */
			SQLS_ENTRY(911, "08004");	/* Server rejected connection */
			SQLS_ENTRY(512, "21000");	/* Subquery returns more than one value */
			SQLS_ENTRY(213, "21S01");	/* Insert column list mismatch */
			SQLS_ENTRY(109, "21S01");
			SQLS_ENTRY(110, "21S01");
			SQLS_ENTRY(1774,"21S02");	/* Ref column mismatch */
			SQLS_ENTRY(8152,"22001");	/* String data would be truncated */
			SQLS_ENTRY(5146,"22003");	/* Numeric value out of range */
			SQLS_ENTRY(168,	"22003");	/* Arithmetic overflow */
			SQLS_ENTRY(220, "22003");
			SQLS_ENTRY(232, "22003");
			SQLS_ENTRY(234, "22003");
			SQLS_ENTRY(236, "22003");
			SQLS_ENTRY(238, "22003");
			SQLS_ENTRY(244, "22003");
			SQLS_ENTRY(246, "22003");
			SQLS_ENTRY(248, "22003");
			SQLS_ENTRY(519, "22003");
			SQLS_ENTRY(520, "22003");
			SQLS_ENTRY(521, "22003");
			SQLS_ENTRY(522, "22003");
			SQLS_ENTRY(523, "22003");
			SQLS_ENTRY(524, "22003");
			SQLS_ENTRY(1007,"22003");
			SQLS_ENTRY(3606,"22003");
			SQLS_ENTRY(8115,"22003");
			SQLS_ENTRY(206, "22005");	/* Error in assignment */
			SQLS_ENTRY(235, "22005");
			SQLS_ENTRY(247, "22005");
			SQLS_ENTRY(249, "22005");
			SQLS_ENTRY(256, "22005");
			SQLS_ENTRY(257, "22005");
			SQLS_ENTRY(305, "22005");
			SQLS_ENTRY(409, "22005");
			SQLS_ENTRY(518, "22005");
			SQLS_ENTRY(529, "22005");
			SQLS_ENTRY(210, "22007");	/* Invalid datetime format */
			SQLS_ENTRY(241, "22007");
			SQLS_ENTRY(295, "22007");
			SQLS_ENTRY(242, "22008");	/* Datetime out of range */
			SQLS_ENTRY(296, "22008");
			SQLS_ENTRY(298, "22008");
			SQLS_ENTRY(535, "22008");
			SQLS_ENTRY(542, "22008");
			SQLS_ENTRY(517, "22008");
			SQLS_ENTRY(3607, "22012");	/* Div by zero */
			SQLS_ENTRY(8134, "22012");
			SQLS_ENTRY(245, "22018");	/* Syntax error? */
			SQLS_ENTRY(2627, "23000");	/* Constraint violation */
			SQLS_ENTRY(515, "23000");
			SQLS_ENTRY(233,	"23000");
			SQLS_ENTRY(273,	"23000");
			SQLS_ENTRY(530,	"23000");
			SQLS_ENTRY(2601,"23000");
			SQLS_ENTRY(2615,"23000");
			SQLS_ENTRY(2626,"23000");
			SQLS_ENTRY(3604,"23000");
			SQLS_ENTRY(3605,"23000");
			SQLS_ENTRY(544, "23000");
			SQLS_ENTRY(547, "23000");
			SQLS_ENTRY(550, "23000");
			SQLS_ENTRY(4415, "23000");
			SQLS_ENTRY(1505, "23000");
			SQLS_ENTRY(1508, "23000");
			SQLS_ENTRY(3725, "23000");
			SQLS_ENTRY(3726, "23000");
			SQLS_ENTRY(4712, "23000");
			SQLS_ENTRY(10055, "23000");
			SQLS_ENTRY(10065, "23000");
			SQLS_ENTRY(11011, "23000");
			SQLS_ENTRY(11040, "23000");
			SQLS_ENTRY(16999, "24000");	/* Invalid cursor state */
			SQLS_ENTRY(16905, "24000");
			SQLS_ENTRY(16917, "24000");
			SQLS_ENTRY(16946, "24000");
			SQLS_ENTRY(16950, "24000");
			SQLS_ENTRY(266, "25000");	/* Invalid transaction state */
			SQLS_ENTRY(277,"25000");
			SQLS_ENTRY(611,"25000");
			SQLS_ENTRY(3906,"25000");
			SQLS_ENTRY(3908,"25000");
			SQLS_ENTRY(6401,"25000");
			SQLS_ENTRY(626, "25000");
			SQLS_ENTRY(627, "25000");
			SQLS_ENTRY(628, "25000");
			SQLS_ENTRY(3902, "25000");
			SQLS_ENTRY(3903, "25000");
			SQLS_ENTRY(3916, "25000");
			SQLS_ENTRY(3918, "25000");
			SQLS_ENTRY(3919, "25000");
			SQLS_ENTRY(3921, "25000");
			SQLS_ENTRY(3922, "25000");
			SQLS_ENTRY(3926, "25000");
			SQLS_ENTRY(7969, "25000");
			SQLS_ENTRY(8506, "25000");
			SQLS_ENTRY(15626, "25000");
			SQLS_ENTRY(18456, "28000");	/* Login failed? */
			SQLS_ENTRY(6104, "37000");	/* Syntax error or access violation */
			SQLS_ENTRY(8114, "37000");
			SQLS_ENTRY(131, "37000");
			SQLS_ENTRY(137, "37000");
			SQLS_ENTRY(170, "37000");
			SQLS_ENTRY(174, "37000");
			SQLS_ENTRY(201, "37000");
			SQLS_ENTRY(2812, "37000");
			SQLS_ENTRY(2526, "37000");
			SQLS_ENTRY(8144, "37000");
			SQLS_ENTRY(17308, "42000");	/* Syntax/Access violation */
			SQLS_ENTRY(17571, "42000");
			SQLS_ENTRY(18002, "42000");
			SQLS_ENTRY(229, "42000");
			SQLS_ENTRY(230, "42000");
			SQLS_ENTRY(262, "42000");
			SQLS_ENTRY(2557, "42000");
			SQLS_ENTRY(2571, "42000");
			SQLS_ENTRY(2760, "42000");
			SQLS_ENTRY(3110, "42000");
			SQLS_ENTRY(3704, "42000");
			SQLS_ENTRY(4613, "42000");
			SQLS_ENTRY(4618, "42000");
			SQLS_ENTRY(4834, "42000");
			SQLS_ENTRY(5011, "42000");
			SQLS_ENTRY(5116, "42000");
			SQLS_ENTRY(5812, "42000");
			SQLS_ENTRY(6004, "42000");
			SQLS_ENTRY(6102, "42000");
			SQLS_ENTRY(7956, "42000");
			SQLS_ENTRY(11010, "42000");
			SQLS_ENTRY(11045, "42000");
			SQLS_ENTRY(14126, "42000");
			SQLS_ENTRY(15247, "42000");
			SQLS_ENTRY(15622, "42000");
			SQLS_ENTRY(20604, "42000");
			SQLS_ENTRY(21049, "42000");
			SQLS_ENTRY(113, "42000");
			SQLS_ENTRY(2714, "42S01");	/* Table or view already exists */
			SQLS_ENTRY(208, "42S02");	/* Table or view not found */
			SQLS_ENTRY(3701, "42S02");
			SQLS_ENTRY(1913, "42S11");	/* Index already exists */
			SQLS_ENTRY(15605, "42S11");
			SQLS_ENTRY(307, "42S12");	/* Index not found */
			SQLS_ENTRY(308, "42S12");
			SQLS_ENTRY(10033, "42S12");
			SQLS_ENTRY(15323, "42S12");
			SQLS_ENTRY(18833, "42S12");
			SQLS_ENTRY(4925, "42S21");	/* Column already exists */
			SQLS_ENTRY(21255, "42S21");
			SQLS_ENTRY(1911, "42S22");	/* Column not found */
			SQLS_ENTRY(207, "42S22");
			SQLS_ENTRY(4924, "42S22");
			SQLS_ENTRY(4926, "42S22");
			SQLS_ENTRY(15645, "42S22");
			SQLS_ENTRY(21166, "42S22");
		}
	} else {
		switch (msgno) {	/* Sybase */
			SQLS_ENTRY(3621, "01000");
			SQLS_ENTRY(9501, "01003");	/* Null in aggregate */
			SQLS_ENTRY(911, "08004");	/* Server rejected connection */
			SQLS_ENTRY(512, "21000");	/* Subquery returns more than one value */
			SQLS_ENTRY(213, "21S01");	/* Insert column list mismatch */
			SQLS_ENTRY(109, "21S01");
			SQLS_ENTRY(110, "21S01");
			SQLS_ENTRY(1715, "21S02");	/* Ref column mismatch */
			SQLS_ENTRY(9502, "22001");	/* String data would be truncated */
			SQLS_ENTRY(220, "22003");	/* Arithmetic overflow */
			SQLS_ENTRY(168, "22003");
			SQLS_ENTRY(227, "22003");
			SQLS_ENTRY(232, "22003");
			SQLS_ENTRY(234, "22003");
			SQLS_ENTRY(236, "22003");
			SQLS_ENTRY(238, "22003");
			SQLS_ENTRY(244, "22003");
			SQLS_ENTRY(246, "22003");
			SQLS_ENTRY(247, "22003");
			SQLS_ENTRY(248, "22003");
			SQLS_ENTRY(519, "22003");
			SQLS_ENTRY(520, "22003");
			SQLS_ENTRY(521, "22003");
			SQLS_ENTRY(522, "22003");
			SQLS_ENTRY(523, "22003");
			SQLS_ENTRY(524, "22003");
			SQLS_ENTRY(3606, "22003");
			SQLS_ENTRY(206, "22005");	/* Error in assignment */
			SQLS_ENTRY(235, "22005");
			SQLS_ENTRY(249, "22005");
			SQLS_ENTRY(256, "22005");
			SQLS_ENTRY(305, "22005");
			SQLS_ENTRY(409, "22005");
			SQLS_ENTRY(518, "22005");
			SQLS_ENTRY(529, "22005");
			SQLS_ENTRY(535, "22008");	/* Datetime out of range */
			SQLS_ENTRY(542, "22008");
			SQLS_ENTRY(517, "22008");
			SQLS_ENTRY(3607, "22012");	/* Div by zero */
			SQLS_ENTRY(245, "22018");	/* Syntax error? */
			SQLS_ENTRY(544, "23000");	/* Constraint violation */
			SQLS_ENTRY(233, "23000");
			SQLS_ENTRY(273,	"23000");
			SQLS_ENTRY(530,	"23000");
			SQLS_ENTRY(2601,"23000");
			SQLS_ENTRY(2615,"23000");
			SQLS_ENTRY(2626,"23000");
			SQLS_ENTRY(3604,"23000");
			SQLS_ENTRY(3605,"23000");
			SQLS_ENTRY(545, "23000");
			SQLS_ENTRY(546, "23000");
			SQLS_ENTRY(547, "23000");
			SQLS_ENTRY(548, "23000");
			SQLS_ENTRY(549, "23000");
			SQLS_ENTRY(550, "23000");
			SQLS_ENTRY(1505, "23000");
			SQLS_ENTRY(1508, "23000");
			SQLS_ENTRY(565, "24000");	/* Invalid cursor state */
			SQLS_ENTRY(558, "24000");
			SQLS_ENTRY(559, "24000");
			SQLS_ENTRY(6235, "24000");
			SQLS_ENTRY(583, "24000");
			SQLS_ENTRY(6259, "24000");
			SQLS_ENTRY(6260, "24000");
			SQLS_ENTRY(562, "24000");
			SQLS_ENTRY(277, "25000");	/* Invalid transaction state */
			SQLS_ENTRY(611,"25000");
			SQLS_ENTRY(3906,"25000");
			SQLS_ENTRY(3908,"25000");
			SQLS_ENTRY(6401,"25000");
			SQLS_ENTRY(627, "25000");
			SQLS_ENTRY(628, "25000");
			SQLS_ENTRY(641, "25000");
			SQLS_ENTRY(642, "25000");
			SQLS_ENTRY(1276, "25000");
			SQLS_ENTRY(3902, "25000");
			SQLS_ENTRY(3903, "25000");
			SQLS_ENTRY(6104, "37000");	/* Syntax error or access violation */
			SQLS_ENTRY(102, "37000");
			SQLS_ENTRY(137, "37000");
			SQLS_ENTRY(7327, "37000");
			SQLS_ENTRY(201, "37000");
			SQLS_ENTRY(257, "37000");
			SQLS_ENTRY(2812, "37000");
			SQLS_ENTRY(2526, "37000");
			SQLS_ENTRY(11021, "37000");
			SQLS_ENTRY(229, "42000");	/* Syntax/Access violation */
			SQLS_ENTRY(230, "42000");
			SQLS_ENTRY(262, "42000");
			SQLS_ENTRY(4602, "42000");
			SQLS_ENTRY(4603, "42000");
			SQLS_ENTRY(4608, "42000");
			SQLS_ENTRY(10306, "42000");
			SQLS_ENTRY(10323, "42000");
			SQLS_ENTRY(10330, "42000");
			SQLS_ENTRY(10331, "42000");
			SQLS_ENTRY(10332, "42000");
			SQLS_ENTRY(11110, "42000");
			SQLS_ENTRY(11113, "42000");
			SQLS_ENTRY(11118, "42000");
			SQLS_ENTRY(11121, "42000");
			SQLS_ENTRY(17222, "42000");
			SQLS_ENTRY(17223, "42000");
			SQLS_ENTRY(18350, "42000");
			SQLS_ENTRY(18351, "42000");
			SQLS_ENTRY(113, "42000");
			SQLS_ENTRY(2714, "42S01");	/* Table or view already exists */
			SQLS_ENTRY(208, "42S02");	/* Table or view not found */
			SQLS_ENTRY(3701, "42S02");
			SQLS_ENTRY(1913, "42S11");	/* Index already exists */
			SQLS_ENTRY(307, "42S12");	/* Index not found */
			SQLS_ENTRY(7010, "42S12");
			SQLS_ENTRY(18091, "42S12");
			SQLS_ENTRY(1921, "42S21");	/* Column already exists */
			SQLS_ENTRY(1720, "42S22");	/* Column not found */
			SQLS_ENTRY(207, "42S22");
			SQLS_ENTRY(4934, "42S22");
			SQLS_ENTRY(18117, "42S22");
		}
	}

	if (p != NULL && (q = strdup(p)) != NULL) {
		/* FIXME correct here ?? */
		/* Convert known ODBC 3.x states listed above to 2.x */
		if (memcmp(q, "42S", 3) == 0)
			memcpy(q, "S00", 3);

		return q;
	}
	return NULL;
}

BCPCOLDATA *
tds_alloc_bcp_column_data(unsigned int column_size)
{
	BCPCOLDATA *coldata;

	TEST_MALLOC(coldata, BCPCOLDATA);

	if (column_size > 4 * 1024)
		column_size = 4 * 1024;
	TEST_CALLOC(coldata->data, unsigned char, column_size);

	return coldata;
Cleanup:
	tds_free_bcp_column_data(coldata);
	return NULL;
}

void
tds_free_bcp_column_data(BCPCOLDATA * coldata)
{
	if (!coldata)
		return;

	free(coldata->data);
	free(coldata);
}

TDSBCPINFO *
tds_alloc_bcpinfo(void)
{
	TDSBCPINFO *bcpinfo;

	TEST_MALLOC(bcpinfo, TDSBCPINFO);

	tds_dstr_init(&bcpinfo->hint);
	tds_dstr_init(&bcpinfo->tablename);

	return bcpinfo;
Cleanup:
	return NULL;
}

void
tds_deinit_bcpinfo(TDSBCPINFO *bcpinfo)
{
	tds_dstr_free(&bcpinfo->hint);
	tds_dstr_free(&bcpinfo->tablename);
	TDS_ZERO_FREE(bcpinfo->insert_stmt);
	tds_free_results(bcpinfo->bindinfo);
	bcpinfo->bindinfo = NULL;
	TDS_ZERO_FREE(bcpinfo->sybase_colinfo);
	bcpinfo->sybase_count = 0;
}

void
tds_free_bcpinfo(TDSBCPINFO *bcpinfo)
{
	if (bcpinfo) {
		tds_deinit_bcpinfo(bcpinfo);
		free(bcpinfo);
	}
}

/**
 * Reallocate a pointer and update it if success
 * \param pp pointer to pointer to be reallocated
 * \param new_size new size to be allocated
 * \return new pointer allocated, NULL on failure
 */
void *
tds_realloc(void **pp, size_t new_size)
{
	void *p;

	/* some implementation of malloc/realloc does not like size==0 */
	if (!new_size)
		new_size = 1;

	/* use malloc if not allocated before, some implementations require it */
	if (*pp)
		p = realloc(*pp, new_size);
	else
		p = malloc(new_size);

	/* update pointer only on success */
	if (p)
		*pp = p;

	return p;
}

void
tds_deinit_tvp(TDS_TVP *table)
{
	TDS_TVP_ROW *tvp_row, *next_row;

	free(table->schema);
	table->schema = NULL;
	free(table->name);
	table->name = NULL;
	tds_free_param_results(table->metadata);
	table->metadata = NULL;
	for (tvp_row = table->row; tvp_row != NULL; tvp_row = next_row) {
		next_row = tvp_row->next;
		tds_free_tvp_row(tvp_row);
		free(tvp_row);
	}
	table->row = NULL;
}

/** @} */
