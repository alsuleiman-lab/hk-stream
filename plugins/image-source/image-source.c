#include <obs-module.h>
#include <graphics/image-file.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <sys/stat.h>

#define blog(log_level, format, ...) \
	blog(log_level, "[image_source: '%s'] " format, obs_source_get_name(context->source), ##__VA_ARGS__)

#define debug(format, ...) blog(LOG_DEBUG, format, ##__VA_ARGS__)
#define info(format, ...) blog(LOG_INFO, format, ##__VA_ARGS__)
#define warn(format, ...) blog(LOG_WARNING, format, ##__VA_ARGS__)

/* Image sources showing the same unchanged file share one decoded image and
 * texture, so duplicated scenes don't decode the same file again at startup.
 * Animated GIFs keep their own copy because each source animates on its own. */
struct shared_image {
	char *file;
	time_t timestamp;
	enum gs_image_alpha_mode alpha_mode;
	long refs;

	pthread_mutex_t texture_mutex;
	bool texture_loaded;
	gs_image_file_ex_t image;

	struct shared_image *next;
};

static pthread_mutex_t shared_images_mutex;
static struct shared_image *shared_images;

struct image_source {
	obs_source_t *source;

	char *file;
	bool persistent;
	bool is_slide;
	bool linear_alpha;
	time_t file_timestamp;
	float update_time_elapsed;
	uint64_t last_time;
	bool active;
	bool restart_gif;
	volatile bool file_decoded;
	volatile bool texture_loaded;

	gs_image_file_ex_t image;
	struct shared_image *shared;
	uint32_t cx;
	uint32_t cy;
	uint64_t mem_usage;
};

static inline gs_image_file_ex_t *get_image(struct image_source *context)
{
	return context->shared ? &context->shared->image : &context->image;
}

/* Caller holds shared_images_mutex. Returns a new reference or NULL. */
static struct shared_image *find_shared_image(const char *file, time_t timestamp, enum gs_image_alpha_mode alpha_mode)
{
	for (struct shared_image *entry = shared_images; entry; entry = entry->next) {
		if (entry->timestamp == timestamp && entry->alpha_mode == alpha_mode && strcmp(entry->file, file) == 0) {
			entry->refs++;
			return entry;
		}
	}
	return NULL;
}

/* Moves the source's freshly decoded image into the shared list, unless
 * another source published the same file first; then that entry is used and
 * the source's own copy is left for the caller to free. */
static struct shared_image *publish_shared_image(struct image_source *context, enum gs_image_alpha_mode alpha_mode)
{
	pthread_mutex_lock(&shared_images_mutex);
	struct shared_image *entry = find_shared_image(context->file, context->file_timestamp, alpha_mode);
	if (!entry) {
		entry = bzalloc(sizeof(*entry));
		entry->file = bstrdup(context->file);
		entry->timestamp = context->file_timestamp;
		entry->alpha_mode = alpha_mode;
		entry->refs = 1;
		pthread_mutex_init(&entry->texture_mutex, NULL);
		entry->image = context->image;
		memset(&context->image, 0, sizeof(context->image));

		entry->next = shared_images;
		shared_images = entry;
	}
	pthread_mutex_unlock(&shared_images_mutex);
	return entry;
}

static void release_shared_image(struct shared_image *entry)
{
	if (!entry)
		return;

	bool last = false;
	pthread_mutex_lock(&shared_images_mutex);
	if (--entry->refs == 0) {
		struct shared_image **prev = &shared_images;
		while (*prev && *prev != entry)
			prev = &(*prev)->next;
		if (*prev)
			*prev = entry->next;
		last = true;
	}
	pthread_mutex_unlock(&shared_images_mutex);

	if (!last)
		return;

	obs_enter_graphics();
	gs_image_file_ex_free(&entry->image);
	obs_leave_graphics();

	pthread_mutex_destroy(&entry->texture_mutex);
	bfree(entry->file);
	bfree(entry);
}

static time_t get_modified_timestamp(const char *filename)
{
	struct stat stats;
	if (os_stat(filename, &stats) != 0)
		return -1;

	return stats.st_mtime;
}

static const char *image_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ImageInput");
}

void image_source_preload_image(void *data)
{
	struct image_source *context = data;
	if (os_atomic_load_bool(&context->file_decoded))
		return;

	const enum gs_image_alpha_mode alpha_mode = context->linear_alpha ? GS_IMAGE_ALPHA_PREMULTIPLY_SRGB
									  : GS_IMAGE_ALPHA_PREMULTIPLY;
	context->file_timestamp = get_modified_timestamp(context->file);

	if (context->file_timestamp != -1) {
		pthread_mutex_lock(&shared_images_mutex);
		context->shared = find_shared_image(context->file, context->file_timestamp, alpha_mode);
		pthread_mutex_unlock(&shared_images_mutex);
	}

	if (!context->shared) {
		gs_image_file_ex_init(&context->image, context->file, alpha_mode);

		if (context->image.loaded && !context->image.is_animated_gif && context->file_timestamp != -1) {
			context->shared = publish_shared_image(context, alpha_mode);

			if (context->image.loaded) {
				obs_enter_graphics();
				gs_image_file_ex_free(&context->image);
				obs_leave_graphics();
			}
		}
	}

	gs_image_file_ex_t *image = get_image(context);
	context->cx = image->cx;
	context->cy = image->cy;
	context->mem_usage = image->mem_usage;
	os_atomic_set_bool(&context->file_decoded, true);
}

