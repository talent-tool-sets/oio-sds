/*
OpenIO SDS cluster
Copyright (C) 2014 Worldine, original work as part of Redcurrant
Copyright (C) 2015 OpenIO, modified as part of OpenIO Software Defined Storage

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 3.0 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <metautils/lib/metautils.h>
#include <cluster/module/module.h>

#include "gridcluster.h"
#include "message.h"

#define MAX_REQ_LENGTH 1024
#define CONNECT_TIMEOUT 5000
#define SOCKET_TIMEOUT 5000
#define BUF (wrkParam+writen)
#define LEN (sizeof(wrkParam)-writen-1)
#define MANAGE_ERROR(Req,Resp,Error) do {\
	if (Resp.data_size > 0 && Resp.data)\
		GSETERROR(Error, "Error from agent : %.*s", Resp.data_size, (char*)(Resp.data));\
	else\
		GSETERROR(Error, "Error from agent : (no response)");\
} while (0)
#define NS_WORM_OPT_NAME "worm"
#define NS_CONTAINER_MAX_SIZE_NAME "container_max_size"
#define NS_STORAGE_POLICY_NAME "storage_policy"
#define NS_CHUNK_SIZE_NAME "chunk_size"
#define NS_WORM_OPT_VALUE_ON "on"
#define NS_COMPRESS_OPT_NAME "compression"
#define NS_COMPRESS_OPT_VALUE_ON "on"

gboolean oio_cluster_allow_agent = TRUE;
gboolean oio_cluster_allow_proxy = TRUE;

static void
clear_request_and_reply( request_t *req, response_t *resp )
{
	if (req) {
		oio_str_clean (&req->cmd);
		oio_str_clean (&req->arg);
	}
	if (resp->data) g_free (resp->data);
	resp->data = NULL;
}

/* -------------------------------------------------------------------------- */

GError *
conscience_remote_get_namespace (const char *cs, namespace_info_t **out)
{
	MESSAGE req = metautils_message_create_named(NAME_MSGNAME_CS_GET_NSINFO);
	GByteArray *gba = NULL;
	GError *err = gridd_client_exec_and_concat (cs, CS_CLIENT_TIMEOUT,
			message_marshall_gba_and_clean(req), &gba);
	if (err) {
		g_assert (gba == NULL);
		g_prefix_error(&err, "request: ");
		return err;
	}

	*out = namespace_info_unmarshall(gba->data, gba->len, &err);
	g_byte_array_unref (gba);
	if (*out) return NULL;
	GSETERROR(&err, "Decoding error");
	return err;
}

GError *
conscience_remote_get_services(const char *cs, const char *type, gboolean full,
		GSList **out)
{
	EXTRA_ASSERT(type != NULL);
	MESSAGE req = metautils_message_create_named(NAME_MSGNAME_CS_GET_SRV);
	metautils_message_add_field_str (req, NAME_MSGKEY_TYPENAME, type);
	if (full)
		metautils_message_add_field_str(req, NAME_MSGKEY_FULL, "1");
	return gridd_client_exec_and_decode (cs, CS_CLIENT_TIMEOUT,
			message_marshall_gba_and_clean(req), out, service_info_unmarshall);
}

GError *
conscience_remote_get_types(const char *cs, GSList **out)
{
	MESSAGE req = metautils_message_create_named (NAME_MSGNAME_CS_GET_SRVNAMES);
	return gridd_client_exec_and_decode (cs, CS_CLIENT_TIMEOUT,
			message_marshall_gba_and_clean(req), out, strings_unmarshall);
}

GError *
conscience_remote_push_services(const char *cs, GSList *ls)
{
	MESSAGE req = metautils_message_create_named (NAME_MSGNAME_CS_PUSH_SRV);
	metautils_message_add_body_unref (req, service_info_marshall_gba (ls, NULL));
	return gridd_client_exec (cs, CS_CLIENT_TIMEOUT,
			message_marshall_gba_and_clean(req));
}

