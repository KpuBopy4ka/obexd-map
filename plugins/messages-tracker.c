/*
 *
 *  OBEX Server
 *
 *  Copyright (C) 2010-2011  Nokia Corporation
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _XOPEN_SOURCE
#define _BSD_SOURCE

#include <errno.h>
#include <glib.h>
#include <string.h>
#include <gdbus.h>
#include <stdlib.h>
#include <time.h>

#include "log.h"
#include "messages.h"
#include "bmsg.h"
#include "bmsg_parser.h"
#include "messages-qt/messages-qt.h"

/* 16 chars and terminating \0 */
#define HANDLE_LEN (16 + 1)

#define TRACKER_SERVICE "org.freedesktop.Tracker1"
#define TRACKER_RESOURCES_PATH "/org/freedesktop/Tracker1/Resources"
#define TRACKER_RESOURCES_INTERFACE "org.freedesktop.Tracker1.Resources"

#define TRACKER_MESSAGE_TSTAMP_FORMAT "%Y-%m-%dT%TZ"

#define QUERY_RESPONSE_SIZE 15
#define MESSAGE_HANDLE_SIZE 16
#define MESSAGE_HANDLE_PREFIX_LEN 8
#define MESSAGE_GRP_PREFIX_LEN 13

#define SMS_DEFAULT_CHARSET "UTF-8"

#define STATUS_NOT_SET 0xFF

#define MESSAGES_FILTER_BY_HANDLE "FILTER (xsd:string(?msg) = \"message:%s\" ) ."

#define MESSAGE_STAT_EMPTY	0x01
#define MESSAGE_STAT_READ	0x02
#define MESSAGE_STAT_DELETED	0x04
#define MESSAGE_STAT_SENT	0x08

#define MESSAGE_HANDLE 0
#define MESSAGE_SUBJECT 1
#define MESSAGE_SDATE 2
#define MESSAGE_RDATE 3
#define MESSAGE_CONTACT_FN 4
#define MESSAGE_CONTACT_GIVEN 5
#define MESSAGE_CONTACT_FAMILY 6
#define MESSAGE_CONTACT_ADDITIONAL 7
#define MESSAGE_CONTACT_PREFIX 8
#define MESSAGE_CONTACT_SUFFIX 9
#define MESSAGE_CONTACT_PHONE 10
#define MESSAGE_READ 11
#define MESSAGE_SENT 12
#define MESSAGE_CONTENT 13
#define MESSAGE_GROUP 14

#define LIST_MESSAGES_QUERY						\
"SELECT "								\
"?msg "									\
"nmo:messageSubject(?msg) "						\
"nmo:sentDate(?msg) "							\
"nmo:receivedDate(?msg) "						\
"nco:fullname(?cont) "							\
"nco:nameGiven(?cont) "							\
"nco:nameFamily(?cont) "						\
"nco:nameAdditional(?cont) "						\
"nco:nameHonorificPrefix(?cont) "					\
"nco:nameHonorificSuffix(?cont) "					\
"nco:phoneNumber(?phone) "						\
"nmo:isRead(?msg) "							\
"nmo:isSent(?msg) "							\
"nie:plainTextContent(?msg) "						\
"nmo:communicationChannel(?msg) "					\
"WHERE { "								\
	"?msg a nmo:SMSMessage . "					\
	"%s "								\
	"%s "								\
	"{ "								\
	"	?msg nmo:from ?msg_cont .  "				\
	"	?msg nmo:isSent false "					\
	"} UNION { "							\
	"	?msg nmo:to ?msg_cont . "				\
	"	?msg nmo:isSent true "					\
	"} "								\
	"?msg_cont nco:hasPhoneNumber ?phone . "			\
	"?phone maemo:localPhoneNumber ?lphone . "			\
	"OPTIONAL { "							\
		"{ SELECT ?cont ?lphone "				\
			"count(?cont) as ?cnt "				\
		"WHERE { "						\
			"?cont a nco:PersonContact . "			\
			"{ "						\
				"?cont nco:hasAffiliation ?_role . "	\
				"?_role nco:hasPhoneNumber ?_phone . "	\
			"} UNION { "					\
				"?cont nco:hasPhoneNumber ?_phone "	\
			"} "						\
			"?_phone maemo:localPhoneNumber ?lphone. "	\
		"} GROUP BY ?lphone } "					\
		"FILTER(?cnt = 1) "					\
	"} "								\
"} ORDER BY DESC(nmo:sentDate(?msg)) "

#define HANDLE_BY_UUID_QUERY		\
"SELECT ?msg { "			\
	"?msg a nmo:SMSMessage "	\
	"; nmo:messageId \"%s\" "	\
"}"

typedef void (*reply_list_foreach_cb)(const char **reply, void *user_data);

enum event_direction {
	DIRECTION_UNKNOWN,
	DIRECTION_INBOUND,
	DIRECTION_OUTBOUND
};

struct message_folder {
	char *name;
	GSList *subfolders;
	char *query;
};

struct push_message_request {
	GString *body;
	messages_push_message_cb cb;
	struct bmsg_bmsg *bmsg;
	void *user_data;
	char *uuid;
	char *name;
	guint watch;
	gboolean retry;
	DBusPendingCall *send_sms, *get_handle;
	void *insert_message_call;
};

struct request {
	char *name;
	uint16_t max;
	uint16_t offset;
	uint16_t size;
	void *user_data;
	gboolean count;
	gboolean new_message;
	reply_list_foreach_cb generate_response;
	struct messages_filter *filter;
	unsigned long flags;
	gboolean deleted;
	void *set_status_call;
	DBusPendingCall *pc;
	union {
		messages_folder_listing_cb folder_list;
		messages_get_messages_listing_cb messages_list;
		messages_get_message_cb message;
		messages_set_message_status_cb status;
	} cb;
};

struct session {
	char *cwd;
	struct message_folder *folder;
	void *event_user_data;
	messages_event_cb event_cb;
	struct request *request;
	GHashTable *msg_stat;
	GDestroyNotify abort_request;
	void *request_data;
	gboolean op_in_progress;
	GSList *mns_event_cache;
};

static struct message_folder *folder_tree = NULL;
static DBusConnection *session_connection = NULL;
static unsigned long message_id_tracker_id;
static GSList *mns_srv;
static gint newmsg_watch_id, delmsg_watch_id, delgrp_watch_id;
static GArray *msg_grp;

static void free_request(struct request *request)
{
	g_free(request->name);
	g_free(request->filter->period_begin);
	g_free(request->filter->period_end);
	g_free(request->filter->originator);
	g_free(request->filter->recipient);
	g_free(request->filter);

	g_free(request);
}

static void free_msg_data(struct messages_message *msg)
{
	g_free(msg->handle);
	g_free(msg->subject);
	g_free(msg->datetime);
	g_free(msg->sender_name);
	g_free(msg->sender_addressing);
	g_free(msg->replyto_addressing);
	g_free(msg->recipient_name);
	g_free(msg->recipient_addressing);
	g_free(msg->type);
	g_free(msg->reception_status);
	g_free(msg->size);
	g_free(msg->attachment_size);

	g_free(msg);
}

