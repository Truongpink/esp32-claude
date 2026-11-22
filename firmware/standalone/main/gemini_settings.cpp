#include "gemini_settings.h"

#include <nvs.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_spiffs.h>

namespace {
constexpr char kNvsNamespace[] = "chatai_ai";
constexpr char kApiKey[] = "gemini_key";
constexpr char kVoice[] = "voice";
constexpr char kSystemPrompt[] = "system_prompt";
constexpr char kInstalledFirmware[] = "fw_installed";
constexpr char kPendingFirmware[] = "fw_pending";
constexpr char kPendingFirmwareAddress[] = "fw_pending_addr";
constexpr size_t kLegacySystemPromptBytes = 3000;
constexpr size_t kMaxSystemPromptBytes = 128 * 1024;
constexpr char kPromptStoragePath0[] = "/assets/system_prompt_0.bin";
constexpr char kPromptStoragePath1[] = "/assets/system_prompt_1.bin";
constexpr uint32_t kPromptMagic = 0x43545052;  // "CTPR"
constexpr uint32_t kPromptVersion = 1;
constexpr const char* kDefaultVoice = "Kore";
constexpr const char* kDefaultSystemPrompt = "Bạn là trợ lý ChatAI TPIOS. Hãy giao tiếp tự nhiên, thân thiện và rõ ràng bằng tiếng Việt. Chỉ trả lời bằng giọng nói trong phiên hội thoại thời gian thực.";
constexpr size_t kMaxApiKeyLength = 256;

struct PromptFileHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t sequence;
    uint32_t length;
    uint32_t crc32;
};
static_assert(sizeof(PromptFileHeader) == 20, "Unexpected prompt file header size");

std::mutex g_prompt_mutex;
bool g_prompt_storage_ready = false;

esp_err_t MountPromptStorage() {
    if (g_prompt_storage_ready) return ESP_OK;

    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "assets");
    if (partition == nullptr) {
        ESP_LOGE("ChatAI-Settings", "Assets partition not found (type=data, subtype=spiffs, label=assets)");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI("ChatAI-Settings", "Assets partition found: address=0x%08lx size=%lu bytes",
             static_cast<unsigned long>(partition->address),
             static_cast<unsigned long>(partition->size));

    esp_vfs_spiffs_conf_t config{};
    config.base_path = "/assets";
    config.partition_label = "assets";
    config.max_files = 2;
    // Preserve existing assets; never format this partition automatically.
    config.format_if_mount_failed = false;
    const esp_err_t result = esp_vfs_spiffs_register(&config);
    if (result != ESP_OK) {
        ESP_LOGE("ChatAI-Settings",
                 "SPIFFS mount failed for assets (address=0x%08lx size=%lu); not formatted: %s",
                 static_cast<unsigned long>(partition->address),
                 static_cast<unsigned long>(partition->size),
                 esp_err_to_name(result));
        return result;
    }

    g_prompt_storage_ready = true;
    ESP_LOGI("ChatAI-Settings", "Prompt storage mounted at /assets");
    return ESP_OK;
}

