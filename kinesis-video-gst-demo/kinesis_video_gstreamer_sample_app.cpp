#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <string.h>
#include <chrono>
#include <Logger.h>
#include "KinesisVideoProducer.h"
#include <vector>
#include <stdlib.h>
#include <signal.h>

using namespace std;
using namespace com::amazonaws::kinesis::video;
using namespace log4cplus;

LOGGER_TAG("com.amazonaws.kinesis.video.gstreamer"); 
//LOGGER_TAG("com.amazonaws.kinesis.video");

#define ACCESS_KEY_ENV_VAR "AWS_ACCESS_KEY_ID"
#define SECRET_KEY_ENV_VAR "AWS_SECRET_ACCESS_KEY"
#define SESSION_TOKEN_ENV_VAR "AWS_SESSION_TOKEN"
#define DEFAULT_REGION_ENV_VAR "AWS_DEFAULT_REGION"

namespace com {
	namespace amazonaws {
		namespace kinesis {
			namespace video {

				class SampleClientCallbackProvider : public ClientCallbackProvider {
				public:

					UINT64 getCallbackCustomData() override {
						return reinterpret_cast<UINT64> (this);
					}

					StorageOverflowPressureFunc getStorageOverflowPressureCallback() override {
						return storageOverflowPressure;
					}

					static STATUS storageOverflowPressure(UINT64 custom_handle, UINT64 remaining_bytes);
				};

				class SampleStreamCallbackProvider : public StreamCallbackProvider {
				public:

					UINT64 getCallbackCustomData() override {
						return reinterpret_cast<UINT64> (this);
					}

					StreamConnectionStaleFunc getStreamConnectionStaleCallback() override {
						return streamConnectionStaleHandler;
					};

					StreamErrorReportFunc getStreamErrorReportCallback() override {
						return streamErrorReportHandler;
					};

					DroppedFrameReportFunc getDroppedFrameReportCallback() override {
						return droppedFrameReportHandler;
					};

				private:
					static STATUS
						streamConnectionStaleHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,
							UINT64 last_buffering_ack);

					static STATUS
						streamErrorReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle, UINT64 errored_timecode,
							STATUS status_code);

					static STATUS
						droppedFrameReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,
							UINT64 dropped_frame_timecode);
				};

				class SampleCredentialProvider : public StaticCredentialProvider {
					// Test rotation period is 40 second for the grace period.
					const std::chrono::duration<uint64_t> ROTATION_PERIOD = std::chrono::seconds(2400);
				public:
					SampleCredentialProvider(const Credentials &credentials) :
						StaticCredentialProvider(credentials) {}

					void updateCredentials(Credentials &credentials) override {
						// Copy the stored creds forward
						credentials = credentials_;

						// Update only the expiration
						auto now_time = std::chrono::duration_cast<std::chrono::seconds>(
							std::chrono::system_clock::now().time_since_epoch());
						auto expiration_seconds = now_time + ROTATION_PERIOD;
						credentials.setExpiration(std::chrono::seconds(expiration_seconds.count()));
						LOG_INFO("New credentials expiration is " << credentials.getExpiration().count());
					}
				};

				class SampleDeviceInfoProvider : public DefaultDeviceInfoProvider {
				public:
					device_info_t getDeviceInfo() override {
						auto device_info = DefaultDeviceInfoProvider::getDeviceInfo();
						// Set the storage size to 256mb
						device_info.storageInfo.storageSize = 512 * 1024 * 1024;
						return device_info;
					}
				};

				STATUS
					SampleClientCallbackProvider::storageOverflowPressure(UINT64 custom_handle, UINT64 remaining_bytes) {
					UNUSED_PARAM(custom_handle);
					LOG_WARN("Reporting storage overflow. Bytes remaining " << remaining_bytes);
					return STATUS_SUCCESS;
				}

				STATUS SampleStreamCallbackProvider::streamConnectionStaleHandler(UINT64 custom_data,
					STREAM_HANDLE stream_handle,
					UINT64 last_buffering_ack) {
					LOG_WARN("Reporting stream stale. Last ACK received " << last_buffering_ack);
					return STATUS_SUCCESS;
				}

				STATUS
					SampleStreamCallbackProvider::streamErrorReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,
						UINT64 errored_timecode, STATUS status_code) {
					LOG_ERROR("Reporting stream error. Errored timecode: " << errored_timecode << " Status: "
						<< status_code);
					return STATUS_SUCCESS;
				}

				STATUS
					SampleStreamCallbackProvider::droppedFrameReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,
						UINT64 dropped_frame_timecode) {
					LOG_WARN("Reporting dropped frame. Frame timecode " << dropped_frame_timecode);
					return STATUS_SUCCESS;
				}

			}  // namespace video
		}  // namespace kinesis
	}  // namespace amazonaws
}  // namespace com;

