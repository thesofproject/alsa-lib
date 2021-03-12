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
#include "list.h"
#include "local.h"
#include "tplg_local.h"
#include "tplg2_local.h"
#include <ctype.h>

int tplg_create_component_object(struct tplg_object *object)
{
	struct tplg_comp_object *comp = &object->object_type.component;
	struct tplg_attribute *widget_type;
	int widget_id;

	widget_type = tplg_get_attribute_by_name(&object->attribute_list, "widget_type");
	if (!widget_type) {
		SNDERR("No widget_type given for %s\n", object->name);
		return -EINVAL;
	}

	widget_id = lookup_widget(widget_type->value.string);

	if (widget_id < 0) {
		SNDERR("Invalid widget ID for %s\n", object->name);
		return widget_id;
	}

	comp->widget_id = widget_id;
	return 0;
}

static int tplg_create_widget_elem(snd_tplg_t *tplg, struct tplg_object *object)
{
	struct tplg_comp_object *widget_object = &object->object_type.component;
	struct tplg_elem *widget_elem, *data_elem;
	struct snd_soc_tplg_dapm_widget *widget;
	char *class_name = object->class_name;
	char *elem_name;
	int ret;

	if (strcmp(class_name, "virtual_widget"))
		elem_name = object->name;
	else
		elem_name = strchr(object->name, '.') + 1;

	widget_elem = tplg_elem_new_common(tplg, NULL, elem_name,
						   SND_TPLG_TYPE_DAPM_WIDGET);
	if (!widget_elem)
		return -ENOMEM;

	/* create data elem for w */
	data_elem = tplg_elem_new_common(tplg, NULL, elem_name, SND_TPLG_TYPE_DATA);
        if (!data_elem)
                return -ENOMEM;

	ret = tplg_ref_add(widget_elem, SND_TPLG_TYPE_DATA, data_elem->id);
	if (ret < 0) {
		SNDERR("failed to add data elem %s to widget elem %s\n", data_elem->id,
		       widget_elem->id);
		return ret;
	}

	widget_object->widget_elem = widget_elem;
	widget = widget_elem->widget;
	widget->id = widget_object->widget_id;
	widget->size = widget_elem->size;
	snd_strlcpy(widget->name, widget_elem->id, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);

	return 0;
}

int tplg_build_comp_object(snd_tplg_t *tplg, struct tplg_object *object)
{
	struct tplg_attribute *pipeline_id;
	struct snd_soc_tplg_dapm_widget *widget;
	struct tplg_comp_object *comp = &object->object_type.component;
	struct tplg_elem *w_elem;
	struct list_head *pos;
	int ret;

	ret = tplg_create_widget_elem(tplg, object);
	if (ret < 0) {
		SNDERR("Failed to create widget elem for object %s\n", object->name);
		return ret;
	}
	w_elem = comp->widget_elem;
	widget = w_elem->widget;

	pipeline_id = tplg_get_attribute_by_name(&object->attribute_list, "pipeline_id");
	if (pipeline_id)
		w_elem->index = pipeline_id->value.integer;

	/* parse widget params from attributes */
	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

		if (!strcmp(attr->name, "stream_name") && attr->found) {
			snd_strlcpy(widget->sname, attr->value.string,
				    SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
			continue;
		}

		if (!attr->cfg)
			continue;

		/* widget type is already processed */
		if (!strcmp(attr->name, "type"))
			continue;

		ret = tplg_parse_dapm_widget_param(attr->cfg, widget, NULL);
		if (ret < 0) {
			SNDERR("Error parsing widget params for %s\n", object->name);
			return ret;
		}
	}

	tplg_dbg("Widget: %s id: %d stream_name: %s no_pm: %d",
		 w_elem->id, widget->id, widget->sname, widget->reg);

	return tplg_build_private_data(tplg, object);
}