static struct messages_filter *copy_messages_filter(
					const struct messages_filter *orig)
{
	struct messages_filter *filter = g_new0(struct messages_filter, 1);
	filter->parameter_mask = orig->parameter_mask;
	filter->type = orig->type;
	filter->period_begin = g_strdup(orig->period_begin);
	filter->period_end = g_strdup(orig->period_end);
	filter->read_status = orig->read_status;
	filter->recipient = g_strdup(orig->recipient);
	filter->originator = g_strdup(orig->originator);
	filter->priority = orig->priority;

	return filter;
}

static gboolean validate_handle(const char *h)
{
	while(*h && *h >= '0' && *h <= '9')
		h++;

	return *h == '\0' ? TRUE : FALSE;
}

static char *fill_handle(const char *handle)
{
	int fill_size;
	char *fill, *ret;

	if (handle == NULL)
		return NULL;

	fill_size = MESSAGE_HANDLE_SIZE - strlen(handle);
	fill = g_strnfill(fill_size, '0');
	ret = g_strdup_printf("%s%s", fill, handle);

	g_free(fill);

	return ret;
}

static char *strip_handle(const char *handle)
{
	const char *ptr_new = handle;

	while (*ptr_new++ == '0') ;

	return g_strdup(ptr_new - 1);
}

static char **string_array_from_iter(DBusMessageIter iter, int array_len)
{
	DBusMessageIter sub;
	char **result;
	int i;

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
		return NULL;

	result = g_new0(char *, array_len + 1);

	dbus_message_iter_recurse(&iter, &sub);

	i = 0;
	while (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID) {
		char *arg;

		if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_STRING) {
			g_free(result);

			return NULL;
		}

		dbus_message_iter_get_basic(&sub, &arg);

		result[i++] = arg;

		dbus_message_iter_next(&sub);
	}

	return result;
}

static void query_reply(DBusPendingCall *call, void *user_data)
{
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	struct session *session = user_data;
	DBusMessageIter iter, element;
	DBusError derr;

	dbus_error_init(&derr);
	if (dbus_set_error_from_message(&derr, reply)) {
		error("Replied with an error: %s, %s", derr.name, derr.message);
		dbus_error_free(&derr);

		goto done;
	}

	dbus_message_iter_init(reply, &iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
		error("SparqlQuery reply is not an array");

		goto done;
	}

	dbus_message_iter_recurse(&iter, &element);

	while (dbus_message_iter_get_arg_type(&element) != DBUS_TYPE_INVALID) {
		char **node;

		if (dbus_message_iter_get_arg_type(&element)
							!= DBUS_TYPE_ARRAY) {
			error("Element is not an array\n");

			goto done;
		}

		node = string_array_from_iter(element, QUERY_RESPONSE_SIZE);

		if (node != NULL)
			session->request->generate_response((const char **) node,
								session);

		g_free(node);

		dbus_message_iter_next(&element);
	}

done:
	session->request->generate_response(NULL, session);

	dbus_message_unref(reply);
	dbus_pending_call_unref(call);
}

static DBusPendingCall *query_tracker(char *query, void *user_data, int *err)
{
	DBusPendingCall *call;
	DBusMessage *msg;

	msg = dbus_message_new_method_call(TRACKER_SERVICE,
						TRACKER_RESOURCES_PATH,
						TRACKER_RESOURCES_INTERFACE,
						"SparqlQuery");
	if (msg == NULL) {
		if (err)
			*err = -EPERM;

		return NULL;
	}

	dbus_message_append_args(msg, DBUS_TYPE_STRING, &query,
							 DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(session_connection, msg, &call,
								-1) == FALSE) {
		error("Could not send dbus message");

		dbus_message_unref(msg);
		if (err)
			*err = -EPERM;

		return NULL;
	}

	dbus_pending_call_set_notify(call, query_reply, user_data, NULL);

	dbus_message_unref(msg);

	return call;
}

static char *folder2query(const struct message_folder *folder,
							const char *query)
{
	return g_strdup_printf(query, folder->query, "");
}

static struct message_folder *get_folder(const char *folder)
{
	GSList *folders = folder_tree->subfolders;
	struct message_folder *last = NULL;
	char **path;
	int i;

	if (g_strcmp0(folder, "/") == 0)
		return folder_tree;

	path = g_strsplit(folder, "/", 0);

	for (i = 1; path[i] != NULL; i++) {
		gboolean match_found = FALSE;
		GSList *l;

		for (l = folders; l != NULL; l = g_slist_next(l)) {
			struct message_folder *folder = l->data;

			if (g_strcmp0(folder->name, path[i]) == 0) {
				match_found = TRUE;
				last = l->data;
				folders = folder->subfolders;
				break;
			}
		}

		if (!match_found) {
			g_strfreev(path);
			return NULL;
		}
	}

	g_strfreev(path);

	return last;
}

static struct message_folder *create_folder(const char *name, const char *query)
{
	struct message_folder *folder = g_new0(struct message_folder, 1);

	folder->name = g_strdup(name);
	folder->query = g_strdup(query);

	return folder;
}

static void destroy_folder_tree(void *root)
{
	struct message_folder *folder = root;
	GSList *tmp, *next;

	if (folder == NULL)
		return;

	g_free(folder->name);
	g_free(folder->query);

	tmp = folder->subfolders;
	while (tmp != NULL) {
		next = g_slist_next(tmp);
		destroy_folder_tree(tmp->data);
		tmp = next;
	}

	g_slist_free(folder->subfolders);
	g_free(folder);
}

static void create_folder_tree()
{
	struct message_folder *parent, *child;

	folder_tree = create_folder("/", "FILTER (!BOUND(?msg))");

	parent = create_folder("telecom", "FILTER (!BOUND(?msg))");
	folder_tree->subfolders = g_slist_append(folder_tree->subfolders,
								parent);

	child = create_folder("msg", "FILTER (!BOUND(?msg))");
	parent->subfolders = g_slist_append(parent->subfolders, child);

	parent = child;

	child = create_folder("inbox", "?msg nmo:isSent \"false\" ; "
				"nmo:isDeleted \"false\" ; "
				"nmo:isDraft \"false\". ");
	parent->subfolders = g_slist_append(parent->subfolders, child);

	child = create_folder("sent", "?msg nmo:isDeleted \"false\" ; "
				"nmo:isSent \"true\" . ");
	parent->subfolders = g_slist_append(parent->subfolders, child);

	child = create_folder("outbox", " FILTER (!BOUND(?msg)) ");
	parent->subfolders = g_slist_append(parent->subfolders, child);

	child = create_folder("deleted", " ");
	parent->subfolders = g_slist_append(parent->subfolders, child);
}

static char *merge_names(const char *name, const char *lastname)
{
	char *tmp = NULL;

	if (strlen(lastname) != 0) {
		if (strlen(name) == 0)
			tmp = g_strdup(lastname);
		else
			tmp = g_strdup_printf("%s %s", name, lastname);

	} else if (strlen(name) != 0)
		tmp = g_strdup(name);
	else
		tmp = g_strdup("");

	return tmp;
}

static char *message2folder(const struct messages_message *data)
{
	if (data->sent == TRUE)
		return g_strdup("telecom/msg/sent");

	if (data->sent == FALSE)
		return g_strdup("telecom/msg/inbox");

	return NULL;
}

