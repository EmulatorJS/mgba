#include "libretro.h"

#include "gba/gba.h"
#include "gba/renderers/video-software.h"
#include "gba/serialize.h"
#include "gba/video.h"
#include "util/vfs.h"

static retro_environment_t environCallback;
static retro_video_refresh_t videoCallback;
static retro_audio_sample_t audioCallback;
static retro_input_poll_t inputPollCallback;
static retro_input_state_t inputCallback;
static retro_log_printf_t logCallback;

static void GBARetroLog(struct GBAThread* thread, enum GBALogLevel level, const char* format, va_list args);

static void _postAudioFrame(struct GBAAVStream*, int16_t left, int16_t right);
static void _postVideoFrame(struct GBAAVStream*, struct GBAVideoRenderer* renderer);

static struct GBA gba;
static struct ARMCore cpu;
static struct GBAVideoSoftwareRenderer renderer;
static struct VFile* rom;
static struct VFile* save;
static void* savedata;
static struct GBAAVStream stream;

unsigned retro_api_version(void) {
   return RETRO_API_VERSION;
}

void retro_set_environment(retro_environment_t environ) {
	environCallback = environ;
}

void retro_set_video_refresh(retro_video_refresh_t video) {
	videoCallback = video;
}

void retro_set_audio_sample(retro_audio_sample_t audio) {
	audioCallback = audio;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t audioBatch) {
	UNUSED(audioBatch);
}

void retro_set_input_poll(retro_input_poll_t inputPoll) {
	inputPollCallback = inputPoll;
}

void retro_set_input_state(retro_input_state_t input) {
	inputCallback = input;
}

void retro_get_system_info(struct retro_system_info* info) {
   info->need_fullpath = false;
   info->valid_extensions = "gba";
   info->library_version = PROJECT_VERSION;
   info->library_name = PROJECT_NAME;
   info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info* info) {
   info->geometry.base_width = VIDEO_HORIZONTAL_PIXELS;
   info->geometry.base_height = VIDEO_VERTICAL_PIXELS;
   info->geometry.max_width = VIDEO_HORIZONTAL_PIXELS;
   info->geometry.max_height = VIDEO_VERTICAL_PIXELS;
   info->timing.fps =  GBA_ARM7TDMI_FREQUENCY / (float) VIDEO_TOTAL_LENGTH;
   info->timing.sample_rate = 32768;
}

void retro_init(void) {
	enum retro_pixel_format fmt;
#ifdef COLOR_16_BIT
#ifdef COLOR_5_6_5
	fmt = RETRO_PIXEL_FORMAT_RGB565;
#else
	fmt = RETRO_PIXEL_FORMAT_0RGB1555;
#endif
#else
	fmt = RETRO_PIXEL_FORMAT_XRGB8888;
#endif
	environCallback(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);

	struct retro_input_descriptor inputDescriptors[] = {
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L" }
	};
	environCallback(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, &inputDescriptors);

	// TODO: RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME when BIOS booting is supported
	// TODO: RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE

	struct retro_log_callback log;
	if (environCallback(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)) {
		logCallback = log.log;
	} else {
		logCallback = 0;
	}

	stream.postAudioFrame = _postAudioFrame;
	stream.postVideoFrame = _postVideoFrame;

	GBACreate(&gba);
	ARMSetComponents(&cpu, &gba.d, 0, 0);
	ARMInit(&cpu);
	gba.logLevel = 0; // TODO: Settings
	gba.logHandler = GBARetroLog;
	gba.stream = &stream;
	gba.idleOptimization = IDLE_LOOP_REMOVE; // TODO: Settings
	rom = 0;

	GBAVideoSoftwareRendererCreate(&renderer);
	renderer.outputBuffer = malloc(256 * VIDEO_VERTICAL_PIXELS * BYTES_PER_PIXEL);
	renderer.outputBufferStride = 256;
	GBAVideoAssociateRenderer(&gba.video, &renderer.d);
}

void retro_deinit(void) {
	GBADestroy(&gba);
}