GError*
conscience_remote_remove_services(const char *cs, const char *type, GSList *ls)
{
	MESSAGE req = metautils_message_create_named (NAME_MSGNAME_CS_RM_SRV);
	if (ls)
		metautils_message_add_body_unref (req, service_info_marshall_gba (ls, NULL));
	if (type) metautils_message_add_field_str (req, NAME_MSGKEY_TYPENAME, type);
	return gridd_client_exec (cs, CS_CLIENT_TIMEOUT,
			message_marshall_gba_and_clean(req));
}

/* -------------------------------------------------------------------------- */

static GError *
conscience_agent_get_namespace(const char *ns, struct namespace_info_s **out)
{
	g_assert (ns != NULL);
	g_assert (out != NULL);
	*out = NULL;

	GError *err = NULL;
	request_t req;
	response_t resp;
	memset(&req, 0, sizeof(request_t));
	memset(&resp, 0, sizeof(response_t));
	req.cmd = g_strdup(MSG_GETNS);
	req.arg = g_strdup(ns);
	req.arg_size = strlen(req.arg);

	if (!send_request(&req, &resp, &err))
		g_prefix_error (&err, "agent request error (%s): ", req.cmd);
	else if (resp.status != STATUS_OK)
		MANAGE_ERROR(req,resp,&err);
	else
		*out = namespace_info_unmarshall(resp.data, resp.data_size, &err);

	clear_request_and_reply(&req,&resp);
	return err;
}

static GError *
conscience_agent_get_types(const char *ns, GSList **out)
{
	g_assert (ns != NULL);
	g_assert (out != NULL);
	*out = NULL;

	GError *err = NULL;
	request_t req;
	response_t resp;
	memset(&req, 0, sizeof(request_t));
	memset(&resp, 0, sizeof(response_t));
	req.cmd = g_strdup(MSG_SRVTYPE_LST);
	req.arg = g_strdup(ns);
	req.arg_size = strlen(req.arg);

	if (!send_request(&req, &resp, &err))
		g_prefix_error (&err, "agent request error (%s): ", req.cmd);
	else if (resp.status != STATUS_OK)
		MANAGE_ERROR(req,resp,&err);
	else 
		strings_unmarshall(out, resp.data, resp.data_size, &err);

	clear_request_and_reply(&req,&resp);
	return err;
}

static GError *
conscience_agent_get_services(const char *ns, const char *type, GSList **out)
{
	g_assert (ns != NULL);
	g_assert (type != NULL);
	g_assert (out != NULL);
	*out = NULL;

	GError *err = NULL;
	request_t req;
	response_t resp;
	memset(&req, 0, sizeof(request_t));
	memset(&resp, 0, sizeof(response_t));
	req.cmd = g_strdup(MSG_SRV_LST);
	req.arg = g_strdup_printf("%s:%s", ns, type);
	req.arg_size = strlen(req.arg);

	if (!send_request(&req, &resp, &err))
		g_prefix_error (&err, "agent request error (%s): ", req.cmd);
	else if (resp.status != STATUS_OK)
		MANAGE_ERROR(req,resp,&err);
	else
		service_info_unmarshall(out, resp.data, resp.data_size,&err);

	clear_request_and_reply(&req,&resp);
	return err;
}

static GError *
conscience_agent_push_service (const char *ns, struct service_info_s *si)
{
	g_assert (ns != NULL);
	g_assert (si != NULL);

	GSList ls = {.data=si, .next=NULL};
	GByteArray *gba = service_info_marshall_gba (&ls, NULL);
	if (!gba)
		return NEWERROR(CODE_INTERNAL_ERROR, "serialisation error");

	GError *err = NULL;
	request_t req;
	response_t resp;
	memset(&req, 0, sizeof(request_t));
	memset(&resp, 0, sizeof(response_t));
	req.cmd = g_strdup(MSG_SRV_PSH);
	req.arg = (char*)gba->data;
	req.arg_size = gba->len;
	g_byte_array_free(gba,FALSE);

	if (!send_request(&req, &resp, &err))
		g_prefix_error (&err, "agent request error (%s): ", req.cmd);
	else if (resp.status != STATUS_OK)
		MANAGE_ERROR(req,resp,&err);

	clear_request_and_reply(&req,&resp);
	return err;
}

