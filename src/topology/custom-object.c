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

static void tplg_set_stream_name(struct tplg_object *object)
{
	struct tplg_attribute *pcm_name, *pcm_id, *dir, *stream_name;
	int ret;

	pcm_name = tplg_get_attribute_by_name(&object->attribute_list, "pcm_name");
	pcm_id = tplg_get_attribute_by_name(&object->attribute_list, "pcm_id");
	dir = tplg_get_attribute_by_name(&object->attribute_list, "direction");
	stream_name = tplg_get_attribute_by_name(&object->attribute_list, "stream_name");

	ret = snprintf(stream_name->value.string, SNDRV_CTL_ELEM_ID_NAME_MAXLEN,
		       "%s.%s.%ld", pcm_name->value.string,
		       dir->value.string, pcm_id->value.integer);
	if (ret > SNDRV_CTL_ELEM_ID_NAME_MAXLEN)
		SNDERR("warning: widget stream name truncated \n");

	stream_name->found = true;
	stream_name->type = SND_CONFIG_TYPE_STRING;
}

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

int tplg_update_automatic_attributes(snd_tplg_t *tplg, struct tplg_object *object,
				     struct tplg_object *parent)
{
	struct list_head *pos;
	int ret;

	if (!strcmp(object->class_name, "host") ||
	    !strcmp(object->class_name, "copier")) {
		tplg_set_stream_name(object);
	}

	/* now update all automatic attributes for all child objects */
	list_for_each(pos, &object->object_list) {
		struct tplg_object *child = list_entry(pos, struct tplg_object, list);

		ret = tplg_update_automatic_attributes(tplg, child, object);
		if (ret < 0)
			return ret;
	}

	return 0;
}
