#include <libk/string.h>
#include <libk/stdio.h>
#include <libk/ctype.h>
#define _STRING_H_
#define _STDIO_H_
#include <vitasdk.h>
#include <taihen.h>
#include <taipool.h>
#include "renderer.h"

/*
 * This plugin is compiled with a slightly modified version of
 * libtremor-lowmem branch. toupper has been replace with
 * strncasecmp and ov_open / ov_test had been made dummy.
 * It also has dummy errno feature.
 */

// strncasecmp implementation for libtremor
int strncasecmp(const char* s1,const char* s2, size_t n){
	int c=0;
	while(c < n){
		if(tolower(s1[c]) != tolower(s2[c]))
			return !0;
		c++;
	}
	return 0;
}

#include <tremor/ogg.h>
#include <tremor/ivorbiscodec.h>
#include <tremor/ivorbisfile.h>

#define DEBUG_FILE "ux0:data/audio.ogg"

#define NSAMPLES  2048 // Samples per read
#define BUFSIZE   8192 // Audiobuffer size (NSAMPLES<<2)
#define HOOKS_NUM 1    // Hooked functions num

static SceUID Audio_Mutex;
static int volume = 32760, vol_bar = 10, menu_idx = 0;
static uint8_t current_hook;
static char filename[64];
static SceUID hooks[HOOKS_NUM];
static tai_hook_ref_t refs[HOOKS_NUM];
static uint8_t menu_mode = 0, loop = 1;
static char vol_string[64];
static SceCtrlData pad, oldpad;
static uint32_t name_timer = 0;

// Supported codecs
enum {
	OGG_VORBIS,
	WAV_PCM16,
	AIFF_PCM16
};

// Opened audio file structure
typedef struct audioFile{
	uint8_t  isPlaying;
	uint8_t  codec;
	uint16_t audiotype;
	uint32_t samplerate;
}audioFile;

// Endianess swap functions
uint32_t Endian_UInt32_Conversion(uint32_t value){
   return ((value >> 24) & 0x000000FF) | ((value >> 8) & 0x0000FF00) | ((value << 8) & 0x00FF0000) | ((value << 24) & 0xFF000000);
}
uint16_t Endian_UInt16_Conversion(uint16_t value){
   return (uint16_t)(((value >> 8) & 0x00FF) | ((value << 8) & 0xFF00));
}

// Simplified generic hooking function
void hookFunction(uint32_t nid, const void *func){
	hooks[current_hook] = taiHookFunctionImport(&refs[current_hook],TAI_MAIN_MODULE,TAI_ANY_LIBRARY,nid,func);
	current_hook++;
}

// Custom functions used as libogg callbacks
long fpos = 0;
size_t sceIoRead_cb(void* ptr, size_t size, size_t nmemb, void* datasource){
	size_t rbytes = sceIoRead((SceUID)datasource, ptr, size * nmemb);
	fpos += rbytes;
	return rbytes / size;
}
long sceIoTell_cb(void* datasource){
	return fpos;
}
int sceIoClose_cb(void* datasource){
	fpos = 0;
	return sceIoClose((SceUID)datasource);
}
int sceIoLseek_cb(void* datasource, ogg_int64_t offset, int whence){
	int res = sceIoLseek((SceUID)datasource, offset, whence);
	fpos = res;
	return res;
}

audioFile song;