static GError *
conscience_agent_remove_services(const char *ns, const char *type)
{
	g_assert (ns != NULL);
	g_assert (type != NULL);

	GError *err = NULL;
	request_t req;
	response_t resp;
	memset(&req, 0, sizeof(request_t));
	memset(&resp, 0, sizeof(response_t));
	req.cmd = g_strdup(MSG_SRV_CLR);
	req.arg = g_strdup_printf("%s:%s",ns,type);
	req.arg_size = strlen(req.arg);

	if (!send_request(&req, &resp, &err))
		g_prefix_error (&err, "agent request failed (%s): ", req.cmd);
	else if (resp.status != STATUS_OK)
		MANAGE_ERROR(req,resp,&err);

	clear_request_and_reply(&req,&resp);
	return err;
}

/* -------------------------------------------------------------------------- */

GError *
conscience_get_namespace (const char *ns, struct namespace_info_s **out)
{
	g_assert (ns != NULL);
	g_assert (out != NULL);
	*out = NULL;

	if (oio_cluster_allow_agent && gridagent_available())
		return conscience_agent_get_namespace (ns, out);

	gchar *cs = gridcluster_get_conscience(ns);
	STRING_STACKIFY(cs);
	if (!cs)
		return NEWERROR(CODE_NAMESPACE_NOTMANAGED, "No such NS");
	return conscience_remote_get_namespace(cs, out);
}

GError *
conscience_get_types (const char *ns, GSList **out)
{
	g_assert (ns != NULL);
	g_assert (out != NULL);
	*out = NULL;

	if (oio_cluster_allow_agent && gridagent_available())
		return conscience_agent_get_types (ns, out);

	if (oio_cluster_allow_proxy) {
		struct oio_cs_client_s *cs = oio_cs_client__create_proxied (ns);
		GSList *l = NULL;
		void _on_type (const char *srvtype) {
			l = g_slist_prepend (l, g_strdup (srvtype));
		}
		GError *err = oio_cs_client__list_types (cs, _on_type);
		oio_cs_client__destroy (cs);
		if (err) {
			g_assert (l == NULL);
			return err;
		}
		*out = l;
		return NULL;
	}

	gchar *cs = gridcluster_get_conscience(ns);
	STRING_STACKIFY(cs);
	if (!cs)
		return NEWERROR(CODE_NAMESPACE_NOTMANAGED, "No such NS");
	return conscience_remote_get_types(cs, out);
}

GError *
conscience_get_services (const char *ns, const char *type, GSList **out)
{
	g_assert (ns != NULL);
	g_assert (type != NULL);
	g_assert (out != NULL);
	*out = NULL;

	if (oio_cluster_allow_agent && gridagent_available())
		return conscience_agent_get_services (ns, type, out);

	if (oio_cluster_allow_proxy) {
		struct oio_cs_client_s *cs = oio_cs_client__create_proxied (ns);
		GSList *l = NULL;
		void _on_reg (const struct oio_cs_registration_s *reg, int score) {
			struct service_info_s *si = g_malloc0 (sizeof(struct service_info_s));
			g_strlcpy (si->ns_name, ns, sizeof(si->ns_name));
			g_strlcpy (si->type, type, sizeof(si->type));
			si->tags = g_ptr_array_new ();
			si->score.value = score;
			service_tag_set_value_string (service_info_ensure_tag (
						si->tags, "tag.id"), reg->id);
			grid_string_to_addrinfo (reg->url, &si->addr);
			for (const char * const *pp = reg->kv_tags;
					pp && *pp && *(pp+1);
					pp += 2) {
				service_tag_set_value_string (service_info_ensure_tag(
						si->tags, *pp), *(pp+1));
			}
			l = g_slist_prepend (l, si);
		}
		GError *err = oio_cs_client__list_services (cs, type, _on_reg);
		oio_cs_client__destroy (cs);
		if (err) {
			g_assert (l == NULL);
			return err;
		}
		*out = l;
		return NULL;
	}

	gchar *cs = gridcluster_get_conscience(ns);
	STRING_STACKIFY (cs);
	if (!cs)
		return NEWERROR(CODE_NAMESPACE_NOTMANAGED, "No such NS");
	return conscience_remote_get_services(cs, type, FALSE, out);
}

