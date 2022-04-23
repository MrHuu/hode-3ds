/*
 * Heart of Darkness engine rewrite
 * Copyright (C) 2009-2011 Gregory Montoir (cyx@users.sourceforge.net)
 */
#include <3ds.h>

#include <SDL.h>
#include <stdarg.h>
#include <math.h>
#include "scaler.h"
#include "system.h"
#include "util.h"

const Scaler scaler_nearest = {
	"nearest",
	1, 1
};
static int _scalerMultiplier = 1;
static const Scaler *_scaler = &scaler_nearest;
static ScaleProc _scalerProc;

static const Scaler *_scalers[] = {
	&scaler_nearest,
	0
};

struct KeyMapping {
	int keyCode;
	int mask;
};

struct System_CTR : System {
	enum {
		kJoystickCommitValue = 3200,
		kKeyMappingsSize = 20,
		kAudioHz = 22050
	};

	uint8_t *_offscreenLut;

	SDL_Surface *screen;
	SDL_Surface *_texture;
	SDL_Surface *_backgroundTexture; // YUV (PSX)
	SDL_Surface *_widescreenTexture;
	int _texW, _texH, _texScale;

	uint32_t _pal[256];
	int _screenW, _screenH;
	int _shakeDx, _shakeDy;

	KeyMapping _keyMappings[kKeyMappingsSize];
	int _keyMappingsCount;
	AudioCallback _audioCb;
	uint8_t _gammaLut[256];

	SDL_Joystick *_joystick;

	System_CTR();
	virtual ~System_CTR() {}
	virtual void init(const char *title, int w, int h, bool fullscreen, bool widescreen, bool yuv);
	virtual void destroy();
	virtual void setScaler(const char *name, int multiplier);
	virtual void setGamma(float gamma);
	virtual void setPalette(const uint8_t *pal, int n, int depth);
	virtual void clearPalette();
	virtual void copyRect(int x, int y, int w, int h, const uint8_t *buf, int pitch);
	virtual void copyYuv(int w, int h, const uint8_t *y, int ypitch, const uint8_t *u, int upitch, const uint8_t *v, int vpitch);
	virtual void fillRect(int x, int y, int w, int h, uint8_t color);
	virtual void copyRectWidescreen(int w, int h, const uint8_t *buf, const uint8_t *pal);
	virtual void shakeScreen(int dx, int dy);
	virtual void updateScreen(bool drawWidescreen);
	virtual void processEvents();
	virtual void sleep(int duration);
	virtual uint32_t getTimeStamp();

	virtual void startAudio(AudioCallback callback);
	virtual void stopAudio();
	virtual void lockAudio();
	virtual void unlockAudio();
	virtual AudioCallback setAudioCallback(AudioCallback callback);

	void addKeyMapping(int key, uint8_t mask);
	void setupDefaultKeyMappings();
	void updateKeys(PlayerInput *inp);
	void prepareScaledGfx(const char *caption, bool fullscreen, bool widescreen, bool yuv);
};

static System_CTR system_ctr;
System *const g_system = &system_ctr;

void System_printLog(FILE *fp, const char *s) {
	printf("WARNING: %s\n", s);
}

void System_fatalError(const char *s) {
	errorConf error;

	if (!gspHasGpuRight())
		gfxInitDefault();

	errorInit(&error, ERROR_TEXT, CFG_LANGUAGE_EN);
	errorText(&error, s);
	errorDisp(&error);
#ifdef CTR_ROMFS
	romfsExit();
#endif
	gfxExit();
	exit(-1);
}

bool System_hasCommandLine() {
	return true;
}

System_CTR::System_CTR() :
	_offscreenLut(0),
	_texture(0), _backgroundTexture(0), _widescreenTexture(0),
	_joystick(0) {
	for (int i = 0; i < 256; ++i) {
		_gammaLut[i] = i;
	}
}

