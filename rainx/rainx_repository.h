/*
OpenIO SDS rainx
Copyright (C) 2014 Worldine, original work as part of Redcurrant
Copyright (C) 2015 OpenIO, modified as part of OpenIO Software Defined Storage

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as
published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef OIO_SDS__rainx__rainx_repository_h
# define OIO_SDS__rainx__rainx_repository_h 1

#include <httpd.h>
#include <http_config.h>
#include <http_log.h>

#include <apr.h>
#include <apr_strings.h>
#include <apr_shm.h>
#include <apr_global_mutex.h>
#include <mod_dav.h>

#include <librain.h>

#include <metautils/lib/metautils.h>
#include <rawx-lib/src/rawx.h>
#include <rawx-lib/src/compression.h>
#include <rainx/rainx_config.h>

// FIXME: first-level separator should be ';' or ',' but not '|'
#define RAWXLIST_SEPARATOR "|"
#define RAWXLIST_SEPARATOR2 ";"
#define MAX_REPLY_MESSAGE_SIZE 1024
#define INIT_REQ_STATUS -1

typedef int func0(int, int, int, const char*);
typedef char* func1(char**, char**, int, int, int, const char*);
typedef char** func2(char*, int, int, int, const char*);

struct req_params_store {
	const dav_resource* resource;

	apr_thread_t *thd_arr;
    apr_threadattr_t *thd_attr;

	char* service_address;

	char* data_to_send;
	int data_to_send_size;

	char* header;
	char* req_type;
	char* reply;
	apr_status_t req_status;
	apr_pool_t *pool;
};

/* context needed to identify a resource */
struct dav_resource_private {
	apr_pool_t *pool;        /* memory storage pool associated with request */
	request_rec *request;

	struct content_textinfo_s content;
	struct chunk_textinfo_s chunk;

	char *namespace; /* Namespace name, in case of VNS */

	struct rain_encoding_s rain_params;

	/* List of rawx services
	 * (i.e http://ip:port/DATA/NS/machine/volume/XX/XX/CID|...).
	 * Must contain k + m addresses */
	char** rawx_list;

	int current_rawx; /* Index of the current rawx in the rawx list */
	/* Number of bytes until the current chunk buffer is totally filled */
	int current_chunk_remaining;
	/* The list (ip:port/chunk_id|stored_size|md5_digest;...) of actual
	 * metachunks stored on the rawx to put in the response header */
	char* response_chunk_list;

	/** TRUE if user asked for on-the-fly reconstruction */
	int on_the_fly;
};

struct dav_stream {
	const dav_resource *r;
	apr_pool_t *pool;
	int original_data_size; /* Size of the original data */
	char* original_data; /* Buffer where the entire received data is stored */
	char* chunk_start_ptr; /* Pointer to the beginning of the current chunk */
	char* chunk_end_ptr; /* Pointer to the end of the current chunk */
	int original_data_stored; /* Amount of data currently stored in 'original_data' */

	struct req_params_store** data_put_params; /* List of thread references for data */

	GChecksum *md5;
};

#endif /*OIO_SDS__rainx__rainx_repository_h*/
