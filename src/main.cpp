#include "initialisation.h"
#include "config.h"
#include "ui.h"
#include "lcd.h"
#include "fft.h"
#include "midi.h"
#include "osc.h"


extern uint32_t SystemCoreClock;
volatile uint32_t SysTickVal = 0;

//	default calibration values for 15k and 100k resistors on input opamp scaling to a maximum of 8v (slightly less for negative signals)
#if defined(STM32F722xx) || defined(STM32F446xx)
	volatile int16_t vCalibOffset = -4240;
	volatile float vCalibScale = 1.41f;
#else
	volatile int16_t vCalibOffset = -4190;
	volatile float vCalibScale = 1.24f;
#endif
volatile uint16_t CalibZeroPos = 9985;

uint16_t DrawBuffer[2][(DRAWHEIGHT + 1) * DRAWBUFFERWIDTH];
volatile uint16_t OscBufferA[2][DRAWWIDTH], OscBufferB[2][DRAWWIDTH], OscBufferC[2][DRAWWIDTH];
volatile uint16_t adcA, adcB, adcC, oldAdc, capturePos = 0, drawPos = 0, bufferSamples = 0, calculatedOffsetYB = 0, calculatedOffsetYC = 0;
volatile bool capturing = false, drawing = false, encoderBtnL = false, encoderBtnR = false;
volatile uint8_t captureBufferNumber = 0, drawBufferNumber = 0;
volatile uint16_t ADC_array[ADC_BUFFER_LENGTH];

mode displayMode = Oscilloscope;

volatile uint32_t debugCount = 0, coverageTimer = 0, coverageTotal = 0, MIDIDebug = 0;


LCD lcd;
FFT fft;
UI ui;
MIDIHandler midi;
Osc osc;
Config cfg;




inline uint16_t CalcZeroSize() {					// returns ADC size that corresponds to 0v
	return (8192 - vCalibOffset) / vCalibScale;
}

inline float FreqFromPos(const uint16_t pos) {		// returns frequency of signal based on number of samples wide the signal is in the screen
	return (float)SystemCoreClock / (2.0f * pos * (TIM3->PSC + 1) * (TIM3->ARR + 1));
}

extern "C"
{
	#include "interrupts.h"
}


