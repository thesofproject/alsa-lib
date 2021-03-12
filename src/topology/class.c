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

/* save valid values for attributes */
static int tplg_parse_constraint_valid_values(snd_tplg_t *tplg, snd_config_t *cfg,
					      struct attribute_constraint *c,
					      char *name)
{
	snd_config_iterator_t i, next;
	snd_config_t *n;
	int err;

	snd_config_for_each(i, next, cfg) {
		struct tplg_attribute_ref *v;
		const char *id, *s;

		n = snd_config_iterator_entry(i);
		if (snd_config_get_id(n, &id) < 0) {
			SNDERR("invalid reference value for '%s'\n", name);
			return -EINVAL;
		}

		err = snd_config_get_string(n, &s);
		if (err < 0) {
			SNDERR("Invalid value for '%s'\n", name);
			return err;
		}

		v = calloc(1, sizeof(*v));

		/*
		 * some attributes come with valid string values that translate to integer values
		 */
		if (c->value_ref) {
			struct tplg_elem *token_elem;

			v->string = s;

			/* get reference token elem */
			token_elem = tplg_elem_lookup(&tplg->token_list,
						      c->value_ref,
						      SND_TPLG_TYPE_TOKEN, SND_TPLG_INDEX_ALL);
			if (!token_elem) {
				SNDERR("No valid token elem for ref '%s'\n",
					c->value_ref);
				free(v);
				return -EINVAL;
			}

			/* save the value corresponding to the string */
			v->value = get_token_value(s, token_elem->tokens);
		} else {
			/* others just have valid string values */
			v->string = s;
			v->value = -EINVAL;
		}

		list_add(&v->list, &c->value_list);
	}

	return 0;
}

/*
 * Attributes can be associated with constraints such as min, max values.
 * Some attributes could also have pre-defined valid values.
 * The pre-defined values are human-readable values that sometimes need to be translated
 * to tuple values for provate data. For ex: the value "playback" and "capture" for
 * direction attributes need to be translated to 0 and 1 respectively for a DAI widget
 */
static int tplg_parse_class_constraints(snd_tplg_t *tplg, snd_config_t *cfg,
					struct attribute_constraint *c, char *name)
{
	snd_config_iterator_t i, next;
	snd_config_t *n;
	int err;

	snd_config_for_each(i, next, cfg) {
		const char *id, *s;
		long v;

		n = snd_config_iterator_entry(i);

		if (snd_config_get_id(n, &id) < 0)
			continue;

		/* set min value constraint */
		if (!strcmp(id, "min")) {
			err = snd_config_get_integer(n, &v);
			if (err < 0) {
				SNDERR("Invalid min constraint for %s\n", name);
				return err;
			}
			c->min = v;
			continue;
		}

		/* set max value constraint */
		if (!strcmp(id, "max")) {
			err = snd_config_get_integer(n, &v);
			if (err < 0) {
				SNDERR("Invalid min constraint for %s\n", name);
				return err;
			}
			c->max = v;
			continue;
		}

		/* parse reference for string values that need to be translated to tuple values */
		if (!strcmp(id, "value_ref")) {
			err = snd_config_get_string(n, &s);
			if (err < 0) {
				SNDERR("Invalid value ref for %s\n", name);
				return err;
			}
			c->value_ref = s;
			continue;
		}

		/* parse the list of valid values */
		if (!strcmp(id, "values")) {
			err = tplg_parse_constraint_valid_values(tplg, n, c, name);
			if (err < 0) {
				SNDERR("Error parsing valid values for %s\n", name);
				return err;
			}
			continue;
		}
	}

	return 0;
}

static int tplg_parse_class_attribute(snd_tplg_t *tplg, snd_config_t *cfg,
				      struct tplg_attribute *attr)
{
	snd_config_iterator_t i, next;
	snd_config_t *n;
	const char *id;
	int ret;

	snd_config_for_each(i, next, cfg) {
		n = snd_config_iterator_entry(i);

		if (snd_config_get_id(n, &id) < 0)
			continue;

		/* Parse class attribute constraints */
		if (!strcmp(id, "constraints")) {
			ret = tplg_parse_class_constraints(tplg, n, &attr->constraint,
								     attr->name);
			if (ret < 0) {
				SNDERR("Error parsing constraints for %s\n", attr->name);
				return -EINVAL;
			}
			continue;
		}

		/*
		 * Parse token reference for class attributes/arguments. The token_ref field stores the
		 * name of SectionVendorTokens and type that will be used to build the tuple value for the
		 * attribute. For ex: "sof_tkn_dai.word" refers to the SectionVendorTokens with the name
		 * "sof_tkn_dai" and "word" refers to the tuple types.
		 */
		if (!strcmp(id, "token_ref")) {
			const char *s;

			if (snd_config_get_string(n, &s) < 0) {
				SNDERR("invalid token_ref for attribute %s\n", attr->name);
				return -EINVAL;
			}

			snd_strlcpy(attr->token_ref, s, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
			continue;
		}
	}

	return 0;
}


/* Parse class attributes/arguments and add to class attribute_list */
static int tplg_parse_class_attributes(snd_tplg_t *tplg, snd_config_t *cfg,
				       struct tplg_class *class, int type)
{
	snd_config_iterator_t i, next;
	snd_config_t *n;
	const char *id;
	int ret, j = 0;

