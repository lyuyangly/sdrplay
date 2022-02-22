#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <alsa/asoundlib.h>
#include <sdrplay_api.h>

#define FIR_TAPS 32

static volatile int do_exit = 0;
snd_pcm_t *handle;
pthread_mutex_t g_pcm_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_pcm_cond = PTHREAD_COND_INITIALIZER;
snd_pcm_uframes_t frames = 64;
sdrplay_api_DeviceT *chosenDevice = NULL;

// Fir31, Gain = 1022, Fc = 0.1
int16_t fir31_0p1[FIR_TAPS] = {-2, -2, -3, -3, -3, -1, 2, 9, 18, 30, 44, 60, 75, 88, 97, 102, 102, 97, 88, 75, 60, 44, 30, 18, 9, 2, -1, -3, -3, -3, -2, -2};
int32_t filt_buf_i[FIR_TAPS] = {0};
int32_t filt_buf_q[FIR_TAPS] = {0};
size_t  samplecnt = 0;

int32_t audio_buf[FIR_TAPS] = {0};
size_t  demodcnt = 0;
int16_t ri_last = 0, rq_last = 0;
uint32_t pcm_cnt = 0;
int16_t *buffer;

void *snd_pcm_thread(void *args)
{
    int rc = 0;
    while (!do_exit) {
        pthread_mutex_lock(&g_pcm_mutex);
        pthread_cond_wait(&g_pcm_cond, &g_pcm_mutex);
        pthread_mutex_unlock(&g_pcm_mutex);
        rc = snd_pcm_writei(handle, buffer, frames);
        if (rc == -EPIPE) {
            fprintf(stderr, "underrun occurred\n");
            snd_pcm_prepare(handle);
        } else if (rc < 0) {
            fprintf(stderr,"error from writei: %s\n", snd_strerror(rc));
        } else if (rc != frames) {
            fprintf(stderr,"short write, write %d frames\n", rc);
        }
    }
    return args;
}

void StreamACallback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, unsigned int reset, void *cbContext)
{
    // Process stream callback data here
   for (size_t i = 0; i < numSamples; ++i, xi++, xq++) {
        int32_t ri = 0, rq = 0;

        for(size_t k = 0; k < FIR_TAPS; ++k) {
            ri += filt_buf_i[k] * fir31_0p1[k];
            rq += filt_buf_q[k] * fir31_0p1[k];
        }

        for(size_t k = FIR_TAPS - 1; k > 0; --k) {
            filt_buf_i[k] = filt_buf_i[k-1];
            filt_buf_q[k] = filt_buf_q[k-1];
        }
        filt_buf_i[0] = *xi;
        filt_buf_q[0] = *xq;

        // Decimation 10:1
        if (samplecnt == 0) {
            int32_t ai = 0, aq = 0;
            int32_t aud = 0;
            int16_t pcm = 0;

            ri = ri / 1024;
            rq = rq / 1024;

            // FM Demod
            // (a1 + b1j) * (a2 - b2j) = a1*a2 + b1*b2 + a2*b1*j - a1*b2*j
            ai = ri_last * ri + rq * rq_last;
            aq = ri * rq_last - ri_last * rq;

            for(size_t k = 0; k < FIR_TAPS; ++k) {
                aud += audio_buf[k] * fir31_0p1[k];
            }

            for(size_t k = FIR_TAPS - 1; k > 0; --k) {
                audio_buf[k] = audio_buf[k-1];
            }

            audio_buf[0] = 16383.0 * atan2(aq, ai) / M_PI;

            // Audio PCM S16_LE
            pcm = aud/1024.0;

            if (demodcnt == 0) {
                // fwrite(&pcm, sizeof(pcm), 1, stdout);
                // Write PCM Audio Data
                buffer[pcm_cnt++] = pcm;
                if (pcm_cnt == frames) {
                    pcm_cnt = 0;
                    pthread_mutex_lock(&g_pcm_mutex);
                    pthread_cond_signal(&g_pcm_cond);
                    pthread_mutex_unlock(&g_pcm_mutex);
                }
            }

            if (demodcnt == 9) {
                demodcnt = 0;
            } else {
                demodcnt++;
            }

            ri_last = ri;
            rq_last = rq;
        }

        if (samplecnt == 9) {
            samplecnt = 0;
        } else {
            samplecnt++;
        }
   }

    return;
}

