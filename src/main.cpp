#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <thread>
#include <cmath>
#include <chrono>
#include <log4cpp/Category.hh>
#include <log4cpp/FileAppender.hh>
#include <log4cpp/OstreamAppender.hh>
#include <portaudio.h>
#include <mumlib/Transport.hpp>
#include "MumpiCallback.hpp"
#include "RingBuffer.hpp"
#include "typedef_ext.h"

#define VIRTUAL_DEVICE "Virtual"

#ifndef PCM_FRAME
#define PCM_FRAME                640    // Frame for incomming PCM buffer stream from decoded opus data.
#endif

int sample_rate = 16000;
const int NUM_CHANNELS = 1;
const int FRAMES_PER_BUFFER = PCM_FRAME;

static log4cpp::Appender *appender = new log4cpp::OstreamAppender("console", &std::cout);
static log4cpp::Category& logger = log4cpp::Category::getRoot();
static volatile sig_atomic_t sig_caught = 0;
static bool mumble_thread_run_flag = true;
static bool input_consumer_thread_run_flag = true;
bool connection_state = false;

wmdevices_t wmdev;
static const PaDeviceInfo *deviceInfo;

namespace WIMIC {
    bool get_connected();
    void stop_threading();
    void detect_devices();
}

bool WIMIC::get_connected()
{
    return connection_state;
}

void WIMIC::stop_threading()
{
    sig_caught = 1;
}

void WIMIC::detect_devices()
{
	PaError err;
	err = Pa_Initialize();
	if(err != paNoError) {
		printf("PortAudio error: %s\n", Pa_GetErrorText(err));
		exit(-1);
	}

    printf("Portaudio ver.: %s\n", Pa_GetVersionText());

    wmdev.dev_count = Pa_GetDeviceCount();
    int output_device = Pa_GetDefaultOutputDevice();
    wmdev.default_dev = output_device;
    bool virtual_output = false;

    for (int i = 0; i < wmdev.dev_count; i++) {
        deviceInfo = Pa_GetDeviceInfo(i);
        wmdev.name[i] = new char[50];
        if (deviceInfo->maxInputChannels > 0) {
            wmdev.name[i] = deviceInfo->name;
            wmdev.inout_dev[i] = INOUT_DEV::INPUT_DEV;
            wmdev.type_dev[i] = TYPE_DEV::PHISICAL;
            printf("Input Device Nr %d: %s  SampleRate: %d\n", i, deviceInfo->name, (uint32_t)deviceInfo->defaultSampleRate);
        }

        if (deviceInfo->maxOutputChannels > 0) {
            wmdev.name[i] = deviceInfo->name;
            wmdev.inout_dev[i] = INOUT_DEV::OUTPUT_DEV;
            wmdev.type_dev[i] = TYPE_DEV::PHISICAL;
            printf("Output Device Nr %d: %s SampleRate: %d\n", i, deviceInfo->name, (uint32_t)deviceInfo->defaultSampleRate);
            if (strstr(deviceInfo->name, VIRTUAL_DEVICE)) {
                output_device = i;
                virtual_output = true;
                wmdev.name[i] = deviceInfo->name;
                wmdev.type_dev[i] = TYPE_DEV::VIRTUAL;
                printf("Virtual Output at Nr %d: %s SampleRate: %d\n", output_device, deviceInfo->name, (uint32_t)deviceInfo->defaultSampleRate);
            }
        }
    }

    printf("\n");
}

/**
 * Signal interrupt handler
 *
 * @param signal the signal
 */
static void sigHandler(int signal) {
	//logger.info("caught signal %d", signal);
    //printf("\ncaught signal %d\n", signal);
	sig_caught = signal;
}

/**
 * Simple data structure for storing audio sample data
 */
struct PaData {
	std::shared_ptr<RingBuffer<int16_t>> rec_buf;	// recording ring buffer
	std::shared_ptr<RingBuffer<int16_t>> out_buf;	// output ring buffer
};

