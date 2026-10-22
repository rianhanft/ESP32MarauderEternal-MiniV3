#include "Switches.h"
#include "configs.h"
#ifdef HAS_TOUCH
#include "Display.h"
extern Display display_obj;
#endif

// Touch-zone constructor: the button is "pressed" while the touchscreen is
// touched within the given rectangle. Used to drive button-based Eternal
// modules on touch-only boards (Marauder V8) with no code changes in them.
Switches::Switches(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t hold_lim) {
	this->pin = -1;
	this->pullup = false;
	this->pressed = false;
	this->hold_lim = hold_lim;
	this->cur_hold = 0;
	this->isheld = false;
	this->is_touch = true;
	this->tz_x0 = x0; this->tz_y0 = y0; this->tz_x1 = x1; this->tz_y1 = y1;
	return;
}

Switches::Switches() {
	this->pin = 0;
	this->pin = false;
	this->pressed = false;
	this->hold_lim = 2000;
	this->cur_hold = 0;
	this->isheld = false;
	
	pinMode(this->pin, INPUT);
	
	return;
}

Switches::Switches(int pin, uint32_t hold_lim, bool pullup) {
	this->pin = pin;
	this->pullup = pullup;
	this->pressed = false;
	this->hold_lim = hold_lim;
	this->cur_hold = 0;
	this->isheld = false;
	
  if (pullup)
  	pinMode(this->pin, INPUT_PULLUP);
  else
    pinMode(this->pin, INPUT_PULLDOWN);
	
	return;
}

int Switches::getPin() {
	return this->pin;
}

bool Switches::getPullup() {
	return this->pullup;
}

bool Switches::isHeld() {
	return this->isheld;
}

bool Switches::getButtonState() {
#ifdef HAS_TOUCH
	if (this->is_touch) {
		uint16_t tx = 0, ty = 0;
		if (!display_obj.updateTouch(&tx, &ty))
			return false;
		// Zone bounds are stored per-mille (0..1000) of the live screen size so
		// they track the active rotation; scale to pixels at read time.
		const int32_t w = display_obj.tft.width();
		const int32_t h = display_obj.tft.height();
		const int32_t x0 = this->tz_x0 * w / 1000, x1 = this->tz_x1 * w / 1000;
		const int32_t y0 = this->tz_y0 * h / 1000, y1 = this->tz_y1 * h / 1000;
		return ((int32_t)tx >= x0 && (int32_t)tx <= x1 &&
		        (int32_t)ty >= y0 && (int32_t)ty <= y1);
	}
#endif
	int buttonState = digitalRead(this->pin);

	if ((this->pullup) && (buttonState == LOW))
		return true;
	else if ((!this->pullup) && (buttonState == HIGH))
		return true;
	else
		return false;
}

bool Switches::isPressedNow() {
	return this->getButtonState();
}

bool Switches::justPressed() {
	bool btn_state = this->getButtonState();
	
	// Button was JUST pressed
	if (btn_state && !this->pressed) {
		this->hold_init = millis();
		this->pressed = btn_state;
		return true;
	}
	else if (btn_state) { // Button is STILL pressed
		// Check if button is held
		//Serial.println("cur_hold: " + (String)this->cur_hold);
		if ((millis() - this->hold_init) < this->hold_lim) {
			this->isheld = false;
		}
		else {
			this->isheld = true;
		}
		
		this->pressed = btn_state;
		return false;
	}
	else { // Button is not pressed
		this->pressed = btn_state;
		this->isheld = false;
		return false;
	}
}

bool Switches::justReleased() {
	bool btn_state = this->getButtonState();
	
	// Button was JUST released
	if (!btn_state && this->pressed) {
		this->isheld = false;
		this->pressed = btn_state;
		return true;
	}
	else { // Button is STILL released
		this->pressed = btn_state;
		return false;
	}
	
}
