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
#include "tplg_local.h"
#include "tplg2_local.h"
#include <ctype.h>

static bool tplg_object_unique_attribute_match(struct tplg_object *object, char *input)
{
	struct tplg_attribute *attr;
	struct list_head *pos;
	bool found = false;

	/* find attribute with TPLG_CLASS_ATTRIBUTE_MASK_UNIQUE mask */
	list_for_each(pos, &object->attribute_list) {
		attr = list_entry(pos, struct tplg_attribute, list);

		if (attr->constraint.mask & TPLG_CLASS_ATTRIBUTE_MASK_UNIQUE) {
			found = true;
			break;
		}
	}

	if (!found)
		return false;

	/* check if the value matches based on type */
	switch (attr->type) {
	case SND_CONFIG_TYPE_INTEGER:
		if (attr->value.integer == atoi(input))
			return true;
		break;
	case SND_CONFIG_TYPE_STRING:
		if (!strcmp(attr->value.string, input))
			return true;
		break;
	default:
		break;
	}

	return false;
}

/* look up object based on class type and input value for the unique attribute */
struct tplg_object *tplg_object_elem_lookup(snd_tplg_t *tplg, const char *class_name,
					    char *input)
{
        struct list_head *pos;
        struct tplg_elem *elem;

        if (!class_name)
                return NULL;

        list_for_each(pos, &tplg->object_list) {
		struct tplg_object *object;

		elem = list_entry(pos, struct tplg_elem, list);
		object = elem->object;

		/* check if class_name natches */
                if (strcmp(object->class_name, class_name))
			continue;

		if (tplg_object_unique_attribute_match(object, input))
			return elem->object;
        }

        return NULL;
}

/* look up object based on class type and unique attribute value in a list */
struct tplg_object *tplg_object_lookup_in_list(struct list_head *list, const char *class_name,
					       char *input)
{
        struct list_head *pos;

        if (!class_name)
                return NULL;

        list_for_each(pos, list) {
		struct tplg_object *object = list_entry(pos, struct tplg_object, list);

		/* check if class_name natches */
                if (strcmp(object->class_name, class_name))
			continue;

		if (tplg_object_unique_attribute_match(object, input))
			return object;
        }

        return NULL;
}

/* Set child object attributes */
static int tplg_set_child_attributes(snd_tplg_t *tplg, snd_config_t *cfg,
				     struct tplg_object *object)
{
	snd_config_iterator_t i, next;
	snd_config_t *n;
	int ret;

	snd_config_for_each(i, next, cfg) {
		snd_config_iterator_t first, second;
		snd_config_t *first_cfg, *second_cfg;
		struct tplg_elem *class_elem;
		struct tplg_object *child;
		const char *class_name, *index_str, *attribute_name;

		n = snd_config_iterator_entry(i);
		if (snd_config_get_id(n, &class_name) < 0)
			continue;

		if (snd_config_get_type(n) != SND_CONFIG_TYPE_COMPOUND)
	                continue;

		/* check if it is a valid class name */
		class_elem = tplg_elem_lookup(&tplg->class_list, class_name,
					      SND_TPLG_TYPE_CLASS, SND_TPLG_INDEX_ALL);
		if (!class_elem)
			continue;

		/* get index */
		first = snd_config_iterator_first(n);
		first_cfg = snd_config_iterator_entry(first);
		if (snd_config_get_id(first_cfg, &index_str) < 0)
			continue;

		if (snd_config_get_type(first_cfg) != SND_CONFIG_TYPE_COMPOUND) {
			SNDERR("No attribute name for child %s.%s\n", class_name, index_str);
			return -EINVAL;
		}

		/* the next node can either be an attribute name or a child class */
		second = snd_config_iterator_first(first_cfg);
		second_cfg = snd_config_iterator_entry(second);
		if (snd_config_get_id(second_cfg, &attribute_name) < 0)
			continue;

		/* get object of type class_name and unique attribute value */
		child = tplg_object_lookup_in_list(&object->object_list, class_name, (char *)index_str);
		if (!child) {
			SNDERR("No child %s.%s found for object %s\n",
				class_name, index_str, object->name);
			return -EINVAL;
		}

		/*
		 * if the second conf node is an attribute name, set the value but do not
		 * override the object value if already set.
		 */
		if (snd_config_get_type(second_cfg) != SND_CONFIG_TYPE_COMPOUND) {
			ret = tplg_parse_attribute_value(second_cfg, &child->attribute_list, false);

			if (ret < 0) {
				SNDERR("Failed to set attribute for object %s\n", object->name);
				return ret;
			}
			continue;
		}

		/* otherwise pass it down to the child object */
		ret = tplg_set_child_attributes(tplg, first_cfg, child);
		if (ret < 0)
			return ret;
	}

