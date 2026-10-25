/* wheel state as the proxy sends it: Windows DirectInput DIJOYSTATE2 layout, 272 bytes */
#ifndef DIJOYSTATE_H
#define DIJOYSTATE_H

typedef struct {
	int lX, lY, lZ;
	int lRx, lRy, lRz;
	int rglSlider[2];
	unsigned rgdwPOV[4];
	char rgbButtons[128];
	int lVX, lVY, lVZ;
	int lVRx, lVRy, lVRz;
	int rglVSlider[2];
	int lAX, lAY, lAZ;
	int lARx, lARy, lARz;
	int rglASlider[2];
	int lFX, lFY, lFZ;
	int lFRx, lFRy, lFRz;
	int rglFSlider[2];
} DIJOYSTATE2_t;

#endif /* DIJOYSTATE_H */