static char *path2query(const char *folder, const char *query,
							const char *user_rule)
{
	if (g_str_has_suffix(folder, "telecom/msg/inbox") == TRUE)
		return g_strdup_printf(query, "?msg nmo:isSent \"false\" ; "
					"nmo:isDeleted \"false\" ; "
					"nmo:isDraft \"false\". ", user_rule);

	if (g_str_has_suffix(folder, "telecom/msg/sent") == TRUE)
		return g_strdup_printf(query, "?msg nmo:isSent \"true\" ; "
				"nmo:isDeleted \"false\" . ", user_rule);

	if (g_str_has_suffix(folder, "telecom/msg/deleted") == TRUE)
		return g_strdup_printf(query, "?msg nmo:isDeleted \"true\" . ",
					user_rule);

	if (g_str_has_suffix(folder, "telecom/msg") == TRUE)
		return g_strdup_printf(query, "", user_rule);

	return NULL;
}

static gboolean filter_message(struct messages_message *message,
						struct messages_filter *filter)
{
	if (filter->type != 0) {
		if (g_strcmp0(message->type, "SMS_GSM") == 0 &&
				(filter->type & 0x01))
			return FALSE;

		if (g_strcmp0(message->type, "SMS_CDMA") == 0 &&
				(filter->type & 0x02))
			return FALSE;

		if (g_strcmp0(message->type, "SMS_EMAIL") == 0 &&
				(filter->type & 0x04))
			return FALSE;

		if (g_strcmp0(message->type, "SMS_MMS") == 0 &&
				(filter->type & 0x08))
			return FALSE;
	}

	if (filter->read_status != 0) {
		if (filter->read_status == 0x01 && message->read != FALSE)
			return FALSE;

		if (filter->read_status == 0x02 && message->read != TRUE)
			return FALSE;
	}

	if (filter->priority != 0) {
		if (filter->priority == 0x01 && message->priority == FALSE)
			return FALSE;

		if (filter->priority == 0x02 && message->priority == TRUE)
			return FALSE;
	}

	if (filter->period_begin != NULL &&
			g_strcmp0(filter->period_begin, message->datetime) > 0)
		return FALSE;

	if (filter->period_end != NULL &&
			g_strcmp0(filter->period_end, message->datetime) < 0)
		return FALSE;

	if (filter->originator != NULL) {
		char *orig = g_strdup_printf("*%s*", filter->originator);

		if (g_pattern_match_simple(orig,
					message->sender_addressing != NULL ?
					message->sender_addressing : "") == FALSE &&
				g_pattern_match_simple(orig,
					message->sender_name != NULL ?
					message->sender_name : "") == FALSE) {
			g_free(orig);
			return FALSE;
		}
		g_free(orig);
	}

	if (filter->recipient != NULL) {
		char *recip = g_strdup_printf("*%s*", filter->recipient);

		if (g_pattern_match_simple(recip,
					message->recipient_addressing != NULL ?
					message->recipient_addressing : "") ==
					FALSE &&
				g_pattern_match_simple(recip,
					message->recipient_name != NULL ?
					message->recipient_name : "") == FALSE) {
			g_free(recip);
			return FALSE;
		}

		g_free(recip);
	}

	return TRUE;
}

static struct phonebook_contact *pull_message_contact(const char **reply,
								gboolean sent)
{
	struct phonebook_contact *contact;
	struct phonebook_field *number;

	contact = g_new0(struct phonebook_contact, 1);

	contact->fullname = g_strdup(reply[MESSAGE_CONTACT_FN]);
	contact->given = g_strdup(reply[MESSAGE_CONTACT_GIVEN]);
	contact->family = g_strdup(reply[MESSAGE_CONTACT_FAMILY]);
	contact->additional = g_strdup(reply[MESSAGE_CONTACT_ADDITIONAL]);
	contact->prefix = g_strdup(reply[MESSAGE_CONTACT_PREFIX]);
	contact->suffix = g_strdup(reply[MESSAGE_CONTACT_SUFFIX]);

	number = g_new0(struct phonebook_field, 1);
	number->text = g_strdup(reply[MESSAGE_CONTACT_PHONE]);
	number->type = TEL_TYPE_NONE;
	contact->numbers = g_slist_append(contact->numbers, number);

	return contact;
}

static char *format_tstamp(const char *stamp, const char *format)
{
	struct tm tm;
	time_t time;
	char *local_time = g_new0(char, 16); /* format: "YYYYMMDDTHHMMSS\0" */

	if (strptime(stamp, format, &tm) == NULL)
		return NULL;

	time = timegm(&tm);

	localtime_r(&time, &tm);

	strftime(local_time, 16, "%Y%m%dT%H%M%S", &tm);

	return local_time;
}

static struct messages_message *pull_message_data(const char **reply)
{
	struct messages_message *data = g_new0(struct messages_message, 1);

	data->handle = g_strdup(reply[MESSAGE_HANDLE] +
						MESSAGE_HANDLE_PREFIX_LEN);

	if (strlen(reply[MESSAGE_SUBJECT]) != 0)
		data->subject = g_strdup(reply[MESSAGE_SUBJECT]);
	else
		data->subject = g_strdup(reply[MESSAGE_CONTENT]);

	data->mask |= PMASK_SUBJECT;

	if (strlen(reply[MESSAGE_SDATE]) != 0)
		data->datetime = format_tstamp(reply[MESSAGE_SDATE],
						TRACKER_MESSAGE_TSTAMP_FORMAT);
	else if (strlen(reply[MESSAGE_RDATE]) != 0)
		data->datetime = format_tstamp(reply[MESSAGE_RDATE],
						TRACKER_MESSAGE_TSTAMP_FORMAT);
	else
		data->datetime = g_strdup("");

	data->mask |= PMASK_DATETIME;

	data->sent = g_strcmp0(reply[MESSAGE_SENT], "true") == 0 ? TRUE : FALSE;
	data->mask |= PMASK_SENT;

	if (!data->sent) {
		data->sender_name = merge_names(reply[MESSAGE_CONTACT_GIVEN],
						reply[MESSAGE_CONTACT_FAMILY]);
		if (data->sender_name[0] != '\0')
			data->mask |= PMASK_SENDER_NAME;

		data->sender_addressing =
					g_strdup(reply[MESSAGE_CONTACT_PHONE]);
		data->mask |= PMASK_SENDER_ADDRESSING;

		data->recipient_addressing = g_strdup("");
	} else {
		data->recipient_name = merge_names(reply[MESSAGE_CONTACT_GIVEN],
						reply[MESSAGE_CONTACT_FAMILY]);
		if (data->recipient_name[0] != '\0')
			data->mask |= PMASK_RECIPIENT_NAME;

		data->recipient_addressing =
					g_strdup(reply[MESSAGE_CONTACT_PHONE]);
	}
	data->mask |= PMASK_RECIPIENT_ADDRESSING;

	data->type = g_strdup("SMS_GSM");
	data->mask |= PMASK_TYPE;

	data->size = g_strdup_printf("%d", strlen(reply[MESSAGE_CONTENT]));
	data->mask |= PMASK_SIZE;

	data->text = TRUE;
	data->mask |= PMASK_TEXT;

	data->reception_status = g_strdup("complete");
	data->mask |= PMASK_RECEPTION_STATUS;

	data->attachment_size = g_strdup("0");
	data->mask |= PMASK_ATTACHMENT_SIZE;

	data->priority = FALSE;
	data->mask |= PMASK_PRIORITY;

