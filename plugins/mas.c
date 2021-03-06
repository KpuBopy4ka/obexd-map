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

#include <errno.h>
#include <string.h>
#include <glib.h>
#include <openobex/obex.h>
#include <fcntl.h>

#include "plugin.h"
#include "log.h"
#include "obex.h"
#include "service.h"
#include "mimetype.h"
#include "filesystem.h"
#include "dbus.h"

#include "messages.h"
#include "bmsg_parser.h"

/* Channel number according to bluez doc/assigned-numbers.txt */
#define MAS_CHANNEL	16

#define MAS_RECORD "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>		\
<record>								\
  <attribute id=\"0x0001\">						\
    <sequence>								\
      <uuid value=\"0x1132\"/>						\
    </sequence>								\
  </attribute>								\
									\
  <attribute id=\"0x0004\">						\
    <sequence>								\
      <sequence>							\
        <uuid value=\"0x0100\"/>					\
      </sequence>							\
      <sequence>							\
        <uuid value=\"0x0003\"/>					\
        <uint8 value=\"%u\" name=\"channel\"/>				\
      </sequence>							\
      <sequence>							\
        <uuid value=\"0x0008\"/>					\
      </sequence>							\
    </sequence>								\
  </attribute>								\
									\
  <attribute id=\"0x0009\">						\
    <sequence>								\
      <sequence>							\
        <uuid value=\"0x1134\"/>					\
        <uint16 value=\"0x0100\" name=\"version\"/>			\
      </sequence>							\
    </sequence>								\
  </attribute>								\
									\
  <attribute id=\"0x0100\">						\
    <text value=\"%s\" name=\"name\"/>					\
  </attribute>								\
									\
  <attribute id=\"0x0315\">						\
    <uint8 value=\"0x00\"/>						\
  </attribute>								\
									\
  <attribute id=\"0x0316\">						\
    <uint8 value=\"0x02\"/>						\
  </attribute>								\
</record>"

#define XML_DECL "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"

/* Building blocks for x-obex/folder-listing */
#define FL_DTD "<!DOCTYPE folder-listing SYSTEM \"obex-folder-listing.dtd\">"
#define FL_BODY_BEGIN "<folder-listing version=\"1.0\">"
#define FL_BODY_EMPTY "<folder-listing version=\"1.0\"/>"
#define FL_PARENT_FOLDER_ELEMENT "<parent-folder/>"
#define FL_FOLDER_ELEMENT "<folder name=\"%s\"/>"
#define FL_BODY_END "</folder-listing>"

#define ML_BODY_BEGIN "<MAP-msg-listing version=\"1.0\">"
#define ML_BODY_END "</MAP-msg-listing>"

/* Tags needed to retrieve and set application parameters */
enum aparam_tag {
	MAXLISTCOUNT_TAG	= 0x01,
	STARTOFFSET_TAG		= 0x02,
	FILTERMESSAGETYPE_TAG	= 0x03,
	FILTERPERIODBEGIN_TAG	= 0x04,
	FILTERPERIODEND_TAG	= 0x05,
	FILTERREADSTATUS_TAG	= 0x06,
	FILTERRECIPIENT_TAG	= 0x07,
	FILTERORIGINATOR_TAG	= 0x08,
	FILTERPRIORITY_TAG	= 0x09,
	ATTACHMENT_TAG		= 0x0A,
	TRANSPARENT_TAG		= 0x0B,
	RETRY_TAG		= 0x0C,
	NEWMESSAGE_TAG		= 0x0D,
	NOTIFICATIONSTATUS_TAG	= 0x0E,
	MASINSTANCEID_TAG	= 0x0F,
	PARAMETERMASK_TAG	= 0x10,
	FOLDERLISTINGSIZE_TAG	= 0x11,
	MESSAGESLISTINGSIZE_TAG	= 0x12,
	SUBJECTLENGTH_TAG	= 0x13,
	CHARSET_TAG		= 0x14,
	FRACTIONREQUEST_TAG	= 0x15,
	FRACTIONDELIVER_TAG	= 0x16,
	STATUSINDICATOR_TAG	= 0x17,
	STATUSVALUE_TAG		= 0x18,
	MSETIME_TAG		= 0x19,
	INVALID_TAG		= 0x100,
};

enum aparam_type {
	APT_UINT8,
	APT_UINT16,
	APT_UINT32,
	APT_STR
};

static const struct aparam_def {
	enum aparam_tag tag;
	const char *name;
	enum aparam_type type;
} aparam_defs[] = {
	{ MAXLISTCOUNT_TAG,		"MAXLISTCOUNT",
		APT_UINT16					},
	{ STARTOFFSET_TAG,		"STARTOFFSET",
		APT_UINT16					},
	{ FILTERMESSAGETYPE_TAG,	"FILTERMESSAGETYPE",
		APT_UINT8					},
	{ FILTERPERIODBEGIN_TAG,	"FILTERPERIODBEGIN",
		APT_STR						},
	{ FILTERPERIODEND_TAG,		"FILTERPERIODEND",
		APT_STR						},
	{ FILTERREADSTATUS_TAG,		"FILTERREADSTATUS",
		APT_UINT8					},
	{ FILTERRECIPIENT_TAG,		"FILTERRECIPIENT",
		APT_STR						},
	{ FILTERORIGINATOR_TAG,		"FILTERORIGINATOR",
		APT_STR						},
	{ FILTERPRIORITY_TAG,		"FILTERPRIORITY",
		APT_UINT8					},
	{ ATTACHMENT_TAG,		"ATTACHMENT",
		APT_UINT8					},
	{ TRANSPARENT_TAG,		"TRANSPARENT",
		APT_UINT8					},
	{ RETRY_TAG,			"RETRY",
		APT_UINT8					},
	{ NEWMESSAGE_TAG,		"NEWMESSAGE",
		APT_UINT8					},
	{ NOTIFICATIONSTATUS_TAG,	"NOTIFICATIONSTATUS",
		APT_UINT8					},
	{ MASINSTANCEID_TAG,		"MASINSTANCEID",
		APT_UINT8					},
	{ PARAMETERMASK_TAG,		"PARAMETERMASK",
		APT_UINT32					},
	{ FOLDERLISTINGSIZE_TAG,	"FOLDERLISTINGSIZE",
		APT_UINT16					},
	{ MESSAGESLISTINGSIZE_TAG,	"MESSAGESLISTINGSIZE",
		APT_UINT16					},
	{ SUBJECTLENGTH_TAG,		"SUBJECTLENGTH",
		APT_UINT8					},
	{ CHARSET_TAG,			"CHARSET",
		APT_UINT8					},
	{ FRACTIONREQUEST_TAG,		"FRACTIONREQUEST",
		APT_UINT8					},
	{ FRACTIONDELIVER_TAG,		"FRACTIONDELIVER",
		APT_UINT8					},
	{ STATUSINDICATOR_TAG,		"STATUSINDICATOR",
		APT_UINT8					},
	{ STATUSVALUE_TAG,		"STATUSVALUE",
		APT_UINT8					},
	{ MSETIME_TAG,			"MSETIME",
		APT_STR						},
	{ INVALID_TAG,			NULL,
		0						},
};

