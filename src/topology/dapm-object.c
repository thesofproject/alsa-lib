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

static int tplg_dapm_route_validate_widget(snd_tplg_t *tplg, char *wname, char *dest)
{
	struct tplg_elem *w_elem;

	/* check if it is a valid widget */
	w_elem = tplg_elem_lookup(&tplg->widget_list, wname,
			      SND_TPLG_TYPE_DAPM_WIDGET, SND_TPLG_INDEX_ALL);
	if (!w_elem) {
		SNDERR("No widget %s found\n", wname);
		return -EINVAL;
	}

	snd_strlcpy(dest, w_elem->id, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);

	return 0;
}

int tplg_build_dapm_route(snd_tplg_t *tplg, struct tplg_object *object)
{
	struct snd_soc_tplg_dapm_graph_elem *line;
	struct list_head *pos;
	struct tplg_elem *elem;
	int ret;

	/* create graph elem */
	elem = tplg_elem_new_route(tplg, 0);
	if (!elem)
		return -ENOMEM;

	line = elem->route;

	/* set graph elem index and control values */
	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

		if (!strcmp(attr->name, "pipeline_id")) {
			elem->index = attr->value.integer;
			continue;
		}

		if (!strcmp(attr->name, "control"))
			snd_strlcpy(line->control, attr->value.string,
				    SNDRV_CTL_ELEM_ID_NAME_MAXLEN);

		if (!strcmp(attr->name, "source_widget")) {
			ret = tplg_dapm_route_validate_widget(tplg, attr->value.string,
							      line->source);
			if (ret < 0) {
				SNDERR("Failed to find source widget for route %s\n", object->name);
				return ret;
			}
		}

		if (!strcmp(attr->name, "sink_widget")) {
			ret = tplg_dapm_route_validate_widget(tplg, attr->value.string,
							      line->sink);
			if (ret < 0) {
				SNDERR("Failed to find sink widget for route %s\n", object->name);
				return ret;
			}
		}
	}

	tplg_dbg("DAPM route: %s -> %s", line->source, line->sink);

	return 0;
}

static int tplg2_parse_channel(struct tplg_object *object, struct tplg_elem *mixer_elem)
{
	struct snd_soc_tplg_mixer_control *mc = mixer_elem->mixer_ctrl;
	struct snd_soc_tplg_channel *channel = mc->channel;
	struct list_head *pos;
	char *channel_name = strchr(object->name, '.') + 1;
	int channel_id = lookup_channel(channel_name);

	if (channel_id < 0) {
		SNDERR("invalid channel %d for mixer %s", channel_id, mixer_elem->id);
		return -EINVAL;
	}

	channel += mc->num_channels;

	channel->id = channel_id;
	channel->size = sizeof(*channel);
	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

		if (!strcmp(attr->name, "reg"))
			channel->reg = attr->value.integer;


		if (!strcmp(attr->name, "shift"))
			channel->shift = attr->value.integer;
	}

	mc->num_channels++;
	if (mc->num_channels >= SND_SOC_TPLG_MAX_CHAN) {
		SNDERR("Max channels exceeded for %s\n", mixer_elem->id);
		return -EINVAL;
	}

	tplg_dbg("channel: %s id: %d reg:%d shift %d", channel_name, channel->id, channel->reg, channel->shift);

	return 0;
}

static int tplg2_parse_tlv(snd_tplg_t *tplg, struct tplg_object *object,
			    struct tplg_elem *mixer_elem)
{
	struct snd_soc_tplg_ctl_tlv *tplg_tlv;
	struct snd_soc_tplg_tlv_dbscale *scale;
	struct tplg_elem *elem;
	struct list_head *pos;
	int ret;

	/* Just add ref if TLV elem exists already */
	elem = tplg_elem_lookup(&tplg->widget_list, object->name, SND_TPLG_TYPE_TLV,
				SND_TPLG_INDEX_ALL);
	if (elem) {
		tplg_tlv = elem->tlv;
		scale = &tplg_tlv->scale;
		goto ref;
	}

	/* otherwise create new TLV elem */
	elem = tplg_elem_new_common(tplg, NULL, object->name, SND_TPLG_TYPE_TLV);
	if (!elem)
		return -ENOMEM;

	tplg_tlv = elem->tlv;
	tplg_tlv->size = sizeof(struct snd_soc_tplg_ctl_tlv);
	tplg_tlv->type = SNDRV_CTL_TLVT_DB_SCALE;
	scale = &tplg_tlv->scale;