unique_ptr<Credentials> credentials_;

typedef struct _CustomData {
	GstElement *pipeline, *source, *source_filter, *encoder, *filter, *appsink, *video_convert, *h264parse;
	GstBus *bus;
	GMainLoop *main_loop;
	unique_ptr<KinesisVideoProducer> kinesis_video_producer;
	shared_ptr<KinesisVideoStream> kinesis_video_stream;
	bool stream_started;
} CustomData;

void create_kinesis_video_frame(Frame *frame, const nanoseconds &pts, const nanoseconds &dts, FRAME_FLAGS flags,
	void *data, size_t len) {
	frame->flags = flags;
	frame->decodingTs = static_cast<UINT64>(dts.count()) / DEFAULT_TIME_UNIT_IN_NANOS;
	frame->presentationTs = static_cast<UINT64>(pts.count()) / DEFAULT_TIME_UNIT_IN_NANOS;
	frame->duration = 10 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
	frame->size = static_cast<UINT32>(len);
	frame->frameData = reinterpret_cast<PBYTE>(data);
}

bool put_frame(shared_ptr<KinesisVideoStream> kinesis_video_stream, void *data, size_t len, const nanoseconds &pts, const nanoseconds &dts, FRAME_FLAGS flags) {
	Frame frame;
	create_kinesis_video_frame(&frame, pts, dts, flags, data, len);
	return kinesis_video_stream->putFrame(frame);
}

static GstFlowReturn on_new_sample(GstElement *sink, CustomData *data) {
	GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
	GstCaps* gstcaps = (GstCaps*)gst_sample_get_caps(sample);
	GstStructure * gststructforcaps = gst_caps_get_structure(gstcaps, 0);

	if (!data->stream_started) {
		data->stream_started = true;
		const GValue *gstStreamFormat = gst_structure_get_value(gststructforcaps, "codec_data");
		gchar *cpd = gst_value_serialize(gstStreamFormat);
		data->kinesis_video_stream->start(std::string(cpd));
	}

	GstBuffer *buffer = gst_sample_get_buffer(sample);
	bool isDroppable = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_CORRUPTED) ||
		GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DECODE_ONLY);
	if (!isDroppable) {
		bool isHeader = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_HEADER);
		// drop if buffer contains header only and has invalid timestamp
		if (!(isHeader && (!GST_BUFFER_PTS_IS_VALID(buffer) || !GST_BUFFER_DTS_IS_VALID(buffer)))) {
			size_t buffer_size = gst_buffer_get_size(buffer);
			uint8_t *frame_data = new uint8_t[buffer_size];
			gst_buffer_extract(buffer, 0, frame_data, buffer_size);

			bool delta = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
			FRAME_FLAGS kinesis_video_flags;
			if (!delta) {
				kinesis_video_flags = FRAME_FLAG_KEY_FRAME;
			}
			else {
				kinesis_video_flags = FRAME_FLAG_NONE;
			}

			if (false == put_frame(data->kinesis_video_stream, frame_data, buffer_size, std::chrono::nanoseconds(buffer->pts),
				std::chrono::nanoseconds(buffer->dts), kinesis_video_flags)) {
				g_printerr("Dropped frame!\n");
			}

			delete[] frame_data;
		}
	}

	gst_sample_unref(sample);
	return GST_FLOW_OK;
}

static bool resolution_supported(GstCaps *src_caps, gchar* filter, int width, int height, int framerate) {
	//gchar *my_string = g_strdup_printf("video/x-raw(memory:NVMM), width=(int)%i, height=(int)%i, format=(string)%s, framerate=(fraction)%i / 1", width, height, framerate);
	gchar *s = g_strdup_printf("%s, width=(int)%i, height=(int)%i, framerate=(fraction)%i/1", filter, width, height, framerate);
	GstCaps *query_caps; 
	query_caps = gst_caps_from_string(s);
	g_free(s);
	return gst_caps_can_intersect(query_caps, src_caps);
}