struct aparam_entry {
	enum aparam_tag tag;
	union {
		uint32_t val32u;
		uint16_t val16u;
		uint8_t val8u;
		char *valstr;
	};
};

/* This comes from OBEX specs */
struct aparam_header {
	uint8_t tag;
	uint8_t len;
	uint8_t val[0];
} __attribute__ ((packed));

struct mas_session {
	char *remote_addr;
	void *backend_data;
	gboolean ap_sent;
	gboolean finished;
	GString *buffer;
	GHashTable *inparams;
	GHashTable *outparams;
	DBusConnection *dbus;
	gboolean mns_enabled;
	DBusPendingCall *pending_session;
	DBusPendingCall *pending_event;
	char *mns_path;
	gboolean disconnected;
	struct obex_session *obex_os;
	obex_object_t *obex_obj;
	void *request;
	GDestroyNotify request_free;
	GQueue *events_queue;
};

struct any_object {
	struct mas_session *mas;
};

struct notification_registration_request {
	uint8_t status;
};

struct msg_listing_request {
	gboolean nth_call;
	gboolean only_count;
	struct messages_filter filter;
	uint8_t subject_len;
};

struct folder_listing_request {
	gboolean nth_call;
	gboolean only_count;
};

struct get_message_request {
	unsigned long flags;
};

struct message_put_request {
	GString *buf;		/* FIXME: use mas_session.buffer instead */
	const char *name;
	struct bmsg_bmsg *bmsg;
	struct bmsg_parser *parser;
	unsigned long flags;
	gboolean parsed;
	size_t remaining;
};

static const uint8_t MAS_TARGET[TARGET_SIZE] = {
			0xbb, 0x58, 0x2b, 0x40, 0x42, 0x0c, 0x11, 0xdb,
			0xb0, 0xde, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66  };

struct messages_event *messages_event_new(enum messages_event_type event_type,
							enum bmsg_type msg_type,
							const char *handle,
							const char *folder,
							const char *old_folder)
{
	struct messages_event *event;

	event = g_new0(struct messages_event, 1);
	event->refcount = 1;

	event->type = event_type;
	event->msg_type = msg_type;
	event->handle = g_strdup(handle);
	event->folder = g_strdup(folder);
	event->old_folder = g_strdup(old_folder);

	return event;
}

void messages_event_ref(struct messages_event *event)
{
	++event->refcount;
}

void messages_event_unref(struct messages_event *event)
{
	--event->refcount;

	if (event->refcount == 0) {
		g_free(event->handle);
		g_free(event->folder);
		g_free(event->old_folder);
		g_free(event);
	}
}

static int find_aparam_tag(uint8_t tag)
{
	int i;

	for (i = 0; aparam_defs[i].tag != INVALID_TAG; ++i) {
		if (aparam_defs[i].tag == tag)
			return i;
	}

	return -1;
}

static void aparams_entry_free(gpointer val)
{
	struct aparam_entry *entry = val;
	int tago;

	tago = find_aparam_tag(entry->tag);

	if (tago < 0)
		goto notagdata;

	if (aparam_defs[tago].type == APT_STR)
		g_free(entry->valstr);

notagdata:
	g_free(entry);
}

static void aparams_free(GHashTable *aparams)
{
	if (!aparams)
		return;

	g_hash_table_destroy(aparams);
}

static GHashTable *aparams_new(void)
{
	GHashTable *aparams;

	aparams = g_hash_table_new_full(NULL, NULL, NULL, aparams_entry_free);

	return aparams;
}

/* Add/replace value of given tag in parameters table. If val is null, then
 * remove selected parameter.
 */
static gboolean aparams_write(GHashTable *params, enum aparam_tag tag,
								gpointer val)
{
	struct aparam_entry *param;
	int tago;
	union {
		char *valstr;
		uint16_t val16u;
		uint32_t val32u;
		uint8_t val8u;
	} *e = val;

	tago = find_aparam_tag(tag);

	if (tago < 0)
		return FALSE;

	param = g_new0(struct aparam_entry, 1);
	param->tag = tag;

	/* XXX: will it free string? */
	g_hash_table_remove(params, GINT_TO_POINTER(tag));

	if (!val)
		return TRUE;

	switch (aparam_defs[tago].type) {
	case APT_STR:
		param->valstr = g_strdup(e->valstr);
		break;
	case APT_UINT16:
		param->val16u = e->val16u;
		break;
	case APT_UINT32:
		param->val32u = e->val32u;
		break;
	case APT_UINT8:
		param->val8u = e->val8u;
		break;
	default:
		goto failed;
	}

	g_hash_table_insert(params, GINT_TO_POINTER(tag), param);

	return TRUE;
failed:
	g_free(param);
	return FALSE;
}

static void aparams_dump(gpointer tag, gpointer val, gpointer user_data)
{
	struct aparam_entry *param = val;
	int tago;

	tago = find_aparam_tag(GPOINTER_TO_INT(tag));

	switch (aparam_defs[tago].type) {
	case APT_STR:
		DBG("%-30s %s", aparam_defs[tago].name, param->valstr);
		break;
	case APT_UINT16:
		DBG("%-30s %08x", aparam_defs[tago].name, param->val16u);
		break;
	case APT_UINT32:
		DBG("%-30s %08x", aparam_defs[tago].name, param->val32u);
		break;
	case APT_UINT8:
		DBG("%-30s %08x", aparam_defs[tago].name, param->val8u);
		break;
	}
}

static gboolean aparams_read(GHashTable *params, enum aparam_tag tag,
								gpointer val)
{
	struct aparam_entry *param;
	int tago;
	union {
		char *valstr;
		uint16_t val16u;
		uint32_t val32u;
		uint8_t val8u;
	} *e = val;

	param = g_hash_table_lookup(params, GINT_TO_POINTER(tag));

	if (!param)
		return FALSE;

	if (!val)
		goto nooutput;

	tago = find_aparam_tag(tag);

	switch (aparam_defs[tago].type) {
	case APT_STR:
		e->valstr = param->valstr;
		break;
	case APT_UINT16:
		e->val16u = param->val16u;
		break;
	case APT_UINT32:
		e->val32u = param->val32u;
		break;
	case APT_UINT8:
		e->val8u = param->val8u;
		break;
	default:
		return FALSE;
	}

nooutput:
	return TRUE;
}