	data->read = g_strcmp0(reply[MESSAGE_READ], "true") == 0 ? TRUE : FALSE;
	data->mask |= PMASK_READ;

	data->protect = FALSE;
	data->mask |= PMASK_PROTECTED;

	return data;
}

static void get_messages_listing_resp(const char **reply, void *user_data)
{
	struct session *session = user_data;
	struct request *request = session->request;
	struct messages_message *msg_data;
	int stat, ihandle, igrp, *group;
	unsigned i;

	DBG("reply %p", reply);

	if (reply == NULL)
		goto end;

	msg_data = pull_message_data(reply);

	ihandle = g_ascii_strtoll(msg_data->handle, NULL, 10);
	igrp = g_ascii_strtoll(reply[MESSAGE_GROUP] + MESSAGE_GRP_PREFIX_LEN,
								NULL, 10);

	for (i = 0; i < msg_grp->len; i++) {
		if (ihandle == g_array_index(msg_grp, int *, i)[1])
			goto already_exists;
	}

	group = g_new0(int, 2);
	group[0] = igrp;
	group[1] = ihandle;
	g_array_append_val(msg_grp, group);

already_exists:
	stat = GPOINTER_TO_INT(g_hash_table_lookup(session->msg_stat,
						GINT_TO_POINTER(ihandle)));
	if (stat == 0) {
		stat = MESSAGE_STAT_EMPTY;
		if(msg_data->read)
			stat |= MESSAGE_STAT_READ;

		if(msg_data->sent)
			stat |= MESSAGE_STAT_SENT;

		g_hash_table_insert(session->msg_stat, GINT_TO_POINTER(ihandle),
							GINT_TO_POINTER(stat));
	} else {
		msg_data->read = stat & MESSAGE_STAT_READ ? TRUE : FALSE;
	}

	if (request->deleted && !(stat & MESSAGE_STAT_DELETED))
		goto done;

	if (!request->deleted && stat & MESSAGE_STAT_DELETED)
		goto done;

	request->size++;

	if (!msg_data->read)
		request->new_message = TRUE;

	if (request->count == TRUE)
		goto done;

	if (request->size > request->offset &&
			filter_message(msg_data, request->filter) &&
			(request->size - request->offset) <= request->max)
		request->cb.messages_list(session, -EAGAIN, 1,
						request->new_message, msg_data,
						request->user_data);

done:
	free_msg_data(msg_data);
	return;

end:
	request->cb.messages_list(session, 0, request->size - request->offset,
						request->new_message, NULL,
						request->user_data);

	g_free(request->filter->period_begin);
	g_free(request->filter->period_end);
	g_free(request->filter->originator);
	g_free(request->filter->recipient);
	g_free(request->filter);

	g_free(request);

	session->request = NULL;
}

static void get_message_resp(const char **reply, void *s)
{
	struct session *session = s;
	struct request *request = session->request;
	struct messages_message *msg_data;
	struct bmsg *bmsg;
	char *final_bmsg, *status, *folder, *handle;
	struct phonebook_contact *contact;
	int err, stat, ihandle;

	DBG("reply %p", reply);

	if (reply == NULL)
		goto done;

	msg_data = pull_message_data(reply);

	contact = pull_message_contact(reply, msg_data->sent);

	ihandle = g_ascii_strtoll(msg_data->handle, NULL, 10);
	stat = GPOINTER_TO_INT(g_hash_table_lookup(session->msg_stat,
						GINT_TO_POINTER(ihandle)));
	if (stat & MESSAGE_STAT_READ)
		msg_data->read = TRUE;
	else if (stat != 0)
		msg_data->read = FALSE;

	status = msg_data->read ? "READ" : "UNREAD";

	if (stat & MESSAGE_STAT_DELETED)
		folder = g_strdup("telecom/msg/deleted");
	else
		folder = message2folder(msg_data);

	handle = fill_handle(msg_data->handle);
	g_free(msg_data->handle);
	msg_data->handle = handle;

	bmsg = g_new0(struct bmsg, 1);
	bmsg_init(bmsg, BMSG_VERSION_1_0, status, BMSG_SMS, folder);

	if (!msg_data->sent)
		bmsg_add_originator(bmsg, contact);

	bmsg_add_envelope(bmsg);

	if (msg_data->sent)
		bmsg_add_recipient(bmsg, contact);

	bmsg_add_content(bmsg, -1, NULL, SMS_DEFAULT_CHARSET, NULL,
						reply[MESSAGE_CONTENT]);

	final_bmsg = bmsg_text(bmsg);

	request->cb.message(session, 0, FALSE, final_bmsg, request->user_data);

	bmsg_destroy(bmsg);
	g_free(folder);
	g_free(final_bmsg);
	free_msg_data(msg_data);
	phonebook_contact_free(contact);

	request->count++;

	return;

done:
	if (request->count == 0)
		err = -ENOENT;
	else
		err = 0;

	request->cb.message(session, err, FALSE, NULL, request->user_data);

	g_free(request->name);
	g_free(request);

	session->request = NULL;
}

static void session_dispatch_event(struct session *session,
						struct messages_event *event)
{
	int ihandle, stat, direction;

	if (event->type == MET_MESSAGE_DELETED) {
		ihandle = g_ascii_strtoll(event->handle, NULL, 10);
		if (!g_hash_table_lookup_extended(session->msg_stat,
						GINT_TO_POINTER(ihandle),
						NULL, (gpointer) &stat))
			return;

		direction = DIRECTION_INBOUND;
		if (stat & MESSAGE_STAT_SENT)
			direction = DIRECTION_OUTBOUND;

		switch (direction) {
		case DIRECTION_INBOUND:
			event->folder = g_strdup("telecom/msg/inbox");
			break;
		case DIRECTION_OUTBOUND:
			event->folder = g_strdup("telecom/msg/sent");
			break;
		default:
			event->folder = g_strdup("");
			break;
		}
	}

	if (session->op_in_progress) {
		struct messages_event *data;

		messages_event_ref(data);
		session->mns_event_cache = g_slist_append(
						session->mns_event_cache,
						data);

		DBG("Event cached");
	} else {
		session->event_cb(session, event, session->event_user_data);

		DBG("Event dispatched");
	}
}

static void notify_new_sms(const char *handle, enum messages_event_type type)
{
	struct messages_event *data;
	GSList *next;
	char *folder;

	DBG("");

	if (type == MET_NEW_MESSAGE)
		folder = "telecom/msg/inbox";
	else if (type == MET_MESSAGE_DELETED)
		folder = NULL;
	else
		folder = "";

	data = messages_event_new(type, BMSG_T_SMS_GSM, handle, folder, "");

	for (next = mns_srv; next != NULL; next = g_slist_next(next)) {
		struct session *session = next->data;

		session_dispatch_event(session, data);
	}

	messages_event_unref(data);
}

static void notify_cached_events(struct session *session, const char *h)
{
	GSList *event;
	char *handle = fill_handle(h);

	DBG("");

	for (event = session->mns_event_cache; event != NULL;
						event = g_slist_next(event)) {
		struct messages_event *data = event->data;

		if (handle != NULL && g_strcmp0(handle, data->handle) == 0) {
			messages_event_unref(data);

			continue;
		}

		if (g_slist_find(mns_srv, session) != NULL)
			session->event_cb(session, data,
						session->event_user_data);

		messages_event_unref(data);
	}

	g_slist_free(session->mns_event_cache);
	session->mns_event_cache = NULL;
	g_free(handle);
}

