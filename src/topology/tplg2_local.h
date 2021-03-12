/*
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 */
#include <limits.h>
#include <stdint.h>
#include <stdbool.h>

#include "local.h"
#include "list.h"
#include "bswap.h"
#include "topology.h"

#include <sound/type_compat.h>
#include <sound/asound.h>
#include <sound/asoc.h>
#include <sound/tlv.h>

#define TPLG_CLASS_ATTRIBUTE_MASK_MANDATORY	1 << 1
#define TPLG_CLASS_ATTRIBUTE_MASK_IMMUTABLE	1 << 2
#define TPLG_CLASS_ATTRIBUTE_MASK_DEPRECATED	1 << 3
#define TPLG_CLASS_ATTRIBUTE_MASK_AUTOMATIC	1 << 4
#define TPLG_CLASS_ATTRIBUTE_MASK_UNIQUE	1 << 5

struct tplg_attribute_ref {
	const char *string;
	int value;
	struct list_head list; /* item in attribute constraint value_list */
};

struct attribute_constraint {
	struct list_head value_list; /* list of valid values */
	const char *value_ref;
	int mask;
	int min;
	int max;
};

enum tplg_class_param_type {
	TPLG_CLASS_PARAM_TYPE_ARGUMENT,
	TPLG_CLASS_PARAM_TYPE_ATTRIBUTE,
};

struct tplg_attribute {
	char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	snd_config_type_t type;
	enum tplg_class_param_type param_type;
	char token_ref[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	char value_ref[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	bool found;
	snd_config_t *cfg;
	struct attribute_constraint constraint;
	struct list_head list; /* item in attribute list */
	union {
		long integer;
		long long integer64;
		double d;
		char string[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	}value;
};

struct tplg_object {
	char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	char class_name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	int num_args;
	struct list_head attribute_list;
	struct tplg_elem *elem;
	snd_config_t *cfg;
	int type;
	struct list_head list; /* item in parent object list */
};

/* class types */
#define SND_TPLG_CLASS_TYPE_BASE		0

struct tplg_class {
	char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	int num_args;
	struct list_head attribute_list; /* list of attributes */
	int type;
};

int tplg_define_classes(snd_tplg_t *tplg, snd_config_t *cfg, void *priv);
void tplg2_free_elem_class(struct tplg_elem *elem);
int tplg_create_objects(snd_tplg_t *tplg, snd_config_t *cfg, void *priv);
int tplg_parse_attribute_value(snd_config_t *cfg, struct list_head *list, bool override);
struct tplg_attribute *tplg_get_attribute_by_name(struct list_head *list, const char *name);
void tplg2_free_elem_object(struct tplg_elem *elem);