void System_CTR::init(const char *title, int w, int h, bool fullscreen, bool widescreen, bool yuv) {
	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK);
	SDL_ShowCursor(SDL_DISABLE);

	memset(&inp, 0, sizeof(inp));
	memset(&pad, 0, sizeof(pad));
	_screenW = w;
	_screenH = h;
	_shakeDx = _shakeDy = 0;
	memset(_pal, 0, sizeof(_pal));
	const int offscreenSize = w * h;
	_offscreenLut = (uint8_t *)malloc(offscreenSize);
	if (!_offscreenLut) {
		error("System_CTR::init() Unable to allocate offscreen buffer");
	}
	memset(_offscreenLut, 0, offscreenSize);
	prepareScaledGfx(title, fullscreen, widescreen, yuv);

	_joystick = 0;

	const int count = SDL_NumJoysticks();
	if (count > 0) {
		for (int i = 0; i < count; ++i) {
			_joystick = SDL_JoystickOpen(i);
		}
	}
}

void System_CTR::destroy() {
	free(_offscreenLut);
	_offscreenLut = 0;

	SDL_Quit();
}

template<bool vertical>
static void blur(int radius, const uint32_t *src, int srcPitch, int w, int h, const SDL_PixelFormat *fmt, uint32_t *dst, int dstPitch) {

	const int count = 2 * radius + 1;

	const uint32_t rmask  = fmt->Rmask;
	const uint32_t rshift = fmt->Rshift;
	const uint32_t gmask  = fmt->Gmask;
	const uint32_t gshift = fmt->Gshift;
	const uint32_t bmask  = fmt->Bmask;
	const uint32_t bshift = fmt->Bshift;

	for (int j = 0; j < (vertical ? w : h); ++j) {

		uint32_t r = 0;
		uint32_t g = 0;
		uint32_t b = 0;

		uint32_t color;

		for (int i = -radius; i <= radius; ++i) {
			if (vertical) {
				color = src[MAX(i, 0) * srcPitch];
			} else {
				color = src[MAX(i, 0)];
			}
			r += (color & rmask) >> rshift;
			g += (color & gmask) >> gshift;
			b += (color & bmask) >> bshift;
		}
		color = ((r / count) << rshift) | ((g / count) << gshift) | ((b / count) << bshift);
		dst[0] = color;

		for (int i = 1; i < (vertical ? h : w); ++i) {
			if (vertical) {
				color = src[MIN(i + radius, h - 1) * srcPitch];
			} else {
				color = src[MIN(i + radius, w - 1)];
			}
			r += (color & rmask) >> rshift;
			g += (color & gmask) >> gshift;
			b += (color & bmask) >> bshift;

			if (vertical) {
				color = src[MAX(i - radius - 1, 0) * srcPitch];
			} else {
				color = src[MAX(i - radius - 1, 0)];
			}
			r -= (color & rmask) >> rshift;
			g -= (color & gmask) >> gshift;
			b -= (color & bmask) >> bshift;

			color = ((r / count) << rshift) | ((g / count) << gshift) | ((b / count) << bshift);
			if (vertical) {
				dst[i * srcPitch] = color;
			} else {
				dst[i] = color;
			}
		}

		src += vertical ? 1 : srcPitch;
		dst += vertical ? 1 : dstPitch;
	}
}

void System_CTR::copyRectWidescreen(int w, int h, const uint8_t *buf, const uint8_t *pal) {
	if (!_widescreenTexture) {
		return;
	}
	if (_backgroundTexture) {
		return;
	}

	assert(w == _screenW && h == _screenH);

	int pitch = 0;

	uint32_t *src = (uint32_t *)malloc(w * h * sizeof(uint32_t));
	uint32_t *tmp = (uint32_t *)malloc(w * h * sizeof(uint32_t));
	uint32_t *dst = (uint32_t *)_widescreenTexture->pixels;

	if (src && tmp) {
		for (int i = 0; i < w * h; ++i) {
			const uint8_t color = buf[i];
			src[i] = SDL_MapRGB(screen->format, _gammaLut[pal[color * 3]], _gammaLut[pal[color * 3 + 1]], _gammaLut[pal[color * 3 + 2]]);
		}
		static const int radius = 8;
		// horizontal pass
		blur<false>(radius, src, w, w, h, screen->format, tmp, w);
		// vertical pass
		blur<true>(radius, tmp, w, w, h, screen->format, dst, pitch / sizeof(uint32_t));
	}

	free(src);
	free(tmp);
}

