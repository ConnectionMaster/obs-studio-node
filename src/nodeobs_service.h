#pragma once

#include <node.h>
#include <obs.h>
#include <string>
#include <iostream>
#include <thread>
#include <util/config-file.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <algorithm>
#include <sys/stat.h>
#include "nodeobs_api.h"
#include <map>
#include <mutex>

#include <nan.h>
#include <uv.h>

#include "nodeobs_audio_encoders.h"

#ifdef _WIN32
 
#ifdef _MSC_VER
#include <direct.h>
#define getcwd _getcwd
#endif 
#else
#include <unistd.h>
#endif

#define SIMPLE_ENCODER_X264                    "obs_x264"
#define SIMPLE_ENCODER_X264_LOWCPU             "obs_x264"
#define SIMPLE_ENCODER_QSV                     "obs_qsv11"
#define SIMPLE_ENCODER_NVENC                   "ffmpeg_nvenc"
#define SIMPLE_ENCODER_AMD                     "amd_amf_h264"

using namespace std;
using namespace v8;

class SignalInfo {
private: 
	std::string m_outputType;
	std::string m_signal;
	int m_code;
	std::string m_errorMessage;
public:

	SignalInfo() {};
	SignalInfo(std::string outputType, std::string signal) {
		m_outputType = outputType;
		m_signal = signal;
		m_code = 0;
		m_errorMessage = "";
	}
	std::string getOutputType(void) { return m_outputType; };
	std::string getSignal(void) { return m_signal; };

	int getCode(void) { return m_code; };
	void setCode(int code) { m_code = code; };
	std::string getErrorMessage(void) { return m_errorMessage; };
	void setErrorMessage(std::string errorMessage) { m_errorMessage = errorMessage; };
};

class ForeignWorker {
private:
	uv_async_t * async;

	static void AsyncClose(uv_handle_t *handle) {
		ForeignWorker *worker =
			reinterpret_cast<ForeignWorker*>(handle->data);

		worker->Destroy();
	}

	static NAUV_WORK_CB(AsyncCallback) {
		ForeignWorker *worker =
			reinterpret_cast<ForeignWorker*>(async->data);
		worker->Execute();
		uv_close(reinterpret_cast<uv_handle_t*>(async), ForeignWorker::AsyncClose);
	}

protected:
	Nan::Callback *callback;

	v8::Local<v8::Value> Call(int argc = 0, v8::Local<v8::Value> params[] = 0) {
		return callback->Call(argc, params);
	}

public:
	ForeignWorker(Nan::Callback *callback) {
		async = new uv_async_t;

		uv_async_init(
			uv_default_loop()
			, async
			, AsyncCallback
		);

		async->data = this;
		this->callback = callback;
	}

	void Send() {
		uv_async_send(async);
	}

	virtual void Execute() = 0;
	virtual void Destroy() {
		delete this;
	};

	virtual ~ForeignWorker() {
		delete async;
	}
};

class Worker : public ForeignWorker {
public:
	SignalInfo m_signalInfo;
	
	Worker(Nan::Callback *callback, SignalInfo signalInfo)
		: ForeignWorker(callback) {
		m_signalInfo = signalInfo;
	}

	virtual void Execute() {
		Isolate *isolate = v8::Isolate::GetCurrent();
		v8::Local<v8::Value> args[1];
		
		v8::Local<v8::Value> argv = v8::Object::New(isolate);
		argv->ToObject()->Set(String::NewFromUtf8(isolate, "type"), String::NewFromUtf8(isolate, m_signalInfo.getOutputType().c_str()));
		argv->ToObject()->Set(String::NewFromUtf8(isolate, "signal"), String::NewFromUtf8(isolate, m_signalInfo.getSignal().c_str()));
		argv->ToObject()->Set(String::NewFromUtf8(isolate, "code"), Number::New(isolate, m_signalInfo.getCode()));
		argv->ToObject()->Set(String::NewFromUtf8(isolate, "error"), String::NewFromUtf8(isolate, m_signalInfo.getErrorMessage().c_str()));
		args[0] = argv;

		Call(1, args);
	}

	virtual void Destroy() {
		delete this;
	}
};

class OBS_service
{
public:
	OBS_service();
	~OBS_service();

	/**
	 * Sets base audio output format/channels/samples/etc
	 *
	 * @note Cannot reset base audio if an output is currently active.
	 */
	static void OBS_service_resetAudioContext(const FunctionCallbackInfo<Value>& args);

	/**
	 * Sets base video ouput base resolution/fps/format.
	 *
	 * @note This data cannot be changed if an output is corrently active.
	 * @note The graphics module cannot be changed without fully destroying the
	 *       OBS context.
	 *
	 * @param   ovi  Pointer to an obs_video_info structure containing the
	 *               specification of the graphics subsystem,

	 * @return       OBS_VIDEO_SUCCESS if sucessful
	 * 		         OBS_VIDEO_NOT_SUPPORTED  if the adapter lacks capabilities
	 * 		         OBS_VIDEO_INVALID_PARAM if a parameter is invalid
	 * 	             OBS_VIDEO_CURRENTLY_ACTIVE if video is currently active
	 * 		         OBS_VIDEO_MODULE_NOT_FOUND if the graphics module is not found
	 * 		         OBS_VIDEO_FAIL for generic failure
	 */
	static void OBS_service_resetVideoContext(const FunctionCallbackInfo<Value>& args);

