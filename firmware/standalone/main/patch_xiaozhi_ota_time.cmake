# Patch the pinned Xiaozhi OTA clock handling without changing its submodule pointer.
# The OTA contract supplies a UTC Unix timestamp; timezone_offset is presentation metadata.
set(_xiaozhi_ota_source "${XIAOZHI_DIR}/main/ota.cc")
file(READ "${_xiaozhi_ota_source}" _xiaozhi_ota_content)

set(_xiaozhi_ota_old [=[
            // 设置系统时间
            struct timeval tv;
            double ts = timestamp->valuedouble;
            
            // 如果有时区偏移，计算本地时间
            if (cJSON_IsNumber(timezone_offset)) {
                ts += (timezone_offset->valueint * 60 * 1000); // 转换分钟为毫秒
            }
            
            tv.tv_sec = (time_t)(ts / 1000);  // 转换毫秒为秒
]=])
set(_xiaozhi_ota_new [=[
            // The server timestamp is UTC Unix time. Never apply the display
            // timezone to the system clock; localtime_r() handles UTC+7.
            struct timeval tv;
            const double ts = timestamp->valuedouble;

            tv.tv_sec = (time_t)(ts / 1000);
]=])

string(FIND "${_xiaozhi_ota_content}" "${_xiaozhi_ota_old}" _xiaozhi_old_index)
if(NOT _xiaozhi_old_index EQUAL -1)
    string(REPLACE "${_xiaozhi_ota_old}" "${_xiaozhi_ota_new}"
           _xiaozhi_ota_content "${_xiaozhi_ota_content}")
    file(WRITE "${_xiaozhi_ota_source}" "${_xiaozhi_ota_content}")
else()
    string(FIND "${_xiaozhi_ota_content}"
           "Never apply the display timezone to the system clock" _xiaozhi_fixed_index)
    if(_xiaozhi_fixed_index EQUAL -1)
        message(FATAL_ERROR "Unexpected Xiaozhi ota.cc; refusing to apply an unverified time patch")
    endif()
endif()