void System_CTR::setScaler(const char *name, int multiplier) {
	if (multiplier > 0) {
		_scalerMultiplier = multiplier;
	}
	if (name) {
		const Scaler *scaler = 0;
		for (int i = 0; _scalers[i]; ++i) {
			if (strcmp(name, _scalers[i]->name) == 0) {
				scaler = _scalers[i];
				break;
			}
		}
		if (!scaler) {
			warning("Unknown scaler '%s', using default '%s'", name, _scaler->name);
		} else {
			_scaler = scaler;
		}
	}
}

void System_CTR::setGamma(float gamma) {
	for (int i = 0; i < 256; ++i) {
		_gammaLut[i] = (uint8_t)round(pow(i / 255., 1. / gamma) * 255);
	}
}

void System_CTR::setPalette(const uint8_t *pal, int n, int depth) {
	assert(n <= 256);
	assert(depth <= 8);
	const int shift = 8 - depth;
	for (int i = 0; i < n; ++i) {
		int r = pal[i * 3 + 0];
		int g = pal[i * 3 + 1];
		int b = pal[i * 3 + 2];
		if (shift != 0) {
			r = (r << shift) | (r >> (depth - shift));
			g = (g << shift) | (g >> (depth - shift));
			b = (b << shift) | (b >> (depth - shift));
		}
		r = _gammaLut[r];
		g = _gammaLut[g];
		b = _gammaLut[b];

		_pal[i] = SDL_MapRGB(screen->format, r, g, b);

	}
	if (_backgroundTexture) {
		_pal[0] = 0;
	}
	if (_scaler->palette) {
		_scaler->palette(_pal);
	}
}

void System_CTR::clearPalette() {
	memset(_pal, 0, sizeof(_pal));
}

void System_CTR::copyRect(int x, int y, int w, int h, const uint8_t *buf, int pitch) {
	assert(x >= 0 && x + w <= _screenW && y >= 0 && y + h <= _screenH);
	if (w == pitch && w == _screenW) {
		memcpy(_offscreenLut + y * _screenW + x, buf, w * h);
	} else {
		for (int i = 0; i < h; ++i) {
			memcpy(_offscreenLut + y * _screenW + x, buf, w);
			buf += pitch;
			++y;
		}
	}
}

void System_CTR::copyYuv(int w, int h, const uint8_t *y, int ypitch, const uint8_t *u, int upitch, const uint8_t *v, int vpitch) {}

void System_CTR::fillRect(int x, int y, int w, int h, uint8_t color) {
	assert(x >= 0 && x + w <= _screenW && y >= 0 && y + h <= _screenH);
	if (w == _screenW) {
		memset(_offscreenLut + y * _screenW + x, color, w * h);
	} else {
		for (int i = 0; i < h; ++i) {
			memset(_offscreenLut + y * _screenW + x, color, w);
			++y;
		}
	}
}

void System_CTR::shakeScreen(int dx, int dy) {
	_shakeDx = dx;
	_shakeDy = dy;
}

static void clearScreen(uint32_t *dst, int dstPitch, int x, int y, int w, int h, int scale) {
	uint32_t *p = dst + (y * dstPitch + x) * scale;
	for (int j = 0; j < h * scale; ++j) {
		memset(p, 0, w * sizeof(uint32_t) * scale);
		p += dstPitch;
	}
}

