/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "render.h"
#include "render_sdl.h"
#include "blastem.h"
#include "genesis.h"
#include "bindings.h"
#include "util.h"
#include "ppm.h"
#include "png.h"
#include "config.h"
#include "controller_info.h"

#ifndef DISABLE_OPENGL
#include <GL/glew.h>
#endif

#define MAX_EVENT_POLL_PER_FRAME 2

static SDL_Window *main_window;
static SDL_Window **extra_windows;
static SDL_Renderer *main_renderer;
static SDL_Renderer **extra_renderers;
static SDL_Texture  **sdl_textures;
static uint8_t num_textures;
static SDL_Rect      main_clip;
static SDL_GLContext *main_context;

static int main_width, main_height, windowed_width, windowed_height, is_fullscreen;

static uint8_t render_gl = 1;
static uint8_t scanlines = 0;

static uint32_t last_frame = 0;

static uint32_t buffer_samples, sample_rate;
static uint32_t missing_count;

static SDL_mutex * audio_mutex;
static SDL_cond * audio_ready;
static uint8_t quitting = 0;

struct audio_source {
	SDL_cond *cond;
	int16_t  *front;
	int16_t  *back;
	double   dt;
	uint64_t buffer_fraction;
	uint64_t buffer_inc;
	uint32_t buffer_pos;
	uint32_t read_start;
	uint32_t read_end;
	uint32_t lowpass_alpha;
	uint32_t mask;
	int16_t  last_left;
	int16_t  last_right;
	uint8_t  num_channels;
	uint8_t  front_populated;
};

static audio_source *audio_sources[8];
static audio_source *inactive_audio_sources[8];
static uint8_t num_audio_sources;
static uint8_t num_inactive_audio_sources;
static uint8_t sync_to_audio;
static uint32_t min_buffered;

typedef int32_t (*mix_func)(audio_source *audio, void *vstream, int len);

static int32_t mix_s16(audio_source *audio, void *vstream, int len)
{
	int samples = len/(sizeof(int16_t)*2);
	int16_t *stream = vstream;
	int16_t *end = stream + 2*samples;
	int16_t *src = audio->front;
	uint32_t i = audio->read_start;
	uint32_t i_end = audio->read_end;
	int16_t *cur = stream;
	if (audio->num_channels == 1) {
		while (cur < end && i != i_end)
		{
			*(cur++) += src[i];
			*(cur++) += src[i++];
			i &= audio->mask;
		}
	} else {
		while (cur < end && i != i_end)
		{
			*(cur++) += src[i++];
			*(cur++) += src[i++];
			i &= audio->mask;
		}
	}
	
	if (cur != end) {
		printf("Underflow of %d samples, read_start: %d, read_end: %d, mask: %X\n", (int)(end-cur)/2, audio->read_start, audio->read_end, audio->mask);
	}
	if (!sync_to_audio) {
		audio->read_start = i;
	}
	if (cur != end) {
		//printf("Underflow of %d samples, read_start: %d, read_end: %d, mask: %X\n", (int)(end-cur)/2, audio->read_start, audio->read_end, audio->mask);
		return (cur-end)/2;
	} else {
		return ((i_end - i) & audio->mask) / audio->num_channels;
	}
}

static int32_t mix_f32(audio_source *audio, void *vstream, int len)
{
	int samples = len/(sizeof(float)*2);
	float *stream = vstream;
	float *end = stream + 2*samples;
	int16_t *src = audio->front;
	uint32_t i = audio->read_start;
	uint32_t i_end = audio->read_end;
	float *cur = stream;
	if (audio->num_channels == 1) {
		while (cur < end && i != i_end)
		{
			*(cur++) += ((float)src[i]) / 0x7FFF;
			*(cur++) += ((float)src[i++]) / 0x7FFF;
			i &= audio->mask;
		}
	} else {
		while(cur < end && i != i_end)
		{
			*(cur++) += ((float)src[i++]) / 0x7FFF;
			*(cur++) += ((float)src[i++]) / 0x7FFF;
			i &= audio->mask;
		}
	}
	if (!sync_to_audio) {
		audio->read_start = i;
	}
	if (cur != end) {
		printf("Underflow of %d samples, read_start: %d, read_end: %d, mask: %X\n", (int)(end-cur)/2, audio->read_start, audio->read_end, audio->mask);
		return (cur-end)/2;
	} else {
		return ((i_end - i) & audio->mask) / audio->num_channels;
	}
}

static int32_t mix_null(audio_source *audio, void *vstream, int len)
{
	return 0;
}

static mix_func mix;

static void audio_callback(void * userdata, uint8_t *byte_stream, int len)
{
	uint8_t num_populated;
	memset(byte_stream, 0, len);
	SDL_LockMutex(audio_mutex);
		do {
			num_populated = 0;
			for (uint8_t i = 0; i < num_audio_sources; i++)
			{
				if (audio_sources[i]->front_populated) {
					num_populated++;
				}
			}
			if (!quitting && num_populated < num_audio_sources) {
				fflush(stdout);
				SDL_CondWait(audio_ready, audio_mutex);
			}
		} while(!quitting && num_populated < num_audio_sources);
		if (!quitting) {
			for (uint8_t i = 0; i < num_audio_sources; i++)
			{
				mix(audio_sources[i], byte_stream, len);
				audio_sources[i]->front_populated = 0;
				SDL_CondSignal(audio_sources[i]->cond);
			}
		}
	SDL_UnlockMutex(audio_mutex);
}

#define NO_LAST_BUFFERED -2000000000
static int32_t last_buffered = NO_LAST_BUFFERED;
static float average_change;
#define BUFFER_FRAMES_THRESHOLD 6
#define BASE_MAX_ADJUST 0.0125
static float max_adjust;
static int32_t cur_min_buffered;
static uint32_t min_remaining_buffer;
static void audio_callback_drc(void *userData, uint8_t *byte_stream, int len)
{
	memset(byte_stream, 0, len);
	if (cur_min_buffered < 0) {
		//underflow last frame, but main thread hasn't gotten a chance to call SDL_PauseAudio yet
		return;
	}
	cur_min_buffered = 0x7FFFFFFF;
	min_remaining_buffer = 0xFFFFFFFF;
	for (uint8_t i = 0; i < num_audio_sources; i++)
	{
		
		int32_t buffered = mix(audio_sources[i], byte_stream, len);
		cur_min_buffered = buffered < cur_min_buffered ? buffered : cur_min_buffered;
		uint32_t remaining = (audio_sources[i]->mask + 1)/audio_sources[i]->num_channels - buffered;
		min_remaining_buffer = remaining < min_remaining_buffer ? remaining : min_remaining_buffer;
	}
}

static void lock_audio()
{
	if (sync_to_audio) {
		SDL_LockMutex(audio_mutex);
	} else {
		SDL_LockAudio();
	}
}

static void unlock_audio()
{
	if (sync_to_audio) {
		SDL_UnlockMutex(audio_mutex);
	} else {
		SDL_UnlockAudio();
	}
}

static void render_close_audio()
{
	SDL_LockMutex(audio_mutex);
		quitting = 1;
		SDL_CondSignal(audio_ready);
	SDL_UnlockMutex(audio_mutex);
	SDL_CloseAudio();
}

#define BUFFER_INC_RES 0x40000000UL

void render_audio_adjust_clock(audio_source *src, uint64_t master_clock, uint64_t sample_divider)
{
	src->buffer_inc = ((BUFFER_INC_RES * (uint64_t)sample_rate) / master_clock) * sample_divider;
}

audio_source *render_audio_source(uint64_t master_clock, uint64_t sample_divider, uint8_t channels)
{
	audio_source *ret = NULL;
	uint32_t alloc_size = sync_to_audio ? channels * buffer_samples : nearest_pow2(min_buffered * 4 * channels);
	lock_audio();
		if (num_audio_sources < 8) {
			ret = malloc(sizeof(audio_source));
			ret->back = malloc(alloc_size * sizeof(int16_t));
			ret->front = sync_to_audio ? malloc(alloc_size * sizeof(int16_t)) : ret->back;
			ret->front_populated = 0;
			ret->cond = SDL_CreateCond();
			ret->num_channels = channels;
			audio_sources[num_audio_sources++] = ret;
		}
	unlock_audio();
	if (!ret) {
		fatal_error("Too many audio sources!");
	} else {
		render_audio_adjust_clock(ret, master_clock, sample_divider);
		double lowpass_cutoff = get_lowpass_cutoff(config);
		double rc = (1.0 / lowpass_cutoff) / (2.0 * M_PI);
		ret->dt = 1.0 / ((double)master_clock / (double)(sample_divider));
		double alpha = ret->dt / (ret->dt + rc);
		ret->lowpass_alpha = (int32_t)(((double)0x10000) * alpha);
		ret->buffer_pos = 0;
		ret->buffer_fraction = 0;
		ret->last_left = ret->last_right = 0;
		ret->read_start = 0;
		ret->read_end = sync_to_audio ? buffer_samples * channels : 0;
		ret->mask = sync_to_audio ? 0xFFFFFFFF : alloc_size-1;
	}
	if (sync_to_audio && SDL_GetAudioStatus() == SDL_AUDIO_PAUSED) {
		SDL_PauseAudio(0);
	}
	return ret;
}