GError *
conscience_push_service (const char *ns, struct service_info_s *si)
{
	g_assert (ns != NULL);
	g_assert (si != NULL);

	if (oio_cluster_allow_agent && gridagent_available())
		return conscience_agent_push_service (ns, si);

	if (oio_cluster_allow_proxy) {
		struct oio_cs_client_s *cs = oio_cs_client__create_proxied (ns);

		/* convert the <service_info_t> into a <struct oio_cs_registration_s> */
		gchar strurl[STRLEN_ADDRINFO], *srvkey, **kv;
		GPtrArray *tmp = g_ptr_array_new ();
		if (si->tags) for (guint i=0; i<si->tags->len ;++i) {
			struct service_tag_s *tag = si->tags->pdata[i];
			gchar v[256];
			service_tag_to_string (tag, v, sizeof(v));
			g_ptr_array_add (tmp, g_strdup(tag->name));
			g_ptr_array_add (tmp, g_strdup(v));
		}
		g_ptr_array_add (tmp, NULL);
		kv = (gchar**) g_ptr_array_free (tmp, FALSE);
		grid_addrinfo_to_string (&si->addr, strurl, sizeof(strurl));
		srvkey = service_info_key (si);
		struct oio_cs_registration_s reg =
			{ .id = srvkey, .url = strurl, .kv_tags = (const char * const *)kv };

		GError *err;
		if (si->score.value == SCORE_UNSET)
			err = oio_cs_client__register_service (cs, si->type, &reg);
		else if (si->score.value == SCORE_UNLOCK)
			err = oio_cs_client__unlock_service (cs, si->type, reg.id);
		else
			err = oio_cs_client__lock_service (cs, si->type, &reg,
					si->score.value);

		g_free (srvkey);
		g_strfreev (kv);
		oio_cs_client__destroy (cs);
		return err;
	}

	gchar *cs = gridcluster_get_conscience(ns);
	STRING_STACKIFY(cs);
	if (!cs)
		return NEWERROR(CODE_BAD_REQUEST, "Unknown namespace/conscience");
	GSList ls = {.data=si, .next=NULL};
	return conscience_remote_push_services (cs, &ls);
}

GError *
conscience_remove_services(const char *ns, const char *type)
{
	g_assert (ns != NULL);
	g_assert (type != NULL);

	if (oio_cluster_allow_agent && gridagent_available())
		conscience_agent_remove_services (ns, type);

	if (oio_cluster_allow_proxy) {
		struct oio_cs_client_s *cs = oio_cs_client__create_proxied (ns);
		GError *err = oio_cs_client__flush_services (cs, type);
		oio_cs_client__destroy (cs);
		return err;
	}

	gchar *cs = gridcluster_get_conscience(ns);
	STRING_STACKIFY(cs);
	if (!cs)
		return NEWERROR(CODE_NAMESPACE_NOTMANAGED, "Unknown namespace/conscience");
	return conscience_remote_remove_services (cs, type, NULL);
}

/* -------------------------------------------------------------------------- */

