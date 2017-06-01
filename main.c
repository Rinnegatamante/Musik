#include <vitasdk.h>
#include <taihen.h>
#include <stdio.h>
#include "renderer.h"

#define DEBUG_FILE "ux0:data/audio.wav"

#define NSAMPLES  2048 // Samples per read
#define BUFSIZE   8192 // Audiobuffer size (NSAMPLES<<2)
#define HOOKS_NUM 1    // Hooked functions num

static SceUID Audio_Mutex;
static int volume = 32760, vol_bar = 10, menu_idx = 0;
static uint8_t audiobuf[BUFSIZE], current_hook;
static char filename[128];
static SceUID hooks[HOOKS_NUM];
static tai_hook_ref_t refs[HOOKS_NUM];
static uint8_t menu_mode = 0, loop = 1;
static char vol_string[64];
static SceCtrlData pad, oldpad;

// Supported codecs
enum {
	WAV_PCM16
};

// Opened audio file structure
typedef struct audioFile{
	uint8_t isPlaying;
	uint8_t codec;
	uint32_t size;
	uint16_t audiotype;
	uint32_t samplerate; // TODO: Check if PORT_TYPE_BGM can be used to avoid software resampling
}audioFile;

// Simplified generic hooking function
void hookFunction(uint32_t nid, const void *func){
	hooks[current_hook] = taiHookFunctionImport(&refs[current_hook],TAI_MAIN_MODULE,TAI_ANY_LIBRARY,nid,func);
	current_hook++;
}

// Audio thread
int audio_thread(SceSize args, void *argp){

	// Opening an audio port
	int ch = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, NSAMPLES, 48000, SCE_AUDIO_OUT_MODE_STEREO);
	
	audioFile song;
	uint32_t magic, jump, pos, chunk;
	uint16_t encoding;
	
	// Main loop
	for (;;){
		
		// Waiting for audio request
		sceKernelWaitSema(Audio_Mutex, 1, NULL);
		
		// Opening audio file
		SceUID fd = sceIoOpen(DEBUG_FILE, SCE_O_RDONLY, 0777);
		
		// Parsing magic
		sceIoRead(fd, &magic, sizeof(uint32_t));
		switch (magic){
			case 0x46464952: // WAV
				
				// Checking encoding
				song.size = sceIoLseek(fd, 0, SCE_SEEK_END); // We'll remove header size later
				sceIoLseek(fd, 20, SCE_SEEK_SET);
				sceIoRead(fd, &encoding, 2);
				if (encoding == 0x01) song.codec = WAV_PCM16; // PCM16
				else if (encoding == 0x11){ //ADPCM, currently unsupported
					sceIoClose(fd);
					fd = 0;
					break;
				}
				
				// Reading audiotype and samplerate
				sceIoRead(fd, &song.audiotype, 2);
				sceIoRead(fd, &song.samplerate, 4);
				
				// Skipping to data chunk
				pos = 16;
				chunk = 0x00000000;
				sceIoLseek(fd, pos, SCE_SEEK_SET);
				while (chunk != 0x61746164){
					sceIoRead(fd, &jump, 4);
					pos += (4+jump);
					sceIoLseek(fd, pos, SCE_SEEK_SET);
					sceIoRead(fd, &chunk, 4);
					pos += 4;
				}
				
				// Removing header size from song size and positioning on data chunk start
				song.size -= (pos+4);
				sceIoLseek(fd, pos+4, SCE_SEEK_SET);
				
				break;
			default: // Unknown
				sceIoClose(fd);
				fd = 0;
				break;
		}
		if (fd == 0) continue;
		
		// Setting volume, audiotype and samplerate	
		sceAudioOutSetConfig(ch, -1, song.samplerate, song.audiotype - 1);
		int vol_stereo[] = {volume, volume};
		sceAudioOutSetVolume(ch, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol_stereo);
		int cur_volume = volume;
		
		// Streaming loop
		song.isPlaying = 1;
		while (song.isPlaying){
			
			// Reading next samples
			switch (song.codec){
				case WAV_PCM16:
					int rbytes = sceIoRead(fd, audiobuf, BUFSIZE);
					if (rbytes < BUFSIZE){
						memset(&audiobuf[rbytes], 0, BUFSIZE - rbytes);
						song.isPlaying = 0;
					}
					break;
			}
			
			// Handling volume changes during playback
			if (cur_volume != volume){
				int new_vol[] = {volume, volume};
				sceAudioOutSetVolume(ch, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, new_vol);
				cur_volume = volume;
			}
			
			// Outputting read samples
			sceAudioOutOutput(ch, audiobuf);
			
		}
		sceIoClose(fd);
		if (loop) sceKernelSignalSema(Audio_Mutex, 1);
		
	}
	
}