void render_pause_source(audio_source *src)
{
	uint8_t need_pause = 0;
	lock_audio();
		for (uint8_t i = 0; i < num_audio_sources; i++)
		{
			if (audio_sources[i] == src) {
				audio_sources[i] = audio_sources[--num_audio_sources];
				if (sync_to_audio) {
					SDL_CondSignal(audio_ready);
				}
				break;
			}
		}
		if (!num_audio_sources) {
			need_pause = 1;
		}
	unlock_audio();
	if (need_pause) {
		SDL_PauseAudio(1);
	}
	inactive_audio_sources[num_inactive_audio_sources++] = src;
}

void render_resume_source(audio_source *src)
{
	lock_audio();
		if (num_audio_sources < 8) {
			audio_sources[num_audio_sources++] = src;
		}
	unlock_audio();
	for (uint8_t i = 0; i < num_inactive_audio_sources; i++)
	{
		if (inactive_audio_sources[i] == src) {
			inactive_audio_sources[i] = inactive_audio_sources[--num_inactive_audio_sources];
		}
	}
	if (sync_to_audio) {
		SDL_PauseAudio(0);
	}
}

void render_free_source(audio_source *src)
{
	render_pause_source(src);
	
	free(src->front);
	if (sync_to_audio) {
		free(src->back);
		SDL_DestroyCond(src->cond);
	}
	free(src);
}
static uint32_t sync_samples;
static void do_audio_ready(audio_source *src)
{
	if (sync_to_audio) {
		SDL_LockMutex(audio_mutex);
			while (src->front_populated) {
				SDL_CondWait(src->cond, audio_mutex);
			}
			int16_t *tmp = src->front;
			src->front = src->back;
			src->back = tmp;
			src->front_populated = 1;
			src->buffer_pos = 0;
			SDL_CondSignal(audio_ready);
		SDL_UnlockMutex(audio_mutex);
	} else {
		uint32_t num_buffered;
		SDL_LockAudio();
			src->read_end = src->buffer_pos;
			num_buffered = ((src->read_end - src->read_start) & src->mask) / src->num_channels;
		SDL_UnlockAudio();
		if (num_buffered >= min_buffered && SDL_GetAudioStatus() == SDL_AUDIO_PAUSED) {
			SDL_PauseAudio(0);
		}
	}
}

static int16_t lowpass_sample(audio_source *src, int16_t last, int16_t current)
{
	int32_t tmp = current * src->lowpass_alpha + last * (0x10000 - src->lowpass_alpha);
	current = tmp >> 16;
	return current;
}

static void interp_sample(audio_source *src, int16_t last, int16_t current)
{
	int64_t tmp = last * ((src->buffer_fraction << 16) / src->buffer_inc);
	tmp += current * (0x10000 - ((src->buffer_fraction << 16) / src->buffer_inc));
	src->back[src->buffer_pos++] = tmp >> 16;
}

void render_put_mono_sample(audio_source *src, int16_t value)
{
	value = lowpass_sample(src, src->last_left, value);
	src->buffer_fraction += src->buffer_inc;
	uint32_t base = sync_to_audio ? 0 : src->read_end;
	while (src->buffer_fraction > BUFFER_INC_RES)
	{
		src->buffer_fraction -= BUFFER_INC_RES;
		interp_sample(src, src->last_left, value);
		
		if (((src->buffer_pos - base) & src->mask) >= sync_samples) {
			do_audio_ready(src);
		}
		src->buffer_pos &= src->mask;
	}
	src->last_left = value;
}

void render_put_stereo_sample(audio_source *src, int16_t left, int16_t right)
{
	left = lowpass_sample(src, src->last_left, left);
	right = lowpass_sample(src, src->last_right, right);
	src->buffer_fraction += src->buffer_inc;
	uint32_t base = sync_to_audio ? 0 : src->read_end;
	while (src->buffer_fraction > BUFFER_INC_RES)
	{
		src->buffer_fraction -= BUFFER_INC_RES;
		
		interp_sample(src, src->last_left, left);
		interp_sample(src, src->last_right, right);
		
		if (((src->buffer_pos - base) & src->mask)/2 >= sync_samples) {
			do_audio_ready(src);
		}
		src->buffer_pos &= src->mask;
	}
	src->last_left = left;
	src->last_right = right;
}

static SDL_Joystick * joysticks[MAX_JOYSTICKS];
static int joystick_sdl_index[MAX_JOYSTICKS];

int render_width()
{
	return main_width;
}

int render_height()
{
	return main_height;
}

int render_fullscreen()
{
	return is_fullscreen;
}

uint32_t render_map_color(uint8_t r, uint8_t g, uint8_t b)
{
	return 255 << 24 | r << 16 | g << 8 | b;
}

#ifndef DISABLE_OPENGL
static GLuint textures[3], buffers[2], vshader, fshader, program, un_textures[2], un_width, un_height, at_pos;

static GLfloat vertex_data_default[] = {
	-1.0f, -1.0f,
	 1.0f, -1.0f,
	-1.0f,  1.0f,
	 1.0f,  1.0f
};

static GLfloat vertex_data[8];

static const GLushort element_data[] = {0, 1, 2, 3};

static GLuint load_shader(char * fname, GLenum shader_type)
{
	char const * parts[] = {get_home_dir(), "/.config/blastem/shaders/", fname};
	char * shader_path = alloc_concat_m(3, parts);
	FILE * f = fopen(shader_path, "rb");
	free(shader_path);
	if (!f) {
		parts[0] = get_exe_dir();
		parts[1] = "/shaders/";
		shader_path = alloc_concat_m(3, parts);
		f = fopen(shader_path, "rb");
		free(shader_path);
		if (!f) {
			warning("Failed to open shader file %s for reading\n", fname);
			return 0;
		}
	}
	long fsize = file_size(f);
	GLchar * text = malloc(fsize);
	if (fread(text, 1, fsize, f) != fsize) {
		warning("Error reading from shader file %s\n", fname);
		free(text);
		return 0;
	}
	GLuint ret = glCreateShader(shader_type);
	glShaderSource(ret, 1, (const GLchar **)&text, (const GLint *)&fsize);
	free(text);
	glCompileShader(ret);
	GLint compile_status, loglen;
	glGetShaderiv(ret, GL_COMPILE_STATUS, &compile_status);
	if (!compile_status) {
		glGetShaderiv(ret, GL_INFO_LOG_LENGTH, &loglen);
		text = malloc(loglen);
		glGetShaderInfoLog(ret, loglen, NULL, text);
		warning("Shader %s failed to compile:\n%s\n", fname, text);
		free(text);
		glDeleteShader(ret);
		return 0;
	}
	return ret;
}
#endif