GError *
register_namespace_service(const struct service_info_s *si)
{
	struct service_info_s *si_copy = service_info_dup(si);
	si_copy->score.value = SCORE_UNSET;
	si_copy->score.timestamp = oio_ext_real_time () / G_TIME_SPAN_SECOND;
	metautils_srvinfo_ensure_tags (si_copy);
	GError *err = conscience_push_service (si->ns_name, si_copy);
	service_info_clean(si_copy);
	return err;
}

GSList*
list_local_services(GError **error)
{
	request_t req;
	response_t resp;
	GSList *srv_list = NULL;

	memset(&req, 0, sizeof(request_t));
	memset(&resp, 0, sizeof(response_t));
	req.cmd = g_strdup(MSG_LSTSVC);

	if (!send_request(&req, &resp, error)) {
		GSETERROR(error, "Request list services failed");
		clear_request_and_reply(&req,&resp);
		return NULL;
	}

	if (resp.status != STATUS_OK) {
		MANAGE_ERROR(req,resp,error);
		clear_request_and_reply(&req,&resp);
		return NULL;
	}

	if (!service_info_unmarshall(&srv_list,resp.data, resp.data_size,error)) {
		GSETERROR(error,"Invalid answer from the agent");
		clear_request_and_reply(&req,&resp);
		return NULL;
	}

	return srv_list;
}

GSList*
list_tasks(GError **error)
{
	request_t req;
	response_t resp;

	/* Build request */
	memset(&req, 0, sizeof(request_t));
	memset(&resp, 0, sizeof(response_t));
	req.cmd = g_strdup(MSG_LSTTASK);

	if (!send_request(&req, &resp, error)) {
		GSETERROR(error, "Request list tasks failed");
		clear_request_and_reply(&req,&resp);
		return NULL;
	}

	if (resp.status == STATUS_OK) {
		size_t size_read = 0;
		struct task_s task;
		GSList *task_list = NULL;
		while (size_read < resp.data_size) {
			memset(&task, 0, sizeof(task));

			memcpy(task.id, resp.data + size_read, sizeof(task.id));
			size_read += sizeof(task.id);

			memcpy(&(task.period), resp.data + size_read, sizeof(task.period));
			size_read += sizeof(task.period);

			memcpy(&(task.busy), resp.data + size_read, sizeof(task.busy));
			size_read += sizeof(task.busy);

			task_list = g_slist_prepend(task_list, g_memdup(&task, sizeof(struct task_s)));
		}

		clear_request_and_reply(&req,&resp);
		return(task_list);

	}

	MANAGE_ERROR(req,resp,error);
	clear_request_and_reply(&req,&resp);
	return NULL;
}

/* -------------------------------------------------------------------------- */

void
metautils_srvinfo_ensure_tags (struct service_info_s *si)
{
	if (!si->tags)
		return ;

	if (!service_info_get_tag (si->tags, "stat.cpu"))
		service_tag_set_value_float(service_info_ensure_tag (
					si->tags, "stat.cpu"), 100.0 * oio_sys_cpu_idle ());

	gchar vol[512];
	struct service_tag_s *tag = service_info_get_tag (si->tags, "tag.vol");
	if (tag) {
		if (service_tag_get_value_string (tag, vol, sizeof(vol), NULL)) {
			if (!service_info_get_tag(si->tags, "stat.io"))
				service_tag_set_value_float (service_info_ensure_tag(
							si->tags, "stat.io"), 100.0 * oio_sys_io_idle (vol));
			if (!service_info_get_tag(si->tags, "stat.space"))
				service_tag_set_value_float (service_info_ensure_tag (
							si->tags, "stat.space"), 100.0 * oio_sys_space_idle (vol));
		}
	}
}

/* -------------------------------------------------------------------------- */

static gint64
_gba_to_int64(GByteArray *gba, gboolean def)
{
	if (!gba)
		return def;
	gchar *str = g_alloca(gba->len + 1);
	memset(str, 0, gba->len + 1);
	memcpy(str, gba->data, gba->len);
	return g_ascii_strtoll(str, NULL, 10);
}