static gboolean handle_new_sms(DBusConnection * connection, DBusMessage * msg,
							void *user_data)
{
	DBusMessageIter arg, inner_arg, struct_arg;
	unsigned ihandle = 0;
	int32_t direction;
	char *handle;

	DBG("");

	if (!dbus_message_iter_init(msg, &arg))
		return TRUE;

	if (dbus_message_iter_get_arg_type(&arg) != DBUS_TYPE_ARRAY)
		return TRUE;

	dbus_message_iter_recurse(&arg, &inner_arg);

	for ( ; dbus_message_iter_get_arg_type(&inner_arg) != DBUS_TYPE_INVALID;
					dbus_message_iter_next(&inner_arg)) {

		if (dbus_message_iter_get_arg_type(&inner_arg)
							!= DBUS_TYPE_STRUCT)
			continue;

		dbus_message_iter_recurse(&inner_arg, &struct_arg);

		if (dbus_message_iter_get_arg_type(&struct_arg) !=
							DBUS_TYPE_INT32)
			continue;

		dbus_message_iter_get_basic(&struct_arg, &ihandle);

		handle = g_strdup_printf("%d", ihandle);

		dbus_message_iter_next(&struct_arg); /* Type */
		dbus_message_iter_next(&struct_arg); /* StartTime */
		dbus_message_iter_next(&struct_arg); /* EndTime */
		dbus_message_iter_next(&struct_arg); /* Direction */

		if (dbus_message_iter_get_arg_type(&struct_arg) !=
								DBUS_TYPE_INT32)
			continue;

		dbus_message_iter_get_basic(&struct_arg, &direction);

		if (direction == DIRECTION_OUTBOUND)
			goto done;

		DBG("new message: %s", handle);

		notify_new_sms(handle, MET_NEW_MESSAGE);

done:
		g_free(handle);
	}

	return TRUE;
}

static void notify_del_sms(const char *handle)
{
	unsigned i;
	int ihandle = g_ascii_strtoll(handle, NULL, 10);

	notify_new_sms(handle, MET_MESSAGE_DELETED);

	for (i = 0; i < msg_grp->len; i++) {
		int *data = g_array_index(msg_grp, int *, i);

		if (ihandle == data[1]) {
			g_free(data);
			msg_grp = g_array_remove_index_fast(msg_grp, i);

			break;
		}
	}
}

static gboolean handle_del_sms(DBusConnection * connection, DBusMessage * msg,
							void *user_data)
{
	DBusMessageIter arg;
	int ihandle;
	char *handle;

	DBG("");

	if (!dbus_message_iter_init(msg, &arg))
		return TRUE;

	if (dbus_message_iter_get_arg_type(&arg) != DBUS_TYPE_INT32)
		return TRUE;

	dbus_message_iter_get_basic(&arg, &ihandle);

	handle = g_strdup_printf("%d", ihandle);

	DBG("message deleted: %s", handle);

	notify_del_sms(handle);

	g_free(handle);

	return TRUE;
}

static gboolean handle_del_grp(DBusConnection * connection, DBusMessage * msg,
							void *user_data)
{
	DBusMessageIter arg, array;

	DBG("");

	if (!dbus_message_iter_init(msg, &arg))
		return TRUE;

	if (dbus_message_iter_get_arg_type(&arg) != DBUS_TYPE_ARRAY)
		return TRUE;

	dbus_message_iter_recurse(&arg, &array);

	for ( ; dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_INVALID;
					dbus_message_iter_next(&array)) {
		int32_t grp;
		unsigned i;

		if (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_INT32)
			return TRUE;

		dbus_message_iter_get_basic(&array, &grp);

		DBG("Group deleted: %d, searching for messages", grp);

		for (i = 0; i < msg_grp->len; i++) {
			int *data = g_array_index(msg_grp, int *, i);

			if (grp == data[0]) {
				char *handle = g_strdup_printf("%d", data[1]);

				DBG("message deleted: %s", handle);

				notify_new_sms(handle, MET_MESSAGE_DELETED);

				g_free(data);
				msg_grp = g_array_remove_index_fast(msg_grp, i);
				/* last element becomes i-th */
				i--;

				g_free(handle);
			}
		}
	}

	return TRUE;
}

static int retrieve_message_id_tracker_id(void)
{
	DBusMessage *msg;
	DBusMessage *reply;
	DBusMessageIter iargs, irows, icols;
	char *id;
	char *query = "SELECT tracker:id(nmo:messageId) {}";

	msg = dbus_message_new_method_call(TRACKER_SERVICE,
						TRACKER_RESOURCES_PATH,
						TRACKER_RESOURCES_INTERFACE,
						"SparqlQuery");
	if (msg == NULL)
		goto failed;

	if (!dbus_message_append_args(msg, DBUS_TYPE_STRING,
					&query,
					DBUS_TYPE_INVALID))
		goto failed;

	reply = dbus_connection_send_with_reply_and_block(session_connection,
								msg, -1, NULL);
	if (reply == NULL)
		goto failed;

	if (!dbus_message_iter_init(reply, &iargs))
		goto failed;

	if (dbus_message_iter_get_arg_type(&iargs) != DBUS_TYPE_ARRAY)
		goto failed;

	dbus_message_iter_recurse(&iargs, &irows);
	if (dbus_message_iter_get_arg_type(&irows) == DBUS_TYPE_ARRAY) {
		dbus_message_iter_recurse(&irows, &icols);
		dbus_message_iter_get_basic(&icols, &id);
		message_id_tracker_id = strtoul(id, NULL, 10);
		DBG("tracker:id(nmo:messageId): %lu", message_id_tracker_id);
	} else {
		goto failed;
	}

	dbus_message_unref(reply);
	dbus_message_unref(msg);

	return 0;

failed:
	DBG("Unable to get tracker.id(nmo:messageId)!");

	if (reply != NULL)
		dbus_message_unref(reply);
	if (msg != NULL)
		dbus_message_unref(msg);

	return -ENOENT;
}

int messages_init(void)
{
	session_connection = dbus_bus_get(DBUS_BUS_SESSION, NULL);
	if (session_connection == NULL) {
		error("Unable to connect to the session bus.");

		return -1;
	}

	if (retrieve_message_id_tracker_id() < 0)
		return -1;

	msg_grp = g_array_new(FALSE, FALSE, sizeof(int *));

	messages_qt_init();

	create_folder_tree();

	return 0;
}

void messages_exit(void)
{
	unsigned i;

	destroy_folder_tree(folder_tree);

	dbus_connection_unref(session_connection);

	for (i = 0; i < msg_grp->len; i++)
		g_free(g_array_index(msg_grp, int *, i));

	g_array_free(msg_grp, TRUE);

	messages_qt_exit();
}

int messages_connect(void **s)
{
	struct session *session = g_new0(struct session, 1);

	session->cwd = g_strdup("/");
	session->folder = folder_tree;

	session->msg_stat = g_hash_table_new(NULL, NULL);

	*s = session;

	return 0;
}