static uint32_t texture_buf[512 * 513];
#ifndef DISABLE_OPENGL
static void gl_setup()
{
	tern_val def = {.ptrval = "linear"};
	char *scaling = tern_find_path_default(config, "video\0scaling\0", def, TVAL_PTR).ptrval;
	GLint filter = strcmp(scaling, "linear") ? GL_NEAREST : GL_LINEAR;
	glGenTextures(3, textures);
	for (int i = 0; i < 3; i++)
	{
		glBindTexture(GL_TEXTURE_2D, textures[i]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		if (i < 2) {
			//TODO: Fixme for PAL + invalid display mode
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 512, 512, 0, GL_BGRA, GL_UNSIGNED_BYTE, texture_buf);
		} else {
			uint32_t blank = 255 << 24;
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_BGRA, GL_UNSIGNED_BYTE, &blank);
		}
	}
	glGenBuffers(2, buffers);
	glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_data), vertex_data, GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(element_data), element_data, GL_STATIC_DRAW);
	def.ptrval = "default.v.glsl";
	vshader = load_shader(tern_find_path_default(config, "video\0vertex_shader\0", def, TVAL_PTR).ptrval, GL_VERTEX_SHADER);
	def.ptrval = "default.f.glsl";
	fshader = load_shader(tern_find_path_default(config, "video\0fragment_shader\0", def, TVAL_PTR).ptrval, GL_FRAGMENT_SHADER);
	program = glCreateProgram();
	glAttachShader(program, vshader);
	glAttachShader(program, fshader);
	glLinkProgram(program);
	GLint link_status;
	glGetProgramiv(program, GL_LINK_STATUS, &link_status);
	if (!link_status) {
		fputs("Failed to link shader program\n", stderr);
		exit(1);
	}
	un_textures[0] = glGetUniformLocation(program, "textures[0]");
	un_textures[1] = glGetUniformLocation(program, "textures[1]");
	un_width = glGetUniformLocation(program, "width");
	un_height = glGetUniformLocation(program, "height");
	at_pos = glGetAttribLocation(program, "pos");
}

static void gl_teardown()
{
	glDeleteProgram(program);
	glDeleteShader(vshader);
	glDeleteShader(fshader);
	glDeleteBuffers(2, buffers);
	glDeleteTextures(3, textures);
}
#endif

static uint8_t texture_init;
static void render_alloc_surfaces()
{
	if (texture_init) {
		return;
	}
	sdl_textures= malloc(sizeof(SDL_Texture *) * 2);
	num_textures = 2;
	texture_init = 1;
#ifndef DISABLE_OPENGL
	if (render_gl) {
		sdl_textures[0] = sdl_textures[1] = NULL;
		gl_setup();
	} else {
#endif
		tern_val def = {.ptrval = "linear"};
		char *scaling = tern_find_path_default(config, "video\0scaling\0", def, TVAL_PTR).ptrval;
		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, scaling);
		//TODO: Fixme for invalid display mode
		sdl_textures[0] = sdl_textures[1] = SDL_CreateTexture(main_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, LINEBUF_SIZE, 588);
#ifndef DISABLE_OPENGL
	}
#endif
}

static void free_surfaces(void)
{
	for (int i = 0; i < num_textures; i++)
	{
		if (sdl_textures[i]) {
			SDL_DestroyTexture(sdl_textures[i]);
		}
	}
	free(sdl_textures);
	sdl_textures = NULL;
	texture_init = 0;
}

static char * caption = NULL;
static char * fps_caption = NULL;

static void render_quit()
{
	render_close_audio();
	free_surfaces();
#ifndef DISABLE_OPENGL
	if (render_gl) {
		gl_teardown();
		SDL_GL_DeleteContext(main_context);
	}
#endif
}

static float config_aspect()
{
	static float aspect = 0.0f;
	if (aspect == 0.0f) {
		char *config_aspect = tern_find_path_default(config, "video\0aspect\0", (tern_val){.ptrval = "4:3"}, TVAL_PTR).ptrval;
		if (strcmp("stretch", config_aspect)) {
			aspect = 4.0f/3.0f;
			char *end;
			float aspect_numerator = strtof(config_aspect, &end);
			if (aspect_numerator > 0.0f && *end == ':') {
				float aspect_denominator = strtof(end+1, &end);
				if (aspect_denominator > 0.0f && !*end) {
					aspect = aspect_numerator / aspect_denominator;
				}
			}
		} else {
			aspect = -1.0f;
		}
	}
	return aspect;
}

static void update_aspect()
{
	//reset default values
	memcpy(vertex_data, vertex_data_default, sizeof(vertex_data));
	main_clip.w = main_width;
	main_clip.h = main_height;
	main_clip.x = main_clip.y = 0;
	if (config_aspect() > 0.0f) {
		float aspect = (float)main_width / main_height;
		if (fabs(aspect - config_aspect()) < 0.01f) {
			//close enough for government work
			return;
		}
#ifndef DISABLE_OPENGL
		if (render_gl) {
			for (int i = 0; i < 4; i++)
			{
				if (aspect > config_aspect()) {
					vertex_data[i*2] *= config_aspect()/aspect;
				} else {
					vertex_data[i*2+1] *= aspect/config_aspect();
				}
			}
		} else {
#endif
			main_clip.w = aspect > config_aspect() ? config_aspect() * (float)main_height : main_width;
			main_clip.h = aspect > config_aspect() ? main_height : main_width / config_aspect();
			main_clip.x = (main_width  - main_clip.w) / 2;
			main_clip.y = (main_height - main_clip.h) / 2;
#ifndef DISABLE_OPENGL
		}
#endif
	}
}

static ui_render_fun on_context_destroyed, on_context_created;
void render_set_gl_context_handlers(ui_render_fun destroy, ui_render_fun create)
{
	on_context_destroyed = destroy;
	on_context_created = create;
}

static uint8_t scancode_map[SDL_NUM_SCANCODES] = {
	[SDL_SCANCODE_A] = 0x1C,
	[SDL_SCANCODE_B] = 0x32,
	[SDL_SCANCODE_C] = 0x21,
	[SDL_SCANCODE_D] = 0x23,
	[SDL_SCANCODE_E] = 0x24,
	[SDL_SCANCODE_F] = 0x2B,
	[SDL_SCANCODE_G] = 0x34,
	[SDL_SCANCODE_H] = 0x33,
	[SDL_SCANCODE_I] = 0x43,
	[SDL_SCANCODE_J] = 0x3B,
	[SDL_SCANCODE_K] = 0x42,
	[SDL_SCANCODE_L] = 0x4B,
	[SDL_SCANCODE_M] = 0x3A,
	[SDL_SCANCODE_N] = 0x31,
	[SDL_SCANCODE_O] = 0x44,
	[SDL_SCANCODE_P] = 0x4D,
	[SDL_SCANCODE_Q] = 0x15,
	[SDL_SCANCODE_R] = 0x2D,
	[SDL_SCANCODE_S] = 0x1B,
	[SDL_SCANCODE_T] = 0x2C,
	[SDL_SCANCODE_U] = 0x3C,
	[SDL_SCANCODE_V] = 0x2A,
	[SDL_SCANCODE_W] = 0x1D,
	[SDL_SCANCODE_X] = 0x22,
	[SDL_SCANCODE_Y] = 0x35,
	[SDL_SCANCODE_Z] = 0x1A,
	[SDL_SCANCODE_1] = 0x16,
	[SDL_SCANCODE_2] = 0x1E,
	[SDL_SCANCODE_3] = 0x26,
	[SDL_SCANCODE_4] = 0x25,
	[SDL_SCANCODE_5] = 0x2E,
	[SDL_SCANCODE_6] = 0x36,
	[SDL_SCANCODE_7] = 0x3D,
	[SDL_SCANCODE_8] = 0x3E,
	[SDL_SCANCODE_9] = 0x46,
	[SDL_SCANCODE_0] = 0x45,
	[SDL_SCANCODE_RETURN] = 0x5A,
	[SDL_SCANCODE_ESCAPE] = 0x76,
	[SDL_SCANCODE_SPACE] = 0x29,
	[SDL_SCANCODE_TAB] = 0x0D,
	[SDL_SCANCODE_BACKSPACE] = 0x66,
	[SDL_SCANCODE_MINUS] = 0x4E,
	[SDL_SCANCODE_EQUALS] = 0x55,
	[SDL_SCANCODE_LEFTBRACKET] = 0x54,
	[SDL_SCANCODE_RIGHTBRACKET] = 0x5B,
	[SDL_SCANCODE_BACKSLASH] = 0x5D,
	[SDL_SCANCODE_SEMICOLON] = 0x4C,
	[SDL_SCANCODE_APOSTROPHE] = 0x52,
	[SDL_SCANCODE_GRAVE] = 0x0E,
	[SDL_SCANCODE_COMMA] = 0x41,
	[SDL_SCANCODE_PERIOD] = 0x49,
	[SDL_SCANCODE_SLASH] = 0x4A,
	[SDL_SCANCODE_CAPSLOCK] = 0x58,
	[SDL_SCANCODE_F1] = 0x05,
	[SDL_SCANCODE_F2] = 0x06,
	[SDL_SCANCODE_F3] = 0x04,
	[SDL_SCANCODE_F4] = 0x0C,
	[SDL_SCANCODE_F5] = 0x03,
	[SDL_SCANCODE_F6] = 0x0B,
	[SDL_SCANCODE_F7] = 0x83,
	[SDL_SCANCODE_F8] = 0x0A,
	[SDL_SCANCODE_F9] = 0x01,
	[SDL_SCANCODE_F10] = 0x09,
	[SDL_SCANCODE_F11] = 0x78,
	[SDL_SCANCODE_F12] = 0x07,
	[SDL_SCANCODE_LCTRL] = 0x14,
	[SDL_SCANCODE_LSHIFT] = 0x12,
	[SDL_SCANCODE_LALT] = 0x11,
	[SDL_SCANCODE_RCTRL] = 0x18,
	[SDL_SCANCODE_RSHIFT] = 0x59,
	[SDL_SCANCODE_RALT] = 0x17,
	[SDL_SCANCODE_INSERT] = 0x81,
	[SDL_SCANCODE_PAUSE] = 0x82,
	[SDL_SCANCODE_PRINTSCREEN] = 0x84,
	[SDL_SCANCODE_SCROLLLOCK] = 0x7E,
	[SDL_SCANCODE_DELETE] = 0x85,
	[SDL_SCANCODE_LEFT] = 0x86,
	[SDL_SCANCODE_HOME] = 0x87,
	[SDL_SCANCODE_END] = 0x88,
	[SDL_SCANCODE_UP] = 0x89,
	[SDL_SCANCODE_DOWN] = 0x8A,
	[SDL_SCANCODE_PAGEUP] = 0x8B,
	[SDL_SCANCODE_PAGEDOWN] = 0x8C,
	[SDL_SCANCODE_RIGHT] = 0x8D,
	[SDL_SCANCODE_NUMLOCKCLEAR] = 0x77,
	[SDL_SCANCODE_KP_DIVIDE] = 0x80,
	[SDL_SCANCODE_KP_MULTIPLY] = 0x7C,
	[SDL_SCANCODE_KP_MINUS] = 0x7B,
	[SDL_SCANCODE_KP_PLUS] = 0x79,
	[SDL_SCANCODE_KP_ENTER] = 0x19,
	[SDL_SCANCODE_KP_1] = 0x69,
	[SDL_SCANCODE_KP_2] = 0x72,
	[SDL_SCANCODE_KP_3] = 0x7A,
	[SDL_SCANCODE_KP_4] = 0x6B,
	[SDL_SCANCODE_KP_5] = 0x73,
	[SDL_SCANCODE_KP_6] = 0x74,
	[SDL_SCANCODE_KP_7] = 0x6C,
	[SDL_SCANCODE_KP_8] = 0x75,
	[SDL_SCANCODE_KP_9] = 0x7D,
	[SDL_SCANCODE_KP_0] = 0x70,
	[SDL_SCANCODE_KP_PERIOD] = 0x71,
};

