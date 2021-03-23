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

static int tplg_get_sample_size_from_format(char *format)
{
	if (!strcmp(format, "s32le") || !strcmp(format, "s24le") || !strcmp(format, "float") )
		return 4;

	if (!strcmp(format, "s16le"))
		return 2;

	return -EINVAL;
}

static int tplg_update_buffer_size(struct tplg_object *buffer_object,
				    struct tplg_object *pipeline_object)
{
	struct list_head *pos;
	struct tplg_attribute *size_attribute = NULL;
	char pipeline_format[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	int periods = 0;
	int sample_size;
	int channels = 0;
	int frames = 0;
	int rate = 0;
	int schedule_period = 0;

	/* get periods and channels from buffer object */
	list_for_each(pos, &buffer_object->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

		if (!strcmp(attr->name, "periods")) {
			if (attr->type == SND_CONFIG_TYPE_INTEGER) {
				periods = attr->value.integer;
			} else {
				SNDERR("Invalid value for periods for object %s \n",
				       buffer_object->name);
				return -EINVAL;
			}
		}

		if (!strcmp(attr->name, "channels")) {
			if (attr->type == SND_CONFIG_TYPE_INTEGER) {
				channels = attr->value.integer;
			} else {
				SNDERR("Invalid value for channels for object %s \n",
				       buffer_object->name);
				return -EINVAL;
			}
		}

		if (!strcmp(attr->name, "size"))
			size_attribute = attr;
	}

	if (!size_attribute) {
		SNDERR("Can't find size attribute for %s\n", buffer_object->name);
		return -EINVAL;
	}

	/* get schedule_period, channels, rate and format from pipeline object */
	list_for_each(pos, &pipeline_object->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

		if (!strcmp(attr->name, "period")) {
			if (attr->type == SND_CONFIG_TYPE_INTEGER) {
				schedule_period = attr->value.integer;
			} else {
				SNDERR("Invalid value for period for object %s \n",
				       pipeline_object->name);
				return -EINVAL;
			}
		}

		if (!strcmp(attr->name, "rate")) {
			if (attr->type == SND_CONFIG_TYPE_INTEGER) {
				rate = attr->value.integer;
			} else {
				SNDERR("Invalid value for rate for object %s \n",
				       pipeline_object->name);
				return -EINVAL;
			}
		}

		if (!strcmp(attr->name, "format")) {
			if (attr->type == SND_CONFIG_TYPE_STRING) {
				snd_strlcpy(pipeline_format, attr->value.string,
					    SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
			} else {
				SNDERR("Invalid format for pipeline %s \n",
				       pipeline_object->name);
				return -EINVAL;
			}
		}
	}

	sample_size = tplg_get_sample_size_from_format(pipeline_format);
	if (sample_size < 0) {
		SNDERR("Invalid value for sample size for object %s \n", pipeline_object->name);
		return sample_size;
	}

	/* compute buffer size */
	frames = (rate * schedule_period)/1000000;
	size_attribute->value.integer = periods * sample_size * channels * frames;
	if (!size_attribute->value.integer) {
		SNDERR("Invalid buffer size %d for %s \n",size_attribute->value.integer,
		       buffer_object->name);
		return -EINVAL;
	}

	size_attribute->found = true;
	size_attribute->type = SND_CONFIG_TYPE_INTEGER;

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

	if (!strcmp(object->class_name, "buffer")) {
		if (parent) {
			ret =  tplg_update_buffer_size(object, parent);
			if (ret < 0)
				return 0;
		}
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
