/*
OpenIO SDS cluster
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

#include <stdlib.h>
#include <string.h>

#include <metautils/lib/metacomm.h>
#include <cluster/conscience/conscience.h>
#include <cluster/conscience/conscience_srvtype.h>
#include <cluster/module/module.h>

#include "./agent.h"
#include "./asn1_request_worker.h"
#include "./namespace_get_task_worker.h"
#include "./services_workers.h"
#include "./task.h"
#include "./task_scheduler.h"

#define TASK_ID "services_task_get_types"

struct session_data_s {
	GSList *names;
	gchar ns[LIMIT_LENGTH_NSNAME];
	gchar task_id[sizeof(TASK_ID)+1+LIMIT_LENGTH_NSNAME];
};

static struct conscience_srvtype_s*
get_srvtype(const gchar *ns, const gchar *type, GError **error)
{
	namespace_data_t *ns_data;
	ns_data = g_hash_table_lookup( namespaces, ns);
	if (!ns_data) {
		GSETERROR(error,"Namespace %s disappeared", ns);
		return NULL;
	}
	if (!ns_data->conscience || !ns_data->configured) {
		GSETERROR(error,"Namespace not yet configured");
		return NULL;
	}
	return conscience_get_srvtype( ns_data->conscience, error, type, MODE_AUTOCREATE);
}

static int
parse_names_list( worker_t *worker, GError **error )
{
	struct session_data_s *sdata;
	asn1_session_t *asn1_session;


	asn1_session = (asn1_session_t*) worker->data.session;
	sdata = asn1_worker_get_session_data(worker);

	if (!asn1_session->resp_body || !asn1_session->resp_body_size) {
		DEBUG("[task_id=%s] No body received", sdata->task_id);
	} else {
		GSList *names = NULL;
		gint rc = strings_unmarshall(&names, asn1_session->resp_body,
				asn1_session->resp_body_size, error);
		if (!rc) {
			GSETERROR(error, "Failed to unmarshall the service_types name list");
			return 0;
		} else {
			DEBUG("[task_id=%s] Received %d names", sdata->task_id, g_slist_length( names ));
			sdata->names = metautils_gslist_precat(sdata->names, names);
		}
	}

	return(1);
}

static int
asn1_final_handler( worker_t *worker, GError **error)
{

	struct session_data_s *sdata = asn1_worker_get_session_data( worker );
	if (!sdata) {
		GSETERROR(error,"Invalid worker (NULL session data)");
		return 0;
	}
	
	task_done(sdata->task_id);
	DEBUG("[task_id=%s] terminated", sdata->task_id);

	for (GSList *l=sdata->names; l ;l=l->next) {
		if (!l->data)
			continue;
		gchar *type_name = l->data;
		if (!get_srvtype( sdata->ns, type_name, error ))
			WARN("[task_id=%s] Failed to init service type [%s/%s] : %s",
				sdata->task_id, sdata->ns, type_name, gerror_get_message(*error));
	}

	return 1;
}

static int
asn1_error_handler( worker_t *worker, GError **error )
{
	struct session_data_s *sdata;
	sdata = asn1_worker_get_session_data( worker );
	if (sdata) {
		GSETERROR(error, "[task_id=%s] Failed to request volume list", sdata->task_id);
		task_done(sdata->task_id);
	}
	else
		GSETERROR(error, "[task_id="TASK_ID".?] Failed to request volume list");
	return(0);
}

static void
sdata_cleaner(struct session_data_s *sdata)
{
	if (!sdata)
		return;
	task_done(sdata->task_id);
	if (sdata->names) {
		g_slist_foreach( sdata->names, g_free1, NULL);
		g_slist_free(sdata->names);
		sdata->names = NULL;
	}
	memset(sdata,0x00,sizeof(struct session_data_s));
	g_free(sdata);
}

static int
task_worker(gpointer p, GError **error )
{

	struct namespace_data_s *ns_data = get_namespace((gchar*)p, error);
	if (!ns_data) {
		GSETERROR(error,"Namespace [%s] not (yet) managed", (gchar*)p);
		return 0;
	}

	struct session_data_s *sdata = g_malloc0(sizeof(struct session_data_s));
	g_strlcpy(sdata->ns, ns_data->name, sizeof(sdata->ns)-1);
	g_snprintf(sdata->task_id,sizeof(sdata->task_id), TASK_ID".%s",ns_data->name);

	worker_t *asn1_worker = create_asn1_worker(&(ns_data->ns_info.addr), NAME_MSGNAME_CS_GET_SRVNAMES);
	asn1_worker_set_handlers(asn1_worker, parse_names_list, asn1_error_handler, asn1_final_handler);
	asn1_worker_set_session_data(asn1_worker, sdata, (GDestroyNotify)sdata_cleaner);

	if (!asn1_request_worker(asn1_worker, error)) {
		g_free(sdata);
		free_asn1_worker(asn1_worker,TRUE);
		GSETERROR(error, "[task_id=%s.%s] Failed to send asn1 request", TASK_ID, ns_data->name);
		return 0;
	}

	return 1;
}

/* ------------------------------------------------------------------------- */

NAMESPACE_TASK_CREATOR(task_starter,TASK_ID,task_worker,period_get_srvtype);

int
services_task_get_types(GError **error)
{

	task_t *task = create_task(2, TASK_ID);
	task->task_handler = task_starter;

	if (!add_task_to_schedule(task, error)) {
		GSETERROR(error, "Failed to add srv_list_get task to scheduler");
		g_free(task);
		return(0);
	}

	INFO("Task started: %s", __FUNCTION__);
	return 1;
}

