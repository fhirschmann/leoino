#include "CrashDump.h"

#include "Log.h"

#include <algorithm>
#include <esp_app_desc.h>
#include <esp_core_dump.h>
#include <esp_partition.h>
#include <sdkconfig.h>

namespace {
CrashDumpStatus s_status;
const esp_partition_t *s_partition = nullptr;
size_t s_imageOffset = 0;

void refreshStatus(void) {
	s_status = {};
	s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
	s_status.partitionPresent = (s_partition != nullptr);
	if (!s_partition) {
		return;
	}

	size_t imageAddress = 0;
	size_t imageSize = 0;
	const esp_err_t imageResult = esp_core_dump_image_get(&imageAddress, &imageSize);
	if ((imageResult != ESP_OK) || (imageAddress < s_partition->address) || (imageSize > s_partition->size)) {
		return;
	}
	const size_t imageOffset = imageAddress - s_partition->address;
	if (imageOffset > s_partition->size - imageSize) {
		return;
	}

	s_status.available = true;
	s_status.size = imageSize;
	s_imageOffset = imageOffset;
	s_status.valid = (esp_core_dump_image_check() == ESP_OK);

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
	if (s_status.valid) {
		esp_core_dump_get_panic_reason(s_status.panicReason, sizeof(s_status.panicReason));
		esp_core_dump_summary_t summary = {};
		if (esp_core_dump_get_summary(&summary) == ESP_OK) {
			s_status.summaryAvailable = true;
			snprintf(s_status.task, sizeof(s_status.task), "%.*s", int(sizeof(summary.exc_task)), summary.exc_task);
			snprintf(s_status.appElfSha256, sizeof(s_status.appElfSha256), "%.*s", int(sizeof(summary.app_elf_sha256)), reinterpret_cast<const char *>(summary.app_elf_sha256));
			char runningSha[65] = {};
			CrashDump_GetRunningElfSha256(runningSha);
			// IDF may store only a prefix in the core dump (CONFIG_APP_RETRIEVE_LEN_ELF_SHA).
			s_status.firmwareMatches = s_status.appElfSha256[0] && strncmp(runningSha, s_status.appElfSha256, strlen(s_status.appElfSha256)) == 0;
			s_status.exceptionPc = summary.exc_pc;
	#if CONFIG_IDF_TARGET_ARCH_XTENSA
			s_status.backtraceDepth = std::min<uint32_t>(summary.exc_bt_info.depth, 16);
			std::copy_n(summary.exc_bt_info.bt, s_status.backtraceDepth, s_status.backtrace);
			s_status.backtraceCorrupted = summary.exc_bt_info.corrupted;
	#endif
		}
	}
#endif
}
} // namespace

void CrashDump_GetRunningElfSha256(char (&output)[65]) {
	// esp_app_get_elf_sha256 truncates to CONFIG_APP_RETRIEVE_LEN_ELF_SHA even with a larger buffer.
	const uint8_t *sha = esp_app_get_description()->app_elf_sha256;
	static constexpr char hex[] = "0123456789abcdef";
	for (size_t i = 0; i < 32; ++i) {
		output[i * 2] = hex[sha[i] >> 4];
		output[i * 2 + 1] = hex[sha[i] & 15];
	}
	output[64] = '\0';
}

void CrashDump_Init(void) {
	refreshStatus();
	if (!s_status.partitionPresent) {
		Log_Println("Core dump storage: partition missing", LOGLEVEL_ERROR);
	} else if (!s_status.available) {
		Log_Println("Core dump storage: ready, no stored crash", LOGLEVEL_DEBUG);
	} else if (s_status.valid) {
		Log_Printf(LOGLEVEL_ERROR, "Stored core dump available (%u bytes%s%s)", static_cast<unsigned>(s_status.size), s_status.panicReason[0] ? ": " : "", s_status.panicReason);
	} else {
		Log_Printf(LOGLEVEL_ERROR, "Stored core dump is corrupt (%u bytes)", static_cast<unsigned>(s_status.size));
	}
}

const CrashDumpStatus &CrashDump_GetStatus(void) {
	return s_status;
}

size_t CrashDump_Read(size_t offset, uint8_t *buffer, size_t length) {
	if (!s_status.available || !s_partition || !buffer || (offset >= s_status.size)) {
		return 0;
	}
	const size_t readLength = std::min(length, s_status.size - offset);
	if (esp_partition_read(s_partition, s_imageOffset + offset, buffer, readLength) != ESP_OK) {
		return 0;
	}
	return readLength;
}

bool CrashDump_Erase(void) {
	if (!s_status.partitionPresent || (esp_core_dump_image_erase() != ESP_OK)) {
		return false;
	}
	refreshStatus();
	return !s_status.available;
}