void retro_run(void) {
	int keys;
	gba.keySource = &keys;
	inputPollCallback();

	keys = 0;
	keys |= (!!inputCallback(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A)) << 0;
	keys |= (!!inputCallback(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B)) << 1;
	keys |= (!!inputCallback(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT)) << 2;
	keys |= (!!inputCallback(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START)) << 3;
	keys |= (!!inputCallback(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT)) << 4;
	keys |= (!!inputCallback(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT)) << 5;
	keys |= (!!inputCallback(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP)) << 6;
	keys |= (!!inputCallback(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN)) << 7;
	keys |= (!!inputCallback(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R)) << 8;
	keys |= (!!inputCallback(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L)) << 9;

	int frameCount = gba.video.frameCounter;
	while (gba.video.frameCounter == frameCount) {
		ARMRunLoop(&cpu);
	}
	videoCallback(renderer.outputBuffer, VIDEO_HORIZONTAL_PIXELS, VIDEO_VERTICAL_PIXELS, BYTES_PER_PIXEL * 256);
}

void retro_reset(void) {
	ARMReset(&cpu);
}

bool retro_load_game(const struct retro_game_info* game) {
	if (game->data) {
		rom = VFileFromMemory((void*) game->data, game->size); // TODO: readonly VFileMem
	} else {
		rom = VFileOpen(game->path, O_RDONLY);
	}
	if (!rom) {
		return false;
	}
	if (!GBAIsROM(rom)) {
		return false;
	}

	// TODO
	save = 0;
	savedata = 0;

	GBALoadROM(&gba, rom, save, game->path);
	ARMReset(&cpu);
	return true;
}

void retro_unload_game(void) {
	// TODO
}

size_t retro_serialize_size(void) {
	return sizeof(struct GBASerializedState);
}

bool retro_serialize(void* data, size_t size) {
	if (size != retro_serialize_size()) {
		return false;
	}
	GBASerialize(&gba, data);
	return true;
}

bool retro_unserialize(const void* data, size_t size) {
	if (size != retro_serialize_size()) {
		return false;
	}
	GBADeserialize(&gba, data);
	return true;
}

void retro_cheat_reset(void) {
	// TODO: Cheats
}

void retro_cheat_set(unsigned index, bool enabled, const char* code) {
	// TODO: Cheats
	UNUSED(index);
	UNUSED(enabled);
	UNUSED(code);
}

unsigned retro_get_region(void) {
	return RETRO_REGION_NTSC; // TODO: This isn't strictly true
}

void retro_set_controller_port_device(unsigned port, unsigned device) {
	UNUSED(port);
	UNUSED(device);
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info) {
	UNUSED(game_type);
	UNUSED(info);
	UNUSED(num_info);
	return false;
}

void* retro_get_memory_data(unsigned id) {
	// TODO
	UNUSED(id);
	return 0;
}

size_t retro_get_memory_size(unsigned id) {
	// TODO
	UNUSED(id);
	return 0;
}

void GBARetroLog(struct GBAThread* thread, enum GBALogLevel level, const char* format, va_list args) {
	UNUSED(thread);
	if (!logCallback) {
		return;
	}

	char message[128];
	vsnprintf(message, sizeof(message), format, args);

	enum retro_log_level retroLevel = RETRO_LOG_INFO;
	switch (level) {
	case GBA_LOG_ALL:
	case GBA_LOG_ERROR:
	case GBA_LOG_FATAL:
		retroLevel = RETRO_LOG_ERROR;
		break;
	case GBA_LOG_WARN:
		retroLevel = RETRO_LOG_WARN;
		break;
	case GBA_LOG_INFO:
	case GBA_LOG_GAME_ERROR:
	case GBA_LOG_SWI:
		retroLevel = RETRO_LOG_INFO;
		break;
	case GBA_LOG_DEBUG:
	case GBA_LOG_STUB:
		retroLevel = RETRO_LOG_DEBUG;
		break;
	}
	logCallback(retroLevel, "%s\n", message);
}

static void _postAudioFrame(struct GBAAVStream* stream, int16_t left, int16_t right) {
	UNUSED(stream);
	audioCallback(left, right);
}

static void _postVideoFrame(struct GBAAVStream* stream, struct GBAVideoRenderer* renderer) {
	UNUSED(stream);
	void* pixels;
	unsigned stride;
	renderer->getPixels(renderer, &stride, &pixels);
	videoCallback(pixels, VIDEO_HORIZONTAL_PIXELS, VIDEO_VERTICAL_PIXELS, BYTES_PER_PIXEL * stride);
}