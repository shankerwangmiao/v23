#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <soundio/soundio.h>

static struct SoundIo *soundio = NULL;
static struct SoundIoDevice *soundio_device = NULL;
static struct SoundIoInStream *instream = NULL;
static struct SoundIoOutStream *outstream = NULL;
struct SoundIoRingBuffer *ring_buffer = NULL;

#define panic(fmt, ...) do {\
    __panic(fmt, __FUNCTION__, __FILE__, __LINE__, ##__VA_ARGS__); \
    exit(1); \
}while(0);

static void __panic(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    size_t buf_len = strlen(format);
    char buf_fmt[buf_len + 100];
    snprintf(buf_fmt, sizeof(buf_fmt), "%s%s\n", "%s(%s %d): ", format);
    vfprintf(stderr, buf_fmt, ap);
    va_end(ap);
}

static int min_int(int a, int b) {
    return (a < b) ? a : b;
}
static void read_callback(struct SoundIoInStream *instream, int frame_count_min, int frame_count_max) {
    struct SoundIoChannelArea *areas;
    int err;
    char *write_ptr = soundio_ring_buffer_write_ptr(ring_buffer);
    int free_bytes = soundio_ring_buffer_free_count(ring_buffer);
    int free_count = free_bytes / instream->bytes_per_frame;
    if (frame_count_min > free_count)
        panic("ring buffer overflow");
    int write_frames = min_int(free_count, frame_count_max);
    int frames_left = write_frames;
    for (;;) {
        int frame_count = frames_left;
        if ((err = soundio_instream_begin_read(instream, &areas, &frame_count)))
            panic("begin read error: %s", soundio_strerror(err));
        if (!frame_count)
            break;
        if (!areas) {
            // Due to an overflow there is a hole. Fill the ring buffer with
            // silence for the size of the hole.
            memset(write_ptr, 0, frame_count * instream->bytes_per_frame);
            fprintf(stderr, "Dropped %d frames due to internal overflow\n", frame_count);
        } else {
            for (int frame = 0; frame < frame_count; frame += 1) {
                for (int ch = 0; ch < instream->layout.channel_count; ch += 1) {
                    memcpy(write_ptr, areas[ch].ptr, instream->bytes_per_sample);
                    areas[ch].ptr += areas[ch].step;
                    write_ptr += instream->bytes_per_sample;
                }
            }
        }
        if ((err = soundio_instream_end_read(instream)))
            panic("end read error: %s", soundio_strerror(err));
        frames_left -= frame_count;
        if (frames_left <= 0)
            break;
    }
    int advance_bytes = write_frames * instream->bytes_per_frame;
    soundio_ring_buffer_advance_write_ptr(ring_buffer, advance_bytes);
}
static void write_callback(struct SoundIoOutStream *outstream, int frame_count_min, int frame_count_max) {
    struct SoundIoChannelArea *areas;
    int frames_left;
    int frame_count;
    int err;
    char *read_ptr = soundio_ring_buffer_read_ptr(ring_buffer);
    int fill_bytes = soundio_ring_buffer_fill_count(ring_buffer);
    int fill_count = fill_bytes / outstream->bytes_per_frame;
    if (frame_count_min > fill_count) {
        // Ring buffer does not have enough data, fill with zeroes.
        frames_left = frame_count_min;
        for (;;) {
            frame_count = frames_left;
            if (frame_count <= 0)
              return;
            if ((err = soundio_outstream_begin_write(outstream, &areas, &frame_count)))
                panic("begin write error: %s", soundio_strerror(err));
            if (frame_count <= 0)
                return;
            for (int frame = 0; frame < frame_count; frame += 1) {
                for (int ch = 0; ch < outstream->layout.channel_count; ch += 1) {
                    memset(areas[ch].ptr, 0, outstream->bytes_per_sample);
                    areas[ch].ptr += areas[ch].step;
                }
            }
            if ((err = soundio_outstream_end_write(outstream)))
                panic("end write error: %s", soundio_strerror(err));
            frames_left -= frame_count;
        }
    }
    int read_count = min_int(frame_count_max, fill_count);
    frames_left = read_count;
    while (frames_left > 0) {
        int frame_count = frames_left;
        if ((err = soundio_outstream_begin_write(outstream, &areas, &frame_count)))
            panic("begin write error: %s", soundio_strerror(err));
        if (frame_count <= 0)
            break;
        for (int frame = 0; frame < frame_count; frame += 1) {
            for (int ch = 0; ch < outstream->layout.channel_count; ch += 1) {
                memcpy(areas[ch].ptr, read_ptr, outstream->bytes_per_sample);
                areas[ch].ptr += areas[ch].step;
                read_ptr += outstream->bytes_per_sample;
            }
        }
        if ((err = soundio_outstream_end_write(outstream)))
            panic("end write error: %s", soundio_strerror(err));
        frames_left -= frame_count;
    }
    soundio_ring_buffer_advance_read_ptr(ring_buffer, read_count * outstream->bytes_per_frame);
}
static void underflow_callback(struct SoundIoOutStream *outstream) {
    static int count = 0;
    fprintf(stderr, "underflow %d\n", ++count);
}
static void overflow_callback(struct SoundIoOutStream *instream) {
    static int count = 0;
    fprintf(stderr, "overflow %d\n", ++count);
}