static drop_handler drag_drop_handler;
void render_set_drag_drop_handler(drop_handler handler)
{
	drag_drop_handler = handler;
}

static event_handler custom_event_handler;
void render_set_event_handler(event_handler handler)
{
	custom_event_handler = handler;
}

static int find_joystick_index(SDL_JoystickID instanceID)
{
	for (int i = 0; i < MAX_JOYSTICKS; i++) {
		if (joysticks[i] && SDL_JoystickInstanceID(joysticks[i]) == instanceID) {
			return i;
		}
	}
	return -1;
}

static int lowest_unused_joystick_index()
{
	for (int i = 0; i < MAX_JOYSTICKS; i++) {
		if (!joysticks[i]) {
			return i;
		}
	}
	return -1;
}

SDL_Joystick *render_get_joystick(int index)
{
	if (index >= MAX_JOYSTICKS) {
		return NULL;
	}
	return joysticks[index];
}

char* render_joystick_type_id(int index)
{
	SDL_Joystick *stick = render_get_joystick(index);
	if (!stick) {
		return NULL;
	}
	char *guid_string = malloc(33);
	SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(stick), guid_string, 33);
	return guid_string;
}

SDL_GameController *render_get_controller(int index)
{
	if (index >= MAX_JOYSTICKS) {
		return NULL;
	}
	return SDL_GameControllerOpen(joystick_sdl_index[index]);
}

static uint32_t overscan_top[NUM_VID_STD] = {2, 21};
static uint32_t overscan_bot[NUM_VID_STD] = {1, 17};
static uint32_t overscan_left[NUM_VID_STD] = {13, 13};
static uint32_t overscan_right[NUM_VID_STD] = {14, 14};
static vid_std video_standard = VID_NTSC;

static int32_t handle_event(SDL_Event *event)
{
	if (custom_event_handler) {
		custom_event_handler(event);
	}
	switch (event->type) {
	case SDL_KEYDOWN:
		handle_keydown(event->key.keysym.sym, scancode_map[event->key.keysym.scancode]);
		break;
	case SDL_KEYUP:
		handle_keyup(event->key.keysym.sym, scancode_map[event->key.keysym.scancode]);
		break;
	case SDL_JOYBUTTONDOWN:
		handle_joydown(find_joystick_index(event->jbutton.which), event->jbutton.button);
		break;
	case SDL_JOYBUTTONUP:
		handle_joyup(find_joystick_index(event->jbutton.which), event->jbutton.button);
		break;
	case SDL_JOYHATMOTION:
		handle_joy_dpad(find_joystick_index(event->jhat.which), event->jhat.hat, event->jhat.value);
		break;
	case SDL_JOYAXISMOTION:
		handle_joy_axis(find_joystick_index(event->jaxis.which), event->jaxis.axis, event->jaxis.value);
		break;
	case SDL_JOYDEVICEADDED:
		if (event->jdevice.which < MAX_JOYSTICKS) {
			int index = lowest_unused_joystick_index();
			if (index >= 0) {
				SDL_Joystick * joy = joysticks[index] = SDL_JoystickOpen(event->jdevice.which);
				joystick_sdl_index[index] = event->jdevice.which;
				if (joy) {
					printf("Joystick %d added: %s\n", index, SDL_JoystickName(joy));
					printf("\tNum Axes: %d\n\tNum Buttons: %d\n\tNum Hats: %d\n", SDL_JoystickNumAxes(joy), SDL_JoystickNumButtons(joy), SDL_JoystickNumHats(joy));
					handle_joy_added(index);
				}
			}
		}
		break;
	case SDL_JOYDEVICEREMOVED: {
		int index = find_joystick_index(event->jdevice.which);
		if (index >= 0) {
			SDL_JoystickClose(joysticks[index]);
			joysticks[index] = NULL;
			printf("Joystick %d removed\n", index);
		} else {
			printf("Failed to find removed joystick with instance ID: %d\n", index);
		}
		break;
	}
	case SDL_MOUSEMOTION:
		handle_mouse_moved(event->motion.which, event->motion.x, event->motion.y + overscan_top[video_standard], event->motion.xrel, event->motion.yrel);
		break;
	case SDL_MOUSEBUTTONDOWN:
		handle_mousedown(event->button.which, event->button.button);
		break;
	case SDL_MOUSEBUTTONUP:
		handle_mouseup(event->button.which, event->button.button);
		break;
	case SDL_WINDOWEVENT:
		switch (event->window.event)
		{
		case SDL_WINDOWEVENT_SIZE_CHANGED:
			main_width = event->window.data1;
			main_height = event->window.data2;
			update_aspect();
#ifndef DISABLE_OPENGL
			if (render_gl) {
				if (on_context_destroyed) {
					on_context_destroyed();
				}
				gl_teardown();
				SDL_GL_DeleteContext(main_context);
				main_context = SDL_GL_CreateContext(main_window);
				gl_setup();
				if (on_context_created) {
					on_context_created();
				}
			}
#endif
			break;
		}
		break;
	case SDL_DROPFILE:
		if (drag_drop_handler) {
			drag_drop_handler(event->drop.file);
		}
		SDL_free(event->drop.file);
		break;
	case SDL_QUIT:
		puts("");
		exit(0);
	}
	return 0;
}

static void drain_events()
{
	SDL_Event event;
	while(SDL_PollEvent(&event))
	{
		handle_event(&event);
	}
}

static char *vid_std_names[NUM_VID_STD] = {"ntsc", "pal"};
static int display_hz;
static int source_hz;
static int source_frame;
static int source_frame_count;
static int frame_repeat[60];

