#pragma once

#include <Arduino.h>

struct CrashDumpStatus {
	bool partitionPresent = false;
	bool available = false;
	bool valid = false;
	size_t size = 0;
	char panicReason[160] = {};
	bool summaryAvailable = false;
	bool firmwareMatches = false;
	char appElfSha256[65] = {};
	char task[16] = {};
	uint32_t exceptionPc = 0;
	uint32_t backtrace[16] = {};
	uint8_t backtraceDepth = 0;
	bool backtraceCorrupted = false;
};

void CrashDump_Init(void);
void CrashDump_GetRunningElfSha256(char (&output)[65]);
const CrashDumpStatus &CrashDump_GetStatus(void);
size_t CrashDump_Read(size_t offset, uint8_t *buffer, size_t length);
bool CrashDump_Erase(void);
