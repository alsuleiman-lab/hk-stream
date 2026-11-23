#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <math.h>

struct border_filter_data {
	obs_source_t *context;
	gs_effect_t *effect;
	gs_eparam_t *dimensions_param;
	gs_eparam_t *source_dimensions_param;
	gs_eparam_t *pad_offset_param;
	gs_eparam_t *crop_origin_param;
	gs_eparam_t *crop_dimensions_param;
	gs_eparam_t *border_enabled_param;
	gs_eparam_t *thickness_param;
	gs_eparam_t *radius_param;
	gs_eparam_t *color_param;
	gs_eparam_t *shadow_enabled_param;
	gs_eparam_t *shadow_color_param;
	gs_eparam_t *shadow_blur_param;
	gs_eparam_t *shadow_offset_param;

	bool border_enabled;
	float thickness;
	float radius;
	struct vec4 color;

	bool shadow_enabled;
	struct vec4 shadow_color;
	float shadow_blur;
	struct vec2 shadow_offset;
	float pad_x;
	float pad_y;
};

static void border_filter_update(void *data, obs_data_t *settings);

static const char *border_filter_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("BorderFilter");
}

static void *border_filter_create(obs_data_t *settings, obs_source_t *context)
{
	struct border_filter_data *filter = bzalloc(sizeof(*filter));
	char *effect_path = obs_module_file("border_filter.effect");
	filter->context = context;

	obs_enter_graphics();
	filter->effect = gs_effect_create_from_file(effect_path, NULL);
	obs_leave_graphics();
	bfree(effect_path);

	if (!filter->effect) {
		bfree(filter);
		return NULL;
	}

	filter->dimensions_param = gs_effect_get_param_by_name(filter->effect, "dimensions");
	filter->source_dimensions_param = gs_effect_get_param_by_name(filter->effect, "source_dimensions");
	filter->pad_offset_param = gs_effect_get_param_by_name(filter->effect, "pad_offset");
	filter->crop_origin_param = gs_effect_get_param_by_name(filter->effect, "crop_origin");
	filter->crop_dimensions_param = gs_effect_get_param_by_name(filter->effect, "crop_dimensions");
	filter->border_enabled_param = gs_effect_get_param_by_name(filter->effect, "border_enabled");
	filter->thickness_param = gs_effect_get_param_by_name(filter->effect, "border_thickness");
	filter->radius_param = gs_effect_get_param_by_name(filter->effect, "corner_radius");
	filter->color_param = gs_effect_get_param_by_name(filter->effect, "border_color");

	filter->shadow_enabled_param = gs_effect_get_param_by_name(filter->effect, "shadow_enabled");
	filter->shadow_color_param = gs_effect_get_param_by_name(filter->effect, "shadow_color");
	filter->shadow_blur_param = gs_effect_get_param_by_name(filter->effect, "shadow_blur");
	filter->shadow_offset_param = gs_effect_get_param_by_name(filter->effect, "shadow_offset");

	border_filter_update(filter, settings);
	return filter;
}

static void border_filter_destroy(void *data)
{
	struct border_filter_data *filter = data;
	obs_enter_graphics();
	gs_effect_destroy(filter->effect);
	obs_leave_graphics();
	bfree(filter);
}

static void border_filter_update(void *data, obs_data_t *settings)
{
	struct border_filter_data *filter = data;
	filter->border_enabled = obs_data_get_bool(settings, "border_enabled");
	filter->thickness = (float)obs_data_get_int(settings, "thickness");
	filter->radius = obs_data_get_bool(settings, "round_corners")
			 ? (float)obs_data_get_int(settings, "corner_radius")
			 : 0.0f;
	vec4_from_rgba(&filter->color, (uint32_t)obs_data_get_int(settings, "color"));

	filter->shadow_enabled = obs_data_get_bool(settings, "shadow_enabled");
	vec4_from_rgba(&filter->shadow_color, (uint32_t)obs_data_get_int(settings, "shadow_color"));
	filter->shadow_blur = (float)obs_data_get_int(settings, "shadow_blur");
	filter->shadow_offset.x = (float)obs_data_get_int(settings, "shadow_offset_x");
	filter->shadow_offset.y = (float)obs_data_get_int(settings, "shadow_offset_y");

	if (filter->shadow_enabled && filter->shadow_color.w > 0.0f) {
		filter->pad_x = ceilf(filter->shadow_blur * 1.5f + fabsf(filter->shadow_offset.x));
		filter->pad_y = ceilf(filter->shadow_blur * 1.5f + fabsf(filter->shadow_offset.y));
	} else {
		filter->pad_x = 0.0f;
		filter->pad_y = 0.0f;
	}
}