// Audio thread
int audio_thread(SceSize args, void *argp){

	// Opening an audio port
	int ch = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, NSAMPLES, 48000, SCE_AUDIO_OUT_MODE_STEREO);
	
	uint32_t magic, jump, pos, chunk;
	uint16_t encoding;
	uint8_t audiobuf[BUFSIZE];
	OggVorbis_File vf;
	
	// Main loop
	for (;;){
		
		// Waiting for audio request
		sceKernelWaitSema(Audio_Mutex, 1, NULL);
		
		// Opening audio file
		SceUID fd = sceIoOpen(filename, SCE_O_RDONLY, 0777);
		
		// Parsing magic
		sceIoRead(fd, &magic, sizeof(uint32_t));
		switch (magic){
			case 0x46464952: // WAV
				
				// Checking encoding
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
				sceIoLseek(fd, pos+4, SCE_SEEK_SET);
				
				break;
			case 0x4D524F46: // AIF/AIFF
				song.codec = AIFF_PCM16;
				
				// Positioning on chunks section start
				pos = 12;
				chunk = 0x00000000;
				sceIoLseek(fd, pos, SCE_SEEK_SET);
				while (chunk != 0x444E5353){ // data chunk
					
					if (chunk == 0x4D4D4F43){ // COMM chunk
					
						// Extracting audiotype and samplerate
						sceIoLseek(fd, pos+8, SCE_SEEK_SET);
						sceIoRead(fd, &song.audiotype, 2);
						song.audiotype = song.audiotype>>8;
						sceIoLseek(fd, pos+18, SCE_SEEK_SET);
						sceIoRead(fd, &song.samplerate, 4);
						song.samplerate = Endian_UInt16_Conversion(song.samplerate);
						
					}
					
					pos += 4;
					sceIoLseek(fd, pos, SCE_SEEK_SET);
					sceIoRead(fd, &jump, 4);
					pos += (4+Endian_UInt32_Conversion(jump));
					sceIoLseek(fd, pos, SCE_SEEK_SET);
					sceIoRead(fd, &chunk, 4);
				}
				pos += 4;
				
				// Positioning on data chunk start
				sceIoLseek(fd, pos+4, SCE_SEEK_SET);
				
				break;
			case 0x5367674F: // OGG
				song.codec = OGG_VORBIS;
				
				// Setting up custom callbacks in order to use sceIo with libogg
				ov_callbacks cb;
				cb.read_func = sceIoRead_cb;
				cb.seek_func = sceIoLseek_cb;
				cb.close_func = sceIoClose_cb;
				cb.tell_func = sceIoTell_cb;
				
				// Opening ogg file with libogg
				sceIoLseek(fd, 0, SEEK_SET);
				if (ov_open_callbacks((void*)fd, &vf, NULL, 0, cb) != 0){
					ov_clear(&vf);
					fd = 0;
				}else{
					vorbis_info* ogg_info = ov_info(&vf,-1);
					song.samplerate = ogg_info->rate;
					song.audiotype = ogg_info->channels;
				}
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
		name_timer = 200;
		song.isPlaying = 1;
		int i=0, z=0, rbytes, sector;
		while (song.isPlaying){
			
			// Reading next samples
			switch (song.codec){
				case WAV_PCM16:
					rbytes = sceIoRead(fd, audiobuf, BUFSIZE);
					if (rbytes < BUFSIZE){
						memset(&audiobuf[rbytes], 0, BUFSIZE - rbytes);
						song.isPlaying = 0;
					}
					break;
				case AIFF_PCM16:
					rbytes = sceIoRead(fd, audiobuf, BUFSIZE);
					for(i=0;i<rbytes;i+=2){
						uint16_t* sample = (uint16_t*)(&audiobuf[i]);
						sample[0] = Endian_UInt16_Conversion(sample[0]);
					}
					if (rbytes < BUFSIZE){
						memset(&audiobuf[rbytes], 0, BUFSIZE - rbytes);
						song.isPlaying = 0;
					}
					break;
				case OGG_VORBIS:
					while (i < BUFSIZE){
						rbytes = ov_read(&vf, &audiobuf[i], BUFSIZE - i, &sector);
						if (rbytes == 0){
							memset(&audiobuf[i], 0, BUFSIZE - i);
							song.isPlaying = 0;
							break;
						}else if (rbytes > 0){
							i += rbytes;
						}
					}
					i = 0;
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
		switch (song.codec){
			case OGG_VORBIS:	
				ov_clear(&vf);
				ov_clear(&vf); // Seems calling it once doesn't properly clean libtremor decoder
				break;
			default:
				sceIoClose(fd);
				break;
		}
		if (loop) sceKernelSignalSema(Audio_Mutex, 1);
		
	}
	
}

int sceDisplaySetFrameBuf_patched(const SceDisplayFrameBuf *pParam, int sync) {
	sceCtrlPeekBufferPositive(0, &pad, 1);
	updateFramebuf(pParam);
	if (menu_mode){
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
		drawString(5, 160, "Close config menu");
		if ((pad.buttons & SCE_CTRL_UP) && (!(oldpad.buttons & SCE_CTRL_UP))){
			menu_idx--;
			if (menu_idx < 0) menu_idx++;
		}else if ((pad.buttons & SCE_CTRL_DOWN) && (!(oldpad.buttons & SCE_CTRL_DOWN))){
			menu_idx++;
			if (menu_idx > 3) menu_idx--;
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
		}else if((pad.buttons & SCE_CTRL_CROSS) && (!(oldpad.buttons & SCE_CTRL_CROSS))){
			if (menu_idx == 1) loop = (loop + 1) % 2;
			else if (menu_idx == 2){
				sprintf(filename, DEBUG_FILE);
				sceKernelSignalSema(Audio_Mutex, 1);
			}if (menu_idx >= 2){
				menu_mode = 0;
				menu_idx = 0;
			}
		}
	}else if ((pad.buttons & SCE_CTRL_SELECT) && (pad.buttons & SCE_CTRL_LTRIGGER) && (pad.buttons & SCE_CTRL_SQUARE)){
		menu_mode = 1;
	}
	oldpad.buttons = pad.buttons;
	
	if (name_timer > 0){
		setTextColor(0x00FFFFFF);
		drawStringF(5, 5, "Now playing %s", filename);
		#ifndef NO_DEBUG
		drawStringF(5, 25, "Channels: %hu, Samplerate: %lu", song.audiotype, song.samplerate);
		#endif
		name_timer--;
	}
	
	#ifndef NO_DEBUG
	setTextColor(0x00FFFFFF);
	drawStringF(5, 400, "taipool free space: %lu KBs", (taipool_get_free_space()>>10));
	#endif
	
	return TAI_CONTINUE(int, refs[0], pParam, sync);
}

void _start() __attribute__ ((weak, alias ("module_start")));
int module_start(SceSize argc, const void *args) {
	
	sprintf(vol_string, "Volume: [**********]");
	
	// Starting mutexes for audio playback managing
	Audio_Mutex = sceKernelCreateSema("Audio Mutex", 0, 0, 1, NULL);
	
	// Starting a secondary thread
	SceUID thd_id = sceKernelCreateThread("Musik_thread", audio_thread, 0x40, 0x400000, 0, 0, NULL);
	if (thd_id >= 0) sceKernelStartThread(thd_id, 0, NULL);
	
	// Initializing taipool mempool for dynamic memory managing
	taipool_init(0x400000);
	
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