void EventCallback(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner, sdrplay_api_EventParamsT *params, void *cbContext)
{
    switch(eventId)
    {
    case sdrplay_api_GainChange:
        printf("sdrplay_api_EventCb: %s, tuner=%s gRdB=%d lnaGRdB=%d systemGain=%.2f\n", "sdrplay_api_GainChange", (tuner == sdrplay_api_Tuner_A)? "sdrplay_api_Tuner_A": "sdrplay_api_Tuner_B", params->gainParams.gRdB, params->gainParams.lnaGRdB, params->gainParams.currGain);
        break;

    case sdrplay_api_PowerOverloadChange:
        printf("sdrplay_api_PowerOverloadChange: tuner=%s powerOverloadChangeType=%s\n", (tuner == sdrplay_api_Tuner_A)? "sdrplay_api_Tuner_A": "sdrplay_api_Tuner_B", (params->powerOverloadParams.powerOverloadChangeType == sdrplay_api_Overload_Detected)? "sdrplay_api_Overload_Detected": "sdrplay_api_Overload_Corrected");
        sdrplay_api_Update(chosenDevice->dev, tuner, sdrplay_api_Update_Ctrl_OverloadMsgAck, sdrplay_api_Update_Ext1_None);
        break;

    case sdrplay_api_DeviceRemoved:
        printf("sdrplay_api_EventCb: %s\n", "sdrplay_api_DeviceRemoved");
        break;

    default:
        printf("sdrplay_api_EventCb: %d, unknown event\n", eventId);
        break;
    }
}

static void sighandler(int signum)
{
    do_exit = 1;
}

void usage(void)
{
    fprintf(stdout,
        "play_sdr, a simple narrow band demodulator for RSP1 based receivers\n\n"
        "Use:\tplay_sdr -f freq [-options] [filename]\n"
        "\t-f frequency_to_tune_to [Hz]\n");
    exit(1);
}