	/**
	 * Create a default AAC audio encoder

	 * @return       Return an audio encoder if successfull, NULL otherwise.
	*/	
	static void OBS_service_createAudioEncoder(const FunctionCallbackInfo<Value>& args);

	/**
	 * Create a default x264 video encoder

	 * @return       Return a video streaming encoder if successfull, NULL otherwise.
	*/	
	static void OBS_service_createVideoStreamingEncoder(const FunctionCallbackInfo<Value>& args);

	/**
	 * Create a default x264 video encoder

	 * @return       Return a video recording encoder if successfull, NULL otherwise.
	*/	
	static void OBS_service_createVideoRecordingEncoder(const FunctionCallbackInfo<Value>& args);

	/**
	 * Create a service that will be associate to the streaming output.
	 * @note The service configuration is composed by the type of the targeted plateform (e.g Twitch, youtube, ...).
	 * @note The configuration file is located under OBS global configuration files.

	 * @return       Return a service if successfull, NULL otherwise.

	*/	
	static void OBS_service_createService(const FunctionCallbackInfo<Value>& args);

    /*obs_data_set_string(settings, "format_name", "avi");
    obs_data_set_string(settings, "video_encoder", "utvideo");
    obs_data_set_string(settings, "audio_encoder", "pcm_s16le");
    obs_data_set_string(settings, "path", "./recording_1.avi");*/

	/**
	 * Create the settings that will be associated to the recording output.

	 * @param   format_name:  	output file extension, possible values are: avi, mpeg4,... 
     * @param   video_encoder:  video encoder that will be used to generate the outptut
	 * @param   audio_encoder:  video encoder that will be used to generate the outptut
     * @param   output_path:  	specify the path of the output file that will be recorded

	 * @return       Return the output settings if successfull, NULL otherwise.
	*/	
	static void OBS_service_createRecordingSettings(const FunctionCallbackInfo<Value>& args);

	/**
	 * Create a RTMP streaming output
	*/	
	static void OBS_service_createStreamingOutput(const FunctionCallbackInfo<Value>& args);

	/**
	 * Create a Ffmpeg muxer recording output
	*/	
	static void OBS_service_createRecordingOutput(const FunctionCallbackInfo<Value>& args);

	/**
	 * Start the streaming output
	*/	
	static void OBS_service_startStreaming(const FunctionCallbackInfo<Value>& args);

	/**
	 * Start the recording output
	*/	
	static void OBS_service_startRecording(const FunctionCallbackInfo<Value>& args);

	/**
	 * Stop the streaming output
	*/
	static void OBS_service_stopStreaming(const FunctionCallbackInfo<Value>& args);

	/**
	 * Stop the recording output
	*/
	static void OBS_service_stopRecording(const FunctionCallbackInfo<Value>& args);

	/**
	 * Associate the audio and video encoder to the current streaming context
	*/
	static void OBS_service_associateAudioAndVideoToTheCurrentStreamingContext(const FunctionCallbackInfo<Value>& args);

	/**
	 * Associate the audio and video encoder to the current recording context
	*/
	static void OBS_service_associateAudioAndVideoToTheCurrentRecordingContext(const FunctionCallbackInfo<Value>& args);

	/**
	 * Associate the audio and video encoder to the current streaming output
	*/
	static void OBS_service_associateAudioAndVideoEncodersToTheCurrentStreamingOutput(const FunctionCallbackInfo<Value>& args);

	/**
	 * Associate the audio and video encoder to the current recording output
	*/
	static void OBS_service_associateAudioAndVideoEncodersToTheCurrentRecordingOutput(const FunctionCallbackInfo<Value>& args);

	/**
	 * Set the service configuration to the current streaming output
	*/
	static void OBS_service_setServiceToTheStreamingOutput(const FunctionCallbackInfo<Value>& args);

	/**
	 * Set the settings to the current recording output
	*/
	static void OBS_service_setRecordingSettings(const FunctionCallbackInfo<Value>& args);

	static void OBS_service_isStreamingOutputActive(const FunctionCallbackInfo<Value>& args);

	static void OBS_service_connectOutputSignals(const FunctionCallbackInfo<Value>& args);