static gboolean
_gba_to_bool(GByteArray *gba, gboolean def)
{
	if (!gba || !gba->data || !gba->len)
		return def;
	if (!gba->data[ gba->len - 1 ])
		return metautils_cfg_get_bool((gchar*)gba->data, def);
	gchar *str = g_alloca(gba->len + 1);
	memset(str, 0, gba->len + 1);
	memcpy(str, gba->data, gba->len);
	return metautils_cfg_get_bool(str, def);
}

static GByteArray *
namespace_param_gba(const namespace_info_t* ns_info, const gchar *ns_name,
		const gchar *param_name)
{
	return namespace_info_get_srv_param_gba(ns_info, ns_name, NULL, param_name);
}

gchar*
gridcluster_get_nsinfo_strvalue(struct namespace_info_s *nsinfo,
		const gchar *key, const gchar *def)
{
	GByteArray *value;

	if (!nsinfo || !nsinfo->options)
		return g_strdup(def);

	value = g_hash_table_lookup(nsinfo->options, key);
	if (!value)
		return g_strdup(def);

	return g_strndup((gchar*)value->data, value->len);
}

gint64
gridcluster_get_nsinfo_int64(struct namespace_info_s *nsinfo,
		const gchar* key, gint64 def)
{
	return namespace_info_get_srv_param_i64(nsinfo, NULL, NULL, key, def);
}

static gsize
namespace_get_size(namespace_info_t *ns_info, const gchar *name, gsize def)
{
	return (gsize) gridcluster_get_nsinfo_int64(ns_info, name, def);
}

gboolean
namespace_in_worm_mode(namespace_info_t* ns_info)
{
	GByteArray *val = namespace_param_gba(ns_info, NULL, NS_WORM_OPT_NAME);
	return _gba_to_bool(val, FALSE);
}

gint64
namespace_container_max_size(namespace_info_t* ns_info)
{
	GByteArray *val = namespace_param_gba(ns_info, NULL, NS_CONTAINER_MAX_SIZE_NAME);
	return _gba_to_int64(val, -1);
}

gint64
namespace_chunk_size(const namespace_info_t* ns_info, const char *ns_name)
{
	GByteArray *val = namespace_param_gba(ns_info, ns_name,
			NS_CHUNK_SIZE_NAME);
	return _gba_to_int64(val, ns_info->chunk_size);
}

gchar *
namespace_storage_policy(const namespace_info_t* ns_info, const char *ns_name)
{
	GByteArray *gba = namespace_param_gba(ns_info, ns_name,
			NS_STORAGE_POLICY_NAME);
	return !gba ? NULL : g_strndup((gchar*)gba->data, gba->len);
}

gchar*
namespace_storage_policy_value(const namespace_info_t *ns_info, const gchar *wanted_policy)
{
	const gchar *policy_to_lookup = wanted_policy ?
			wanted_policy : namespace_storage_policy(ns_info, ns_info->name);

	if (!ns_info || ns_info->storage_policy)
		return NULL;

	GByteArray *gba = g_hash_table_lookup(ns_info->storage_policy, policy_to_lookup);

	if (!wanted_policy)
		g_free((gpointer)policy_to_lookup);

	return !gba ? NULL : g_strndup((gchar*)gba->data, gba->len);
}

static gchar*
_get_token(const gchar *colon_separated_tokens, const guint token_rank)
{
	gchar **tokens = g_strsplit(colon_separated_tokens, ":", 0);
	gchar *token_wanted = NULL;

	if (g_strv_length(tokens) < token_rank) {
		ERROR("Cannot split string [%s] into %i ':'-separated tokens.", colon_separated_tokens, token_rank);
		goto end;
	}

	token_wanted = g_strdup(tokens[token_rank]);

end:
	if (tokens)
		g_strfreev(tokens);

	return token_wanted;
}

