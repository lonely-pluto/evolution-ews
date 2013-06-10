/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Authors :
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 * USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>
#include <libical/icalcomponent.h>
#include <libical/icalproperty.h>
#include <libical/ical.h>
#include <libedataserver/libedataserver.h>

#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <libxml/tree.h>

#include "e-ews-connection.h"
#include "e-ews-message.h"
#include "e-ews-item-change.h"

#define d(x) x

#define E_EWS_CONNECTION_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), E_TYPE_EWS_CONNECTION, EEwsConnectionPrivate))

/* For the number of connections */
#define EWS_CONNECTION_MAX_REQUESTS 1

#define QUEUE_LOCK(x) (g_rec_mutex_lock(&(x)->priv->queue_lock))
#define QUEUE_UNLOCK(x) (g_rec_mutex_unlock(&(x)->priv->queue_lock))

#define CHECK_ELEMENT(element_name, expected_name) \
	(check_element (G_STRFUNC, (element_name), (expected_name)))

struct _EwsNode;
static GMutex connecting;
static GHashTable *loaded_connections_permissions = NULL;
static gint comp_func (gconstpointer a, gconstpointer b);

static void ews_response_cb (SoupSession *session, SoupMessage *msg, gpointer data);

static void	ews_connection_authenticate	(SoupSession *sess,
						 SoupMessage *msg,
						 SoupAuth *auth,
						 gboolean retrying,
						 gpointer data);

/* Connection APIS */

struct _EEwsConnectionPrivate {
	SoupSession *soup_session;
	GThread *soup_thread;
	GMainLoop *soup_loop;
	GMainContext *soup_context;
	EProxy *proxy;

	CamelEwsSettings *settings;
	GMutex password_lock;

	/* Hash key for the loaded_connections_permissions table. */
	gchar *hash_key;

	gchar *uri;
	gchar *password;
	gchar *email;
	gchar *impersonate_user;

	GSList *jobs;
	GSList *active_job_queue;
	GRecMutex queue_lock;

	EEwsServerVersion version;
};

enum {
	PROP_0,
	PROP_PASSWORD,
	PROP_SETTINGS
};

typedef struct _EwsNode EwsNode;
typedef struct _EwsAsyncData EwsAsyncData;

struct _EwsAsyncData {
	GSList *items_created;
	GSList *items_updated;
	GSList *items_deleted;

	gint total_items;
	const gchar *directory;
	GSList *items;
	EwsPhotoAttachmentInfo *photo;
	gchar *sync_state;
	gboolean includes_last_item;
	EwsDelegateDeliver deliver_to;
	EEwsFolderType folder_type;
	EEwsConnection *cnc;
};

struct _EwsNode {
	ESoapMessage *msg;
	EEwsConnection *cnc;
	GSimpleAsyncResult *simple;

	gint pri;                /* the command priority */
	EEwsResponseCallback cb;

	GCancellable *cancellable;
	gulong cancel_handler_id;
};

/* Forward Declarations */
static void	e_ews_connection_authenticator_init
				(ESourceAuthenticatorInterface *interface);

G_DEFINE_TYPE_WITH_CODE (
	EEwsConnection,
	e_ews_connection,
	G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE (
		E_TYPE_SOURCE_AUTHENTICATOR,
		e_ews_connection_authenticator_init))

/* Static Functions */

GQuark
ews_connection_error_quark (void)
{
	static GQuark quark = 0;

	if (G_UNLIKELY (quark == 0)) {
		const gchar *string = "ews-connection-error-quark";
		quark = g_quark_from_static_string (string);
	}

	return quark;
}

static gboolean
ews_auth_mech_to_use_ntlm (GBinding *binding,
                           const GValue *source_value,
                           GValue *target_value,
                           gpointer user_data)
{
	const gchar *auth_mechanism;
	gboolean use_ntlm;

	/* Use NTLM unless the auth mechanism is "PLAIN". */
	auth_mechanism = g_value_get_string (source_value);
	use_ntlm = (g_strcmp0 (auth_mechanism, "PLAIN") != 0);
	g_value_set_boolean (target_value, use_ntlm);

	return TRUE;
}

static gpointer
ews_unref_in_thread_func (gpointer data)
{
	g_object_unref (G_OBJECT (data));

	return NULL;
}

static void
ews_unref_in_thread (gpointer object)
{
	GThread *thread;

	g_return_if_fail (G_IS_OBJECT (object));

	thread = g_thread_new (NULL, ews_unref_in_thread_func, object);
	g_thread_unref (thread);
}

static void
async_data_free (EwsAsyncData *async_data)
{
	g_free (async_data);
}

static gboolean
check_element (const gchar *function_name,
               const gchar *element_name,
               const gchar *expected_name)
{
	/* Do not call this directory, use CHECK_ELEMENT macro instead. */

	g_return_val_if_fail (function_name != NULL, FALSE);
	g_return_val_if_fail (element_name != NULL, FALSE);
	g_return_val_if_fail (expected_name != NULL, FALSE);

	if (!g_str_equal (element_name, expected_name)) {
		g_warning (
			"%s: Expected <%s> but got <%s>",
			function_name, expected_name, element_name);
		return FALSE;
	}

	return TRUE;
}

static EwsNode *
ews_node_new ()
{
	EwsNode *node;

	node = g_new0 (EwsNode, 1);
	return node;
}

static gboolean
autodiscover_parse_protocol (xmlNode *node,
                             EwsUrls *urls)
{
	for (node = node->children; node; node = node->next) {
		if (node->type == XML_ELEMENT_NODE &&
		    !strcmp ((gchar *) node->name, "ASUrl")) {
			urls->as_url = (gchar *) xmlNodeGetContent (node);
		} else if (node->type == XML_ELEMENT_NODE &&
		    !strcmp ((gchar *) node->name, "OABUrl"))
			urls->oab_url = (gchar *) xmlNodeGetContent (node);

		if (urls->as_url && urls->oab_url)
			return TRUE;
	}

	return FALSE;
}

static gint
comp_func (gconstpointer a,
           gconstpointer b)
{
	EwsNode *node1 = (EwsNode *) a;
	EwsNode *node2 = (EwsNode *) b;

	if (node1->pri > node2->pri)
		return -1;
	else if (node1->pri < node2->pri)
		return 1;
	else
		return 0;
}

typedef enum _EwsScheduleOp {
	EWS_SCHEDULE_OP_QUEUE_MESSAGE,
	EWS_SCHEDULE_OP_CANCEL,
	EWS_SCHEDULE_OP_ABORT
} EwsScheduleOp;

typedef struct _EwsScheduleData
{
	EEwsConnection *cnc;
	SoupMessage *message;

	EwsScheduleOp op;

	SoupSessionCallback queue_callback;
	gpointer queue_user_data;
} EwsScheduleData;

/* this is run in priv->soup_thread */
static gboolean
ews_connection_scheduled_cb (gpointer user_data)
{
	EwsScheduleData *sd = user_data;

	g_return_val_if_fail (sd != NULL, FALSE);

	switch (sd->op) {
	case EWS_SCHEDULE_OP_QUEUE_MESSAGE:
		soup_session_queue_message (
			sd->cnc->priv->soup_session, sd->message,
			sd->queue_callback, sd->queue_user_data);
		break;
	case EWS_SCHEDULE_OP_CANCEL:
		soup_session_cancel_message (sd->cnc->priv->soup_session, sd->message, SOUP_STATUS_CANCELLED);
		break;
	case EWS_SCHEDULE_OP_ABORT:
		soup_session_abort (sd->cnc->priv->soup_session);
		break;
	}

	if (sd->message)
		g_object_unref (sd->message);
	/* in case this is the last reference */
	ews_unref_in_thread (sd->cnc);
	g_free (sd);

	return FALSE;
}

static void
ews_connection_schedule_queue_message (EEwsConnection *cnc,
                                       SoupMessage *message,
                                       SoupSessionCallback callback,
                                       gpointer user_data)
{
	EwsScheduleData *sd;
	GSource *source;

	g_return_if_fail (E_IS_EWS_CONNECTION (cnc));
	g_return_if_fail (SOUP_IS_MESSAGE (message));

	sd = g_new0 (EwsScheduleData, 1);
	sd->cnc = g_object_ref (cnc);
	sd->message = g_object_ref (message);
	sd->op = EWS_SCHEDULE_OP_QUEUE_MESSAGE;
	sd->queue_callback = callback;
	sd->queue_user_data = user_data;

	source = g_idle_source_new ();
	g_source_set_priority (source, G_PRIORITY_DEFAULT);
	g_source_set_callback (source, ews_connection_scheduled_cb, sd, NULL);
	g_source_attach (source, cnc->priv->soup_context);
}

static void
ews_connection_schedule_cancel_message (EEwsConnection *cnc,
                                        SoupMessage *message)
{
	EwsScheduleData *sd;
	GSource *source;

	g_return_if_fail (E_IS_EWS_CONNECTION (cnc));
	g_return_if_fail (SOUP_IS_MESSAGE (message));

	sd = g_new0 (EwsScheduleData, 1);
	sd->cnc = g_object_ref (cnc);
	sd->message = g_object_ref (message);
	sd->op = EWS_SCHEDULE_OP_CANCEL;

	source = g_idle_source_new ();
	g_source_set_priority (source, G_PRIORITY_DEFAULT);
	g_source_set_callback (source, ews_connection_scheduled_cb, sd, NULL);
	g_source_attach (source, cnc->priv->soup_context);
}

static void
ews_connection_schedule_abort (EEwsConnection *cnc)
{
	EwsScheduleData *sd;
	GSource *source;

	g_return_if_fail (E_IS_EWS_CONNECTION (cnc));

	sd = g_new0 (EwsScheduleData, 1);
	sd->cnc = g_object_ref (cnc);
	sd->op = EWS_SCHEDULE_OP_ABORT;

	source = g_idle_source_new ();
	g_source_set_priority (source, G_PRIORITY_DEFAULT);
	g_source_set_callback (source, ews_connection_scheduled_cb, sd, NULL);
	g_source_attach (source, cnc->priv->soup_context);
}

static void ews_cancel_request (GCancellable *cancellable, gpointer user_data);

static void
ews_discover_server_version (EEwsConnection *cnc,
			     ESoapResponse *response)
{
	ESoapParameter *param;
	gchar *version;

	g_return_if_fail (cnc != NULL);

	if (cnc->priv->version != E_EWS_EXCHANGE_UNKNOWN)
		return;

	param = e_soap_response_get_first_parameter_by_name (
		response, "ServerVersionInfo", NULL);
	if (!param)
		return;

	version = e_soap_parameter_get_property (param, "Version");

	if (g_strcmp0 (version, "Exchange2007") == 0)
		cnc->priv->version = E_EWS_EXCHANGE_2007;
	else if (g_strcmp0 (version, "Exchange2007_SP1") == 0)
		cnc->priv->version = E_EWS_EXCHANGE_2007_SP1;
	else if (g_strcmp0 (version, "Exchange2010") == 0)
		cnc->priv->version = E_EWS_EXCHANGE_2010;
	else if (g_strcmp0 (version, "Exchange2010_SP1") == 0)
		cnc->priv->version = E_EWS_EXCHANGE_2010_SP1;
	else if (g_strcmp0 (version, "Exchange2010_SP2") == 0)
		cnc->priv->version = E_EWS_EXCHANGE_2010_SP2;
	else
		cnc->priv->version = E_EWS_EXCHANGE_FUTURE;

	g_free (version);
}

/* this is run in priv->soup_thread */
static gboolean
ews_next_request (gpointer _cnc)
{
	EEwsConnection *cnc = _cnc;
	GSList *l;
	EwsNode *node;

	QUEUE_LOCK (cnc);

	l = cnc->priv->jobs;

	if (!l || g_slist_length (cnc->priv->active_job_queue) >= EWS_CONNECTION_MAX_REQUESTS) {
		QUEUE_UNLOCK (cnc);
		return FALSE;
	}

	node = (EwsNode *) l->data;

	if (g_getenv ("EWS_DEBUG") && (atoi (g_getenv ("EWS_DEBUG")) == 1)) {
		soup_buffer_free (soup_message_body_flatten (SOUP_MESSAGE (node->msg)->request_body));
		/* print request's body */
		printf ("\n The request headers");
		fputc ('\n', stdout);
		fputs (SOUP_MESSAGE (node->msg)->request_body->data, stdout);
		fputc ('\n', stdout);
	}

	/* Remove the node from the priority queue */
	cnc->priv->jobs = g_slist_remove (cnc->priv->jobs, (gconstpointer *) node);

	/* Add to active job queue */
	cnc->priv->active_job_queue = g_slist_append (cnc->priv->active_job_queue, node);

	if (cnc->priv->soup_session) {
		soup_session_queue_message (cnc->priv->soup_session, SOUP_MESSAGE (node->msg), ews_response_cb, node);
		QUEUE_UNLOCK (cnc);
	} else {
		QUEUE_UNLOCK (cnc);

		ews_cancel_request (NULL, node);
	}

	return FALSE;
}

static void
ews_trigger_next_request (EEwsConnection *cnc)
{
	GSource *source;

	g_return_if_fail (cnc != NULL);

	if (cnc->priv->soup_session) {
		source = g_idle_source_new ();
		g_source_set_priority (source, G_PRIORITY_DEFAULT);
		g_source_set_callback (source, ews_next_request, cnc, NULL);
		g_source_attach (source, cnc->priv->soup_context);
	} else {
		ews_next_request (cnc);
	}
}

/**
 * ews_active_job_done
 * @cnc:
 * @msg:
 * Removes the node from active Queue and free's the node
 *
 * Returns:
 **/
static void
ews_active_job_done (EEwsConnection *cnc,
                     EwsNode *ews_node)
{
	g_return_if_fail (cnc != NULL);
	g_return_if_fail (ews_node != NULL);

	QUEUE_LOCK (cnc);

	cnc->priv->active_job_queue = g_slist_remove (cnc->priv->active_job_queue, ews_node);
	if (ews_node->cancellable && ews_node->cancel_handler_id)
		g_signal_handler_disconnect (ews_node->cancellable, ews_node->cancel_handler_id);

	QUEUE_UNLOCK (cnc);

	ews_trigger_next_request (cnc);

	if (ews_node->cancellable)
		g_object_unref (ews_node->cancellable);

	/* the 'simple' holds reference on 'cnc' and this function
	 * is called in a dedicated thread, which 'cnc' joins on dispose,
	 * thus to avoid race condition, unref the object in its own thread */
	ews_unref_in_thread (ews_node->simple);
	g_free (ews_node);
}

static void
ews_cancel_request (GCancellable *cancellable,
                    gpointer user_data)
{
	EwsNode *node = user_data;
	EEwsConnection *cnc = node->cnc;
	GSimpleAsyncResult *simple = node->simple;
	ESoapMessage *msg = node->msg;
	GSList *found;

	QUEUE_LOCK (cnc);
	found = g_slist_find (cnc->priv->active_job_queue, node);
	QUEUE_UNLOCK (cnc);

	g_simple_async_result_set_error (
		simple,
		EWS_CONNECTION_ERROR,
		EWS_CONNECTION_ERROR_CANCELLED,
		_("Operation Cancelled"));
	if (found) {
		ews_connection_schedule_cancel_message (cnc, SOUP_MESSAGE (msg));
	} else {
		QUEUE_LOCK (cnc);
		cnc->priv->jobs = g_slist_remove (cnc->priv->jobs, (gconstpointer) node);
		QUEUE_UNLOCK (cnc);

		ews_response_cb (cnc->priv->soup_session, SOUP_MESSAGE (msg), node);
	}
}

void
e_ews_connection_queue_request (EEwsConnection *cnc,
                                ESoapMessage *msg,
                                EEwsResponseCallback cb,
                                gint pri,
                                GCancellable *cancellable,
                                GSimpleAsyncResult *simple)
{
	EwsNode *node;

	g_return_if_fail (cnc != NULL);
	g_return_if_fail (cb != NULL);
	g_return_if_fail (G_IS_SIMPLE_ASYNC_RESULT (simple));

	node = ews_node_new ();
	node->msg = msg;
	node->pri = pri;
	node->cb = cb;
	node->cnc = cnc;
	node->simple = g_object_ref (simple);

	QUEUE_LOCK (cnc);
	cnc->priv->jobs = g_slist_insert_sorted (cnc->priv->jobs, (gpointer *) node, (GCompareFunc) comp_func);
	QUEUE_UNLOCK (cnc);

	if (cancellable) {
		node->cancellable = g_object_ref (cancellable);
		if (g_cancellable_is_cancelled (cancellable))
			ews_cancel_request (cancellable, node);
		else
			node->cancel_handler_id = g_cancellable_connect (
				cancellable,
				G_CALLBACK (ews_cancel_request),
				(gpointer) node, NULL);
	}

	ews_trigger_next_request (cnc);
}

/* Response callbacks */

static void
ews_response_cb (SoupSession *session,
                 SoupMessage *msg,
                 gpointer data)
{
	EwsNode *enode = (EwsNode *) data;
	ESoapResponse *response;

	if (g_cancellable_is_cancelled (enode->cancellable))
		goto exit;

	if (msg->status_code == SOUP_STATUS_UNAUTHORIZED) {
		g_simple_async_result_set_error (
			enode->simple,
			EWS_CONNECTION_ERROR,
			EWS_CONNECTION_ERROR_AUTHENTICATION_FAILED,
			_("Authentication failed"));
		goto exit;
	}

	response = e_soap_message_parse_response ((ESoapMessage *) msg);

	if (response == NULL) {
		g_simple_async_result_set_error (
			enode->simple,
			EWS_CONNECTION_ERROR,
			EWS_CONNECTION_ERROR_NORESPONSE,
			_("No response: %s"), msg->reason_phrase);
		goto exit;
	}

	/* TODO: The stdout can be replaced with Evolution's
	 * Logging framework also */
	if (g_getenv ("EWS_DEBUG") && (atoi (g_getenv ("EWS_DEBUG")) >= 1))
		e_soap_response_dump_response (response, stdout);

	if (enode->cb != NULL)
		enode->cb (response, enode->simple);

	g_object_unref (response);

exit:
	g_simple_async_result_complete_in_idle (enode->simple);

	ews_active_job_done (enode->cnc, enode);
}

typedef gpointer (*ItemParser) (ESoapParameter *param);

static void
sync_xxx_response_cb (ESoapParameter *subparam,
                      EwsAsyncData *async_data,
                      ItemParser parser,
                      const gchar *last_tag,
                      const gchar *delete_id_tag)
{
	ESoapParameter *node;
	gchar *new_sync_state = NULL, *value, *last;
	GSList *items_created = NULL, *items_updated = NULL, *items_deleted = NULL;
	gboolean includes_last_item = FALSE;

	node = e_soap_parameter_get_first_child_by_name (subparam, "SyncState");
	new_sync_state = e_soap_parameter_get_string_value (node);

	node = e_soap_parameter_get_first_child_by_name (subparam, last_tag);
	last = e_soap_parameter_get_string_value (node);
	if (!strcmp (last, "true"))
		includes_last_item = TRUE;
	g_free (last);

	node = e_soap_parameter_get_first_child_by_name (subparam, "Changes");

	if (node) {
		ESoapParameter *subparam1;
		for (subparam1 = e_soap_parameter_get_first_child_by_name (node, "Create");
		     subparam1 != NULL;
		     subparam1 = e_soap_parameter_get_next_child_by_name (subparam1, "Create")) {
			EEwsFolder *folder;

			folder = parser (subparam1);
			items_created = g_slist_append (items_created, folder);
		}

		for (subparam1 = e_soap_parameter_get_first_child_by_name (node, "Update");
		     subparam1 != NULL;
		     subparam1 = e_soap_parameter_get_next_child_by_name (subparam1, "Update")) {
			EEwsFolder *folder;

			folder = parser (subparam1);
			items_updated = g_slist_append (items_updated, folder);
		}
		  /* Exchange 2007SP1 introduced <ReadFlagChange> which is basically identical
		   * to <Update>; no idea why they thought it was a good idea. */
		for (subparam1 = e_soap_parameter_get_first_child_by_name (node, "ReadFlagChange");
		     subparam1 != NULL;
		     subparam1 = e_soap_parameter_get_next_child_by_name (subparam1, "ReadFlagChange")) {
			EEwsFolder *folder;

			folder = parser (subparam1);
			items_updated = g_slist_append (items_updated, folder);
		}

		for (subparam1 = e_soap_parameter_get_first_child_by_name (node, "Delete");
		     subparam1 != NULL;
		     subparam1 = e_soap_parameter_get_next_child_by_name (subparam1, "Delete")) {
			ESoapParameter *folder_param;

			folder_param = e_soap_parameter_get_first_child_by_name (subparam1, delete_id_tag);
			value = e_soap_parameter_get_property (folder_param, "Id");
			items_deleted = g_slist_append (items_deleted, value);
		}
	}

	async_data->items_created = items_created;
	async_data->items_updated = items_updated;
	async_data->items_deleted = items_deleted;
	async_data->sync_state = new_sync_state;
	async_data->includes_last_item = includes_last_item;
}

static void
sync_hierarchy_response_cb (ESoapResponse *response,
                            GSimpleAsyncResult *simple)
{
	EwsAsyncData *async_data;
	ESoapParameter *param;
	ESoapParameter *subparam;
	GError *error = NULL;

	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	param = e_soap_response_get_first_parameter_by_name (
		response, "ResponseMessages", &error);

	/* Sanity check */
	g_return_if_fail (
		(param != NULL && error == NULL) ||
		(param == NULL && error != NULL));

	if (error != NULL) {
		g_simple_async_result_take_error (simple, error);
		return;
	}

	subparam = e_soap_parameter_get_first_child (param);

	while (subparam != NULL) {
		const gchar *name = (const gchar *) subparam->name;

		if (!ews_get_response_status (subparam, &error)) {
			g_simple_async_result_take_error (simple, error);
			return;
		}

		if (CHECK_ELEMENT (name, "SyncFolderHierarchyResponseMessage"))
			sync_xxx_response_cb (
				subparam, async_data,
				(ItemParser) e_ews_folder_new_from_soap_parameter,
				"IncludesLastFolderInRange", "FolderId");

		subparam = e_soap_parameter_get_next_child (subparam);
	}
}

static void
sync_folder_items_response_cb (ESoapResponse *response,
                               GSimpleAsyncResult *simple)
{
	EwsAsyncData *async_data;
	ESoapParameter *param;
	ESoapParameter *subparam;
	GError *error = NULL;

	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	param = e_soap_response_get_first_parameter_by_name (
		response, "ResponseMessages", &error);

	/* Sanity check */
	g_return_if_fail (
		(param != NULL && error == NULL) ||
		(param == NULL && error != NULL));

	if (error != NULL) {
		g_simple_async_result_take_error (simple, error);
		return;
	}

	subparam = e_soap_parameter_get_first_child (param);

	while (subparam != NULL) {
		const gchar *name = (const gchar *) subparam->name;

		if (!ews_get_response_status (subparam, &error)) {
			g_simple_async_result_take_error (simple, error);
			return;
		}

		if (CHECK_ELEMENT (name, "SyncFolderItemsResponseMessage"))
			sync_xxx_response_cb (
				subparam, async_data,
				(ItemParser) e_ews_item_new_from_soap_parameter,
				"IncludesLastItemInRange", "ItemId");

		subparam = e_soap_parameter_get_next_child (subparam);
	}
}

static void
ews_handle_folders_param (ESoapParameter *subparam,
                          EwsAsyncData *async_data)
{
	ESoapParameter *node;
	EEwsFolder *folder;

	for (node = e_soap_parameter_get_first_child_by_name (subparam, "Folders");
	     node; node = e_soap_parameter_get_next_child_by_name (subparam, "Folders")) {
		folder = e_ews_folder_new_from_soap_parameter (node);
		if (!folder) continue;
		async_data->items = g_slist_append (async_data->items, folder);
	}
}

static void
get_folder_response_cb (ESoapResponse *response,
                        GSimpleAsyncResult *simple)
{
	EwsAsyncData *async_data;
	ESoapParameter *param;
	ESoapParameter *subparam;
	GError *error = NULL;

	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	/*
	 * During the first connection, we are able to get the current version of the Exchange server.
	 * We are ensuring it happens during the ews_connection_try_password_sync(), that calls
	 * ews_connection_get_folder_sync() and then we are able to get the current version of the
	 * server from this first response.
	 */
	ews_discover_server_version (async_data->cnc, response);

	param = e_soap_response_get_first_parameter_by_name (
		response, "ResponseMessages", &error);

	/* Sanity check */
	g_return_if_fail (
		(param != NULL && error == NULL) ||
		(param == NULL && error != NULL));

	if (error != NULL) {
		g_simple_async_result_take_error (simple, error);
		return;
	}

	subparam = e_soap_parameter_get_first_child (param);

	while (subparam != NULL) {
		const gchar *name = (const gchar *) subparam->name;

		if (!ews_get_response_status (subparam, &error)) {
			if (g_strcmp0 (name, "GetFolderResponseMessage") == 0) {
				async_data->items = g_slist_append (async_data->items, e_ews_folder_new_from_error (error));
				g_clear_error (&error);
			} else {
				g_simple_async_result_take_error (simple, error);
				return;
			}
		} else if (CHECK_ELEMENT (name, "GetFolderResponseMessage"))
			ews_handle_folders_param (subparam, async_data);

		subparam = e_soap_parameter_get_next_child (subparam);
	}
}

static void
ews_handle_root_folder_param_items (ESoapParameter *subparam,
				    EwsAsyncData *async_data)
{
	ESoapParameter *node, *subparam1;
	gchar *last, *total;
	gint total_items;
	EEwsItem *item;
	gboolean includes_last_item = FALSE;

	node = e_soap_parameter_get_first_child_by_name (subparam, "RootFolder");
	total = e_soap_parameter_get_property (node, "TotalItemsInView");
	total_items = atoi (total);
	g_free (total);
	last = e_soap_parameter_get_property (node, "IncludesLastItemInRange");
	if (!strcmp (last, "true"))
		includes_last_item = TRUE;
	g_free (last);

	node = e_soap_parameter_get_first_child_by_name (node, "Items");
	for (subparam1 = e_soap_parameter_get_first_child (node);
	     subparam1; subparam1 = e_soap_parameter_get_next_child (subparam1)) {
		item = e_ews_item_new_from_soap_parameter (subparam1);
		if (!item) continue;
		async_data->items = g_slist_append (async_data->items, item);
	}
	async_data->total_items = total_items;
	async_data->includes_last_item = includes_last_item;
}