/**
 * Record callback for PortAudio engine. This gets called when audio input is
 * available. This will simply take the data from the input buffer and store
 * it in a ring buffer that we allocated. That data will then be consumed by
 * another thread.
 *
 * @param  inputBuffer     input sample buffer (interleaved if multi channel)
 * @param  outputBuffer    output sample buffer (interleaved if multi channel)
 * @param  framesPerBuffer number of frames per buffer
 * @param  timeInfo        time information for stream
 * @param  statusFlags     i/o buffer status flags
 * @param  userData        circular buffer
 * @return                 PaStreamCallbackResult, paContinue usually
 */
#ifndef INPUT_STREAM_DISABLED
static int paRecordCallback(const void *inputBuffer,
                            void *outputBuffer,
                            unsigned long framesPerBuffer,
                            const PaStreamCallbackTimeInfo* timeInfo,
                            PaStreamCallbackFlags statusFlags,
                            void *userData ) {
	int result = paContinue;
	// cast the pointers to the appropriate types
	const PaData *pa_data = (const PaData*) userData;
	int16_t *input_buffer = (int16_t*) inputBuffer;
	(void) outputBuffer;
	(void) timeInfo;
	(void) statusFlags;

	if(inputBuffer != NULL) {
		// fill ring buffer with samples
		pa_data->rec_buf->push(input_buffer, 0, framesPerBuffer * NUM_CHANNELS);
	} else {
		// fill ring buffer with silence
		for(int i = 0; i < framesPerBuffer * NUM_CHANNELS; i += NUM_CHANNELS) {
			for(int j = 0; j < NUM_CHANNELS; j++) {
				pa_data->rec_buf->push(0);
			}
		}
	}

	return result;
}
#endif // INPUT_STREAM_DISABLED
/**
 * Output callback for PortAudio engine. This gets called when audio output is
 * ready to be sent. This will simply consume data from a ring buffer that we
 * allocated which is being filled by another thread.
 *
 * @param  inputBuffer     input sample buffer (interleaved if multi channel)
 * @param  outputBuffer    output sample buffer (interleaved if multi channel)
 * @param  framesPerBuffer number of frames per buffer
 * @param  timeInfo        time information for stream
 * @param  statusFlags     i/o buffer status flags
 * @param  userData        circular buffer
 * @return                 PaStreamCallbackResult, paContinue usually
 */
static int paOutputCallback(const void *inputBuffer,
                            void *outputBuffer,
                            unsigned long framesPerBuffer,
                            const PaStreamCallbackTimeInfo* timeInfo,
                            PaStreamCallbackFlags statusFlags,
                            void *userData ) {
	int result = paContinue;
	// cast the pointers to the appropriate types
	const PaData *pa_data = (const PaData*) userData;
	int16_t *output_buffer = (int16_t*) outputBuffer;
	(void) inputBuffer;
	(void) timeInfo;
	(void) framesPerBuffer;
	(void) statusFlags;

	// output pcm data to PortAudio's output_buffer by reading from our ring buffer
	// if we dont have enough samples in our ring buffer, we have to still supply 0s to the output_buffer
	const size_t requested_samples = (framesPerBuffer * NUM_CHANNELS);
	const size_t available_samples = pa_data->out_buf->getRemaining();
	//logger.info("requested_samples: %d", requested_samples);
	//logger.info("available_samples: %d", available_samples);
	if(requested_samples > available_samples) {
		pa_data->out_buf->top(output_buffer, 0, available_samples);
		for(size_t i = available_samples; i < requested_samples - available_samples; i++) {
			output_buffer[i] = 0;
		}
	} else {
		pa_data->out_buf->top(output_buffer, 0, requested_samples);
	}

	return result;
}

/**
 * Gets the next power of 2 for the passed argument
 *
 * @param  val input value
 * @return     next power of 2 for passed arg
 */
static unsigned nextPowerOf2(unsigned val) {
    val--;
    val = (val >> 1) | val;
    val = (val >> 2) | val;
    val = (val >> 4) | val;
    val = (val >> 8) | val;
    val = (val >> 16) | val;
    return ++val;
}

/**
 * Displays usage
 */
