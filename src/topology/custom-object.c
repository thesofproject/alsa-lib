/*
  Copyright(c) 2020-2021 Intel Corporation
  All rights reserved.

  This library is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of
  the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Lesser General Public License for more details.

  Author: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
*/

/*
 * This file contains the create/build routines for custom classes that are not
 * DAI, component or PCM type
 */
#include "list.h"
#include "local.h"
#include "tplg_local.h"
#include "tplg2_local.h"
#include <ctype.h>

/* pipeline object customization */
int tplg_create_pipeline_object(struct tplg_class *class, struct tplg_object *object)
{
	struct list_head *pos;

	/* check if child objects are of the right type */
	list_for_each(pos, &class->object_list) {
		struct tplg_object *obj = list_entry(pos, struct tplg_object, list);

		switch (obj->type) {
		case SND_TPLG_CLASS_TYPE_BASE:
			if (!strcmp(obj->class_name, "connection") ||
			    !strcmp(obj->class_name, "endpoint"))
				break;
			SNDERR("Unexpected child class %s for pipeline %s\n", obj->class_name,
			       object->name);
			return -EINVAL;
		case SND_TPLG_CLASS_TYPE_COMPONENT:
		case SND_TPLG_CLASS_TYPE_PCM:
			break;
		default:
			SNDERR("Unexpected child object type %d for %s\n", obj->type, object->name);
			return -EINVAL;
		}
	}

	return 0;
}
