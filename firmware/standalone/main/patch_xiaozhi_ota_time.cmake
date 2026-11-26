# Patch the pinned Xiaozhi OTA clock handling without changing its submodule pointer.
# The OTA contract supplies a UTC Unix timestamp; timezone_offset is presentation metadata.
set(_xiaozhi_ota_source "${XIAOZHI_DIR}/main/ota.cc")
file(READ "${_xiaozhi_ota_source}" _xiaozhi_ota_content)

set(_xiaozhi_timezone_add [=[
            if (cJSON_IsNumber(timezone_offset)) {
                ts += (timezone_offset->valueint * 60 * 1000); // 转换分钟为毫秒
            }
]=])
set(_xiaozhi_timezone_keep [=[
            (void)timezone_offset;  // Offset is for display only; system time remains UTC.
]=])

string(FIND "${_xiaozhi_ota_content}" "${_xiaozhi_timezone_add}" _xiaozhi_old_index)
if(NOT _xiaozhi_old_index EQUAL -1)
    string(REPLACE "${_xiaozhi_timezone_add}" "${_xiaozhi_timezone_keep}"
           _xiaozhi_ota_content "${_xiaozhi_ota_content}")
    file(WRITE "${_xiaozhi_ota_source}" "${_xiaozhi_ota_content}")
else()
    string(FIND "${_xiaozhi_ota_content}"
           "Offset is for display only; system time remains UTC" _xiaozhi_fixed_index)
    if(_xiaozhi_fixed_index EQUAL -1)
        message(FATAL_ERROR "Unexpected Xiaozhi ota.cc; refusing to apply an unverified time patch")
    endif()
endif()
