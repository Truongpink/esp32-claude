#include "chatai_xiaozhi_audio_runtime.h"
#include <chrono>
#include <memory>
#include <utility>
#include <esp_log.h>
namespace chatai::audio {
namespace { constexpr char kTag[]="XiaozhiAudioRuntime";
bool SupportedRate(uint32_t r){return r==8000||r==12000||r==16000||r==24000||r==48000;}
bool SupportedDuration(uint32_t d){return d==5||d==10||d==20||d==40||d==60||d==80||d==100||d==120;} }
XiaozhiAudioRuntime::~XiaozhiAudioRuntime(){Stop();codec_.Stop();}
esp_err_t XiaozhiAudioRuntime::Initialize(){
 std::lock_guard<std::mutex> l(lifecycle_mutex_);if(initialized_)return ESP_OK;
 AudioServiceCallbacks cb;cb.on_send_queue_available=[this](){ {std::lock_guard<std::mutex>g(send_mutex_);send_queue_notified_=true;}send_cv_.notify_one();};
 service_.SetCallbacks(cb);esp_err_t e=service_.Initialize(&codec_);if(e!=ESP_OK){ESP_LOGE(kTag,"AudioService init: %s",esp_err_to_name(e));return e;}
 if(codec_.last_error()!=ESP_OK)return codec_.last_error();initialized_=true;return ESP_OK;
}
esp_err_t XiaozhiAudioRuntime::Start(){
 std::lock_guard<std::mutex>l(lifecycle_mutex_);if(!initialized_)return ESP_ERR_INVALID_STATE;if(running_)return ESP_OK;
 {std::lock_guard<std::mutex>g(send_mutex_);send_queue_notified_=false;}esp_err_t e=service_.Start();if(e==ESP_OK)running_=true;return e;
}
esp_err_t XiaozhiAudioRuntime::Stop(){
 std::lock_guard<std::mutex>l(lifecycle_mutex_);if(!running_.exchange(false)){send_cv_.notify_all();return ESP_OK;}
 service_.EnableVoiceProcessing(false);service_.RequestInputStop();service_.Stop();
 esp_err_t mic=codec_.EnableInput(false),spk=codec_.EnableOutput(false);send_cv_.notify_all();
 if(mic!=ESP_OK&&mic!=ESP_ERR_INVALID_STATE)return mic;if(spk!=ESP_OK&&spk!=ESP_ERR_INVALID_STATE)return spk;return ESP_OK;
}
void XiaozhiAudioRuntime::SetCaptureEnabled(bool enabled){
 std::lock_guard<std::mutex>l(lifecycle_mutex_);if(!running_)return;
 if(enabled){if(!service_.EnableVoiceProcessing(true))ESP_LOGE(kTag,"AFE initialization failed");}
 else{service_.EnableVoiceProcessing(false);service_.RequestInputStop();}
}
bool XiaozhiAudioRuntime::WaitPopUplinkFrame(XiaozhiUplinkFrame& f,uint32_t timeout){
 if(!running_)return false;auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);
 while(running_){auto p=service_.PopPacketFromSendQueue();if(p){f.sample_rate=p->sample_rate;f.frame_duration_ms=p->frame_duration;f.timestamp_ms=p->timestamp;f.payload=std::move(p->payload);return true;}
  if(timeout==0)return false;std::unique_lock<std::mutex>l(send_mutex_);if(!send_cv_.wait_until(l,deadline,[this](){return send_queue_notified_||!running_;}))return false;send_queue_notified_=false;}
 return false;
}
esp_err_t XiaozhiAudioRuntime::SubmitDownlinkFrame(const uint8_t*p,size_t n,uint32_t rate,uint32_t duration){
 if(!running_)return ESP_ERR_INVALID_STATE;if(!p||n==0||n>kMaxOpusPacketBytes)return ESP_ERR_INVALID_ARG;
 if(!SupportedRate(rate)||!SupportedDuration(duration))return ESP_ERR_NOT_SUPPORTED;
 auto packet=std::make_unique<AudioStreamPacket>();packet->sample_rate=rate;packet->frame_duration=duration;packet->payload.assign(p,p+n);
 return service_.PushPacketToDecodeQueue(std::move(packet),false)?ESP_OK:ESP_ERR_NO_MEM;
}
}