static GHashTable *parse_aparam(const uint8_t *buffer, uint32_t hlen)
{
	GHashTable *aparams;
	struct aparam_header *hdr;
	uint32_t len = 0;
	uint16_t val16;
	uint32_t val32;
	union {
		char *valstr;
		uint16_t val16u;
		uint32_t val32u;
		uint8_t val8u;
	} entry;
	int tago;

	aparams = aparams_new();
	if (!aparams)
		return NULL;

	while (len < hlen) {
		hdr = (void *) buffer + len;

		tago = find_aparam_tag(hdr->tag);

		if (tago < 0)
			goto skip;

		switch (aparam_defs[tago].type) {
		case APT_STR:
			entry.valstr = g_try_malloc0(hdr->len + 1);
			if (entry.valstr)
				memcpy(entry.valstr, hdr->val, hdr->len);
			break;
		case APT_UINT16:
			if (hdr->len != 2)
				goto failed;
			memcpy(&val16, hdr->val, sizeof(val16));
			entry.val16u = GUINT16_FROM_BE(val16);
			break;
		case APT_UINT32:
			if (hdr->len != 4)
				goto failed;
			memcpy(&val32, hdr->val, sizeof(val32));
			entry.val32u = GUINT32_FROM_BE(val32);
			break;
		case APT_UINT8:
			if (hdr->len != 1)
				goto failed;
			entry.val8u = hdr->val[0];
			break;
		default:
			goto failed;
		}
		aparams_write(aparams, hdr->tag, &entry);
		/* aparams_write makes its own copy, thus: */
		if (aparam_defs[tago].type == APT_STR)
			g_free(entry.valstr);

skip:
		len += hdr->len + sizeof(struct aparam_header);
	}

	g_hash_table_foreach(aparams, aparams_dump, NULL);

	return aparams;
failed:
	aparams_free(aparams);

	return NULL;
}

static GString *revparse_aparam(GHashTable *aparams)
{
	struct aparam_header hdr;
	gpointer key;
	gpointer value;
	uint16_t val16;
	uint32_t val32;
	union {
		char *valstr;
		uint16_t val16u;
		uint32_t val32u;
		uint8_t val8u;
	} entry;
	int tago;
	GHashTableIter iter;
	GString *buffer = NULL;

	if (!aparams)
		return NULL;

	g_hash_table_iter_init(&iter, aparams);
	buffer = g_string_new("");

	while (g_hash_table_iter_next(&iter, &key, &value)) {

		tago = find_aparam_tag(GPOINTER_TO_INT(key));

		if (tago < 0)
			goto failed;

		hdr.tag = aparam_defs[tago].tag;
		aparams_read(aparams, GPOINTER_TO_INT(key), &entry);

		switch (aparam_defs[tago].type) {
		case APT_STR:
			hdr.len = strlen(entry.valstr);
			g_string_append_len(buffer, (gpointer)&hdr,
							sizeof(hdr));
			g_string_append_len(buffer, entry.valstr, hdr.len);
			break;
		case APT_UINT16:
			hdr.len = 2;
			val16 = GUINT16_TO_BE(entry.val16u);
			g_string_append_len(buffer, (gpointer)&hdr,
							sizeof(hdr));
			g_string_append_len(buffer, (gpointer)&val16,
							sizeof(entry.val16u));
			break;
		case APT_UINT32:
			hdr.len = 4;
			val32 = GUINT32_TO_BE(entry.val32u);
			g_string_append_len(buffer, (gpointer)&hdr,
							sizeof(hdr));
			g_string_append_len(buffer, (gpointer)&val32,
							sizeof(entry.val32u));
			break;
		case APT_UINT8:
			hdr.len = 1;
			g_string_append_len(buffer, (gpointer)&hdr,
							sizeof(hdr));
			g_string_append_len(buffer, (gpointer)&entry.val8u,
							sizeof(entry.val8u));
			break;
		default:
			goto failed;
		}

	}

	return buffer;

failed:
	g_string_free(buffer, TRUE);

	return NULL;
}

static void append_entry(DBusMessageIter *dict,
				const char *key, void *val)
{
	DBusMessageIter entry, value;

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY,
								NULL, &entry);

	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);


	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
					DBUS_TYPE_STRING_AS_STRING, &value);
	dbus_message_iter_append_basic(&value, DBUS_TYPE_STRING, val);
	dbus_message_iter_close_container(&entry, &value);

	dbus_message_iter_close_container(dict, &entry);
}

static void clear_events_queue(struct mas_session *mas)
{
	GList *cur;

	for (cur = mas->events_queue->head; cur != NULL; cur = cur->next)
		messages_event_unref(cur->data);

	g_queue_clear(mas->events_queue);
}

static void messages_event_pcn(DBusPendingCall *pc, void *user_data);

static void send_next_event(struct mas_session *mas)
{
	struct messages_event *event;
	DBusMessage *outgoing;
	unsigned char evt;
	unsigned char msgtype = 2;
	unsigned char instance_id = 0;

	event = g_queue_pop_head(mas->events_queue);
	if (event == NULL)
		return;

	evt = (unsigned char)event->type + 1;

	outgoing = dbus_message_new_method_call("org.openobex.client",
			mas->mns_path, "org.openobex.MNS", "SendEvent");

	dbus_message_append_args(outgoing,
			DBUS_TYPE_BYTE, &instance_id,
			DBUS_TYPE_BYTE, &evt,
			DBUS_TYPE_STRING, &event->handle,
			DBUS_TYPE_STRING, &event->folder,
			DBUS_TYPE_STRING, &event->old_folder,
			DBUS_TYPE_BYTE, &msgtype,
			DBUS_TYPE_INVALID);

	dbus_connection_send_with_reply(mas->dbus, outgoing,
			&mas->pending_event, -1);

	dbus_message_unref(outgoing);

	dbus_pending_call_set_notify(mas->pending_event,
			messages_event_pcn, mas, NULL);

	messages_event_unref(event);
}

static int mns_stop_session(struct mas_session *mas);

static void mns_start_session_pcn(DBusPendingCall *pc, void *user_data)
{
	struct mas_session *mas = user_data;
	DBusMessage *incoming;
	char *path;

	DBG("");
	incoming = dbus_pending_call_steal_reply(pc);

	if (!incoming) {
		DBG("No reply!");	/* This probably should not happen */
		goto cleanup;
	}

	if (dbus_message_get_type(incoming) != DBUS_MESSAGE_TYPE_METHOD_RETURN) {
		DBG("Error when starting session!");
		goto cleanup;
	}

	if (!dbus_message_has_signature(incoming,
				DBUS_TYPE_OBJECT_PATH_AS_STRING)) {
		DBG("Wrong signature!");
		goto cleanup;
	}

	dbus_message_get_args(incoming, NULL, DBUS_TYPE_OBJECT_PATH,
			&path, DBUS_TYPE_INVALID);

	mas->mns_path = g_strdup(path);
	DBG("Path: %s", mas->mns_path);

cleanup:
	dbus_message_unref(incoming);
	dbus_pending_call_unref(pc);
	mas->pending_session = NULL;

	if (mas->mns_enabled == FALSE)
		mns_stop_session(mas);
	else
		send_next_event(mas);
}

/* XXX: How to act when connection is unexpectedly closed.
 */
