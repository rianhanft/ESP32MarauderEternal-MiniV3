#ifndef Switches_h
#define Switches_h

#include <Arduino.h>

class Switches {
	private:
		int pin;
		uint32_t hold_lim;
		uint32_t cur_hold;

		uint32_t hold_init = millis();

		bool isheld;

		bool pullup;

		bool pressed;

		// Touch-zone mode: on touch-only boards (e.g. Marauder V8) a Switches
		// instance can be backed by a rectangular region of the touchscreen
		// instead of a GPIO, so button-driven Eternal modules work unchanged.
		bool is_touch = false;
		int16_t tz_x0 = 0, tz_y0 = 0, tz_x1 = 0, tz_y1 = 0;

		bool getButtonState();

	public:
		Switches();

		Switches(int pin, uint32_t hold_lim, bool pullup);

		// Construct a touch-zone button spanning the rectangle (x0,y0)-(x1,y1).
		Switches(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t hold_lim = 1000);

		int getPin();
		bool getPullup();
		
		bool justPressed();
		bool justReleased();
		bool isHeld();
		bool isPressedNow();

		// Alias used by the ported Eternal GameInput/game modules, which were
		// written against Eternal's Switches API. Equivalent to isPressedNow().
		bool isDown() { return isPressedNow(); }
};

#endif
