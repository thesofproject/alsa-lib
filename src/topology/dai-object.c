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

int tplg_create_dai_object(struct tplg_class *class, struct tplg_object *object)
{
	struct list_head *pos;

	/* check if child objects are of the right type */
	list_for_each(pos, &class->object_list) {
		struct tplg_object *obj = list_entry(pos, struct tplg_object, list);

		switch (obj->type) {
		case SND_TPLG_CLASS_TYPE_BASE:
			if (!strcmp(obj->class_name, "endpoint"))
				break;

			SNDERR("Unexpected child class %s for dai %s\n", obj->class_name,
			       object->name);
			return -EINVAL;
		case SND_TPLG_CLASS_TYPE_COMPONENT:
			break;
		default:
			SNDERR("Unexpected child type %d for %s\n", obj->type, object->name);
			return -EINVAL;
		}
	}

	return 0;
}

static int tplg_create_link_elem(snd_tplg_t *tplg, struct tplg_object *object)
{
	struct tplg_attribute *stream_name, *id;
	struct tplg_attribute *default_hw_cfg;
	struct tplg_dai_object *dai = &object->object_type.dai;
	struct tplg_elem *link_elem, *data_elem;
	struct snd_soc_tplg_link_config *link;
	int ret;

	stream_name = tplg_get_attribute_by_name(&object->attribute_list, "stream_name");
	id = tplg_get_attribute_by_name(&object->attribute_list, "id");
	default_hw_cfg = tplg_get_attribute_by_name(&object->attribute_list, "default_hw_config");

	if (!stream_name || stream_name->type != SND_CONFIG_TYPE_STRING) {
		SNDERR("No DAI name for %s\n", object->name);
		return -EINVAL;
	}

	link_elem = tplg_elem_new_common(tplg, NULL, stream_name->value.string, SND_TPLG_TYPE_BE);
	if (!link_elem)
		return -ENOMEM;
	dai->link_elem = link_elem;

	link = link_elem->link;
	link->size = link_elem->size;
	snd_strlcpy(link->name, link_elem->id, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	link->default_hw_config_id = default_hw_cfg->value.integer;
	link->id = id->value.integer;

	/* create data elem for link */
	data_elem = tplg_elem_new_common(tplg, NULL, object->name, SND_TPLG_TYPE_DATA);
        if (!data_elem)
                return -ENOMEM;

	ret = tplg_ref_add(link_elem, SND_TPLG_TYPE_DATA, data_elem->id);
	if (ret < 0) {
		SNDERR("failed to add data elem %s to link elem %s\n", data_elem->id,
		       link_elem->id);
		return ret;
	}

	return 0;
}

int tplg_build_dai_object(snd_tplg_t *tplg, struct tplg_object *object)
{
	struct tplg_dai_object *dai = &object->object_type.dai;
	struct snd_soc_tplg_link_config *link;
	struct tplg_elem *l_elem;
	struct list_head *pos, *_pos;
	int i = 0;
	int ret;

	ret = tplg_create_link_elem(tplg, object);
	if (ret < 0) {
		SNDERR("Failed to create widget elem for object\n", object->name);
		return ret;
	}
	l_elem = dai->link_elem;
	link = l_elem->link;

	list_for_each(pos, &object->object_list) {
		struct tplg_object *child = list_entry(pos, struct tplg_object, list);
		struct list_head *pos1;

		if (!strcmp(child->class_name, "hw_config")) {
			struct tplg_attribute *id;
			struct snd_soc_tplg_hw_config *hw_cfg = &link->hw_config[i++];

			/* set hw_config ID */
			id = tplg_get_attribute_by_name(&child->attribute_list, "id");
			if (!id || id->type != SND_CONFIG_TYPE_INTEGER) {
				SNDERR("No ID for hw_config %s\n", child->name);
				return -EINVAL;
			}
			hw_cfg->id = id->value.integer;

			/* parse hw_config params from attributes */
			list_for_each(pos1, &child->attribute_list) {
				struct tplg_attribute *attr;

				attr = list_entry(pos1, struct tplg_attribute, list);
				if (!attr->cfg)
					continue;

				ret = tplg_set_hw_config_param(attr->cfg, hw_cfg);
				if (ret < 0) {
					SNDERR("Error parsing hw_config for object %s\n",
					       object->name);
					return ret;
				}
			}
			tplg_dbg("HW Config: %d", hw_cfg->id);
		}

		if (!strcmp(child->class_name, "pdm_config")) {
			/* build tuple sets for pdm_config object */
			ret = tplg_build_object_tuple_sets(child);
			if (ret < 0)
				return ret;

			list_for_each_safe(pos1, _pos, &child->tuple_set_list) {
				struct tplg_tuple_set *set;
				set = list_entry(pos1, struct tplg_tuple_set, list);
				list_del(&set->list);
				list_add_tail(&set->list, &object->tuple_set_list);
			}
		}
	}

	/* parse link params from attributes */
	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

		if (!attr->cfg)
			continue;

		ret = tplg_parse_link_param(tplg, attr->cfg, link, NULL);
		if (ret < 0) {
			SNDERR("Error parsing hw_config for object %s\n",
			       object->name);
			return ret;
		}
	}

	link->num_hw_configs = i;
	tplg_dbg("Link elem: %s num_hw_configs: %d", l_elem->id, link->num_hw_configs);

	return tplg_build_private_data(tplg, object);
}