static void
find_folder_items_response_cb (ESoapResponse *response,
                               GSimpleAsyncResult *simple)
{
	EwsAsyncData *async_data;
	ESoapParameter *param;
	ESoapParameter *subparam;
	GError *error = NULL;

	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	param = e_soap_response_get_first_parameter_by_name (
		response, "ResponseMessages", &error);

	/* Sanity check */
	g_return_if_fail (
		(param != NULL && error == NULL) ||
		(param == NULL && error != NULL));

	if (error != NULL) {
		g_simple_async_result_take_error (simple, error);
		return;
	}

	subparam = e_soap_parameter_get_first_child (param);

	while (subparam != NULL) {
		const gchar *name = (const gchar *) subparam->name;

		if (!ews_get_response_status (subparam, &error)) {
			g_simple_async_result_take_error (simple, error);
			return;
		}

		if (CHECK_ELEMENT (name, "FindItemResponseMessage"))
			ews_handle_root_folder_param_items (subparam, async_data);

		subparam = e_soap_parameter_get_next_child (subparam);
	}
}

/* Used for CreateItems and GetItems */
static void
ews_handle_items_param (ESoapParameter *subparam,
                        EwsAsyncData *async_data,
                        const GError *error)
{
	ESoapParameter *node;
	EEwsItem *item;

	for (node = e_soap_parameter_get_first_child_by_name (subparam, "Items");
	     node; node = e_soap_parameter_get_next_child_by_name (subparam, "Items")) {
		if (node->children)
			item = e_ews_item_new_from_soap_parameter (node);
		else
			item = NULL;
		if (!item && error != NULL)
			item = e_ews_item_new_from_error (error);
		if (!item) continue;
		async_data->items = g_slist_append (async_data->items, item);
	}
}

static void
handle_get_items_response_cb (EwsAsyncData *async_data, ESoapParameter *param)
{
	ESoapParameter *subparam;
	GError *error = NULL;

	subparam = e_soap_parameter_get_first_child (param);

	while (subparam != NULL) {
		const gchar *name = (const gchar *) subparam->name;

		if (g_str_has_suffix (name, "ResponseMessage")) {
			ews_get_response_status (subparam, &error);
			ews_handle_items_param (subparam, async_data, error);
		} else {
			g_warning (
				"%s: Unexpected element <%s>",
				G_STRFUNC, name);
		}

		/* Do not stop on errors. */
		if (error != NULL)
			g_clear_error (&error);

		subparam = e_soap_parameter_get_next_child (subparam);
	}
}

static void
get_items_response_cb (ESoapResponse *response,
                       GSimpleAsyncResult *simple)
{
	EwsAsyncData *async_data;
	ESoapParameter *param;
	GError *error = NULL;

	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	param = e_soap_response_get_first_parameter_by_name (
		response, "ResponseMessages", &error);

	/* Sanity check */
	g_return_if_fail (
		(param != NULL && error == NULL) ||
		(param == NULL && error != NULL));

	if (error != NULL) {
		g_simple_async_result_take_error (simple, error);
		return;
	}

	handle_get_items_response_cb (async_data, param);
}

static void
ews_handle_resolution_set_param (ESoapParameter *subparam,
                                 EwsAsyncData *async_data)
{
	ESoapParameter *node;
	gchar *prop;
	gboolean includes_last_item;
	GSList *mailboxes = NULL, *contact_items = NULL;

	subparam = e_soap_parameter_get_first_child_by_name (subparam, "ResolutionSet");
	prop = e_soap_parameter_get_property (subparam, "IncludesLastItemInRange");

	if (prop && !strcmp (prop, "true"))
		includes_last_item = TRUE;
	g_free (prop);

	for (subparam = e_soap_parameter_get_first_child_by_name (subparam, "Resolution");
		subparam != NULL;
		subparam = e_soap_parameter_get_next_child_by_name (subparam, "Resolution")) {
		EwsMailbox *mb;

		node = e_soap_parameter_get_first_child_by_name (subparam, "Mailbox");
		mb = e_ews_item_mailbox_from_soap_param (node);
		if (mb) {
			EwsResolveContact *rc;

			mailboxes = g_slist_prepend (mailboxes, mb);

			/* 'mailboxes' and 'contact_items' match 1:1, but if the contact information
			 * wasn't found, then NULL is stored in the corresponding position */
			node = e_soap_parameter_get_first_child_by_name (subparam, "Contact");
			rc = e_ews_item_resolve_contact_from_soap_param (node);
			contact_items = g_slist_prepend (contact_items, rc);
		}
	}

	/* Reuse existing variables */
	async_data->items = g_slist_reverse (mailboxes);
	async_data->includes_last_item = includes_last_item;
	async_data->items_created = g_slist_reverse (contact_items);
}

static void
resolve_names_response_cb (ESoapResponse *response,
                           GSimpleAsyncResult *simple)
{
	EwsAsyncData *async_data;
	ESoapParameter *param;
	ESoapParameter *subparam;
	GError *error = NULL;

	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	param = e_soap_response_get_first_parameter_by_name (
		response, "ResponseMessages", &error);

	/* Sanity check */
	g_return_if_fail (
		(param != NULL && error == NULL) ||
		(param == NULL && error != NULL));

	if (error != NULL) {
		g_simple_async_result_take_error (simple, error);
		return;
	}

	subparam = e_soap_parameter_get_first_child (param);

	while (subparam != NULL) {
		const gchar *name = (const gchar *) subparam->name;

		if (!ews_get_response_status (subparam, &error)) {
			g_simple_async_result_take_error (simple, error);
			return;
		}

		if (CHECK_ELEMENT (name, "ResolveNamesResponseMessage"))
			ews_handle_resolution_set_param (subparam, async_data);

		subparam = e_soap_parameter_get_next_child (subparam);
	}
}

static void
ews_handle_dl_expansion_param (ESoapParameter *subparam,
                               EwsAsyncData *async_data)
{
	gboolean includes_last_item;
	GSList *mailboxes = NULL;
	gchar *prop;

	subparam = e_soap_parameter_get_first_child_by_name (subparam, "DLExpansion");
	prop = e_soap_parameter_get_property (subparam, "IncludesLastItemInRange");

	if (prop && !strcmp (prop, "true"))
		includes_last_item = TRUE;
	g_free (prop);

	for (subparam = e_soap_parameter_get_first_child_by_name (subparam, "Mailbox");
		subparam != NULL;
		subparam = e_soap_parameter_get_next_child_by_name (subparam, "Mailbox")) {
		EwsMailbox *mb;

		mb = e_ews_item_mailbox_from_soap_param (subparam);
		if (mb)
			mailboxes = g_slist_append (mailboxes, mb);
	}

	/* Reuse existing variables */
	async_data->items = mailboxes;
	async_data->includes_last_item = includes_last_item;
}

static void
expand_dl_response_cb (ESoapResponse *response,
                       GSimpleAsyncResult *simple)
{
	EwsAsyncData *async_data;
	ESoapParameter *param;
	ESoapParameter *subparam;
	GError *error = NULL;

	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	param = e_soap_response_get_first_parameter_by_name (
		response, "ResponseMessages", &error);

	/* Sanity check */
	g_return_if_fail (
		(param != NULL && error == NULL) ||
		(param == NULL && error != NULL));

	if (error != NULL) {
		g_simple_async_result_take_error (simple, error);
		return;
	}

	subparam = e_soap_parameter_get_first_child (param);

	while (subparam != NULL) {
		const gchar *name = (const gchar *) subparam->name;

		if (!ews_get_response_status (subparam, &error)) {
			g_simple_async_result_take_error (simple, error);
			return;
		}

		if (CHECK_ELEMENT (name, "ExpandDLResponseMessage"))
			ews_handle_dl_expansion_param (subparam, async_data);

		subparam = e_soap_parameter_get_next_child (subparam);
	}
}

/* TODO scan all folders if we support creating multiple folders in the request */
static void
ews_handle_create_folders_param (ESoapParameter *soapparam,
                                 EwsAsyncData *async_data)
{
	ESoapParameter *param, *node;
	EwsFolderId *fid = NULL;
	GSList *fids = NULL;
	const gchar *folder_element;

	switch (async_data->folder_type) {
		case E_EWS_FOLDER_TYPE_MAILBOX:
			folder_element = "Folder";
			break;
		case E_EWS_FOLDER_TYPE_CALENDAR:
			folder_element = "CalendarFolder";
			break;
		case E_EWS_FOLDER_TYPE_CONTACTS:
			folder_element = "ContactsFolder";
			break;
		case E_EWS_FOLDER_TYPE_SEARCH:
			folder_element = "SearchFolder";
			break;
		case E_EWS_FOLDER_TYPE_TASKS:
			folder_element = "TasksFolder";
			break;
		default:
			g_warn_if_reached ();
			folder_element = "Folder";
			break;
	}

	node = e_soap_parameter_get_first_child_by_name (soapparam, "Folders");
	node = e_soap_parameter_get_first_child_by_name (node, folder_element);
	param = e_soap_parameter_get_first_child_by_name (node, "FolderId");

	fid = g_new0 (EwsFolderId, 1);
	fid->id = e_soap_parameter_get_property (param, "Id");
	fid->change_key = e_soap_parameter_get_property (param, "ChangeKey");
	fids = g_slist_append (fids, fid);

	async_data->items_created = fids;
}

static void
create_folder_response_cb (ESoapResponse *response,
                           GSimpleAsyncResult *simple)
{
	EwsAsyncData *async_data;
	ESoapParameter *param;
	ESoapParameter *subparam;
	GError *error = NULL;

	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	param = e_soap_response_get_first_parameter_by_name (
		response, "ResponseMessages", &error);

	/* Sanity check */
	g_return_if_fail (
		(param != NULL && error == NULL) ||
		(param == NULL && error != NULL));

	if (error != NULL) {
		g_simple_async_result_take_error (simple, error);
		return;
	}

	subparam = e_soap_parameter_get_first_child (param);

	while (subparam != NULL) {
		const gchar *name = (const gchar *) subparam->name;

		if (!ews_get_response_status (subparam, &error)) {
			g_simple_async_result_take_error (simple, error);
			return;
		}

		if (CHECK_ELEMENT (name, "CreateFolderResponseMessage"))
			ews_handle_create_folders_param (subparam, async_data);

		subparam = e_soap_parameter_get_next_child (subparam);
	}
}

static void
proxy_settings_changed (EProxy *proxy,
                        gpointer user_data)
{
	SoupURI *proxy_uri = NULL;
	EEwsConnection *cnc = (EEwsConnection *) user_data;

	if (!cnc || !cnc->priv || !cnc->priv->uri || !cnc->priv->soup_session)
		return;

	/* use proxy if necessary */
	if (e_proxy_require_proxy_for_uri (proxy, cnc->priv->uri)) {
		proxy_uri = e_proxy_peek_uri_for (proxy, cnc->priv->uri);
	}

	g_object_set (cnc->priv->soup_session, SOUP_SESSION_PROXY_URI, proxy_uri, NULL);
}

static void
ews_connection_set_settings (EEwsConnection *connection,
                             CamelEwsSettings *settings)
{
	g_return_if_fail (CAMEL_IS_EWS_SETTINGS (settings));
	g_return_if_fail (connection->priv->settings == NULL);

	connection->priv->settings = g_object_ref (settings);
}