int main(int argc, char *argv[])
{
    sdrplay_api_DeviceT devs[3];
    unsigned int ndev;
    char c;
    int i, opt, rc, dir;
    double Freq = 97.9e6;
    double Fs = 2.4e6;
    unsigned int PCM_Fs = 24000;
    struct sigaction sigact;
    pthread_t pcm_thread;
    sdrplay_api_ErrT err;
    sdrplay_api_DeviceParamsT *deviceParams = NULL;
    sdrplay_api_CallbackFnsT cbFns;
    sdrplay_api_Bw_MHzT bwType;
    snd_pcm_hw_params_t *params;

    while ((opt = getopt(argc, argv, "f:h")) != -1) {
        switch (opt) {
        case 'f':
            Freq = atof(optarg);
            break;
        case 'h':
        default:
            usage();
            break;
        }
    }

    // SetConsoleCtrlHandler
    sigact.sa_flags = 0;
    sigact.sa_handler = sighandler;
    sigemptyset(&sigact.sa_mask);
    sigaction(SIGINT, &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);
    sigaction(SIGQUIT, &sigact, NULL);
    sigaction(SIGPIPE, &sigact, NULL);

    // Open PCM device for playback
    rc = snd_pcm_open(&handle, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        fprintf(stderr, "unable to open pcm device: %s\n", snd_strerror(rc));
        exit(1);
    }

    // Allocate a hardware parameters object
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(handle, params);
    snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S16);
    snd_pcm_hw_params_set_channels(handle, params, 1);
    snd_pcm_hw_params_set_rate_near(handle, params, &PCM_Fs, &dir);
    printf("ALSA PCM Sample Rate = %d\n Hz", PCM_Fs);
    snd_pcm_hw_params_set_period_size_near(handle, params, &frames, &dir);
    printf("ALSA PCM Sample Period = %ld Frames\n", frames);
    snd_pcm_hw_params_set_buffer_size(handle, params, 8192U);
    // Write the parameters to the driver
    rc = snd_pcm_hw_params(handle, params);
    if (rc < 0) {
        fprintf(stderr, "unable to set hw parameters: %s\n", snd_strerror(rc));
        exit(1);
    }

    buffer = malloc(sizeof(int16_t) * 2 * frames);

    // PCM Thread Begin
    rc = pthread_create(&pcm_thread, NULL, snd_pcm_thread, NULL);
    if (rc != 0) {
        printf("PCM Thread Error!\n");
        return 0;
    }

    // SDRPlay Initialize
    if ((err = sdrplay_api_Open()) != sdrplay_api_Success) {
        printf("sdrplay_api_Open failed %s\n", sdrplay_api_GetErrorString(err));
        goto CloseApi;
        return 0;
    }

    sdrplay_api_LockDeviceApi();
    if ((err = sdrplay_api_GetDevices(devs, &ndev, sizeof(devs) / sizeof(sdrplay_api_DeviceT))) != sdrplay_api_Success) {
        printf("sdrplay_api_GetDevices failed %s\n", sdrplay_api_GetErrorString(err));
        goto UnlockDeviceAndCloseApi;
    }

    if (ndev == 0) {
        goto UnlockDeviceAndCloseApi;
    }

    for (i = 0; i < (int)ndev; i++) {
        printf("Dev%d: SerNo=%s hwVer=%d tuner=0x%.2x\n", i, devs[i].SerNo, devs[i].hwVer, devs[i].tuner);
    }

    chosenDevice = &devs[0];

    // Select chosen device
    if ((err = sdrplay_api_SelectDevice(chosenDevice)) != sdrplay_api_Success) {
        printf("sdrplay_api_SelectDevice failed %s\n", sdrplay_api_GetErrorString(err));
        goto UnlockDeviceAndCloseApi;
    }
    sdrplay_api_UnlockDeviceApi();

    // Retrieve device parameters so they can be changed if wanted
    if ((err = sdrplay_api_GetDeviceParams(chosenDevice->dev, &deviceParams)) != sdrplay_api_Success) {
        printf("sdrplay_api_GetDeviceParams failed %s\n", sdrplay_api_GetErrorString(err));
        goto CloseApi;
    }

    printf("Sample Rate = %.3f MHz, Tune Freq = %.3f MHz\n", Fs/1e6, Freq/1e6);

    // Change from default Fs 2MHz - 10MHz
    deviceParams->devParams->fsFreq.fsHz = Fs;

    // Configure tuner parameters (depends on selected Tuner which set of parameters to use)
    if (Fs < 300e3) { bwType = sdrplay_api_BW_0_200; }
    else if (Fs < 600e3) { bwType = sdrplay_api_BW_0_300; }
    else if (Fs < 1536e3) { bwType = sdrplay_api_BW_0_600; }
    else if (Fs < 5e6) { bwType = sdrplay_api_BW_1_536; }
    else if (Fs < 6e6) { bwType = sdrplay_api_BW_5_000; }
    else if (Fs < 7e6) { bwType = sdrplay_api_BW_6_000; }
    else if (Fs < 8e6) { bwType = sdrplay_api_BW_7_000; }
    else { bwType = sdrplay_api_BW_8_000; }
    deviceParams->rxChannelA->tunerParams.rfFreq.rfHz = Freq;
    deviceParams->rxChannelA->tunerParams.bwType = bwType;
    deviceParams->rxChannelA->tunerParams.ifType = sdrplay_api_IF_Zero;
    deviceParams->rxChannelA->tunerParams.gain.gRdB = 40;
    deviceParams->rxChannelA->tunerParams.gain.LNAstate = 0;
    deviceParams->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_100HZ; // sdrplay_api_AGC_DISABLE

    // Assign callback functions to be passed to sdrplay_api_Init()
    cbFns.StreamACbFn = StreamACallback;
    cbFns.EventCbFn = EventCallback;

    // This will configure the device and start streaming
    if ((err = sdrplay_api_Init(chosenDevice->dev, &cbFns, NULL)) != sdrplay_api_Success) {
        printf("sdrplay_api_Init failed %s\n", sdrplay_api_GetErrorString(err));
        goto CloseApi;
    }

    while (1) {
        if (do_exit != 0) {
            break;
        }

        c = getchar();
        if (c == 'q') {
            do_exit = 1;
            break;
        }
        else if (c == 'u') {
            deviceParams->rxChannelA->tunerParams.gain.gRdB += 1;
            // Limit it to a maximum of 59dB
            if (deviceParams->rxChannelA->tunerParams.gain.gRdB > 59)
                deviceParams->rxChannelA->tunerParams.gain.gRdB = 20;
            if ((err = sdrplay_api_Update(chosenDevice->dev, chosenDevice->tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None)) != sdrplay_api_Success) {
                printf("sdrplay_api_Update sdrplay_api_Update_Tuner_Gr failed %s\n", sdrplay_api_GetErrorString(err));
                break;
            }
        }
        else if (c == 'd') {
            deviceParams->rxChannelA->tunerParams.gain.gRdB -= 1;
            // Limit it to a minimum of 20dB
            if (deviceParams->rxChannelA->tunerParams.gain.gRdB < 20)
                deviceParams->rxChannelA->tunerParams.gain.gRdB = 59;
            if ((err = sdrplay_api_Update(chosenDevice->dev, chosenDevice->tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None)) != sdrplay_api_Success) {
                printf("sdrplay_api_Update sdrplay_api_Update_Tuner_Gr failed %s\n", sdrplay_api_GetErrorString(err));
                break;
            }
        }
        usleep(100000);
    }

    // PCM Thread End
    pthread_join(pcm_thread, NULL);

    // Close PCM
    snd_pcm_drop(handle);
    snd_pcm_drain(handle);
    snd_pcm_close(handle);
    free(buffer);

    // Finished with device so uninitialise it
    if ((err = sdrplay_api_Uninit(chosenDevice->dev)) != sdrplay_api_Success) {
        printf("sdrplay_api_Uninit failed %s\n", sdrplay_api_GetErrorString(err));
        goto CloseApi;
    }

    sdrplay_api_ReleaseDevice(chosenDevice);

UnlockDeviceAndCloseApi:
    sdrplay_api_UnlockDeviceApi();

CloseApi:
    sdrplay_api_Close();

    return 0;
}