void messages_disconnect(void *s)
{
	struct session *session = s;

	messages_set_notification_registration(session, NULL, NULL);

	messages_abort(session);

	g_hash_table_destroy(session->msg_stat);
	g_free(session->cwd);
	g_free(session);
}

int messages_set_notification_registration(void *s, messages_event_cb cb,
							void *user_data)
{
	struct session *session = s;

	if (cb != NULL) {
		if (g_slist_find(mns_srv, session) != NULL)
			return 0;

		if (g_slist_length(mns_srv) == 0) {
			newmsg_watch_id = g_dbus_add_signal_watch(
							session_connection,
							NULL, NULL,
							"com.nokia.commhistory",
							"eventsAdded",
							handle_new_sms,
							NULL, NULL);
			delmsg_watch_id = g_dbus_add_signal_watch(
							session_connection,
							NULL, NULL,
							"com.nokia.commhistory",
							"eventDeleted",
							handle_del_sms,
							NULL, NULL);
			delgrp_watch_id = g_dbus_add_signal_watch(
							session_connection,
							NULL, NULL,
							"com.nokia.commhistory",
							"groupsDeleted",
							handle_del_grp,
							NULL, NULL);
		}
		if (newmsg_watch_id == 0 || delmsg_watch_id == 0 ||
							delgrp_watch_id == 0)
			return -EIO;

		session->event_user_data = user_data;
		session->event_cb = cb;

		mns_srv = g_slist_prepend(mns_srv, session);
	} else {
		mns_srv = g_slist_remove(mns_srv, session);

		if (g_slist_length(mns_srv) == 0) {
			g_dbus_remove_watch(session_connection,
							newmsg_watch_id);
			g_dbus_remove_watch(session_connection,
							delmsg_watch_id);
			g_dbus_remove_watch(session_connection,
							delgrp_watch_id);
		}
	}

	return 0;
}

int messages_set_folder(void *s, const char *name, gboolean cdup)
{
	struct session *session = s;
	char *newrel = NULL;
	char *newabs;
	char *tmp;

	if (name && (strchr(name, '/') || strcmp(name, "..") == 0))
		return -EBADR;

	if (cdup) {
		if (session->cwd[0] == 0)
			return -ENOENT;

		newrel = g_path_get_dirname(session->cwd);

		/* We use empty string for indication of the root directory */
		if (newrel[0] == '.' && newrel[1] == 0)
			newrel[0] = 0;
	}

	tmp = newrel;
	if (!cdup && (!name || name[0] == 0))
		newrel = g_strdup("");
	else
		newrel = g_build_filename(newrel ? newrel : session->cwd, name,
									NULL);
	g_free(tmp);

	if (newrel[0] != '/')
		newabs = g_build_filename("/", newrel, NULL);
	else
		newabs = g_strdup(newrel);

	session->folder = get_folder(newabs);
	if (session->folder == NULL) {
		g_free(newrel);
		g_free(newabs);

		return -ENOENT;
	}

	g_free(newrel);
	g_free(session->cwd);
	session->cwd = newabs;

	return 0;
}

int messages_get_folder_listing(void *s, const char *name,
					uint16_t max, uint16_t offset,
					messages_folder_listing_cb callback,
					void *user_data)
{
	struct session *session = s;
	gboolean count = FALSE;
	int folder_count = 0;
	char *path = NULL;
	struct message_folder *folder;
	GSList *dir;

	if (name && strchr(name, '/') != NULL)
		goto done;

	path = g_build_filename(session->cwd, name, NULL);

	if (path == NULL || strlen(path) == 0)
		goto done;

	folder = get_folder(path);

	if (folder == NULL)
		goto done;

	if (max == 0) {
		max = 0xffff;
		offset = 0;
		count = TRUE;
	}

	for (dir = folder->subfolders; dir &&
				(folder_count - offset) < max;
				folder_count++, dir = g_slist_next(dir)) {
		struct message_folder *dir_data = dir->data;

		if (count == FALSE && offset <= folder_count)
			callback(session, -EAGAIN, 1, dir_data->name,
								user_data);
	}

done:
	callback(session, 0, folder_count, NULL, user_data);

	g_free(path);

	return 0;
}

int messages_get_messages_listing(void *s, const char *name,
				uint16_t max, uint16_t offset,
				const struct messages_filter *filter,
				messages_get_messages_listing_cb callback,
				void *user_data)
{
	struct session *session = s;
	struct request *request;
	char *path, *query;
	struct message_folder *folder = NULL;
	int err = 0;

	if (name == NULL || strlen(name) == 0) {
		path = g_strdup(session->cwd);

		folder = session->folder;
		if (folder == NULL)
			folder = get_folder(path);
	} else {
		if (strchr(name, '/') != NULL)
			return -EBADR;

		path = g_build_filename(session->cwd, name, NULL);
		folder = get_folder(path);
	}

	g_free(path);

	if (folder == NULL)
		return -EBADR;

	query = folder2query(folder, LIST_MESSAGES_QUERY);
	if (query == NULL)
		return -ENOENT;

	request = g_new0(struct request, 1);

	request->filter = copy_messages_filter(filter);
	request->generate_response = get_messages_listing_resp;
	request->cb.messages_list = callback;
	request->offset = offset;
	request->max = max;
	request->user_data = user_data;
	request->deleted = g_strcmp0(folder->name, "deleted") ? FALSE : TRUE;

	session->request = request;

	if (max == 0) {
		request->max = 0xffff;
		request->offset = 0;
		request->count = TRUE;
	}

	request->pc = query_tracker(query, session, &err);

	g_free(query);

	return err;
}

int messages_get_message(void *s, const char *h, unsigned long flags,
				messages_get_message_cb cb, void *user_data)
{
	struct session *session = s;
	struct request *request;
	int err = 0;
	char *handle, *query_handle, *query;

	if (!validate_handle(h))
		return -ENOENT;

	handle = strip_handle(h);
	query_handle = g_strdup_printf(MESSAGES_FILTER_BY_HANDLE, handle);
	query = path2query("telecom/msg", LIST_MESSAGES_QUERY, query_handle);

	if (query == NULL) {
		err = -ENOENT;

		goto failed;
	}

	if (flags & MESSAGES_FRACTION && flags & MESSAGES_NEXT) {
		err = -EBADR;

		goto failed;
	}

	request = g_new0(struct request, 1);

	request->name = g_strdup(handle);
	request->flags = flags;
	request->cb.message = cb;
	request->generate_response = get_message_resp;
	request->user_data = user_data;

	session->request = request;

	request->pc = query_tracker(query, session, &err);

failed:
	g_free(query_handle);
	g_free(handle);
	g_free(query);

	return err;
}

static void messages_qt_callback(int err, void *user_data)
{
	struct session *session = user_data;
	struct request *request = session->request;

	if (request->cb.status)
		request->cb.status(session, err, request->user_data);

	g_free(request);
	session->request = NULL;
}