	list_for_each(pos, &object->object_list) {
		struct tplg_object *child = list_entry(pos, struct tplg_object, list);

		if (!strcmp(child->class_name, "scale")) {
			list_for_each(pos, &child->attribute_list) {
				struct tplg_attribute *attr;

				attr = list_entry(pos, struct tplg_attribute, list);
				if (!attr->cfg)
					continue;

				ret = tplg_parse_tlv_dbscale_param(attr->cfg, scale);
				if (ret < 0) {
					SNDERR("failed to DBScale for tlv %s", object->name);
					return ret;
				}
			}

			break;
		}
	}
ref:
	tplg_dbg("TLV: %s scale min: %d step %d mute %d", elem->id, scale->min, scale->step, scale->mute);

	ret = tplg_ref_add(mixer_elem, SND_TPLG_TYPE_TLV, elem->id);
	if (ret < 0) {
		SNDERR("failed to add tlv elem %s to mixer elem %s\n",
		       elem->id, mixer_elem->id);
		return ret;
	}

	return 0;
}

static struct tplg_elem *tplg_build_comp_mixer(snd_tplg_t *tplg, struct tplg_object *object,
						 char *name)
{
	struct snd_soc_tplg_mixer_control *mc;
	struct snd_soc_tplg_ctl_hdr *hdr;
	struct tplg_elem *elem;
	struct list_head *pos;
	bool access_set = false, tlv_set = false;
	int j, ret;

	elem = tplg_elem_new_common(tplg, NULL, name, SND_TPLG_TYPE_MIXER);
	if (!elem)
		return NULL;

	/* init new mixer */
	mc = elem->mixer_ctrl;
	snd_strlcpy(mc->hdr.name, elem->id, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	mc->hdr.type = SND_SOC_TPLG_TYPE_MIXER;
	mc->size = elem->size;
	hdr = &mc->hdr;

	/* set channel reg to default state */
	for (j = 0; j < SND_SOC_TPLG_MAX_CHAN; j++)
		mc->channel[j].reg = -1;

	/* parse some control params from attributes */
	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr;

		attr = list_entry(pos, struct tplg_attribute, list);

		if (!attr->cfg)
			continue;

		ret = tplg_parse_control_mixer_param(tplg, attr->cfg, mc, elem);
		if (ret < 0) {
			SNDERR("Error parsing hw_config for %s\n", object->name);
			return NULL;
		}

		if (!strcmp(attr->name, "access")) {
			ret = parse_access_values(attr->cfg, &mc->hdr);
			if (ret < 0) {
				SNDERR("Error parsing access attribute for %s\n", object->name);
				return NULL;
			}
			access_set = true;
		}

	}

	/* parse the rest from child objects */
	list_for_each(pos, &object->object_list) {
		struct tplg_object *child = list_entry(pos, struct tplg_object, list);

		if (!object->cfg)
			continue;

		if (!strcmp(child->class_name, "ops")) {
			ret = tplg_parse_ops(tplg, child->cfg, hdr);
			if (ret < 0) {
				SNDERR("Error parsing ops for mixer %s\n", object->name);
				return NULL;
			}
			continue;
		}

		if (!strcmp(child->class_name, "tlv")) {
			ret = tplg2_parse_tlv(tplg, child, elem);
			if (ret < 0) {
				SNDERR("Error parsing tlv for mixer %s\n", object->name);
				return NULL;
			}
			tlv_set = true;
			continue;
		}

		if (!strcmp(child->class_name, "channel")) {
			ret = tplg2_parse_channel(child, elem);
			if (ret < 0) {
				SNDERR("Error parsing channel %d for mixer %s\n", child->name,
				       object->name);
				return NULL;
			}
			continue;
		}
	}
	tplg_dbg("Mixer: %s, num_channels: %d", elem->id, mc->num_channels);
	tplg_dbg("Ops info: %d get: %d put: %d max: %d", hdr->ops.info, hdr->ops.get, hdr->ops.put, mc->max);

	/* set CTL access to default values if none provided */
	if (!access_set) {
		mc->hdr.access = SNDRV_CTL_ELEM_ACCESS_READWRITE;
		if (tlv_set)
			mc->hdr.access |= SNDRV_CTL_ELEM_ACCESS_TLV_READ;
	}

	return elem;
}