static void init_audio()
{
	SDL_AudioSpec desired, actual;
    char * rate_str = tern_find_path(config, "audio\0rate\0", TVAL_PTR).ptrval;
   	int rate = rate_str ? atoi(rate_str) : 0;
   	if (!rate) {
   		rate = 48000;
   	}
    desired.freq = rate;
	desired.format = AUDIO_S16SYS;
	desired.channels = 2;
    char * samples_str = tern_find_path(config, "audio\0buffer\0", TVAL_PTR).ptrval;
   	int samples = samples_str ? atoi(samples_str) : 0;
   	if (!samples) {
   		samples = 512;
   	}
    printf("config says: %d\n", samples);
    desired.samples = samples*2;
	desired.callback = sync_to_audio ? audio_callback : audio_callback_drc;
	desired.userdata = NULL;

	if (SDL_OpenAudio(&desired, &actual) < 0) {
		fatal_error("Unable to open SDL audio: %s\n", SDL_GetError());
	}
	buffer_samples = actual.samples;
	sample_rate = actual.freq;
	printf("Initialized audio at frequency %d with a %d sample buffer, ", actual.freq, actual.samples);
	if (actual.format == AUDIO_S16SYS) {
		puts("signed 16-bit int format");
		mix = mix_s16;
	} else if (actual.format == AUDIO_F32SYS) {
		puts("32-bit float format");
		mix = mix_f32;
	} else {
		printf("unsupported format %X\n", actual.format);
		warning("Unsupported audio sample format: %X\n", actual.format);
		mix = mix_null;
	}
}

void window_setup(void)
{
	uint32_t flags = SDL_WINDOW_RESIZABLE;
	if (is_fullscreen) {
		flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	}
	
	tern_val def = {.ptrval = "video"};
	char *sync_src = tern_find_path_default(config, "system\0sync_source\0", def, TVAL_PTR).ptrval;
	sync_to_audio = !strcmp(sync_src, "audio");
	
	const char *vsync;
	if (sync_to_audio) {
		def.ptrval = "off";
		vsync = tern_find_path_default(config, "video\0vsync\0", def, TVAL_PTR).ptrval;
	} else {
		vsync = "on";
	}
	
	tern_node *video = tern_find_node(config, "video");
	if (video)
	{
		for (int i = 0; i < NUM_VID_STD; i++)
		{
			tern_node *std_settings = tern_find_node(video, vid_std_names[i]);
			if (std_settings) {
				char *val = tern_find_path_default(std_settings, "overscan\0top\0", (tern_val){.ptrval = NULL}, TVAL_PTR).ptrval;
				if (val) {
					overscan_top[i] = atoi(val);
				}
				val = tern_find_path_default(std_settings, "overscan\0bottom\0", (tern_val){.ptrval = NULL}, TVAL_PTR).ptrval;
				if (val) {
					overscan_bot[i] = atoi(val);
				}
				val = tern_find_path_default(std_settings, "overscan\0left\0", (tern_val){.ptrval = NULL}, TVAL_PTR).ptrval;
				if (val) {
					overscan_left[i] = atoi(val);
				}
				val = tern_find_path_default(std_settings, "overscan\0right\0", (tern_val){.ptrval = NULL}, TVAL_PTR).ptrval;
				if (val) {
					overscan_right[i] = atoi(val);
				}
			}
		}
	}
	render_gl = 0;
	
#ifndef DISABLE_OPENGL
	char *gl_enabled_str = tern_find_path_default(config, "video\0gl\0", def, TVAL_PTR).ptrval;
	uint8_t gl_enabled = strcmp(gl_enabled_str, "off") != 0;
	if (gl_enabled)
	{
		flags |= SDL_WINDOW_OPENGL;
		SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
		SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 5);
		SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
		SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	}
#endif
	main_window = SDL_CreateWindow(caption, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, main_width, main_height, flags);
	if (!main_window) {
		fatal_error("Unable to create SDL window: %s\n", SDL_GetError());
	}
#ifndef DISABLE_OPENGL
	if (gl_enabled)
	{
		main_context = SDL_GL_CreateContext(main_window);
		GLenum res = glewInit();
		if (res != GLEW_OK) {
			warning("Initialization of GLEW failed with code %d\n", res);
		}

		if (res == GLEW_OK && GLEW_VERSION_2_0) {
			render_gl = 1;
			if (!strcmp("tear", vsync)) {
				if (SDL_GL_SetSwapInterval(-1) < 0) {
					warning("late tear is not available (%s), using normal vsync\n", SDL_GetError());
					vsync = "on";
				} else {
					vsync = NULL;
				}
			}
			if (vsync) {
				if (SDL_GL_SetSwapInterval(!strcmp("on", vsync)) < 0) {
					warning("Failed to set vsync to %s: %s\n", vsync, SDL_GetError());
				}
			}
		} else {
			warning("OpenGL 2.0 is unavailable, falling back to SDL2 renderer\n");
		}
	}
	if (!render_gl) {
#endif
		flags = SDL_RENDERER_ACCELERATED;
		if (!strcmp("on", vsync) || !strcmp("tear", vsync)) {
			flags |= SDL_RENDERER_PRESENTVSYNC;
		}
		main_renderer = SDL_CreateRenderer(main_window, -1, flags);

		if (!main_renderer) {
			fatal_error("unable to create SDL renderer: %s\n", SDL_GetError());
		}
		main_clip.x = main_clip.y = 0;
		main_clip.w = main_width;
		main_clip.h = main_height;
#ifndef DISABLE_OPENGL
	}
#endif

	SDL_GetWindowSize(main_window, &main_width, &main_height);
	printf("Window created with size: %d x %d\n", main_width, main_height);
	update_aspect();
	render_alloc_surfaces();
	def.ptrval = "off";
	scanlines = !strcmp(tern_find_path_default(config, "video\0scanlines\0", def, TVAL_PTR).ptrval, "on");
}

void render_init(int width, int height, char * title, uint8_t fullscreen)
{
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0) {
		fatal_error("Unable to init SDL: %s\n", SDL_GetError());
	}
	atexit(SDL_Quit);
	if (height <= 0) {
		float aspect = config_aspect() > 0.0f ? config_aspect() : 4.0f/3.0f;
		height = ((float)width / aspect) + 0.5f;
	}
	printf("width: %d, height: %d\n", width, height);
	windowed_width = width;
	windowed_height = height;
	
	SDL_DisplayMode mode;
	//TODO: Explicit multiple monitor support
	SDL_GetCurrentDisplayMode(0, &mode);
	display_hz = mode.refresh_rate;

	if (fullscreen) {
		//the SDL2 migration guide suggests setting width and height to 0 when using SDL_WINDOW_FULLSCREEN_DESKTOP
		//but that doesn't seem to work right when using OpenGL, at least on Linux anyway
		width = mode.w;
		height = mode.h;
	}
	main_width = width;
	main_height = height;
	is_fullscreen = fullscreen;
	
	caption = title;
	
	window_setup();

	audio_mutex = SDL_CreateMutex();
	audio_ready = SDL_CreateCond();
	
	init_audio();
	
	uint32_t db_size;
	char *db_data = read_bundled_file("gamecontrollerdb.txt", &db_size);
	if (db_data) {
		int added = SDL_GameControllerAddMappingsFromRW(SDL_RWFromMem(db_data, db_size), 1);
		free(db_data);
		printf("Added %d game controller mappings from gamecontrollerdb.txt\n", added);
	}
	
	controller_add_mappings();
	
	SDL_JoystickEventState(SDL_ENABLE);
	
	render_set_video_standard(VID_NTSC);

	atexit(render_quit);
}
#include<unistd.h>
static int in_toggle;
static void update_source(audio_source *src, double rc, uint8_t sync_changed)
{
	double alpha = src->dt / (src->dt + rc);
	int32_t lowpass_alpha = (int32_t)(((double)0x10000) * alpha);
	src->lowpass_alpha = lowpass_alpha;
	if (sync_changed) {
		uint32_t alloc_size = sync_to_audio ? src->num_channels * buffer_samples : nearest_pow2(min_buffered * 4 * src->num_channels);
		src->back = realloc(src->back, alloc_size * sizeof(int16_t));
		if (sync_to_audio) {
			src->front = malloc(alloc_size * sizeof(int16_t));
		} else {
			free(src->front);
			src->front = src->back;
		}
		src->mask = sync_to_audio ? 0xFFFFFFFF : alloc_size-1;
		src->read_start = 0;
		src->read_end = sync_to_audio ? buffer_samples * src->num_channels : 0;
		src->buffer_pos = 0;
	}
}