static void image_source_load_texture(void *data)
{
	struct image_source *context = data;
	if (os_atomic_load_bool(&context->texture_loaded))
		return;

	debug("loading texture '%s'", context->file);

	struct shared_image *entry = context->shared;
	if (entry) {
		pthread_mutex_lock(&entry->texture_mutex);
		if (!entry->texture_loaded) {
			obs_enter_graphics();
			gs_image_file_ex_init_texture(&entry->image);
			obs_leave_graphics();
			entry->texture_loaded = true;
		}
		pthread_mutex_unlock(&entry->texture_mutex);
	} else {
		obs_enter_graphics();
		gs_image_file_ex_init_texture(&context->image);
		obs_leave_graphics();
	}

	if (!get_image(context)->loaded)
		warn("failed to load texture '%s'", context->file);

	context->update_time_elapsed = 0;
	os_atomic_set_bool(&context->texture_loaded, true);
}

static void image_source_unload(void *data)
{
	struct image_source *context = data;
	os_atomic_set_bool(&context->file_decoded, false);
	os_atomic_set_bool(&context->texture_loaded, false);

	/* Detach inside the graphics context so a render in progress finishes
	 * with the shared texture before this source lets go of it. */
	obs_enter_graphics();
	gs_image_file_ex_free(&context->image);
	struct shared_image *entry = context->shared;
	context->shared = NULL;
	obs_leave_graphics();

	context->cx = 0;
	context->cy = 0;
	context->mem_usage = 0;
	release_shared_image(entry);
}

static void image_source_load(struct image_source *context)
{
	image_source_unload(context);

	if (context->file && *context->file) {
		image_source_preload_image(context);
		image_source_load_texture(context);
	}
}

static void image_source_update(void *data, obs_data_t *settings)
{
	struct image_source *context = data;
	const char *file = obs_data_get_string(settings, "file");
	const bool unload = obs_data_get_bool(settings, "unload");
	const bool linear_alpha = obs_data_get_bool(settings, "linear_alpha");
	const bool is_slide = obs_data_get_bool(settings, "is_slide");

	if (context->file)
		bfree(context->file);

	context->file = bstrdup(file);
	context->persistent = !unload;
	context->linear_alpha = linear_alpha;
	context->is_slide = is_slide;

	if (is_slide)
		return;

	/* Load the image if the source is persistent or showing */
	if (context->persistent || obs_source_showing(context->source))
		image_source_load(data);
	else
		image_source_unload(data);
}

static void image_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "unload", false);
	obs_data_set_default_bool(settings, "linear_alpha", false);
}

static void image_source_show(void *data)
{
	struct image_source *context = data;

	if (!context->persistent && !context->is_slide)
		image_source_load(context);
}

static void image_source_hide(void *data)
{
	struct image_source *context = data;

	if (!context->persistent && !context->is_slide)
		image_source_unload(context);
}

static void restart_gif(void *data)
{
	struct image_source *context = data;

	if (context->image.is_animated_gif) {
		context->image.cur_frame = 0;
		context->image.cur_loop = 0;
		context->image.cur_time = 0;

		obs_enter_graphics();
		gs_image_file_ex_update_texture(&context->image);
		obs_leave_graphics();

		context->restart_gif = false;
	}
}

static void image_source_activate(void *data)
{
	struct image_source *context = data;
	context->restart_gif = true;
}

static void *image_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct image_source *context = bzalloc(sizeof(struct image_source));
	context->source = source;

	image_source_update(context, settings);
	return context;
}

static void image_source_destroy(void *data)
{
	struct image_source *context = data;

	image_source_unload(context);

	if (context->file)
		bfree(context->file);

	bfree(context);
}

static uint32_t image_source_getwidth(void *data)
{
	struct image_source *context = data;
	return context->cx;
}

static uint32_t image_source_getheight(void *data)
{
	struct image_source *context = data;
	return context->cy;
}

static void image_source_render(void *data, gs_effect_t *effect)
{
	struct image_source *context = data;
	if (!os_atomic_load_bool(&context->texture_loaded))
		return;

	gs_image_file_ex_t *const image = get_image(context);
	gs_texture_t *const texture = image->texture;
	if (!texture)
		return;

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	gs_eparam_t *const param = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture_srgb(param, texture);

	gs_draw_sprite(texture, 0, image->cx, image->cy);

	gs_blend_state_pop();

	gs_enable_framebuffer_srgb(previous);
}