uint32_t UpdateCrc32(uint32_t crc, const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

uint32_t PromptCrc32(const std::string& prompt) {
    return UpdateCrc32(0xFFFFFFFFu,
        reinterpret_cast<const uint8_t*>(prompt.data()), prompt.size()) ^ 0xFFFFFFFFu;
}

const char* PromptPath(int slot) {
    return slot == 0 ? kPromptStoragePath0 : kPromptStoragePath1;
}

bool ReadPromptSlotHeader(int slot, PromptFileHeader* header) {
    FILE* file = std::fopen(PromptPath(slot), "rb");
    if (file == nullptr) return false;

    const bool header_read = std::fread(header, sizeof(*header), 1, file) == 1;
    if (!header_read || header->magic != kPromptMagic ||
        header->version != kPromptVersion || header->length == 0 ||
        header->length > kMaxSystemPromptBytes) {
        std::fclose(file);
        return false;
    }

    uint8_t buffer[1024];
    size_t remaining = header->length;
    uint32_t crc = 0xFFFFFFFFu;
    bool valid = true;
    while (remaining > 0) {
        const size_t count = std::min(remaining, sizeof(buffer));
        if (std::fread(buffer, 1, count, file) != count) {
            valid = false;
            break;
        }
        crc = UpdateCrc32(crc, buffer, count);
        remaining -= count;
    }
    std::fclose(file);
    return valid && (crc ^ 0xFFFFFFFFu) == header->crc32;
}

bool LoadPromptSlot(int slot, const PromptFileHeader& header, std::string* prompt) {
    FILE* file = std::fopen(PromptPath(slot), "rb");
    if (file == nullptr) return false;
    if (std::fseek(file, static_cast<long>(sizeof(PromptFileHeader)), SEEK_SET) != 0) {
        std::fclose(file);
        return false;
    }
    prompt->assign(header.length, '\0');
    const bool read_ok = std::fread(prompt->data(), 1, header.length, file) == header.length;
    std::fclose(file);
    if (!read_ok) {
        prompt->clear();
        return false;
    }
    return true;
}

std::string GetLegacyNvsSystemPrompt() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return kDefaultSystemPrompt;
    }
    size_t size = 0;
    if (nvs_get_str(handle, kSystemPrompt, nullptr, &size) != ESP_OK ||
        size <= 1 || size > kLegacySystemPromptBytes + 1) {
        nvs_close(handle);
        return kDefaultSystemPrompt;
    }
    std::string value(size, '\0');
    const esp_err_t result = nvs_get_str(handle, kSystemPrompt, value.data(), &size);
    nvs_close(handle);
    if (result != ESP_OK) return kDefaultSystemPrompt;
    value.resize(size - 1);
    return value;
}

void RemoveLegacyNvsPrompt() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    const esp_err_t erase_result = nvs_erase_key(handle, kSystemPrompt);
    if (erase_result == ESP_OK) {
        const esp_err_t commit_result = nvs_commit(handle);
        if (commit_result != ESP_OK) {
            ESP_LOGW("ChatAI-Settings", "Could not reclaim legacy NVS prompt: %s",
                     esp_err_to_name(commit_result));
        }
    }
    nvs_close(handle);
}

bool IsNewerSequence(uint32_t left, uint32_t right) {
    return static_cast<int32_t>(left - right) > 0;
}
}  // namespace

namespace chatai::settings {

esp_err_t MigrateLegacyGeminiApiKey() {
    if (HasGeminiApiKey()) return ESP_OK;

    nvs_handle_t legacy_handle;
    if (nvs_open("gemini", NVS_READONLY, &legacy_handle) != ESP_OK) {
        return ESP_OK;
    }

    size_t size = 0;
    esp_err_t result = nvs_get_str(legacy_handle, "api_key", nullptr, &size);
    if (result != ESP_OK || size <= 1 || size > kMaxApiKeyLength + 1) {
        nvs_close(legacy_handle);
        return ESP_OK;
    }

    std::string legacy_key(size, '\0');
    result = nvs_get_str(legacy_handle, "api_key", legacy_key.data(), &size);
    nvs_close(legacy_handle);
    if (result != ESP_OK) return ESP_OK;

    legacy_key.resize(size - 1);
    result = SaveGeminiApiKey(legacy_key);
    std::fill(legacy_key.begin(), legacy_key.end(), '\0');
    if (result == ESP_OK) {
        ESP_LOGI("ChatAI-Settings", "Migrated existing Gemini API Key into standalone settings");
    }
    return result;
}

bool IsSupportedGeminiVoice(const std::string& voice) {
    static constexpr const char* kVoices[] = {
        "Achernar", "Achird", "Algenib", "Algieba", "Alnilam", "Aoede",
        "Autonoe", "Callirrhoe", "Charon", "Despina", "Enceladus", "Erinome",
        "Fenrir", "Gacrux", "Iapetus", "Kore", "Laomedeia", "Leda", "Orus",
        "Puck", "Pulcherrima", "Rasalgethi", "Sadachbia", "Sadaltager",
        "Schedar", "Sulafat", "Umbriel", "Vindemiatrix", "Zephyr", "Zubenelgenubi"
    };
    for (const char* candidate : kVoices) {
        if (voice == candidate) return true;
    }
    return false;
}

std::string GetGeminiVoice() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return kDefaultVoice;
    size_t size = 0;
    if (nvs_get_str(handle, kVoice, nullptr, &size) != ESP_OK || size <= 1 || size > 32) {
        nvs_close(handle);
        return kDefaultVoice;
    }
    std::string value(size, '\0');
    const esp_err_t result = nvs_get_str(handle, kVoice, value.data(), &size);
    nvs_close(handle);
    if (result != ESP_OK) return kDefaultVoice;
    value.resize(size - 1);
    return IsSupportedGeminiVoice(value) ? value : kDefaultVoice;
}