void System_CTR::updateScreen(bool drawWidescreen) {
	int texturePitch = 0;

	texturePitch = _texture->pitch;
	uint32_t *dst = (uint32_t *)_texture->pixels;

	int w = _screenW;
	int h = _screenH;
	const uint8_t *src = _offscreenLut;

	assert((texturePitch & 3) == 0);
	const int dstPitch = texturePitch / sizeof(uint32_t);
	const int srcPitch = _screenW;
	/*
	if (!_widescreenTexture) {
		if (_shakeDy > 0) {
			clearScreen(dst, dstPitch, 0, 0, w, _shakeDy, _texScale);
			h -= _shakeDy;
			dst += _shakeDy * dstPitch * _texScale;
		} else if (_shakeDy < 0) {
			h += _shakeDy;
			clearScreen(dst, dstPitch, 0, h, w, -_shakeDy, _texScale);
			src -= _shakeDy * srcPitch;
		}
		if (_shakeDx > 0) {
			clearScreen(dst, dstPitch, 0, 0, _shakeDx, h, _texScale);
			w -= _shakeDx;
			dst += _shakeDx * _texScale;
		} else if (_shakeDx < 0) {
			w += _shakeDx;
			clearScreen(dst, dstPitch, w, 0, -_shakeDx, h, _texScale);
			src -= _shakeDx;
		}
	}
	*/
	if (!_scalerProc) {
		for (int i = 0; i < w * h; ++i) {
			dst[i] = _pal[src[i]];
		}
	} else {
		_scalerProc(dst, dstPitch, src, srcPitch, w, h, _pal);
	}

	if (_widescreenTexture) {
		if (drawWidescreen) {
			SDL_BlitSurface(_widescreenTexture, NULL, _texture, NULL);
		}
		SDL_BlitSurface(_texture, NULL, screen, NULL);
	} else {
		SDL_BlitSurface(_texture, NULL, screen, NULL);
	}
	SDL_Flip(screen);

}

void System_CTR::processEvents() {
	SDL_Event ev;
	pad.prevMask = pad.mask;
	while (SDL_PollEvent(&ev)) {
		switch (ev.type) {
		case SDL_KEYUP:
			if (ev.key.keysym.sym == SDLK_s) {
				inp.screenshot = true;
			}
			break;
		case SDL_JOYHATMOTION:
			if (_joystick) {
				pad.mask &= ~(SYS_INP_UP | SYS_INP_DOWN | SYS_INP_LEFT | SYS_INP_RIGHT);
				if (ev.jhat.value & SDL_HAT_UP) {
					pad.mask |= SYS_INP_UP;
				}
				if (ev.jhat.value & SDL_HAT_DOWN) {
					pad.mask |= SYS_INP_DOWN;
				}
				if (ev.jhat.value & SDL_HAT_LEFT) {
					pad.mask |= SYS_INP_LEFT;
				}
				if (ev.jhat.value & SDL_HAT_RIGHT) {
					pad.mask |= SYS_INP_RIGHT;
				}
			}
			break;
		case SDL_JOYAXISMOTION:
			if (_joystick) {
				switch (ev.jaxis.axis) {
				case 0:
					pad.mask &= ~(SYS_INP_RIGHT | SYS_INP_LEFT);
					if (ev.jaxis.value > kJoystickCommitValue) {
						pad.mask |= SYS_INP_RIGHT;
					} else if (ev.jaxis.value < -kJoystickCommitValue) {
						pad.mask |= SYS_INP_LEFT;
					}
					break;
				case 1:
					pad.mask &= ~(SYS_INP_UP | SYS_INP_DOWN);
					if (ev.jaxis.value > kJoystickCommitValue) {
						pad.mask |= SYS_INP_DOWN;
					} else if (ev.jaxis.value < -kJoystickCommitValue) {
						pad.mask |= SYS_INP_UP;
					}
					break;
				}
			}
			break;
		case SDL_JOYBUTTONDOWN:
		case SDL_JOYBUTTONUP:
			if (_joystick) {
				const bool pressed = (ev.jbutton.state == SDL_PRESSED);
				switch (ev.jbutton.button) {
				case 4: //Y
					if (pressed) {
						pad.mask |= SYS_INP_RUN;
					} else {
						pad.mask &= ~SYS_INP_RUN;
					}
					break;
				case 2: //B
					if (pressed) {
						pad.mask |= SYS_INP_JUMP;
					} else {
						pad.mask &= ~SYS_INP_JUMP;
					}
					break;
				case 1: //A
					if (pressed) {
						pad.mask |= SYS_INP_SHOOT;
					} else {
						pad.mask &= ~SYS_INP_SHOOT;
					}
					break;
				case 3: //X
					if (pressed) {
						pad.mask |= SYS_INP_SHOOT | SYS_INP_RUN;
					} else {
						pad.mask &= ~(SYS_INP_SHOOT | SYS_INP_RUN);
					}
					break;
				case 7: //BACK
					if (pressed) {
						pad.mask |= SYS_INP_ESC;
					} else {
						pad.mask &= ~SYS_INP_ESC;
					}
					break;
				}
			}
			break;
		case SDL_QUIT:
			inp.quit = true;
			break;
		}
	}
	updateKeys(&inp);
}

