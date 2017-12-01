#define NK_IMPLEMENTATION
#define NK_SDL_GLES2_IMPLEMENTATION

#include <stdlib.h>
#include "blastem_nuklear.h"
#include "font.h"
#include "../render.h"
#include "../render_sdl.h"
#include "../util.h"
#include "../paths.h"
#include "../saves.h"
#include "../blastem.h"
#include "../config.h"

static struct nk_context *context;

typedef void (*view_fun)(struct nk_context *);
static view_fun current_view;
static view_fun *previous_views;
static uint32_t view_storage;
static uint32_t num_prev;

static void push_view(view_fun new_view)
{
	if (num_prev == view_storage) {
		view_storage = view_storage ? 2*view_storage : 2;
		previous_views = realloc(previous_views, view_storage*sizeof(view_fun));
	}
	previous_views[num_prev++] = current_view;
	current_view = new_view;
}

static void pop_view()
{
	if (num_prev) {
		current_view = previous_views[--num_prev];
	}
}

static void clear_view_stack()
{
	num_prev = 0;
}

void view_play(struct nk_context *context)
{
	
}

void view_file_browser(struct nk_context *context, uint8_t normal_open)
{
	static char *current_path;
	static dir_entry *entries;
	static size_t num_entries;
	static uint32_t selected_entry;
	static char **ext_list;
	static uint32_t num_exts;
	static uint8_t got_ext_list;
	if (!current_path) {
		get_initial_browse_path(&current_path);
	}
	if (!entries) {
		entries = get_dir_list(current_path, &num_entries);
		if (entries) {
			sort_dir_list(entries, num_entries);
		}
	}
	if (!got_ext_list) {
		ext_list = get_extension_list(config, &num_exts);
		got_ext_list = 1;
	}
	uint32_t width = render_width();
	uint32_t height = render_height();
	if (nk_begin(context, "Load ROM", nk_rect(0, 0, width, height), 0)) {
		nk_layout_row_static(context, height - 100, width - 60, 1);
		if (nk_group_begin(context, "Select ROM", NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
			nk_layout_row_static(context, 28, width-100, 1);
			for (uint32_t i = 0; i < num_entries; i++)
			{
				if (entries[i].name[0] == '.' && entries[i].name[1] != '.') {
					continue;
				}
				if (num_exts && !entries[i].is_dir && !path_matches_extensions(entries[i].name, ext_list, num_exts)) {
					continue;
				}
				int selected = i == selected_entry;
				nk_selectable_label(context, entries[i].name, NK_TEXT_ALIGN_LEFT, &selected);
				if (selected) {
					selected_entry = i;
				}
			}
			nk_group_end(context);
		}
		nk_layout_row_static(context, 52, width > 600 ? 300 : width / 2, 2);
		if (nk_button_label(context, "Back")) {
			pop_view();
		}
		if (nk_button_label(context, "Open")) {
			char *full_path = path_append(current_path, entries[selected_entry].name);
			if (entries[selected_entry].is_dir) {
				free(current_path);
				current_path = full_path;
				free_dir_list(entries, num_entries);
				entries = NULL;
			} else {
				if(normal_open) {
					if (current_system) {
						current_system->next_rom = full_path;
						current_system->request_exit(current_system);
					} else {
						init_system_with_media(full_path, SYSTEM_UNKNOWN);
						free(full_path);
					}
				} else {
					lockon_media(full_path);
					free(full_path);
				}
				clear_view_stack();
				current_view = view_play;
			}
		}
		nk_end(context);
	}
}

void view_load(struct nk_context *context)
{
	view_file_browser(context, 1);
}

void view_lock_on(struct nk_context *context)
{
	view_file_browser(context, 0);
}

void view_about(struct nk_context *context)
{
}

typedef struct {
	const char *title;
	view_fun   next_view;
} menu_item;

static save_slot_info *slots;
static uint32_t num_slots, selected_slot;

void view_choose_state(struct nk_context *context, uint8_t is_load)
{
	uint32_t width = render_width();
	uint32_t height = render_height();
	if (nk_begin(context, "Slot Picker", nk_rect(0, 0, width, height), 0)) {
		nk_layout_row_static(context, height - 100, width - 60, 1);
		if (nk_group_begin(context, "Select Save Slot", NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
			nk_layout_row_static(context, 28, width-100, 1);
			if (!slots) {
				slots = get_slot_info(current_system, &num_slots);
			}
			for (uint32_t i = 0; i < num_slots; i++)
			{
				int selected = i == selected_slot;
				nk_selectable_label(context, slots[i].desc, NK_TEXT_ALIGN_LEFT, &selected);
				if (selected && (slots[i].modification_time || !is_load)) {
					selected_slot = i;
				}
			}
			nk_group_end(context);
		}
		nk_layout_row_static(context, 52, width > 600 ? 300 : width / 2, 2);
		if (nk_button_label(context, "Back")) {
			pop_view();
		}
		if (is_load) {
			if (nk_button_label(context, "Load")) {
				current_system->load_state(current_system, selected_slot);
				current_view = view_play;
			}
		} else {
			if (nk_button_label(context, "Save")) {
				current_system->save_state = selected_slot + 1;
				current_view = view_play;
			}
		}
		nk_end(context);
	}
}

void view_save_state(struct nk_context *context)
{
	view_choose_state(context, 0);
}

void view_load_state(struct nk_context *context)
{
	view_choose_state(context, 1);
}

static void menu(struct nk_context *context, uint32_t num_entries, const menu_item *items)
{
	const uint32_t button_height = 52;
	const uint32_t ideal_button_width = 300;
	const uint32_t button_space = 6;
	
	uint32_t width = render_width();
	uint32_t height = render_height();
	uint32_t top = height/2 - (button_height * num_entries)/2;
	uint32_t button_width = width > ideal_button_width ? ideal_button_width : width;
	uint32_t left = width/2 - button_width/2;
	
	nk_layout_space_begin(context, NK_STATIC, top + button_height * num_entries, num_entries);
	for (uint32_t i = 0; i < num_entries; i++)
	{
		nk_layout_space_push(context, nk_rect(left, top + i * button_height, button_width, button_height-button_space));
		if (nk_button_label(context, items[i].title)) {
			push_view(items[i].next_view);
			if (!current_view) {
				exit(0);
			}
			if (current_view == view_save_state || current_view == view_load_state) {
				free_slot_info(slots);
				slots = NULL;
			}
		}
	}
	nk_layout_space_end(context);
}

void view_key_bindings(struct nk_context *context)
{
	
}
void view_controllers(struct nk_context *context)
{
	
}

void settings_toggle(struct nk_context *context, char *label, char *path, uint8_t def)
{
	uint8_t curval = !strcmp("on", tern_find_path_default(config, path, (tern_val){.ptrval = def ? "on": "off"}, TVAL_PTR).ptrval);
	nk_label(context, label, NK_TEXT_LEFT);
	uint8_t newval = nk_check_label(context, "", curval);
	if (newval != curval) {
		config = tern_insert_path(config, path, (tern_val){.ptrval = strdup(newval ? "on" : "off")}, TVAL_PTR);
	}
}

void settings_int_input(struct nk_context *context, char *label, char *path, char *def)
{
	char buffer[12];
	nk_label(context, label, NK_TEXT_LEFT);
	uint32_t curval;
	char *curstr = tern_find_path_default(config, path, (tern_val){.ptrval = def}, TVAL_PTR).ptrval;
	uint32_t len = strlen(curstr);
	if (len > 11) {
		len = 11;
	}
	memcpy(buffer, curstr, len);
	nk_edit_string(context, NK_EDIT_SIMPLE, buffer, &len, sizeof(buffer)-1, nk_filter_decimal);
	buffer[len] = 0;
	if (strcmp(buffer, curstr)) {
		config = tern_insert_path(config, path, (tern_val){.ptrval = strdup(buffer)}, TVAL_PTR);
	}
}

void settings_int_property(struct nk_context *context, char *label, char *name, char *path, int def, int min, int max)
{
	char *curstr = tern_find_path(config, path, TVAL_PTR).ptrval;
	int curval = curstr ? atoi(curstr) : def;
	nk_label(context, label, NK_TEXT_LEFT);
	int val = curval;
	nk_property_int(context, name, min, &val, max, 1, 1.0f);
	if (val != curval) {
		char buffer[12];
		sprintf(buffer, "%d", val);
		config = tern_insert_path(config, path, (tern_val){.ptrval = strdup(buffer)}, TVAL_PTR);
	}
}

void view_video_settings(struct nk_context *context)
{
	uint32_t width = render_width();
	uint32_t height = render_height();
	if (nk_begin(context, "Video Settings", nk_rect(0, 0, width, height), 0)) {
		nk_layout_row_static(context, 30, width > 300 ? 300 : width, 2);
		settings_toggle(context, "Fullscreen", "video\0fullscreen\0", 0);
		settings_toggle(context, "Open GL", "video\0gl\0", 1);
		settings_toggle(context, "Scanlines", "video\0scanlines\0", 0);
		settings_int_input(context, "Windowed Width", "video\0width\0", "640");
		settings_int_property(context, "NTSC Overscan", "Top", "video\0ntsc\0overscan\0top\0", 2, 0, 32);
		settings_int_property(context, "", "Bottom", "video\0ntsc\0overscan\0bottom\0", 17, 0, 32);
		settings_int_property(context, "", "Left", "video\0ntsc\0overscan\0left\0", 13, 0, 32);
		settings_int_property(context, "", "Right", "video\0ntsc\0overscan\0right\0", 14, 0, 32);
		settings_int_property(context, "PAL Overscan", "Top", "video\0pal\0overscan\0top\0", 2, 0, 32);
		settings_int_property(context, "", "Bottom", "video\0pal\0overscan\0bottom\0", 17, 0, 32);
		settings_int_property(context, "", "Left", "video\0pal\0overscan\0left\0", 13, 0, 32);
		settings_int_property(context, "", "Right", "video\0pal\0overscan\0right\0", 14, 0, 32);
		if (nk_button_label(context, "Back")) {
			pop_view();
		}
		nk_end(context);
	}
}
void view_audio_settings(struct nk_context *context)
{
	
}
void view_system_settings(struct nk_context *context)
{
	
}

void view_back(struct nk_context *context)
{
	pop_view();
	pop_view();
	current_view(context);
}

void view_settings(struct nk_context *context)
{
	static menu_item items[] = {
		{"Key Bindings", view_key_bindings},
		{"Controllers", view_controllers},
		{"Video", view_video_settings},
		{"Audio", view_audio_settings},
		{"System", view_system_settings},
		{"Back", view_back}
	};
	
	const uint32_t num_buttons = 6;
	if (nk_begin(context, "Settings Menu", nk_rect(0, 0, render_width(), render_height()), 0)) {
		menu(context, sizeof(items)/sizeof(*items), items);
		nk_end(context);
	}
}

void view_pause(struct nk_context *context)
{
	static menu_item items[] = {
		{"Resume", view_play},
		{"Load ROM", view_load},
		{"Lock On", view_lock_on},
		{"Save State", view_save_state},
		{"Load State", view_load_state},
		{"Settings", view_settings},
		{"Exit", NULL}
	};
	
	const uint32_t num_buttons = 3;
	if (nk_begin(context, "Main Menu", nk_rect(0, 0, render_width(), render_height()), 0)) {
		menu(context, sizeof(items)/sizeof(*items), items);
		nk_end(context);
	}
}

void view_menu(struct nk_context *context)
{
	static menu_item items[] = {
		{"Load ROM", view_load},
		{"Settings", view_settings},
		{"About", view_about},
		{"Exit", NULL}
	};
	
	const uint32_t num_buttons = 3;
	if (nk_begin(context, "Main Menu", nk_rect(0, 0, render_width(), render_height()), 0)) {
		menu(context, sizeof(items)/sizeof(*items), items);
		nk_end(context);
	}
}

void blastem_nuklear_render(void)
{
	nk_input_end(context);
	current_view(context);
	nk_sdl_render(NK_ANTI_ALIASING_ON, 512 * 1024, 128 * 1024);
	nk_input_begin(context);
}

void ui_idle_loop(void)
{
	const uint32_t MIN_UI_DELAY = 15;
	static uint32_t last;
	while (current_view != view_play)
	{
		uint32_t current = render_elapsed_ms();
		if ((current - last) < MIN_UI_DELAY) {
			render_sleep_ms(MIN_UI_DELAY - (current - last) - 1);
		}
		last = current;
		render_update_display();
	}
}
static void handle_event(SDL_Event *event)
{
	nk_sdl_handle_event(event);
}

static void context_destroyed(void)
{
	nk_sdl_device_destroy();
}
static void context_created(void)
{
	nk_sdl_device_create();
	struct nk_font_atlas *atlas;
	nk_sdl_font_stash_begin(&atlas);
	char *font = default_font_path();
	if (!font) {
		fatal_error("Failed to find default font path\n");
	}
	struct nk_font *def_font = nk_font_atlas_add_from_file(atlas, font, 30, NULL);
	nk_sdl_font_stash_end();
	nk_style_set_font(context, &def_font->handle);
}

void show_pause_menu(void)
{
	context->style.window.background = nk_rgba(0, 0, 0, 128);
	context->style.window.fixed_background = nk_style_item_color(nk_rgba(0, 0, 0, 128));
	current_view = view_pause;
	current_system->request_exit(current_system);
}

static uint8_t active;
uint8_t is_nuklear_active(void)
{
	return active;
}

uint8_t is_nuklear_available(void)
{
	if (!render_has_gl()) {
		//currently no fallback if GL2 unavailable
		return 0;
	}
	char *style = tern_find_path(config, "ui\0style\0", TVAL_PTR).ptrval;
	if (!style) {
		return 1;
	}
	return strcmp(style, "rom") != 0;
}

void blastem_nuklear_init(uint8_t file_loaded)
{
	context = nk_sdl_init(render_get_window());
	
	struct nk_font_atlas *atlas;
	nk_sdl_font_stash_begin(&atlas);
	char *font = default_font_path();
	if (!font) {
		fatal_error("Failed to find default font path\n");
	}
	struct nk_font *def_font = nk_font_atlas_add_from_file(atlas, font, 30, NULL);
	nk_sdl_font_stash_end();
	nk_style_set_font(context, &def_font->handle);
	current_view = file_loaded ? view_play : view_menu;
	render_set_ui_render_fun(blastem_nuklear_render);
	render_set_event_handler(handle_event);
	render_set_gl_context_handlers(context_destroyed, context_created);
	active = 1;
	ui_idle_loop();
}