	snd_config_for_each(i, next, cfg) {
		struct tplg_attribute *attr;

		attr = calloc(1, sizeof(*attr));
		if (!attr)
			return -ENOMEM;
		attr->param_type = type;
		if (type == TPLG_CLASS_PARAM_TYPE_ARGUMENT)
			j++;

		/* init attribute */
		INIT_LIST_HEAD(&attr->constraint.value_list);
		attr->constraint.min = INT_MIN;
		attr->constraint.max = INT_MAX;

		n = snd_config_iterator_entry(i);

		if (snd_config_get_id(n, &id) < 0)
			continue;

		/* set attribute name */
		snd_strlcpy(attr->name, id, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);

		ret = tplg_parse_class_attribute(tplg, n, attr);
		if (ret < 0)
			return ret;

		/* add to class attribute list */
		list_add_tail(&attr->list, &class->attribute_list);
	}

	if (type == TPLG_CLASS_PARAM_TYPE_ARGUMENT)
		class->num_args = j;

	return 0;
}

/* create topology elem of type SND_TPLG_TYPE_CLASS */
static struct tplg_elem *tplg_class_elem(snd_tplg_t *tplg, snd_config_t *cfg, int type)
{
	struct tplg_class *class;
	struct tplg_elem *elem;
	const char *id;

	if (snd_config_get_id(cfg, &id) < 0)
		return NULL;

	elem = tplg_elem_new_common(tplg, cfg, NULL, SND_TPLG_TYPE_CLASS);
	if (!elem)
		return NULL;

	/* init class */
	class = calloc(1, sizeof(*class));
	if (!class)
		return NULL;

	class->type = type;
	snd_strlcpy(class->name, id, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	INIT_LIST_HEAD(&class->attribute_list);
	elem->class = class;

	return elem;
}

static int tplg_define_class(snd_tplg_t *tplg, snd_config_t *cfg, int type)
{
	snd_config_iterator_t i, next;
	struct tplg_elem *class_elem;
	struct tplg_class *class;
	struct tplg_elem *elem;
	snd_config_t *n;
	const char *id;
	int ret;

	if (snd_config_get_id(cfg, &id) < 0) {
		SNDERR("Invalid name for class\n");
		return -EINVAL;
	}

	/* check if the class exists already */
	class_elem = tplg_elem_lookup(&tplg->class_list, id,
				      SND_TPLG_TYPE_CLASS, SND_TPLG_INDEX_ALL);
	if (class_elem)
		return 0;

	/* create class topology elem */
	elem = tplg_class_elem(tplg, cfg, type);
	if (!elem)
		return -ENOMEM;

	class = elem->class;

	/* Parse the class definition */
	snd_config_for_each(i, next, cfg) {
		n = snd_config_iterator_entry(i);
		if (snd_config_get_id(n, &id) < 0)
			continue;

		/* parse arguments */
		if (!strcmp(id, "DefineArgument")) {
			ret = tplg_parse_class_attributes(tplg, n, class,
							  TPLG_CLASS_PARAM_TYPE_ARGUMENT);
			if (ret < 0) {
				SNDERR("failed to parse args for class %s\n", class->name);
				return ret;
			}

			continue;
		}

		/* parse attributes */
		if (!strcmp(id, "DefineAttribute")) {
			ret = tplg_parse_class_attributes(tplg, n, class,
							  TPLG_CLASS_PARAM_TYPE_ATTRIBUTE);
			if (ret < 0) {
				SNDERR("failed to parse attributes for class %s\n", class->name);
				return ret;
			}
		}

	}

	return 0;
}

int tplg_define_classes(snd_tplg_t *tplg, snd_config_t *cfg, void *priv ATTRIBUTE_UNUSED)
{
	snd_config_iterator_t i, next;
	snd_config_t *n;
	const char *id;
	int ret;

	if (snd_config_get_id(cfg, &id) < 0)
		return -EINVAL;

	/* create class */
	snd_config_for_each(i, next, cfg) {
		n = snd_config_iterator_entry(i);
		if (snd_config_get_id(n, &id) < 0)
			continue;

		ret = tplg_define_class(tplg, n, SND_TPLG_CLASS_TYPE_BASE);
		if (ret < 0) {
			SNDERR("Failed to create class %s\n", id);
			return ret;
		}
	}

	return 0;
}

void tplg2_free_elem_class(struct tplg_elem *elem)
{
	struct tplg_class *class = elem->class;
	struct list_head *pos, *npos;
        struct tplg_attribute *attr;

	/* free attributes */
	list_for_each_safe(pos, npos, &class->attribute_list) {
		attr = list_entry(pos, struct tplg_attribute, list);
		list_del(&attr->list);
		free(attr);
        }
}