bool audioio_alsa_init(const char* device, int rate, int audio_latency, char mode)
{

    if(soundio){
        panic("Audio device is already initialized");
    }
    soundio = soundio_create();
    if (!soundio)
        panic("out of memory");
    int err = soundio_connect(soundio);
    if (err)
        panic("error connecting: %s", soundio_strerror(err));

    soundio_flush_events(soundio);

    if (mode != 'r' && mode != 'w'){
        fprintf(stderr, "Invalid mode specified (%c)\n", mode);
        return false;
    }

    int default_device_index = mode == 'w' ?
        soundio_default_output_device_index(soundio) :
        soundio_default_input_device_index(soundio);

    if (default_device_index < 0) {
        panic("no device found");
    }

    struct SoundIoDevice* (*__soundio_get_device) (struct SoundIo *,int);
    __soundio_get_device = mode == 'w' ?
        soundio_get_output_device :
        soundio_get_input_device;

    int device_index = default_device_index;
    if (device) {
        bool found = false;
        int device_count = mode == 'w' ?
            soundio_output_device_count(soundio) :
            soundio_input_device_count(soundio);

        for (int i = 0; i < device_count; i++){
            struct SoundIoDevice *snd_device = __soundio_get_device(soundio, i);
            if (strcmp(snd_device->id, device) == 0) {
                device_index = i;
                found = true;
            }
            soundio_device_unref(snd_device);
            if(found) {
                break;
            }
        }

        if(!found){
            panic("invalid device name: %s", device);
        }
    }

    soundio_device = __soundio_get_device(soundio, device_index);

    if(!soundio_device){
        panic("could not get device: out of memory");
    }

    fprintf(stderr, "Device: %s\n", soundio_device->name);

    enum SoundIoFormat format = SoundIoFormatS16NE;

    if(mode == 'w'){
        outstream = soundio_outstream_create(soundio_device);
        if (!outstream)
            panic("out of memory");
        outstream->format = format;
        outstream->sample_rate = rate;
        outstream->layout = *soundio_channel_layout_get_builtin(SoundIoChannelLayoutIdMono);
        outstream->software_latency = audio_latency / 1000.0;
        outstream->write_callback = write_callback;
        outstream->underflow_callback = underflow_callback;
        if ((err = soundio_outstream_open(outstream))) {
            panic("unable to open output stream: %s", soundio_strerror(err));
        }
        if ((err = soundio_outstream_start(outstream))) {
            panic("unable to start device: %s", soundio_strerror(err));
        }
    }else{
        instream = soundio_instream_create(soundio_device);
        if (!instream)
            panic("out of memory");
        instream->format = format;
        instream->sample_rate = rate;
        instream->layout = *soundio_channel_layout_get_builtin(SoundIoChannelLayoutIdMono);
        instream->software_latency = audio_latency / 1000.0;
        instream->read_callback = read_callback;
        instream->overflow_callback = overflow_callback;
        if ((err = soundio_instream_open(instream))) {
            panic("unable to open input stream: %s", soundio_strerror(err));
        }
        if ((err = soundio_instream_start(instream))) {
            panic("unable to start device: %s", soundio_strerror(err));
        }
    }

    int capacity = audio_latency * 2 * rate * sizeof(int16_t) / 1000;
    ring_buffer = soundio_ring_buffer_create(soundio, capacity);
    if (!ring_buffer)
        panic("unable to create ring buffer: out of memory");
    char *buf = soundio_ring_buffer_write_ptr(ring_buffer);
    memset(buf, 0, capacity / 2);
    soundio_ring_buffer_advance_write_ptr(ring_buffer, capacity / 2);
    return true;
}

size_t audioio_alsa_getsamples(int16_t *buf, size_t n)
{
    while(soundio_ring_buffer_fill_count(ring_buffer) == 0);
    char *read_ptr = soundio_ring_buffer_read_ptr(ring_buffer);
    int fill_count = soundio_ring_buffer_fill_count(ring_buffer) / sizeof(buf[0]);
    fill_count = fill_count > n ? n : fill_count;
    memcpy(buf, read_ptr, fill_count * sizeof(buf[0]));
    soundio_ring_buffer_advance_read_ptr(ring_buffer, fill_count * sizeof(buf[0]));
    return fill_count;
}

size_t audioio_alsa_putsamples(int16_t *buf, size_t n)
{
    //fprintf(stderr, "putsamples: start\n");
    while(soundio_ring_buffer_free_count(ring_buffer) == 0);
    char *write_ptr = soundio_ring_buffer_write_ptr(ring_buffer);
    int free_count = soundio_ring_buffer_free_count(ring_buffer) / sizeof(buf[0]);
    free_count = free_count > n ? n : free_count;
    memcpy(write_ptr, buf, free_count * sizeof(buf[0]));
    soundio_ring_buffer_advance_write_ptr(ring_buffer, free_count * sizeof(buf[0]));
    return free_count;
}

void audioio_alsa_stop()
{
    if(outstream){
        soundio_outstream_destroy(outstream);
        outstream = NULL;
    }
    if(instream){
        soundio_instream_destroy(instream);
        instream = NULL;
    }
    if(soundio_device){
        soundio_device_unref(soundio_device);
        soundio_device = NULL;
    }
    if(soundio){
        soundio_destroy(soundio);
        soundio = NULL;
    }
}