int main(void) {

	SystemInit();							// Activates floating point coprocessor and resets clock
	SystemClock_Config();					// Configure the clock and PLL - NB Currently done in SystemInit but will need updating for production board
	SystemCoreClockUpdate();				// Update SystemCoreClock (system clock frequency) derived from settings of oscillators, prescalers and PLL
	InitCoverageTimer();					// Timer 4 only activated/deactivated when CP_ON/CP_CAP macros are used
	InitDebounceTimer();					// Timer 5 used to count button press bounces
	InitLCDHardware();
	InitADC();
	InitEncoders();
	InitUART();
	InitSysTick();

	lcd.Init();								// Initialize ILI9341 LCD

	// check if resetting config by holding left encoder button while resetting
	if (L_BTN_GPIO->IDR & L_BTN_NO(GPIO_IDR_IDR_,))
		cfg.RestoreConfig();

	InitSampleAcquisition();
	ui.ResetMode();
	CalibZeroPos = CalcZeroSize();


	while (1) {

		ui.handleEncoders();

		if (cfg.scheduleSave && SysTickVal > cfg.saveBooked + 170000)			// this equates to around 60 seconds between saves
			cfg.SaveConfig();

		if (ui.menuMode) {

		} else if (displayMode == MIDI) {

			midi.ProcessMidi();

		} else if (displayMode == Fourier || displayMode == Waterfall) {

			fft.sampleCapture(false);									// checks if ready to start new capture

			if (fft.dataAvailable[0] || fft.dataAvailable[1]) {

				drawBufferNumber = fft.dataAvailable[0] ? 0 : 1;		// select correct draw buffer based on whether buffer 0 or 1 contains data
				if (displayMode == Fourier) {
					fft.runFFT(fft.FFTBuffer[drawBufferNumber]);
				}
				else
					fft.waterfall(fft.FFTBuffer[drawBufferNumber]);
			}

		} else if (displayMode == Oscilloscope) {

			if (!drawing && (capturing || osc.noTriggerDraw)) {								// check if we should start drawing
				drawBufferNumber = osc.noTriggerDraw ? !(bool)captureBufferNumber : captureBufferNumber;
				drawing = true;
				drawPos = 0;

				//	If in multi-lane mode get lane count from number of displayed channels and calculate vertical offset of channels B and C
				osc.laneCount = (osc.multiLane && osc.oscDisplay == 0b111 ? 3 : osc.multiLane && osc.oscDisplay > 2 && osc.oscDisplay != 4 ? 2 : 1);
				calculatedOffsetYB = (osc.laneCount > 1 && osc.oscDisplay & 0b001 ? DRAWHEIGHT / osc.laneCount : 0);
				calculatedOffsetYC = (osc.laneCount == 2 ? DRAWHEIGHT / 2 : osc.laneCount == 3 ? DRAWHEIGHT * 2 / 3 : 0);
				CP_ON
			}

			// Check if drawing and that the sample capture is at or ahead of the draw position
			if (drawing && (drawBufferNumber != captureBufferNumber || osc.capturedSamples[captureBufferNumber] >= drawPos || osc.noTriggerDraw)) {
				// Calculate offset between capture and drawing positions to display correct sample
				uint16_t calculatedOffsetX = (osc.drawOffset[drawBufferNumber] + drawPos) % DRAWWIDTH;


				uint16_t pixelA = osc.CalcVertOffset(OscBufferA[drawBufferNumber][calculatedOffsetX]);
				uint16_t pixelB = osc.CalcVertOffset(OscBufferB[drawBufferNumber][calculatedOffsetX]) + calculatedOffsetYB;
				uint16_t pixelC = osc.CalcVertOffset(OscBufferC[drawBufferNumber][calculatedOffsetX]) + calculatedOffsetYC;

				// Starting a new screen: Set previous pixel to current pixel and clear frequency calculations
				if (drawPos == 0) {
					osc.prevPixelA = pixelA;
					osc.prevPixelB = pixelB;
					osc.prevPixelC = pixelC;
					osc.freqBelowZero = false;
					osc.freqCrossZero = 0;
				}

				//	frequency calculation - detect upwards zero crossings
				if (!osc.freqBelowZero && OscBufferA[drawBufferNumber][calculatedOffsetX] < CalibZeroPos) {		// first time reading goes below zero
					osc.freqBelowZero = true;
				}
				if (osc.freqBelowZero && OscBufferA[drawBufferNumber][calculatedOffsetX] >= CalibZeroPos) {		// zero crossing
					//	second zero crossing - calculate frequency averaged over a number passes to smooth
					if (osc.freqCrossZero > 0 && drawPos - osc.freqCrossZero > 3) {
						if (osc.Freq > 0)
							osc.Freq = (3 * osc.Freq + FreqFromPos(drawPos - osc.freqCrossZero)) / 4;
						else
							osc.Freq = FreqFromPos(drawPos - osc.freqCrossZero);
					}
					osc.freqCrossZero = drawPos;
					osc.freqBelowZero = false;
				}

				// create draw buffer
				std::pair<uint16_t, uint16_t> AY = std::minmax(pixelA, (uint16_t)osc.prevPixelA);
				std::pair<uint16_t, uint16_t> BY = std::minmax(pixelB, (uint16_t)osc.prevPixelB);
				std::pair<uint16_t, uint16_t> CY = std::minmax(pixelC, (uint16_t)osc.prevPixelC);

				uint8_t vOffset = (drawPos < 27 || drawPos > 250) ? 11 : 0;		// offset draw area so as not to overwrite voltage and freq labels
				for (uint8_t h = 0; h <= DRAWHEIGHT - (drawPos < 27 ? 12 : 0); ++h) {

					if (h < vOffset) {
						// do not draw
					} else if (osc.oscDisplay & 1 && h >= AY.first && h <= AY.second) {
						DrawBuffer[osc.DrawBufferNumber][h - vOffset] = LCD_GREEN;
					} else if (osc.oscDisplay & 2 && h >= BY.first && h <= BY.second) {
						DrawBuffer[osc.DrawBufferNumber][h - vOffset] = LCD_LIGHTBLUE;
					} else if (osc.oscDisplay & 4 && h >= CY.first && h <= CY.second) {
						DrawBuffer[osc.DrawBufferNumber][h - vOffset] = LCD_ORANGE;
					} else if (drawPos % 4 == 0 && (h + (DRAWHEIGHT / (osc.laneCount * 2))) % (DRAWHEIGHT / (osc.laneCount)) == 0) {						// 0v center mark
						DrawBuffer[osc.DrawBufferNumber][h - vOffset] = LCD_GREY;
					} else {
						DrawBuffer[osc.DrawBufferNumber][h - vOffset] = LCD_BLACK;
					}
				}
				if (drawPos < 5) {
					for (int m = 0; m < (osc.laneCount == 1 ? osc.voltScale * 2 : (osc.laneCount * 2)); ++m) {
						DrawBuffer[osc.DrawBufferNumber][m * DRAWHEIGHT / (osc.laneCount == 1 ? osc.voltScale * 2 : (osc.laneCount * 2)) - 11] = LCD_GREY;
					}
				}

				lcd.PatternFill(drawPos, vOffset, drawPos, DRAWHEIGHT - (drawPos < 27 ? 12 : 0), DrawBuffer[osc.DrawBufferNumber]);
				osc.DrawBufferNumber = osc.DrawBufferNumber == 0 ? 1 : 0;

				// Store previous sample so next sample can be drawn as a line from old to new
				osc.prevPixelA = pixelA;
				osc.prevPixelB = pixelB;
				osc.prevPixelC = pixelC;

				drawPos ++;
				if (drawPos == DRAWWIDTH){
					drawing = false;
					osc.noTriggerDraw = false;
					CP_CAP
				}

				// Draw trigger as a yellow cross
				if (drawPos == osc.TriggerX + 4) {
					uint16_t vo = osc.CalcVertOffset(osc.TriggerY) + (osc.TriggerTest == &adcB ? calculatedOffsetYB : osc.TriggerTest == &adcC ? calculatedOffsetYC : 0);
					if (vo > 4 && vo < DRAWHEIGHT - 4) {
						lcd.DrawLine(osc.TriggerX, vo - 4, osc.TriggerX, vo + 4, LCD_YELLOW);
						lcd.DrawLine(std::max(osc.TriggerX - 4, 0), vo, osc.TriggerX + 4, vo, LCD_YELLOW);
					}
				}

				if (drawPos == 1) {
					// Write voltage
					lcd.DrawString(0, 1, " " + ui.intToString(osc.voltScale) + "v ", &lcd.Font_Small, LCD_GREY, LCD_BLACK);
					lcd.DrawString(0, DRAWHEIGHT - 10, "-" + ui.intToString(osc.voltScale) + "v ", &lcd.Font_Small, LCD_GREY, LCD_BLACK);

					// Write frequency
					if (osc.noTriggerDraw) {
						lcd.DrawString(250, 1, "No Trigger " , &lcd.Font_Small, LCD_WHITE, LCD_BLACK);
					} else {
						lcd.DrawString(250, 1, osc.Freq != 0 ? ui.floatToString(osc.Freq, false) + "Hz    " : "          ", &lcd.Font_Small, LCD_WHITE, LCD_BLACK);
						osc.Freq = 0;
					}

				}

			}

		} else if (displayMode == Circular) {


			if ((!osc.circDrawing[0] && osc.circDataAvailable[0] && (osc.circDrawPos[1] >= osc.zeroCrossings[1] || !osc.circDrawing[1])) ||
				(!osc.circDrawing[1] && osc.circDataAvailable[1] && (osc.circDrawPos[0] >= osc.zeroCrossings[0] || !osc.circDrawing[0]))) {								// check if we should start drawing

				drawBufferNumber = (!osc.circDrawing[0] && osc.circDataAvailable[0] && (osc.circDrawPos[1] == osc.zeroCrossings[1] || !osc.circDrawing[1])) ? 0 : 1;
				osc.circDrawing[drawBufferNumber] = true;
				osc.circDrawPos[drawBufferNumber] = 0;
				lcd.DrawString(140, DRAWHEIGHT + 8, ui.floatToString(osc.captureFreq[drawBufferNumber], true) + "Hz  ", &lcd.Font_Small, LCD_WHITE, LCD_BLACK);
				osc.laneCount = 1;
				CP_ON
			}

			// to have a continuous display drawing next sample as old sample is finishing
			for (drawBufferNumber = 0; drawBufferNumber < 2; drawBufferNumber++) {
				if (osc.circDrawing[drawBufferNumber]) {

					// each of these loops will draw a trail from one draw buffer from current position (pos) up to end of previous draw position: osc.circDrawPos[drawBufferNumber]
					for (int pos = std::max((int)osc.circDrawPos[drawBufferNumber] - CIRCLENGTH, 0); pos <= osc.circDrawPos[drawBufferNumber] && pos <= osc.zeroCrossings[drawBufferNumber]; pos++) {

						int b = (int)std::round(pos * LUTSIZE / osc.zeroCrossings[drawBufferNumber] + LUTSIZE / 4) % LUTSIZE;
						int x = fft.SineLUT[b] * 70 + 160;

						// draw a line getting fainter as it goes from current position backwards
						int pixelA = osc.CalcVertOffset(OscBufferA[drawBufferNumber][pos]);
						if (pos == std::max((int)osc.circDrawPos[drawBufferNumber] - CIRCLENGTH, 0)) {
							osc.prevPixelA = pixelA;
						}

						uint16_t frontColour = ui.DarkenColour(fft.channel == channelA ? LCD_GREEN : fft.channel == channelB ? LCD_LIGHTBLUE : LCD_ORANGE,
								(osc.circDrawPos[drawBufferNumber] - pos) / (fft.channel == channelA ? 3 : 4));
						uint16_t backColour = ui.DarkenColour(LCD_GREY, (osc.circDrawPos[drawBufferNumber] - pos) / 8);

						if (pos < (int)osc.circDrawPos[drawBufferNumber] - CIRCLENGTH + 2) {
							frontColour = LCD_BLACK;
							backColour = LCD_BLACK;
						}

						// Draw 'circle'
						lcd.DrawLine(x, pixelA, x, osc.prevPixelA, frontColour);

						// Draw normal osc
						uint16_t oscPos = pos * DRAWWIDTH / osc.zeroCrossings[drawBufferNumber];
						lcd.DrawLine(oscPos, pixelA, oscPos, osc.prevPixelA, backColour);

						osc.prevPixelA = pixelA;
					}

					osc.circDrawPos[drawBufferNumber] ++;
					if (osc.circDrawPos[drawBufferNumber] == osc.zeroCrossings[drawBufferNumber] + CIRCLENGTH){
						osc.circDrawing[drawBufferNumber] = false;
						osc.circDataAvailable[drawBufferNumber] = false;
						CP_CAP
					}
				}

			}
		}
	}
}