/* This function is called when an error message is posted on the bus */
static void error_cb(GstBus *bus, GstMessage *msg, CustomData *data) {
	GError *err;
	gchar *debug_info;

	/* Print error details on the screen */
	gst_message_parse_error(msg, &err, &debug_info);
	g_printerr("Error received from element %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
	g_printerr("Debugging information: %s\n", debug_info ? debug_info : "none");
	g_clear_error(&err);
	g_free(debug_info);

	g_main_loop_quit(data->main_loop);
}

void kinesis_video_init(CustomData *data, char *stream_name) {
	unique_ptr<DeviceInfoProvider> device_info_provider = make_unique<SampleDeviceInfoProvider>();
	unique_ptr<ClientCallbackProvider> client_callback_provider = make_unique<SampleClientCallbackProvider>();
	unique_ptr<StreamCallbackProvider> stream_callback_provider = make_unique<SampleStreamCallbackProvider>();

	char const *accessKey;
	char const *secretKey;
	char const *sessionToken;
	char const *defaultRegion;
	string defaultRegionStr;
	string sessionTokenStr;
	if (nullptr == (accessKey = getenv(ACCESS_KEY_ENV_VAR))) {
		accessKey = "AccessKey";
	}

	if (nullptr == (secretKey = getenv(SECRET_KEY_ENV_VAR))) {
		secretKey = "SecretKey";
	}

	if (nullptr == (sessionToken = getenv(SESSION_TOKEN_ENV_VAR))) {
		sessionTokenStr = "";
	}
	else {
		sessionTokenStr = string(sessionToken);
	}

	if (nullptr == (defaultRegion = getenv(DEFAULT_REGION_ENV_VAR))) {
		defaultRegionStr = DEFAULT_AWS_REGION;
	}
	else {
		defaultRegionStr = string(defaultRegion);
	}

	credentials_ = make_unique<Credentials>(string(accessKey),
		string(secretKey),
		sessionTokenStr,
		std::chrono::seconds(180));
	unique_ptr<CredentialProvider> credential_provider = make_unique<SampleCredentialProvider>(*credentials_.get());

	data->kinesis_video_producer = KinesisVideoProducer::createSync(move(device_info_provider),
		move(client_callback_provider),
		move(stream_callback_provider),
		move(credential_provider),
		defaultRegionStr);

	LOG_DEBUG("Client is ready");
	/* create a test stream */
	map<string, string> tags;
	char tag_name[MAX_TAG_NAME_LEN];
	char tag_val[MAX_TAG_VALUE_LEN];
	sprintf(tag_name, "piTag");
	sprintf(tag_val, "piValue");
	auto stream_definition = make_unique<StreamDefinition>(stream_name,
		hours(2),
		&tags,
		"",
		STREAMING_TYPE_REALTIME,
		"video/h264",
		milliseconds::zero(),
		seconds(2),
		milliseconds(1),
		true,//Construct a fragment at each key frame
		true,//Use provided frame timecode
		false,//Relative timecode
		true,//Ack on fragment is enabled
		true,//SDK will restart when error happens
		true,//recalculate_metrics
		0,
		30,
		4 * 1024 * 1024,
		seconds(120),
		seconds(40),
		seconds(30),
		"V_MPEG4/ISO/AVC",
		"kinesis_video",
		nullptr,
		0);
	data->kinesis_video_stream = data->kinesis_video_producer->createStreamSync(move(stream_definition));

	LOG_DEBUG("Stream is ready");
}

CustomData* pData = NULL;
void free_resources(void)
{
	if (pData != NULL && pData->pipeline != NULL) {
		g_print("Shutting down...\n");
		gst_element_set_state(pData->pipeline, GST_STATE_NULL);
		gst_object_unref(pData->pipeline);
		pData = NULL;
	}
}
void signal_handler(int sigNumber)
{
	free_resources();
	exit(0);
}

#define SOURCE "nvcamerasrc"
#define SOURCE_FILTER "video/x-raw(memory:NVMM)"
#define SOURCE_FORMAT "I420"
#define ENCODER "omxh264enc"
//#define WIDTH 1920
//#define HEIGHT 1080
//#define FRAMERATE 24
//#define BITRATE 512

int gstreamer_init(char* stream_name, int width = 1920, int height = 1080, int framerate = 24, int bitrate = 1000000) {

	BasicConfigurator config;
	config.configure();

	CustomData data;
	memset(&data, 0, sizeof(data));
	pData = &data;

	kinesis_video_init(&data, stream_name);

	/* create the elemnents */
	/*
	//gst-launch-1.0 v4l2src device=/dev/video0 ! video/x-raw,format=I420,width=1280,height=720,framerate=15/1 ! x264enc pass=quant bframes=0 ! video/x-h264,profile=baseline,format=I420,width=1280,height=720,framerate=15/1 ! matroskamux ! filesink location=test.mkv
	
	gst-launch-1.0 nvcamerasrc ! 'video/x-raw(memory:NVMM), width=(int)1280, height=(int)720, format=(string)I420' ! omxh264enc ! h264parse ! matroskamux ! filesink location=test9.mkv

	gst-launch-1.0 nvcamerasrc ! 'video/x-raw(memory:NVMM), width=(int)1920, height=(int)1080, format=(string)I420, framerate=(fraction)24/1' ! omxh264enc ! matroskamux ! filesink location=test10.mkv
	gst-launch-1.0 nvcamerasrc ! 'video/x-raw(memory:NVMM), width=(int)1920, height=(int)1080, format=(string)I420, framerate=(fraction)24/1' ! x264enc ! matroskamux ! filesink location=test10.mkv
	*/

	data.pipeline = gst_pipeline_new(SOURCE"-pipeline");
	if (!data.pipeline) {
		g_printerr("data.pipeline could not be created.\n");
		return 1;
	}

	data.source = gst_element_factory_make(SOURCE, "source");
	if (!data.source) {
		g_printerr("data.source could not be created.\n");
		return 1;
	}
	g_object_set(G_OBJECT(data.source), "do-timestamp", TRUE/*, "device", "/dev/video0"*/, NULL);
	if (GST_STATE_CHANGE_FAILURE == gst_element_set_state(data.source, GST_STATE_READY)) {
		g_printerr("Unable to set the source to ready state.\n");
		return 1;
	}
	GstPad *srcpad = gst_element_get_static_pad(data.source, "src");
	GstCaps *src_caps = gst_pad_query_caps(srcpad, NULL);
	gst_element_set_state(data.source, GST_STATE_NULL);
	gst_caps_unref(src_caps);
	gst_object_unref(srcpad);

	/*data.video_convert = gst_element_factory_make("videoconvert", "video_convert");
	if (!data.video_convert) {
	g_printerr("data.video_convert could not be created.\n");
	return 1;
	}*/

	data.source_filter = gst_element_factory_make("capsfilter", "source_filter");
	if (!data.source_filter) {
		g_printerr("data.source_filter could not be created.\n");
		return 1;
	}
	// nv omxh264enc only support I420 or NV12 as input formats see doc	
	gchar *s = g_strdup_printf("%s, width=(int)%i, height=(int)%i, format=(string)%s, framerate=(fraction)%i/1", SOURCE_FILTER, width, height, SOURCE_FORMAT, framerate);
	LOG_INFO(">>Source filter: " << s);	
	GstCaps *source_filter_caps = gst_caps_from_string(s);
	g_free(s);
	g_object_set(G_OBJECT(data.source_filter), "caps", source_filter_caps, NULL);
	gst_caps_unref(source_filter_caps);

	data.encoder = gst_element_factory_make(ENCODER, "encoder");
	if (!data.encoder) {
		g_printerr("data.encoder could not be created.\n");
		return 1;
	}
	//g_object_set(G_OBJECT(data.encoder), "control-rate", 1/*, "target-bitrate", bitrateInKBPS * 10000*/, "periodicity-idr", idrFramePeriodicty, "inline-header", FALSE, NULL);
	//omxh264enc.qp-range="-1,-1:0,6:-1,-1", omxh264enc.iframeinterval=6 allowed to reduce fragment duration from 1.2-1.5 sec to 0.2s as it seen in AWS console
	//omxh264enc supplied by NVIDIA has different parameters against that which are expected by SDK!
	g_object_set(G_OBJECT(data.encoder), "control-rate", 1, "qp-range", "-1,-1:-1,-1:-1,-1", "iframeinterval", 6/*, "bitrate", bitrate*/, NULL);
	//g_object_set(G_OBJECT(data.encoder), "bframes", 0, "key-int-max", idrFramePeriodicty, "bitrate", bitrateInKBPS, NULL);

	//data.h264parse = gst_element_factory_make("h264parse", "h264parse"); // needed to enforce avc stream format
	//if (!data.h264parse) {
	//	g_printerr("data.h264parse could not be created.\n");
	//	return 1;
	//}

	//data.filter = gst_element_factory_make("capsfilter", "encoder_filter");
	//if (!data.filter) {
	//	g_printerr("data.filter could not be created.\n");
	//	return 1;
	//}
	//GstCaps *h264_caps = gst_caps_new_simple("video/x-h264",
	//	"stream-format", G_TYPE_STRING, "avc",
	//	"alignment", G_TYPE_STRING, "au",
	//	"width", G_TYPE_INT, width,
	//	"height", G_TYPE_INT, height,
	//	"framerate", GST_TYPE_FRACTION_RANGE, framerate, 1, framerate + 1, 1,
	//	NULL);
	//	gst_caps_set_simple(h264_caps, "profile", G_TYPE_STRING, "baseline", NULL);
	//g_object_set(G_OBJECT(data.filter), "caps", h264_caps, NULL);
	//gst_caps_unref(h264_caps);
	
	data.appsink = gst_element_factory_make("appsink", "appsink");
	if (!data.appsink) {
		LOG_ERROR("data.appsink could not be created.\n");
		return 1;
	}
	g_object_set(G_OBJECT(data.appsink), "emit-signals", TRUE, "sync", FALSE, NULL);
	g_signal_connect(data.appsink, "new-sample", G_CALLBACK(on_new_sample), &data);

	gst_bin_add_many(GST_BIN(data.pipeline), data.source/*, data.video_convert*/, data.source_filter, data.encoder/*, data.h264parse, data.filter*/, data.appsink, NULL);
	if (gst_element_link_many(data.source/*, data.video_convert*/, data.source_filter, data.encoder/*, data.h264parse, data.filter*/, data.appsink, NULL) != TRUE) {
		LOG_ERROR("Elements could not be linked.\n");
		gst_object_unref(data.pipeline);
		return 1;
	}

	/* Instruct the bus to emit signals for each received message, and connect to the interesting signals */
	data.bus = gst_element_get_bus(data.pipeline);
	gst_bus_add_signal_watch(data.bus);
	g_signal_connect(G_OBJECT(data.bus), "message::error", (GCallback)error_cb, &data);
	gst_object_unref(data.bus);
	
	/* start streaming */
	GstStateChangeReturn ret = gst_element_set_state(data.pipeline, GST_STATE_PLAYING);
	if (ret == GST_STATE_CHANGE_FAILURE) {
		g_printerr("Unable to set the pipeline to the playing state.\n");
		gst_object_unref(data.pipeline);
		return 1;
	}

	data.main_loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(data.main_loop);

	free_resources();
	return 0;
}

int run_gst_pipeline(int argc, char* argv[]) {
	gst_init(&argc, &argv);

	char stream_name[MAX_STREAM_NAME_LEN];
	SNPRINTF(stream_name, MAX_STREAM_NAME_LEN, argv[optind]);
	if (0 != gstreamer_init(stream_name)) {
		free_resources();
		return 1;
	}

	///* Instruct the bus to emit signals for each received message, and connect to the interesting signals */
	//pData->bus = gst_element_get_bus(pData->pipeline);
	//gst_bus_add_signal_watch(pData->bus);
	//g_signal_connect(G_OBJECT(pData->bus), "message::error", (GCallback)error_cb, pData);
	//gst_object_unref(pData->bus);

	//g_print(">>>>fdszfszd");

	///* start streaming */
	//GstStateChangeReturn ret = gst_element_set_state(pData->pipeline, GST_STATE_PLAYING);
	//if (ret == GST_STATE_CHANGE_FAILURE) {
	//	g_printerr("Unable to set the pipeline to the playing state.\n");
	//	gst_object_unref(pData->pipeline);
	//	return 1;
	//}
	//g_print(">>>>uytjygfj");

	//pData->main_loop = g_main_loop_new(NULL, FALSE);
	//g_print(">>>>kuhkhg");
	//g_main_loop_run(pData->main_loop);

	free_resources();
	return 0;
}

/*TBD:
- how to suppress DEBUG output of kinesis?
*/

int main(int argc, char* argv[]) {
	g_print("####################################################################################################\n");
	g_print("####################################################################################################\n");
	g_print(">>>Version:180617-46!\n");

	PropertyConfigurator::doConfigure("kvs_log_configuration");
	//LOG_CONFIGURE_STDOUT("INFO");
	//LOG_CONFIGURE_STDERR("INFO");   
	//Logger::getRoot().setLogLevel(0);

	if (argc < 2) {
		LOG_ERROR("Usage: AWS_ACCESS_KEY_ID=SAMPLEKEY AWS_SECRET_ACCESS_KEY=SAMPLESECRET ./kinesis_video_gstreamer_sample_app my-stream-name");
		return 1;
	}

	if (atexit(free_resources)) {
		LOG_ERROR("Could not atexit\n");
		return 1;
	}
	if (signal(SIGINT, signal_handler) == SIG_ERR) {
		LOG_ERROR("Could not signal\n");
		return 1;
	}

	return run_gst_pipeline(argc, argv);
}