void help() {
	printf("mumpi - Simple mumble client daemon for the RaspberryPi\n\n");
	printf("Usage:\n");
	printf("mumpi [options]\n\n");
	printf("Options:\n");
	printf("-h, --help                Displays this information.\n");
	printf("-v, --verbose             Verbose mode on.\n");
	printf("-s, --server <string>     mumble server IP:PORT. Required.\n");
	printf("-u, --username <username> username. Required.\n");
	printf("-p, --password <password> password.\n");
	printf("-d, --delay <delay>       output delay in seconds. Default: \n");
	printf("                          out device's reccomended latency. 0.1\n");
	printf("                          - 0.5s should be good.\n");
	printf("-r, --sample-rate <rate>  sample rate for recording and encoding\n");
	printf("                          Default: 48000. Available options are:\n");
	printf("                          12000, 24000, or 48000\n");
	printf("-x, --vox-threshold <threshold>\n");
	printf("                          vox threshold in dB. Default: -90dB\n");
	printf("-i, --voice-hold <interval>\n");
	printf("                          voice hold interval in seconds. This \n");
	printf("                          is how long to keep transmitting after \n");
	printf("                          silence. Default: 0.050s \n");
	exit(1);
}

/**
 * main function
 *
 * Program flow:
 * 1. Parse command line args
 * 2. Init PortAudio engine and open default input and output audio devices
 * 3. Init mumlib client
 * 4. Busy loop until CTRL+C
 * 5. Clean up mumlib client
 * 6. Clean up PortAudio engine
 */

