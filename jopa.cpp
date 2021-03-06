/*
    JACK-over-PulseAudio (jopa)
    Copyright (C) 2013-2017 StarBrilliant <m13253@hotmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <stdexcept>
#include <string>
#include <pthread.h>
#include <spawn.h>
#include <unistd.h>
#include <jack/jack.h>
#include <jack/ringbuffer.h>
#include <pulse/pulseaudio.h>

class JopaSession {

private:

    typedef jack_default_audio_sample_t jack_sample_t;
    typedef float pulse_sample_t;
    static constexpr unsigned num_channels = 2;
    static constexpr unsigned ringbuffer_fragments = 2;
    jack_nframes_t sample_rate = 48000;
    jack_nframes_t jack_buffer_size = 1024;

    jack_client_t* jack_client = nullptr;
    jack_port_t* jack_playback_ports[num_channels] = { nullptr };
    jack_port_t* jack_capture_ports[num_channels] = { nullptr };
    jack_port_t* jack_monitor_ports[num_channels] = { nullptr };
    jack_ringbuffer_t* jack_playback_ringbuffer = nullptr;
    jack_ringbuffer_t* jack_capture_ringbuffer = nullptr;
    jack_ringbuffer_t* jack_monitor_ringbuffer = nullptr;

    static void jack_on_shutdown(void* arg);
    static int jack_on_process(jack_nframes_t nframes, void* arg);
    static int jack_on_buffer_size(jack_nframes_t nframes, void* arg);
    static int jack_on_sample_rate(jack_nframes_t nframes, void* arg);
    static void jack_on_port_connect(jack_port_id_t a, jack_port_id_t b, int connect, void* arg);
    static void jack_on_error(char const* reason);

    pa_threaded_mainloop* pulse_mainloop = nullptr;
    pa_context* pulse_context = nullptr;
    pa_stream* pulse_playback_stream = nullptr;
    pa_stream* pulse_record_stream = nullptr;
    pa_stream* pulse_monitor_stream = nullptr;

    static void pulse_on_context_state(pa_context* c, void* userdata);
    static void pulse_on_playback_writable(pa_stream* p, size_t nbytes, void* userdata);
    static void pulse_on_record_readable(pa_stream* p, size_t nbytes, void* userdata);
    static void pulse_on_monitor_readable(pa_stream* p, size_t nbytes, void* userdata);
    static void pulse_on_playback_stream_moved(pa_stream* p, void* userdata);
    static void pulse_on_record_stream_moved(pa_stream* p, void* userdata);
    static void pulse_on_get_sink_info(pa_context* c, pa_sink_info const* i, int eol, void* userdata);

    struct JackConnectOperation {

        std::string port_name_a;
        std::string port_name_b;
        bool connect;

    };

    std::queue<JackConnectOperation> jack_connect_operations;
    void jack_schedule_connect(char const* port_name_a, char const* port_name_b, bool connect);
    void jack_finish_connect();

    class PulseThreadedMainloopLocker {

    private:

        pa_threaded_mainloop* mainloop;

    public:

        PulseThreadedMainloopLocker(pa_threaded_mainloop* mainloop);
        ~PulseThreadedMainloopLocker();

    };

    static bool pulse_is_stream_ready(pa_stream* p);
    static bool pulse_check_operation(pa_operation* o);
    static void pulse_throw_exception(pa_context* c, char const* reason);
    pa_sample_spec pulse_calc_sample_spec() const;
    pa_buffer_attr pulse_calc_buffer_attr(bool record) const;

public:

    void init();
    void run();
    ~JopaSession();

};

extern char** environ;

int main() {
    JopaSession session;
    session.init();
    session.run();
    return 0;
}

void JopaSession::init() {
    jack_set_error_function(jack_on_error);

    // Try to use the default JACK server
    jack_client = jack_client_open("JACK over PulseAudio", JackNoStartServer, nullptr);
    if(jack_client == nullptr) {
        // Create a new JACK server
        pid_t jack_server_pid;
        static char const* const jack_server_argv[] = {"jackd", "-T", "-d", "dummy", "-p", "1024", NULL};
        if(posix_spawnp(&jack_server_pid, "jackd", nullptr, nullptr, const_cast<char* const*>(jack_server_argv), environ) != 0) {
            throw std::runtime_error("Unable to start a JACK server");
        }
        // Try to use the newly started JACK server
        for(int i = 0; i < 5; ++i) {
            jack_client = jack_client_open("JACK over PulseAudio", JackNoStartServer, nullptr);
            if(jack_client != nullptr) {
                break;
            } else {
                sleep(1);
            }
        }
    }
    if(jack_client == nullptr) {
        throw std::runtime_error("Unable to connect to the JACK server");
    }

    struct sched_param sched_parameters;
    memset(&sched_parameters, 0, sizeof sched_parameters);
    sched_parameters.sched_priority = 10;
    if(pthread_setschedparam(pthread_self(), SCHED_FIFO, &sched_parameters) != 0) {
        std::fprintf(stderr, "Cannot use real-time scheduling (FIFO at priority 10)\n");
    }

    // Register callbacks
    ::jack_on_shutdown(jack_client, this->jack_on_shutdown, this);
    if(jack_set_process_callback(jack_client, jack_on_process, this) != 0) {
        throw std::runtime_error("Unable to register JACK callback functions");
    }
    if(jack_set_sample_rate_callback(jack_client, jack_on_sample_rate, this) != 0) {
        throw std::runtime_error("Unable to register JACK callback functions");
    }
    if(jack_set_buffer_size_callback(jack_client, jack_on_buffer_size, this) != 0) {
        throw std::runtime_error("Unable to register JACK callback functions");
    }
    if(jack_set_port_connect_callback(jack_client, jack_on_port_connect, this) != 0) {
        throw std::runtime_error("Unable to register JACK callback functions");
    }

    // Get JACK server information
    sample_rate = jack_get_sample_rate(jack_client);
    jack_buffer_size = jack_get_buffer_size(jack_client);

    // Create JACK ports
    for(unsigned ch = 0; ch < num_channels; ++ch) {
        std::string port_name = "playback_";
        port_name += std::to_string(ch + 1);
        jack_playback_ports[ch] = jack_port_register(jack_client, port_name.c_str(), JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput | JackPortIsTerminal, 0);
        if(jack_playback_ports[ch] == nullptr) {
            throw std::runtime_error("Unable to create JACK playback ports");
        }
    }
    for(unsigned ch = 0; ch < num_channels; ++ch) {
        std::string port_name = "capture_";
        port_name += std::to_string(ch + 1);
        jack_capture_ports[ch] = jack_port_register(jack_client, port_name.c_str(), JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput | JackPortIsTerminal, 0);
        if(jack_capture_ports[ch] == nullptr) {
            throw std::runtime_error("Unable to create JACK capture ports");
        }
    }
    for(unsigned ch = 0; ch < num_channels; ++ch) {
        std::string port_name = "monitor_";
        port_name += std::to_string(ch + 1);
        jack_monitor_ports[ch] = jack_port_register(jack_client, port_name.c_str(), JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if(jack_monitor_ports[ch] == nullptr) {
            throw std::runtime_error("Unable to create JACK monitor ports");
        }
    }

    // Create JACK ringbuffers
    jack_playback_ringbuffer = jack_ringbuffer_create(jack_buffer_size * (num_channels * sizeof (pulse_sample_t) * ringbuffer_fragments));
    if(jack_playback_ringbuffer == nullptr) {
        throw std::runtime_error("Unable to create JACK playback buffer");
    }
    jack_capture_ringbuffer = jack_ringbuffer_create(jack_buffer_size * (num_channels * sizeof (pulse_sample_t) * ringbuffer_fragments));
    if(jack_capture_ringbuffer == nullptr) {
        throw std::runtime_error("Unable to create JACK capture buffer");
    }
    jack_monitor_ringbuffer = jack_ringbuffer_create(jack_buffer_size * (num_channels * sizeof (pulse_sample_t) * ringbuffer_fragments));
    if(jack_monitor_ringbuffer == nullptr) {
        throw std::runtime_error("Unable to create JACK monitor buffer");
    }

    // Activate JACK event loop
    if(jack_activate(jack_client) != 0) {
        throw std::runtime_error("Unable to activate the JACK event loop");
    }

    // Create PulseAudio mainloop
    pulse_mainloop = pa_threaded_mainloop_new();
    if(pulse_mainloop == nullptr) {
        throw std::runtime_error("Unable to create a PulseAudio event loop");
    }
    // Create PulseAudio context
    pulse_context = pa_context_new(pa_threaded_mainloop_get_api(pulse_mainloop), "JACK over PulseAudio");
    if(pulse_context == nullptr) {
        throw std::runtime_error("Unable to create a PulseAudio context");
    }
    // Connect to PulseAudio server
    pa_context_set_state_callback(pulse_context, pulse_on_context_state, this);
    if(pa_context_connect(pulse_context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
        pulse_throw_exception(pulse_context, "Unable to connect to the PulseAudio server");
    }
}

void JopaSession::run() {
    if(pa_threaded_mainloop_start(pulse_mainloop) < 0) {
        throw std::runtime_error("Unable to run PulseAudio event loop");
    }
    // Deadlock
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mutex);
    pthread_mutex_lock(&mutex);
    std::abort();
}

JopaSession::~JopaSession() {
    if(pulse_monitor_stream != nullptr) {
        pa_stream_disconnect(pulse_monitor_stream);
        pa_stream_unref(pulse_monitor_stream);
        pulse_monitor_stream = nullptr;
    }
    if(pulse_record_stream != nullptr) {
        pa_stream_disconnect(pulse_record_stream);
        pa_stream_unref(pulse_record_stream);
        pulse_record_stream = nullptr;
    }
    if(pulse_playback_stream != nullptr) {
        pa_stream_disconnect(pulse_playback_stream);
        pa_stream_unref(pulse_playback_stream);
        pulse_playback_stream = nullptr;
    }
    if(pulse_context != nullptr) {
        pa_context_disconnect(pulse_context);
        pa_context_unref(pulse_context);
        pulse_context = nullptr;
    }
    if(pulse_mainloop != nullptr) {
        pa_threaded_mainloop_stop(pulse_mainloop);
        pa_threaded_mainloop_free(pulse_mainloop);
        pulse_mainloop = nullptr;
    }
    for(unsigned ch = 0; ch < num_channels; ++ch) {
        if(jack_monitor_ports[ch] != nullptr) {
            jack_port_unregister(jack_client, jack_monitor_ports[ch]);
            jack_monitor_ports[ch] = nullptr;
        }
    }
    for(unsigned ch = 0; ch < num_channels; ++ch) {
        if(jack_capture_ports[ch] != nullptr) {
            jack_port_unregister(jack_client, jack_capture_ports[ch]);
            jack_capture_ports[ch] = nullptr;
        }
    }
    for(unsigned ch = 0; ch < num_channels; ++ch) {
        if(jack_playback_ports[ch] != nullptr) {
            jack_port_unregister(jack_client, jack_playback_ports[ch]);
            jack_playback_ports[ch] = nullptr;
        }
    }
    if(jack_client != nullptr) {
        jack_client_close(jack_client);
        jack_client = nullptr;
    }
}

void JopaSession::jack_on_shutdown(void* arg) {
    std::exit(0);
}

int JopaSession::jack_on_process(jack_nframes_t nframes, void* arg) {
    JopaSession* self = reinterpret_cast<JopaSession*>(arg);

    self->jack_finish_connect();

    // Copy playback stream
    {
        jack_sample_t* jack_buffer[num_channels];
        for(unsigned ch = 0; ch < num_channels; ++ch) {
            if(self->jack_playback_ports[ch] != nullptr) {
                jack_buffer[ch] = (jack_sample_t*) jack_port_get_buffer(self->jack_playback_ports[ch], nframes);
            } else {
                jack_buffer[ch] = nullptr;
            }
        }
        size_t buffer_space = jack_ringbuffer_write_space(self->jack_playback_ringbuffer);
        size_t buffer_required = nframes * (num_channels * sizeof (pulse_sample_t));
        if(buffer_space >= buffer_required) {
            jack_ringbuffer_data_t write_vector[2];
            jack_ringbuffer_get_write_vector(self->jack_playback_ringbuffer, write_vector);
            for(jack_nframes_t i = 0; i < nframes; ++i) {
                for(unsigned ch = 0; ch < num_channels; ++ch) {
                    jack_nframes_t index = i * num_channels + ch;
                    size_t offset = index * sizeof (pulse_sample_t);
                    if(offset < write_vector[0].len) {
                        *(pulse_sample_t*) &write_vector[0].buf[offset] = jack_buffer[ch][i];
                    } else {
                        *(pulse_sample_t*) &write_vector[1].buf[offset - write_vector[0].len] = jack_buffer[ch][i];
                    }
                }
            }
            jack_ringbuffer_write_advance(self->jack_playback_ringbuffer, buffer_required);
        } else {
            std::fprintf(stderr, "Playback buffer overflow: %zu < %zu\n", buffer_space, buffer_required);
        }
    }

    // Copy record stream
    {
        jack_sample_t* jack_buffer[num_channels];
        for(unsigned ch = 0; ch < num_channels; ++ch) {
            if(self->jack_capture_ports[ch] != nullptr) {
                jack_buffer[ch] = (jack_sample_t*) jack_port_get_buffer(self->jack_capture_ports[ch], nframes);
            } else {
                jack_buffer[ch] = nullptr;
            }
        }
        size_t buffer_space = jack_ringbuffer_read_space(self->jack_capture_ringbuffer);
        size_t buffer_required = nframes * (num_channels * sizeof (pulse_sample_t));
        if(buffer_space >= buffer_required) {
            jack_ringbuffer_data_t read_vector[2];
            jack_ringbuffer_get_read_vector(self->jack_capture_ringbuffer, read_vector);
            for(jack_nframes_t i = 0; i < nframes; ++i) {
                for(unsigned ch = 0; ch < num_channels; ++ch) {
                    jack_nframes_t index = i * num_channels + ch;
                    size_t offset = index * sizeof (pulse_sample_t);
                    if(offset < read_vector[0].len) {
                        jack_buffer[ch][i] = *(pulse_sample_t*) &read_vector[0].buf[offset];
                    } else {
                        jack_buffer[ch][i] = *(pulse_sample_t*) &read_vector[1].buf[offset - read_vector[0].len];
                    }
                }
            }
            jack_ringbuffer_read_advance(self->jack_capture_ringbuffer, buffer_required);
        } else {
            std::fprintf(stderr, "Record buffer underflow: %zu < %zu\n", buffer_space, buffer_required);
        }
    }

    // Copy monitor stream
    {
        jack_sample_t* jack_buffer[num_channels];
        for(unsigned ch = 0; ch < num_channels; ++ch) {
            if(self->jack_monitor_ports[ch] != nullptr) {
                jack_buffer[ch] = (jack_sample_t*) jack_port_get_buffer(self->jack_monitor_ports[ch], nframes);
            } else {
                jack_buffer[ch] = nullptr;
            }
        }
        size_t buffer_space = jack_ringbuffer_read_space(self->jack_monitor_ringbuffer);
        size_t buffer_required = nframes * (num_channels * sizeof (pulse_sample_t));
        if(buffer_space >= buffer_required) {
            jack_ringbuffer_data_t read_vector[2];
            jack_ringbuffer_get_read_vector(self->jack_monitor_ringbuffer, read_vector);
            for(jack_nframes_t i = 0; i < nframes; ++i) {
                for(unsigned ch = 0; ch < num_channels; ++ch) {
                    jack_nframes_t index = i * num_channels + ch;
                    size_t offset = index * sizeof (pulse_sample_t);
                    if(offset < read_vector[0].len) {
                        jack_buffer[ch][i] = *(pulse_sample_t*) &read_vector[0].buf[offset];
                    } else {
                        jack_buffer[ch][i] = *(pulse_sample_t*) &read_vector[1].buf[offset - read_vector[0].len];
                    }
                }
            }
            jack_ringbuffer_read_advance(self->jack_monitor_ringbuffer, buffer_required);
        } else {
            std::fprintf(stderr, "Monitor buffer underflow: %zu < %zu\n", buffer_space, buffer_required);
        }
    }

    return 0;
}

int JopaSession::jack_on_buffer_size(jack_nframes_t nframes, void* arg) {
    JopaSession* self = reinterpret_cast<JopaSession*>(arg);
    PulseThreadedMainloopLocker(self->pulse_mainloop);

    // Reset PulseAudio buffer
    self->jack_buffer_size = nframes;
    pa_buffer_attr playback_buffer_attr = self->pulse_calc_buffer_attr(false);
    pa_buffer_attr record_buffer_attr = self->pulse_calc_buffer_attr(true);
    if(pulse_is_stream_ready(self->pulse_playback_stream)) {
        if(!pulse_check_operation(pa_stream_set_buffer_attr(self->pulse_playback_stream, &playback_buffer_attr, nullptr, nullptr))) {
            pulse_throw_exception(self->pulse_context, "Unable to reset PulseAudio playback buffer");
        }
    }
    if(pulse_is_stream_ready(self->pulse_record_stream)) {
        if(!pulse_check_operation(pa_stream_set_buffer_attr(self->pulse_record_stream, &record_buffer_attr, nullptr, nullptr))) {
            pulse_throw_exception(self->pulse_context, "Unable to reset PulseAudio record buffer");
        }
    }
    if(pulse_is_stream_ready(self->pulse_monitor_stream)) {
        if(!pulse_check_operation(pa_stream_set_buffer_attr(self->pulse_monitor_stream, &record_buffer_attr, nullptr, nullptr))) {
            pulse_throw_exception(self->pulse_context, "Unable to reset PulseAudio monitor buffer");
        }
    }

    // Reset JACK ringbuffer
    {
        PulseThreadedMainloopLocker locker(self->pulse_mainloop);
        jack_ringbuffer_free(self->jack_playback_ringbuffer);
        self->jack_playback_ringbuffer = jack_ringbuffer_create(self->jack_buffer_size * (num_channels * sizeof (pulse_sample_t) * ringbuffer_fragments));
        if(self->jack_playback_ringbuffer == nullptr) {
            throw std::runtime_error("Unable to create JACK playback buffer");
        }
        jack_ringbuffer_free(self->jack_capture_ringbuffer);
        self->jack_capture_ringbuffer = jack_ringbuffer_create(self->jack_buffer_size * (num_channels * sizeof (pulse_sample_t) * ringbuffer_fragments));
        if(self->jack_capture_ringbuffer == nullptr) {
            throw std::runtime_error("Unable to create JACK capture buffer");
        }
        jack_ringbuffer_free(self->jack_monitor_ringbuffer);
        self->jack_monitor_ringbuffer = jack_ringbuffer_create(self->jack_buffer_size * (num_channels * sizeof (pulse_sample_t) * ringbuffer_fragments));
        if(self->jack_monitor_ringbuffer == nullptr) {
            throw std::runtime_error("Unable to create JACK monitor buffer");
        }
    }

    std::fprintf(stderr, "JACK buffer size is %u samples (%.2lf ms).\n", nframes, 1000.0 * nframes / self->sample_rate);
    std::fprintf(stderr, "JOPA buffer size is %u samples (%.2lf ms).\n", nframes * ringbuffer_fragments, 1000.0 * nframes * ringbuffer_fragments / self->sample_rate);
    std::fprintf(stderr, "PulseAudio buffer size is %u samples (%.2lf ms).\n", nframes, 1000.0 * nframes / self->sample_rate);

    return 0;
}

int JopaSession::jack_on_sample_rate(jack_nframes_t nframes, void* arg) {
    JopaSession* self = reinterpret_cast<JopaSession*>(arg);
    PulseThreadedMainloopLocker(self->pulse_mainloop);

    // Reset PulseAudio streams
    self->sample_rate = nframes;
    if(pulse_is_stream_ready(self->pulse_playback_stream)) {
        if(!pulse_check_operation(pa_stream_update_sample_rate(self->pulse_playback_stream, nframes, nullptr, nullptr))) {
            pulse_throw_exception(self->pulse_context, "Unable to reset PulseAudio playback sample rate");
        }
    }
    if(pulse_is_stream_ready(self->pulse_record_stream)) {
        if(!pulse_check_operation(pa_stream_update_sample_rate(self->pulse_record_stream, nframes, nullptr, nullptr))) {
            pulse_throw_exception(self->pulse_context, "Unable to reset PulseAudio record sample rate");
        }
    }
    if(pulse_is_stream_ready(self->pulse_monitor_stream)) {
        if(!pulse_check_operation(pa_stream_update_sample_rate(self->pulse_monitor_stream, nframes, nullptr, nullptr))) {
            pulse_throw_exception(self->pulse_context, "Unable to reset PulseAudio monitor sample rate");
        }
    }

    std::fprintf(stderr, "Sample rate is %u Hz.\n", nframes);

    return 0;
}

void JopaSession::jack_on_port_connect(jack_port_id_t a, jack_port_id_t b, int connect, void* arg) {
    JopaSession* self = reinterpret_cast<JopaSession*>(arg);

    jack_port_t* port_a = jack_port_by_id(self->jack_client, a);
    jack_port_t* port_b = jack_port_by_id(self->jack_client, b);

    char const* port_name_a = jack_port_name(port_a);
    char const* port_name_b = jack_port_name(port_b);

    char const* port_short_name_a = jack_port_short_name(port_a);
    char const* port_short_name_b = jack_port_short_name(port_b);

    // Search for system:capture ports
    if(std::strncmp(port_name_a, "system:", 7) == 0) {
        for(unsigned ch = 0; ch < num_channels; ++ch) {
            if(std::strcmp(jack_port_short_name(self->jack_capture_ports[ch]), port_short_name_a) == 0) {
                self->jack_schedule_connect(jack_port_name(self->jack_capture_ports[ch]), port_name_b, connect);
                break;
            }
        }
    }

    // Search for system:playback ports
    if(std::strncmp(port_name_b, "system:", 7) == 0) {
        for(unsigned ch = 0; ch < num_channels; ++ch) {
            if(std::strcmp(jack_port_short_name(self->jack_playback_ports[ch]), port_short_name_b) == 0) {
                self->jack_schedule_connect(port_name_a, jack_port_name(self->jack_playback_ports[ch]), connect);
                break;
            }
        }
    }

    if(connect) {
        std::fprintf(stderr, "%s =====> %s\n", port_name_a, port_name_b);
    } else {
        std::fprintf(stderr, "%s ==X==> %s\n", port_name_a, port_name_b);
    }

}

void JopaSession::jack_on_error(char const* reason) {
    std::fprintf(stderr, "JACK error: %s\n", reason);
}

void JopaSession::pulse_on_context_state(pa_context* c, void* userdata) {
    JopaSession* self = reinterpret_cast<JopaSession*>(userdata);

    switch(pa_context_get_state(c)) {
    case PA_CONTEXT_READY:
        break;
    case PA_CONTEXT_FAILED:
        pulse_throw_exception(c, "Unable to connect to the PulseAudio server");
        return;
    case PA_CONTEXT_TERMINATED:
        std::exit(0);
        return;
    default:
        return;
    }

    // Create streams
    pa_sample_spec sample_spec = self->pulse_calc_sample_spec();
    self->pulse_playback_stream = pa_stream_new(c, "JACK playback", &sample_spec, nullptr);
    if(self->pulse_playback_stream == nullptr) {
        pulse_throw_exception(c, "Unable to create a PulseAudio playback stream");
    }
    self->pulse_record_stream = pa_stream_new(c, "JACK record", &sample_spec, nullptr);
    if(self->pulse_record_stream == nullptr) {
        pulse_throw_exception(c, "Unable to create a PulseAudio playback stream");
    }
    self->pulse_monitor_stream = pa_stream_new(c, "JACK monitor", &sample_spec, nullptr);
    if(self->pulse_monitor_stream == nullptr) {
        pulse_throw_exception(c, "Unable to create a PulseAudio monitor stream");
    }

    // Set stream read/write callback
    pa_stream_set_write_callback(self->pulse_playback_stream, pulse_on_playback_writable, self);
    pa_stream_set_read_callback(self->pulse_record_stream, pulse_on_record_readable, self);
    pa_stream_set_read_callback(self->pulse_monitor_stream, pulse_on_monitor_readable, self);

    // A move operation resets the stream's buffer attributes
    // Use a callback to detect the change
    pa_stream_set_moved_callback(self->pulse_playback_stream, pulse_on_playback_stream_moved, self);
    pa_stream_set_moved_callback(self->pulse_record_stream, pulse_on_record_stream_moved, self);
    pa_stream_set_moved_callback(self->pulse_monitor_stream, pulse_on_record_stream_moved, self);

    // Connect play & record streams
    pa_buffer_attr playback_buffer_attr = self->pulse_calc_buffer_attr(false);
    pa_buffer_attr record_buffer_attr = self->pulse_calc_buffer_attr(true);
    if(pa_stream_connect_playback(self->pulse_playback_stream, nullptr, &playback_buffer_attr, (pa_stream_flags_t) (PA_STREAM_VARIABLE_RATE | PA_STREAM_ADJUST_LATENCY), nullptr, nullptr) < 0) {
        pulse_throw_exception(c, "Unable to connect to PulseAudio playback stream");
    }
    if(pa_stream_connect_record(self->pulse_record_stream, nullptr, &record_buffer_attr, (pa_stream_flags_t) (PA_STREAM_VARIABLE_RATE | PA_STREAM_ADJUST_LATENCY)) < 0) {
        pulse_throw_exception(c, "Unable to connect to PulseAudio record stream");
    }

    // Prepare monitor stream
    uint32_t play_device_index = pa_stream_get_device_index(self->pulse_playback_stream);
    if(!pulse_check_operation(pa_context_get_sink_info_by_index(c, play_device_index, pulse_on_get_sink_info, self))) {
        pulse_throw_exception(c, "Unable to query PulseAudio for sink information");
    }
}

void JopaSession::pulse_on_playback_writable(pa_stream* p, size_t nbytes, void* userdata) {
    JopaSession* self = reinterpret_cast<JopaSession*>(userdata);

    pulse_sample_t* data;
    size_t nbytes_readable = jack_ringbuffer_read_space(self->jack_playback_ringbuffer);
    size_t nbytes_writable = nbytes;
    if(pa_stream_begin_write(self->pulse_playback_stream, (void**) &data, &nbytes_writable) < 0) {
        pulse_throw_exception(self->pulse_context, "Unable to write to PulseAudio playback buffer");
    }
    if(nbytes_readable >= nbytes_writable) {
        jack_ringbuffer_read(self->jack_playback_ringbuffer, (char*) data, nbytes_writable);
    } else {
        std::memset(data, 0, nbytes_writable);
        std::fprintf(stderr, "Playback buffer underflow: %zu < %zu\n", nbytes_readable, nbytes_writable);
    }
    if(pa_stream_write(self->pulse_playback_stream, data, nbytes_writable, nullptr, 0, PA_SEEK_RELATIVE) < 0) {
        pulse_throw_exception(self->pulse_context, "Unable to write to PulseAudio playback buffer");
    }
}

void JopaSession::pulse_on_record_readable(pa_stream* p, size_t, void* userdata) {
    JopaSession* self = reinterpret_cast<JopaSession*>(userdata);

    while(pa_stream_readable_size(p) > 0) {
        pulse_sample_t const* data;
        size_t nbytes_readable;
        if(pa_stream_peek(p, (void const**) &data, &nbytes_readable) < 0) {
            pulse_throw_exception(self->pulse_context, "Unable to read from PulseAudio record buffer");
        }
        if(data != nullptr) {
            size_t nbytes_writable = jack_ringbuffer_write_space(self->jack_capture_ringbuffer);
            if(nbytes_writable >= nbytes_readable) {
                jack_ringbuffer_write(self->jack_capture_ringbuffer, (char const*) data, nbytes_readable);
            } else {
                std::fprintf(stderr, "Record buffer overflow: %zu < %zu\n", nbytes_writable, nbytes_readable);
            }
            if(pa_stream_drop(p) < 0) {
                pulse_throw_exception(self->pulse_context, "Unable to read from PulseAudio record buffer");
            }
        } else if(nbytes_readable != 0) {
            std::fprintf(stderr, "Record buffer overflow: %zu bytes hole\n", nbytes_readable);
            if(pa_stream_drop(p) < 0) {
                pulse_throw_exception(self->pulse_context, "Unable to read from PulseAudio record buffer");
            }
        }
    }
}

void JopaSession::pulse_on_monitor_readable(pa_stream* p, size_t, void* userdata) {
    JopaSession* self = reinterpret_cast<JopaSession*>(userdata);

    while(pa_stream_readable_size(p) > 0) {
        pulse_sample_t const* data;
        size_t nbytes_readable;
        if(pa_stream_peek(p, (void const**) &data, &nbytes_readable) < 0) {
            pulse_throw_exception(self->pulse_context, "Unable to read from PulseAudio monitor buffer");
        }
        if(data != nullptr) {
            size_t nbytes_writable = jack_ringbuffer_write_space(self->jack_monitor_ringbuffer);
            if(nbytes_writable >= nbytes_readable) {
                jack_ringbuffer_write(self->jack_monitor_ringbuffer, (char const*) data, nbytes_readable);
            } else {
                std::fprintf(stderr, "Monitor buffer overflow: %zu < %zu\n", nbytes_writable, nbytes_readable);
            }
            if(pa_stream_drop(p) < 0) {
                pulse_throw_exception(self->pulse_context, "Unable to read from PulseAudio monitor buffer");
            }
        } else if(nbytes_readable != 0) {
            std::fprintf(stderr, "Monitor buffer overflow: %zu bytes hole\n", nbytes_readable);
            if(pa_stream_drop(p) < 0) {
                pulse_throw_exception(self->pulse_context, "Unable to read from PulseAudio monitor buffer");
            }
        }
    }
}

void JopaSession::pulse_on_playback_stream_moved(pa_stream* p, void* userdata) {
    JopaSession* self = reinterpret_cast<JopaSession*>(userdata);

    // Reset buffer attributes
    pa_buffer_attr playback_buffer_attr = self->pulse_calc_buffer_attr(false);
    if(pulse_is_stream_ready(p)) {
        if(!pulse_check_operation(pa_stream_set_buffer_attr(p, &playback_buffer_attr, nullptr, nullptr))) {
            pulse_throw_exception(self->pulse_context, "Unable to reset PulseAudio playback buffer");
        }
    }
}

void JopaSession::pulse_on_record_stream_moved(pa_stream* p, void* userdata) {
    JopaSession* self = reinterpret_cast<JopaSession*>(userdata);

    // Reset buffer attributes
    pa_buffer_attr record_buffer_attr = self->pulse_calc_buffer_attr(true);
    if(pulse_is_stream_ready(p)) {
        if(!pulse_check_operation(pa_stream_set_buffer_attr(p, &record_buffer_attr, nullptr, nullptr))) {
            pulse_throw_exception(self->pulse_context, "Unable to reset PulseAudio record / monitor buffer");
        }
    }
}

void JopaSession::pulse_on_get_sink_info(pa_context* c, pa_sink_info const* i, int eol, void* userdata) {
    JopaSession* self = reinterpret_cast<JopaSession*>(userdata);

    if(eol < 0) {
        pulse_throw_exception(c, "Unable to get PulseAudio sink info");
    }
    if(i == nullptr) {
        return;
    }

    // Connect monitor stream
    pa_buffer_attr monitor_buffer_attr = self->pulse_calc_buffer_attr(true);
    if(pa_stream_connect_record(self->pulse_monitor_stream, i->monitor_source_name, &monitor_buffer_attr, (pa_stream_flags_t) (PA_STREAM_VARIABLE_RATE | PA_STREAM_ADJUST_LATENCY)) < 0) {
        pulse_throw_exception(c, "Unable to connect to PulseAudio monitor stream");
    }
}

void JopaSession::jack_schedule_connect(char const* port_name_a, char const* port_name_b, bool connect) {
    JackConnectOperation operation = {
        .port_name_a = port_name_a,
        .port_name_b = port_name_b,
        .connect = connect
    };
    jack_connect_operations.push(std::move(operation));
}

void JopaSession::jack_finish_connect() {
    while(!jack_connect_operations.empty()) {
        JackConnectOperation const& operation = jack_connect_operations.front();
        if(operation.connect) {
            jack_connect(jack_client, operation.port_name_a.c_str(), operation.port_name_b.c_str());
        } else {
            jack_disconnect(jack_client, operation.port_name_a.c_str(), operation.port_name_b.c_str());
        }
        jack_connect_operations.pop();
    }
}

bool JopaSession::pulse_is_stream_ready(pa_stream* p) {
    return p != nullptr && pa_stream_get_state(p) == PA_STREAM_READY;
}

bool JopaSession::pulse_check_operation(pa_operation* o) {
    if(o == nullptr) {
        return false;
    } else {
        pa_operation_unref(o);
        return true;
    }
}

void JopaSession::pulse_throw_exception(pa_context* c, char const* reason) {
    std::string message(reason);
    message += ": ";
    message += pa_strerror(pa_context_errno(c));
    throw std::runtime_error(message.c_str());
}

pa_sample_spec JopaSession::pulse_calc_sample_spec() const {
    pa_sample_spec sample_spec = {
        .format   = PA_SAMPLE_FLOAT32NE,
        .rate     = sample_rate,
        .channels = num_channels
    };
    return sample_spec;
}

pa_buffer_attr JopaSession::pulse_calc_buffer_attr(bool record) const {
    pa_buffer_attr buffer_attr = {
        .maxlength = (uint32_t) -1,
        .tlength   = record ? (uint32_t) -1 : (uint32_t) (jack_buffer_size * (num_channels * sizeof (pulse_sample_t))),
        .prebuf    = (uint32_t) -1,
        .minreq    = (uint32_t) -1,
        .fragsize  = record ? (uint32_t) (jack_buffer_size * (num_channels * sizeof (pulse_sample_t))) : (uint32_t) -1
    };
    return buffer_attr;
}

JopaSession::PulseThreadedMainloopLocker::PulseThreadedMainloopLocker(pa_threaded_mainloop* mainloop) {
    this->mainloop = mainloop;
    if(mainloop) {
        pa_threaded_mainloop_lock(mainloop);
    }
}

JopaSession::PulseThreadedMainloopLocker::~PulseThreadedMainloopLocker() {
    if(mainloop) {
        pa_threaded_mainloop_unlock(mainloop);
    }
}