static obs_properties_t *border_filter_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_bool(props, "border_enabled", obs_module_text("Border.Enabled"));
	obs_properties_add_color_alpha(props, "color", obs_module_text("Border.Color"));
	obs_properties_add_int_slider(props, "thickness", obs_module_text("Border.Thickness"), 1, 128, 1);
	obs_properties_add_bool(props, "round_corners", obs_module_text("Border.RoundCorners"));
	obs_properties_add_int_slider(props, "corner_radius", obs_module_text("Border.CornerRadius"), 0, 512, 1);

	obs_properties_add_bool(props, "shadow_enabled", obs_module_text("Shadow.Enabled"));
	obs_properties_add_color_alpha(props, "shadow_color", obs_module_text("Shadow.Color"));
	obs_properties_add_int_slider(props, "shadow_blur", obs_module_text("Shadow.Blur"), 0, 100, 1);
	obs_properties_add_int_slider(props, "shadow_offset_x", obs_module_text("Shadow.OffsetX"), -100, 100, 1);
	obs_properties_add_int_slider(props, "shadow_offset_y", obs_module_text("Shadow.OffsetY"), -100, 100, 1);

	UNUSED_PARAMETER(data);
	return props;
}

static void border_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "border_enabled", true);
	obs_data_set_default_int(settings, "color", 0xFF4FA6E3);
	obs_data_set_default_int(settings, "thickness", 12);
	obs_data_set_default_bool(settings, "round_corners", true);
	obs_data_set_default_int(settings, "corner_radius", 24);

	obs_data_set_default_bool(settings, "shadow_enabled", false);
	obs_data_set_default_int(settings, "shadow_color", 0x80000000);
	obs_data_set_default_int(settings, "shadow_blur", 20);
	obs_data_set_default_int(settings, "shadow_offset_x", 0);
	obs_data_set_default_int(settings, "shadow_offset_y", 10);
}

static void border_filter_get_item_crop(struct border_filter_data *filter, uint32_t width, uint32_t height,
						struct vec2 *origin, struct vec2 *dimensions, bool *has_crop)
{
	vec2_zero(origin);
	vec2_set(dimensions, (float)width, (float)height);
	*has_crop = false;

	/* Scene-item crop metadata is stored on the per-item render wrapper, which
	 * is the parent of this filter.  The filter's own private settings do not
	 * contain the crop values. */
	obs_source_t *parent = obs_filter_get_parent(filter->context);
	if (!parent)
		return;

	obs_data_t *private_settings = obs_source_get_private_settings(parent);
	if (!private_settings)
		return;

	int64_t left = obs_data_get_int(private_settings, "hk_scene_crop_left");
	int64_t top = obs_data_get_int(private_settings, "hk_scene_crop_top");
	int64_t right = obs_data_get_int(private_settings, "hk_scene_crop_right");
	int64_t bottom = obs_data_get_int(private_settings, "hk_scene_crop_bottom");
	obs_data_release(private_settings);

	const int64_t max_left = width > 1 ? (int64_t)width - 1 : 0;
	const int64_t max_top = height > 1 ? (int64_t)height - 1 : 0;
	if (left < 0) left = 0;
	if (top < 0) top = 0;
	if (left > max_left) left = max_left;
	if (top > max_top) top = max_top;
	const int64_t max_right = width - (uint32_t)left > 1 ? width - (uint32_t)left - 1 : 0;
	const int64_t max_bottom = height - (uint32_t)top > 1 ? height - (uint32_t)top - 1 : 0;
	if (right < 0) right = 0;
	if (bottom < 0) bottom = 0;
	if (right > max_right) right = max_right;
	if (bottom > max_bottom) bottom = max_bottom;

	vec2_set(origin, (float)left, (float)top);
	vec2_set(dimensions, (float)(width - (uint32_t)left - (uint32_t)right),
		 (float)(height - (uint32_t)top - (uint32_t)bottom));
	*has_crop = left > 0 || top > 0 || right > 0 || bottom > 0;
}