	return 0;
}

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

static int create_child_object_instance(snd_tplg_t *tplg, snd_config_t *cfg,
					struct tplg_object *parent, struct list_head *list,
					struct tplg_elem *class_elem)
{
	snd_config_iterator_t i, next;
	snd_config_t *n;
	const char *id;

	snd_config_for_each(i, next, cfg) {
		struct tplg_object *object;

		n = snd_config_iterator_entry(i);
		if (snd_config_get_id(n, &id) < 0)
			continue;

		object = tplg_create_object(tplg, n, class_elem->class, parent, list);
		if (!object) {
			SNDERR("Error creating child %s for parent %s\n", id, parent->name);
			return -EINVAL;
		}
	}

	return 0;
}

/* create child object */
int tplg_create_child_object_type(snd_tplg_t *tplg, snd_config_t *cfg,
			     struct tplg_object *parent, struct list_head *list)
{
	snd_config_iterator_t i, next;
	struct tplg_elem *class_elem;
	snd_config_t *n;
	const char *id;
	int ret;

	snd_config_for_each(i, next, cfg) {
		n = snd_config_iterator_entry(i);
		if (snd_config_get_id(n, &id) < 0)
			continue;

		/* check if it is a valid object */
		class_elem = tplg_elem_lookup(&tplg->class_list, id,
					      SND_TPLG_TYPE_CLASS, SND_TPLG_INDEX_ALL);

		if (!class_elem)
			continue;

		ret = create_child_object_instance(tplg, n, parent, list, class_elem);
		if (ret < 0) {
			SNDERR("Error creating %s type child object for parent %s\n",
			       class_elem->id, parent->name);
			return ret;
		}
	}

	return 0;
}

/* create child objects that are part of the parent object instance */
static int tplg_create_child_objects(snd_tplg_t *tplg, snd_config_t *cfg,
				   struct tplg_object *parent)
{
	snd_config_iterator_t i, next;
	snd_config_t *n;
	const char *id;
	int ret;

	snd_config_for_each(i, next, cfg) {
		n = snd_config_iterator_entry(i);
		if (snd_config_get_id(n, &id) < 0)
			continue;

		if (strcmp(id, "Object"))
			continue;

		/* create object */
		ret = tplg_create_child_object_type(tplg, n, parent, &parent->object_list);
		if (ret < 0) {
			SNDERR("Error creating child objects for %s\n", parent->name);
			return ret;
		}
	}

	return 0;
}

/*
 * Child objects could have arguments inherited from the parent. Update the name now that the
 * parent has been instantiated and values updated.
 */