void render_config_updated(void)
{
	uint8_t old_sync_to_audio = sync_to_audio;
	
	free_surfaces();
#ifndef DISABLE_OPENGL
	if (render_gl) {
		if (on_context_destroyed) {
			on_context_destroyed();
		}
		gl_teardown();
		SDL_GL_DeleteContext(main_context);
	} else {
#endif
		SDL_DestroyRenderer(main_renderer);
#ifndef DISABLE_OPENGL
	}
#endif
	in_toggle = 1;
	SDL_DestroyWindow(main_window);
	drain_events();
	
	char *config_width = tern_find_path(config, "video\0width\0", TVAL_PTR).ptrval;
	if (config_width) {
		windowed_width = atoi(config_width);
	}
	char *config_height = tern_find_path(config, "video\0height\0", TVAL_PTR).ptrval;
	if (config_height) {
		windowed_height = atoi(config_height);
	} else {
		float aspect = config_aspect() > 0.0f ? config_aspect() : 4.0f/3.0f;
		windowed_height = ((float)windowed_width / aspect) + 0.5f;
	}
	char *config_fullscreen = tern_find_path(config, "video\0fullscreen\0", TVAL_PTR).ptrval;
	is_fullscreen = config_fullscreen && !strcmp("on", config_fullscreen);
	if (is_fullscreen) {
		SDL_DisplayMode mode;
		//TODO: Multiple monitor support
		SDL_GetCurrentDisplayMode(0, &mode);
		main_width = mode.w;
		main_height = mode.h;
	} else {
		main_width = windowed_width;
		main_height = windowed_height;
	}
	
	window_setup();
	update_aspect();
#ifndef DISABLE_OPENGL
	//need to check render_gl again after window_setup as render option could have changed
	if (render_gl && on_context_created) {
		on_context_created();
	}
#endif

	uint8_t was_paused = SDL_GetAudioStatus() == SDL_AUDIO_PAUSED;
	render_close_audio();
	quitting = 0;
	init_audio();
	render_set_video_standard(video_standard);
	
	double lowpass_cutoff = get_lowpass_cutoff(config);
	double rc = (1.0 / lowpass_cutoff) / (2.0 * M_PI);
	lock_audio();
		for (uint8_t i = 0; i < num_audio_sources; i++)
		{
			update_source(audio_sources[i], rc, old_sync_to_audio != sync_to_audio);
		}
	unlock_audio();
	for (uint8_t i = 0; i < num_inactive_audio_sources; i++)
	{
		update_source(inactive_audio_sources[i], rc, old_sync_to_audio != sync_to_audio);
	}
	drain_events();
	in_toggle = 0;
	if (!was_paused) {
		SDL_PauseAudio(0);
	}
}

SDL_Window *render_get_window(void)
{
	return main_window;
}

void render_set_video_standard(vid_std std)
{
	video_standard = std;
	source_hz = std == VID_PAL ? 50 : 60;
	uint32_t max_repeat = 0;
	if (abs(source_hz - display_hz) < 2) {
		memset(frame_repeat, 0, sizeof(int)*display_hz);
	} else {
		int inc = display_hz * 100000 / source_hz;
		int accum = 0;
		int dst_frames = 0;
		for (int src_frame = 0; src_frame < source_hz; src_frame++)
		{
			frame_repeat[src_frame] = -1;
			accum += inc;
			while (accum > 100000)
			{
				accum -= 100000;
				frame_repeat[src_frame]++;
				max_repeat = frame_repeat[src_frame] > max_repeat ? frame_repeat[src_frame] : max_repeat;
				dst_frames++;
			}
		}
		if (dst_frames != display_hz) {
			frame_repeat[source_hz-1] += display_hz - dst_frames;
		}
	}
	source_frame = 0;
	source_frame_count = frame_repeat[0];
	//sync samples with audio thread approximately every 8 lines
	sync_samples = sync_to_audio ? buffer_samples : 8 * sample_rate / (source_hz * (VID_PAL ? 313 : 262));
	max_repeat++;
	min_buffered = (((float)max_repeat * (float)sample_rate/(float)source_hz)/* / (float)buffer_samples*/);// + 0.9999;
	//min_buffered *= buffer_samples;
	printf("Min samples buffered before audio start: %d\n", min_buffered);
	max_adjust = BASE_MAX_ADJUST / source_hz;
}

void render_update_caption(char *title)
{
	caption = title;
	free(fps_caption);
	fps_caption = NULL;
}

static char *screenshot_path;
void render_save_screenshot(char *path)
{
	if (screenshot_path) {
		free(screenshot_path);
	}
	screenshot_path = path;
}