	static void OBS_service_test_resetAudioContext(const FunctionCallbackInfo<Value>& args);	
	static void OBS_service_test_resetVideoContext(const FunctionCallbackInfo<Value>& args);
	static void OBS_service_test_createAudioEncoder(const FunctionCallbackInfo<Value>& args);
	static void OBS_service_test_createVideoStreamingEncoder(const FunctionCallbackInfo<Value>& args);
	static void OBS_service_test_createVideoRecordingEncoder(const FunctionCallbackInfo<Value>& args);
	static void OBS_service_test_createService(const FunctionCallbackInfo<Value>& args);
	static void OBS_service_test_createRecordingSettings(const FunctionCallbackInfo<Value>& args);
	static void OBS_service_test_createStreamingOutput(const FunctionCallbackInfo<Value>& args);
	static void OBS_service_test_createRecordingOutput(const FunctionCallbackInfo<Value>& args);
	static void OBS_service_test_startStreaming(const FunctionCallbackInfo<Value>& args);
	static void OBS_service_test_startRecording(const FunctionCallbackInfo<Value>& args);
	static void OBS_service_test_stopStreaming(const FunctionCallbackInfo<Value>& args);
	static void OBS_service_test_stopRecording(const FunctionCallbackInfo<Value>& args);
	static void OBS_service_test_associateAudioAndVideoToTheCurrentStreamingContext(const FunctionCallbackInfo<Value>& args);
	static void OBS_service_test_associateAudioAndVideoToTheCurrentRecordingContext(const FunctionCallbackInfo<Value>& args);
	static void OBS_service_test_associateAudioAndVideoEncodersToTheCurrentStreamingOutput(const FunctionCallbackInfo<Value>& args);
	static void OBS_service_test_associateAudioAndVideoEncodersToTheCurrentRecordingOutput(const FunctionCallbackInfo<Value>& args);
	static void OBS_service_test_setServiceToTheStreamingOutput(const FunctionCallbackInfo<Value>& args);
	static void OBS_service_test_setRecordingSettings(const FunctionCallbackInfo<Value>& args);

private:
	static obs_data_t* createRecordingSettings(void);
	static bool startStreaming(void);
	static bool startRecording(void);
	static void stopStreaming(bool forceStop);
	static void stopRecording(void);
	static void setRecordingSettings(void);

	static void OBS_service::LoadRecordingPreset_h264(const char *encoder);
	static void OBS_service::LoadRecordingPreset_Lossless(void);
	// static void LoadRecordingPreset(void);

	static void UpdateRecordingSettings_x264_crf(int crf);
	static void UpdateRecordingSettings_qsv11(int crf);
	static void UpdateRecordingSettings_nvenc(int cqp);
	static void UpdateStreamingSettings_amd(obs_data_t *settings, int bitrate);
	static void UpdateRecordingSettings_amd_cqp(int cqp);
	static void UpdateRecordingSettings(void);


public:
	// Service
	static void 				createService();
	static obs_service_t* 		getService(void);
	static void 				setService(obs_service_t* newService);
	static void 				saveService(void);
	static void 				updateService(void);
	static void 				setServiceToTheStreamingOutput(void);

	// Encoders
	static void					createAudioEncoder(obs_encoder_t** audioEncoder);
	static void 				createVideoStreamingEncoder();
	static void 				createVideoRecordingEncoder();
	static obs_encoder_t* 		getStreamingEncoder(void);
	static void 				setStreamingEncoder(obs_encoder_t* encoder);
	static obs_encoder_t*		getRecordingEncoder(void);
	static void 				setRecordingEncoder(obs_encoder_t* encoder);
	static obs_encoder_t*		getAudioStreamingEncoder(void);
	static void 				setAudioStreamingEncoder(obs_encoder_t* encoder);
	static obs_encoder_t*		getAudioRecordingEncoder(void);
	static void 				setAudioRecordingEncoder(obs_encoder_t* encoder);

	// Outputs
	static void 				createStreamingOutput(void);
	static void 				createRecordingOutput(void);
	static obs_output_t*		getStreamingOutput(void);
	static void 				setStreamingOutput(obs_output_t* output);
	static obs_output_t*		getRecordingOutput(void);
	static void 				setRecordingOutput(obs_output_t* output);

	// Update settings
	static void updateStreamSettings(void);
	static void updateRecordSettings(void);

	// Update video encoders
	static void updateVideoStreamingEncoder(void);
	static void updateVideoRecordingEncoder(void);

	// Update outputs
	static void updateStreamingOutput(void);
	static void updateRecordingOutput(void);
	static void updateAdvancedRecordingOutput(void);
	static void UpdateFFmpegOutput(void);

	static std::string GetDefaultVideoSavePath(void);

	static bool isStreamingOutputActive(void);

	// Reset contexts
	static bool resetAudioContext(void);
	static bool resetVideoContext(const char* outputType);
	
	static void associateAudioAndVideoToTheCurrentStreamingContext(void);
	static void associateAudioAndVideoToTheCurrentRecordingContext(void);
	static void associateAudioAndVideoEncodersToTheCurrentStreamingOutput(void);
	static void associateAudioAndVideoEncodersToTheCurrentRecordingOutput(void);

	static int GetAudioBitrate(void);

	// Output signals
	static void connectOutputSignals(void);
	static void JSCallbackOutputSignal(void *data, calldata_t *);
};