static gchar*
_get_data_security_id(const gchar *storage_policy_value)
{
	gchar *data_sec_id = _get_token(storage_policy_value, 1);

	if (!data_sec_id) {
		WARN("Storage policy configuration seems to be wrong: [%s]"
				" Correct pattern is STG_CLASS:DATA_SEC:DATA_THREAT",
				storage_policy_value ? storage_policy_value : "NULL");
	}

	return data_sec_id;
}

gchar*
namespace_data_security_value(const namespace_info_t *ns_info, const gchar *wanted_policy)
{
	gchar *storage_policy_value = namespace_storage_policy_value(ns_info, wanted_policy);
	gchar *data_sec_id = _get_data_security_id(storage_policy_value);
	GByteArray *data_sec_val = NULL;
	gchar str_data_sec_val[LIMIT_LENGTH_STGPOLICY];

	if (storage_policy_value && data_sec_id) {
		data_sec_val = g_hash_table_lookup(ns_info->data_security, data_sec_id);
	}

	if (!data_sec_val) {
		WARN("Cannot find data security with id [%s] (namespace [%s], wanted policy [%s])",
				data_sec_id, ns_info->name, wanted_policy);
	}

	if (data_sec_id)
		g_free(data_sec_id);
	if (storage_policy_value)
		g_free(storage_policy_value);

	metautils_gba_data_to_string(data_sec_val, str_data_sec_val, LIMIT_LENGTH_STGPOLICY);
	return g_strdup(str_data_sec_val);
}

gboolean
namespace_is_storage_policy_valid(const namespace_info_t* ns_info, const gchar *storage_policy)
{
	if (!ns_info || !ns_info->storage_policy || !storage_policy)
		return FALSE;
	if (!g_hash_table_lookup(ns_info->storage_policy, storage_policy))
		return FALSE;
	return TRUE;
}

gboolean
namespace_in_compression_mode(namespace_info_t* ns_info)
{
	if (!ns_info || !ns_info->options)
		return FALSE;
	GByteArray *val = namespace_param_gba(ns_info, NULL, NS_COMPRESS_OPT_NAME);
	gboolean res = _gba_to_bool(val, FALSE);
	return res;
}

gsize
namespace_get_autocontainer_src_offset(namespace_info_t* ns_info)
{
	return namespace_get_size(ns_info, "FLATNS_hash_offset", 0);
}

gsize
namespace_get_autocontainer_src_size(namespace_info_t* ns_info)
{
	return namespace_get_size(ns_info, "FLATNS_hash_size", 0);
}

gsize
namespace_get_autocontainer_dst_bits(namespace_info_t* ns_info)
{
	return namespace_get_size(ns_info, "FLATNS_hash_bitlength", 17);
}

gint64
gridcluster_get_container_max_versions(struct namespace_info_s *nsinfo)
{
	/* For backward compatibility, versioning is disabled by default */
	return gridcluster_get_nsinfo_int64(nsinfo, "meta2_max_versions", 0);
}

gint64
gridcluster_get_keep_deleted_delay(struct namespace_info_s *nsinfo)
{
	return gridcluster_get_nsinfo_int64(nsinfo, "meta2_keep_deleted_delay", -1);
}

gchar *
gridcluster_get_service_update_policy (struct namespace_info_s *nsinfo)
{
	const gchar *def = "meta2=KEEP|1|1|;sqlx=KEEP|1|1|";

	if (!nsinfo || !nsinfo->options)
		return g_strdup(def);

	return gridcluster_get_nsinfo_strvalue (nsinfo, "service_update_policy", def);
}

gchar *
gridcluster_get_agent(void)
{
	gchar *cfg = oio_cfg_get_value(NULL, OIO_CFG_AGENT);
	return cfg ? cfg : g_strdup(GCLUSTER_AGENT_SOCK_PATH);
}

