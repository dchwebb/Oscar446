#include <osc.h>

uint16_t Osc::CalcVertOffset(volatile const uint16_t& vPos) {
	return std::max(std::min(((((float)(vPos * vCalibScale + vCalibOffset) / (4 * 4096) - 0.5f) * (8.0f / voltScale)) + 0.5f) / laneCount * DRAWHEIGHT, (float)((DRAWHEIGHT - 1) / laneCount)), 1.0f);
}