#ifdef __LIB_URUSSTUDIO__
int main_start(int argc, char *argv[])
{
    sig_caught = 0;
	optind = 0;
	connection_state = false;
#else
int main(int argc, char *argv[])
{
#endif // __URUSSTUDIO__

	bool verbose = false;
	std::string server;
	std::string username;
	std::string password;
	int next_option;
	const char* const short_options = "hvs:u:p:d:r:x:i:";
	const struct option long_options[] =
	{
		{ "help", no_argument, NULL, 'h' },
		{ "verbose", required_argument, NULL, 'v' },
		{ "server", required_argument, NULL, 's' },
		{ "username", required_argument, NULL, 'u' },
		{ "password", required_argument, NULL, 'p' },
		{ "delay", required_argument, NULL, 'd'},
		{ "sample-rate", required_argument, NULL, 'r'},
		{ "vox-threshold", required_argument, NULL, 'x'},
		{ "voice-hold", required_argument, NULL, 'i'},
		{ NULL, 0, NULL, 0 }
	};

#ifdef __MSYS__
	double output_delay = 0.02;
#else
    double output_delay = 0.02;
#endif

	double vox_threshold = -50.0;	// dB
	std::chrono::duration<double> voice_hold_interval(0.03);	// 50 ms

	// init logger
	appender->setLayout(new log4cpp::BasicLayout());
	logger.setPriority(log4cpp::Priority::WARN);
	logger.addAppender(appender);

	// parse command line args using getopt
	while(1) {
		// obtain a option
		next_option = getopt_long(argc, argv, short_options, long_options, NULL);

		if(next_option == -1)
			break;  // no more options

		switch(next_option) {

		case 'h':      // -h or --help
			help();
			break;

		case 'v':      // -v or --verbose
			verbose = true;
			break;

		case 's':      // -s or --server
			server = std::string(optarg);
			break;

		case 'u':      // -u or --username
			username = std::string(optarg);
			break;

		case 'p':
			password = std::string(optarg);
			break;

		case 'd':
			output_delay = std::stod(optarg);
			break;

		case 'r':
			sample_rate = std::atoi(optarg);
			break;

		case 'x':
			vox_threshold = std::stod(optarg);
			break;

		case 'i':
			voice_hold_interval = std::chrono::duration<double>(std::stod(optarg));
			break;

		case '?':      // Invalid option
			help();

		case -1:      // No more options
			break;

		default:      // shouldn't happen :-)
			return(1);
		}
	}

	if(verbose)
		logger.setPriority(log4cpp::Priority::INFO);

	// check for mandatory arguments
	if(server.empty() || username.empty()) {
		logger.error("Mandatory arguments not specified");
		help();
	}

	// check for valid sample rate
	if(sample_rate != 48000 && sample_rate != 24000 && sample_rate != 16000) {
		logger.error("--sample-rate option must be 12000, 24000, or 48000");
		exit(-1);
	}

	printf("Server:        %s\n", server.c_str());
	printf("Username:      %s\n", username.c_str());
	printf("delay:         %f\n", output_delay);
	printf("sample rate    %d\n", sample_rate);
	printf("vox threshold  %f\n", vox_threshold);
	printf("voice hold interval %f\n\n", voice_hold_interval);

	// logger.info("Starting in 5 seconds...");
	// std::this_thread::sleep_for(std::chrono::seconds(5));

    if (wmdev.dev_count < 1) {
        WIMIC::detect_devices();
    }

    printf("\n");

	// init audio I/O streams
#ifndef INPUT_STREAM_DISABLED
	PaStream *input_stream;
#endif // INPUT_STREAM_DISABLED
	PaStream *output_stream;
	PaData data;
	PaStreamParameters inputParameters;
	PaStreamParameters output_parameters;

	// set ring buffer size to about 500ms
	const size_t MAX_SAMPLES = nextPowerOf2(4 * sample_rate * NUM_CHANNELS);
	data.rec_buf = std::make_shared<RingBuffer<int16_t>>(MAX_SAMPLES);
	data.out_buf = std::make_shared<RingBuffer<int16_t>>(MAX_SAMPLES);

    if( wmdev.dev_count < 0 )
    {
        printf( "ERROR: Pa_CountDevices returned 0x%x\n", wmdev.dev_count );
        exit(-1);
    }

    deviceInfo = Pa_GetDeviceInfo(Pa_GetDefaultOutputDevice());
    printf("Default output device Nr %d: %s\n", Pa_GetDefaultOutputDevice(), deviceInfo->name);
    PaError err;
#ifndef INPUT_STREAM_DISABLED
    deviceInfo = Pa_GetDeviceInfo(Pa_GetDefaultInputDevice());
    printf("Default input device Nr %d: %s\n\n", Pa_GetDefaultInputDevice(), deviceInfo->name);

	inputParameters.device = Pa_GetDefaultInputDevice();
	if (inputParameters.device == paNoDevice) {
		logger.info("No default input device.");
		exit(-1);
	}

	inputParameters.channelCount = NUM_CHANNELS;
	inputParameters.sampleFormat = paInt16;

#ifdef __MSYS__
	inputParameters.suggestedLatency = 0.1;//Pa_GetDeviceInfo(inputParameters.device)->defaultLowInputLatency;
#else
    inputParameters.suggestedLatency = 0.02;//Pa_GetDeviceInfo(inputParameters.device)->defaultLowInputLatency;
#endif

	inputParameters.hostApiSpecificStreamInfo = NULL;

	logger.info("inputParameters.suggestedLatency: %.4f", inputParameters.suggestedLatency);
    printf("inputParameters.suggestedLatency: %.4f\n", inputParameters.suggestedLatency);

	err = Pa_OpenStream(&input_stream,         // the input stream
						&inputParameters,      // input params
						NULL,                  // output params
						sample_rate,           // sample rate
						FRAMES_PER_BUFFER,     // frames per buffer
						paClipOff,             // we won't output out of range samples so don't bother clipping them
						paRecordCallback,      // PortAudio callback function
						&data);                // data pointer

	logger.info("inputParameters defaultHighOutputLatency: %.4f", Pa_GetDeviceInfo(inputParameters.device)->defaultHighOutputLatency);
    printf("inputParameters defaultHighOutputLatency: %.4f\n\n", Pa_GetDeviceInfo(inputParameters.device)->defaultHighOutputLatency);

	if(err != paNoError) {
	    logger.info("Failed to open input stream: %s", Pa_GetErrorText(err));
		exit(-1);
	}
#endif
	output_parameters.device = wmdev.default_dev;
	if(output_parameters.device == paNoDevice) {
		logger.info("No default output device.");
		exit(-1);
	}
	output_parameters.channelCount = NUM_CHANNELS;
	output_parameters.sampleFormat =  paInt16;

	if(output_delay < 0.0)
		output_delay = Pa_GetDeviceInfo(output_parameters.device)->defaultHighOutputLatency;
	output_parameters.suggestedLatency = output_delay;
	output_parameters.hostApiSpecificStreamInfo = NULL;

	logger.info("output_parameters.suggestedLatency: %.4f", output_parameters.suggestedLatency);
    printf("output_parameters.suggestedLatency: %.4f\n", output_parameters.suggestedLatency);

	err = Pa_OpenStream(&output_stream,		// the output stream
						NULL, 				// input params
						&output_parameters,	// output params
						sample_rate,		// sample rate
						FRAMES_PER_BUFFER,	// frames per buffer
						paClipOff,      	// we won't output out of range samples so don't bother clipping them
						paOutputCallback,	// PortAudio callback function
						&data);				// data pointer

	logger.info("output_parameters defaultHighOutputLatency: %.4f", Pa_GetDeviceInfo(output_parameters.device)->defaultHighOutputLatency);
    printf("output_parameters defaultHighOutputLatency: %.4f\n\n", Pa_GetDeviceInfo(output_parameters.device)->defaultHighOutputLatency);

	if(err != paNoError) {
		logger.info("Failed to open output stream: %s", Pa_GetErrorText(err));
		exit(-1);
	}

	// start the streams
#ifndef INPUT_STREAM_DISABLED
	err = Pa_StartStream(input_stream);
#endif // INPUT_STREAM_DISABLED
	if(err != paNoError) {
		logger.info("Failed to start input stream: %s", Pa_GetErrorText(err));
		exit(-1);
	}

	err = Pa_StartStream(output_stream);
	if(err != paNoError) {
		logger.info("Failed to start output stream: %s", Pa_GetErrorText(err));
		exit(-1);
	}

	///////////////////////
	// init mumble library
	///////////////////////
	// open output audio stream, pipe incoming audio PCM data to output audio stream

	// This stuff should be on a separate thread
	MumpiCallback mumble_callback(data.out_buf);
	mumlib::MumlibConfiguration conf;
	//conf.opusEncoderBitrate = sample_rate;
    conf.opusSampleRate = sample_rate;

	std::thread mumble_thread([&]() {
		while(!sig_caught) {
			try {
				logger.warn("Connecting to %s", server.c_str());
                mumlib::Mumlib mum(mumble_callback, conf);
                mumble_callback.mum = &mum;
				mum.connect(server, 1234, username, password);
				mum.run();
			} catch (mumlib::TransportException &exp) {
				logger.info("TransportException main: %s.", exp.what());
				logger.info("Attempting to reconnect in 5 s.");
                logger.warn("Connection State: %d", (uint8_t)mumble_callback.mum->getConnectionState());
				std::this_thread::sleep_for(std::chrono::seconds(3));
			}
		}
	});

    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::vector<mumlib::MumbleUser> mumuser = mumble_callback.mum->getListAllUser();
    for(int i = 0; i < mumuser.size(); i++) {
        //if(mumuser[i].name == name) {
            printf("User: %s id: %d\n", mumuser[i].name.c_str(), mumuser[i].sessionId);
        //}
    }

    //mumble_callback.mum->joinChannel("Lobby");
    //mumble_callback.mum->sendVoiceTarget(0, mumlib::VoiceTargetType::CHANNEL, 1);
    //mumble_callback.mum->sendVoiceTarget(0, mumlib::VoiceTargetType::USER, 1);

	std::thread input_consumer_thread([&]() {
		// consumes the data that the input audio thread receives and sends it
		// through mumble client
		// this will continuously read from the input data circular buffer

		// Opus can encode frames of 2.5, 5, 10, 20, 40, or 60 ms
		// the Opus RFC 6716 recommends using 20ms frame sizes
		// so at 48k sample rate, 20ms is 960 samples
		const int OPUS_FRAME_SIZE = (sample_rate / 1000.0)*20;

		logger.warn("OPUS_FRAME_SIZE: %d", OPUS_FRAME_SIZE);

		std::chrono::steady_clock::time_point start;
		std::chrono::steady_clock::time_point now;
		bool voice_hold_flag = false;
		bool first_run_flag = true;
		int16_t *out_buf = new int16_t[MAX_SAMPLES];

        int cnt = 0;
        bool sw = false;

		while(!sig_caught) {
			if(!data.rec_buf->isEmpty() && data.rec_buf->getRemaining() >= OPUS_FRAME_SIZE) {

				// perform VOX algorithm
				// convert each sample to dB
				// take average (RMS) of all samples
				// if average >= threshold, transmit, else ignore
				// dB = 20 * log_10(rms)

				// also perform a "voice hold" for aprox voice_hold_interval
				// if we have just transmitted

				// do a bulk get and send it through mumble client
				if(mumble_callback.mum->getConnectionState() == mumlib::ConnectionState::CONNECTED) {
					data.rec_buf->top(out_buf, 0, OPUS_FRAME_SIZE);

					// compute RMS of sample window
					double sum = 0;
					for(int i = 0; i < OPUS_FRAME_SIZE; i++) {
						const double sample = std::abs(out_buf[i]) / (double)INT16_MAX;
						sum += sample * sample;
					}
					const double rms = std::sqrt(sum / (double)OPUS_FRAME_SIZE);

					double db = vox_threshold;
					if(rms > 0.0)
						db = 20.0 * std::log10(rms);

					//logger.warn("Recorded voice dB: %.2f", db);

					if (!first_run_flag) {
						now = std::chrono::steady_clock::now();
						auto duration = now - start;
						if(duration < voice_hold_interval)
							voice_hold_flag = true;
						else
							voice_hold_flag = false;
					}

					if (db > vox_threshold || voice_hold_flag) { // only tx if vox threshold met
                        if ((out_buf != NULL) && (sw)) {
                            //logger.warn("SW Recorded voice dB: %.2f", db);
                            mumble_callback.mum->sendAudioDataTarget(0, out_buf, OPUS_FRAME_SIZE);
                        }
						if(!voice_hold_flag) {
							start = std::chrono::steady_clock::now();
							first_run_flag = false;
						}
					}
				}
			} else {
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
                if (cnt < 250) {
                    cnt++;
                    if (cnt > 249) {
                        logger.warn("Sending audio now!");
                    }
                } else {
                    sw = true;
                }
			}

            if (mumble_callback.mum->getConnectionState() != mumlib::ConnectionState::CONNECTED) {
                static bool tictoc;
                tictoc = !tictoc;
                logger.warn("Sleeping... %s", tictoc ? "tic" : "toc");
                sleep(1);
            } else {
                connection_state = true;
            }
		}

		delete[] out_buf;
	});

	// init signal handler
	struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
	action.sa_handler = sigHandler;
	action.sa_flags = 0;
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);

	// busy loop until signal is caught
	while(!sig_caught) {
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}

	logger.setPriority(log4cpp::Priority::INFO);
	///////////////////////
	// CLEAN UP
	///////////////////////
	logger.info("Cleaning up...");

	///////////////////////////
	// clean up mumble library
	///////////////////////////
	printf("Disconnecting...\n");
    mumble_callback.mum->disconnect();
    //input_consumer_thread.detach();
	input_consumer_thread.join();
	//mumble_thread.detach();
	mumble_thread.join();
	std::this_thread::sleep_for(std::chrono::seconds(1));

	///////////////////////////
	// clean up audio library
	///////////////////////////
	logger.info("Cleaning up PortAudio...");

	// close streams
	logger.info("Closing input stream");
#ifndef INPUT_STREAM_DISABLED
	err = Pa_CloseStream(input_stream);
#endif // INPUT_STREAM_DISABLED
	if(err != paNoError) {
		logger.info("Failed to close inputstream: %s", Pa_GetErrorText(err));
		exit(-1);
	}
	logger.info("Closing output stream");
	err = Pa_CloseStream(output_stream);
	if(err != paNoError) {
		logger.info("Failed to close output stream: %s", Pa_GetErrorText(err));
		exit(-1);
	}

	// terminate PortAudio engine
	logger.info("Terminating PortAudio engine");
	err = Pa_Terminate();
	if(err != paNoError) {
		logger.info("PortAudio error: %s", Pa_GetErrorText(err));
		exit(-1);
	}

    connection_state = false;
	logger.info("Terminated!");

	return 0;
}