static int mns_start_session(struct mas_session *mas)
{
	DBusMessage *outgoing;
	DBusMessageIter iter, dict;
	char *mns = "MNS";

	DBG("");
	mas->mns_enabled = TRUE;

	if (mas->mns_path)
		return 0;

	if (mas->pending_session)
		return 0;

	if (!mas->dbus)
		mas->dbus = obex_dbus_get_connection();

	outgoing = dbus_message_new_method_call("org.openobex.client", "/",
			"org.openobex.Client", "CreateSession");

	if (!outgoing) {
		DBG("Failed message creation.");
		return -1;
	}

	dbus_message_iter_init_append(outgoing, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	append_entry(&dict, "Destination", &mas->remote_addr);
	append_entry(&dict, "Target", &mns);

	dbus_message_iter_close_container(&iter, &dict);

	dbus_connection_send_with_reply(mas->dbus, outgoing,
			&mas->pending_session, -1);

	dbus_message_unref(outgoing);

	DBG("before set notify");
	dbus_pending_call_set_notify(mas->pending_session,
			mns_start_session_pcn, mas, NULL);

	return 0;
}

static void mas_clean(struct mas_session *mas);

static void mns_stop_session_pcn(DBusPendingCall *pc, void *user_data)
{
	struct mas_session *mas = user_data;

	DBG("");

	/* Ignore errors */

	dbus_pending_call_unref(pc);
	mas->pending_session = NULL;

	g_free(mas->mns_path);
	mas->mns_path = NULL;

	if (mas->mns_enabled)
		mns_start_session(mas);

	if (mas->disconnected)
		mas_clean(mas);
}

static int mns_stop_session(struct mas_session *mas)
{
	DBusMessage *outgoing;

	DBG("");

	clear_events_queue(mas);
	mas->mns_enabled = FALSE;

	if (mas->pending_session)
		return 0;

	if (!mas->mns_path)
		return -1;


	if (!mas->dbus)
		mas->dbus = obex_dbus_get_connection();

	outgoing = dbus_message_new_method_call("org.openobex.client", "/",
			"org.openobex.Client", "RemoveSession");

	if (!outgoing) {
		DBG("Failed message creation.");
		return -1;
	}

	dbus_message_append_args(outgoing, DBUS_TYPE_OBJECT_PATH,
			&mas->mns_path, DBUS_TYPE_INVALID);

	dbus_connection_send_with_reply(mas->dbus, outgoing,
			&mas->pending_session, -1);

	dbus_message_unref(outgoing);

	dbus_pending_call_set_notify(mas->pending_session,
			mns_stop_session_pcn, mas, NULL);

	return 0;
}

static void messages_event_pcn(DBusPendingCall *pc, void *user_data)
{
	struct mas_session *mas = user_data;
	DBusMessage *incoming;

	incoming = dbus_pending_call_steal_reply(pc);

	if (!incoming || dbus_message_get_type(incoming)
			!= DBUS_MESSAGE_TYPE_METHOD_RETURN) {
		DBG("Error when sending notification!");
		mas->mns_enabled = FALSE;
	}

	dbus_message_unref(incoming);
	dbus_pending_call_unref(pc);
	mas->pending_event = NULL;

	/* XXX: Or maybe better call ResetSession without waiting for event
	 * sending to finish
	 */
	if (!mas->mns_enabled)
		mns_stop_session(mas);
	else
		send_next_event(mas);
}

static void my_messages_event_cb(void *session, struct messages_event *data, void *user_data)
{
	struct mas_session *mas = user_data;

	DBG("");

	if (!mas->mns_path) {
		DBG("Backend tried to pushed event, but MNS is not connected!");
		return;
	}

	messages_event_ref(data);
	g_queue_push_tail(mas->events_queue, data);

	if (mas->pending_session || mas->pending_event) {
		DBG("MNS session connection or event sending in progress.");
		return;
	}

	send_next_event(mas);

}

static int set_notification_registration(struct mas_session *mas, int state)
{
	if (state == 1)
		mns_start_session(mas);
	else if (state == 0)
		mns_stop_session(mas);
	else
		return -EBADR;

	return 0;
}

static int get_params(struct obex_session *os, obex_object_t *obj,
					struct mas_session *mas)
{
	const uint8_t *buffer;
	GHashTable *inparams = NULL;
	ssize_t rsize;

	rsize = obex_aparam_read(os, obj, &buffer);

	if (rsize > 0) {
		inparams = parse_aparam(buffer, rsize);
		if (inparams == NULL) {
			DBG("Error when parsing parameters!");
			return -EBADR;
		}
	}

	if (inparams == NULL)
		inparams = aparams_new();

	mas->inparams = inparams;
	mas->outparams = aparams_new();

	return 0;
}

static void mas_reset(struct obex_session *os, void *user_data)
{
	struct mas_session *mas = user_data;

	DBG("");

	if (mas->buffer) {
		g_string_free(mas->buffer, TRUE);
		mas->buffer = NULL;
	}

	if (mas->request_free != NULL)
		mas->request_free(mas->request);

	aparams_free(mas->inparams);
	mas->inparams = NULL;
	aparams_free(mas->outparams);
	mas->outparams = NULL;
	mas->ap_sent = FALSE;
	mas->finished = FALSE;
	mas->request_free = NULL;
	mas->request = NULL;
}

static void mas_clean(struct mas_session *mas)
{
	if (mas->dbus)
		dbus_connection_unref(mas->dbus);

	g_queue_free(mas->events_queue);
	g_free(mas->remote_addr);
	g_free(mas);
}

static void *mas_connect(struct obex_session *os, int *err)
{
	struct mas_session *mas;
	char *sep = NULL;

	DBG("");

	mas = g_new0(struct mas_session, 1);

	*err = messages_connect(&mas->backend_data);
	if (*err < 0)
		goto failed;

	/* This gets bluetooth remote party address and port */
	mas->remote_addr = obex_get_id(os);
	if (mas->remote_addr)
		sep = strchr(mas->remote_addr, '+');
	if (sep)
		*sep = 0;

	mas->obex_os = os;
	mas->events_queue = g_queue_new();

	manager_register_session(os);

	return mas;

failed:
	g_free(mas);

	return NULL;
}

static void mas_disconnect(struct obex_session *os, void *user_data)
{
	struct mas_session *mas = user_data;

	DBG("");

	manager_unregister_session(os);
	messages_disconnect(mas->backend_data);

	mas->disconnected = TRUE;
	clear_events_queue(mas);

	if (mas->mns_enabled || mas->pending_event || mas->pending_session)
		set_notification_registration(mas, 0);
	else
		mas_clean(mas);
}

static int mas_get(struct obex_session *os, obex_object_t *obj, void *user_data)
{
	struct mas_session *mas = user_data;
	const char *type = obex_get_type(os);
	const char *name = obex_get_name(os);
	int ret;

	DBG("GET: name %s type %s mas %p",
			name, type, mas);

	mas->buffer = g_string_new("");

	if (type == NULL)
		return -EBADR;

	ret = get_params(os, obj, mas);
	if (ret < 0)
		return ret;

	ret = obex_get_stream_start(os, name);
	if (ret < 0)
		return ret;

	return 0;
}

static int mas_put(struct obex_session *os, obex_object_t *obj, void *user_data)
{
	struct mas_session *mas = user_data;
	const char *type = obex_get_type(os);
	const char *name = obex_get_name(os);
	int ret;

	DBG("PUT: name %s type %s mas %p", name, type, mas);

	mas->buffer = g_string_new("");

	if (type == NULL)
		return -EBADR;

	ret = get_params(os, obj, mas);
	if (ret < 0)
		return ret;

	mas->obex_obj = obj;

	ret = obex_put_stream_start(os, name);
	if (ret < 0)
		return ret;

	return 0;
}

/* FIXME: Preserve whitespaces */
static void g_string_append_escaped_printf(GString *string, const gchar *format,
		...)
{
	va_list ap;
	char *escaped;

	va_start(ap, format);
	escaped = g_markup_vprintf_escaped(format, ap);
	g_string_append(string, escaped);
	g_free(escaped);
	va_end(ap);
}

static const char *yesorno(gboolean a)
{
	if (a)
		return "yes";

	return "no";
}

static char *trim_utf8(char *s, size_t len)
{
	char *end;
	size_t l;

	l = strlen(s);
	if (l > len)
		l = len;

	g_utf8_validate(s, len, (const gchar **)&end);
	*end = '\0';

	return s;
}

static void get_messages_listing_cb(void *session, int err,
		uint16_t size, gboolean newmsg,
		const struct messages_message *entry,
		void *user_data)
{
	struct mas_session *mas = user_data;
	struct msg_listing_request *request = mas->request;
	uint8_t newmsg_byte;
	char timebuf[21];
	char *timestr = timebuf;
	time_t t;

	if (err < 0 && err != -EAGAIN) {
		obex_object_set_io_flags(mas, G_IO_ERR, err);
		return;
	}

	if (!request->nth_call) {
		if (!request->only_count)
			g_string_append(mas->buffer, ML_BODY_BEGIN);
		request->nth_call = TRUE;
	}

	if (!entry) {
		if (!request->only_count)
			g_string_append(mas->buffer, ML_BODY_END);
		mas->finished = TRUE;

		newmsg_byte = newmsg ? 1 : 0;
		aparams_write(mas->outparams, NEWMESSAGE_TAG, &newmsg_byte);
		aparams_write(mas->outparams, MESSAGESLISTINGSIZE_TAG, &size);
		time(&t);
		strftime(timestr, sizeof(timebuf), "%Y%m%dT%H%M%S%z",
				localtime(&t));
		aparams_write(mas->outparams, MSETIME_TAG, &timestr);

		goto proceed;
	}

	g_string_append(mas->buffer, "<msg");

	g_string_append_escaped_printf(mas->buffer, " handle=\"%s\"",
								entry->handle);

	if (request->filter.parameter_mask & PMASK_SUBJECT &&
			entry->mask & PMASK_SUBJECT) {
		char *subject;

		subject	= g_strdup(entry->subject);
		trim_utf8(subject, request->subject_len);

		g_string_append_escaped_printf(mas->buffer, " subject=\"%s\"",
				subject);

		g_free(subject);
	}

	if (request->filter.parameter_mask & PMASK_DATETIME &&
			entry->mask & PMASK_DATETIME)
		g_string_append_escaped_printf(mas->buffer, " datetime=\"%s\"",
				entry->datetime);

	if (request->filter.parameter_mask & PMASK_SENDER_NAME &&
			entry->mask & PMASK_SENDER_NAME)
		g_string_append_escaped_printf(mas->buffer,
						" sender_name=\"%s\"",
						entry->sender_name);

	if (request->filter.parameter_mask & PMASK_SENDER_ADDRESSING &&
			entry->mask & PMASK_SENDER_ADDRESSING)
		g_string_append_escaped_printf(mas->buffer,
						" sender_addressing=\"%s\"",
						entry->sender_addressing);

	if (request->filter.parameter_mask & PMASK_REPLYTO_ADDRESSING &&
			entry->mask & PMASK_REPLYTO_ADDRESSING)
		g_string_append_escaped_printf(mas->buffer,
						" replyto_addressing=\"%s\"",
						entry->replyto_addressing);

	if (request->filter.parameter_mask & PMASK_RECIPIENT_NAME &&
			entry->mask & PMASK_RECIPIENT_NAME)
		g_string_append_escaped_printf(mas->buffer,
						" recipient_name=\"%s\"",
						entry->recipient_name);

	if (request->filter.parameter_mask & PMASK_RECIPIENT_ADDRESSING &&
			entry->mask & PMASK_RECIPIENT_ADDRESSING)
		g_string_append_escaped_printf(mas->buffer,
						" recipient_addressing=\"%s\"",
						entry->recipient_addressing);

	if (request->filter.parameter_mask & PMASK_TYPE &&
			entry->mask & PMASK_TYPE)
		g_string_append_escaped_printf(mas->buffer, " type=\"%s\"",
				entry->type);

	if (request->filter.parameter_mask & PMASK_RECEPTION_STATUS &&
			entry->mask & PMASK_RECEPTION_STATUS)
		g_string_append_escaped_printf(mas->buffer,
						" reception_status=\"%s\"",
						entry->reception_status);

	if (request->filter.parameter_mask & PMASK_SIZE &&
			entry->mask & PMASK_SIZE)
		g_string_append_escaped_printf(mas->buffer, " size=\"%s\"",
				entry->size);

	if (request->filter.parameter_mask & PMASK_ATTACHMENT_SIZE &&
			entry->mask & PMASK_ATTACHMENT_SIZE)
		g_string_append_escaped_printf(mas->buffer,
						" attachment_size=\"%s\"",
						entry->attachment_size);

	if (request->filter.parameter_mask & PMASK_TEXT &&
			entry->mask & PMASK_TEXT)
		g_string_append_escaped_printf(mas->buffer, " text=\"%s\"",
				yesorno(entry->text));

	if (request->filter.parameter_mask & PMASK_READ &&
			entry->mask & PMASK_READ)
		g_string_append_escaped_printf(mas->buffer, " read=\"%s\"",
				yesorno(entry->read));

	if (request->filter.parameter_mask & PMASK_SENT &&
			entry->mask & PMASK_SENT)
		g_string_append_escaped_printf(mas->buffer, " sent=\"%s\"",
				yesorno(entry->sent));

	if (request->filter.parameter_mask & PMASK_PROTECTED &&
			entry->mask & PMASK_PROTECTED)
		g_string_append_escaped_printf(mas->buffer, " protected=\"%s\"",
				yesorno(entry->protect));

	if (request->filter.parameter_mask & PMASK_PRIORITY &&
			entry->mask & PMASK_PRIORITY)
		g_string_append_escaped_printf(mas->buffer, " priority=\"%s\"",
				yesorno(entry->priority));

	g_string_append(mas->buffer, "/>\n");

proceed:
	if (err != -EAGAIN)
		obex_object_set_io_flags(mas, G_IO_IN, 0);
}

static void get_message_cb(void *session, int err, gboolean fmore,
	const char *chunk, void *user_data)
{
	struct mas_session *mas = user_data;
	struct get_message_request *request = mas->request;
	uint8_t fmore_byte;

	DBG("");

	if (err < 0 && err != -EAGAIN) {
		obex_object_set_io_flags(mas, G_IO_ERR, err);
		return;
	}

	if (!chunk) {
		mas->finished = TRUE;

		if (request->flags & MESSAGES_FRACTION) {
			fmore_byte = fmore ? 1 : 0;
			aparams_write(mas->outparams, FRACTIONDELIVER_TAG,
									&fmore);
		}

		goto proceed;
	}

	g_string_append(mas->buffer, chunk);

proceed:
	if (err != -EAGAIN)
		obex_object_set_io_flags(mas, G_IO_IN, 0);
}

static void get_folder_listing_cb(void *session, int err, uint16_t size,
					const char *name, void *user_data)
{
	struct mas_session *mas = user_data;
	struct folder_listing_request *request = mas->request;

	if (err < 0 && err != -EAGAIN) {
		obex_object_set_io_flags(mas, G_IO_ERR, err);
		return;
	}

	if (request->only_count) {
		if (err != -EAGAIN)
			aparams_write(mas->outparams, FOLDERLISTINGSIZE_TAG,
					&size);
		if (!name)
			mas->finished = TRUE;
		goto proceed;
	}

	if (!request->nth_call) {
		g_string_append(mas->buffer, XML_DECL);
		g_string_append(mas->buffer, FL_DTD);
		if (!name) {
			g_string_append(mas->buffer, FL_BODY_EMPTY);
			mas->finished = TRUE;
			goto proceed;
		}
		g_string_append(mas->buffer, FL_BODY_BEGIN);
		request->nth_call = TRUE;
	}

	if (!name) {
		g_string_append(mas->buffer, FL_BODY_END);
		mas->finished = TRUE;
		goto proceed;
	}

	if (g_strcmp0(name, "..") == 0)
		g_string_append(mas->buffer, FL_PARENT_FOLDER_ELEMENT);
	else
		g_string_append_escaped_printf(mas->buffer, FL_FOLDER_ELEMENT,
									name);

proceed:
	if (err != -EAGAIN)
		obex_object_set_io_flags(mas, G_IO_IN, err);
}

static int mas_setpath(struct obex_session *os, obex_object_t *obj,
		void *user_data)
{
	const char *name;
	uint8_t *nonhdr;
	struct mas_session *mas = user_data;

	if (OBEX_ObjectGetNonHdrData(obj, &nonhdr) != 2) {
		error("Set path failed: flag and constants not found!");
		return -EBADR;
	}

	name = obex_get_name(os);

	DBG("SETPATH: name %s nonhdr 0x%x%x", name, nonhdr[0], nonhdr[1]);

	if ((nonhdr[0] & 0x02) != 0x02) {
		DBG("Error: requested directory creation");
		return -EBADR;
	}

	return messages_set_folder(mas->backend_data, name, nonhdr[0] & 0x01);
}

static void *folder_listing_open(const char *name, int oflag, mode_t mode,
				void *driver_data, size_t *size, int *err)
{
	struct mas_session *mas = driver_data;
	struct folder_listing_request *request;
	/* 1024 is the default when there was no MaxListCount sent */
	uint16_t max = 1024;
	uint16_t offset = 0;

	if (oflag != O_RDONLY) {
		*err = -EBADR;
		return NULL;
	}

	DBG("name = %s", name);

	request = g_new0(struct folder_listing_request, 1);
	mas->request = request;
	mas->request_free = g_free;

	aparams_read(mas->inparams, MAXLISTCOUNT_TAG, &max);
	request->only_count = max > 0 ? FALSE : TRUE;

	aparams_read(mas->inparams, STARTOFFSET_TAG, &offset);

	*err = messages_get_folder_listing(mas->backend_data, name, max, offset,
			get_folder_listing_cb, mas);

	if (*err < 0)
		return NULL;
	else
		return mas;
}

static void msg_listing_free(gpointer data)
{
	struct msg_listing_request *request = data;

	g_free(request);
}

static void *msg_listing_open(const char *name, int oflag, mode_t mode,
				void *driver_data, size_t *size, int *err)
{
	struct mas_session *mas = driver_data;
	struct msg_listing_request *request;
	uint16_t max = 1024;
	uint16_t offset = 0;

	DBG("");

	if (oflag != O_RDONLY) {
		*err = -EBADR;
		return NULL;
	}

	request = g_new0(struct msg_listing_request, 1);
	mas->request = request;
	mas->request_free = msg_listing_free;

	aparams_read(mas->inparams, MAXLISTCOUNT_TAG, &max);
	request->only_count = max > 0 ? FALSE : TRUE;

	aparams_read(mas->inparams, PARAMETERMASK_TAG,
					&request->filter.parameter_mask);
	if (request->filter.parameter_mask == 0)
		request->filter.parameter_mask = 0xFFFF;

	aparams_read(mas->inparams, SUBJECTLENGTH_TAG, &request->subject_len);
	if (request->subject_len == 0)
		request->subject_len = 255;

	aparams_read(mas->inparams, STARTOFFSET_TAG, &offset);
	aparams_read(mas->inparams, FILTERMESSAGETYPE_TAG,
					&request->filter.type);
	aparams_read(mas->inparams, FILTERPERIODBEGIN_TAG,
					&request->filter.period_begin);
	aparams_read(mas->inparams, FILTERPERIODEND_TAG,
					&request->filter.period_end);
	aparams_read(mas->inparams, FILTERREADSTATUS_TAG,
					&request->filter.read_status);
	aparams_read(mas->inparams, FILTERRECIPIENT_TAG,
					&request->filter.recipient);
	aparams_read(mas->inparams, FILTERORIGINATOR_TAG,
					&request->filter.originator);
	aparams_read(mas->inparams, FILTERPRIORITY_TAG,
					&request->filter.priority);

	*err = messages_get_messages_listing(mas->backend_data, name, max,
			offset, &request->filter,
			get_messages_listing_cb, mas);

	if (*err < 0)
		return NULL;
	else
		return mas;
}

static void push_message_cb(void *session, int err, const char *handle,
								void *user_data)
{
	struct mas_session *mas = user_data;

	DBG("");

	mas->finished = TRUE;

	if (handle == NULL) {
		DBG("err: %d", err);
		obex_object_set_io_flags(mas, G_IO_ERR, err);
	} else {
		DBG("handle: %s", handle);
		obex_name_write(mas->obex_os, mas->obex_obj, handle);
		obex_object_set_io_flags(mas, G_IO_OUT, 0);
	}
}

static void message_put_free(gpointer data)
{
	struct message_put_request *request = data;

	bmsg_parser_free(request->parser);
	bmsg_free(request->bmsg);
	g_string_free(request->buf, TRUE);
	g_free(request);
}

static void message_put(struct mas_session *mas, const char *name, int *err)
{
	struct message_put_request *request;
	uint8_t value;

	DBG("");

	request = g_new0(struct message_put_request, 1);

	mas->request = request;
	mas->request_free = message_put_free;

	aparams_read(mas->inparams, CHARSET_TAG, &value);
	if (value & 0x01)
		request->flags |= MESSAGES_UTF8;

	aparams_read(mas->inparams, TRANSPARENT_TAG, &value);
	if (value & 0x01)
		request->flags |= MESSAGES_TRANSPARENT;

	aparams_read(mas->inparams, RETRY_TAG, &value);
	if (value & 0x01)
		request->flags |= MESSAGES_RETRY;

	request->parser = bmsg_parser_new();
	request->buf = g_string_new("");
	request->name = name;

	*err = 0;
}

static void message_get(struct mas_session *mas, const char *name, int *err)
{
	struct get_message_request *request;
	uint8_t freq;
	uint8_t charset = 0;

	DBG("");

	request = g_new0(struct get_message_request, 1);
	mas->request = request;
	mas->request_free = g_free;

	if (aparams_read(mas->inparams, FRACTIONREQUEST_TAG, &freq)) {
		request->flags |= MESSAGES_FRACTION;
		if (freq & 0x01)
			request->flags |= MESSAGES_NEXT;
	}

	aparams_read(mas->inparams, CHARSET_TAG, &charset);
	if (charset & 0x01)
		request->flags |= MESSAGES_UTF8;

	*err = messages_get_message(mas->backend_data, name, request->flags,
			get_message_cb, mas);
}

static void *message_open(const char *name, int oflag, mode_t mode,
				void *driver_data, size_t *size, int *err)
{
	struct mas_session *mas = driver_data;

	DBG("");

	if (oflag == O_RDONLY)
		message_get(mas, name, err);
	else
		message_put(mas, name, err);

	if (*err < 0)
		return NULL;
	else
		return mas;
}

static ssize_t message_write_body(struct mas_session *mas, const void *buf,
								size_t count)
{
	struct message_put_request *request = mas->request;
	size_t n;
	ssize_t ret;

	if (request->remaining == 0) {
		g_string_append_len(request->buf, buf, count);
		return count;
	}

	if (request->remaining > count)
		n = count;
	else
		n = request->remaining;

	ret = messages_push_message_body(mas->backend_data, buf, n);
	if (ret < 0)
		return ret;

	request->remaining -= ret;

	return ret;
}

static ssize_t message_write_header(struct mas_session *mas, const void *buf,
								size_t count)
{
	struct message_put_request *request = mas->request;
	char *pos;
	int ret;
	ssize_t size;

	DBG("");

	g_string_append_len(request->buf, buf, count);
	pos = request->buf->str;

	ret = bmsg_parser_process(request->parser, &pos, count);
	if (ret < 0)
		return ret;

	size = pos - request->buf->str;
	g_string_erase(request->buf, 0, size);

	if (ret > 0)
		return count;

	DBG("Parsing done");

	request->bmsg = bmsg_parser_get_bmsg(request->parser);

	bmsg_parser_free(request->parser);
	request->parser = NULL;

	request->parsed = TRUE;
	request->remaining = request->bmsg->length;

	g_string_set_size(request->buf, 0);

	ret = messages_push_message(mas->backend_data, request->bmsg,
			request->name, MESSAGES_UTF8, push_message_cb, mas);
	if (ret < 0)
		return ret;

	return size;
}

static ssize_t message_write(void *object, const void *buf, size_t count)
{
	struct mas_session *mas = object;
	struct message_put_request *request = mas->request;

	if (request->parsed)
		return message_write_body(mas, buf, count);
	else
		return message_write_header(mas, buf, count);

}

static int message_flush(void *obj)
{
	struct mas_session *mas = obj;
	struct message_put_request *request = mas->request;
	char *tail;
	size_t len;
	int ret;

	if (mas->finished)
		return 0;

	if (request->bmsg == NULL) {
		DBG("Flushing and no bmsg - this should not happened.");
		return -EIO;
	}

	len = bmsg_parser_tail_length(request->bmsg);

	if (request->buf->len < len) {
		DBG("Not enough bytes received!");
		return -EBADR;
	}

	tail = request->buf->str + request->buf->len - len;
	if (!bmsg_parser_tail_correct(request->bmsg, tail, len)) {
		DBG("BMSG tail check failed!");
		DBG("!!! Falling back to hopelessly broken PTS strategy !!!");
		g_string_append(request->buf, "\r\n");
		tail += 2;
		if (!bmsg_parser_tail_correct(request->bmsg, tail, len)) {
			DBG("This doesn't help either!");
			return -EBADR;
		}
	}

	if (request->buf->len > len) {
		ret = messages_push_message_body(mas->backend_data,
						request->buf->str,
						request->buf->len - len);
		if (ret < 0)
			return ret;
	}

	ret = messages_push_message_body(mas->backend_data, NULL, 0);
	if (ret < 0)
		return ret;

	return -EAGAIN;
}

static void *notification_registration_open(const char *name, int oflag,
		mode_t mode, void *driver_data, size_t *size, int *err)
{
	struct mas_session *mas = driver_data;
	struct notification_registration_request *nr;
	uint8_t status;

	if (!(oflag & O_WRONLY)) {
		DBG("Tried GET on a PUT-only type");
		*err = -EBADR;

		return NULL;
	}

	if (!aparams_read(mas->inparams, NOTIFICATIONSTATUS_TAG, &status)) {
		DBG("Missing status parameter");
		*err = -EBADR;

		return NULL;
	}

	DBG("status: %d", status);

	if (status > 1) {
		DBG("Status parameter carrying incorrect value");
		*err = -EBADR;

		return NULL;
	}

	nr = g_new0(struct notification_registration_request, 1);
	nr->status = status;
	mas->request = nr;

	/* MNS connection operations take place in
	 * notification_registration_close() - PTS requires that MAS responds to
	 * SetNotificationReqistration first before changing state of MNS. */

	*err = 0;
	mas->finished = TRUE;

	return mas;
}

static int notification_registration_close(void *obj)
{
	struct mas_session *mas = obj;
	struct notification_registration_request *nr = mas->request;

	DBG("");

	set_notification_registration(mas, nr->status);

	if (nr->status) {
		messages_set_notification_registration(mas->backend_data,
				my_messages_event_cb,
				mas);
	} else {
		messages_set_notification_registration(mas->backend_data,
				NULL,
				NULL);
	}

	g_free(nr);

	return 0;
}

static void message_status_cb(void *session, int err, void *user_data)
{
	struct mas_session *mas = user_data;

	DBG("");

	if (err == -EAGAIN)
		return;

	mas->finished = TRUE;

	if (err < 0)
		obex_object_set_io_flags(mas, G_IO_ERR, err);
	else
		obex_object_set_io_flags(mas, G_IO_OUT, 0);
}

static void *message_status_open(const char *name, int oflag, mode_t mode,
				void *driver_data, size_t *size, int *err)
{
	struct mas_session *mas = driver_data;
	uint8_t indicator;
	uint8_t value;

	if (!(oflag & O_WRONLY)) {
		DBG("Tried GET on a PUT-only type");
		*err = -EBADR;

		return NULL;
	}

	if (!aparams_read(mas->inparams, STATUSINDICATOR_TAG, &indicator)) {
		DBG("Missing status indicator parameter");
		*err = -EBADR;

		return NULL;
	}

	if (!aparams_read(mas->inparams, STATUSVALUE_TAG, &value)) {
		DBG("Missing status value parameter");
		*err = -EBADR;

		return NULL;
	}

	DBG("indicator: %d, value: %d", indicator, value);
	*err = messages_set_message_status(mas->backend_data, name, indicator,
						value, message_status_cb, mas);
	if (*err)
		return NULL;
	else
		return mas;
}

static void *any_open(const char *name, int oflag, mode_t mode,
				void *driver_data, size_t *size, int *err)
{
	DBG("");

	*err = -EINVAL;

	return NULL;
}

static ssize_t any_write(void *object, const void *buf, size_t count)
{
	struct mas_session *mas = object;

	if (!mas->finished)
		return -EAGAIN;

	DBG("ignored %zu byte(s)", count);

	return count;
}

static ssize_t any_get_next_header(void *object, void *buf, size_t mtu,
								uint8_t *hi)
{
	struct mas_session *mas = object;
	GString *buffer;
	ssize_t ret;

	DBG("");

	if (mas->buffer->len == 0 && !mas->finished)
		return -EAGAIN;

	if (mas->ap_sent)
		return 0;

	*hi = OBEX_HDR_APPARAM;

	buffer = revparse_aparam(mas->outparams);
	if (buffer == NULL) {
		mas->ap_sent = TRUE;
		return 0;
	}

	ret = string_read(buffer, buf, mtu);
	if (buffer->len > 0) {
		DBG("Application parameters header won't fit in MTU, "
							"aborting request!");
		ret = -EIO;
	}

	g_string_free(buffer, TRUE);
	mas->ap_sent = TRUE;

	return ret;
}

static ssize_t any_read(void *obj, void *buf, size_t count)
{
	struct mas_session *mas = obj;
	ssize_t len;

	DBG("");

	len = string_read(mas->buffer, buf, count);

	if (len == 0 && !mas->finished)
		return -EAGAIN;

	return len;
}

static int any_close(void *obj)
{
	struct mas_session *mas = obj;

	DBG("");

	if (!mas->finished)
		messages_abort(mas->backend_data);

	return 0;
}

static struct obex_service_driver mas = {
	.name = "Message Access server",
	.service = OBEX_MAS,
	.channel = MAS_CHANNEL,
	.record = MAS_RECORD,
	.target = MAS_TARGET,
	.target_size = TARGET_SIZE,
	.connect = mas_connect,
	.get = mas_get,
	.put = mas_put,
	.setpath = mas_setpath,
	.disconnect = mas_disconnect,
	.reset = mas_reset,
};

static struct obex_mime_type_driver mime_map = {
	.target = MAS_TARGET,
	.target_size = TARGET_SIZE,
	.mimetype = NULL,
	.open = any_open,
	.close = any_close,
	.read = any_read,
	.write = any_write,
};

static struct obex_mime_type_driver mime_message = {
	.target = MAS_TARGET,
	.target_size = TARGET_SIZE,
	.mimetype = "x-bt/message",
	.get_next_header = any_get_next_header,
	.open = message_open,
	.close = any_close,
	.read = any_read,
	.write = message_write,
	.flush = message_flush,
};

static struct obex_mime_type_driver mime_folder_listing = {
	.target = MAS_TARGET,
	.target_size = TARGET_SIZE,
	.mimetype = "x-obex/folder-listing",
	.get_next_header = any_get_next_header,
	.open = folder_listing_open,
	.close = any_close,
	.read = any_read,
	.write = any_write,
};

static struct obex_mime_type_driver mime_msg_listing = {
	.target = MAS_TARGET,
	.target_size = TARGET_SIZE,
	.mimetype = "x-bt/MAP-msg-listing",
	.get_next_header = any_get_next_header,
	.open = msg_listing_open,
	.close = any_close,
	.read = any_read,
	.write = any_write,
};

static struct obex_mime_type_driver mime_notification_registration = {
	.target = MAS_TARGET,
	.target_size = TARGET_SIZE,
	.mimetype = "x-bt/MAP-NotificationRegistration",
	.open = notification_registration_open,
	.close = notification_registration_close,
	.read = any_read,
	.write = any_write,
};

static struct obex_mime_type_driver mime_message_status = {
	.target = MAS_TARGET,
	.target_size = TARGET_SIZE,
	.mimetype = "x-bt/messageStatus",
	.open = message_status_open,
	.close = any_close,
	.read = any_read,
	.write = any_write,
};

static struct obex_mime_type_driver mime_message_update = {
	.target = MAS_TARGET,
	.target_size = TARGET_SIZE,
	.mimetype = "x-bt/MAP-messageUpdate",
	.open = any_open,
	.close = any_close,
	.read = any_read,
	.write = any_write,
};

static struct obex_mime_type_driver *map_drivers[] = {
	&mime_map,
	&mime_message,
	&mime_folder_listing,
	&mime_msg_listing,
	&mime_notification_registration,
	&mime_message_status,
	&mime_message_update,
	NULL
};

static int mas_init(void)
{
	int err;
	int i;

	err = messages_init();
	if (err < 0)
		return err;

	for (i = 0; map_drivers[i] != NULL; ++i) {
		err = obex_mime_type_driver_register(map_drivers[i]);
		if (err < 0)
			goto failed;
	}

	err = obex_service_driver_register(&mas);
	if (err < 0)
		goto failed;

	return 0;

failed:
	for (--i; i >= 0; --i)
		obex_mime_type_driver_unregister(map_drivers[i]);

	messages_exit();

	return err;
}

static void mas_exit(void)
{
	int i;

	/* XXX: Is mas_disconnect() guaranteed before mas_exit()? */
	/* XXX: Shall I keep waiting here for closing MNS connections? */

	obex_service_driver_unregister(&mas);

	for (i = 0; map_drivers[i] != NULL; ++i)
		obex_mime_type_driver_unregister(map_drivers[i]);

	messages_exit();
}

OBEX_PLUGIN_DEFINE(mas, mas_init, mas_exit)
