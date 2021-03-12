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

/* Process the attribute values provided during object instantiation */
static int tplg_process_attributes(snd_config_t *cfg, struct tplg_object *object)
{
	snd_config_iterator_t i, next;
	struct list_head *pos;
	snd_config_t *n;
	const char *id;
	int ret;

	snd_config_for_each(i, next, cfg) {
		n = snd_config_iterator_entry(i);
		if (snd_config_get_id(n, &id) < 0)
			continue;

		list_for_each(pos, &object->attribute_list) {
			struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

			/* copy new value based on type */
			if (!strcmp(id, attr->name)) {
				/* cannot update immutable attributes */
				if (attr->constraint.mask & TPLG_CLASS_ATTRIBUTE_MASK_IMMUTABLE) {
					SNDERR("Warning: cannot update immutable attribute: %s for object %s\n",
					       attr->name, object->name);
					continue;
				}

				ret = tplg_parse_attribute_value(n, &object->attribute_list, true);
				if (ret < 0) {
					SNDERR("Error parsing attribute %s value: %d\n",
					       attr->name, ret);
					return ret;
				}

				attr->found = true;
				break;
			}
		}
	}

	return 0;
}

/* copy the preset attribute values and constraints */
static int tplg_copy_attribute(struct tplg_attribute *attr, struct tplg_attribute *ref_attr)
{
	struct list_head *pos1;

	snd_strlcpy(attr->name, ref_attr->name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	snd_strlcpy(attr->token_ref, ref_attr->token_ref, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	attr->found = ref_attr->found;
	attr->param_type = ref_attr->param_type;
	attr->cfg = ref_attr->cfg;
	attr->type = ref_attr->type;

	/* copy value */
	if (ref_attr->found) {
		switch (ref_attr->type) {
		case SND_CONFIG_TYPE_INTEGER:
			attr->value.integer = ref_attr->value.integer;
			break;
		case SND_CONFIG_TYPE_INTEGER64:
			attr->value.integer64 = ref_attr->value.integer64;
			break;
		case SND_CONFIG_TYPE_STRING:
			snd_strlcpy(attr->value.string, ref_attr->value.string,
				    SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
			break;
		case SND_CONFIG_TYPE_REAL:
		{
			attr->value.d = ref_attr->value.d;
			break;
		}
		case SND_CONFIG_TYPE_COMPOUND:
			break;
		default:
			SNDERR("Unsupported type %d for attribute %s\n", attr->type, attr->name);
			return -EINVAL;
		}
	}

	/* copy attribute constraints */
	INIT_LIST_HEAD(&attr->constraint.value_list);
	attr->constraint.value_ref = ref_attr->constraint.value_ref;
	list_for_each(pos1, &ref_attr->constraint.value_list) {
		struct tplg_attribute_ref *ref;
		struct tplg_attribute_ref *new_ref = calloc(1, sizeof(*new_ref));

		ref = list_entry(pos1, struct tplg_attribute_ref, list);
		memcpy(new_ref, ref, sizeof(*ref));
		list_add(&new_ref->list, &attr->constraint.value_list);
	}
	attr->constraint.mask = ref_attr->constraint.mask;
	attr->constraint.min = INT_MIN;
	attr->constraint.max = INT_MAX;

	return 0;
}

/* find the attribute with the mask TPLG_CLASS_ATTRIBUTE_MASK_UNIQUE and set the value */
static int tplg_object_set_unique_attribute(struct tplg_object *object, snd_config_t *cfg)
{
	struct tplg_attribute *attr;
	struct list_head *pos;
	const char *id;
	bool found = false;

	list_for_each(pos, &object->attribute_list) {
		attr = list_entry(pos, struct tplg_attribute, list);

		if (attr->constraint.mask & TPLG_CLASS_ATTRIBUTE_MASK_UNIQUE) {
			found = true;
			break;
		}
	}

	/* no unique attribute for object */
	if (!found) {
		SNDERR("No unique attribute set for object %s\n", object->name);
		return -EINVAL;
	}

	/* no need to check return value as it is already checked in tplg_create_object() */
	snd_config_get_id(cfg, &id);

	if (id[0] >= '0' && id[0] <= '9') {
		attr->value.integer = atoi(id);
		attr->type = SND_CONFIG_TYPE_INTEGER;
	} else {
		snd_strlcpy(attr->value.string, id, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
		attr->type = SND_CONFIG_TYPE_STRING;
	}

	attr->found = true;

	return 0;
}

/*
 * Create an object of class "class" by copying the attribute list, number of arguments
 * and default attribute values from the class definition. Objects can also be given
 * new values during instantiation and these will override the default values set in
 * the class definition
 */
struct tplg_object *
tplg_create_object(snd_tplg_t *tplg, snd_config_t *cfg, struct tplg_class *class,
		   struct tplg_object *parent ATTRIBUTE_UNUSED, struct list_head *list)
{
	struct tplg_object *object;
	struct tplg_elem *elem;
	struct list_head *pos;
	const char *id;
	char object_name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	int ret;

	if (!class) {
		SNDERR("Invalid class elem for object\n");
		return NULL;
	}

	/* get object index */
	if (snd_config_get_id(cfg, &id) < 0)
		return NULL;

	ret = snprintf(object_name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s.%s", class->name, id);
	if (ret > SNDRV_CTL_ELEM_ID_NAME_MAXLEN)
		SNDERR("Warning: object name %s truncated to %d characters\n", object_name,
		       SNDRV_CTL_ELEM_ID_NAME_MAXLEN);

	/* create and initialize object type element */
	elem = tplg_elem_new_common(tplg, NULL, object_name, SND_TPLG_TYPE_OBJECT);
	if (!elem) {
		SNDERR("Failed to create tplg elem for %s\n", object_name);
		return NULL;
	}

	object = calloc(1, sizeof(*object));
	if (!object) {
		return NULL;
	}

	object->cfg = cfg;
	object->elem = elem;
	object->num_args = class->num_args;
	snd_strlcpy(object->name, object_name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	snd_strlcpy(object->class_name, class->name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	object->type = class->type;
	INIT_LIST_HEAD(&object->attribute_list);
	elem->object = object;

	/* copy attributes from class */
	list_for_each(pos, &class->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);
		struct tplg_attribute *new_attr = calloc(1, sizeof(*attr));

		if (!new_attr)
			return NULL;

		ret = tplg_copy_attribute(new_attr, attr);
		if (ret < 0) {
			SNDERR("Error copying attribute %s\n", attr->name);
			free(new_attr);
			return NULL;
		}
		list_add_tail(&new_attr->list, &object->attribute_list);
	}

	/* set unique attribute for object */
	ret = tplg_object_set_unique_attribute(object, cfg);
	if (ret < 0)
		return NULL;

	/* process object attribute values */
	ret = tplg_process_attributes(cfg, object);
	if (ret < 0) {
		SNDERR("Failed to process attributes for %s\n", object->name);
		return NULL;
	}

	if (list)
		list_add_tail(&object->list, list);

	return object;
}

int tplg_create_new_object(snd_tplg_t *tplg, snd_config_t *cfg, struct tplg_elem *class_elem)
{
	snd_config_iterator_t i, next;
	struct tplg_class *class = class_elem->class;
	struct tplg_object *object;
	snd_config_t *n;
	const char *id;

	/* create all objects of the same class type */
	snd_config_for_each(i, next, cfg) {

		n = snd_config_iterator_entry(i);
		if (snd_config_get_id(n, &id) < 0)
			continue;

		/*
		 * Create object by duplicating the attributes and child objects from the class
		 * definiion
		 */
		object = tplg_create_object(tplg, n, class_elem->class, NULL, NULL);
		if (!object) {
			SNDERR("Error creating object for class %s\n", class->name);
			return -EINVAL;
		}
	}

	return 0;
}

/* create top-level topology objects */
int tplg_create_objects(snd_tplg_t *tplg, snd_config_t *cfg, void *private ATTRIBUTE_UNUSED)
{
	struct tplg_elem *class_elem;
	const char *id;

	if (snd_config_get_id(cfg, &id) < 0)
		return -EINVAL;

	/* look up class elem */
	class_elem = tplg_elem_lookup(&tplg->class_list, id,
				      SND_TPLG_TYPE_CLASS, SND_TPLG_INDEX_ALL);
	if (!class_elem) {
		SNDERR("No class elem found for %s\n", id);
		return -EINVAL;
	}

	return tplg_create_new_object(tplg, cfg, class_elem);
}

void tplg2_free_elem_object(struct tplg_elem *elem)
{
	struct tplg_object *object = elem->object;
	struct list_head *pos, *npos;
        struct tplg_attribute *attr;

	/*
	 * free args, attributes and tuples. Child objects will be freed when
	 * tplg->object_list is freed
	 */
        list_for_each_safe(pos, npos, &object->attribute_list) {
		attr = list_entry(pos, struct tplg_attribute, list);
		list_del(&attr->list);
                free(attr);
        }
}