uint8_t render_create_window(char *caption, uint32_t width, uint32_t height)
{
	uint8_t win_idx = 0xFF;
	for (int i = 0; i < num_textures - FRAMEBUFFER_USER_START; i++)
	{
		if (!extra_windows[i]) {
			win_idx = i;
			break;
		}
	}
	
	if (win_idx == 0xFF) {
		num_textures++;
		sdl_textures = realloc(sdl_textures, num_textures * sizeof(*sdl_textures));
		extra_windows = realloc(extra_windows, (num_textures - FRAMEBUFFER_USER_START) * sizeof(*extra_windows));
		extra_renderers = realloc(extra_renderers, (num_textures - FRAMEBUFFER_USER_START) * sizeof(*extra_renderers));
		win_idx = num_textures - FRAMEBUFFER_USER_START - 1;
	}
	extra_windows[win_idx] = SDL_CreateWindow(caption, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width, height, 0);
	if (!extra_windows[win_idx]) {
		goto fail_window;
	}
	extra_renderers[win_idx] = SDL_CreateRenderer(extra_windows[win_idx], -1, SDL_RENDERER_ACCELERATED);
	if (!extra_renderers[win_idx]) {
		goto fail_renderer;
	}
	uint8_t texture_idx = win_idx + FRAMEBUFFER_USER_START;
	sdl_textures[texture_idx] = SDL_CreateTexture(extra_renderers[win_idx], SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
	if (!sdl_textures[texture_idx]) {
		goto fail_texture;
	}
	return texture_idx;
	
fail_texture:
	SDL_DestroyRenderer(extra_renderers[win_idx]);
fail_renderer:
	SDL_DestroyWindow(extra_windows[win_idx]);
fail_window:
	num_textures--;
	return 0;
}

void render_destroy_window(uint8_t which)
{
	uint8_t win_idx = which - FRAMEBUFFER_USER_START;
	//Destroying the renderers also frees the textures
	SDL_DestroyRenderer(extra_renderers[win_idx]);
	SDL_DestroyWindow(extra_windows[win_idx]);
	
	extra_renderers[win_idx] = NULL;
	extra_windows[win_idx] = NULL;
}

uint32_t *locked_pixels;
uint32_t locked_pitch;
uint32_t *render_get_framebuffer(uint8_t which, int *pitch)
{
#ifndef DISABLE_OPENGL
	if (render_gl && which <= FRAMEBUFFER_EVEN) {
		*pitch = LINEBUF_SIZE * sizeof(uint32_t);
		return texture_buf;
	} else {
#endif
		if (which >= num_textures) {
			warning("Request for invalid framebuffer number %d\n", which);
			return NULL;
		}
		void *pixels;
		if (SDL_LockTexture(sdl_textures[which], NULL, &pixels, pitch) < 0) {
			warning("Failed to lock texture: %s\n", SDL_GetError());
			return NULL;
		}
		static uint8_t last;
		if (which <= FRAMEBUFFER_EVEN) {
			locked_pixels = pixels;
			if (which == FRAMEBUFFER_EVEN) {
				pixels += *pitch;
			}
			locked_pitch = *pitch;
			if (which != last) {
				*pitch *= 2;
			}
			last = which;
		}
		return pixels;
#ifndef DISABLE_OPENGL
	}
#endif
}

uint8_t events_processed;
#ifdef __ANDROID__
#define FPS_INTERVAL 10000
#else
#define FPS_INTERVAL 1000
#endif

static uint32_t last_width, last_height;
static uint8_t interlaced;
void render_framebuffer_updated(uint8_t which, int width)
{
	static uint8_t last;
	if (!sync_to_audio && which <= FRAMEBUFFER_EVEN && source_frame_count < 0) {
		source_frame++;
		if (source_frame >= source_hz) {
			source_frame = 0;
		}
		source_frame_count = frame_repeat[source_frame];
		//TODO: Figure out what to do about SDL Render API texture locking
		return;
	}
	
	last_width = width;
	uint32_t height = which <= FRAMEBUFFER_EVEN 
		? (video_standard == VID_NTSC ? 243 : 294) - (overscan_top[video_standard] + overscan_bot[video_standard])
		: 240;
	FILE *screenshot_file = NULL;
	uint32_t shot_height, shot_width;
	char *ext;
	if (screenshot_path && which == FRAMEBUFFER_ODD) {
		screenshot_file = fopen(screenshot_path, "wb");
		if (screenshot_file) {
#ifndef DISABLE_ZLIB
			ext = path_extension(screenshot_path);
#endif
			info_message("Saving screenshot to %s\n", screenshot_path);
		} else {
			warning("Failed to open screenshot file %s for writing\n", screenshot_path);
		}
		free(screenshot_path);
		screenshot_path = NULL;
		shot_height = video_standard == VID_NTSC ? 243 : 294;
		shot_width = width;
	}
	interlaced = last != which;
	width -= overscan_left[video_standard] + overscan_right[video_standard];
#ifndef DISABLE_OPENGL
	if (render_gl && which <= FRAMEBUFFER_EVEN) {
		SDL_GL_MakeCurrent(main_window, main_context);
		glBindTexture(GL_TEXTURE_2D, textures[which]);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, LINEBUF_SIZE, height, GL_BGRA, GL_UNSIGNED_BYTE, texture_buf + overscan_left[video_standard] + LINEBUF_SIZE * overscan_top[video_standard]);
		
		if (screenshot_file) {
			//properly supporting interlaced modes here is non-trivial, so only save the odd field for now
#ifndef DISABLE_ZLIB
			if (!strcasecmp(ext, "png")) {
				free(ext);
				save_png(screenshot_file, texture_buf, shot_width, shot_height, LINEBUF_SIZE*sizeof(uint32_t));
			} else {
				free(ext);
#endif
				save_ppm(screenshot_file, texture_buf, shot_width, shot_height, LINEBUF_SIZE*sizeof(uint32_t));
#ifndef DISABLE_ZLIB
			}
#endif
		}
	} else {
#endif
		if (which <= FRAMEBUFFER_EVEN && last != which) {
			uint8_t *cur_dst = (uint8_t *)locked_pixels;
			uint8_t *cur_saved = (uint8_t *)texture_buf;
			uint32_t dst_off = which == FRAMEBUFFER_EVEN ? 0 : locked_pitch;
			uint32_t src_off = which == FRAMEBUFFER_EVEN ? locked_pitch : 0;
			for (int i = 0; i < height; ++i)
			{
				//copy saved line from other field
				memcpy(cur_dst + dst_off, cur_saved, locked_pitch);
				//save line from this field to buffer for next frame
				memcpy(cur_saved, cur_dst + src_off, locked_pitch);
				cur_dst += locked_pitch * 2;
				cur_saved += locked_pitch;
			}
			height = 480;
		}
		if (screenshot_file) {
			uint32_t shot_pitch = locked_pitch;
			if (which == FRAMEBUFFER_EVEN) {
				shot_height *= 2;
			} else {
				shot_pitch *= 2;
			}
#ifndef DISABLE_ZLIB
			if (!strcasecmp(ext, "png")) {
				free(ext);
				save_png(screenshot_file, locked_pixels, shot_width, shot_height, shot_pitch);
			} else {
				free(ext);
#endif
				save_ppm(screenshot_file, locked_pixels, shot_width, shot_height, shot_pitch);
#ifndef DISABLE_ZLIB
			}
#endif
		}
		SDL_UnlockTexture(sdl_textures[which]);
#ifndef DISABLE_OPENGL
	}
#endif
	last_height = height;
	if (which <= FRAMEBUFFER_EVEN) {
		render_update_display();
	} else {
		SDL_RenderCopy(extra_renderers[which - FRAMEBUFFER_USER_START], sdl_textures[which], NULL, NULL);
		SDL_RenderPresent(extra_renderers[which - FRAMEBUFFER_USER_START]);
	}
	if (screenshot_file) {
		fclose(screenshot_file);
	}
	if (which <= FRAMEBUFFER_EVEN) {
		last = which;
		static uint32_t frame_counter, start;
		frame_counter++;
		last_frame= SDL_GetTicks();
		if ((last_frame - start) > FPS_INTERVAL) {
			if (start && (last_frame-start)) {
	#ifdef __ANDROID__
				info_message("%s - %.1f fps", caption, ((float)frame_counter) / (((float)(last_frame-start)) / 1000.0));
	#else
				if (!fps_caption) {
					fps_caption = malloc(strlen(caption) + strlen(" - 100000000.1 fps") + 1);
				}
				sprintf(fps_caption, "%s - %.1f fps", caption, ((float)frame_counter) / (((float)(last_frame-start)) / 1000.0));
				SDL_SetWindowTitle(main_window, fps_caption);
	#endif
			}
			start = last_frame;
			frame_counter = 0;
		}
	}
	if (!sync_to_audio) {
		int32_t local_cur_min, local_min_remaining;
		SDL_LockAudio();
			if (last_buffered > NO_LAST_BUFFERED) {
				average_change *= 0.9f;
				average_change += (cur_min_buffered - last_buffered) * 0.1f;
			}
			local_cur_min = cur_min_buffered;
			local_min_remaining = min_remaining_buffer;
			last_buffered = cur_min_buffered;
		SDL_UnlockAudio();
		float frames_to_problem;
		if (average_change < 0) {
			frames_to_problem = (float)local_cur_min / -average_change;
		} else {
			frames_to_problem = (float)local_min_remaining / average_change;
		}
		float adjust_ratio = 0.0f;
		if (
			frames_to_problem < BUFFER_FRAMES_THRESHOLD
			|| (average_change < 0 && local_cur_min < 3*min_buffered / 4)
			|| (average_change >0 && local_cur_min > 5 * min_buffered / 4)
			|| cur_min_buffered < 0
		) {
			
			if (cur_min_buffered < 0) {
				adjust_ratio = max_adjust;
				SDL_PauseAudio(1);
				last_buffered = NO_LAST_BUFFERED;
				cur_min_buffered = 0;
			} else {
				adjust_ratio = -1.0 * average_change / ((float)sample_rate / (float)source_hz);
				adjust_ratio /= 2.5 * source_hz;
				if (fabsf(adjust_ratio) > max_adjust) {
					adjust_ratio = adjust_ratio > 0 ? max_adjust : -max_adjust;
				}
			}
		} else if (local_cur_min < min_buffered / 2) {
			adjust_ratio = max_adjust;
		}
		if (adjust_ratio != 0.0f) {
			average_change = 0;
			for (uint8_t i = 0; i < num_audio_sources; i++)
			{
				audio_sources[i]->buffer_inc = ((double)audio_sources[i]->buffer_inc) + ((double)audio_sources[i]->buffer_inc) * adjust_ratio + 0.5;
			}
		}
		while (source_frame_count > 0)
		{
			render_update_display();
			source_frame_count--;
		}
		source_frame++;
		if (source_frame >= source_hz) {
			source_frame = 0;
		}
		source_frame_count = frame_repeat[source_frame];
	}
}

static ui_render_fun render_ui;
void render_set_ui_render_fun(ui_render_fun fun)
{
	render_ui = fun;
}

void render_update_display()
{
#ifndef DISABLE_OPENGL
	if (render_gl) {
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		glUseProgram(program);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, textures[0]);
		glUniform1i(un_textures[0], 0);

		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, textures[interlaced ? 1 : scanlines ? 2 : 0]);
		glUniform1i(un_textures[1], 1);

		glUniform1f(un_width, render_emulated_width());
		glUniform1f(un_height, last_height);

		glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
		glVertexAttribPointer(at_pos, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat[2]), (void *)0);
		glEnableVertexAttribArray(at_pos);

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
		glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_SHORT, (void *)0);

		glDisableVertexAttribArray(at_pos);
		
		if (render_ui) {
			render_ui();
		}

		SDL_GL_SwapWindow(main_window);
	} else {
#endif
		SDL_Rect src_clip = {
			.x = overscan_left[video_standard],
			.y = overscan_top[video_standard],
			.w = render_emulated_width(),
			.h = last_height
		};
		SDL_SetRenderDrawColor(main_renderer, 0, 0, 0, 255);
		SDL_RenderClear(main_renderer);
		SDL_RenderCopy(main_renderer, sdl_textures[FRAMEBUFFER_ODD], &src_clip, &main_clip);
		if (render_ui) {
			render_ui();
		}
		SDL_RenderPresent(main_renderer);
#ifndef DISABLE_OPENGL
	}