int messages_set_message_status(void *s, const char *handle, uint8_t indicator,
			uint8_t value, messages_set_message_status_cb callback,
			void *user_data)
{
	struct session *session = s;
	struct request *request;
	int ret, ihandle, stat;

	ihandle = g_ascii_strtoll(handle, NULL, 10);
	stat = GPOINTER_TO_INT(g_hash_table_lookup(session->msg_stat,
						GINT_TO_POINTER(ihandle)));
	if (stat == 0) {
		stat = MESSAGE_STAT_EMPTY;
		g_hash_table_insert(session->msg_stat, GINT_TO_POINTER(ihandle),
							GINT_TO_POINTER(stat));
	}

	request = g_new0(struct request, 1);
	request->cb.status = callback;
	request->user_data = user_data;
	session->request = request;

	switch (indicator) {
	case 0x0:
		ret = messages_qt_set_read(&request->set_status_call,
						handle, value & 0x01,
						messages_qt_callback, session);
		if (ret < 0)
			return ret;

		if(value == 1)
			stat |= MESSAGE_STAT_READ;
		else if(value == 0)
			stat &= ~MESSAGE_STAT_READ;

		break;
	case 0x1:
		session->op_in_progress = TRUE;

		ret = messages_qt_set_deleted(&request->set_status_call,
						handle, value & 0x01,
						messages_qt_callback, session);
		if (ret < 0)
			return ret;

		notify_del_sms(handle);

		session->op_in_progress = FALSE;
		notify_cached_events(session, handle);

		if(value == 1)
			stat |= MESSAGE_STAT_DELETED;
		else if(value == 0)
			stat &= ~MESSAGE_STAT_DELETED;

		break;
	default:
		g_free(request);
		session->request = NULL;
		return -EBADR;
	}

	g_hash_table_insert(session->msg_stat, GINT_TO_POINTER(ihandle),
							GINT_TO_POINTER(stat));

	return 0;
}

static void push_message_abort(gpointer s)
{
	struct session *session = s;
	struct push_message_request *request = session->request_data;

	DBG("");

	if (request == NULL)
		return;

	if (request->send_sms != NULL) {
		dbus_pending_call_cancel(request->send_sms);
		dbus_pending_call_unref(request->send_sms);
	}

	if (request->get_handle != NULL) {
		DBG("Cancelled get_handle");
		dbus_pending_call_cancel(request->get_handle);
		dbus_pending_call_unref(request->get_handle);
	}

	if (request->insert_message_call != NULL)
		messages_qt_insert_message_abort(request->insert_message_call);

	g_dbus_remove_watch(session_connection, request->watch);

	if (request->body)
		g_string_free(request->body, TRUE);

	if (request->name)
		g_free(request->name);

	g_free(request->uuid);
	g_free(request);

	session->op_in_progress = FALSE;
	notify_cached_events(session, NULL);
}

static void push_message_finalize(struct session *session)
{
	DBG("");

	push_message_abort(session);
	session->request_data = NULL;
	session->abort_request = NULL;
}

static void send_sms_finalize(struct session *session, const char *uri)
{
	struct push_message_request *request = session->request_data;
	char handle[HANDLE_LEN];
	unsigned long uri_no;

	if (sscanf(uri, "message:%lu", &uri_no) != 1)
		request->cb(session, -EIO, NULL, request->user_data);
	else {
		snprintf(handle, HANDLE_LEN, "%016lu", uri_no);
		request->cb(session, 0, handle, request->user_data);
	}

	notify_cached_events(session, handle);
	push_message_finalize(session);
}

static int get_uri_by_uuid(void *s, const char *id);

static void get_uri_by_uuid_pc(DBusPendingCall *pc, void *user_data)
{
	struct session *session = user_data;
	struct push_message_request *request = session->request_data;
	DBusMessage *reply;
	DBusError error;
	DBusMessageIter iargs, irows, icols;
	char *uri;

	reply = dbus_pending_call_steal_reply(pc);
	dbus_error_init(&error);

	if (dbus_set_error_from_message(&error, reply)) {
		DBG("%s: %s", error.name, error.message);
		dbus_error_free(&error);

		goto cont;
	}

	if (!dbus_message_has_signature(reply, "aas")) {
		DBG("Unexpected signature: %s",
					dbus_message_get_signature(reply));

		goto cont;
	}

	dbus_message_iter_init(reply, &iargs);
	dbus_message_iter_recurse(&iargs, &irows);

	if (dbus_message_iter_get_arg_type(&irows) != DBUS_TYPE_ARRAY) {
		DBG("Message not yet in Tracker");
		goto cont;
	}

	dbus_message_iter_recurse(&irows, &icols);
	dbus_message_iter_get_basic(&icols, &uri);
	DBG("URI: %s", uri);
	dbus_message_unref(reply);
	send_sms_finalize(session, uri);

	return;

cont:
	dbus_message_unref(reply);
	dbus_pending_call_unref(request->get_handle);
	request->get_handle = NULL;

	if (request->retry) {
		request->retry = FALSE;
		get_uri_by_uuid(session, request->uuid);
	}
}

static int get_uri_by_uuid(void *s, const char *id)
{
	struct session *session = s;
	struct push_message_request *request = session->request_data;
	DBusMessage *msg;
	DBusPendingCall *pc;
	char *query;

	if (request->get_handle != NULL) {
		request->retry = TRUE;
		return 0;
	}

	msg = dbus_message_new_method_call(TRACKER_SERVICE,
						TRACKER_RESOURCES_PATH,
						TRACKER_RESOURCES_INTERFACE,
						"SparqlQuery");
	if (msg == NULL)
		goto failed;

	query = g_strdup_printf(HANDLE_BY_UUID_QUERY, id);
	if (!dbus_message_append_args(msg, DBUS_TYPE_STRING, &query,
							DBUS_TYPE_INVALID)) {
		g_free(query);
		goto failed;
	}
	g_free(query);

	if (!dbus_connection_send_with_reply(session_connection, msg, &pc, -1))
		goto failed;

	if (!dbus_pending_call_set_notify(pc, get_uri_by_uuid_pc, session,
									NULL)) {
		dbus_pending_call_cancel(pc);
		goto failed;
	}

	dbus_message_unref(msg);
	request->get_handle = pc;

	return 0;

failed:
	if (pc != NULL)
		dbus_pending_call_unref(pc);

	if (msg != NULL)
		dbus_message_unref(msg);

	push_message_finalize(session);

	return -ENOMEM;
}

static gboolean send_sms_graph_updated(DBusConnection *connection,
					DBusMessage *msg, void *user_data)
{
	struct session *session = user_data;
	struct push_message_request *request = session->request_data;
	DBusMessageIter iargs, irows, icols;
	char *class;
	dbus_uint32_t predicate;

	dbus_message_iter_init(msg, &iargs);
	if (dbus_message_iter_get_arg_type(&iargs) != DBUS_TYPE_STRING)
		return TRUE;

	dbus_message_iter_get_basic(&iargs, &class);
	if (g_strcmp0("http://www.semanticdesktop.org/ontologies"
					"/2007/03/22/nmo#Message", class) != 0)
		return TRUE;

	dbus_message_iter_next(&iargs);
	if (dbus_message_iter_get_arg_type(&iargs) != DBUS_TYPE_ARRAY)
		return TRUE;

	dbus_message_iter_recurse(&iargs, &irows);

	while (dbus_message_iter_get_arg_type(&irows) == DBUS_TYPE_STRUCT) {
		dbus_message_iter_recurse(&irows, &icols);
		dbus_message_iter_next(&icols);
		dbus_message_iter_next(&icols);
		dbus_message_iter_get_basic(&icols, &predicate);
		if (predicate == message_id_tracker_id) {
			DBG("Predicate hit!");
			get_uri_by_uuid(session, request->uuid);
		}
		dbus_message_iter_next(&irows);
	}

	return TRUE;
}