static void image_source_tick(void *data, float seconds)
{
	struct image_source *context = data;
	if (!os_atomic_load_bool(&context->texture_loaded)) {
		if (os_atomic_load_bool(&context->file_decoded))
			image_source_load_texture(context);
		else
			return;
	}

	uint64_t frame_time = obs_get_video_frame_time();

	context->update_time_elapsed += seconds;

	if (obs_source_showing(context->source)) {
		if (context->update_time_elapsed >= 1.0f) {
			time_t t = get_modified_timestamp(context->file);
			context->update_time_elapsed = 0.0f;

			if (context->file_timestamp != t) {
				image_source_load(context);
			}
		}
	}

	if (obs_source_showing(context->source)) {
		if (!context->active) {
			if (context->image.is_animated_gif)
				context->last_time = frame_time;

			context->active = true;
		}

		if (context->restart_gif)
			restart_gif(context);

	} else {
		if (context->active) {
			restart_gif(context);
			context->active = false;
		}

		return;
	}

	if (context->last_time && context->image.is_animated_gif) {
		uint64_t elapsed = frame_time - context->last_time;
		bool updated = gs_image_file_ex_tick(&context->image, elapsed);

		if (updated) {
			obs_enter_graphics();
			gs_image_file_ex_update_texture(&context->image);
			obs_leave_graphics();
		}
	}

	context->last_time = frame_time;
}

static const char *image_filter =
#ifdef _WIN32
	"All formats (*.bmp *.tga *.png *.jpeg *.jpg *.jxr *.gif *.psd *.webp);;"
#else
	"All formats (*.bmp *.tga *.png *.jpeg *.jpg *.gif *.psd *.webp);;"
#endif
	"BMP Files (*.bmp);;"
	"Targa Files (*.tga);;"
	"PNG Files (*.png);;"
	"JPEG Files (*.jpeg *.jpg);;"
#ifdef _WIN32
	"JXR Files (*.jxr);;"
#endif
	"GIF Files (*.gif);;"
	"PSD Files (*.psd);;"
	"WebP Files (*.webp);;"
	"All Files (*.*)";

static obs_properties_t *image_source_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_path(props, "file", obs_module_text("File"), OBS_PATH_FILE, image_filter, NULL);
	obs_properties_add_bool(props, "unload", obs_module_text("UnloadWhenNotShowing"));
	obs_properties_add_bool(props, "linear_alpha", obs_module_text("LinearAlpha"));

	return props;
}

uint64_t image_source_get_memory_usage(void *data)
{
	struct image_source *s = data;
	return s->mem_usage;
}

static void missing_file_callback(void *src, const char *new_path, void *data)
{
	struct image_source *s = src;

	obs_source_t *source = s->source;
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_set_string(settings, "file", new_path);
	obs_source_update(source, settings);
	obs_data_release(settings);

	UNUSED_PARAMETER(data);
}

static obs_missing_files_t *image_source_missingfiles(void *data)
{
	struct image_source *s = data;
	obs_missing_files_t *files = obs_missing_files_create();

	if (strcmp(s->file, "") != 0) {
		if (!os_file_exists(s->file)) {
			obs_missing_file_t *file = obs_missing_file_create(s->file, missing_file_callback,
									   OBS_MISSING_FILE_SOURCE, s->source, NULL);

			obs_missing_files_add_file(files, file);
		}
	}

	return files;
}

static enum gs_color_space image_source_get_color_space(void *data, size_t count,
							const enum gs_color_space *preferred_spaces)
{
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred_spaces);

	struct image_source *const s = data;
	gs_image_file_ex_t *const image = get_image(s);
	return image->texture ? image->space : GS_CS_SRGB;
}

static struct obs_source_info image_source_info = {
	.id = "image_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = image_source_get_name,
	.create = image_source_create,
	.destroy = image_source_destroy,
	.update = image_source_update,
	.get_defaults = image_source_defaults,
	.show = image_source_show,
	.hide = image_source_hide,
	.get_width = image_source_getwidth,
	.get_height = image_source_getheight,
	.video_render = image_source_render,
	.video_tick = image_source_tick,
	.missing_files = image_source_missingfiles,
	.get_properties = image_source_properties,
	.icon_type = OBS_ICON_TYPE_IMAGE,
	.activate = image_source_activate,
	.video_get_color_space = image_source_get_color_space,
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("image-source", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Image/color/slideshow sources";
}

extern struct obs_source_info slideshow_info;
extern struct obs_source_info slideshow_info_mk2;
extern struct obs_source_info color_source_info_v1;
extern struct obs_source_info color_source_info_v2;
extern struct obs_source_info color_source_info_v3;

bool obs_module_load(void)
{
	pthread_mutex_init(&shared_images_mutex, NULL);
	obs_register_source(&image_source_info);
	obs_register_source(&color_source_info_v1);
	obs_register_source(&color_source_info_v2);
	obs_register_source(&color_source_info_v3);
	obs_register_source(&slideshow_info);
	obs_register_source(&slideshow_info_mk2);
	return true;
}

void obs_module_unload(void)
{
	pthread_mutex_destroy(&shared_images_mutex);
}
