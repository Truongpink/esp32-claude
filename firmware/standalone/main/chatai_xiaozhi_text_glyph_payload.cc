#include "text_glyph_payload.h"

#include <esp_log.h>

#define TAG "ChatAI-GlyphPayload"

namespace TextGlyphPayload {

bool Parse(const cJSON* root, std::vector<TextGlyph>& result, uint8_t& bpp) {
    result.clear();
    bpp = 0;

    const cJSON* payload = cJSON_GetObjectItem(root, "glyph_push");
    if (cJSON_IsObject(payload)) {
        ESP_LOGW(TAG, "Ignoring glyph_push: ChatAI uses its own OLED renderer and does not mount Xiaozhi assets");
        return false;
    }

    return true;
}

}  // namespace TextGlyphPayload