static void send_sms_messaging_pc(DBusPendingCall *pc, void *user_data)
{
	struct session *session = user_data;
	struct push_message_request *request = session->request_data;
	DBusMessage *reply;
	DBusError error;
	char *uuid;
	int err;

	DBG("");

	reply = dbus_pending_call_steal_reply(pc);
	dbus_error_init(&error);

	if (dbus_set_error_from_message(&error, reply)) {
		DBG("%s: %s", error.name, error.message);
		dbus_error_free(&error);
		err = -EIO;

		goto failed;
	}

	if (!dbus_message_has_signature(reply, DBUS_TYPE_STRING_AS_STRING)) {
		DBG("Unexpected signature: %s",
					dbus_message_get_signature(reply));
		err = -EIO;

		goto failed;
	}

	dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &uuid,
							DBUS_TYPE_INVALID);
	if (uuid[0] == '\0') {
		DBG("Empty response from SendSMS, possibly wrong phone number");
		err = -EBADR;

		goto failed;
	}

	DBG("Message UUID: %s", uuid);
	request->uuid = g_strdup(uuid);

	request->watch = g_dbus_add_signal_watch(session_connection,
					NULL, NULL,
					"org.freedesktop.Tracker1.Resources",
					"GraphUpdated",
					send_sms_graph_updated,
					session, NULL);
	get_uri_by_uuid(session, uuid);

	dbus_message_unref(reply);
	dbus_pending_call_unref(request->send_sms);
	request->send_sms = NULL;

	return;

failed:
	request->cb(session, err, NULL, request->user_data);
	push_message_finalize(session);
	dbus_message_unref(reply);
}

static int send_sms(struct session *session, const char *recipient,
					const char *body, gboolean store)
{
	struct push_message_request *request = session->request_data;
	DBusMessage *msg = NULL;
	DBusPendingCall *pc = NULL;

	msg = dbus_message_new_method_call("com.nokia.Messaging", "/",
					"com.nokia.MessagingIf", "sendSMS");
	if (msg == NULL)
		goto failed;

	if (!dbus_message_append_args(msg, DBUS_TYPE_STRING, &recipient,
						DBUS_TYPE_STRING, &body,
						DBUS_TYPE_BOOLEAN, &store,
						DBUS_TYPE_INVALID))
		goto failed;

	if (!dbus_connection_send_with_reply(session_connection, msg,
							&pc, -1))
		goto failed;

	if (!dbus_pending_call_set_notify(pc, send_sms_messaging_pc,
							session, NULL)) {
		/* XXX: Now, that's kind of a problem */
		dbus_pending_call_cancel(pc);
		goto failed;
	}

	dbus_message_unref(msg);
	request->send_sms = pc;

	return 0;

failed:
	if (pc != NULL)
		dbus_pending_call_unref(pc);
	if (msg != NULL)
		dbus_message_unref(msg);

	return -ENOMEM;
}

static void insert_message_cb(int id, void *s)
{
	struct session *session = s;
	struct push_message_request *request = session->request_data;
	char handle[HANDLE_LEN];

	DBG("");

	if (id < 0) {
		request->cb(session, id, NULL, request->user_data);
		goto finalize;
	}

	request->insert_message_call = NULL;

	snprintf(handle, HANDLE_LEN, "%016d", id);
	request->cb(session, 0, handle, request->user_data);

	notify_cached_events(session, handle);

finalize:
	push_message_finalize(session);
}

static int store_sms(struct session *session, const char *recipient,
					const char *body)
{
	struct push_message_request *request = session->request_data;

	DBG("");

	return messages_qt_insert_message(&request->insert_message_call,
						recipient, body, request->name,
						insert_message_cb, session);
}

int messages_push_message(void *s, struct bmsg_bmsg *bmsg, const char *name,
						unsigned long flags,
						messages_push_message_cb cb,
						void *user_data)
{
	struct session *session = s;
	struct push_message_request *request;

	session->op_in_progress = TRUE;

	if ((flags & MESSAGES_UTF8) != MESSAGES_UTF8) {
		DBG("Tried to push non-utf message");
		return -EINVAL;
	}

	if ((flags & MESSAGES_TRANSPARENT) == MESSAGES_TRANSPARENT)
		DBG("Warning! Transparent flag is ignored");

	if ((flags & MESSAGES_RETRY) != MESSAGES_RETRY)
		DBG("Warning! Retry flag is ignored.");

	if (bmsg->type != BMSG_T_SMS_GSM ||
					bmsg->charset != BMSG_C_UTF8 ||
					bmsg->part_id != -1 ||
					bmsg->nenvelopes < 1) {
		DBG("Incorrect BMSG format!");
		return -EBADR;
	}

	request = g_new0(struct push_message_request, 1);
	session->request_data = request;
	session->abort_request = push_message_abort;

	request->bmsg = bmsg;
	request->cb = cb;
	request->body = g_string_new("");
	request->user_data = user_data;

	request->name = g_build_filename(session->cwd, name, NULL);
	DBG("Push destination: %s", request->name);

	return 0;
}

static int prepare_body(GString *body)
{
	if (body->len < MSG_BLOCK_OVERHEAD)
		return -EBADR;

	if (!g_str_has_prefix(body->str, "BEGIN:MSG\r\n"))
		return -EBADR;

	if (!g_str_has_prefix(body->str + body->len - 11, "\r\nEND:MSG\r\n"))
		return -EBADR;

	g_string_erase(body, 0, 11);
	g_string_set_size(body, body->len - 10);
	body->str[body->len - 1] = '\0';

	return 0;
}

int messages_push_message_body(void *s, const char *body, size_t len)
{
	struct session *session = s;
	struct push_message_request *request = session->request_data;
	struct bmsg_bmsg_vcard *vcard;
	int env, ret;

	if (len > 0) {
		g_string_append_len(request->body, body, len);
		return len;
	}

	env = request->bmsg->nenvelopes - 1;
	if (env < 0 || request->bmsg->recipients[env] == NULL)
	{
		ret = -EBADR;
		goto failed;
	}

	if (request->bmsg->recipients[env]->next != NULL)
	{
		ret = -EINVAL;
		goto failed;
	}

	vcard = request->bmsg->recipients[request->bmsg->nenvelopes - 1]->data;
	if (vcard->tel == NULL) {
		ret = -EBADR;
		goto failed;
	}

	ret = prepare_body(request->body);
	if (ret < 0)
		goto failed;

	if (g_strcmp0(request->name, "/telecom/msg/outbox") == 0)
		ret = send_sms(session, vcard->tel, request->body->str, TRUE);
	else
		ret = store_sms(session, vcard->tel, request->body->str);

	if (ret < 0)
		goto failed;

	g_string_free(request->body, TRUE);
	request->body = NULL;

	return 0;

failed:
	push_message_finalize(session);

	return ret;
}

void messages_abort(void *s)
{
	struct session *session = s;

	DBG("");

	if (session->abort_request != NULL)
		session->abort_request(session);

	if (session->request != NULL && session->request->pc != NULL) {
		dbus_pending_call_cancel(session->request->pc);
		free_request(session->request);
	}

	session->abort_request = NULL;
	session->request_data = NULL;
}