static void border_filter_get_effective_padding(struct border_filter_data *filter, uint32_t width,
						uint32_t height, float *pad_x, float *pad_y)
{
	struct vec2 crop_origin, crop_dimensions;
	bool has_crop;
	border_filter_get_item_crop(filter, width, height, &crop_origin, &crop_dimensions, &has_crop);
	*pad_x = has_crop ? 0.0f : filter->pad_x;
	*pad_y = has_crop ? 0.0f : filter->pad_y;
}

static void border_filter_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct border_filter_data *filter = data;
	obs_source_t *target = obs_filter_get_target(filter->context);
	if (!target || !filter->effect) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	uint32_t width = obs_source_get_base_width(target);
	uint32_t height = obs_source_get_base_height(target);
	if (!width || !height) {
		obs_source_skip_video_filter(filter->context);
		return;
	}
	/* Render the target and its own filter chain to a texture before activating
	 * the border effect. Direct rendering can nest the target's effects inside
	 * this effect and leave the graphics effect stack active across scene items. */
	if (!obs_source_process_filter_begin(filter->context, GS_RGBA, OBS_NO_DIRECT_RENDERING))
		return;

	struct vec2 crop_origin, crop_dimensions;
	bool has_crop;
	border_filter_get_item_crop(filter, width, height, &crop_origin, &crop_dimensions, &has_crop);
	float pad_x = has_crop ? 0.0f : filter->pad_x;
	float pad_y = has_crop ? 0.0f : filter->pad_y;
	float total_w = (float)width + pad_x * 2.0f;
	float total_h = (float)height + pad_y * 2.0f;
	struct vec2 dimensions = {total_w, total_h};
	struct vec2 source_dimensions = {(float)width, (float)height};
	struct vec2 pad_offset = {pad_x, pad_y};

	gs_effect_set_vec2(filter->dimensions_param, &dimensions);
	gs_effect_set_vec2(filter->source_dimensions_param, &source_dimensions);
	gs_effect_set_vec2(filter->pad_offset_param, &pad_offset);
	gs_effect_set_vec2(filter->crop_origin_param, &crop_origin);
	gs_effect_set_vec2(filter->crop_dimensions_param, &crop_dimensions);

	gs_effect_set_float(filter->border_enabled_param, filter->border_enabled ? 1.0f : 0.0f);
	gs_effect_set_float(filter->thickness_param, filter->thickness);
	gs_effect_set_float(filter->radius_param, filter->radius);
	gs_effect_set_vec4(filter->color_param, &filter->color);

	gs_effect_set_float(filter->shadow_enabled_param, filter->shadow_enabled ? 1.0f : 0.0f);
	gs_effect_set_vec4(filter->shadow_color_param, &filter->shadow_color);
	gs_effect_set_float(filter->shadow_blur_param, filter->shadow_blur);
	gs_effect_set_vec2(filter->shadow_offset_param, &filter->shadow_offset);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_process_filter_end(filter->context, filter->effect, (uint32_t)total_w, (uint32_t)total_h);
	gs_blend_state_pop();
}

static uint32_t border_filter_width(void *data)
{
	struct border_filter_data *filter = data;
	obs_source_t *target = obs_filter_get_target(filter->context);
	uint32_t width = target ? obs_source_get_base_width(target) : 0;
	if (width) {
		float pad_x, pad_y;
		border_filter_get_effective_padding(filter, width, obs_source_get_base_height(target), &pad_x, &pad_y);
		if (pad_x > 0.0f)
			width += (uint32_t)(pad_x * 2.0f);
	}
	return width;
}

static uint32_t border_filter_height(void *data)
{
	struct border_filter_data *filter = data;
	obs_source_t *target = obs_filter_get_target(filter->context);
	uint32_t height = target ? obs_source_get_base_height(target) : 0;
	if (height) {
		float pad_x, pad_y;
		border_filter_get_effective_padding(filter, obs_source_get_base_width(target), height, &pad_x, &pad_y);
		if (pad_y > 0.0f)
			height += (uint32_t)(pad_y * 2.0f);
	}
	return height;
}

struct obs_source_info border_filter = {
	.id = "hk_border_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = border_filter_name,
	.create = border_filter_create,
	.destroy = border_filter_destroy,
	.update = border_filter_update,
	.get_properties = border_filter_properties,
	.get_defaults = border_filter_defaults,
	.video_render = border_filter_render,
	.get_width = border_filter_width,
	.get_height = border_filter_height,
};