static void
ews_connection_set_property (GObject *object,
                             guint property_id,
                             const GValue *value,
                             GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_PASSWORD:
			e_ews_connection_set_password (
				E_EWS_CONNECTION (object),
				g_value_get_string (value));
			return;

		case PROP_SETTINGS:
			ews_connection_set_settings (
				E_EWS_CONNECTION (object),
				g_value_get_object (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
ews_connection_get_property (GObject *object,
                             guint property_id,
                             GValue *value,
                             GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_PASSWORD:
			g_value_take_string (
				value,
				e_ews_connection_dup_password (
				E_EWS_CONNECTION (object)));
			return;

		case PROP_SETTINGS:
			g_value_take_object (
				value,
				e_ews_connection_ref_settings (
				E_EWS_CONNECTION (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
ews_connection_dispose (GObject *object)
{
	EEwsConnectionPrivate *priv;

	priv = E_EWS_CONNECTION_GET_PRIVATE (object);

	g_mutex_lock (&connecting);

	/* remove the connection from the hash table */
	if (loaded_connections_permissions != NULL) {
		g_hash_table_remove (
			loaded_connections_permissions, priv->hash_key);
		if (g_hash_table_size (loaded_connections_permissions) == 0) {
			g_hash_table_destroy (loaded_connections_permissions);
			loaded_connections_permissions = NULL;
		}
	}

	g_mutex_unlock (&connecting);

	if (priv->soup_session) {
		g_signal_handlers_disconnect_by_func (
			priv->soup_session,
			ews_connection_authenticate, object);

		g_main_loop_quit (priv->soup_loop);
		g_thread_join (priv->soup_thread);
		priv->soup_thread = NULL;

		g_main_loop_unref (priv->soup_loop);
		priv->soup_loop = NULL;
		g_main_context_unref (priv->soup_context);
		priv->soup_context = NULL;
	}

	if (priv->proxy) {
		g_object_unref (priv->proxy);
		priv->proxy = NULL;
	}

	if (priv->settings != NULL) {
		g_object_unref (priv->settings);
		priv->settings = NULL;
	}

	e_ews_connection_set_password (E_EWS_CONNECTION (object), NULL);

	g_slist_free (priv->jobs);
	priv->jobs = NULL;

	g_slist_free (priv->active_job_queue);
	priv->active_job_queue = NULL;

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (e_ews_connection_parent_class)->dispose (object);
}

static void
ews_connection_finalize (GObject *object)
{
	EEwsConnectionPrivate *priv;

	priv = E_EWS_CONNECTION_GET_PRIVATE (object);

	g_free (priv->uri);
	g_free (priv->password);
	g_free (priv->email);
	g_free (priv->hash_key);
	g_free (priv->impersonate_user);

	g_mutex_clear (&priv->password_lock);
	g_rec_mutex_clear (&priv->queue_lock);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (e_ews_connection_parent_class)->finalize (object);
}

static ESourceAuthenticationResult
ews_connection_try_password_sync (ESourceAuthenticator *authenticator,
                                  const GString *password,
                                  GCancellable *cancellable,
                                  GError **error)
{
	EEwsConnection *connection;
	ESourceAuthenticationResult result;
	EwsFolderId *fid = NULL;
	GSList *folders = NULL;
	GSList *ids = NULL;
	GError *local_error = NULL;

	connection = E_EWS_CONNECTION (authenticator);

	e_ews_connection_set_password (connection, password->str);

	fid = g_new0 (EwsFolderId, 1);
	fid->id = g_strdup ("inbox");
	fid->is_distinguished_id = TRUE;
	ids = g_slist_append (ids, fid);

	/* FIXME Should be able to pass NULL for folders since we're
	 *       not interested.  Currently the code assumes non-NULL. */
	e_ews_connection_get_folder_sync (
		connection, EWS_PRIORITY_MEDIUM, "Default",
		NULL, ids, &folders, cancellable, &local_error);

	g_slist_free_full (folders, g_object_unref);
	g_slist_free_full (ids, (GDestroyNotify) e_ews_folder_id_free);

	if (local_error == NULL) {
		result = E_SOURCE_AUTHENTICATION_ACCEPTED;

	} else {
		gboolean auth_failed;

		auth_failed = g_error_matches (
			local_error, EWS_CONNECTION_ERROR,
			EWS_CONNECTION_ERROR_AUTHENTICATION_FAILED);

		if (auth_failed) {
			g_clear_error (&local_error);
			result = E_SOURCE_AUTHENTICATION_REJECTED;
		} else {
			g_propagate_error (error, local_error);
			result = E_SOURCE_AUTHENTICATION_ERROR;
		}

		e_ews_connection_set_password (connection, NULL);
	}

	return result;
}

static void
e_ews_connection_class_init (EEwsConnectionClass *class)
{
	GObjectClass *object_class;

	g_type_class_add_private (class, sizeof (EEwsConnectionPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->set_property = ews_connection_set_property;
	object_class->get_property = ews_connection_get_property;
	object_class->dispose = ews_connection_dispose;
	object_class->finalize = ews_connection_finalize;

	g_object_class_install_property (
		object_class,
		PROP_PASSWORD,
		g_param_spec_string (
			"password",
			"Password",
			"Authentication password",
			NULL,
			G_PARAM_READWRITE |
			G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (
		object_class,
		PROP_SETTINGS,
		g_param_spec_object (
			"settings",
			"Settings",
			"Connection settings",
			CAMEL_TYPE_EWS_SETTINGS,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT_ONLY |
			G_PARAM_STATIC_STRINGS));
}

static void
e_ews_connection_authenticator_init (ESourceAuthenticatorInterface *interface)
{
	interface->try_password_sync = ews_connection_try_password_sync;
}

static gpointer
e_ews_soup_thread (gpointer user_data)
{
	EEwsConnection *cnc = user_data;

	g_main_context_push_thread_default (cnc->priv->soup_context);
	g_main_loop_run (cnc->priv->soup_loop);
	g_main_context_pop_thread_default (cnc->priv->soup_context);

	/* abort any pending operations */
	soup_session_abort (cnc->priv->soup_session);

	g_object_unref (cnc->priv->soup_session);
	cnc->priv->soup_session = NULL;

	return NULL;
}

static void
e_ews_connection_init (EEwsConnection *cnc)
{
	cnc->priv = E_EWS_CONNECTION_GET_PRIVATE (cnc);

	cnc->priv->soup_context = g_main_context_new ();
	cnc->priv->soup_loop = g_main_loop_new (cnc->priv->soup_context, FALSE);

	cnc->priv->soup_thread = g_thread_new (NULL, e_ews_soup_thread, cnc);

	/* create the SoupSession for this connection */
	cnc->priv->soup_session = soup_session_async_new_with_options (
		SOUP_SESSION_USE_NTLM, TRUE,
		SOUP_SESSION_ASYNC_CONTEXT,
		cnc->priv->soup_context,
		NULL);

	cnc->priv->version = E_EWS_EXCHANGE_UNKNOWN;

	cnc->priv->proxy = e_proxy_new ();
	e_proxy_setup_proxy (cnc->priv->proxy);
	g_signal_connect (cnc->priv->proxy, "changed", G_CALLBACK (proxy_settings_changed), cnc);

	if (g_getenv ("EWS_DEBUG") && (atoi (g_getenv ("EWS_DEBUG")) >= 2)) {
		SoupLogger *logger;
		logger = soup_logger_new (SOUP_LOGGER_LOG_BODY, -1);
		soup_session_add_feature (
			cnc->priv->soup_session,
			SOUP_SESSION_FEATURE (logger));
	}

	g_mutex_init (&cnc->priv->password_lock);
	g_rec_mutex_init (&cnc->priv->queue_lock);

	g_signal_connect (
		cnc->priv->soup_session, "authenticate",
		G_CALLBACK (ews_connection_authenticate), cnc);
}

static void
ews_connection_authenticate (SoupSession *sess,
                             SoupMessage *msg,
                             SoupAuth *auth,
                             gboolean retrying,
                             gpointer data)
{
	EEwsConnection *cnc = data;
	CamelNetworkSettings *network_settings;
	gchar *user, *password;

	g_return_if_fail (cnc != NULL);

	if (retrying)
		e_ews_connection_set_password (cnc, NULL);

	network_settings = CAMEL_NETWORK_SETTINGS (cnc->priv->settings);
	user = camel_network_settings_dup_user (network_settings);

	password = e_ews_connection_dup_password (cnc);

	if (password != NULL)
		soup_auth_authenticate (auth, user, password);

	g_free (password);
	g_free (user);
}

void
ews_oal_free (EwsOAL *oal)
{
	if (oal != NULL) {
		g_free (oal->id);
		g_free (oal->dn);
		g_free (oal->name);
		g_free (oal);
	}
}

void
ews_oal_details_free (EwsOALDetails *details)
{
	if (details != NULL) {
		g_free (details->type);
		g_free (details->sha);
		g_free (details->filename);
		g_free (details);
	}
}

void
ews_user_id_free (EwsUserId *id)
{
	if (id) {
		g_free (id->sid);
		g_free (id->primary_smtp);
		g_free (id->display_name);
		g_free (id->distinguished_user);
		g_free (id->external_user);
		g_free (id);
	}
}

void
ews_delegate_info_free (EwsDelegateInfo *info)
{
	if (!info)
		return;

	ews_user_id_free (info->user_id);
	g_free (info);
}

EEwsAttachmentInfo *
e_ews_attachment_info_new (EEwsAttachmentInfoType type)
{
	EEwsAttachmentInfo *info;
	info = g_new0 (EEwsAttachmentInfo, 1);

	info->type = type;
	return info;
}

void
e_ews_attachment_info_free (EEwsAttachmentInfo *info)
{
	if (!info)
		return;

	switch (info->type) {
	case E_EWS_ATTACHMENT_INFO_TYPE_INLINED:
		g_free (info->data.inlined.filename);
		g_free (info->data.inlined.mime_type);
		g_free (info->data.inlined.data);
		break;
	case E_EWS_ATTACHMENT_INFO_TYPE_URI:
		g_free (info->data.uri);
		break;
	default:
		g_warning ("Unknown EEwsAttachmentInfoType %d", info->type);
		break;
	}

	g_free (info);
}

EEwsAttachmentInfoType
e_ews_attachment_info_get_type (EEwsAttachmentInfo *info)
{
	return info->type;
}

const gchar *
e_ews_attachment_info_get_inlined_data (EEwsAttachmentInfo *info,
					gsize *len)
{
	g_return_val_if_fail (info != NULL, NULL);
	g_return_val_if_fail (info->type == E_EWS_ATTACHMENT_INFO_TYPE_INLINED, NULL);

	*len = info->data.inlined.length;
	return info->data.inlined.data;
}

void
e_ews_attachment_info_set_inlined_data (EEwsAttachmentInfo *info,
					const guchar *data,
					gsize len)
{
	g_return_if_fail (info != NULL);
	g_return_if_fail (info->type == E_EWS_ATTACHMENT_INFO_TYPE_INLINED);

	info->data.inlined.data = g_malloc (len);
	memcpy (info->data.inlined.data, data, len);
	info->data.inlined.length = len;
}

const gchar *
e_ews_attachment_info_get_mime_type (EEwsAttachmentInfo *info)
{
	g_return_val_if_fail (info != NULL, NULL);
	g_return_val_if_fail (info->type == E_EWS_ATTACHMENT_INFO_TYPE_INLINED, NULL);

	return info->data.inlined.mime_type;
}

void
e_ews_attachment_info_set_mime_type (EEwsAttachmentInfo *info,
				     const gchar *mime_type)
{
	g_return_if_fail (info != NULL);
	g_return_if_fail (info->type == E_EWS_ATTACHMENT_INFO_TYPE_INLINED);

	g_free (info->data.inlined.mime_type);
	info->data.inlined.mime_type = g_strdup (mime_type);
}

const gchar *
e_ews_attachment_info_get_filename (EEwsAttachmentInfo *info)
{
	g_return_val_if_fail (info != NULL, NULL);
	g_return_val_if_fail (info->type == E_EWS_ATTACHMENT_INFO_TYPE_INLINED, NULL);

	return info->data.inlined.filename;
}

void
e_ews_attachment_info_set_filename (EEwsAttachmentInfo *info,
				    const gchar *filename)
{
	g_return_if_fail (info != NULL);
	g_return_if_fail (info->type == E_EWS_ATTACHMENT_INFO_TYPE_INLINED);

	g_free (info->data.inlined.filename);
	info->data.inlined.filename = g_strdup (filename);
}


const gchar *
e_ews_attachment_info_get_uri (EEwsAttachmentInfo *info)
{
	g_return_val_if_fail (info != NULL, NULL);
	g_return_val_if_fail (info->type == E_EWS_ATTACHMENT_INFO_TYPE_URI, NULL);

	return info->data.uri;
}

void
e_ews_attachment_info_set_uri (EEwsAttachmentInfo *info,
			       const gchar *uri)
{
	g_return_if_fail (info != NULL);
	g_return_if_fail (info->type == E_EWS_ATTACHMENT_INFO_TYPE_URI);

	g_free (info->data.uri);
	info->data.uri = g_strdup (uri);
}

/* Connection APIS */

/**
 * e_ews_connection_find
 * @uri: Exchange server uri
 * @username:
 *
 * Find an existing connection for this user/uri, if it exists.
 *
 * Returns: EEwsConnection
 **/
EEwsConnection *
e_ews_connection_find (const gchar *uri,
                       const gchar *username)
{
	EEwsConnection *cnc;
	gchar *hash_key;

	g_mutex_lock (&connecting);

	/* search the connection in our hash table */
	if (loaded_connections_permissions != NULL) {
		hash_key = g_strdup_printf (
			"%s@%s",
			username ? username : "",
			uri);
		cnc = g_hash_table_lookup (
			loaded_connections_permissions, hash_key);
		g_free (hash_key);

		if (E_IS_EWS_CONNECTION (cnc)) {
			g_object_ref (cnc);
			g_mutex_unlock (&connecting);
			return cnc;
		}
	}

	g_mutex_unlock (&connecting);

	return NULL;
}

/**
 * e_ews_connection_new
 * @uri: Exchange server uri
 * @settings: a #CamelEwsSettings
 *
 * This does not authenticate to the server. It merely stores the username and password.
 * Authentication happens when a request is made to the server.
 *
 * Returns: EEwsConnection
 **/
EEwsConnection *
e_ews_connection_new (const gchar *uri,
                      CamelEwsSettings *settings)
{
	CamelNetworkSettings *network_settings;
	EEwsConnection *cnc;
	gchar *hash_key;
	gchar *user;

	g_return_val_if_fail (uri != NULL, NULL);
	g_return_val_if_fail (CAMEL_IS_EWS_SETTINGS (settings), NULL);

	network_settings = CAMEL_NETWORK_SETTINGS (settings);
	user = camel_network_settings_dup_user (network_settings);
	hash_key = g_strdup_printf ("%s@%s", user, uri);
	g_free (user);

	g_mutex_lock (&connecting);

	/* search the connection in our hash table */
	if (loaded_connections_permissions != NULL) {
		cnc = g_hash_table_lookup (
			loaded_connections_permissions, hash_key);

		if (E_IS_EWS_CONNECTION (cnc)) {
			g_object_ref (cnc);

			g_free (hash_key);

			g_mutex_unlock (&connecting);
			return cnc;
		}
	}

	/* not found, so create a new connection */
	cnc = g_object_new (
		E_TYPE_EWS_CONNECTION,
		"settings", settings, NULL);

	cnc->priv->uri = g_strdup (uri);
	cnc->priv->hash_key = hash_key;  /* takes ownership */

	g_free (cnc->priv->impersonate_user);
	if (camel_ews_settings_get_use_impersonation (settings)) {
		cnc->priv->impersonate_user = camel_ews_settings_dup_impersonate_user (settings);
		if (cnc->priv->impersonate_user && !*cnc->priv->impersonate_user) {
			g_free (cnc->priv->impersonate_user);
			cnc->priv->impersonate_user = NULL;
		}
	} else {
		cnc->priv->impersonate_user = NULL;
	}

	g_object_bind_property_full (
		settings, "auth-mechanism",
		cnc->priv->soup_session, "use-ntlm",
		G_BINDING_SYNC_CREATE,
		ews_auth_mech_to_use_ntlm,
		NULL,
		NULL, (GDestroyNotify) NULL);

	g_object_bind_property (
		settings, "timeout",
		cnc->priv->soup_session, "timeout",
		G_BINDING_SYNC_CREATE);

	/* add the connection to the loaded_connections_permissions hash table */
	if (loaded_connections_permissions == NULL)
		loaded_connections_permissions = g_hash_table_new_full (
			g_str_hash, g_str_equal,
			g_free, NULL);
	g_hash_table_insert (
		loaded_connections_permissions,
		g_strdup (cnc->priv->hash_key), cnc);

	/* update proxy with set 'uri' */
	proxy_settings_changed (cnc->priv->proxy, cnc);

	/* free memory */
	g_mutex_unlock (&connecting);
	return cnc;

}

const gchar *
e_ews_connection_get_uri (EEwsConnection *cnc)
{
	g_return_val_if_fail (E_IS_EWS_CONNECTION (cnc), NULL);

	return cnc->priv->uri;
}

const gchar *
e_ews_connection_get_password (EEwsConnection *cnc)
{
	g_return_val_if_fail (E_IS_EWS_CONNECTION (cnc), NULL);

	return cnc->priv->password;
}

gchar *
e_ews_connection_dup_password (EEwsConnection *cnc)
{
	const gchar *protected;
	gchar *duplicate;

	g_return_val_if_fail (E_IS_EWS_CONNECTION (cnc), NULL);

	g_mutex_lock (&cnc->priv->password_lock);

	protected = e_ews_connection_get_password (cnc);
	duplicate = g_strdup (protected);

	g_mutex_unlock (&cnc->priv->password_lock);

	return duplicate;
}

void
e_ews_connection_set_password (EEwsConnection *cnc,
                               const gchar *password)
{
	g_return_if_fail (E_IS_EWS_CONNECTION (cnc));

	g_mutex_lock (&cnc->priv->password_lock);

	/* Zero-fill the old password before freeing it. */
	if (cnc->priv->password != NULL && *cnc->priv->password != '\0')
		memset (cnc->priv->password, 0, strlen (cnc->priv->password));

	g_free (cnc->priv->password);
	cnc->priv->password = g_strdup (password);

	g_mutex_unlock (&cnc->priv->password_lock);

	g_object_notify (G_OBJECT (cnc), "password");
}

const gchar *
e_ews_connection_get_impersonate_user (EEwsConnection *cnc)
{
	g_return_val_if_fail (E_IS_EWS_CONNECTION (cnc), NULL);

	return cnc->priv->impersonate_user;
}

CamelEwsSettings *
e_ews_connection_ref_settings (EEwsConnection *cnc)
{
	g_return_val_if_fail (E_IS_EWS_CONNECTION (cnc), NULL);

	return g_object_ref (cnc->priv->settings);
}

SoupSession *
e_ews_connection_ref_soup_session (EEwsConnection *cnc)
{
	g_return_val_if_fail (E_IS_EWS_CONNECTION (cnc), NULL);

	return g_object_ref (cnc->priv->soup_session);
}

static xmlDoc *
e_ews_autodiscover_ws_xml (const gchar *email_address)
{
	xmlDoc *doc;
	xmlNode *node;
	xmlNs *ns;

	doc = xmlNewDoc ((xmlChar *) "1.0");
	node = xmlNewDocNode (doc, NULL, (xmlChar *)"Autodiscover", NULL);
	xmlDocSetRootElement (doc, node);
	ns = xmlNewNs (
		node,
		(xmlChar *)"http://schemas.microsoft.com/exchange/autodiscover/outlook/requestschema/2006", NULL);

	node = xmlNewChild (node, ns, (xmlChar *)"Request", NULL);
	xmlNewChild (
		node, ns, (xmlChar *)"EMailAddress",
		(xmlChar *) email_address);
	xmlNewChild (
		node, ns, (xmlChar *)"AcceptableResponseSchema",
		(xmlChar *)"http://schemas.microsoft.com/exchange/autodiscover/outlook/responseschema/2006a");

	return doc;
}

struct _autodiscover_data {
	EEwsConnection *cnc;
	xmlOutputBuffer *buf;
	SoupMessage *msgs[4];

	GCancellable *cancellable;
	gulong cancel_id;

	/* Results */
	gchar *as_url;
	gchar *oab_url;
};

static void
autodiscover_data_free (struct _autodiscover_data *ad)
{
	g_object_unref (ad->cnc);
	xmlOutputBufferClose (ad->buf);

	if (ad->cancellable != NULL) {
		g_cancellable_disconnect (ad->cancellable, ad->cancel_id);
		g_object_unref (ad->cancellable);
	}

	g_free (ad->as_url);
	g_free (ad->oab_url);

	g_slice_free (struct _autodiscover_data, ad);
}

static void
ews_dump_raw_soup_response (SoupMessage *msg)
{
	if (g_getenv ("EWS_DEBUG") && (atoi (g_getenv ("EWS_DEBUG")) >= 1)) {
		soup_buffer_free (soup_message_body_flatten (SOUP_MESSAGE (msg)->response_body));
		/* print response body */
		printf ("\n The response headers");
		printf ("\n =====================");
		fputc ('\n', stdout);
		fputs (SOUP_MESSAGE (msg)->response_body->data, stdout);
		fputc ('\n', stdout);
	}

}

static void
autodiscover_cancelled_cb (GCancellable *cancellable,
                           EEwsConnection *cnc)
{
	ews_connection_schedule_abort (cnc);
}

/* Called when each soup message completes */
static void
autodiscover_response_cb (SoupSession *session,
                          SoupMessage *msg,
                          gpointer data)

{
	GSimpleAsyncResult *simple = data;
	struct _autodiscover_data *ad;
	EwsUrls *urls = NULL;
	guint status = msg->status_code;
	xmlDoc *doc;
	xmlNode *node;
	gint idx;
	gboolean success = FALSE;
	GError *error = NULL;

	ad = g_simple_async_result_get_op_res_gpointer (simple);

	for (idx = 0; idx < 4; idx++) {
		if (ad->msgs[idx] == msg)
			break;
	}
	if (idx == 4) {
		/* We already got removed (cancelled). Do nothing */
		goto unref;
	}

	ad->msgs[idx] = NULL;

	if (status != 200) {
		g_set_error (
			&error, SOUP_HTTP_ERROR, status,
			"%d %s", status, msg->reason_phrase);
		goto failed;
	}

	ews_dump_raw_soup_response (msg);
	doc = xmlReadMemory (
		msg->response_body->data,
		msg->response_body->length,
		"autodiscover.xml", NULL, 0);
	if (!doc) {
		g_set_error (
			&error, EWS_CONNECTION_ERROR, -1,
			_("Failed to parse autodiscover response XML"));
		goto failed;
	}
	node = xmlDocGetRootElement (doc);
	if (strcmp ((gchar *) node->name, "Autodiscover")) {
		g_set_error (
			&error, EWS_CONNECTION_ERROR, -1,
			_("Failed to find <Autodiscover> element"));
		goto failed;
	}
	for (node = node->children; node; node = node->next) {
		if (node->type == XML_ELEMENT_NODE &&
		    !strcmp ((gchar *) node->name, "Response"))
			break;
	}
	if (!node) {
		g_set_error (
			&error, EWS_CONNECTION_ERROR, -1,
			_("Failed to find <Response> element"));
		goto failed;
	}
	for (node = node->children; node; node = node->next) {
		if (node->type == XML_ELEMENT_NODE &&
		    !strcmp ((gchar *) node->name, "Account"))
			break;
	}
	if (!node) {
		g_set_error (
			&error, EWS_CONNECTION_ERROR, -1,
			_("Failed to find <Account> element"));
		goto failed;
	}

	urls = g_new0 (EwsUrls, 1);
	for (node = node->children; node; node = node->next) {
		if (node->type == XML_ELEMENT_NODE &&
		    !strcmp ((gchar *) node->name, "Protocol")) {
			success = autodiscover_parse_protocol (node, urls);
			break;
		}
	}

	if (!success) {
		g_free (urls->as_url);
		g_free (urls->oab_url);
		g_free (urls);
		g_set_error (
			&error, EWS_CONNECTION_ERROR, -1,
			_("Failed to find <ASUrl> and <OABUrl> in autodiscover response"));
		goto failed;
	}

	/* We have a good response; cancel all the others */
	for (idx = 0; idx < 4; idx++) {
		if (ad->msgs[idx]) {
			SoupMessage *m = ad->msgs[idx];
			ad->msgs[idx] = NULL;
			ews_connection_schedule_cancel_message (ad->cnc, m);
		}
	}

	ad->as_url = urls->as_url;
	urls->as_url = NULL;

	ad->oab_url = urls->oab_url;
	urls->oab_url = NULL;

	g_free (urls);

	goto exit;

 failed:
	for (idx = 0; idx < 4; idx++) {
		if (ad->msgs[idx]) {
			/* There's another request outstanding.
			 * Hope that it has better luck. */
			g_clear_error (&error);
			goto unref;
		}
	}

	/* FIXME: We're actually returning the *last* error here,
	 * and in some cases (stupid firewalls causing timeouts)
	 * that's going to be the least interesting one. We probably
	 * want the *first* error */
	g_simple_async_result_take_error (simple, error);

 exit:
	g_simple_async_result_complete_in_idle (simple);

 unref:
	/* This function is processed within e_ews_soup_thread() and the 'simple'
	 * holds reference to EEwsConnection. For cases when this is the last
	 * reference to 'simple' the unref would cause crash, because of g_thread_join()
	 * in connection's dispose, trying to wait on the end of itself, thus it's
	 * safer to unref the 'simple' in a dedicated thread.
	*/
	ews_unref_in_thread (simple);
}

static void post_restarted (SoupMessage *msg, gpointer data)
{
	xmlOutputBuffer *buf = data;

	/* In violation of RFC2616, libsoup will change a POST request to
	 * a GET on receiving a 302 redirect. */
	printf ("Working around libsoup bug with redirect\n");
	g_object_set (msg, SOUP_MESSAGE_METHOD, "POST", NULL);

	soup_message_set_request (
		msg, "text/xml; charset=utf-8", SOUP_MEMORY_COPY,
		(gchar *)
			#ifdef LIBXML2_NEW_BUFFER
			xmlOutputBufferGetContent (buf), xmlOutputBufferGetSize (buf)
			#else
			buf->buffer->content, buf->buffer->use
			#endif
		);
}

static SoupMessage *
e_ews_get_msg_for_url (const gchar *url,
                       xmlOutputBuffer *buf,
                       GError **error)
{
	SoupMessage *msg;

	if (url == NULL) {
		g_set_error_literal (
			error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			_("URL cannot be NULL"));
		return NULL;
	}

	msg = soup_message_new (buf != NULL ? "POST" : "GET", url);
	if (!msg) {
		g_set_error (
			error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			_("URL '%s' is not valid"), url);
		return NULL;
	}

	soup_message_headers_append (
		msg->request_headers,
		"User-Agent", "libews/0.1");

	if (buf != NULL) {
		soup_message_set_request (
			msg, "text/xml; charset=utf-8", SOUP_MEMORY_COPY,
			(gchar *)
			#ifdef LIBXML2_NEW_BUFFER
			xmlOutputBufferGetContent (buf), xmlOutputBufferGetSize (buf)
			#else
			buf->buffer->content, buf->buffer->use
			#endif
			);
		g_signal_connect (
			msg, "restarted",
			G_CALLBACK (post_restarted), buf);
	}

	if (g_getenv ("EWS_DEBUG") && (atoi (g_getenv ("EWS_DEBUG")) >= 1)) {
		soup_buffer_free (
			soup_message_body_flatten (
			SOUP_MESSAGE (msg)->request_body));
		/* print request's body */
		printf ("\n The request headers");
		printf ("\n ===================");
		fputc ('\n', stdout);
		fputs (SOUP_MESSAGE (msg)->request_body->data, stdout);
		fputc ('\n', stdout);
	}

	return msg;
}

gboolean
e_ews_autodiscover_ws_url_sync (CamelEwsSettings *settings,
                                const gchar *email_address,
                                const gchar *password,
                                GCancellable *cancellable,
                                GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (CAMEL_IS_EWS_SETTINGS (settings), FALSE);
	g_return_val_if_fail (email_address != NULL, FALSE);
	g_return_val_if_fail (password != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_autodiscover_ws_url (
		settings, email_address, password, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_autodiscover_ws_url_finish (settings, result, error);

	e_async_closure_free (closure);

	return success;
}

void
e_ews_autodiscover_ws_url (CamelEwsSettings *settings,
                           const gchar *email_address,
                           const gchar *password,
                           GCancellable *cancellable,
                           GAsyncReadyCallback callback,
                           gpointer user_data)
{
	CamelNetworkSettings *network_settings;
	GSimpleAsyncResult *simple;
	struct _autodiscover_data *ad;
	xmlOutputBuffer *buf;
	gchar *url1, *url2, *url3, *url4;
	gchar *domain;
	xmlDoc *doc;
	EEwsConnection *cnc;
	SoupURI *soup_uri = NULL;
	gboolean use_secure = TRUE;
	const gchar *host_url;
	const gchar *user;
	GError *error = NULL;

	g_return_if_fail (CAMEL_IS_EWS_SETTINGS (settings));
	g_return_if_fail (email_address != NULL);
	g_return_if_fail (password != NULL);

	simple = g_simple_async_result_new (
		G_OBJECT (settings), callback,
		user_data, e_ews_autodiscover_ws_url);

	domain = strchr (email_address, '@');
	if (domain == NULL || *domain == '\0') {
		g_simple_async_result_set_error (
			simple, EWS_CONNECTION_ERROR, -1,
			"%s", _("Email address is missing a domain part"));
		g_simple_async_result_complete_in_idle (simple);
		g_object_unref (simple);
		return;
	}
	domain++;

	doc = e_ews_autodiscover_ws_xml (email_address);
	buf = xmlAllocOutputBuffer (NULL);
	xmlNodeDumpOutput (buf, doc, xmlDocGetRootElement (doc), 0, 1, NULL);
	xmlOutputBufferFlush (buf);

	url1 = NULL;
	url2 = NULL;
	url3 = NULL;
	url4 = NULL;

	host_url = camel_ews_settings_get_hosturl (settings);
	if (host_url != NULL)
		soup_uri = soup_uri_new (host_url);

	if (soup_uri != NULL) {
		const gchar *host = soup_uri_get_host (soup_uri);
		const gchar *scheme = soup_uri_get_scheme (soup_uri);

		use_secure = g_strcmp0 (scheme, "https") == 0;

		url1 = g_strdup_printf ("http%s://%s/autodiscover/autodiscover.xml", use_secure ? "s" : "", host);
		url2 = g_strdup_printf ("http%s://autodiscover.%s/autodiscover/autodiscover.xml", use_secure ? "s" : "", host);

		soup_uri_free (soup_uri);
	}

	url3 = g_strdup_printf ("http%s://%s/autodiscover/autodiscover.xml", use_secure ? "s" : "", domain);
	url4 = g_strdup_printf ("http%s://autodiscover.%s/autodiscover/autodiscover.xml", use_secure ? "s" : "", domain);

	network_settings = CAMEL_NETWORK_SETTINGS (settings);
	user = camel_network_settings_get_user (network_settings);
	if (user == NULL || *user == '\0')
		user = email_address;

	cnc = e_ews_connection_new (url3, settings);
	e_ews_connection_set_password (cnc, password);

	/*
	 * http://msdn.microsoft.com/en-us/library/ee332364.aspx says we are
	 * supposed to try $domain and then autodiscover.$domain. But some
	 * people have broken firewalls on the former which drop packets
	 * instead of rejecting connections, and make the request take ages
	 * to time out. So run both queries in parallel and let the fastest
	 * (successful) one win.
	 */
	ad = g_slice_new0 (struct _autodiscover_data);
	ad->cnc = cnc;  /* takes ownership */
	ad->buf = buf;  /* takes ownership */

	if (G_IS_CANCELLABLE (cancellable)) {
		ad->cancellable = g_object_ref (cancellable);
		ad->cancel_id = g_cancellable_connect (
			ad->cancellable,
			G_CALLBACK (autodiscover_cancelled_cb),
			g_object_ref (cnc),
			g_object_unref);
	}

	g_simple_async_result_set_op_res_gpointer (
		simple, ad, (GDestroyNotify) autodiscover_data_free);

	/* Passing a NULL URL string returns NULL. */
	ad->msgs[0] = e_ews_get_msg_for_url (url1, buf, &error);
	ad->msgs[1] = e_ews_get_msg_for_url (url2, buf, NULL);
	ad->msgs[2] = e_ews_get_msg_for_url (url3, buf, NULL);
	ad->msgs[3] = e_ews_get_msg_for_url (url4, buf, NULL);

	/* These have to be submitted only after they're both set in ad->msgs[]
	 * or there will be races with fast completion */
	if (ad->msgs[0] != NULL)
		ews_connection_schedule_queue_message (cnc, ad->msgs[0], autodiscover_response_cb, g_object_ref (simple));
	if (ad->msgs[1] != NULL)
		ews_connection_schedule_queue_message (cnc, ad->msgs[1], autodiscover_response_cb, g_object_ref (simple));
	if (ad->msgs[2] != NULL)
		ews_connection_schedule_queue_message (cnc, ad->msgs[2], autodiscover_response_cb, g_object_ref (simple));
	if (ad->msgs[3] != NULL)
		ews_connection_schedule_queue_message (cnc, ad->msgs[3], autodiscover_response_cb, g_object_ref (simple));

	xmlFreeDoc (doc);
	g_free (url1);
	g_free (url2);
	g_free (url3);
	g_free (url4);

	if (error && !ad->msgs[0] && !ad->msgs[1] && !ad->msgs[2] && !ad->msgs[3]) {
		g_simple_async_result_take_error (simple, error);
		g_simple_async_result_complete_in_idle (simple);
	} else {
		g_clear_error (&error);

		/* each request holds a reference to 'simple',
		 * thus remove one, to have it actually freed */
		g_object_unref (simple);
	}
}

static gboolean
has_suffix_icmp (const gchar *text,
                 const gchar *suffix)
{
	gint ii, tlen, slen;

	g_return_val_if_fail (text != NULL, FALSE);
	g_return_val_if_fail (suffix != NULL, FALSE);

	tlen = strlen (text);
	slen = strlen (suffix);

	if (!*text || !*suffix || tlen < slen)
		return FALSE;

	for (ii = 0; ii < slen; ii++) {
		if (g_ascii_tolower (text[tlen - ii - 1]) !=
		    g_ascii_tolower (suffix[slen - ii - 1]))
			break;
	}

	return ii == slen;
}

gboolean
e_ews_autodiscover_ws_url_finish (CamelEwsSettings *settings,
                                  GAsyncResult *result,
                                  GError **error)
{
	GSimpleAsyncResult *simple;
	struct _autodiscover_data *ad;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (settings),
		e_ews_autodiscover_ws_url), FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	ad = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	g_warn_if_fail (ad->as_url != NULL);
	g_warn_if_fail (ad->oab_url != NULL);

	camel_ews_settings_set_hosturl (settings, ad->as_url);

	if (!has_suffix_icmp (ad->oab_url, "oab.xml")) {
		gchar *tmp;

		if (g_str_has_suffix (ad->oab_url, "/"))
			tmp = g_strconcat (ad->oab_url, "oab.xml", NULL);
		else
			tmp = g_strconcat (ad->oab_url, "/", "oab.xml", NULL);

		camel_ews_settings_set_oaburl (settings, tmp);
		g_free (tmp);
	} else {
		camel_ews_settings_set_oaburl (settings, ad->oab_url);
	}

	return TRUE;
}

struct _oal_req_data {
	EEwsConnection *cnc;
	SoupMessage *soup_message;
	gchar *oal_id;
	gchar *oal_element;

	GSList *oals;
	GSList *elements;
	gchar *etag;

	GCancellable *cancellable;
	gulong cancel_id;

	/* for dowloading oal file */
	gchar *cache_filename;
	GError *error;
	EwsProgressFn progress_fn;
	gpointer progress_data;
	gsize response_size;
	gsize received_size;
};

static void
oal_req_data_free (struct _oal_req_data *data)
{
	/* The SoupMessage is owned by the SoupSession. */
	g_object_unref (data->cnc);

	g_free (data->oal_id);
	g_free (data->oal_element);
	g_free (data->etag);

	g_slist_free_full (data->oals, (GDestroyNotify) ews_oal_free);
	g_slist_free_full (data->elements, (GDestroyNotify) ews_oal_details_free);

	if (data->cancellable != NULL) {
		g_cancellable_disconnect (data->cancellable, data->cancel_id);
		g_object_unref (data->cancellable);
	}

	g_free (data->cache_filename);

	g_slice_free (struct _oal_req_data, data);
}

static gchar *
get_property (xmlNodePtr node_ptr,
              const gchar *name)
{
	xmlChar *xml_s;
	gchar *s;

	xml_s = xmlGetProp (node_ptr, (const xmlChar *) name);
	s = g_strdup ((gchar *) xml_s);
	xmlFree (xml_s);

	return s;
}

static guint32
get_property_as_uint32 (xmlNodePtr node_ptr,
                        const gchar *name)
{
	gchar *s;
	guint32 val = -1;

	s = get_property (node_ptr, name);
	if (s)
		sscanf (s,"%"G_GUINT32_FORMAT, &val);
	g_free (s);

	return val;
}

static gchar *
get_content (xmlNodePtr node_ptr)
{
	xmlChar *xml_s;
	gchar *s;

	xml_s = xmlNodeGetContent (node_ptr);
	s = g_strdup ((gchar *) xml_s);
	xmlFree (xml_s);

	return s;
}

static GSList *
parse_oal_full_details (xmlNode *node,
                        const gchar *element)
{
	GSList *elements = NULL;

	for (node = node->children; node; node = node->next) {
		EwsOALDetails *det;
		if (node->type != XML_ELEMENT_NODE)
			continue;
		if (element && strcmp ((gchar *) node->name, element))
			continue;
		if (!element && strcmp ((gchar *) node->name, "Full") &&
		    strcmp((gchar *) node->name, "Diff"))
			continue;

		det = g_new0 (EwsOALDetails, 1);
		det->type = g_strdup((gchar *) node->name);
		det->seq = get_property_as_uint32 (node, "seq");
		det->ver = get_property_as_uint32 (node, "ver");
		det->size = get_property_as_uint32 (node, "size");
		det->uncompressed_size = get_property_as_uint32 (node, "uncompressedsize");
		det->sha = get_property (node, "uncompressedsize");
		det->filename = g_strstrip (get_content (node));

		elements = g_slist_prepend (elements, det);
		if (element && !strcmp (element, "Full"))
			break;
	}

	return elements;
}

/* this is run in cnc->priv->soup_thread */
static void
oal_response_cb (SoupSession *soup_session,
                 SoupMessage *soup_message,
                 gpointer user_data)
{
	GSimpleAsyncResult *simple;
	struct _oal_req_data *data;
	const gchar *etag;
	xmlDoc *doc;
	xmlNode *node;

	simple = G_SIMPLE_ASYNC_RESULT (user_data);
	data = g_simple_async_result_get_op_res_gpointer (simple);

	if (soup_message->status_code != 200) {
		g_simple_async_result_set_error (
			simple, SOUP_HTTP_ERROR,
			soup_message->status_code,
			"%d %s",
			soup_message->status_code,
			soup_message->reason_phrase);
		goto exit;
	}

	etag = soup_message_headers_get_one(soup_message->response_headers,
					    "ETag");
	if (etag)
		data->etag = g_strdup(etag);

	ews_dump_raw_soup_response (soup_message);

	doc = xmlReadMemory (
		soup_message->response_body->data,
		soup_message->response_body->length,
		"oab.xml", NULL, 0);
	if (doc == NULL) {
		g_simple_async_result_set_error (
			simple, EWS_CONNECTION_ERROR, -1,
			"%s", _("Failed to parse oab XML"));
		goto exit;
	}

	node = xmlDocGetRootElement (doc);
	if (strcmp ((gchar *) node->name, "OAB") != 0) {
		g_simple_async_result_set_error (
			simple, EWS_CONNECTION_ERROR, -1,
			"%s", _("Failed to find <OAB> element\n"));
		goto exit_doc;
	}

	for (node = node->children; node; node = node->next) {
		if (node->type == XML_ELEMENT_NODE && strcmp ((gchar *) node->name, "OAL") == 0) {
			if (data->oal_id == NULL) {
				EwsOAL *oal = g_new0 (EwsOAL, 1);

				oal->id = get_property (node, "id");
				oal->dn = get_property (node, "dn");
				oal->name = get_property (node, "name");

				data->oals = g_slist_prepend (data->oals, oal);
			} else {
				gchar *id = get_property (node, "id");

				if (strcmp (id, data->oal_id) == 0) {
					/* parse details of full_details file */
					data->elements = parse_oal_full_details (node, data->oal_element);

					g_free (id);
					break;
				}

				g_free (id);
			}
		}
	}

	data->oals = g_slist_reverse (data->oals);

 exit_doc:
	xmlFreeDoc (doc);
 exit:
	g_simple_async_result_complete_in_idle (simple);
	/* This is run in cnc->priv->soup_thread, and the cnc is held by simple, thus
	 * for cases when the complete_in_idle is finished before the unref call, when
	 * the cnc will be left with the last reference and thus cannot join the soup_thread
	 * while still in it, the unref is done in a dedicated thread. */
	ews_unref_in_thread (simple);
}

static void
ews_cancel_msg (GCancellable *cancellable,
                struct _oal_req_data *data)
{
	ews_connection_schedule_cancel_message (data->cnc, data->soup_message);
}

gboolean
e_ews_connection_get_oal_list_sync (EEwsConnection *cnc,
                                    GSList **oals,
                                    GCancellable *cancellable,
                                    GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (E_IS_EWS_CONNECTION (cnc), FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_get_oal_list (
		cnc, cancellable, e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_get_oal_list_finish (
		cnc, result, oals, error);

	e_async_closure_free (closure);

	return success;
}

void
e_ews_connection_get_oal_list (EEwsConnection *cnc,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
	GSimpleAsyncResult *simple;
	SoupMessage *soup_message;
	struct _oal_req_data *data;
	GError *error = NULL;

	g_return_if_fail (E_IS_EWS_CONNECTION (cnc));

	soup_message = e_ews_get_msg_for_url (cnc->priv->uri, NULL, &error);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_get_oal_list);

	if (!soup_message) {
		g_simple_async_result_take_error (simple, error);
		g_simple_async_result_complete_in_idle (simple);
		return;
	}

	data = g_slice_new0 (struct _oal_req_data);
	data->cnc = g_object_ref (cnc);
	data->soup_message = soup_message;  /* the session owns this */

	if (G_IS_CANCELLABLE (cancellable)) {
		data->cancellable = g_object_ref (cancellable);
		data->cancel_id = g_cancellable_connect (
			data->cancellable,
			G_CALLBACK (ews_cancel_msg),
			data, (GDestroyNotify) NULL);
	}

	g_simple_async_result_set_op_res_gpointer (
		simple, data, (GDestroyNotify) oal_req_data_free);

	ews_connection_schedule_queue_message (cnc, soup_message, oal_response_cb, simple);
}

gboolean
e_ews_connection_get_oal_list_finish (EEwsConnection *cnc,
                                      GAsyncResult *result,
                                      GSList **oals,
                                      GError **error)
{
	GSimpleAsyncResult *simple;
	struct _oal_req_data *data;

	g_return_val_if_fail (E_IS_EWS_CONNECTION (cnc), FALSE);

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_get_oal_list),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	if (oals != NULL) {
		*oals = data->oals;
		data->oals = NULL;
	}

	return TRUE;
}

/**
 * e_ews_connection_get_oal_detail 
 * @cnc: 
 * @oal_id: 
 * @oal_element: 
 * @elements: "Full" "Diff" "Template" are the possible values.
 * @cancellable: 
 * @error: 
 * 
 * 
 * Returns: 
 **/
gboolean
e_ews_connection_get_oal_detail_sync (EEwsConnection *cnc,
                                      const gchar *oal_id,
                                      const gchar *oal_element,
				      const gchar *old_etag,
                                      GSList **elements,
				      gchar **etag,
                                      GCancellable *cancellable,
                                      GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (E_IS_EWS_CONNECTION (cnc), FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_get_oal_detail (
		cnc, oal_id, oal_element, old_etag,
		cancellable, e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_get_oal_detail_finish (
		cnc, result, elements, etag, error);

	e_async_closure_free (closure);

	return success;
}

void
e_ews_connection_get_oal_detail (EEwsConnection *cnc,
                                 const gchar *oal_id,
                                 const gchar *oal_element,
				 const gchar *etag,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data)
{
	GSimpleAsyncResult *simple;
	SoupMessage *soup_message;
	struct _oal_req_data *data;
	gchar *sep;
	GError *error = NULL;

	g_return_if_fail (E_IS_EWS_CONNECTION (cnc));

	soup_message = e_ews_get_msg_for_url (cnc->priv->uri, NULL, &error);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_get_oal_detail);

	if (!soup_message) {
		g_simple_async_result_take_error (simple, error);
		g_simple_async_result_complete_in_idle (simple);
		return;
	}

	if (etag && *etag)
		soup_message_headers_append (soup_message->request_headers,
					     "If-None-Match", etag);

	data = g_slice_new0 (struct _oal_req_data);
	data->cnc = g_object_ref (cnc);
	data->soup_message = soup_message;  /* the session owns this */
	data->oal_id = g_strdup (oal_id);
	data->oal_element = g_strdup (oal_element);

	/* oal_id can be of form "GUID:name", but here is compared only GUID */
	sep = strchr (data->oal_id, ':');
	if (sep)
		*sep = '\0';

	if (G_IS_CANCELLABLE (cancellable)) {
		data->cancellable = g_object_ref (cancellable);
		data->cancel_id = g_cancellable_connect (
			data->cancellable,
			G_CALLBACK (ews_cancel_msg),
			data, (GDestroyNotify) NULL);
	}

	g_simple_async_result_set_op_res_gpointer (
		simple, data, (GDestroyNotify) oal_req_data_free);

	ews_connection_schedule_queue_message (cnc, soup_message, oal_response_cb, simple);
}

gboolean
e_ews_connection_get_oal_detail_finish (EEwsConnection *cnc,
                                        GAsyncResult *result,
                                        GSList **elements,
					gchar **etag,
                                        GError **error)
{
	GSimpleAsyncResult *simple;
	struct _oal_req_data *data;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_get_oal_detail),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	if (elements != NULL) {
		*elements = data->elements;
		data->elements = NULL;
	}
	if (etag != NULL) {
		*etag = data->etag;
		data->etag = NULL;
	}

	return TRUE;

}

static void
oal_download_response_cb (SoupSession *soup_session,
                          SoupMessage *soup_message,
                          gpointer user_data)
{
	GSimpleAsyncResult *simple;
	struct _oal_req_data *data;

	simple = G_SIMPLE_ASYNC_RESULT (user_data);
	data = g_simple_async_result_get_op_res_gpointer (simple);

	if (soup_message->status_code != 200) {
		g_simple_async_result_set_error (
			simple, SOUP_HTTP_ERROR,
			soup_message->status_code,
			"%d %s",
			soup_message->status_code,
			soup_message->reason_phrase);
		g_unlink (data->cache_filename);

	} else if (data->error != NULL) {
		g_simple_async_result_take_error (simple, data->error);
		data->error = NULL;
		g_unlink (data->cache_filename);
	}

	g_simple_async_result_complete_in_idle (simple);
	ews_unref_in_thread (simple);
}

static void
ews_soup_got_headers (SoupMessage *msg,
                      gpointer user_data)
{
	struct _oal_req_data *data = (struct _oal_req_data *) user_data;
	const gchar *size;

	size = soup_message_headers_get_one (
		msg->response_headers,
		"Content-Length");

	if (size)
		data->response_size = strtol (size, NULL, 10);
}

static void
ews_soup_restarted (SoupMessage *msg,
                    gpointer user_data)
{
	struct _oal_req_data *data = (struct _oal_req_data *) user_data;

	data->response_size = 0;
	data->received_size = 0;
}

static void
ews_soup_got_chunk (SoupMessage *msg,
                    SoupBuffer *chunk,
                    gpointer user_data)
{
	struct _oal_req_data *data = (struct _oal_req_data *) user_data;
	gint fd;

	if (msg->status_code != 200)
		return;

	data->received_size += chunk->length;

	if (data->response_size && data->progress_fn) {
		gint pc = data->received_size * 100 / data->response_size;
		data->progress_fn (data->progress_data, pc);
	}

	fd = g_open (data->cache_filename, O_RDONLY | O_WRONLY | O_APPEND | O_CREAT, 0600);
	if (fd != -1) {
		if (write (fd, (const gchar *) chunk->data, chunk->length) != chunk->length) {
			g_set_error (
				&data->error, EWS_CONNECTION_ERROR, EWS_CONNECTION_ERROR_UNKNOWN,
				"Failed to write streaming data to file : %d ", errno);
		}
#ifdef G_OS_WIN32
		closesocket (fd);
#else
		close (fd);
#endif
	} else {
		g_set_error (
			&data->error, EWS_CONNECTION_ERROR, EWS_CONNECTION_ERROR_UNKNOWN,
			"Failed to open the cache file : %d ", errno);
	}
}

gboolean
e_ews_connection_download_oal_file_sync (EEwsConnection *cnc,
                                         const gchar *cache_filename,
                                         EwsProgressFn progress_fn,
                                         gpointer progress_data,
                                         GCancellable *cancellable,
                                         GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (E_IS_EWS_CONNECTION (cnc), FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_download_oal_file (
		cnc, cache_filename,
		progress_fn, progress_data, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_download_oal_file_finish (
		cnc, result, error);

	e_async_closure_free (closure);

	return success;
}

void
e_ews_connection_download_oal_file (EEwsConnection *cnc,
                                    const gchar *cache_filename,
                                    EwsProgressFn progress_fn,
                                    gpointer progress_data,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data)
{
	GSimpleAsyncResult *simple;
	SoupMessage *soup_message;
	struct _oal_req_data *data;
	GError *error = NULL;

	g_return_if_fail (E_IS_EWS_CONNECTION (cnc));

	soup_message = e_ews_get_msg_for_url (cnc->priv->uri, NULL, &error);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_download_oal_file);

	if (!soup_message) {
		g_simple_async_result_take_error (simple, error);
		g_simple_async_result_complete_in_idle (simple);
		return;
	}

	data = g_slice_new0 (struct _oal_req_data);
	data->cnc = g_object_ref (cnc);
	data->soup_message = soup_message;  /* the session owns this */
	data->cache_filename = g_strdup (cache_filename);
	data->progress_fn = progress_fn;
	data->progress_data = progress_data;

	if (G_IS_CANCELLABLE (cancellable)) {
		data->cancellable = g_object_ref (cancellable);
		data->cancel_id = g_cancellable_connect (
			data->cancellable,
			G_CALLBACK (ews_cancel_msg),
			data, (GDestroyNotify) NULL);
	}

	g_simple_async_result_set_op_res_gpointer (
		simple, data, (GDestroyNotify) oal_req_data_free);

	soup_message_body_set_accumulate (soup_message->response_body, FALSE);

	g_signal_connect (
		soup_message, "got-headers",
		G_CALLBACK (ews_soup_got_headers), data);
	g_signal_connect (
		soup_message, "got-chunk",
		G_CALLBACK (ews_soup_got_chunk), data);
	g_signal_connect (
		soup_message, "restarted",
		G_CALLBACK (ews_soup_restarted), data);

	ews_connection_schedule_queue_message (cnc, soup_message, oal_download_response_cb, simple);
}

gboolean
e_ews_connection_download_oal_file_finish (EEwsConnection *cnc,
                                           GAsyncResult *result,
                                           GError **error)
{
	GSimpleAsyncResult *simple;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc),
		e_ews_connection_download_oal_file), FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);

	/* Assume success unless a GError is set. */
	return !g_simple_async_result_propagate_error (simple, error);
}

const gchar *
e_ews_connection_get_mailbox (EEwsConnection *cnc)
{
	g_return_val_if_fail (E_IS_EWS_CONNECTION (cnc), NULL);

	if (!cnc->priv->email || !*cnc->priv->email)
		return camel_ews_settings_get_email (cnc->priv->settings);

	return cnc->priv->email;
}

void
e_ews_connection_set_mailbox (EEwsConnection *cnc,
                              const gchar *email)
{
	g_return_if_fail (E_IS_EWS_CONNECTION (cnc));
	g_return_if_fail (email != NULL);

	g_free (cnc->priv->email);
	cnc->priv->email = g_strdup (email);
}

static void
ews_append_additional_props_to_msg (ESoapMessage *msg,
                                    EwsAdditionalProps *add_props)
{
	GSList *l;

	if (!add_props)
		return;

	e_soap_message_start_element (msg, "AdditionalProperties", NULL, NULL);

	if (add_props->field_uri) {
		gchar **prop = g_strsplit (add_props->field_uri, " ", 0);
		gint i = 0;

		while (prop[i]) {
			e_ews_message_write_string_parameter_with_attribute (msg, "FieldURI", NULL, NULL, "FieldURI", prop[i]);
			i++;
		}

		g_strfreev (prop);
	}

	if (add_props->extended_furis) {
		for (l = add_props->extended_furis; l != NULL; l = g_slist_next (l)) {
			EwsExtendedFieldURI *ex_furi = (EwsExtendedFieldURI *) l->data;

			e_soap_message_start_element (msg, "ExtendedFieldURI", NULL, NULL);

			if (ex_furi->distinguished_prop_set_id)
				e_soap_message_add_attribute (msg, "DistinguishedPropertySetId", ex_furi->distinguished_prop_set_id, NULL, NULL);

			if (ex_furi->prop_set_id)
				e_soap_message_add_attribute (msg, "PropertySetId", ex_furi->prop_set_id, NULL, NULL);

			if (ex_furi->prop_name)
				e_soap_message_add_attribute (msg, "PropertyName", ex_furi->prop_name, NULL, NULL);

			if (ex_furi->prop_id)
				e_soap_message_add_attribute (msg, "PropertyId", ex_furi->prop_id, NULL, NULL);

			if (ex_furi->prop_type)
				e_soap_message_add_attribute (msg, "PropertyType", ex_furi->prop_type, NULL, NULL);

			e_soap_message_end_element (msg);
		}
	}

	if (add_props->indexed_furis) {
		for (l = add_props->indexed_furis; l != NULL; l = g_slist_next (l)) {
			EwsIndexedFieldURI *in_furi = (EwsIndexedFieldURI *) l->data;

			e_soap_message_start_element (msg, "IndexedFieldURI", NULL, NULL);

			e_soap_message_add_attribute (msg, "FieldURI", in_furi->field_uri, NULL, NULL);
			e_soap_message_add_attribute (msg, "FieldIndex", in_furi->field_index, NULL, NULL);

			e_soap_message_end_element (msg);
		}
	}

	e_soap_message_end_element (msg);
}

static void
ews_write_sort_order_to_msg (ESoapMessage *msg,
                             EwsSortOrder *sort_order)
{
	if (!sort_order)
		return;

	e_soap_message_start_element (msg, "SortOrder", NULL, NULL);
	e_soap_message_start_element (msg, "FieldOrder", NULL, NULL);
	e_soap_message_add_attribute (msg, "Order", sort_order->order, NULL, NULL);

	if (sort_order->uri_type == NORMAL_FIELD_URI)
		e_ews_message_write_string_parameter_with_attribute (msg, "FieldURI", NULL, NULL, "FieldURI", (gchar *) sort_order->field_uri);
	else if (sort_order->uri_type == INDEXED_FIELD_URI) {
		EwsIndexedFieldURI *in_furi = (EwsIndexedFieldURI *) sort_order->field_uri;

		e_soap_message_start_element (msg, "IndexedFieldURI", NULL, NULL);
		e_soap_message_add_attribute (msg, "FieldURI", in_furi->field_uri, NULL, NULL);
		e_soap_message_add_attribute (msg, "FieldIndex", in_furi->field_index, NULL, NULL);
		e_soap_message_end_element (msg);
	} else if (sort_order->uri_type == EXTENDED_FIELD_URI) {
		EwsExtendedFieldURI *ex_furi = (EwsExtendedFieldURI *) sort_order->field_uri;

		e_soap_message_start_element (msg, "ExtendedFieldURI", NULL, NULL);

		if (ex_furi->distinguished_prop_set_id)
			e_soap_message_add_attribute (msg, "DistinguishedPropertySetId", ex_furi->distinguished_prop_set_id, NULL, NULL);
		if (ex_furi->prop_set_id)
			e_soap_message_add_attribute (msg, "PropertySetId", ex_furi->prop_set_id, NULL, NULL);
		if (ex_furi->prop_name)
			e_soap_message_add_attribute (msg, "PropertyName", ex_furi->prop_name, NULL, NULL);
		if (ex_furi->prop_id)
			e_soap_message_add_attribute (msg, "PropertyId", ex_furi->prop_id, NULL, NULL);
		if (ex_furi->prop_type)
			e_soap_message_add_attribute (msg, "PropertyType", ex_furi->prop_type, NULL, NULL);

		e_soap_message_end_element (msg);
	}

	e_soap_message_end_element (msg);
	e_soap_message_end_element (msg);
}

/**
 * e_ews_connection_sync_folder_items:
 * @cnc: The EWS Connection
 * @pri: The priority associated with the request
 * @last_sync_state: To sync with the previous requests
 * @folder_id: The folder to which the items belong
 * @default_props: Can take one of the values: IdOnly,Default or AllProperties
 * @additional_props: Specify any additional properties to be fetched
 * @max_entries: Maximum number of items to be returned
 * @cancellable: a GCancellable to monitor cancelled operations
 * @callback: Responses are parsed and returned to this callback
 * @user_data: user data passed to callback
 **/
void
e_ews_connection_sync_folder_items (EEwsConnection *cnc,
                                    gint pri,
                                    const gchar *last_sync_state,
                                    const gchar *fid,
                                    const gchar *default_props,
                                    const gchar *additional_props,
                                    guint max_entries,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_if_fail (cnc != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"SyncFolderItems",
			NULL,
			NULL,
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);
	e_soap_message_start_element (msg, "ItemShape", "messages", NULL);
	e_ews_message_write_string_parameter (msg, "BaseShape", NULL, default_props);

	if (additional_props && *additional_props) {
		gchar **prop = g_strsplit (additional_props, " ", 0);
		gint i = 0;

		e_soap_message_start_element (msg, "AdditionalProperties", NULL, NULL);
		while (prop[i]) {
			e_ews_message_write_string_parameter_with_attribute (msg, "FieldURI", NULL, NULL, "FieldURI", prop[i]);
			i++;
		}
		g_strfreev (prop);
		e_soap_message_end_element (msg);
	}

	e_soap_message_end_element (msg);

	e_soap_message_start_element (msg, "SyncFolderId", "messages", NULL);
	e_ews_message_write_string_parameter_with_attribute (msg, "FolderId", NULL, NULL, "Id", fid);
	e_soap_message_end_element (msg);

	if (last_sync_state)
		e_ews_message_write_string_parameter (msg, "SyncState", "messages", last_sync_state);

	/* Max changes requested */
	e_ews_message_write_int_parameter (msg, "MaxChangesReturned", "messages", max_entries);

	/* Complete the footer and print the request */
	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_sync_folder_items);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, sync_folder_items_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_sync_folder_items_finish (EEwsConnection *cnc,
                                           GAsyncResult *result,
                                           gchar **new_sync_state,
                                           gboolean *includes_last_item,
                                           GSList **items_created,
                                           GSList **items_updated,
                                           GSList **items_deleted,
                                           GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_sync_folder_items),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	*new_sync_state = async_data->sync_state;
	*includes_last_item = async_data->includes_last_item;
	*items_created = async_data->items_created;
	*items_updated = async_data->items_updated;
	*items_deleted = async_data->items_deleted;

	return TRUE;
}

gboolean
e_ews_connection_sync_folder_items_sync (EEwsConnection *cnc,
                                         gint pri,
                                         const gchar *old_sync_state,
                                         const gchar *fid,
                                         const gchar *default_props,
                                         const gchar *additional_props,
                                         guint max_entries,
                                         gchar **new_sync_state,
                                         gboolean *includes_last_item,
                                         GSList **items_created,
                                         GSList **items_updated,
                                         GSList **items_deleted,
                                         GCancellable *cancellable,
                                         GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (cnc != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_sync_folder_items (
		cnc, pri, old_sync_state, fid, default_props,
		additional_props, max_entries, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_sync_folder_items_finish (
		cnc, result, new_sync_state, includes_last_item,
		items_created, items_updated, items_deleted, error);

	e_async_closure_free (closure);

	return success;
}

static void
ews_append_folder_id_to_msg (ESoapMessage *msg,
                             const gchar *email,
                             const EwsFolderId *fid)
{
	g_return_if_fail (msg != NULL);
	g_return_if_fail (fid != NULL);

	if (fid->is_distinguished_id)
		e_soap_message_start_element (msg, "DistinguishedFolderId", NULL, NULL);
	else
		e_soap_message_start_element (msg, "FolderId", NULL, NULL);

	e_soap_message_add_attribute (msg, "Id", fid->id, NULL, NULL);
	if (fid->change_key)
		e_soap_message_add_attribute (msg, "ChangeKey", fid->change_key, NULL, NULL);

	if (fid->is_distinguished_id && email) {
		e_soap_message_start_element (msg, "Mailbox", NULL, NULL);
		e_ews_message_write_string_parameter (msg, "EmailAddress", NULL, email);
		e_soap_message_end_element (msg);
	}

	e_soap_message_end_element (msg);
}

static void
ews_append_folder_ids_to_msg (ESoapMessage *msg,
                              const gchar *email,
                              GSList *folder_ids)
{
	GSList *l;

	for (l = folder_ids; l != NULL; l = g_slist_next (l)) {
		const EwsFolderId *fid = l->data;

		ews_append_folder_id_to_msg (msg, email, fid);
	}
}

/**
 * e_ews_connection_find_folder_items:
 * @cnc: The EWS Connection
 * @pri: The priority associated with the request
 * @fid: The folder id to which the items belong
 * @default_props: Can take one of the values: IdOnly,Default or AllProperties
 * @add_props: Specify any additional properties to be fetched
 * @sort_order: Specific sorting order for items
 * @query: evo query based on which items will be fetched
 * @type: type of folder
 * @convert_query_cb: a callback method to convert query to ews restiction
 * @cancellable: a GCancellable to monitor cancelled operations
 * @callback: Responses are parsed and returned to this callback
 * @user_data: user data passed to callback
 **/
void
e_ews_connection_find_folder_items (EEwsConnection *cnc,
                                    gint pri,
                                    EwsFolderId *fid,
                                    const gchar *default_props,
                                    EwsAdditionalProps *add_props,
                                    EwsSortOrder *sort_order,
                                    const gchar *query,
                                    EEwsFolderType type,
                                    EwsConvertQueryCallback convert_query_cb,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_if_fail (cnc != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"FindItem",
			"Traversal",
			"Shallow",
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);
	e_soap_message_start_element (msg, "ItemShape", "messages", NULL);
	e_ews_message_write_string_parameter (msg, "BaseShape", NULL, default_props);

	ews_append_additional_props_to_msg (msg, add_props);

	e_soap_message_end_element (msg);

	/*write restriction message based on query*/
	if (convert_query_cb)
		convert_query_cb (msg, query, type);

	if (sort_order)
		ews_write_sort_order_to_msg (msg, sort_order);

	e_soap_message_start_element (msg, "ParentFolderIds", "messages", NULL);

	if (fid->is_distinguished_id)
		e_ews_message_write_string_parameter_with_attribute (msg, "DistinguishedFolderId", NULL, NULL, "Id", fid->id);
	else
		e_ews_message_write_string_parameter_with_attribute (msg, "FolderId", NULL, NULL, "Id", fid->id);

	e_soap_message_end_element (msg);

	/* Complete the footer and print the request */
	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_find_folder_items);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, find_folder_items_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_find_folder_items_finish (EEwsConnection *cnc,
                                           GAsyncResult *result,
                                           gboolean *includes_last_item,
                                           GSList **items,
                                           GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_find_folder_items),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	*includes_last_item = async_data->includes_last_item;
	*items = async_data->items;

	return TRUE;
}

gboolean
e_ews_connection_find_folder_items_sync (EEwsConnection *cnc,
                                         gint pri,
                                         EwsFolderId *fid,
                                         const gchar *default_props,
                                         EwsAdditionalProps *add_props,
                                         EwsSortOrder *sort_order,
                                         const gchar *query,
                                         EEwsFolderType type,
                                         gboolean *includes_last_item,
                                         GSList **items,
                                         EwsConvertQueryCallback convert_query_cb,
                                         GCancellable *cancellable,
                                         GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (cnc != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_find_folder_items (
		cnc, pri, fid, default_props,
		add_props, sort_order, query,
		type, convert_query_cb, NULL,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_find_folder_items_finish (
		cnc, result, includes_last_item, items, error);

	e_async_closure_free (closure);

	return success;
}

void
e_ews_connection_sync_folder_hierarchy (EEwsConnection *cnc,
                                        gint pri,
                                        const gchar *sync_state,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_if_fail (cnc != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"SyncFolderHierarchy",
			NULL,
			NULL,
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);
	e_soap_message_start_element (msg, "FolderShape", "messages", NULL);
	e_ews_message_write_string_parameter (msg, "BaseShape", NULL, "AllProperties");
	e_soap_message_end_element (msg);

	if (sync_state)
		e_ews_message_write_string_parameter (msg, "SyncState", "messages", sync_state);

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_sync_folder_hierarchy);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, sync_hierarchy_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_sync_folder_hierarchy_finish (EEwsConnection *cnc,
                                               GAsyncResult *result,
                                               gchar **sync_state,
                                               gboolean *includes_last_folder,
                                               GSList **folders_created,
                                               GSList **folders_updated,
                                               GSList **folders_deleted,
                                               GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_sync_folder_hierarchy),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	*sync_state = async_data->sync_state;
	*includes_last_folder = async_data->includes_last_item;
	*folders_created = async_data->items_created;
	*folders_updated = async_data->items_updated;
	*folders_deleted = async_data->items_deleted;

	return TRUE;
}

gboolean
e_ews_connection_sync_folder_hierarchy_sync (EEwsConnection *cnc,
                                             gint pri,
					     const gchar *old_sync_state,
                                             gchar **new_sync_state,
                                             gboolean *includes_last_folder,
                                             GSList **folders_created,
                                             GSList **folders_updated,
                                             GSList **folders_deleted,
                                             GCancellable *cancellable,
                                             GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (cnc != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_sync_folder_hierarchy (
		cnc, pri, old_sync_state, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_sync_folder_hierarchy_finish (
		cnc, result, new_sync_state,
		includes_last_folder,
		folders_created,
		folders_updated,
		folders_deleted,
		error);

	e_async_closure_free (closure);

	return success;
}

EEwsServerVersion
e_ews_connection_get_server_version (EEwsConnection *cnc)
{
	g_return_val_if_fail (cnc != NULL, E_EWS_EXCHANGE_UNKNOWN);
	g_return_val_if_fail (cnc->priv != NULL, E_EWS_EXCHANGE_UNKNOWN);

	return cnc->priv->version;
}

gboolean
e_ews_connection_satisfies_server_version (EEwsConnection *cnc,
					  EEwsServerVersion version)
{
	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (cnc->priv != NULL, FALSE);

	/*
	 * This test always will fail if, for some reason, we were not able to get the server version.
	 * It occurrs intentionally because we don't want to call any function that expects an EWS
	 * Server version higher than 2007 SP1 without be sure we using an EWS Server with version
	 * 2007 SP1 or later.
	 */
	return cnc->priv->version >= version;
}

void
e_ews_connection_get_items (EEwsConnection *cnc,
                            gint pri,
                            const GSList *ids,
                            const gchar *default_props,
                            const gchar *additional_props,
                            gboolean include_mime,
                            const gchar *mime_directory,
			    EEwsBodyType body_type,
                            ESoapProgressFn progress_fn,
                            gpointer progress_data,
                            GCancellable *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;
	const GSList *l;

	g_return_if_fail (cnc != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"GetItem",
			NULL,
			NULL,
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);

	if (progress_fn && progress_data)
		e_soap_message_set_progress_fn (msg, progress_fn, progress_data);

	e_soap_message_start_element (msg, "ItemShape", "messages", NULL);
	e_ews_message_write_string_parameter (msg, "BaseShape", NULL, default_props);

	if (include_mime)
		e_ews_message_write_string_parameter (msg, "IncludeMimeContent", NULL, "true");
	else
		e_ews_message_write_string_parameter (msg, "IncludeMimeContent", NULL, "false");
	if (mime_directory)
		e_soap_message_store_node_data (msg, "MimeContent", mime_directory, TRUE);

	switch (body_type) {
	case E_EWS_BODY_TYPE_BEST:
		e_ews_message_write_string_parameter (msg, "BodyType", NULL, "Best");
		break;
	case E_EWS_BODY_TYPE_HTML:
		e_ews_message_write_string_parameter (msg, "BodyType", NULL, "HTML");
		break;
	case E_EWS_BODY_TYPE_TEXT:
		e_ews_message_write_string_parameter (msg, "BodyType", NULL, "Text");
		break;
	case E_EWS_BODY_TYPE_ANY:
		break;
	}

	if (additional_props && *additional_props) {
		gchar **prop = g_strsplit (additional_props, " ", 0);
		gint i = 0;

		e_soap_message_start_element (msg, "AdditionalProperties", NULL, NULL);
		while (prop[i]) {
			/* XX FIXME: Come up with a better way of doing this */
			if (!g_ascii_strncasecmp (prop[i], "mapi:int:0x", 11)) {
				e_soap_message_start_element (msg, "ExtendedFieldURI", NULL, NULL);
				e_soap_message_add_attribute (msg, "PropertyTag", prop[i] + 9, NULL, NULL);
				e_soap_message_add_attribute (msg, "PropertyType", "Integer", NULL, NULL);
				e_soap_message_end_element (msg);
			} else {
				e_ews_message_write_string_parameter_with_attribute (msg, "FieldURI", NULL, NULL, "FieldURI", prop[i]);
			}
			i++;
		}
		g_strfreev (prop);
		e_soap_message_end_element (msg);
	}
	e_soap_message_end_element (msg);

	e_soap_message_start_element (msg, "ItemIds", "messages", NULL);

	for (l = ids; l != NULL; l = g_slist_next (l))
		e_ews_message_write_string_parameter_with_attribute (msg, "ItemId", NULL, NULL, "Id", l->data);

	e_soap_message_end_element (msg);

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_get_items);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, get_items_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_get_items_finish (EEwsConnection *cnc,
                                   GAsyncResult *result,
                                   GSList **items,
                                   GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_get_items),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	if (!async_data->items) {
		g_set_error_literal (error, EWS_CONNECTION_ERROR, EWS_CONNECTION_ERROR_ITEMNOTFOUND, _("No items found"));
		return FALSE;
	}

	*items = async_data->items;

	return TRUE;
}

gboolean
e_ews_connection_get_items_sync (EEwsConnection *cnc,
                                 gint pri,
                                 const GSList *ids,
                                 const gchar *default_props,
                                 const gchar *additional_props,
                                 gboolean include_mime,
                                 const gchar *mime_directory,
				 EEwsBodyType body_type,
                                 GSList **items,
                                 ESoapProgressFn progress_fn,
                                 gpointer progress_data,
                                 GCancellable *cancellable,
                                 GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (cnc != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_get_items (
		cnc, pri,ids, default_props,
		additional_props, include_mime,
		mime_directory, body_type, progress_fn,
		progress_data, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_get_items_finish (
		cnc, result, items, error);

	e_async_closure_free (closure);

	return success;
}

static const gchar *
ews_delete_type_to_str (EwsDeleteType delete_type)
{
	switch (delete_type) {
		case EWS_HARD_DELETE:
			return "HardDelete";
		case EWS_SOFT_DELETE:
			return "SoftDelete";
		case EWS_MOVE_TO_DELETED_ITEMS:
			return "MoveToDeletedItems";
	}
	return NULL;
}

static const gchar *
ews_send_cancels_to_str (EwsSendMeetingCancellationsType send_cancels)
{
	switch (send_cancels) {
		case EWS_SEND_TO_NONE:
			return "SendToNone";
		case EWS_SEND_ONLY_TO_ALL:
			return "SendOnlyToAll";
		case EWS_SEND_TO_ALL_AND_SAVE_COPY:
			return "SendToAllAndSaveCopy";
	}
	return NULL;
}

static const gchar *
ews_affected_tasks_to_str (EwsAffectedTaskOccurrencesType affected_tasks)
{
	switch (affected_tasks) {
		case EWS_ALL_OCCURRENCES:
			return "AllOccurrences";
		case EWS_SPECIFIED_OCCURRENCE_ONLY:
			return "SpecifiedOccurrenceOnly";
	}
	return NULL;
}

static void
delete_item_response_cb (ESoapResponse *response,
                         GSimpleAsyncResult *simple)
{
	ESoapParameter *param;
	ESoapParameter *subparam;
	GError *error = NULL;

	param = e_soap_response_get_first_parameter_by_name (
		response, "ResponseMessages", &error);

	/* Sanity check */
	g_return_if_fail (
		(param != NULL && error == NULL) ||
		(param == NULL && error != NULL));

	if (error != NULL) {
		g_simple_async_result_take_error (simple, error);
		return;
	}

	subparam = e_soap_parameter_get_first_child (param);

	while (subparam != NULL) {
		if (!ews_get_response_status (subparam, &error)) {
			g_simple_async_result_take_error (simple, error);
			return;
		}

		subparam = e_soap_parameter_get_next_child (subparam);
	}
}

void
e_ews_connection_delete_items (EEwsConnection *cnc,
                               gint pri,
                               GSList *ids,
                               EwsDeleteType delete_type,
                               EwsSendMeetingCancellationsType send_cancels,
                               EwsAffectedTaskOccurrencesType affected_tasks,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;
	GSList *l;

	g_return_if_fail (cnc != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"DeleteItem",
			"DeleteType",
			ews_delete_type_to_str (delete_type),
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);

	if (send_cancels)
		e_soap_message_add_attribute (
			msg, "SendMeetingCancellations",
			ews_send_cancels_to_str (send_cancels), NULL, NULL);

	if (affected_tasks)
		e_soap_message_add_attribute (
			msg, "AffectedTaskOccurrences",
			ews_affected_tasks_to_str (affected_tasks), NULL, NULL);

	e_soap_message_start_element (msg, "ItemIds", "messages", NULL);

	for (l = ids; l != NULL; l = g_slist_next (l))
		e_ews_message_write_string_parameter_with_attribute (msg, "ItemId", NULL, NULL, "Id", l->data);

	e_soap_message_end_element (msg);

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_delete_items);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, delete_item_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

void
e_ews_connection_delete_item (EEwsConnection *cnc,
                              gint pri,
                              EwsId *item_id,
                              guint index,
                              EwsDeleteType delete_type,
                              EwsSendMeetingCancellationsType send_cancels,
                              EwsAffectedTaskOccurrencesType affected_tasks,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;
	gchar buffer[32];

	g_return_if_fail (cnc != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"DeleteItem",
			"DeleteType",
			ews_delete_type_to_str (delete_type),
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);

	if (send_cancels)
		e_soap_message_add_attribute (
			msg, "SendMeetingCancellations",
			ews_send_cancels_to_str (send_cancels), NULL, NULL);

	if (affected_tasks)
		e_soap_message_add_attribute (
			msg, "AffectedTaskOccurrences",
			ews_affected_tasks_to_str (affected_tasks), NULL, NULL);

	e_soap_message_start_element (msg, "ItemIds", "messages", NULL);

	if (index) {
		e_soap_message_start_element (msg, "OccurrenceItemId", NULL, NULL);
		e_soap_message_add_attribute (msg, "RecurringMasterId", item_id->id, NULL, NULL);
		if (item_id->change_key)
			e_soap_message_add_attribute (msg, "ChangeKey", item_id->change_key, NULL, NULL);
		snprintf (buffer, 32, "%u", index);
		e_soap_message_add_attribute (msg, "InstanceIndex", buffer, NULL, NULL);
		e_soap_message_end_element (msg);
	} else {
		e_soap_message_start_element (msg, "ItemId", NULL, NULL);
		e_soap_message_add_attribute (msg, "Id", item_id->id, NULL, NULL);
		if (item_id->change_key)
			e_soap_message_add_attribute (msg, "ChangeKey", item_id->change_key, NULL, NULL);
		e_soap_message_end_element (msg);
	}

	e_soap_message_end_element (msg);

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_delete_items);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, delete_item_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_delete_items_finish (EEwsConnection *cnc,
                                      GAsyncResult *result,
                                      GError **error)
{
	GSimpleAsyncResult *simple;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_delete_items),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	return TRUE;
}

gboolean
e_ews_connection_delete_items_sync (EEwsConnection *cnc,
                                    gint pri,
                                    GSList *ids,
                                    EwsDeleteType delete_type,
                                    EwsSendMeetingCancellationsType send_cancels,
                                    EwsAffectedTaskOccurrencesType affected_tasks,
                                    GCancellable *cancellable,
                                    GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (cnc != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_delete_items (
		cnc, pri, ids, delete_type,
		send_cancels, affected_tasks, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_delete_items_finish (cnc, result, error);

	e_async_closure_free (closure);

	return success;
}

gboolean
e_ews_connection_delete_item_sync (EEwsConnection *cnc,
                                   gint pri,
                                   EwsId *id,
                                   guint index,
                                   EwsDeleteType delete_type,
                                   EwsSendMeetingCancellationsType send_cancels,
                                   EwsAffectedTaskOccurrencesType affected_tasks,
                                   GCancellable *cancellable,
                                   GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (cnc != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_delete_item (
		cnc, pri, id, index, delete_type,
		send_cancels, affected_tasks, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_delete_items_finish (cnc, result, error);

	e_async_closure_free (closure);

	return success;
}

static xmlXPathObjectPtr
xpath_eval (xmlXPathContextPtr ctx,
	    const gchar *format,
	    ...)
{
	xmlXPathObjectPtr result;
	va_list args;
	gchar *expr;

	if (ctx == NULL)
		return NULL;

	va_start (args, format);
	expr = g_strdup_vprintf (format, args);
	va_end (args);

	result = xmlXPathEvalExpression ((xmlChar *) expr, ctx);
	g_free (expr);

	if (result == NULL)
		return NULL;

	if (result->type == XPATH_NODESET && xmlXPathNodeSetIsEmpty (result->nodesetval)) {
		xmlXPathFreeObject (result);
		return NULL;
	}

	return result;
}

static gboolean
element_has_child (ESoapMessage *message,
		   const gchar *path)
{
	xmlDocPtr doc;
	xmlXPathContextPtr xpctx;
	xmlXPathObjectPtr result;
	xmlNodeSetPtr nodeset;
	xmlNodePtr node;
	gboolean ret = FALSE;

	doc = e_soap_message_get_xml_doc (message);
	xpctx = xmlXPathNewContext (doc);

	xmlXPathRegisterNs (
			xpctx,
			(xmlChar *) "s",
			(xmlChar *) "http://schemas.xmlsoap.org/soap/envelope/");

	xmlXPathRegisterNs (
			xpctx,
			(xmlChar *) "m",
			(xmlChar *) "http://schemas.microsoft.com/exchange/services/2006/messages");

	xmlXPathRegisterNs (
			xpctx,
			(xmlChar *) "t",
			(xmlChar *) "http://schemas.microsoft.com/exchange/services/2006/types");

	result = xpath_eval (xpctx, path);

	if (result == NULL)
		goto exit;

	if (!xmlXPathNodeSetGetLength (result->nodesetval))
		goto exit;

	nodeset = result->nodesetval;
	node = nodeset->nodeTab[0];
	if (!node->children)
		goto exit;

	ret = TRUE;

exit:
	xmlXPathFreeObject (result);
	xmlXPathFreeContext (xpctx);
	return ret;
}

void
e_ews_connection_update_items (EEwsConnection *cnc,
                               gint pri,
                               const gchar *conflict_res,
                               const gchar *msg_disposition,
                               const gchar *send_invites,
                               const gchar *folder_id,
                               EEwsRequestCreationCallback create_cb,
                               gpointer create_user_data,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_if_fail (cnc != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"UpdateItem",
			NULL,
			NULL,
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);

	if (conflict_res)
		e_soap_message_add_attribute (
			msg, "ConflictResolution",
			conflict_res, NULL, NULL);
	if (msg_disposition)
		e_soap_message_add_attribute (
			msg, "MessageDisposition",
			msg_disposition, NULL, NULL);
	if (send_invites)
		e_soap_message_add_attribute (
			msg, "SendMeetingInvitationsOrCancellations",
			send_invites, NULL, NULL);

	if (folder_id) {
		e_soap_message_start_element (msg, "SavedItemFolderId", "messages", NULL);
		e_ews_message_write_string_parameter_with_attribute (
			msg, "FolderId",
			NULL, NULL, "Id", folder_id);
		e_soap_message_end_element (msg);
	}

	e_soap_message_start_element (msg, "ItemChanges", "messages", NULL);

	create_cb (msg, create_user_data);

	e_soap_message_end_element (msg); /* ItemChanges */

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_update_items);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	/*
	 * We need to check for both namespaces, because, the message is being wrote without use the types
	 * namespace. Maybe it is wrong, but the server doesn't complain about that. But this is the reason
	 * for the first check. The second one, is related to "how it should be" accord with EWS specifications.
	 */
	if (!element_has_child (msg, "/s:Envelope/s:Body/m:UpdateItem/m:ItemChanges/ItemChange/Updates") &&
		!element_has_child (msg, "/s:Envelope/s:Body/m:UpdateItem/m:ItemChanges/t:ItemChange/t:Updates"))
		g_simple_async_result_complete_in_idle (simple);
	else
		e_ews_connection_queue_request (
			cnc, msg, get_items_response_cb,
			pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_update_items_finish (EEwsConnection *cnc,
                                      GAsyncResult *result,
                                      GSList **ids,
                                      GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_update_items),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	/* if there is only one item, then check whether it's an error */
	if (async_data->items && !async_data->items->next) {
		EEwsItem *item = async_data->items->data;

		if (item && e_ews_item_get_item_type (item) == E_EWS_ITEM_TYPE_ERROR) {
			if (error)
				*error = g_error_copy (e_ews_item_get_error (item));

			g_slist_free_full (async_data->items, g_object_unref);
			async_data->items = NULL;

			return FALSE;
		}
	}

	if (ids)
		*ids = async_data->items;
	else {
		while (async_data->items) {
			g_object_unref (async_data->items->data);
			async_data->items = g_slist_remove (
				async_data->items,
				async_data->items->data);
		}
	}
	return TRUE;
}

gboolean
e_ews_connection_update_items_sync (EEwsConnection *cnc,
                                    gint pri,
                                    const gchar *conflict_res,
                                    const gchar *msg_disposition,
                                    const gchar *send_invites,
                                    const gchar *folder_id,
                                    EEwsRequestCreationCallback create_cb,
                                    gpointer create_user_data,
                                    GSList **ids,
                                    GCancellable *cancellable,
                                    GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (cnc != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_update_items (
		cnc, pri, conflict_res,
		msg_disposition, send_invites,
		folder_id, create_cb,
		create_user_data, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_update_items_finish (
		cnc, result, ids, error);

	e_async_closure_free (closure);

	return success;
}

void
e_ews_connection_create_items (EEwsConnection *cnc,
                               gint pri,
                               const gchar *msg_disposition,
                               const gchar *send_invites,
                               const EwsFolderId *fid,
                               EEwsRequestCreationCallback create_cb,
                               gpointer create_user_data,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_if_fail (cnc != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"CreateItem",
			NULL,
			NULL,
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);

	if (msg_disposition)
		e_soap_message_add_attribute (
			msg, "MessageDisposition",
			msg_disposition, NULL, NULL);
	if (send_invites)
		e_soap_message_add_attribute (
			msg, "SendMeetingInvitations",
			send_invites, NULL, NULL);

	if (fid) {
		e_soap_message_start_element (msg, "SavedItemFolderId", "messages", NULL);
		ews_append_folder_id_to_msg (msg, cnc->priv->email, fid);
		e_soap_message_end_element (msg);
	}

	e_soap_message_start_element (msg, "Items", "messages", NULL);

	create_cb (msg, create_user_data);

	e_soap_message_end_element (msg); /* Items */

	e_ews_message_write_footer (msg); /* CreateItem */

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_create_items);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, get_items_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_create_items_finish (EEwsConnection *cnc,
                                      GAsyncResult *result,
                                      GSList **ids,
                                      GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_create_items),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	/* if there is only one item, then check whether it's an error */
	if (async_data->items && !async_data->items->next) {
		EEwsItem *item = async_data->items->data;

		if (item && e_ews_item_get_item_type (item) == E_EWS_ITEM_TYPE_ERROR) {
			if (error)
				*error = g_error_copy (e_ews_item_get_error (item));

			g_slist_free_full (async_data->items, g_object_unref);
			async_data->items = NULL;

			return FALSE;
		}
	}

	*ids = async_data->items;

	return TRUE;
}

gboolean
e_ews_connection_create_items_sync (EEwsConnection *cnc,
                                    gint pri,
                                    const gchar *msg_disposition,
                                    const gchar *send_invites,
                                    const EwsFolderId *fid,
                                    EEwsRequestCreationCallback create_cb,
                                    gpointer create_user_data,
                                    GSList **ids,
                                    GCancellable *cancellable,
                                    GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (cnc != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_create_items (
		cnc, pri, msg_disposition,
		send_invites, fid,
		create_cb, create_user_data,
		cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_create_items_finish (
		cnc, result, ids, error);

	e_async_closure_free (closure);

	return success;
}

static const gchar *
get_search_scope_str (EwsContactsSearchScope scope)
{
	switch (scope) {
		case EWS_SEARCH_AD:
			return "ActiveDirectory";
		case EWS_SEARCH_AD_CONTACTS:
			return "ActiveDirectoryContacts";
		case EWS_SEARCH_CONTACTS:
			return "Contacts";
		case EWS_SEARCH_CONTACTS_AD:
			return "ContactsActiveDirectory";
		default:
			g_assert_not_reached ();
			return NULL;

	}
}

void
e_ews_connection_resolve_names (EEwsConnection *cnc,
                                gint pri,
                                const gchar *resolve_name,
                                EwsContactsSearchScope scope,
                                GSList *parent_folder_ids,
                                gboolean fetch_contact_data,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_if_fail (cnc != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"ResolveNames",
			NULL,
			NULL,
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);

	e_soap_message_add_attribute (msg, "SearchScope", get_search_scope_str (scope), NULL, NULL);

	if (fetch_contact_data)
		e_soap_message_add_attribute (msg, "ReturnFullContactData", "true", NULL, NULL);
	else
		e_soap_message_add_attribute (msg, "ReturnFullContactData", "false", NULL, NULL);

	if (parent_folder_ids) {
		e_soap_message_start_element (msg, "ParentFolderIds", "messages", NULL);
		ews_append_folder_ids_to_msg (msg, cnc->priv->email, parent_folder_ids);
		e_soap_message_end_element (msg);
	}

	e_ews_message_write_string_parameter (msg, "UnresolvedEntry", "messages", resolve_name);

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_resolve_names);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, resolve_names_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_resolve_names_finish (EEwsConnection *cnc,
                                       GAsyncResult *result,
                                       GSList **mailboxes,
                                       GSList **contact_items,
                                       gboolean *includes_last_item,
                                       GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_resolve_names),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	*includes_last_item = async_data->includes_last_item;

	if (contact_items)
		*contact_items = async_data->items_created;
	else
		g_slist_free_full (async_data->items_created, e_ews_free_resolve_contact);
	*mailboxes = async_data->items;

	return TRUE;
}

gboolean
e_ews_connection_resolve_names_sync (EEwsConnection *cnc,
                                     gint pri,
                                     const gchar *resolve_name,
                                     EwsContactsSearchScope scope,
                                     GSList *parent_folder_ids,
                                     gboolean fetch_contact_data,
                                     GSList **mailboxes,
                                     GSList **contact_items,
                                     gboolean *includes_last_item,
                                     GCancellable *cancellable,
                                     GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (cnc != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_resolve_names (
		cnc, pri, resolve_name,
		scope, parent_folder_ids,
		fetch_contact_data,
		cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_resolve_names_finish (
		cnc, result,
		mailboxes, contact_items,
		includes_last_item, error);

	e_async_closure_free (closure);

	return success;
}

static void
ews_connection_resolve_by_name (EEwsConnection *cnc,
                                gint pri,
                                const gchar *usename,
                                gboolean is_user_name,
                                gchar **smtp_address,
                                GCancellable *cancellable)
{
	GSList *mailboxes = NULL;
	GSList *contacts = NULL;
	gboolean includes_last_item = FALSE;
	GSList *miter;
	gint len;

	g_return_if_fail (cnc != NULL);
	g_return_if_fail (usename != NULL);
	g_return_if_fail (smtp_address != NULL);

	if (!*usename)
		return;

	len = strlen (usename);
	mailboxes = NULL;
	contacts = NULL;

	/* use the first error, not the guess-part error */
	e_ews_connection_resolve_names_sync (
		cnc, pri, usename,
		EWS_SEARCH_AD_CONTACTS, NULL, TRUE, &mailboxes, &contacts,
		&includes_last_item, cancellable, NULL);

	for (miter = mailboxes; miter; miter = miter->next) {
		const EwsMailbox *mailbox = miter->data;
		if (mailbox->email && *mailbox->email && g_strcmp0 (mailbox->routing_type, "EX") != 0
		    && ((!is_user_name && g_str_has_prefix (mailbox->email, usename) && mailbox->email[len] == '@') ||
		    (is_user_name && g_str_equal (usename, mailbox->name)))) {
			*smtp_address = g_strdup (mailbox->email);
			break;
		} else if (contacts && !contacts->next && contacts->data) {
			const EwsResolveContact *resolved = contacts->data;
			GList *emails = g_hash_table_get_values (resolved->email_addresses), *iter;
			gboolean found = FALSE;

			for (iter = emails; iter && !found; iter = iter->next) {
				const gchar *it_email = iter->data;

				if (it_email && g_str_has_prefix (it_email, "SMTP:")
				    && ((!is_user_name && g_str_has_prefix (it_email, usename) && it_email[len] == '@') ||
				    (is_user_name && g_str_equal (usename, resolved->display_name)))) {
					found = TRUE;
					break;
				}
			}

			g_list_free (emails);

			if (found) {
				gint ii;

				for (ii = 0; ii < g_hash_table_size (resolved->email_addresses); ii++) {
					gchar *key, *value;

					key = g_strdup_printf ("EmailAddress%d", ii + 1);
					value = g_hash_table_lookup (resolved->email_addresses, key);
					g_free (key);

					if (value && g_str_has_prefix (value, "SMTP:")) {
						/* pick the first available SMTP address */
						*smtp_address = g_strdup (value + 5);
						break;
					}
				}
				break;
			}
		}
	}

	g_slist_free_full (mailboxes, (GDestroyNotify) e_ews_mailbox_free);
	g_slist_free_full (contacts, (GDestroyNotify) e_ews_free_resolve_contact);
}

gboolean
e_ews_connection_ex_to_smtp_sync (EEwsConnection *cnc,
                                  gint pri,
                                  const gchar *name,
                                  const gchar *ex_address,
                                  gchar **smtp_address,
                                  GCancellable *cancellable,
                                  GError **error)
{
	GSList *mailboxes = NULL;
	GSList *contacts = NULL;
	gboolean includes_last_item = FALSE;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (ex_address != NULL, FALSE);
	g_return_val_if_fail (smtp_address != NULL, FALSE);

	*smtp_address = NULL;

	e_ews_connection_resolve_names_sync (
		cnc, pri, ex_address,
		EWS_SEARCH_AD_CONTACTS, NULL, TRUE, &mailboxes, &contacts,
		&includes_last_item, cancellable, error);

	/* only one mailbox matches */
	if (mailboxes && !mailboxes->next && mailboxes->data) {
		const EwsMailbox *mailbox = mailboxes->data;
		if (mailbox->email && *mailbox->email && g_strcmp0 (mailbox->routing_type, "EX") != 0) {
			*smtp_address = g_strdup (mailbox->email);
		} else if (contacts && !contacts->next && contacts->data) {
			const EwsResolveContact *resolved = contacts->data;
			gint ii;

			for (ii = 0; ii < g_hash_table_size (resolved->email_addresses); ii++) {
				gchar *key, *value;

				key = g_strdup_printf ("EmailAddress%d", ii + 1);
				value = g_hash_table_lookup (resolved->email_addresses, key);
				g_free (key);

				if (value && g_str_has_prefix (value, "SMTP:")) {
					/* pick the first available SMTP address */
					*smtp_address = g_strdup (value + 5);
					break;
				}
			}
		}
	}

	g_slist_free_full (mailboxes, (GDestroyNotify) e_ews_mailbox_free);
	g_slist_free_full (contacts, (GDestroyNotify) e_ews_free_resolve_contact);

	if (!*smtp_address) {
		const gchar *usename;

		usename = strrchr (ex_address, '/');
		if (usename && g_ascii_strncasecmp (usename, "/cn=", 4) == 0) {
			usename += 4;

			/* try to guess from common name of the EX address */
			ews_connection_resolve_by_name (cnc, pri, usename, FALSE, smtp_address, cancellable);
		}

		if (!*smtp_address && name && *name) {
			/* try to guess from mailbox name */
			ews_connection_resolve_by_name (cnc, pri, name, TRUE, smtp_address, cancellable);
		}
	}

	if (*smtp_address)
		g_clear_error (error);

	return *smtp_address != NULL;
}

void
e_ews_connection_expand_dl (EEwsConnection *cnc,
                            gint pri,
                            const EwsMailbox *mb,
                            GCancellable *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_if_fail (cnc != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"ExpandDL",
			NULL,
			NULL,
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);

	e_soap_message_start_element (msg, "Mailbox", "messages", NULL);

	if (mb->item_id) {
		e_soap_message_start_element (msg, "ItemId", NULL, NULL);

		e_soap_message_add_attribute (msg, "Id", mb->item_id->id, NULL, NULL);
		e_soap_message_add_attribute (msg, "ChangeKey", mb->item_id->change_key, NULL, NULL);

		e_soap_message_end_element (msg); /* Mailbox */

	} else if (mb->email)
		e_ews_message_write_string_parameter (msg, "EmailAddress", NULL, mb->email);

	e_soap_message_end_element (msg); /* Mailbox */

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_expand_dl);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, expand_dl_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

/* includes_last_item does not make sense as expand_dl does not support recursive 
 * fetch, wierd */
gboolean
e_ews_connection_expand_dl_finish (EEwsConnection *cnc,
                                   GAsyncResult *result,
                                   GSList **mailboxes,
                                   gboolean *includes_last_item,
                                   GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_expand_dl),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	*includes_last_item = async_data->includes_last_item;
	*mailboxes = async_data->items;

	return TRUE;

}

gboolean
e_ews_connection_expand_dl_sync (EEwsConnection *cnc,
                                 gint pri,
                                 const EwsMailbox *mb,
                                 GSList **mailboxes,
                                 gboolean *includes_last_item,
                                 GCancellable *cancellable,
                                 GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (cnc != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_expand_dl (
		cnc, pri, mb, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_expand_dl_finish (
		cnc, result, mailboxes, includes_last_item, error);

	e_async_closure_free (closure);

	return success;
}

static void
update_folder_response_cb (ESoapResponse *response,
                           GSimpleAsyncResult *simple)
{
	ESoapParameter *param;
	ESoapParameter *subparam;
	GError *error = NULL;

	param = e_soap_response_get_first_parameter_by_name (
		response, "ResponseMessages", &error);

	/* Sanity check */
	g_return_if_fail (
		(param != NULL && error == NULL) ||
		(param == NULL && error != NULL));

	if (error != NULL) {
		g_simple_async_result_take_error (simple, error);
		return;
	}

	subparam = e_soap_parameter_get_first_child (param);

	while (subparam != NULL) {
		if (!ews_get_response_status (subparam, &error)) {
			g_simple_async_result_take_error (simple, error);
			return;
		}

		subparam = e_soap_parameter_get_next_child (param);
	}
}

void
e_ews_connection_update_folder (EEwsConnection *cnc,
                                gint pri,
                                EEwsRequestCreationCallback create_cb,
                                gpointer create_user_data,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_if_fail (cnc != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"UpdateFolder",
			NULL,
			NULL,
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);

	e_soap_message_start_element (msg, "FolderChanges", "messages", NULL);

	create_cb (msg, create_user_data);

	e_soap_message_end_element (msg); /* FolderChanges */

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_update_folder);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, update_folder_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_update_folder_finish (EEwsConnection *cnc,
                                       GAsyncResult *result,
                                       GError **error)
{
	GSimpleAsyncResult *simple;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_update_folder),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	return TRUE;
}

gboolean
e_ews_connection_update_folder_sync (EEwsConnection *cnc,
                                     gint pri,
                                     EEwsRequestCreationCallback create_cb,
                                     gpointer create_user_data,
                                     GCancellable *cancellable,
                                     GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (cnc != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_update_folder (
		cnc, pri, create_cb, create_user_data, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_update_folder_finish (cnc, result, error);

	e_async_closure_free (closure);

	return success;
}

static void
move_folder_response_cb (ESoapResponse *response,
                         GSimpleAsyncResult *simple)
{
	ESoapParameter *param;
	ESoapParameter *subparam;
	GError *error = NULL;

	param = e_soap_response_get_first_parameter_by_name (
		response, "ResponseMessages", &error);

	/* Sanity check */
	g_return_if_fail (
		(param != NULL && error == NULL) ||
		(param == NULL && error != NULL));

	if (error != NULL) {
		g_simple_async_result_take_error (simple, error);
		return;
	}

	subparam = e_soap_parameter_get_first_child (param);

	while (subparam != NULL) {
		if (!ews_get_response_status (subparam, &error)) {
			g_simple_async_result_take_error (simple, error);
			return;
		}

		subparam = e_soap_parameter_get_next_child (subparam);
	}
}

void
e_ews_connection_move_folder (EEwsConnection *cnc,
                              gint pri,
                              const gchar *to_folder,
                              const gchar *folder,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_if_fail (cnc != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"MoveFolder",
			NULL,
			NULL,
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);

	e_soap_message_start_element (msg, "ToFolderId", "messages", NULL);
	if (to_folder)
		e_ews_message_write_string_parameter_with_attribute (
			msg, "FolderId", NULL,
			NULL, "Id", to_folder);
	else
		e_ews_message_write_string_parameter_with_attribute (
			msg, "DistinguishedFolderId", NULL,
			NULL, "Id", "msgfolderroot");

	e_soap_message_end_element (msg);

	e_soap_message_start_element (msg, "FolderIds", "messages", NULL);
	e_ews_message_write_string_parameter_with_attribute (
		msg, "FolderId", NULL,
		NULL, "Id", folder);
	e_soap_message_end_element (msg);

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_move_folder);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, move_folder_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_move_folder_finish (EEwsConnection *cnc,
                                     GAsyncResult *result,
                                     GError **error)
{
	GSimpleAsyncResult *simple;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_move_folder),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	return TRUE;
}

gboolean
e_ews_connection_move_folder_sync (EEwsConnection *cnc,
                                   gint pri,
                                   const gchar *to_folder,
                                   const gchar *folder,
                                   GCancellable *cancellable,
                                   GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (cnc != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_move_folder (
		cnc, pri, to_folder, folder, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_move_folder_finish (cnc, result, error);

	e_async_closure_free (closure);

	return success;
}

void
e_ews_connection_get_folder (EEwsConnection *cnc,
                             gint pri,
                             const gchar *folder_shape,
                             EwsAdditionalProps *add_props,
                             GSList *folder_ids,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_if_fail (cnc != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"GetFolder",
			NULL,
			NULL,
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			TRUE);

	e_soap_message_start_element (msg, "FolderShape", "messages", NULL);
	e_ews_message_write_string_parameter (msg, "BaseShape", NULL, folder_shape);

	ews_append_additional_props_to_msg (msg, add_props);
	e_soap_message_end_element (msg);

	if (folder_ids) {
		e_soap_message_start_element (msg, "FolderIds", "messages", NULL);
		ews_append_folder_ids_to_msg (msg, cnc->priv->email, folder_ids);
		e_soap_message_end_element (msg);
	}

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_get_folder);

	async_data = g_new0 (EwsAsyncData, 1);
	async_data->cnc = cnc;
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, get_folder_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_get_folder_finish (EEwsConnection *cnc,
                                    GAsyncResult *result,
                                    GSList **folders,
                                    GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_get_folder),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	*folders = async_data->items;

	return TRUE;
}

gboolean
e_ews_connection_get_folder_sync (EEwsConnection *cnc,
                                  gint pri,
                                  const gchar *folder_shape,
                                  EwsAdditionalProps *add_props,
                                  GSList *folder_ids,
                                  GSList **folders,
                                  GCancellable *cancellable,
                                  GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (cnc != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_get_folder (
		cnc, pri, folder_shape, add_props,
		folder_ids, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_get_folder_finish (
		cnc, result, folders, error);

	e_async_closure_free (closure);

	return success;
}

void
e_ews_connection_create_folder (EEwsConnection *cnc,
                                gint pri,
                                const gchar *parent_folder_id,
                                gboolean is_distinguished_id,
                                const gchar *folder_name,
                                EEwsFolderType folder_type,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;
	const gchar *folder_element;

	g_return_if_fail (cnc != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"CreateFolder",
			NULL,
			NULL,
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);

	e_soap_message_start_element (msg, "ParentFolderId", "messages", NULL);

	/* If NULL passed for parent_folder_id, use "msgfolderroot" */
	if (is_distinguished_id || !parent_folder_id) {
		e_soap_message_start_element (msg, "DistinguishedFolderId", NULL, NULL);
		e_soap_message_add_attribute (
				msg, "Id", parent_folder_id ?: "msgfolderroot", NULL, NULL);
		if (is_distinguished_id && cnc->priv->email) {
			e_soap_message_start_element (msg, "Mailbox", NULL, NULL);
			e_ews_message_write_string_parameter(
					msg, "EmailAddress", NULL, cnc->priv->email);
			e_soap_message_end_element (msg);
		}
		e_soap_message_end_element (msg);
	} else {
		e_ews_message_write_string_parameter_with_attribute (msg, "FolderId", NULL, NULL, "Id", parent_folder_id);
	}

	e_soap_message_end_element (msg);

	switch (folder_type) {
		case E_EWS_FOLDER_TYPE_MAILBOX:
			folder_element = "Folder";
			break;
		case E_EWS_FOLDER_TYPE_CALENDAR:
			folder_element = "CalendarFolder";
			break;
		case E_EWS_FOLDER_TYPE_CONTACTS:
			folder_element = "ContactsFolder";
			break;
		case E_EWS_FOLDER_TYPE_SEARCH:
			folder_element = "SearchFolder";
			break;
		case E_EWS_FOLDER_TYPE_TASKS:
			folder_element = "TasksFolder";
			break;
		default:
			g_warn_if_reached ();
			folder_element = "Folder";
			break;
	}

	e_soap_message_start_element (msg, "Folders", "messages", NULL);
	e_soap_message_start_element (msg, folder_element, NULL, NULL);
	e_ews_message_write_string_parameter (msg, "DisplayName", NULL, folder_name);

	e_soap_message_end_element (msg);
	e_soap_message_end_element (msg);

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_create_folder);

	async_data = g_new0 (EwsAsyncData, 1);
	async_data->folder_type = folder_type;

	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, create_folder_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_create_folder_finish (EEwsConnection *cnc,
                                       GAsyncResult *result,
                                       EwsFolderId **fid,
                                       GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_create_folder),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	*fid = (EwsFolderId *) async_data->items_created->data;
	g_slist_free (async_data->items_created);

	return TRUE;
}

gboolean
e_ews_connection_create_folder_sync (EEwsConnection *cnc,
                                     gint pri,
                                     const gchar *parent_folder_id,
                                     gboolean is_distinguished_id,
                                     const gchar *folder_name,
                                     EEwsFolderType folder_type,
                                     EwsFolderId **folder_id,
                                     GCancellable *cancellable,
                                     GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (cnc != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_create_folder (
		cnc, pri, parent_folder_id,
		is_distinguished_id, folder_name,
		folder_type, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_create_folder_finish (
		cnc, result, folder_id, error);

	e_async_closure_free (closure);

	return success;
}

void
e_ews_connection_move_items (EEwsConnection *cnc,
                             gint pri,
                             const gchar *folder_id,
                             gboolean docopy,
                             GSList *ids,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;
	GSList *l;

	g_return_if_fail (cnc != NULL);

	if (docopy)
		msg = e_ews_message_new_with_header (
				cnc->priv->uri,
				cnc->priv->impersonate_user,
				"CopyItem",
				NULL,
				NULL,
				cnc->priv->version,
				E_EWS_EXCHANGE_2007_SP1,
				FALSE);
	else
		msg = e_ews_message_new_with_header (
				cnc->priv->uri,
				cnc->priv->impersonate_user,
				"MoveItem",
				NULL,
				NULL,
				cnc->priv->version,
				E_EWS_EXCHANGE_2007_SP1,
				FALSE);

	e_soap_message_start_element (msg, "ToFolderId", "messages", NULL);
	e_soap_message_start_element (msg, "FolderId", NULL, NULL);
	e_soap_message_add_attribute (msg, "Id", folder_id, NULL, NULL);
	e_soap_message_end_element (msg); /* FolderId */
	e_soap_message_end_element (msg); /* ToFolderId */

	e_soap_message_start_element (msg, "ItemIds", "messages", NULL);
	for (l = ids; l != NULL; l = g_slist_next (l))
		e_ews_message_write_string_parameter_with_attribute (msg, "ItemId", NULL, NULL, "Id", l->data);
	e_soap_message_end_element (msg); /* ItemIds */

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_move_items);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, get_items_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_move_items_finish (EEwsConnection *cnc,
                                    GAsyncResult *result,
                                    GSList **items,
                                    GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_move_items),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	/* if there is only one item, then check whether it's an error */
	if (async_data->items && !async_data->items->next) {
		EEwsItem *item = async_data->items->data;

		if (item && e_ews_item_get_item_type (item) == E_EWS_ITEM_TYPE_ERROR) {
			if (error)
				*error = g_error_copy (e_ews_item_get_error (item));

			g_slist_free_full (async_data->items, g_object_unref);
			async_data->items = NULL;

			return FALSE;
		}
	}

	*items = async_data->items;

	return TRUE;
}

gboolean
e_ews_connection_move_items_sync (EEwsConnection *cnc,
                                  gint pri,
                                  const gchar *folder_id,
                                  gboolean docopy,
                                  GSList *ids,
                                  GSList **items,
                                  GCancellable *cancellable,
                                  GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (cnc != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_move_items (
		cnc, pri, folder_id, docopy, ids, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_move_items_finish (
		cnc, result, items, error);

	e_async_closure_free (closure);

	return success;
}

static void
delete_folder_response_cb (ESoapResponse *response,
                           GSimpleAsyncResult *simple)
{
	ESoapParameter *param;
	ESoapParameter *subparam;
	GError *error = NULL;

	param = e_soap_response_get_first_parameter_by_name (
		response, "ResponseMessages", &error);

	/* Sanity check */
	g_return_if_fail (
		(param != NULL && error == NULL) ||
		(param == NULL && error != NULL));

	if (error != NULL) {
		g_simple_async_result_take_error (simple, error);
		return;
	}

	subparam = e_soap_parameter_get_first_child (param);

	while (subparam != NULL) {
		if (!ews_get_response_status (subparam, &error)) {
			g_simple_async_result_take_error (simple, error);
			return;
		}

		subparam = e_soap_parameter_get_next_child (subparam);
	}
}

/**
 * e_ews_connection_delete_folder:
 * @cnc:
 * @pri:
 * @folder_id: folder to be deleted
 * @is_distinguished_id:
 * @delete_type: "HardDelete", "SoftDelete", "MoveToDeletedItems"
 * @cancellable:
 * @callback:
 * @user_data:
 **/
void
e_ews_connection_delete_folder (EEwsConnection *cnc,
                                gint pri,
                                const gchar *folder_id,
                                gboolean is_distinguished_id,
                                const gchar *delete_type,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_if_fail (cnc != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"DeleteFolder",
			"DeleteType",
			delete_type,
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);

	e_soap_message_start_element (msg, "FolderIds", "messages", NULL);

	e_soap_message_start_element (
			msg,
			is_distinguished_id ? "DistinguishedFolderId" : "FolderId",
			NULL,
			NULL);
	e_soap_message_add_attribute (msg, "Id", folder_id, NULL, NULL);

	/* This element is required for delegate access */
	if (is_distinguished_id && cnc->priv->email) {
		e_soap_message_start_element (msg, "Mailbox", NULL, NULL);
		e_ews_message_write_string_parameter(
				msg, "EmailAddress", NULL, cnc->priv->email);
		e_soap_message_end_element (msg);
	}

	e_soap_message_end_element (msg); /* </DistinguishedFolderId> || </FolderId> */

	e_soap_message_end_element (msg); /* </FolderIds> */

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_delete_folder);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, delete_folder_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_delete_folder_finish (EEwsConnection *cnc,
                                       GAsyncResult *result,
                                       GError **error)
{
	GSimpleAsyncResult *simple;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_delete_folder),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	return TRUE;
}

/**
 * e_ews_connection_delete_folder_sync:
 * @cnc:
 * @pri:
 * @folder_id: folder to be deleted
 * @is_distinguished_id:
 * @delete_type: "HardDelete", "SoftDelete", "MoveToDeletedItems"
 * @cancellable:
 * @error:
 **/
gboolean
e_ews_connection_delete_folder_sync (EEwsConnection *cnc,
                                     gint pri,
                                     const gchar *folder_id,
                                     gboolean is_distinguished_id,
                                     const gchar *delete_type,
                                     GCancellable *cancellable,
                                     GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (cnc != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_delete_folder (
		cnc, pri, folder_id,
		is_distinguished_id,
		delete_type,
		cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_delete_folder_finish (cnc, result, error);

	e_async_closure_free (closure);

	return success;
}

static void
ews_handle_create_attachments_param (ESoapParameter *param,
                                     EwsAsyncData *async_data)
{
	/* http://msdn.microsoft.com/en-us/library/aa565877%28v=EXCHG.80%29.aspx */
	ESoapParameter *subparam, *attspara, *last_relevant = NULL, *attparam;

	attspara = e_soap_parameter_get_first_child_by_name (param, "Attachments");

	for (subparam = e_soap_parameter_get_first_child (attspara); subparam != NULL; subparam = e_soap_parameter_get_next_child (subparam)) {
		if (!g_ascii_strcasecmp (e_soap_parameter_get_name (subparam), "FileAttachment")) {
			attparam = e_soap_parameter_get_first_child (subparam);
			last_relevant = attparam;

			async_data->items = g_slist_append (async_data->items, e_soap_parameter_get_property (attparam, "Id"));
		}
	}

	if (last_relevant != NULL) {
		async_data->sync_state = e_soap_parameter_get_property (last_relevant, "RootItemChangeKey");
	}
}

static void
create_attachments_response_cb (ESoapResponse *response,
                                GSimpleAsyncResult *simple)
{
	EwsAsyncData *async_data;
	ESoapParameter *param;
	ESoapParameter *subparam;
	GError *error = NULL;

	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	param = e_soap_response_get_first_parameter_by_name (
		response, "ResponseMessages", &error);

	/* Sanity check */
	g_return_if_fail (
		(param != NULL && error == NULL) ||
		(param == NULL && error != NULL));

	if (error != NULL) {
		g_simple_async_result_take_error (simple, error);
		return;
	}

	subparam = e_soap_parameter_get_first_child (param);

	while (subparam != NULL) {
		const gchar *name = (const gchar *) subparam->name;

		if (!ews_get_response_status (subparam, &error)) {
			g_simple_async_result_take_error (simple, error);
			return;
		}

		if (CHECK_ELEMENT (name, "CreateAttachmentResponseMessage"))
			ews_handle_create_attachments_param (subparam, async_data);

		subparam = e_soap_parameter_get_next_child (subparam);
	}
}

static gboolean
e_ews_connection_attach_file (ESoapMessage *msg,
                              EEwsAttachmentInfo *info,
			      gboolean contact_photo,
			      GError **error)
{
	EEwsAttachmentInfoType type = e_ews_attachment_info_get_type (info);
	gchar *filename = NULL, *buffer = NULL;
	const gchar *content = NULL;
	gsize length;

	switch (type) {
		case E_EWS_ATTACHMENT_INFO_TYPE_URI: {
			/* TODO - handle a situation where the file isnt accessible/other problem with it */
			/* TODO - This is a naive implementation that just uploads the whole content into */
			/*        memory, ie very inefficient */
			const gchar *uri;
			gchar *filepath;
			GError *local_error = NULL;

			uri = e_ews_attachment_info_get_uri (info);

			/* convert uri to actual file path */
			filepath = g_filename_from_uri (uri, NULL, &local_error);
			if (local_error != NULL) {
				g_propagate_error (error, local_error);
				return FALSE;
			}

			g_file_get_contents (uri, &buffer, &length, &local_error);
			if (local_error != NULL) {
				g_free (filepath);
				g_propagate_error (error, local_error);
				return FALSE;
			}

			content = buffer;

			filename = strrchr (filepath, G_DIR_SEPARATOR);
			filename = filename ? g_strdup (++filename) : g_strdup (filepath);

			g_free (filepath);
			break;
		}
		case E_EWS_ATTACHMENT_INFO_TYPE_INLINED:
			content = e_ews_attachment_info_get_inlined_data (info, &length);
			filename = g_strdup (e_ews_attachment_info_get_filename (info));
			break;
		default:
			g_warning ("Unknown EwsAttachmentInfoType %d", type);
			return FALSE;
	}

	e_soap_message_start_element (msg, "FileAttachment", NULL, NULL);

	e_ews_message_write_string_parameter (msg, "Name", NULL, filename);
	if (contact_photo)
		e_ews_message_write_string_parameter (msg, "IsContactPhoto", NULL, "true");
	e_soap_message_start_element (msg, "Content", NULL, NULL);
	e_soap_message_write_base64 (msg, content, length);
	e_soap_message_end_element (msg); /* "Content" */
	e_soap_message_end_element (msg); /* "FileAttachment" */

	g_free (filename);
	g_free (buffer);

	return TRUE;
}

void
e_ews_connection_create_attachments (EEwsConnection *cnc,
                                     gint pri,
                                     const EwsId *parent,
                                     const GSList *files,
				     gboolean is_contact_photo,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;
	const GSList *l;
	GError *local_error = NULL;

	g_return_if_fail (cnc != NULL);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_create_attachments);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"CreateAttachment",
			NULL,
			NULL,
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);

	e_soap_message_start_element (msg, "ParentItemId", "messages", NULL);
	e_soap_message_add_attribute (msg, "Id", parent->id, NULL, NULL);
	if (parent->change_key)
		e_soap_message_add_attribute (msg, "ChangeKey", parent->change_key, NULL, NULL);
	e_soap_message_end_element (msg);

	/* start interation over all items to get the attachemnts */
	e_soap_message_start_element (msg, "Attachments", "messages", NULL);

	for (l = files; l != NULL; l = g_slist_next (l))
		if (!e_ews_connection_attach_file (msg, l->data, is_contact_photo, &local_error)) {
			if (local_error != NULL)
				g_simple_async_result_take_error (simple, local_error);
			g_simple_async_result_complete_in_idle (simple);
			g_object_unref (simple);

			return;
		}

	e_soap_message_end_element (msg); /* "Attachments" */

	e_ews_message_write_footer (msg);

	e_ews_connection_queue_request (
		cnc, msg, create_attachments_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_create_attachments_finish (EEwsConnection *cnc,
                                            gchar **change_key,
					    GSList **attachments_ids,
                                            GAsyncResult *result,
                                            GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_create_attachments),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	if (attachments_ids)
		*attachments_ids = async_data->items;
	else
		g_slist_free_full (async_data->items, g_free);

	if (change_key)
		*change_key = async_data->sync_state;
	else
		g_free (async_data->sync_state);

	return TRUE;
}

gboolean
e_ews_connection_create_attachments_sync (EEwsConnection *cnc,
                                          gint pri,
                                          const EwsId *parent,
                                          const GSList *files,
					  gboolean is_contact_photo,
                                          gchar **change_key,
					  GSList **attachments_ids,
                                          GCancellable *cancellable,
                                          GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean ret;

	g_return_val_if_fail (cnc != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_create_attachments (
		cnc, pri, parent, files, is_contact_photo, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	ret = e_ews_connection_create_attachments_finish (
		cnc, change_key, attachments_ids, result, error);

	e_async_closure_free (closure);

	return ret;
}

/* Delete attachemnts */
static void
ews_handle_root_item_id_param (ESoapParameter *subparam,
                               EwsAsyncData *async_data)
{
	/* http://msdn.microsoft.com/en-us/library/aa580782%28v=EXCHG.80%29.aspx */
	ESoapParameter *attspara;

	attspara = e_soap_parameter_get_first_child_by_name (
		subparam, "RootItemId");

	if (attspara == NULL)
		return;

	async_data->items = g_slist_append (
		async_data->items,
		e_soap_parameter_get_property (attspara, "RootItemChangeKey"));
}

static void
delete_attachments_response_cb (ESoapResponse *response,
                                GSimpleAsyncResult *simple)
{
	EwsAsyncData *async_data;
	ESoapParameter *param;
	ESoapParameter *subparam;
	GError *error = NULL;

	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	param = e_soap_response_get_first_parameter_by_name (
		response, "ResponseMessages", &error);

	/* Sanity check */
	g_return_if_fail (
		(param != NULL && error == NULL) ||
		(param == NULL && error != NULL));

	if (error != NULL) {
		g_simple_async_result_take_error (simple, error);
		return;
	}

	subparam = e_soap_parameter_get_first_child (param);

	while (subparam != NULL) {
		const gchar *name = (const gchar *) subparam->name;

		if (!ews_get_response_status (subparam, &error)) {
			g_simple_async_result_take_error (simple, error);
			return;
		}

		if (CHECK_ELEMENT (name, "DeleteAttachmentResponseMessage"))
			ews_handle_root_item_id_param (subparam, async_data);

		subparam = e_soap_parameter_get_next_child (subparam);
	}
}

void
e_ews_connection_delete_attachments (EEwsConnection *cnc,
                                     gint pri,
                                     const GSList *attachments_ids,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;
	const GSList *l;

	g_return_if_fail (cnc != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"DeleteAttachment",
			NULL,
			NULL,
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);

	/* start interation over all items to get the attachemnts */
	e_soap_message_start_element (msg, "AttachmentIds", "messages", NULL);

	for (l = attachments_ids; l != NULL; l = l->next) {
		e_ews_message_write_string_parameter_with_attribute (msg, "AttachmentId", NULL, NULL, "Id", l->data);
	}

	e_soap_message_end_element (msg); /* "AttachmentIds" */

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_delete_attachments);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, delete_attachments_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_delete_attachments_finish (EEwsConnection *cnc,
                                            GAsyncResult *result,
					    GSList **parents_ids,
                                            GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_delete_attachments),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	if (parents_ids)
		*parents_ids = async_data->items;
	else
		g_slist_free_full (async_data->items, g_free);

	return TRUE;
}

gboolean
e_ews_connection_delete_attachments_sync (EEwsConnection *cnc,
                                          gint pri,
                                          const GSList *attachments_ids,
					  GSList **parents_ids,
                                          GCancellable *cancellable,
                                          GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean ret;

	g_return_val_if_fail (cnc != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_delete_attachments (
		cnc, pri, attachments_ids, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	ret = e_ews_connection_delete_attachments_finish (
		cnc, result, parents_ids, error);

	e_async_closure_free (closure);

	return ret;
}

static void
ews_handle_attachments_param (ESoapParameter *param,
                              EwsAsyncData *async_data)
{
	ESoapParameter *subparam, *attspara;
	EEwsAttachmentInfo *info = NULL;
	EEwsItem *item;
	const gchar *name;

	attspara = e_soap_parameter_get_first_child_by_name (param, "Attachments");

	for (subparam = e_soap_parameter_get_first_child (attspara); subparam != NULL; subparam = e_soap_parameter_get_next_child (subparam)) {
		name = e_soap_parameter_get_name (subparam);

		if (!g_ascii_strcasecmp (name, "ItemAttachment")) {
			item = e_ews_item_new_from_soap_parameter (subparam);
			info = e_ews_item_dump_mime_content (item, async_data->directory);

		} else if (!g_ascii_strcasecmp (name, "FileAttachment")) {
			info = e_ews_dump_file_attachment_from_soap_parameter (
					subparam,
					async_data->directory,
					async_data->sync_state);
		}

		if (info)
			async_data->items = g_slist_append (async_data->items, info);
		else
			e_ews_attachment_info_free (info);

		info = NULL;
	}
}

static void
get_attachments_response_cb (ESoapResponse *response,
                             GSimpleAsyncResult *simple)
{
	EwsAsyncData *async_data;
	ESoapParameter *param;
	ESoapParameter *subparam;
	GError *error = NULL;

	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	param = e_soap_response_get_first_parameter_by_name (
		response, "ResponseMessages", &error);

	/* Sanity check */
	g_return_if_fail (
		(param != NULL && error == NULL) ||
		(param == NULL && error != NULL));

	if (error != NULL) {
		g_simple_async_result_take_error (simple, error);
		return;
	}

	subparam = e_soap_parameter_get_first_child (param);

	while (subparam != NULL) {
		const gchar *name = (const gchar *) subparam->name;

		if (!ews_get_response_status (subparam, &error)) {
			g_simple_async_result_take_error (simple, error);
			return;
		}

		if (CHECK_ELEMENT (name, "GetAttachmentResponseMessage"))
			ews_handle_attachments_param (subparam, async_data);

		subparam = e_soap_parameter_get_next_child (subparam);
	}
}

void
e_ews_connection_get_attachments (EEwsConnection *cnc,
                                  gint pri,
                                  const gchar *uid,
                                  const GSList *ids,
                                  const gchar *cache,
                                  gboolean include_mime,
                                  ESoapProgressFn progress_fn,
                                  gpointer progress_data,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;
	const GSList *l;

	g_return_if_fail (cnc != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"GetAttachment",
			NULL,
			NULL,
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);

	/* not sure why I need it, need to check */
	if (progress_fn && progress_data)
		e_soap_message_set_progress_fn (msg, progress_fn, progress_data);

	if (cache)
		e_soap_message_store_node_data (msg, "MimeContent Content", cache, TRUE);

	/* wrtie empty attachments shape, need to discover maybe usefull in some cases*/
	e_soap_message_start_element (msg, "AttachmentShape", "messages", NULL);
	e_ews_message_write_string_parameter (msg, "IncludeMimeContent", NULL, "true");
	e_soap_message_end_element (msg);

	/* start interation over all items to get the attachemnts */
	e_soap_message_start_element (msg, "AttachmentIds", "messages", NULL);

	for (l = ids; l != NULL; l = g_slist_next (l))
		e_ews_message_write_string_parameter_with_attribute (msg, "AttachmentId", NULL, NULL, "Id", l->data);

	e_soap_message_end_element (msg);

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_get_attachments);

	async_data = g_new0 (EwsAsyncData, 1);
	async_data->directory = cache;
	async_data->sync_state = (gchar *) uid;
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, get_attachments_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_get_attachments_finish (EEwsConnection *cnc,
                                         GAsyncResult *result,
                                         GSList **items,
                                         GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_get_attachments),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	if (items)
		*items = async_data->items;
	else
		g_slist_free_full (async_data->items, (GDestroyNotify) e_ews_attachment_info_free);

	return TRUE;
}

gboolean
e_ews_connection_get_attachments_sync (EEwsConnection *cnc,
                                       gint pri,
                                       const gchar *uid,
                                       const GSList *ids,
                                       const gchar *cache,
                                       gboolean include_mime,
                                       GSList **items,
                                       ESoapProgressFn progress_fn,
                                       gpointer progress_data,
                                       GCancellable *cancellable,
                                       GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean ret;

	g_return_val_if_fail (cnc != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_get_attachments (
		cnc, pri, uid, ids, cache, include_mime,
		progress_fn, progress_data, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	ret = e_ews_connection_get_attachments_finish (
		cnc, result, items, error);

	e_async_closure_free (closure);

	return ret;
}

static void
ews_handle_free_busy_view (ESoapParameter *param,
                           EwsAsyncData *async_data)
{
       /*parse the response to create a free_busy data
	http://msdn.microsoft.com / en - us / library / aa564001 % 28v = EXCHG.140 % 29.aspx */
	icalcomponent *vfb;
	icalproperty *icalprop = NULL;
	struct icalperiodtype ipt;
	ESoapParameter *viewparam, *eventarray, *event_param, *subparam;
	GTimeVal t_val;
	const gchar *name;
	gchar *value, *new_val = NULL, *summary = NULL, *location = NULL;

	viewparam = e_soap_parameter_get_first_child_by_name (param, "FreeBusyView");
	if (!viewparam) return;
	vfb = icalcomponent_new_vfreebusy ();
	eventarray = e_soap_parameter_get_first_child_by_name (viewparam, "CalendarEventArray");
	for (event_param = e_soap_parameter_get_first_child (eventarray); event_param != NULL; event_param = e_soap_parameter_get_next_child (event_param), icalprop = NULL) {
		for (subparam = e_soap_parameter_get_first_child (event_param); subparam != NULL; subparam = e_soap_parameter_get_next_child (subparam)) {
			name = e_soap_parameter_get_name (subparam);

			if (!g_ascii_strcasecmp (name, "StartTime")) {
				value = e_soap_parameter_get_string_value (subparam);
				/*We are sending UTC timezone and expect server to return in same*/

				/*Remove leading and trailing whitespace*/
				g_strstrip (value);

				if (g_utf8_strlen (value, -1) == 19) {
					/*If server returns time without zone add Z to treat it in UTC*/
					new_val = g_strdup_printf ("%sZ", value);
					g_free (value);
				} else
					new_val = value;

				g_time_val_from_iso8601 (new_val, &t_val);
				g_free (new_val);

				ipt.start = icaltime_from_timet_with_zone (t_val.tv_sec, 0, NULL);

			} else if (!g_ascii_strcasecmp (name, "EndTime")) {
				value = e_soap_parameter_get_string_value (subparam);
				/*We are sending UTC timezone and expect server to return in same*/

				/*Remove leading and trailing whitespace*/
				g_strstrip (value);

				if (g_utf8_strlen (value, -1) == 19) {
					/*If server returns time without zone add Z to treat it in UTC*/
					new_val = g_strdup_printf ("%sZ", value);
					g_free (value);
				} else
					new_val = value;

				g_time_val_from_iso8601 (new_val, &t_val);
				g_free (new_val);

				ipt.end = icaltime_from_timet_with_zone (t_val.tv_sec, 0, NULL);

				icalprop = icalproperty_new_freebusy (ipt);
			} else if (!g_ascii_strcasecmp (name, "BusyType")) {
				value = e_soap_parameter_get_string_value (subparam);
				if (!strcmp (value, "Busy"))
					icalproperty_set_parameter_from_string (icalprop, "FBTYPE", "BUSY");
				else if (!strcmp (value, "Tentative"))
					icalproperty_set_parameter_from_string (icalprop, "FBTYPE", "BUSY-TENTATIVE");
				else if (!strcmp (value, "OOF"))
					icalproperty_set_parameter_from_string (icalprop, "FBTYPE", "BUSY-UNAVAILABLE");
				else if (!strcmp (value, "Free"))
					icalproperty_set_parameter_from_string (icalprop, "FBTYPE", "FREE");
				g_free (value);
			} else if (!g_ascii_strcasecmp (name, "CalendarEventDetails")) {
				ESoapParameter *dparam;

				dparam = e_soap_parameter_get_first_child_by_name (subparam, "Subject");
				if (dparam)
					summary = e_soap_parameter_get_string_value (dparam);

				dparam = e_soap_parameter_get_first_child_by_name (subparam, "Location");
				if (dparam)
					location = e_soap_parameter_get_string_value (dparam);
			}
		}
		if (icalprop != NULL) {
			if (summary)
				icalproperty_set_parameter_from_string (icalprop, "X-SUMMARY", summary);
			if (location)
				icalproperty_set_parameter_from_string (icalprop, "X-LOCATION", location);
			icalcomponent_add_property (vfb, icalprop);
		}

		g_free (summary);
		g_free (location);
		summary = NULL;
		location = NULL;
	}

	async_data->items = g_slist_append (async_data->items, vfb);
}

static void
get_free_busy_response_cb (ESoapResponse *response,
                           GSimpleAsyncResult *simple)
{
	EwsAsyncData *async_data;
	ESoapParameter *param;
	ESoapParameter *subparam;
	GError *error = NULL;

	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	param = e_soap_response_get_first_parameter_by_name (
		response, "FreeBusyResponseArray", &error);

	/* Sanity check */
	g_return_if_fail (
		(param != NULL && error == NULL) ||
		(param == NULL && error != NULL));

	if (error != NULL) {
		g_simple_async_result_take_error (simple, error);
		return;
	}

	subparam = e_soap_parameter_get_first_child (param);

	while (subparam != NULL) {
		ESoapParameter *subsubparam;

		subsubparam = e_soap_parameter_get_first_child_by_name (
			subparam, "ResponseMessage");

		if (subsubparam) {
			if (!ews_get_response_status (subsubparam, &error)) {
				g_simple_async_result_take_error (simple, error);
				return;
			}

			ews_handle_free_busy_view (subparam, async_data);
		}

		subparam = e_soap_parameter_get_next_child (subparam);
	}
}

void
e_ews_connection_get_free_busy (EEwsConnection *cnc,
                                gint pri,
                                EEwsRequestCreationCallback free_busy_cb,
                                gpointer free_busy_user_data,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_if_fail (cnc != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"GetUserAvailabilityRequest",
			NULL,
			NULL,
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);

	free_busy_cb (msg, free_busy_user_data);

	e_ews_message_write_footer (msg); /*GetUserAvailabilityRequest  */

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_get_free_busy);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, get_free_busy_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_get_free_busy_finish (EEwsConnection *cnc,
                                       GAsyncResult *result,
                                       GSList **free_busy,
                                       GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_get_free_busy),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;
	*free_busy = async_data->items;

	return TRUE;
}

gboolean
e_ews_connection_get_free_busy_sync (EEwsConnection *cnc,
                                     gint pri,
                                     EEwsRequestCreationCallback free_busy_cb,
                                     gpointer free_busy_user_data,
                                     GSList **free_busy,
                                     GCancellable *cancellable,
                                     GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (cnc != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_get_free_busy (
		cnc, pri, free_busy_cb,
		free_busy_user_data, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_get_free_busy_finish (
		cnc, result, free_busy, error);

	e_async_closure_free (closure);

	return success;
}

static EwsPermissionLevel
get_permission_from_string (const gchar *permission)
{
	g_return_val_if_fail (permission != NULL, EwsPermissionLevel_Unknown);

	if (!g_ascii_strcasecmp (permission, "Editor"))
		return EwsPermissionLevel_Editor;
	else if (!g_ascii_strcasecmp (permission, "Author"))
		return EwsPermissionLevel_Author;
	else if (!g_ascii_strcasecmp (permission, "Reviewer"))
		return EwsPermissionLevel_Reviewer;
	else if (!g_ascii_strcasecmp (permission, "Custom"))
		return EwsPermissionLevel_Custom;
	else
		return EwsPermissionLevel_None;

}

static void
ews_handle_delegate_user_param (ESoapParameter *param,
                                EwsAsyncData *async_data)
{
	ESoapParameter *subparam, *node, *child;
	EwsDelegateInfo *data;
	gchar *value;

	node = e_soap_parameter_get_first_child_by_name (param, "DelegateUser");
	if (!node)
		return;

	subparam = e_soap_parameter_get_first_child_by_name (node, "UserId");
	if (!subparam)
		return;

	data = g_new0 (EwsDelegateInfo, 1);
	data->user_id = g_new0 (EwsUserId, 1);

	/*Parse User Id*/

	child = e_soap_parameter_get_first_child_by_name (subparam, "SID");
	data->user_id->sid = e_soap_parameter_get_string_value (child);

	child = e_soap_parameter_get_first_child_by_name (subparam, "PrimarySmtpAddress");
	data->user_id->primary_smtp = e_soap_parameter_get_string_value (child);

	child = e_soap_parameter_get_first_child_by_name (subparam, "DisplayName");
	data->user_id->display_name = e_soap_parameter_get_string_value (child);

	subparam = e_soap_parameter_get_first_child_by_name (node, "DelegatePermissions");

	/*Parse Delegate Permissions*/
	child = e_soap_parameter_get_first_child_by_name (subparam, "CalendarFolderPermissionLevel");
	if (child) {
		value = e_soap_parameter_get_string_value (child);
		data->calendar = get_permission_from_string (value);
		g_free (value);
	}

	child = e_soap_parameter_get_first_child_by_name (subparam, "ContactsFolderPermissionLevel");
	if (child) {
		value = e_soap_parameter_get_string_value (child);
		data->contacts = get_permission_from_string (value);
		g_free (value);
	}

	child = e_soap_parameter_get_first_child_by_name (subparam, "InboxFolderPermissionLevel");
	if (child) {
		value = e_soap_parameter_get_string_value (child);
		data->inbox = get_permission_from_string (value);
		g_free (value);
	}

	child = e_soap_parameter_get_first_child_by_name (subparam, "TasksFolderPermissionLevel");
	if (child) {
		value = e_soap_parameter_get_string_value (child);
		data->tasks = get_permission_from_string (value);
		g_free (value);
	}

	child = e_soap_parameter_get_first_child_by_name (subparam, "NotesFolderPermissionLevel");
	if (child) {
		value = e_soap_parameter_get_string_value (child);
		data->notes = get_permission_from_string (value);
		g_free (value);
	}

	child = e_soap_parameter_get_first_child_by_name (subparam, "JournalFolderPermissionLevel");
	if (child) {
		value = e_soap_parameter_get_string_value (child);
		data->journal = get_permission_from_string (value);
		g_free (value);
	}

	subparam = e_soap_parameter_get_first_child_by_name (node, "ReceiveCopiesOfMeetingMessages");
	if (subparam) {
		value = e_soap_parameter_get_string_value (subparam);
		data->meetingcopies = g_strcmp0 (value, "true") == 0;
		g_free (value);
	}

	subparam = e_soap_parameter_get_first_child_by_name (node, "ViewPrivateItems");
	if (subparam) {
		value = e_soap_parameter_get_string_value (subparam);
		data->view_priv_items = g_strcmp0 (value, "true") == 0;
		g_free (value);
	}

	async_data->items = g_slist_append (async_data->items, data);
}

static void
get_delegate_response_cb (ESoapResponse *response,
                          GSimpleAsyncResult *simple)
{
	EwsAsyncData *async_data;
	ESoapParameter *param;
	ESoapParameter *subparam;
	gchar *value;
	GError *error = NULL;

	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (ews_get_response_status (e_soap_response_get_parameter (response), &error))
		param = e_soap_response_get_first_parameter_by_name (
			response, "DeliverMeetingRequests", &error);
	else
		param = NULL;

	/* Sanity check */
	g_return_if_fail (
		(param != NULL && error == NULL) ||
		(param == NULL && error != NULL));

	if (error != NULL) {
		g_simple_async_result_take_error (simple, error);
		return;
	}

	value = e_soap_parameter_get_string_value (param);
	if (g_strcmp0 (value, "DelegatesOnly") == 0)
		async_data->deliver_to = EwsDelegateDeliver_DelegatesOnly;
	else if (g_strcmp0 (value, "DelegatesAndMe") == 0)
		async_data->deliver_to = EwsDelegateDeliver_DelegatesAndMe;
	else if (g_strcmp0 (value, "DelegatesAndSendInformationToMe") == 0)
		async_data->deliver_to = EwsDelegateDeliver_DelegatesAndSendInformationToMe;
	else {
		g_message ("%s: Unknown deliver-to value '%s'", G_STRFUNC, value ? value : "[null]");
		async_data->deliver_to = EwsDelegateDeliver_DelegatesAndSendInformationToMe;
	}
	g_free (value);

	param = e_soap_response_get_first_parameter_by_name (
		response, "ResponseMessages", NULL);
	/* it's OK to not have set any delegate */
	if (!param)
		return;

	/* Sanity check */
	g_return_if_fail (
		(param != NULL && error == NULL) ||
		(param == NULL && error != NULL));

	if (error != NULL) {
		g_simple_async_result_take_error (simple, error);
		return;
	}

	subparam = e_soap_parameter_get_first_child (param);

	while (subparam != NULL) {
		const gchar *name = (const gchar *) subparam->name;

		if (!ews_get_response_status (subparam, &error)) {
			g_simple_async_result_take_error (simple, error);
			return;
		}

		if (CHECK_ELEMENT (name, "DelegateUserResponseMessageType"))
			ews_handle_delegate_user_param (subparam, async_data);

		subparam = e_soap_parameter_get_next_child (subparam);
	}
}

void
e_ews_connection_get_delegate (EEwsConnection *cnc,
                               gint pri,
                               const gchar *mail_id,
                               gboolean include_permissions,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_if_fail (cnc != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"GetDelegate",
			"IncludePermissions",
			include_permissions ? "true" : "false",
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);

	e_soap_message_start_element (msg, "Mailbox", "messages", NULL);

	e_ews_message_write_string_parameter (msg, "EmailAddress", NULL, mail_id ? mail_id : cnc->priv->email);

	e_soap_message_end_element (msg);

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_get_delegate);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, get_delegate_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_get_delegate_finish (EEwsConnection *cnc,
                                      GAsyncResult *result,
                                      EwsDelegateDeliver *deliver_to,
                                      GSList **delegates, /* EwsDelegateInfo * */
                                      GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (delegates != NULL, FALSE);
	g_return_val_if_fail (deliver_to != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_get_delegate),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	*deliver_to = async_data->deliver_to;
	*delegates = async_data->items;
	async_data->items = NULL;

	return TRUE;
}

gboolean
e_ews_connection_get_delegate_sync (EEwsConnection *cnc,
                                    gint pri,
                                    const gchar *mail_id,
                                    gboolean include_permissions,
                                    EwsDelegateDeliver *deliver_to,
                                    GSList **delegates, /* EwsDelegateInfo * */
                                    GCancellable *cancellable,
                                    GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (E_IS_EWS_CONNECTION (cnc), FALSE);
	g_return_val_if_fail (deliver_to != NULL, FALSE);
	g_return_val_if_fail (delegates != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_get_delegate (
		cnc, pri, mail_id,
		include_permissions, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_get_delegate_finish (
		cnc, result, deliver_to, delegates, error);

	e_async_closure_free (closure);

	return success;
}

static void
update_delegate_response_cb (ESoapResponse *response,
                             GSimpleAsyncResult *simple)
{
	ESoapParameter *param;
	ESoapParameter *subparam;
	GError *error = NULL;

	if (ews_get_response_status (e_soap_response_get_parameter (response), &error)) {
		param = e_soap_response_get_first_parameter_by_name (
			response, "ResponseMessages", NULL);
		/* that's OK to not receive any ResponseMessages here */
		if (!param)
			return;
	} else
		param = NULL;

	/* Sanity check */
	g_return_if_fail (
		(param != NULL && error == NULL) ||
		(param == NULL && error != NULL));

	if (error != NULL) {
		g_simple_async_result_take_error (simple, error);
		return;
	}

	subparam = e_soap_parameter_get_first_child (param);

	while (subparam != NULL) {
		if (!ews_get_response_status (subparam, &error)) {
			g_simple_async_result_take_error (simple, error);
			return;
		}

		subparam = e_soap_parameter_get_next_child (param);
	}
}

static void
set_delegate_permission (ESoapMessage *msg,
                         const gchar *elem_name,
                         EwsPermissionLevel perm_level)
{
	const gchar *level_name = NULL;

	if (perm_level == EwsPermissionLevel_None)
		level_name = "None";
	else if (perm_level == EwsPermissionLevel_Reviewer)
		level_name = "Reviewer";
	else if (perm_level == EwsPermissionLevel_Author)
		level_name = "Author";
	else if (perm_level == EwsPermissionLevel_Editor)
		level_name = "Editor";

	if (!level_name)
		return;

	e_ews_message_write_string_parameter (msg, elem_name, NULL, level_name);
}

void
e_ews_connection_add_delegate (EEwsConnection *cnc,
                               gint pri,
                               const gchar *mail_id,
                               const GSList *delegates, /* EwsDelegateInfo * */
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;
	const GSList *iter;

	g_return_if_fail (cnc != NULL);
	g_return_if_fail (delegates != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"AddDelegate",
			NULL,
			NULL,
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);

	e_soap_message_start_element (msg, "Mailbox", "messages", NULL);
	e_ews_message_write_string_parameter (msg, "EmailAddress", NULL, mail_id ? mail_id : cnc->priv->email);
	e_soap_message_end_element (msg);

	e_soap_message_start_element (msg, "DelegateUsers", "messages", NULL);
	for (iter = delegates; iter; iter = iter->next) {
		const EwsDelegateInfo *di = iter->data;

		if (!di)
			continue;

		e_soap_message_start_element (msg, "DelegateUser", NULL, NULL);

		e_soap_message_start_element (msg, "UserId", NULL, NULL);
		e_ews_message_write_string_parameter (msg, "PrimarySmtpAddress", NULL, di->user_id->primary_smtp);
		e_soap_message_end_element (msg); /* UserId */

		e_soap_message_start_element (msg, "DelegatePermissions", NULL, NULL);
		set_delegate_permission (msg, "CalendarFolderPermissionLevel", di->calendar);
		set_delegate_permission (msg, "TasksFolderPermissionLevel", di->tasks);
		set_delegate_permission (msg, "InboxFolderPermissionLevel", di->inbox);
		set_delegate_permission (msg, "ContactsFolderPermissionLevel", di->contacts);
		set_delegate_permission (msg, "NotesFolderPermissionLevel", di->notes);
		set_delegate_permission (msg, "JournalFolderPermissionLevel", di->journal);
		e_soap_message_end_element (msg); /* DelegatePermissions */

		e_ews_message_write_string_parameter (
			msg, "ReceiveCopiesOfMeetingMessages", NULL,
			di->meetingcopies ? "true" : "false");
		e_ews_message_write_string_parameter (
			msg, "ViewPrivateItems", NULL,
			di->view_priv_items ? "true" : "false");

		e_soap_message_end_element (msg); /* DelegateUser */
	}

	e_soap_message_end_element (msg); /* DelegateUsers */

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_add_delegate);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, update_delegate_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_add_delegate_finish (EEwsConnection *cnc,
                                      GAsyncResult *result,
                                      GError **error)
{
	GSimpleAsyncResult *simple;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_add_delegate),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);

	return !g_simple_async_result_propagate_error (simple, error);
}

gboolean
e_ews_connection_add_delegate_sync (EEwsConnection *cnc,
                                    gint pri,
                                    const gchar *mail_id,
                                    const GSList *delegates, /* EwsDelegateInfo * */
                                    GCancellable *cancellable,
                                    GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (E_IS_EWS_CONNECTION (cnc), FALSE);
	g_return_val_if_fail (delegates != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_add_delegate (
		cnc, pri, mail_id, delegates, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_add_delegate_finish (cnc, result, error);

	e_async_closure_free (closure);

	return success;
}

void
e_ews_connection_remove_delegate (EEwsConnection *cnc,
                                  gint pri,
                                  const gchar *mail_id,
                                  const GSList *delegate_ids, /* EwsUserId * */
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;
	const GSList *iter;

	g_return_if_fail (cnc != NULL);
	g_return_if_fail (delegate_ids != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"RemoveDelegate",
			NULL,
			NULL,
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);

	e_soap_message_start_element (msg, "Mailbox", "messages", NULL);
	e_ews_message_write_string_parameter (msg, "EmailAddress", NULL, mail_id ? mail_id : cnc->priv->email);
	e_soap_message_end_element (msg);

	e_soap_message_start_element (msg, "UserIds", "messages", NULL);
	for (iter = delegate_ids; iter; iter = iter->next) {
		const EwsUserId *user_id = iter->data;

		if (!user_id)
			continue;

		e_soap_message_start_element (msg, "UserId", NULL, NULL);
		e_ews_message_write_string_parameter (msg, "PrimarySmtpAddress", NULL, user_id->primary_smtp);
		e_soap_message_end_element (msg); /* UserId */
	}

	e_soap_message_end_element (msg); /* UserIds */

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_remove_delegate);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, update_delegate_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_remove_delegate_finish (EEwsConnection *cnc,
                                         GAsyncResult *result,
                                         GError **error)
{
	GSimpleAsyncResult *simple;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_remove_delegate),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);

	return !g_simple_async_result_propagate_error (simple, error);
}

gboolean
e_ews_connection_remove_delegate_sync (EEwsConnection *cnc,
                                       gint pri,
                                       const gchar *mail_id,
                                       const GSList *delegate_ids, /* EwsUserId * */
                                       GCancellable *cancellable,
                                       GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (E_IS_EWS_CONNECTION (cnc), FALSE);
	g_return_val_if_fail (delegate_ids != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_remove_delegate (
		cnc, pri, mail_id, delegate_ids, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_remove_delegate_finish (cnc, result, error);

	e_async_closure_free (closure);

	return success;
}

void
e_ews_connection_update_delegate (EEwsConnection *cnc,
                                  gint pri,
                                  const gchar *mail_id,
                                  EwsDelegateDeliver deliver_to,
                                  const GSList *delegates, /* EwsDelegateInfo * */
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;
	const GSList *iter;

	g_return_if_fail (cnc != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"UpdateDelegate",
			NULL,
			NULL,
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);

	e_soap_message_start_element (msg, "Mailbox", "messages", NULL);
	e_ews_message_write_string_parameter (msg, "EmailAddress", NULL, mail_id ? mail_id : cnc->priv->email);
	e_soap_message_end_element (msg);

	if (delegates) {
		e_soap_message_start_element (msg, "DelegateUsers", "messages", NULL);
		for (iter = delegates; iter; iter = iter->next) {
			const EwsDelegateInfo *di = iter->data;

			if (!di)
				continue;

			e_soap_message_start_element (msg, "DelegateUser", NULL, NULL);

			e_soap_message_start_element (msg, "UserId", NULL, NULL);
			e_ews_message_write_string_parameter (msg, "PrimarySmtpAddress", NULL, di->user_id->primary_smtp);
			e_soap_message_end_element (msg); /* UserId */

			e_soap_message_start_element (msg, "DelegatePermissions", NULL, NULL);
			set_delegate_permission (msg, "CalendarFolderPermissionLevel", di->calendar);
			set_delegate_permission (msg, "TasksFolderPermissionLevel", di->tasks);
			set_delegate_permission (msg, "InboxFolderPermissionLevel", di->inbox);
			set_delegate_permission (msg, "ContactsFolderPermissionLevel", di->contacts);
			set_delegate_permission (msg, "NotesFolderPermissionLevel", di->notes);
			set_delegate_permission (msg, "JournalFolderPermissionLevel", di->journal);
			e_soap_message_end_element (msg); /* DelegatePermissions */

			e_ews_message_write_string_parameter (
				msg, "ReceiveCopiesOfMeetingMessages", NULL,
				di->meetingcopies ? "true" : "false");
			e_ews_message_write_string_parameter (
				msg, "ViewPrivateItems", NULL,
				di->view_priv_items ? "true" : "false");

			e_soap_message_end_element (msg); /* DelegateUser */
		}

		e_soap_message_end_element (msg); /* DelegateUsers */
	}

	e_ews_message_write_string_parameter (
		msg, "DeliverMeetingRequests", "messages",
		deliver_to == EwsDelegateDeliver_DelegatesOnly ? "DelegatesOnly" :
		deliver_to == EwsDelegateDeliver_DelegatesAndMe ? "DelegatesAndMe" :
		"DelegatesAndSendInformationToMe");

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_update_delegate);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, update_delegate_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_update_delegate_finish (EEwsConnection *cnc,
                                         GAsyncResult *result,
                                         GError **error)
{
	GSimpleAsyncResult *simple;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_update_delegate),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);

	return !g_simple_async_result_propagate_error (simple, error);
}

gboolean
e_ews_connection_update_delegate_sync (EEwsConnection *cnc,
                                       gint pri,
                                       const gchar *mail_id,
                                       EwsDelegateDeliver deliver_to,
                                       const GSList *delegates, /* EwsDelegateInfo * */
                                       GCancellable *cancellable,
                                       GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (E_IS_EWS_CONNECTION (cnc), FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_update_delegate (
		cnc, pri, mail_id, deliver_to, delegates, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_update_delegate_finish (cnc, result, error);

	e_async_closure_free (closure);

	return success;
}

static void
get_folder_permissions_response_cb (ESoapResponse *response,
                                    GSimpleAsyncResult *simple)
{
	EwsAsyncData *async_data;
	ESoapParameter *param;
	ESoapParameter *subparam;
	GError *error = NULL;

	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	param = e_soap_response_get_first_parameter_by_name (
		response, "ResponseMessages", &error);

	/* Sanity check */
	g_return_if_fail (
		(param != NULL && error == NULL) ||
		(param == NULL && error != NULL));

	if (error != NULL) {
		g_simple_async_result_take_error (simple, error);
		return;
	}

	subparam = e_soap_parameter_get_first_child (param);

	while (subparam != NULL) {
		const gchar *name = (const gchar *) subparam->name;

		if (!ews_get_response_status (subparam, &error)) {
			g_simple_async_result_take_error (simple, error);
			return;
		}

		if (CHECK_ELEMENT (name, "GetFolderResponseMessage")) {
			ESoapParameter *node;

			node = e_soap_parameter_get_first_child_by_name (subparam, "Folders");
			if (node) {
				subparam = node;

				node = e_soap_parameter_get_first_child (subparam);
				if (node && node->name && g_str_has_suffix ((const gchar *) node->name, "Folder")) {
					node = e_soap_parameter_get_first_child_by_name (node, "PermissionSet");
					if (node) {
						async_data->items = e_ews_permissions_from_soap_param (node);
					}
				}
			}

			break;
		}

		subparam = e_soap_parameter_get_next_child (subparam);
	}
}

void
e_ews_connection_get_folder_permissions (EEwsConnection *cnc,
                                         gint pri,
                                         EwsFolderId *folder_id,
                                         GCancellable *cancellable,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_if_fail (cnc != NULL);
	g_return_if_fail (folder_id != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"GetFolder",
			NULL,
			NULL,
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);

	e_soap_message_start_element (msg, "FolderShape", "messages", NULL);
	e_ews_message_write_string_parameter (msg, "BaseShape", NULL, "IdOnly");
	e_soap_message_start_element (msg, "AdditionalProperties", NULL, NULL);
	e_ews_message_write_string_parameter_with_attribute (msg, "FieldURI", NULL, NULL, "FieldURI", "folder:PermissionSet");
	e_soap_message_end_element (msg); /* AdditionalProperties */
	e_soap_message_end_element (msg); /* FolderShape */

	e_soap_message_start_element (msg, "FolderIds", "messages", NULL);
	ews_append_folder_id_to_msg (msg, cnc->priv->email, folder_id);
	e_soap_message_end_element (msg);

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_get_folder_permissions);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, get_folder_permissions_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

/* free permissions with e_ews_permissions_free() */
gboolean
e_ews_connection_get_folder_permissions_finish (EEwsConnection *cnc,
                                                GAsyncResult *result,
                                                GSList **permissions,
                                                GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (permissions != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_get_folder_permissions),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	*permissions = async_data->items;

	return TRUE;
}

/* free permissions with e_ews_permissions_free() */
gboolean
e_ews_connection_get_folder_permissions_sync (EEwsConnection *cnc,
                                              gint pri,
                                              EwsFolderId *folder_id,
                                              GSList **permissions,
                                              GCancellable *cancellable,
                                              GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (E_IS_EWS_CONNECTION (cnc), FALSE);
	g_return_val_if_fail (folder_id != NULL, FALSE);
	g_return_val_if_fail (permissions != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_get_folder_permissions (
		cnc, pri, folder_id, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_get_folder_permissions_finish (
		cnc, result, permissions, error);

	e_async_closure_free (closure);

	return success;
}

void
e_ews_connection_set_folder_permissions (EEwsConnection *cnc,
                                         gint pri,
                                         EwsFolderId *folder_id,
                                         EEwsFolderType folder_type,
                                         const GSList *permissions,
                                         GCancellable *cancellable,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;
	const GSList *iter;

	g_return_if_fail (cnc != NULL);
	g_return_if_fail (folder_id != NULL);
	g_return_if_fail (permissions != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"UpdateFolder",
			NULL,
			NULL,
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);

	e_soap_message_start_element (msg, "FolderChanges", "messages", NULL);
	e_ews_message_start_item_change (
		msg, E_EWS_ITEMCHANGE_TYPE_FOLDER,
		folder_id->id, folder_id->change_key, 0);

	e_soap_message_start_element (msg, "SetFolderField", NULL, NULL);
	e_ews_message_write_string_parameter_with_attribute (msg, "FieldURI", NULL, NULL, "FieldURI", "folder:PermissionSet");

	switch (folder_type) {
	default:
	case E_EWS_FOLDER_TYPE_MAILBOX:
		e_soap_message_start_element (msg, "Folder", NULL, NULL);
		break;
	case E_EWS_FOLDER_TYPE_CALENDAR:
		e_soap_message_start_element (msg, "CalendarFolder", NULL, NULL);
		break;
	case E_EWS_FOLDER_TYPE_CONTACTS:
		e_soap_message_start_element (msg, "ContactsFolder", NULL, NULL);
		break;
	case E_EWS_FOLDER_TYPE_SEARCH:
		e_soap_message_start_element (msg, "SearchFolder", NULL, NULL);
		break;
	case E_EWS_FOLDER_TYPE_TASKS:
		e_soap_message_start_element (msg, "TasksFolder", NULL, NULL);
		break;
	}

	e_soap_message_start_element (msg, "PermissionSet", NULL, NULL);
	if (folder_type == E_EWS_FOLDER_TYPE_CALENDAR)
		e_soap_message_start_element (msg, "CalendarPermissions", NULL, NULL);
	else
		e_soap_message_start_element (msg, "Permissions", NULL, NULL);

	for (iter = permissions; iter; iter = iter->next) {
		EEwsPermission *perm = iter->data;
		const gchar *perm_level_name;

		if (!perm)
			continue;

		if (folder_type == E_EWS_FOLDER_TYPE_CALENDAR)
			e_soap_message_start_element (msg, "CalendarPermission", NULL, NULL);
		else
			e_soap_message_start_element (msg, "Permission", NULL, NULL);

		e_soap_message_start_element (msg, "UserId", NULL, NULL);

		switch (perm->user_type) {
		case E_EWS_PERMISSION_USER_TYPE_NONE:
			g_return_if_reached ();
			break;
		case E_EWS_PERMISSION_USER_TYPE_ANONYMOUS:
			e_ews_message_write_string_parameter (msg, "DistinguishedUser", NULL, "Anonymous");
			break;
		case E_EWS_PERMISSION_USER_TYPE_DEFAULT:
			e_ews_message_write_string_parameter (msg, "DistinguishedUser", NULL, "Default");
			break;
		case E_EWS_PERMISSION_USER_TYPE_REGULAR:
			e_ews_message_write_string_parameter (msg, "PrimarySmtpAddress", NULL, perm->primary_smtp);
			break;
		}

		e_soap_message_end_element (msg); /* UserId */

		e_ews_permission_rights_to_level_name (perm->rights);

		perm_level_name = e_ews_permission_rights_to_level_name (perm->rights);

		if (g_strcmp0 (perm_level_name, "Custom") == 0) {
			e_ews_message_write_string_parameter (
				msg, "CanCreateItems", NULL,
				(perm->rights & E_EWS_PERMISSION_BIT_CREATE) != 0 ? "true" : "false");
			e_ews_message_write_string_parameter (
				msg, "CanCreateSubFolders", NULL,
				(perm->rights & E_EWS_PERMISSION_BIT_CREATE_SUBFOLDER) != 0 ? "true" : "false");
			e_ews_message_write_string_parameter (
				msg, "IsFolderOwner", NULL,
				(perm->rights & E_EWS_PERMISSION_BIT_FOLDER_OWNER) != 0 ? "true" : "false");
			e_ews_message_write_string_parameter (
				msg, "IsFolderVisible", NULL,
				(perm->rights & E_EWS_PERMISSION_BIT_FOLDER_VISIBLE) != 0 ? "true" : "false");
			e_ews_message_write_string_parameter (
				msg, "IsFolderContact", NULL,
				(perm->rights & E_EWS_PERMISSION_BIT_FOLDER_CONTACT) != 0 ? "true" : "false");
			e_ews_message_write_string_parameter (
				msg, "EditItems", NULL,
				(perm->rights & E_EWS_PERMISSION_BIT_EDIT_ANY) != 0 ? "All" :
				(perm->rights & E_EWS_PERMISSION_BIT_EDIT_OWNED) != 0 ? "Owned" : "None");
			e_ews_message_write_string_parameter (
				msg, "DeleteItems", NULL,
				(perm->rights & E_EWS_PERMISSION_BIT_DELETE_ANY) != 0 ? "All" :
				(perm->rights & E_EWS_PERMISSION_BIT_DELETE_OWNED) != 0 ? "Owned" : "None");
			if (folder_type == E_EWS_FOLDER_TYPE_CALENDAR)
				e_ews_message_write_string_parameter (
					msg, "ReadItems", NULL,
					(perm->rights & E_EWS_PERMISSION_BIT_READ_ANY) != 0 ? "FullDetails" :
					(perm->rights & E_EWS_PERMISSION_BIT_FREE_BUSY_DETAILED) != 0 ? "TimeAndSubjectAndLocation" :
					(perm->rights & E_EWS_PERMISSION_BIT_FREE_BUSY_SIMPLE) != 0 ? "TimeOnly" : "None");
			else
				e_ews_message_write_string_parameter (
					msg, "ReadItems", NULL,
					(perm->rights & E_EWS_PERMISSION_BIT_READ_ANY) != 0 ? "FullDetails" : "None");
		}

		e_ews_message_write_string_parameter (
			msg,
			folder_type == E_EWS_FOLDER_TYPE_CALENDAR ? "CalendarPermissionLevel" : "PermissionLevel", NULL,
			perm_level_name);

		e_soap_message_end_element (msg); /* Permission/CalendarPermission */
	}

	e_soap_message_end_element (msg); /* Permissions */
	e_soap_message_end_element (msg); /* PermissionSet */
	e_soap_message_end_element (msg); /* Folder/CalendarFolder/... */
	e_soap_message_end_element (msg); /* SetFolderField */

	e_ews_message_end_item_change (msg);

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_set_folder_permissions);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, update_folder_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_set_folder_permissions_finish (EEwsConnection *cnc,
                                                GAsyncResult *result,
                                                GError **error)
{
	GSimpleAsyncResult *simple;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_set_folder_permissions),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);

	return !g_simple_async_result_propagate_error (simple, error);
}

gboolean
e_ews_connection_set_folder_permissions_sync (EEwsConnection *cnc,
                                              gint pri,
                                              EwsFolderId *folder_id,
                                              EEwsFolderType folder_type,
                                              const GSList *permissions,
                                              GCancellable *cancellable,
                                              GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (E_IS_EWS_CONNECTION (cnc), FALSE);
	g_return_val_if_fail (folder_id != NULL, FALSE);
	g_return_val_if_fail (permissions != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_set_folder_permissions (
		cnc, pri, folder_id, folder_type, permissions, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_set_folder_permissions_finish (
		cnc, result, error);

	e_async_closure_free (closure);

	return success;
}

static void
get_password_expiration_response_cb (ESoapResponse *response,
                                     GSimpleAsyncResult *simple)
{
	EwsAsyncData *async_data;
	ESoapParameter *param;
	gchar *exp_date;
	GError *error = NULL;

	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	param = e_soap_response_get_first_parameter_by_name (
		response, "PasswordExpirationDate", &error);

	/* Sanity check */
	g_return_if_fail (
		(param != NULL && error == NULL) ||
		(param == NULL && error != NULL));

	if (error != NULL) {
		g_simple_async_result_take_error (simple, error);
		return;
	}

	exp_date = e_soap_parameter_get_string_value (param);

	async_data->items = g_slist_append (async_data->items, exp_date);
}

/**
 * e_ews_connection_get_password_expiration
 * @cnc:
 * @pri:
 * @mail_id: mail is for which password expiration is requested
 * @cb:
 * @cancellable:
 * @user_data:
 **/
void
e_ews_connection_get_password_expiration (EEwsConnection *cnc,
                                          gint pri,
                                          const gchar *mail_id,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"GetPasswordExpirationDate",
			NULL,
			NULL,
			cnc->priv->version,
			E_EWS_EXCHANGE_2010_SP2,
			FALSE);
	e_ews_message_write_string_parameter (msg, "MailboxSmtpAddress", NULL, mail_id ? mail_id : cnc->priv->email);
	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_get_password_expiration);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, get_password_expiration_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_get_password_expiration_finish (EEwsConnection *cnc,
                                                 GAsyncResult *result,
                                                 gchar **exp_date,
                                                 GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (exp_date != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_get_password_expiration),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	g_return_val_if_fail (async_data->items != NULL, FALSE);

	*exp_date = async_data->items->data;
	g_slist_free (async_data->items);

	return TRUE;
}

/**
 * e_ews_connection_get_password_expiration_sync
 * @cnc:
 * @pri:
 * @mail_id: mail id for which password expiration is requested
 * @cancellable:
 * @error:
 **/
gboolean
e_ews_connection_get_password_expiration_sync (EEwsConnection *cnc,
                                               gint pri,
                                               const gchar *mail_id,
                                               gchar **exp_date,
                                               GCancellable *cancellable,
                                               GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (E_IS_EWS_CONNECTION (cnc), FALSE);
	g_return_val_if_fail (exp_date != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_get_password_expiration (
		cnc, pri, mail_id, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_get_password_expiration_finish (
		cnc, result, exp_date, error);

	e_async_closure_free (closure);

	return success;
}

static void
get_folder_info_response_cb (ESoapResponse *response,
                             GSimpleAsyncResult *simple)
{
	EwsAsyncData *async_data;
	ESoapParameter *param;
	ESoapParameter *subparam;
	GError *error = NULL;

	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	param = e_soap_response_get_first_parameter_by_name (
		response, "ResponseMessages", &error);

	/* Sanity check */
	g_return_if_fail (
		(param != NULL && error == NULL) ||
		(param == NULL && error != NULL));

	if (error != NULL) {
		g_simple_async_result_take_error (simple, error);
		return;
	}

	subparam = e_soap_parameter_get_first_child (param);

	while (subparam != NULL) {
		const gchar *name = (const gchar *) subparam->name;

		if (!ews_get_response_status (subparam, &error)) {
			g_simple_async_result_take_error (simple, error);
			return;
		}

		if (CHECK_ELEMENT (name, "GetFolderResponseMessage")) {
			ESoapParameter *node;

			node = e_soap_parameter_get_first_child_by_name (subparam, "Folders");
			if (node) {
				EEwsFolder *folder = e_ews_folder_new_from_soap_parameter (node);

				if (folder)
					async_data->items = g_slist_prepend (NULL, folder);
			}

			break;
		}

		subparam = e_soap_parameter_get_next_child (subparam);
	}
}

void
e_ews_connection_get_folder_info (EEwsConnection *cnc,
                                  gint pri,
                                  const gchar *mail_id,
                                  const EwsFolderId *folder_id,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_if_fail (cnc != NULL);
	g_return_if_fail (folder_id != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"GetFolder",
			NULL,
			NULL,
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);

	e_soap_message_start_element (msg, "FolderShape", "messages", NULL);
	e_ews_message_write_string_parameter (msg, "BaseShape", NULL, "Default");
	e_soap_message_start_element (msg, "AdditionalProperties", NULL, NULL);
	e_ews_message_write_string_parameter_with_attribute (msg, "FieldURI", NULL, NULL, "FieldURI", "folder:FolderClass");
	e_ews_message_write_string_parameter_with_attribute (msg, "FieldURI", NULL, NULL, "FieldURI", "folder:ParentFolderId");
	e_soap_message_end_element (msg); /* AdditionalProperties */
	e_soap_message_end_element (msg); /* FolderShape */

	e_soap_message_start_element (msg, "FolderIds", "messages", NULL);
	ews_append_folder_id_to_msg (msg, mail_id, folder_id);
	e_soap_message_end_element (msg);

	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_get_folder_info);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, get_folder_info_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_get_folder_info_finish (EEwsConnection *cnc,
                                         GAsyncResult *result,
                                         EEwsFolder **folder,
                                         GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (folder != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_get_folder_info),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	if (!async_data->items)
		return FALSE;

	*folder = async_data->items ? async_data->items->data : NULL;

	g_slist_free (async_data->items);
	async_data->items = NULL;

	return TRUE;
}

gboolean
e_ews_connection_get_folder_info_sync (EEwsConnection *cnc,
                                       gint pri,
                                       const gchar *mail_id,
                                       const EwsFolderId *folder_id,
                                       EEwsFolder **folder,
                                       GCancellable *cancellable,
                                       GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (E_IS_EWS_CONNECTION (cnc), FALSE);
	g_return_val_if_fail (folder_id != NULL, FALSE);
	g_return_val_if_fail (folder != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_get_folder_info (
		cnc, pri, mail_id, folder_id, cancellable,
		e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_get_folder_info_finish (
		cnc, result, folder, error);

	e_async_closure_free (closure);

	return success;
}

static void
ews_handle_root_folder_param_folders (ESoapParameter *subparam,
				      EwsAsyncData *async_data)
{
	ESoapParameter *node, *subparam1;
	gchar *last, *total;
	gint total_items;
	EEwsFolder *folder;
	gboolean includes_last_item = FALSE;

	node = e_soap_parameter_get_first_child_by_name (subparam, "RootFolder");
	total = e_soap_parameter_get_property (node, "TotalItemsInView");
	total_items = atoi (total);
	g_free (total);
	last = e_soap_parameter_get_property (node, "IncludesLastItemInRange");
	if (!strcmp (last, "true"))
		includes_last_item = TRUE;
	g_free (last);

	node = e_soap_parameter_get_first_child_by_name (node, "Folders");
	for (subparam1 = e_soap_parameter_get_first_child (node);
	     subparam1; subparam1 = e_soap_parameter_get_next_child (subparam1)) {
		folder = e_ews_folder_new_from_soap_parameter (subparam1);
		if (!folder) continue;
		async_data->items = g_slist_append (async_data->items, folder);
	}
	async_data->total_items = total_items;
	async_data->includes_last_item = includes_last_item;
}

static void
find_folder_response_cb (ESoapResponse *response,
			 GSimpleAsyncResult *simple)
{
	EwsAsyncData *async_data;
	ESoapParameter *param;
	ESoapParameter *subparam;
	GError *error = NULL;

	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	param = e_soap_response_get_first_parameter_by_name (
		response, "ResponseMessages", &error);

	/* Sanity check */
	g_return_if_fail (
		(param != NULL && error == NULL) ||
		(param == NULL && error != NULL));

	if (error != NULL) {
		g_simple_async_result_take_error (simple, error);
		return;
	}

	subparam = e_soap_parameter_get_first_child (param);

	while (subparam != NULL) {
		const gchar *name = (const gchar *) subparam->name;

		if (!ews_get_response_status (subparam, &error)) {
			g_simple_async_result_take_error (simple, error);
			return;
		}

		if (CHECK_ELEMENT (name, "FindFolderResponseMessage"))
			ews_handle_root_folder_param_folders (subparam, async_data);

		subparam = e_soap_parameter_get_next_child (subparam);
	}
}

void
e_ews_connection_find_folder (EEwsConnection *cnc,
			      gint pri,
			      const EwsFolderId *fid,
			      GCancellable *cancellable,
			      GAsyncReadyCallback callback,
			      gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_if_fail (cnc != NULL);

	msg = e_ews_message_new_with_header (
			cnc->priv->uri,
			cnc->priv->impersonate_user,
			"FindFolder",
			"Traversal",
			"Shallow",
			cnc->priv->version,
			E_EWS_EXCHANGE_2007_SP1,
			FALSE);
	e_soap_message_start_element (msg, "FolderShape", "messages", NULL);
	e_ews_message_write_string_parameter (msg, "BaseShape", NULL, "Default");
	e_soap_message_start_element (msg, "AdditionalProperties", NULL, NULL);
	e_ews_message_write_string_parameter_with_attribute (msg, "FieldURI", NULL, NULL, "FieldURI", "folder:FolderClass");
	e_ews_message_write_string_parameter_with_attribute (msg, "FieldURI", NULL, NULL, "FieldURI", "folder:ChildFolderCount");
	e_soap_message_end_element (msg); /* AdditionalProperties */
	e_soap_message_end_element (msg);

	e_soap_message_start_element (msg, "ParentFolderIds", "messages", NULL);

	if (fid->is_distinguished_id)
		e_ews_message_write_string_parameter_with_attribute (msg, "DistinguishedFolderId", NULL, NULL, "Id", fid->id);
	else
		e_ews_message_write_string_parameter_with_attribute (msg, "FolderId", NULL, NULL, "Id", fid->id);

	e_soap_message_end_element (msg);

	/* Complete the footer and print the request */
	e_ews_message_write_footer (msg);

	simple = g_simple_async_result_new (
		G_OBJECT (cnc), callback, user_data,
		e_ews_connection_find_folder);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	e_ews_connection_queue_request (
		cnc, msg, find_folder_response_cb,
		pri, cancellable, simple);

	g_object_unref (simple);
}

gboolean
e_ews_connection_find_folder_finish (EEwsConnection *cnc,
				     GAsyncResult *result,
				     gboolean *includes_last_item,
				     GSList **folders,
				     GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (cnc != NULL, FALSE);
	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_find_folder),
		FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	*includes_last_item = async_data->includes_last_item;
	*folders = async_data->items;

	return TRUE;
}

gboolean
e_ews_connection_find_folder_sync (EEwsConnection *cnc,
				   gint pri,
				   const EwsFolderId *fid,
				   gboolean *includes_last_item,
				   GSList **folders,
				   GCancellable *cancellable,
				   GError **error)
{
	EAsyncClosure *closure;
	GAsyncResult *result;
	gboolean success;

	g_return_val_if_fail (cnc != NULL, FALSE);

	closure = e_async_closure_new ();

	e_ews_connection_find_folder (cnc, pri, fid, cancellable, e_async_closure_callback, closure);

	result = e_async_closure_wait (closure);

	success = e_ews_connection_find_folder_finish (
		cnc, result, includes_last_item, folders, error);

	e_async_closure_free (closure);

	return success;
}
