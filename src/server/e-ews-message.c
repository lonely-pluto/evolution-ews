/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Authors :
 *  JP Rosevear <jpr@ximian.com>
 *  Rodrigo Moya <rodrigo@ximian.com>
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
#include <libsoup/soup-uri.h>
#include "e-ews-message.h"

ESoapMessage *
e_ews_message_new_with_header (const gchar *uri,
			       const gchar *impersonate_user,
                               const gchar *method_name,
                               const gchar *attribute_name,
                               const gchar *attribute_value,
                               EwsServerVersion server_info)
{
	ESoapMessage *msg;
	const gchar *server_ver;

	msg = e_soap_message_new (
		SOUP_METHOD_POST, uri, FALSE, NULL, NULL, NULL);
	if (msg == NULL) {
		g_warning (G_STRLOC ": Could not build SOAP message");
		return NULL;
	}

	soup_message_headers_append (
		SOUP_MESSAGE (msg)->request_headers,
		"Content-Type", "text/xml; charset=utf-8");
	soup_message_headers_append (
		SOUP_MESSAGE (msg)->request_headers,
		"User-Agent", "Evolution/" VERSION);
	soup_message_headers_append (
		SOUP_MESSAGE (msg)->request_headers,
		"Connection", "Keep-Alive");

	e_soap_message_start_envelope (msg);

	/* server info */
	if (server_info == EWS_EXCHANGE_2007_SP1)
		server_ver = "Exchange2007_SP1";
	else if (server_info == EWS_EXCHANGE_2010)
		server_ver = "Exchange2010";
	else if (server_info == EWS_EXCHANGE_2010_SP1)
		server_ver = "Exchange2010_SP1";
	else if (server_info == EWS_EXCHANGE_2010_SP2)
		server_ver = "Exchange2010_SP2";
	else
		server_ver = "Exchange2007";

	e_soap_message_start_header (msg);

	e_soap_message_start_element (
		msg, "RequestServerVersion", "types",
		"http://schemas.microsoft.com/exchange/services/2006/types");
	e_soap_message_add_attribute (msg, "Version", server_ver, NULL, NULL);
	e_soap_message_end_element (msg);

	if (impersonate_user && *impersonate_user) {
		e_soap_message_start_element (msg, "ExchangeImpersonation", "types",
			"http://schemas.microsoft.com/exchange/services/2006/types");

		e_soap_message_start_element (msg, "ConnectingSID", "types", NULL);

		if (strchr (impersonate_user, '@') != 0)
			e_soap_message_start_element (msg, "PrimarySmtpAddress", "types", NULL);
		else
			e_soap_message_start_element (msg, "PrincipalName", "types", NULL);
		e_soap_message_write_string (msg, impersonate_user);
		e_soap_message_end_element (msg); /* PrimarySmtpAddress or PrincipalName */

		e_soap_message_end_element (msg); /* ConnectingSID */

		e_soap_message_end_element (msg); /* ExchangeImpersonation */
	}

	e_soap_message_end_header (msg);

	e_soap_message_start_body (msg);
	e_soap_message_add_namespace (
		msg, "messages",
		"http://schemas.microsoft.com/exchange/services/2006/messages");
	e_soap_message_start_element (msg, method_name, "messages", NULL);
	e_soap_message_set_default_namespace (
		msg,
		"http://schemas.microsoft.com/exchange/services/2006/types");
	if (attribute_name != NULL)
		e_soap_message_add_attribute (
			msg, attribute_name, attribute_value, NULL, NULL);

	return msg;
}

void
e_ews_message_write_string_parameter (ESoapMessage *msg,
                                      const gchar *name,
                                      const gchar *prefix,
                                      const gchar *value)
{
	e_soap_message_start_element (msg, name, prefix, NULL);
	e_soap_message_write_string (msg, value);
	e_soap_message_end_element (msg);
}

void
e_ews_message_write_string_parameter_with_attribute (ESoapMessage *msg,
                                                     const gchar *name,
                                                     const gchar *prefix,
                                                     const gchar *value,
                                                     const gchar *attribute_name,
                                                     const gchar *attribute_value)
{
	e_soap_message_start_element (msg, name, prefix, NULL);
	e_soap_message_add_attribute (
		msg, attribute_name, attribute_value, NULL, NULL);
	e_soap_message_write_string (msg, value);
	e_soap_message_end_element (msg);
}

void
e_ews_message_write_base64_parameter (ESoapMessage *msg,
                                      const gchar *name,
                                      const gchar *prefix,
                                      const gchar *value)
{
	e_soap_message_start_element (msg, name, prefix, NULL);
	e_soap_message_write_base64 (msg, value, strlen (value));
	e_soap_message_end_element (msg);
}

void
e_ews_message_write_int_parameter (ESoapMessage *msg,
                                   const gchar *name,
                                   const gchar *prefix,
                                   glong value)
{
	e_soap_message_start_element (msg, name, prefix, NULL);
	e_soap_message_write_int (msg, value);
	e_soap_message_end_element (msg);
}

void
e_ews_message_write_footer (ESoapMessage *msg)
{
	e_soap_message_end_element (msg);
	e_soap_message_end_body (msg);
	e_soap_message_end_envelope (msg);

	e_soap_message_persist (msg);
}