static struct tplg_elem *tplg_build_comp_bytes(snd_tplg_t *tplg, struct tplg_object *object,
						 char *name)
{
	struct snd_soc_tplg_bytes_control *be;
	struct snd_soc_tplg_ctl_hdr *hdr;
	struct tplg_elem *elem;
	struct list_head *pos;
	bool access_set = false, tlv_set = false;
	int ret;

	elem = tplg_elem_new_common(tplg, NULL, name, SND_TPLG_TYPE_BYTES);
	if (!elem)
		return NULL;

	/* init new byte control */
	be = elem->bytes_ext;
	snd_strlcpy(be->hdr.name, elem->id, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	be->hdr.type = SND_SOC_TPLG_TYPE_BYTES;
	be->size = elem->size;
	hdr = &be->hdr;

	/* parse some control params from attributes */
	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr;

		attr = list_entry(pos, struct tplg_attribute, list);

		if (!attr->cfg)
			continue;

		ret = tplg_parse_control_bytes_param(tplg, attr->cfg, be, elem);
		if (ret < 0) {
			SNDERR("Error parsing control bytes params for %s\n", object->name);
			return NULL;
		}

		if (!strcmp(attr->name, "access")) {
			ret = parse_access_values(attr->cfg, &be->hdr);
			if (ret < 0) {
				SNDERR("Error parsing access attribute for %s\n", object->name);
				return NULL;
			} else  {
				access_set = true;
			}
		}

	}

	/* parse the rest from child objects */
	list_for_each(pos, &object->object_list) {
		struct tplg_object *child = list_entry(pos, struct tplg_object, list);

		if (!object->cfg)
			continue;

		if (!strcmp(child->class_name, "ops")) {
			ret = tplg_parse_ops(tplg, child->cfg, hdr);
			if (ret < 0) {
				SNDERR("Error parsing ops for mixer %s\n", object->name);
				return NULL;
			}
			continue;
		}

		if (!strcmp(child->class_name, "tlv")) {
			ret = tplg2_parse_tlv(tplg, child, elem);
			if (ret < 0) {
				SNDERR("Error parsing tlv for mixer %s\n", object->name);
				return NULL;
			} else {
				tlv_set = true;
			}
			continue;
		}

		if (!strcmp(child->class_name, "extops")) {
			ret = tplg_parse_ext_ops(tplg, child->cfg, &be->hdr);
			if (ret < 0) {
				SNDERR("Error parsing ext ops for bytes %s\n", object->name);
				return NULL;
			}
			continue;
		}

		/* add data reference for byte control by adding a new obect */
		if (!strcmp(child->class_name, "data")) {
			struct tplg_attribute *name;

			name = tplg_get_attribute_by_name(&child->attribute_list, "name");
			/* add reference to data elem */
			ret = tplg_ref_add(elem, SND_TPLG_TYPE_DATA, name->value.string);
			if (ret < 0) {
				SNDERR("failed to add data elem %s to byte control %s\n",
				       name->value.string, elem->id);
				return NULL;
			}
		}
	}

	tplg_dbg("Bytes: %s Ops info: %d get: %d put: %d", elem->id, hdr->ops.info, hdr->ops.get,
		 hdr->ops.put);
	tplg_dbg("Ext Ops info: %d get: %d put: %d", be->ext_ops.info, be->ext_ops.get, be->ext_ops.put);

	/* set CTL access to default values if none provided */
	if (!access_set) {
		be->hdr.access = SNDRV_CTL_ELEM_ACCESS_READWRITE;
		if (tlv_set)
			be->hdr.access |= SNDRV_CTL_ELEM_ACCESS_TLV_READ;
	}

	return elem;
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

	/* build controls */
	list_for_each(pos, &object->object_list) {
		struct tplg_object *child = list_entry(pos, struct tplg_object, list);
		struct tplg_elem *elem;
		char *class_name = child->class_name;

		if (!strcmp(class_name, "mixer")) {
			struct tplg_attribute *name_attr;

			/* skip if no name is provided */
			name_attr = tplg_get_attribute_by_name(&child->attribute_list,
								"name");

			if (!name_attr || !strcmp(name_attr->value.string, ""))
				continue;

			elem = tplg_build_comp_mixer(tplg, child, name_attr->value.string);
			if (!elem) {
				SNDERR("Failed to build mixer control for %s\n", object->name);
				return -EINVAL;
			}

			ret = tplg_ref_add(w_elem, SND_TPLG_TYPE_MIXER, elem->id);
			if (ret < 0) {
				SNDERR("failed to add mixer elem %s to widget elem %s\n",
				       elem->id, w_elem->id);
				return ret;
			}
		}

		if (!strcmp(class_name, "bytes")) {
			struct tplg_attribute *name_attr;

			/* skip if no name is provided */
			name_attr = tplg_get_attribute_by_name(&child->attribute_list,
								"name");

			if (!name_attr || !strcmp(name_attr->value.string, ""))
				continue;

			elem = tplg_build_comp_bytes(tplg, child, name_attr->value.string);
			if (!elem) {
				SNDERR("Failed to build bytes control for %s\n", object->name);
				return -EINVAL;
			}

			ret = tplg_ref_add(w_elem, SND_TPLG_TYPE_BYTES, elem->id);
			if (ret < 0) {
				SNDERR("failed to add bytes control elem %s to widget elem %s\n",
				       elem->id, w_elem->id);
				return ret;
			}
		}
	}

	tplg_dbg("Widget: %s id: %d stream_name: %s no_pm: %d",
		 w_elem->id, widget->id, widget->sname, widget->reg);

	return tplg_build_private_data(tplg, object);
}
