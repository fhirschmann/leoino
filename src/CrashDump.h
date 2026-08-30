#pragma once

#include <Arduino.h>

struct CrashDumpStatus {
	bool partitionPresent = false;
	bool available = false;
	bool valid = false;
	size_t size = 0;
	char panicReason[160] = {};
};

void CrashDump_Init(void);
const CrashDumpStatus &CrashDump_GetStatus(void);
size_t CrashDump_Read(size_t offset, uint8_t *buffer, size_t length);
bool CrashDump_Erase(void);