#endif
	if (!events_processed) {
		process_events();
	}
	events_processed = 0;
}

uint32_t render_emulated_width()
{
	return last_width - overscan_left[video_standard] - overscan_right[video_standard];
}

uint32_t render_emulated_height()
{
	return (video_standard == VID_NTSC ? 243 : 294) - overscan_top[video_standard] - overscan_bot[video_standard];
}

uint32_t render_overscan_left()
{
	return overscan_left[video_standard];
}

uint32_t render_overscan_top()
{
	return overscan_top[video_standard];
}

void render_wait_quit(vdp_context * context)
{
	SDL_Event event;
	while(SDL_WaitEvent(&event)) {
		switch (event.type) {
		case SDL_QUIT:
			return;
		}
	}
}

int render_lookup_button(char *name)
{
	static tern_node *button_lookup;
	if (!button_lookup) {
		for (int i = SDL_CONTROLLER_BUTTON_A; i < SDL_CONTROLLER_BUTTON_MAX; i++)
		{
			button_lookup = tern_insert_int(button_lookup, SDL_GameControllerGetStringForButton(i), i);
		}
		//alternative Playstation-style names
		button_lookup = tern_insert_int(button_lookup, "cross", SDL_CONTROLLER_BUTTON_A);
		button_lookup = tern_insert_int(button_lookup, "circle", SDL_CONTROLLER_BUTTON_B);
		button_lookup = tern_insert_int(button_lookup, "square", SDL_CONTROLLER_BUTTON_X);
		button_lookup = tern_insert_int(button_lookup, "triangle", SDL_CONTROLLER_BUTTON_Y);
		button_lookup = tern_insert_int(button_lookup, "share", SDL_CONTROLLER_BUTTON_BACK);
		button_lookup = tern_insert_int(button_lookup, "select", SDL_CONTROLLER_BUTTON_BACK);
		button_lookup = tern_insert_int(button_lookup, "options", SDL_CONTROLLER_BUTTON_START);
		button_lookup = tern_insert_int(button_lookup, "l1", SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
		button_lookup = tern_insert_int(button_lookup, "r1", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
		button_lookup = tern_insert_int(button_lookup, "l3", SDL_CONTROLLER_BUTTON_LEFTSTICK);
		button_lookup = tern_insert_int(button_lookup, "r3", SDL_CONTROLLER_BUTTON_RIGHTSTICK);
	}
	return (int)tern_find_int(button_lookup, name, SDL_CONTROLLER_BUTTON_INVALID);
}

int render_lookup_axis(char *name)
{
	static tern_node *axis_lookup;
	if (!axis_lookup) {
		for (int i = SDL_CONTROLLER_AXIS_LEFTX; i < SDL_CONTROLLER_AXIS_MAX; i++)
		{
			axis_lookup = tern_insert_int(axis_lookup, SDL_GameControllerGetStringForAxis(i), i);
		}
		//alternative Playstation-style names
		axis_lookup = tern_insert_int(axis_lookup, "l2", SDL_CONTROLLER_AXIS_TRIGGERLEFT);
		axis_lookup = tern_insert_int(axis_lookup, "r2", SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
	}
	return (int)tern_find_int(axis_lookup, name, SDL_CONTROLLER_AXIS_INVALID);
}

int32_t render_translate_input_name(int32_t controller, char *name, uint8_t is_axis)
{
	tern_node *button_lookup, *axis_lookup;
	if (controller > MAX_JOYSTICKS || !joysticks[controller]) {
		return RENDER_NOT_PLUGGED_IN;
	}
	
	if (!SDL_IsGameController(joystick_sdl_index[controller])) {
		return RENDER_NOT_MAPPED;
	}
	SDL_GameController *control = SDL_GameControllerOpen(joystick_sdl_index[controller]);
	if (!control) {
		warning("Failed to open game controller %d: %s\n", controller, SDL_GetError());
		return RENDER_NOT_PLUGGED_IN;
	}
	
	SDL_GameControllerButtonBind cbind;
	if (is_axis) {
		
		int sdl_axis = render_lookup_axis(name);
		if (sdl_axis == SDL_CONTROLLER_AXIS_INVALID) {
			SDL_GameControllerClose(control);
			return RENDER_INVALID_NAME;
		}
		cbind = SDL_GameControllerGetBindForAxis(control, sdl_axis);
	} else {
		int sdl_button = render_lookup_button(name);
		if (sdl_button == SDL_CONTROLLER_BUTTON_INVALID) {
			SDL_GameControllerClose(control);
			return RENDER_INVALID_NAME;
		}
		cbind = SDL_GameControllerGetBindForButton(control, sdl_button);
	}
	SDL_GameControllerClose(control);
	switch (cbind.bindType)
	{
	case SDL_CONTROLLER_BINDTYPE_BUTTON:
		return cbind.value.button;
	case SDL_CONTROLLER_BINDTYPE_AXIS:
		return RENDER_AXIS_BIT | cbind.value.axis;
	case SDL_CONTROLLER_BINDTYPE_HAT:
		return RENDER_DPAD_BIT | (cbind.value.hat.hat << 4) | cbind.value.hat.hat_mask;
	}
	return RENDER_NOT_MAPPED;
}

int32_t render_dpad_part(int32_t input)
{
	return input >> 4 & 0xFFFFFF;
}

uint8_t render_direction_part(int32_t input)
{
	return input & 0xF;
}

int32_t render_axis_part(int32_t input)
{
	return input & 0xFFFFFFF;
}

void process_events()
{
	if (events_processed > MAX_EVENT_POLL_PER_FRAME) {
		return;
	}
	drain_events();
	events_processed++;
}

#define TOGGLE_MIN_DELAY 250
void render_toggle_fullscreen()
{
	//protect against event processing causing us to attempt to toggle while still toggling
	if (in_toggle) {
		return;
	}
	in_toggle = 1;
	
	//toggling too fast seems to cause a deadlock
	static uint32_t last_toggle;
	uint32_t cur = SDL_GetTicks();
	if (last_toggle && cur - last_toggle < TOGGLE_MIN_DELAY) {
		in_toggle = 0;
		return;
	}
	last_toggle = cur;
	
	drain_events();
	is_fullscreen = !is_fullscreen;
	if (is_fullscreen) {
		SDL_DisplayMode mode;
		//TODO: Multiple monitor support
		SDL_GetCurrentDisplayMode(0, &mode);
		//In theory, the SDL2 docs suggest this is unnecessary
		//but without it the OpenGL context remains the original size
		//This needs to happen before the fullscreen transition to have any effect
		//because SDL does not apply window size changes in fullscreen
		SDL_SetWindowSize(main_window, mode.w, mode.h);
	}
	SDL_SetWindowFullscreen(main_window, is_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
	//Since we change the window size on transition to full screen
	//we need to set it back to normal so we can also go back to windowed mode
	//normally you would think that this should only be done when actually transitioning
	//but something is screwy in the guts of SDL (at least on Linux) and setting it each time
	//is the only thing that seems to work reliably
	//when we've just switched to fullscreen mode this should be harmless though
	SDL_SetWindowSize(main_window, windowed_width, windowed_height);
	drain_events();
	in_toggle = 0;
}

uint32_t render_audio_buffer()
{
	return buffer_samples;
}

uint32_t render_sample_rate()
{
	return sample_rate;
}

void render_errorbox(char *title, char *message)
{
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, NULL);
}

void render_warnbox(char *title, char *message)
{
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, title, message, NULL);
}

void render_infobox(char *title, char *message)
{
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title, message, NULL);
}

uint32_t render_elapsed_ms(void)
{
	return SDL_GetTicks();
}

void render_sleep_ms(uint32_t delay)
{
	return SDL_Delay(delay);
}

uint8_t render_has_gl(void)
{
	return render_gl;
}

uint8_t render_get_active_framebuffer(void)
{
	if (SDL_GetWindowFlags(main_window) & SDL_WINDOW_INPUT_FOCUS) {
		return FRAMEBUFFER_ODD;
	}
	for (int i = 0; i < num_textures - 2; i++)
	{
		if (extra_windows[i] && (SDL_GetWindowFlags(extra_windows[i]) & SDL_WINDOW_INPUT_FOCUS)) {
			return FRAMEBUFFER_USER_START + i; 
		}
	}
	return 0xFF;
}
