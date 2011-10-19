/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* e-book-backend-ews-factory.c - Ews contact backend factory.
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Authors: Chris Toshok <toshok@ximian.com>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <libedata-book/e-book-backend-factory.h>
#include "e-book-backend-ews.h"

typedef EBookBackendFactory EBookBackendEwsFactory;
typedef EBookBackendFactoryClass EBookBackendEwsFactoryClass;

/* Module Entry Points */
void e_module_load (GTypeModule *type_module);
void e_module_unload (GTypeModule *type_module);

/* Forward Declarations */
GType e_book_backend_ews_factory_get_type (void);

G_DEFINE_DYNAMIC_TYPE (
	EBookBackendEwsFactory,
	e_book_backend_ews_factory,
	E_TYPE_BOOK_BACKEND_FACTORY)

static void
e_book_backend_ews_factory_class_init (EBookBackendFactoryClass *class)
{
	class->factory_name = "ews";
	class->backend_type = E_TYPE_BOOK_BACKEND_EWS;
}

static void
e_book_backend_ews_factory_class_finalize (EBookBackendFactoryClass *class)
{
}

static void
e_book_backend_ews_factory_init (EBookBackendFactory *factory)
{
}

G_MODULE_EXPORT void
e_module_load (GTypeModule *type_module)
{
	e_book_backend_ews_factory_register_type (type_module);
}

G_MODULE_EXPORT void
e_module_unload (GTypeModule *type_module)
{
}
