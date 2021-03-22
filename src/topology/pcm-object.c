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

int tplg_build_pcm_caps_object(snd_tplg_t *tplg, struct tplg_object *object)
{
	struct snd_soc_tplg_stream_caps *sc;
	struct tplg_elem *elem;
	struct list_head *pos;
	char *pcm_caps_name;
	int ret;

	/* drop the class name from the object name to extract the pcm caps name */
	pcm_caps_name = strchr(object->name, '.') + 1;
	elem = tplg_elem_new_common(tplg, NULL, pcm_caps_name, SND_TPLG_TYPE_STREAM_CAPS);
	if (!elem)
		return -ENOMEM;

	tplg_dbg("PCM caps elem: %s", elem->id);

	sc = elem->stream_caps;
	sc->size = elem->size;
	snd_strlcpy(sc->name, elem->id, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);

	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

		if (!strcmp(attr->name, "rate_min")) {
			sc->rate_min = attr->value.integer;
			continue;
		}

		if (!strcmp(attr->name, "rate_max")) {
			sc->rate_max = attr->value.integer;
			continue;
		}

		if (!strcmp(attr->name, "channels_min")) {
			sc->channels_min = attr->value.integer;
			continue;
		}

		if (!strcmp(attr->name, "channels_max")) {
			sc->channels_max = attr->value.integer;
			continue;
		}

		if (!attr->cfg)
			continue;


		ret = tplg_parse_stream_caps_param(attr->cfg, sc);
		if (ret < 0) {
			SNDERR("Failed to parse PCM caps %s\n", object->name);
			return ret;
		}
	}

	return 0;
}

static int tplg2_get_unsigned_attribute(struct tplg_attribute *arg, unsigned int *val, int base)
{
	const char *str;
	long lval;
	unsigned long uval;

	if (arg->type == SND_CONFIG_TYPE_INTEGER) {
		lval = arg->value.integer;
		if (lval < 0 && lval >= INT_MIN)
			lval = UINT_MAX + lval + 1;
		if (lval < 0 || lval > UINT_MAX)
			return -ERANGE;
		*val = lval;
		return 0;
	}

	if (arg->type == SND_CONFIG_TYPE_STRING) {
		SNDERR("Invalid type for %s\n", arg->name);
		return -EINVAL;
	}

	str = strdup(arg->value.string);

	uval = strtoul(str, NULL, base);
	if (errno == ERANGE && uval == ULONG_MAX)
		return -ERANGE;
	if (errno && uval == 0)
		return -EINVAL;
	if (uval > UINT_MAX)
		return -ERANGE;
	*val = uval;

	return 0;
}

static struct tplg_elem* tplg2_lookup_pcm_by_name(snd_tplg_t *tplg, char *pcm_name)
{
	struct snd_soc_tplg_pcm *pcm;
	struct list_head *pos;

	list_for_each(pos, &tplg->pcm_list) {
		struct tplg_elem *elem = list_entry(pos, struct tplg_elem, list);
		pcm = elem->pcm;

		if (!strcmp(pcm->pcm_name, pcm_name)) {
			return elem;
		}
	}

	return NULL;
}

static int tplg_build_pcm_object(snd_tplg_t *tplg, struct tplg_object *object)
{
	struct tplg_attribute *pcm_id;
	struct tplg_attribute *name;
	struct tplg_attribute *dir;
	struct snd_soc_tplg_stream_caps *caps;
	struct snd_soc_tplg_pcm *pcm;
	struct tplg_elem *elem;
	struct list_head *pos;
	char *dai_name;
	char *caps_name;
	unsigned int dai_id;
	int ret;

	dir = tplg_get_attribute_by_name(&object->attribute_list, "direction");
	name = tplg_get_attribute_by_name(&object->attribute_list, "pcm_name");
	pcm_id = tplg_get_attribute_by_name(&object->attribute_list, "pcm_id");
	caps_name = strchr(object->name, '.') + 1;
	dai_name = strdup(name->value.string);

	/* check if pcm elem exists already */
	elem = tplg2_lookup_pcm_by_name(tplg, name->value.string);
	if (!elem) {
		elem = tplg_elem_new_common(tplg, NULL, name->value.string, SND_TPLG_TYPE_PCM);
		if (!elem)
			return -ENOMEM;

		pcm = elem->pcm;
		pcm->size = elem->size;

		/* set PCM name */
		snd_strlcpy(pcm->pcm_name, name->value.string, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	} else {
		pcm = elem->pcm;
	}

	ret = tplg2_get_unsigned_attribute(pcm_id, &dai_id, 0);
	if (ret < 0) {
		SNDERR("Invalid value for PCM DAI ID");
		return ret;
	}

	/*TODO: check if pcm_id and dai_id are always the same */
	pcm->pcm_id = dai_id;
	unaligned_put32(&pcm->dai_id, dai_id);

	/* set dai name */
	snprintf(pcm->dai_name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s %ld", dai_name,
		 pcm_id->value.integer);
	free(dai_name);

	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

		if (!attr->cfg)
			continue;

		ret = tplg_parse_pcm_param(tplg, attr->cfg, elem);
		if (ret < 0) {
			SNDERR("Failed to parse PCM %s\n", object->name);
			return -EINVAL;
		}
	}

	caps = pcm->caps;
	if (!strcmp(dir->value.string, "playback")) {
		if (strcmp(caps[SND_SOC_TPLG_STREAM_PLAYBACK].name, "")) {
			SNDERR("PCM Playback capabilities already set for %s\n", object->name);
			return -EINVAL;
		}

		unaligned_put32(&pcm->playback, 1);
		snd_strlcpy(caps[SND_SOC_TPLG_STREAM_PLAYBACK].name, caps_name,
				SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	} else {
		if (strcmp(caps[SND_SOC_TPLG_STREAM_CAPTURE].name, "")) {
			SNDERR("PCM Capture capabilities already set for %s\n", object->name);
			return -EINVAL;
		}

		snd_strlcpy(caps[SND_SOC_TPLG_STREAM_CAPTURE].name, caps_name,
				SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
		unaligned_put32(&pcm->capture, 1);
	}

	tplg_dbg(" PCM: %s ID: %d dai_name: %s", pcm->pcm_name, pcm->dai_id, pcm->dai_name);

	return tplg_build_private_data(tplg, object);
}

int tplg_build_pcm_type_object(snd_tplg_t *tplg, struct tplg_object *object)
{
	if (!strcmp(object->class_name, "pcm"))
		return tplg_build_pcm_object(tplg, object);

	if (!strcmp(object->class_name, "pcm_caps"))
		return tplg_build_pcm_caps_object(tplg, object);

	return 0;
}