static int tplg_update_object_name_from_args(struct tplg_object *object)
{
	struct list_head *pos;
	char string[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	int ret;

	snprintf(string, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s", object->class_name);

	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);
		char new_str[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];

		if (attr->param_type != TPLG_CLASS_PARAM_TYPE_ARGUMENT)
			continue;

		switch (attr->type) {
		case SND_CONFIG_TYPE_INTEGER:
			ret = snprintf(new_str, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s.%ld",
				 string, attr->value.integer);
			if (ret > SNDRV_CTL_ELEM_ID_NAME_MAXLEN) {
				SNDERR("Object name too long for %s\n", object->name);
				return -EINVAL;
			}
			snd_strlcpy(string, new_str, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
			break;
		case SND_CONFIG_TYPE_STRING:
			ret = snprintf(new_str, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s.%s",
				 string, attr->value.string);
			if (ret > SNDRV_CTL_ELEM_ID_NAME_MAXLEN) {
				SNDERR("Object name too long for %s\n", object->name);
				return -EINVAL;
			}
			snd_strlcpy(string, new_str, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
			break;
		default:
			break;
		}
	}

	snd_strlcpy(object->name, string, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);

	return 0;
}

/* update attributes inherited from parent */
static int tplg_update_attributes_from_parent(struct tplg_object *object,
					    struct tplg_object *ref_object)
{
	struct list_head *pos, *pos1;

	/* update object attribute values from reference object */
	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr =  list_entry(pos, struct tplg_attribute, list);

		if (attr->found)
			continue;

		list_for_each(pos1, &ref_object->attribute_list) {
			struct tplg_attribute *ref_attr;

			ref_attr = list_entry(pos1, struct tplg_attribute, list);
			if (!ref_attr->found)
				continue;

			if (!strcmp(attr->name, ref_attr->name)) {
				switch (ref_attr->type) {
				case SND_CONFIG_TYPE_INTEGER:
					attr->value.integer = ref_attr->value.integer;
					attr->type = ref_attr->type;
					break;
				case SND_CONFIG_TYPE_INTEGER64:
					attr->value.integer64 = ref_attr->value.integer64;
					attr->type = ref_attr->type;
					break;
				case SND_CONFIG_TYPE_STRING:
					snd_strlcpy(attr->value.string,
						    ref_attr->value.string,
						    SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
					attr->type = ref_attr->type;
					break;
				case SND_CONFIG_TYPE_REAL:
					attr->value.d = ref_attr->value.d;
					attr->type = ref_attr->type;
					break;
				default:
					SNDERR("Unsupported type %d for attribute %s\n",
						attr->type, attr->name);
					return -EINVAL;
				}
				attr->found = true;
			}
		}
	}

	return 0;
}

/* Propdagate the updated attribute values to child o bjects */
static int tplg_process_child_objects(struct tplg_object *parent)
{
	struct list_head *pos;
	int ret;

	list_for_each(pos, &parent->object_list) {
		struct tplg_object *object = list_entry(pos, struct tplg_object, list);

		/* update attribute values inherited from parent */
		ret = tplg_update_attributes_from_parent(object, parent);
		if (ret < 0) {
			SNDERR("failed to update arguments for %s\n", object->name);
			return ret;
		}

		/* update object name after args update */
		ret = tplg_update_object_name_from_args(object);
		if (ret < 0)
			return ret;

		/* update the object elem ID as well */
		snd_strlcpy(object->elem->id, object->name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);

		/* now process its child objects */
		ret = tplg_process_child_objects(object);
		if (ret < 0) {
			SNDERR("Cannot update child object for %s\n", object->name);
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

static int tplg_object_attributes_sanity_check(struct tplg_object *object)
{
	struct list_head *pos;

	/* sanity check for object attributes */
	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

		/* check if mandatory and value provided */
		if ((attr->constraint.mask & TPLG_CLASS_ATTRIBUTE_MASK_MANDATORY) &&
		    !(attr->constraint.mask & TPLG_CLASS_ATTRIBUTE_MASK_IMMUTABLE) &&
		    !attr->found) {
			SNDERR("Mandatory attribute %s not found for object %s\n", attr->name,
			       object->name);
			return -EINVAL;
		}

		/* translate string values to integer if needed to be added to private data */
		switch (attr->type) {
		case SND_CONFIG_TYPE_STRING:
		{
			struct list_head *pos1;

			/* skip attributes with no pre-defined valid values */
			if (list_empty(&attr->constraint.value_list))
				continue;

			/* Translate the string value to integer if needed */
			list_for_each(pos1, &attr->constraint.value_list) {
				struct tplg_attribute_ref *v;

				v = list_entry(pos1, struct tplg_attribute_ref, list);

				if (!strcmp(attr->value.string, v->string)) {
					if (v->value != -EINVAL) {
						attr->value.integer = v->value;
						attr->type = SND_CONFIG_TYPE_INTEGER;
					}
					break;
				}
			}
			break;
		}
		default:
			break;
		}
	}

	/* recursively check all child objects */
	list_for_each(pos, &object->object_list) {
		struct tplg_object *child = list_entry(pos, struct tplg_object, list);
		int ret;

		ret = tplg_object_attributes_sanity_check(child);
		if (ret < 0)
			return ret;
	}

	return 0;
}

/* Copy object from class definition and create the topology element for the newly copied object */
static int tplg_copy_object(snd_tplg_t *tplg, struct tplg_object *src, struct tplg_object *dest,
			     struct list_head *list)
{
	struct tplg_elem *elem;
	struct list_head *pos;
	int ret;

	if (!src || !dest) {
		SNDERR("Invalid src/dest object\n");
		return -EINVAL;
	}

	dest->num_args = src->num_args;
	snd_strlcpy(dest->name, src->name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	snd_strlcpy(dest->class_name, src->class_name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	dest->type = src->type;
	dest->cfg = src->cfg;
	INIT_LIST_HEAD(&dest->tuple_set_list);
	INIT_LIST_HEAD(&dest->attribute_list);
	INIT_LIST_HEAD(&dest->object_list);

	/* copy attributes */
	list_for_each(pos, &src->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);
		struct tplg_attribute *new_attr = calloc(1, sizeof(*attr));

		if (!new_attr)
			return -ENOMEM;

		ret = tplg_copy_attribute(new_attr, attr);
		if (ret < 0) {
			SNDERR("Error copying attribute %s\n", attr->name);
			free(new_attr);
			return -ENOMEM;
		}
		list_add_tail(&new_attr->list, &dest->attribute_list);
	}

	switch(src->type) {
	case SND_TPLG_CLASS_TYPE_COMPONENT:
	{
		struct tplg_comp_object *dest_comp_object = &dest->object_type.component;
		struct tplg_comp_object *src_comp_object = &src->object_type.component;

		memcpy(dest_comp_object, src_comp_object, sizeof(*dest_comp_object));
		break;
	}
	default:
		break;
	}

	/* copy its child objects */
	list_for_each(pos, &src->object_list) {
		struct tplg_object *child = list_entry(pos, struct tplg_object, list);
		struct tplg_object *new_child = calloc(1, sizeof(*new_child));

		ret = tplg_copy_object(tplg, child, new_child, &dest->object_list);
		if (ret < 0) {
			SNDERR("error copying child object %s\n", child->name);
			return ret;
		}
	}

	/* create tplg elem of type SND_TPLG_TYPE_OBJECT */
	elem = tplg_elem_new_common(tplg, NULL, dest->name, SND_TPLG_TYPE_OBJECT);
	if (!elem)
		return -ENOMEM;
	elem->object = dest;
	dest->elem = elem;

	list_add_tail(&dest->list, list);
	return 0;
}

/* class definitions may have pre-defined objects. Copy these into the object */
static int tplg_copy_child_objects(snd_tplg_t *tplg, struct tplg_class *class,
				  struct tplg_object *object)
{
	struct list_head *pos;
	int ret;

	/* copy child objects */
	list_for_each(pos, &class->object_list) {
		struct tplg_object *obj = list_entry(pos, struct tplg_object, list);
		struct tplg_object *new_obj = calloc(1, sizeof(*obj));

		ret = tplg_copy_object(tplg, obj, new_obj, &object->object_list);
		if (ret < 0) {
			free(new_obj);
			return ret;
		}
	}

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
	INIT_LIST_HEAD(&object->tuple_set_list);
	INIT_LIST_HEAD(&object->attribute_list);
	INIT_LIST_HEAD(&object->object_list);
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

	/* sanity check */
	switch(object->type) {
	case SND_TPLG_CLASS_TYPE_PIPELINE:
		ret = tplg_create_pipeline_object(class, object);
		if (ret < 0) {
			SNDERR("Failed to create pipeline object for %s\n", object->name);
			return NULL;
		}
		break;
	case SND_TPLG_CLASS_TYPE_DAI:
		ret = tplg_create_dai_object(class, object);
		if (ret < 0) {
			SNDERR("Failed to create DAI object for %s\n", object->name);
			return NULL;
		}
		break;
	case SND_TPLG_CLASS_TYPE_COMPONENT:
		ret = tplg_create_component_object(object);
		if (ret < 0) {
			SNDERR("Failed to create component object for %s\n", object->name);
			return NULL;
		}
		break;
	default:
		break;
	}

	/* now copy child objects */
	ret = tplg_copy_child_objects(tplg, class, object);
	if (ret < 0) {
		SNDERR("Failed to create DAI object for %s\n", object->name);
		return NULL;
	}

	/* create child objects that are part of the object instance */
	ret = tplg_create_child_objects(tplg, cfg, object);
	if (ret < 0) {
		SNDERR("failed to create child objects for %s\n", object->name);
		return NULL;
	}

	/* pass down the object attributes to its child objects */
	ret = tplg_process_child_objects(object);
	if (ret < 0) {
		SNDERR("failed to create child objects for %s\n", object->name);
		return NULL;
	}

	/* process child object attributes set explictly in the parent object */
	ret = tplg_set_child_attributes(tplg, cfg, object);
	if (ret < 0) {
		SNDERR("failed to set child attributes for %s\n", object->name);
		return NULL;
	}

	/* update automatic attributes in object */
	ret = tplg_update_automatic_attributes(tplg, object, parent);
	if (ret < 0) {
		SNDERR("failed to update automatic attributes for %s\n", object->name);
		return NULL;
	}

	if (list)
		list_add_tail(&object->list, list);

	return object;
}

static int tplg2_get_bool(struct tplg_attribute *attr)
{
	if (attr->type != SND_CONFIG_TYPE_INTEGER)
		return -EINVAL;

	if (attr->value.integer != 0 && attr->value.integer != 1)
		return -EINVAL;

	return attr->value.integer;
}

static int tplg_get_object_tuple_set(struct tplg_object *object, struct tplg_tuple_set **out,
				     const char *token_ref)
{
	struct list_head *pos, *_pos;
	struct tplg_tuple_set *set;
	const char *type;
	char *tokenref_str;
	int len, set_type;
	int size;

	/* get tuple set type */
	type = strchr(token_ref, '.');
	if (!type) {
		SNDERR("No type given for tuple set: '%s' in object: '%s'\n",
		       token_ref, object->name);
		return -EINVAL;
	}

	set_type = get_tuple_type(type + 1);
	if (set_type < 0) {
		SNDERR("Invalid type for tuple set: '%s' in object: '%s'\n",
		       token_ref, object->name);
		return -EINVAL;
	}

	/* get tuple token ref name */
	len = strlen(token_ref) - strlen(type) + 1;
	tokenref_str = calloc(1, len);
	snd_strlcpy(tokenref_str, token_ref, len);

	/* realloc set if set is found */
	list_for_each_safe(pos, _pos, &object->tuple_set_list) {
		struct tplg_tuple_set *set2;

		set = list_entry(pos, struct tplg_tuple_set, list);

		if (set->type == (unsigned int)set_type && !(strcmp(set->token_ref, tokenref_str))) {

			set->num_tuples++;
			size = sizeof(*set) + set->num_tuples * sizeof(struct tplg_tuple);
			set2 = realloc(set, size);
			if (!set2)
				return -ENOMEM;
			list_del(&set->list);

			set = set2;
			list_add_tail(&set->list, &object->tuple_set_list);
			*out = set;
			return 0;
		}
	}

	/* else create a new set and add it to the object's tuple_set_list */
	size = sizeof(*set) + sizeof(struct tplg_tuple);
	set = calloc(1, size);
	set->num_tuples = 1;
	set->type = set_type;
	snd_strlcpy(set->token_ref, tokenref_str, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	list_add_tail(&set->list, &object->tuple_set_list);
	*out = set;

	return 0;
}

static int tplg_build_object_tuple_set_from_attributes(struct tplg_object *object,
						       struct tplg_attribute *attr)
{
	struct tplg_tuple_set *set;
	struct tplg_tuple *tuple;
	struct list_head *pos;
	int ret;

	/* get tuple set if it exists already or create one */
	ret = tplg_get_object_tuple_set(object, &set, attr->token_ref);
	if (ret < 0) {
		SNDERR("Invalid tuple set for '%s'\n", object->name);
		return ret;
	}

	/* update set with new tuple */
	tuple = &set->tuple[set->num_tuples - 1];
	snd_strlcpy(tuple->token, attr->name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	switch (set->type) {
	case SND_SOC_TPLG_TUPLE_TYPE_UUID:
	{
		const char *value;

		if (snd_config_get_string(attr->cfg, &value) < 0)
			break;
		if (get_uuid(value, tuple->uuid) < 0) {
			SNDERR("failed to get uuid from string %s\n", value);
			return -EINVAL;
		}
		tplg_dbg("\t\tuuid string %s ", value);
		tplg_dbg("\t\t%s = 0x%x", tuple->token, tuple->uuid);
		break;
	}
	case SND_SOC_TPLG_TUPLE_TYPE_STRING:
		snd_strlcpy(tuple->string, attr->value.string,
			    SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
		tplg_dbg("\t\t%s = %s", tuple->token, tuple->string);
		break;

	case SND_SOC_TPLG_TUPLE_TYPE_BOOL:
	{
		int ret = tplg2_get_bool(attr);

		if (ret < 0) {
			SNDERR("Invalid value for tuple %s\n", tuple->token);
			return tuple->value;
		}
		tuple->value = ret;
		tplg_dbg("\t\t%s = %d", tuple->token, tuple->value);
		break;
	}
	case SND_SOC_TPLG_TUPLE_TYPE_BYTE:
	case SND_SOC_TPLG_TUPLE_TYPE_SHORT:
	case SND_SOC_TPLG_TUPLE_TYPE_WORD:
	{
		unsigned int tuple_val = 0;

		switch(attr->type) {
		case SND_CONFIG_TYPE_STRING:
			if (!attr->constraint.value_ref) {
				SNDERR("Invalid tuple value type for %s\n", tuple->token);
				return -EINVAL;
			}

			/* convert attribute string values to corresponding integer value */
			list_for_each(pos, &attr->constraint.value_list) {
				struct tplg_attribute_ref *v;

				v = list_entry(pos, struct tplg_attribute_ref, list);
				if (!strcmp(attr->value.string, v->string))
					if (v->value != -EINVAL)
						tuple_val = v->value;
			}
			break;
		case SND_CONFIG_TYPE_INTEGER:
			tuple_val = attr->value.integer;
			break;
		case SND_CONFIG_TYPE_INTEGER64:
			tuple_val = attr->value.integer64;
			break;
		default:
			SNDERR("Invalid value type %d for tuple %s for object %s \n", attr->type,
			       tuple->token, object->name);
			return -EINVAL;
		}

		if ((set->type == SND_SOC_TPLG_TUPLE_TYPE_WORD
				&& tuple_val > UINT_MAX)
			|| (set->type == SND_SOC_TPLG_TUPLE_TYPE_SHORT
				&& tuple_val > USHRT_MAX)
			|| (set->type == SND_SOC_TPLG_TUPLE_TYPE_BYTE
				&& tuple_val > UCHAR_MAX)) {
			SNDERR("tuple %s: invalid value", tuple->token);
			return -EINVAL;
		}

		tuple->value = tuple_val;
		tplg_dbg("\t\t%s = 0x%x", tuple->token, tuple->value);
		break;
	}
	default:
		break;
	}

	return 0;
}

/* Build tuple sets from object attributes */
int tplg_build_object_tuple_sets(struct tplg_object *object)
{
	struct list_head *pos;
	int ret = 0;

	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

		if (attr->constraint.mask & TPLG_CLASS_ATTRIBUTE_MASK_DEPRECATED) {
			if (attr->found)
				SNDERR("Warning: attibute %s decprecated\n", attr->name);
			continue;
		}

		if (strcmp(attr->token_ref, "")) {
			if (!attr->found)
				continue;

			ret = tplg_build_object_tuple_set_from_attributes(object, attr);
			if (ret < 0)
				return ret;
		}
	}

	return ret;
}

int tplg_build_private_data(snd_tplg_t *tplg, struct tplg_object *object)
{
	struct snd_soc_tplg_private *priv;
	struct tplg_elem *data_elem;
	struct list_head *pos;
	int ret;

	/* build tuple sets for object */
	ret = tplg_build_object_tuple_sets(object);
	if (ret < 0)
		return ret;

	data_elem = tplg_elem_lookup(&tplg->pdata_list, object->name,
				      SND_TPLG_TYPE_DATA, SND_TPLG_INDEX_ALL);
	if(!data_elem)
		return 0;

	priv = data_elem->data;

	/* build link private data from tuple sets */
	list_for_each(pos, &object->tuple_set_list) {
		struct tplg_tuple_set *set = list_entry(pos, struct tplg_tuple_set, list);
		struct tplg_elem *token_elem;

		if (!set->token_ref) {
			SNDERR("No valid token ref for tuple set type %d\n", set->type);
			return -EINVAL;
		}

		/* get reference token elem */
		token_elem = tplg_elem_lookup(&tplg->token_list, set->token_ref,
					      SND_TPLG_TYPE_TOKEN, SND_TPLG_INDEX_ALL);
		if (!token_elem) {
			SNDERR("No valid tokens for ref %s\n", set->token_ref);
			return -EINVAL;
		}

		ret = scan_tuple_set(data_elem, set, token_elem->tokens, priv ? priv->size : 0);
		if (ret < 0)
			return ret;

		/* priv gets modified while scanning new sets */
		priv = data_elem->data;
	}

	tplg_dbg("Object %s built", object->name);

	return 0;
}

static int tplg_build_manifest_object(snd_tplg_t *tplg, struct tplg_object *object) {
	struct snd_soc_tplg_manifest *manifest;
	struct tplg_elem *m_elem;
	struct list_head *pos;
	int ret;

	if (!list_empty(&tplg->manifest_list)) {
		SNDERR("Manifest data already exists");
		return -EINVAL;
	}

	m_elem = tplg_elem_new_common(tplg, NULL, object->name, SND_TPLG_TYPE_MANIFEST);
	if (!m_elem)
		return -ENOMEM;

	manifest = m_elem->manifest;
	manifest->size = m_elem->size;

	list_for_each(pos, &object->object_list) {
		struct tplg_object *child = list_entry(pos, struct tplg_object, list);

		if (!object->cfg)
			continue;

		if (!strcmp(child->class_name, "data")) {
			struct tplg_attribute *name;

			name = tplg_get_attribute_by_name(&object->attribute_list, "name");

			ret = tplg_ref_add(m_elem, SND_TPLG_TYPE_DATA, name->value.string);
			if (ret < 0) {
				SNDERR("failed to add data elem %s to manifest elem %s\n",
				       name->value.string, m_elem->id);
				return ret;
			}
		}
	}

	tplg_dbg(" Manifest: %s", m_elem->id);

	return 0;
}

static int tplg_build_data_object(snd_tplg_t *tplg, struct tplg_object *object) {
	struct tplg_attribute *bytes, *name;
	struct tplg_elem *data_elem;
	int ret;

	name = tplg_get_attribute_by_name(&object->attribute_list, "name");
	if (!name) {
		SNDERR("invalid name for data object: %s", object->name);
		return -EINVAL;
	}

	/* check if data elem exists already */
	data_elem = tplg_elem_lookup(&tplg->widget_list, name->value.string,
				      SND_TPLG_TYPE_DATA, SND_TPLG_INDEX_ALL);
	if (!data_elem) {
		/* create data elem */
		data_elem = tplg_elem_new_common(tplg, NULL, name->value.string,
						 SND_TPLG_TYPE_DATA);
	        if (!data_elem) {
			SNDERR("failed to create data elem for %s\n", object->name);
	                return -EINVAL;
		}
	}

	bytes = tplg_get_attribute_by_name(&object->attribute_list, "bytes");

	if (!bytes || !bytes->cfg)
		return 0;

	ret = tplg_parse_data_hex(bytes->cfg, data_elem, 1);
	if (ret < 0)
		SNDERR("failed to parse byte for data: %s", object->name);

	tplg_dbg("data: %s", name->value.string);

	return ret;
}


static int tplg_build_base_object(snd_tplg_t *tplg, struct tplg_object *object)
{
	if (!strcmp(object->class_name, "manifest"))
		return tplg_build_manifest_object(tplg, object);

	if (!strcmp(object->class_name, "data"))
		return tplg_build_data_object(tplg, object);

	if (!strcmp(object->class_name, "connection"))
		return tplg_build_dapm_route(tplg, object);


	return 0;
}

static int tplg_build_object(snd_tplg_t *tplg, struct tplg_object *object)
{
	struct list_head *pos;
	int ret;

	switch (object->type) {
	case SND_TPLG_CLASS_TYPE_COMPONENT:
	{
		ret = tplg_build_comp_object(tplg, object);
		if (ret < 0) {
			SNDERR("Failed to build comp object %s\n", object->name);
			return ret;
		}
		break;
	}
	case SND_TPLG_CLASS_TYPE_DAI:
		ret = tplg_build_dai_object(tplg, object);
		if (ret < 0) {
			SNDERR("Failed to build DAI object %s\n", object->name);
			return ret;
		}
		break;
	case SND_TPLG_CLASS_TYPE_PCM:
		ret = tplg_build_pcm_type_object(tplg, object);
		if (ret < 0) {
			SNDERR("Failed to build PCM class object %s\n", object->name);
			return ret;
		}
		break;
	case SND_TPLG_CLASS_TYPE_BASE:
		ret = tplg_build_base_object(tplg, object);
		if (ret < 0) {
			SNDERR("Failed to build object %s\n", object->name);
			return ret;
		}
		break;
	}

	/* build child objects */
	list_for_each(pos, &object->object_list) {
		struct tplg_object *child = list_entry(pos, struct tplg_object, list);

		ret = tplg_build_object(tplg, child);
		if (ret < 0) {
			SNDERR("Failed to build object\n", child->name);
			return ret;
		}
	}

	return 0;
}


int tplg_create_new_object(snd_tplg_t *tplg, snd_config_t *cfg, struct tplg_elem *class_elem)
{
	snd_config_iterator_t i, next;
	struct tplg_class *class = class_elem->class;
	struct tplg_object *object;
	snd_config_t *n;
	const char *id;
	int ret;

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

		/* check if all mandatory values are present and translate string values to integer */
		ret = tplg_object_attributes_sanity_check(object);
		if (ret < 0) {
			SNDERR("Object %s failed sanity check\n", object->name);
			return -EINVAL;
		}

		/* Build the object by creating the topology element */
		ret = tplg_build_object(tplg, object);
		if (ret < 0) {
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
        struct tplg_tuple_set *set;

	/*
	 * free args, attributes and tuples. Child objects will be freed when
	 * tplg->object_list is freed
	 */
        list_for_each_safe(pos, npos, &object->attribute_list) {
		attr = list_entry(pos, struct tplg_attribute, list);
		list_del(&attr->list);
                free(attr);
        }

	list_for_each_safe(pos, npos, &object->tuple_set_list) {
                set = list_entry(pos, struct tplg_tuple_set, list);
                list_del(&set->list);
                free(set);
        }
}
