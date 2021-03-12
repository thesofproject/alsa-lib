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

enum tplg_class_param_type {
	TPLG_CLASS_PARAM_TYPE_ARGUMENT,
	TPLG_CLASS_PARAM_TYPE_ATTRIBUTE,
};

struct tplg_attribute {
	char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	snd_config_type_t type;
	enum tplg_class_param_type param_type;
	char token_ref[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	struct list_head list; /* item in attribute list */
	union {
		long integer;
		long long integer64;
		double d;
		char string[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	}value;
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