esp_err_t SaveGeminiVoice(const std::string& voice) {
    if (!IsSupportedGeminiVoice(voice)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_str(handle, kVoice, voice.c_str());
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

std::string GetGeminiSystemPrompt() {
    std::lock_guard<std::mutex> lock(g_prompt_mutex);
    if (MountPromptStorage() == ESP_OK) {
        PromptFileHeader headers[2]{};
        const bool valid0 = ReadPromptSlotHeader(0, &headers[0]);
        const bool valid1 = ReadPromptSlotHeader(1, &headers[1]);
        if (valid0 || valid1) {
            const int selected = valid0 && valid1
                ? (IsNewerSequence(headers[1].sequence, headers[0].sequence) ? 1 : 0)
                : (valid1 ? 1 : 0);
            std::string prompt;
            if (LoadPromptSlot(selected, headers[selected], &prompt)) return prompt;
        }
    }
    // Keep prompts saved by older firmware readable until the first new-format save.
    return GetLegacyNvsSystemPrompt();
}

esp_err_t SaveGeminiSystemPrompt(const std::string& prompt) {
    if (prompt.empty() || prompt.size() > kMaxSystemPromptBytes) {
        return ESP_ERR_INVALID_SIZE;
    }

    std::lock_guard<std::mutex> lock(g_prompt_mutex);
    const esp_err_t mount_result = MountPromptStorage();
    if (mount_result != ESP_OK) return mount_result;

    PromptFileHeader headers[2]{};
    const bool valid0 = ReadPromptSlotHeader(0, &headers[0]);
    const bool valid1 = ReadPromptSlotHeader(1, &headers[1]);
    const int active = valid0 && valid1
        ? (IsNewerSequence(headers[1].sequence, headers[0].sequence) ? 1 : 0)
        : (valid0 ? 0 : (valid1 ? 1 : -1));
    const int target = active == 0 ? 1 : 0;
    const uint32_t sequence = active < 0 ? 1 : headers[active].sequence + 1;

    FILE* file = std::fopen(PromptPath(target), "wb");
    if (file == nullptr) {
        ESP_LOGE("ChatAI-Settings", "Could not open Prompt slot %d for writing", target);
        return ESP_FAIL;
    }

    const PromptFileHeader header{
        kPromptMagic, kPromptVersion, sequence,
        static_cast<uint32_t>(prompt.size()), PromptCrc32(prompt)
    };
    // File layout: [20-byte header][prompt payload]. The active slot remains untouched.
    const bool header_written = std::fwrite(&header, sizeof(header), 1, file) == 1;
    const bool payload_written = header_written &&
        std::fwrite(prompt.data(), 1, prompt.size(), file) == prompt.size() &&
        std::fflush(file) == 0;
    const bool close_ok = std::fclose(file) == 0;
    if (!header_written || !payload_written || !close_ok) {
        ESP_LOGE("ChatAI-Settings", "Writing Prompt slot %d failed", target);
        return ESP_FAIL;
    }

    PromptFileHeader verified{};
    std::string verified_prompt;
    if (!ReadPromptSlotHeader(target, &verified) ||
        verified.sequence != sequence || verified.length != prompt.size() ||
        !LoadPromptSlot(target, verified, &verified_prompt) || verified_prompt != prompt) {
        ESP_LOGE("ChatAI-Settings", "Prompt slot %d read-back/CRC verification failed", target);
        return ESP_FAIL;
    }

    RemoveLegacyNvsPrompt();
    return ESP_OK;
}

size_t GetMaxGeminiSystemPromptBytes() {
    std::lock_guard<std::mutex> lock(g_prompt_mutex);
    const esp_err_t mount_result = MountPromptStorage();
    if (mount_result != ESP_OK) {
        ESP_LOGE("ChatAI-Settings", "Prompt storage unavailable: mount error=%s",
                 esp_err_to_name(mount_result));
        return 0;
    }

    size_t total = 0;
    size_t used = 0;
    const esp_err_t info_result = esp_spiffs_info("assets", &total, &used);
    if (info_result != ESP_OK) {
        ESP_LOGE("ChatAI-Settings", "Could not read SPIFFS usage for assets: %s",
                 esp_err_to_name(info_result));
        return 0;
    }
    if (total <= used + 4096) {
        ESP_LOGE("ChatAI-Settings", "Insufficient Prompt storage: total=%lu used=%lu reserve=4096",
                 static_cast<unsigned long>(total), static_cast<unsigned long>(used));
        return 0;
    }
    ESP_LOGI("ChatAI-Settings", "Prompt storage ready: total=%lu used=%lu available=%lu",
             static_cast<unsigned long>(total), static_cast<unsigned long>(used),
             static_cast<unsigned long>(total - used - 4096));
    return std::min(kMaxSystemPromptBytes, total - used - 4096);
}

esp_err_t SavePendingFirmwareName(const std::string& filename, uint32_t target_address) {
    if (filename.empty() || filename.size() > 64 ||
        filename.find('/') != std::string::npos ||
        filename.find('\\') != std::string::npos) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_str(handle, kPendingFirmware, filename.c_str());
    if (result == ESP_OK) result = nvs_set_u32(handle, kPendingFirmwareAddress, target_address);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

esp_err_t ConfirmPendingFirmwareName(uint32_t running_address) {
    nvs_handle_t handle;
    esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : result;
    uint32_t pending_address = 0;
    size_t size = 0;
    const esp_err_t address_result = nvs_get_u32(handle, kPendingFirmwareAddress, &pending_address);
    const esp_err_t size_result = nvs_get_str(handle, kPendingFirmware, nullptr, &size);
    std::string pending_name;
    if (address_result == ESP_OK && size_result == ESP_OK && size > 1 && size <= 65) {
        pending_name.resize(size);
        if (nvs_get_str(handle, kPendingFirmware, pending_name.data(), &size) == ESP_OK)
            pending_name.resize(size - 1);
        else
            pending_name.clear();
    }
    if (address_result == ESP_OK && pending_address == running_address && !pending_name.empty())
        result = nvs_set_str(handle, kInstalledFirmware, pending_name.c_str());
    else
        result = ESP_OK;
    if (result == ESP_OK) {
        const esp_err_t e1 = nvs_erase_key(handle, kPendingFirmware);
        const esp_err_t e2 = nvs_erase_key(handle, kPendingFirmwareAddress);
        if (e1 != ESP_OK && e1 != ESP_ERR_NVS_NOT_FOUND) result = e1;
        if (result == ESP_OK && e2 != ESP_OK && e2 != ESP_ERR_NVS_NOT_FOUND) result = e2;
    }
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

std::string GetInstalledFirmwareName() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return {};
    size_t size = 0;
    if (nvs_get_str(handle, kInstalledFirmware, nullptr, &size) != ESP_OK || size <= 1 || size > 65) {
        nvs_close(handle);
        return {};
    }
    std::string value(size, '\0');
    const esp_err_t result = nvs_get_str(handle, kInstalledFirmware, value.data(), &size);
    nvs_close(handle);
    if (result != ESP_OK) return {};
    value.resize(size - 1);
    return value;
}

bool HasGeminiApiKey() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return false;

    size_t size = 0;
    const esp_err_t result = nvs_get_str(handle, kApiKey, nullptr, &size);
    nvs_close(handle);
    return result == ESP_OK && size > 1 && size <= kMaxApiKeyLength + 1;
}

std::string GetGeminiApiKey() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return {};

    size_t size = 0;
    if (nvs_get_str(handle, kApiKey, nullptr, &size) != ESP_OK ||
        size <= 1 || size > kMaxApiKeyLength + 1) {
        nvs_close(handle);
        return {};
    }

    std::string value(size, '\0');
    const esp_err_t result = nvs_get_str(handle, kApiKey, value.data(), &size);
    nvs_close(handle);
    if (result != ESP_OK) return {};
    value.resize(size - 1);
    return value;
}

esp_err_t SaveGeminiApiKey(const std::string& api_key) {
    if (api_key.empty() || api_key.size() > kMaxApiKeyLength) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;

    result = nvs_set_str(handle, kApiKey, api_key.c_str());
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

esp_err_t DeleteGeminiApiKey() {
    nvs_handle_t handle;
    esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;

    result = nvs_erase_key(handle, kApiKey);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

}  // namespace chatai::settings