int sceDisplaySetFrameBuf_patched(const SceDisplayFrameBuf *pParam, int sync) {
	sceCtrlPeekBufferPositive(0, &pad, 1);
	if (menu_mode){
		updateFramebuf(pParam);
		setTextColor(0x00FF00FF);
		drawString(5, 50, "Musik v.0.1 - CONFIG MENU");
		drawString(5, 80, "Detected songs: 1");
		(menu_idx == 0) ? setTextColor(0x0000FF00) : setTextColor(0x00FFFFFF);
		drawString(5, 100, vol_string);
		(menu_idx == 1) ? setTextColor(0x0000FF00) : setTextColor(0x00FFFFFF);
		drawStringF(5, 120, "Loop mode: %s", loop ? "On " : "Off");
		(menu_idx == 2) ? setTextColor(0x0000FF00) : setTextColor(0x00FFFFFF);
		drawString(5, 140, "Start audio playback");
		(menu_idx == 3) ? setTextColor(0x0000FF00) : setTextColor(0x00FFFFFF);
		drawString(5, 160, "Return to the game");
		if ((pad.buttons & SCE_CTRL_UP) && (!(oldpad.buttons & SCE_CTRL_UP))){
			menu_idx--;
			if (menu_idx < 0) menu_idx++;
		}else if ((pad.buttons & SCE_CTRL_DOWN) && (!(oldpad.buttons & SCE_CTRL_DOWN))){
			menu_idx++;
			if (menu_idx > 2) menu_idx--;
		}else if((menu_idx == 0) && (pad.buttons & SCE_CTRL_LEFT) && (!(oldpad.buttons & SCE_CTRL_LEFT))){
			vol_bar--;
			if (vol_bar < 0) vol_bar++;
			volume = vol_bar * 3276;
			vol_string[9 + vol_bar] = ' ';
		}else if((menu_idx == 0) && (pad.buttons & SCE_CTRL_RIGHT) && (!(oldpad.buttons & SCE_CTRL_RIGHT))){
			vol_bar++;
			if (vol_bar > 10) vol_bar--;
			volume = vol_bar * 3276;
			vol_string[8 + vol_bar] = '*';
		}else if(pad.buttons & SCE_CTRL_CROSS){
			if (menu_idx == 1) loop = (loop + 1) % 2;
			else if (menu_idx == 2) sceKernelSignalSema(Audio_Mutex, 1);
			if (menu_idx >= 2){
				menu_mode = 0;
				menu_idx = 0;
			}
		}
	}else if ((pad.buttons & SCE_CTRL_SELECT) && (pad.buttons & SCE_CTRL_LTRIGGER) && (pad.buttons & SCE_CTRL_SQUARE)) menu_mode = 1;
	oldpad.buttons = pad.buttons;
	return TAI_CONTINUE(int, refs[0], pParam, sync);
}

void _start() __attribute__ ((weak, alias ("module_start")));
int module_start(SceSize argc, const void *args) {
	
	sprintf(vol_string, "Volume: [**********]");
	
	// Starting mutexes for audio playback managing
	Audio_Mutex = sceKernelCreateSema("Audio Mutex", 0, 0, 1, NULL);
	
	// Starting a secondary thread
	SceUID thd_id = sceKernelCreateThread("Musik_thread", audio_thread, 0x40, 0x400000, 0, 0, NULL);
	sceKernelStartThread(thd_id, 0, NULL);
	
	// Hooking functions required for the config menu
	hookFunction(0x7A410B64, sceDisplaySetFrameBuf_patched);
	
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
	
	// Freeing hooks
	while (current_hook-- > 0){
		taiHookRelease(hooks[current_hook], refs[current_hook]);
	}
	
	return SCE_KERNEL_STOP_SUCCESS;	
}