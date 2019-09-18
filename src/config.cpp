#include <config.h>

// called whenever a config setting is changed to schedule a save after waiting to see if any more changes are being made
void Config::ScheduleSave() {
	scheduleSave = true;
	saveBooked = SysTickVal;
}

// Write calibration settings to Flash memory
void Config::SaveConfig() {
	scheduleSave = false;

	uint32_t address = ADDR_FLASH_SECTOR_7;		// Store data in Sector 10 (second last sector in F405 - sector 11 appears to be write only) to allow maximum space for program code
	FLASH_Status flash_status = FLASH_COMPLETE;

	configValues cv;
	SetConfig(cv);

	__disable_irq();		// Disable Interrupts
	FLASH_Unlock();			// Unlock Flash memory for writing

	// Clear error flags in Status Register
	FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_OPERR |FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

	// Erase sector 7 (has to be erased before write - this sets all bits to 1 as write can only switch to 0)
	flash_status = FLASH_EraseSector(FLASH_Sector_7, VoltageRange_3);

	// If erase worked, program the Flash memory with the config settings byte by byte
	uint16_t cfgsize = sizeof(cv);
	if (flash_status == FLASH_COMPLETE) {
		for (unsigned int f = 0; f < sizeof(cv); f++) {
			char byte = *((char*)(&cv) + f);
			flash_status = FLASH_ProgramByte((uint32_t)address + f, byte);
		}
	}

	FLASH_Lock();			// Lock the Flash memory
	__enable_irq(); 		// Enable Interrupts
}

void Config::SetConfig(configValues &cv) {
	cv.gen_displayMode = displayMode;
	cv.gen_vCalibOffset = vCalibOffset;
	cv.gen_vCalibScale = vCalibScale;

	cv.osc_TriggerX = osc.TriggerX;
	cv.osc_TriggerY = osc.TriggerY;
	cv.osc_TriggerTest = (osc.TriggerTest == &adcA ? 1 : osc.TriggerTest == &adcB ? 2 : osc.TriggerTest == &adcC ? 3 : 0);
	cv.osc_EncModeL = osc.EncModeL;
	cv.osc_EncModeR = osc.EncModeR;
	cv.osc_SampleTimer = osc.SampleTimer;
	cv.osc_oscDisplay = osc.oscDisplay;
	cv.osc_multiLane = osc.multiLane;
	cv.osc_voltScale = osc.voltScale;
}


// Restore configuration settings from flash memory
void Config::RestoreConfig()
{
	// create temporary copy of settings from memory to check if they are valid
	configValues cv;
	memcpy(&cv, (uint32_t*)ADDR_FLASH_SECTOR_7, sizeof(cv));

	if (strcmp(cv.StartMarker, "CFG") == 0 && strcmp(cv.EndMarker, "END") == 0) {
		displayMode = cv.gen_displayMode;
		vCalibOffset = cv.gen_vCalibOffset;
		vCalibScale = cv.gen_vCalibScale;

		osc.TriggerX = cv.osc_TriggerX;
		osc.TriggerY = cv.osc_TriggerY;
		osc.TriggerTest = (cv.osc_TriggerTest == 1 ? &adcA : cv.osc_TriggerTest == 2 ? &adcB : cv.osc_TriggerTest == 3 ? &adcC : nullptr);
		osc.EncModeL = cv.osc_EncModeL;
		osc.EncModeR = cv.osc_EncModeR;
		osc.SampleTimer = cv.osc_SampleTimer;
		osc.oscDisplay = cv.osc_oscDisplay;
		osc.multiLane = cv.osc_multiLane;
		osc.voltScale = cv.osc_voltScale;
	}
}