void System_CTR::sleep(int duration) {
	SDL_Delay(duration);
}

uint32_t System_CTR::getTimeStamp() {
	return SDL_GetTicks();
}

static void mixAudioS16(void *param, uint8_t *buf, int len) {
	memset(buf, 0, len);
	system_ctr._audioCb.proc(system_ctr._audioCb.userdata, (int16_t *)buf, len / 2);
}

void System_CTR::startAudio(AudioCallback callback) {
	SDL_AudioSpec desired;
	memset(&desired, 0, sizeof(desired));
	desired.freq = kAudioHz;
	desired.format = AUDIO_S16;
	desired.channels = 2;
	desired.samples = 4096;
	desired.callback = mixAudioS16;
	desired.userdata = this;
	if (SDL_OpenAudio(&desired, 0) == 0) {
		_audioCb = callback;
		SDL_PauseAudio(0);
	} else {
		error("System_CTR::startAudio() Unable to open sound device");
	}
}

void System_CTR::stopAudio() {
	SDL_CloseAudio();
}

void System_CTR::lockAudio() {
	SDL_LockAudio();
}

void System_CTR::unlockAudio() {
	SDL_UnlockAudio();
}

AudioCallback System_CTR::setAudioCallback(AudioCallback callback) {
	SDL_LockAudio();
	AudioCallback cb = _audioCb;
	_audioCb = callback;
	SDL_UnlockAudio();
	return cb;
}

void System_CTR::addKeyMapping(int key, uint8_t mask) {
	if (_keyMappingsCount < kKeyMappingsSize) {
		for (int i = 0; i < _keyMappingsCount; ++i) {
			if (_keyMappings[i].keyCode == key) {
				_keyMappings[i].mask = mask;
				return;
			}
		}
		if (_keyMappingsCount < kKeyMappingsSize) {
			_keyMappings[_keyMappingsCount].keyCode = key;
			_keyMappings[_keyMappingsCount].mask = mask;
			++_keyMappingsCount;
		}
	}
}

void System_CTR::setupDefaultKeyMappings() {
	_keyMappingsCount = 0;
	memset(_keyMappings, 0, sizeof(_keyMappings));
}

void System_CTR::updateKeys(PlayerInput *inp) {
	inp->prevMask = inp->mask;
	inp->mask = 0;
	inp->mask |= pad.mask;
}

void System_CTR::prepareScaledGfx(const char *caption, bool fullscreen, bool widescreen, bool yuv) {
	widescreen = false;
	_texW = _screenW;
	_texH = _screenH;
	_texScale = 1;

	screen = SDL_SetVideoMode(_texW,_texH,24,SDL_FULLSCREEN|SDL_HWPALETTE|SDL_CONSOLEBOTTOM);
	_texture = SDL_CreateRGBSurface(0,_texW,_texH,32,0,0,0,0);

	if (widescreen) {
		_widescreenTexture = SDL_CreateRGBSurface(0,_screenW,_screenH,32,0,0,0,0);
	} else {
		_widescreenTexture = 0;
	}
}
