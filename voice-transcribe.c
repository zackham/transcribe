/*
 * Voice Transcription with Background Recording
 * Runs in background without stealing focus
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <alsa/asoundlib.h>
#include <curl/curl.h>
#include <math.h>

#define SAMPLE_RATE 16000
#define CHANNELS 1
#define BUFFER_SIZE 4096
#define MAX_RECORDING_TIME 300
#define PIDFILE "/tmp/voice_transcribe.pid"
#define STATUSFILE "/tmp/voice_transcribe.status"

typedef struct {
    void *data;
    size_t size;
    size_t capacity;
} AudioBuffer;

typedef struct {
    char *data;
    size_t size;
} CurlResponse;

static AudioBuffer g_audio_buffer = {0};
static pthread_t g_record_thread;
static pthread_t g_monitor_thread;
static volatile int g_stop_recording = 0;
static char *g_api_key = NULL;
static time_t g_record_start_time;
static FILE *g_status_file = NULL;
static float g_current_level = 0.0f;

// CURL callback
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    CurlResponse *resp = (CurlResponse *)userp;

    char *ptr = realloc(resp->data, resp->size + realsize + 1);
    if (!ptr) return 0;

    resp->data = ptr;
    memcpy(&(resp->data[resp->size]), contents, realsize);
    resp->size += realsize;
    resp->data[resp->size] = 0;

    return realsize;
}

// Audio buffer functions
static void init_audio_buffer(AudioBuffer *buf, size_t initial_size) {
    buf->data = malloc(initial_size);
    buf->size = 0;
    buf->capacity = initial_size;
}

static void append_audio_buffer(AudioBuffer *buf, const void *data, size_t size) {
    if (buf->size + size > buf->capacity) {
        size_t new_capacity = buf->capacity * 2;
        while (new_capacity < buf->size + size) {
            new_capacity *= 2;
        }
        void *new_data = realloc(buf->data, new_capacity);
        if (!new_data) return;
        buf->data = new_data;
        buf->capacity = new_capacity;
    }
    memcpy((char*)buf->data + buf->size, data, size);
    buf->size += size;
}

static void free_audio_buffer(AudioBuffer *buf) {
    free(buf->data);
    buf->data = NULL;
    buf->size = 0;
    buf->capacity = 0;
}

// Update status file
static void update_status(const char *status, float level) {
    if (g_status_file) {
        fseek(g_status_file, 0, SEEK_SET);
        time_t elapsed = time(NULL) - g_record_start_time;
        fprintf(g_status_file, "%s|%.2f|%02ld:%02ld\n", status, level, elapsed / 60, elapsed % 60);
        ftruncate(fileno(g_status_file), ftell(g_status_file)); // Truncate any leftover content
        fflush(g_status_file);
    }
}

// Monitor thread - creates a visual indicator using waybar or similar
static void *monitor_thread(void *arg) {
    // Create a simple working Python visualization script
    const char *viz_script =
        "#!/usr/bin/env python3\n"
        "import gi\n"
        "gi.require_version('Gtk', '3.0')\n"
        "from gi.repository import Gtk, Gdk, GLib\n"
        "import cairo\n"
        "import math, time, os\n"
        "\n"
        "class AudioVisualizer(Gtk.Window):\n"
        "    def __init__(self):\n"
        "        super().__init__()\n"
        "        self.set_title('Recording')\n"
        "        self.set_type_hint(Gdk.WindowTypeHint.NOTIFICATION)\n"
        "        self.set_default_size(400, 100)\n"
        "        self.set_decorated(False)\n"
        "        self.set_keep_above(True)\n"
        "        self.set_app_paintable(True)\n"
        "        screen = self.get_screen()\n"
        "        visual = screen.get_rgba_visual()\n"
        "        if visual: self.set_visual(visual)\n"
        "        \n"
        "        # Center on screen\n"
        "        self.set_position(Gtk.WindowPosition.CENTER)\n"
        "        self.stick()  # Show on all workspaces\n"
        "        self.set_skip_taskbar_hint(True)\n"
        "        self.set_skip_pager_hint(True)\n"
        "        \n"
        "        # Make click-through and non-focusable\n"
        "        self.set_events(0)\n"
        "        self.input_shape_combine_region(None)\n"
        "        self.set_accept_focus(False)\n"
        "        self.set_can_focus(False)\n"
        "        \n"
        "        self.drawing_area = Gtk.DrawingArea()\n"
        "        self.drawing_area.connect('draw', self.on_draw)\n"
        "        self.add(self.drawing_area)\n"
        "        \n"
        "        self.level = 0.0\n"
        "        self.time_str = '00:00'\n"
        "        self.status = 'RECORDING'\n"
        "        self.history = [0.0] * 60\n"
        "        \n"
        "        GLib.timeout_add(50, self.update_display)\n"
        "        self.show_all()\n"
        "    \n"
        "    def update_display(self):\n"
        "        try:\n"
        "            with open('/tmp/voice_transcribe.status', 'r') as f:\n"
        "                line = f.readline().strip()\n"
        "                if line:\n"
        "                    parts = line.split('|')\n"
        "                    self.status = parts[0]\n"
        "                    \n"
        "                    if self.status in ['COPIED', 'FAILED', 'NO_AUDIO']:\n"
        "                        if self.status == 'COPIED':\n"
        "                            GLib.timeout_add(1000, Gtk.main_quit)\n"
        "                        else:\n"
        "                            GLib.timeout_add(2000, Gtk.main_quit)\n"
        "                        return False\n"
        "                    \n"
        "                    if len(parts) > 1:\n"
        "                        self.level = float(parts[1])\n"
        "                    if len(parts) > 2:\n"
        "                        self.time_str = parts[2]\n"
        "                    \n"
        "                    self.history.append(self.level)\n"
        "                    self.history.pop(0)\n"
        "        except: pass\n"
        "        \n"
        "        self.drawing_area.queue_draw()\n"
        "        return True\n"
        "    \n"
        "    def on_draw(self, widget, cr):\n"
        "        width = widget.get_allocated_width()\n"
        "        height = widget.get_allocated_height()\n"
        "        \n"
        "        # Clear background properly\n"
        "        cr.set_operator(cairo.OPERATOR_SOURCE)\n"
        "        cr.set_source_rgba(0.1, 0.1, 0.2, 0.95)\n"
        "        cr.paint()\n"
        "        cr.set_operator(cairo.OPERATOR_OVER)\n"
        "        \n"
        "        # Draw border\n"
        "        cr.set_line_width(1)\n"
        "        cr.set_source_rgba(0.3, 0.3, 0.5, 0.5)\n"
        "        cr.rectangle(0.5, 0.5, width-1, height-1)\n"
        "        cr.stroke()\n"
        "        \n"
        "        # Draw waveform bars\n"
        "        bar_width = width / len(self.history)\n"
        "        for i, level in enumerate(self.history):\n"
        "            x = i * bar_width\n"
        "            bar_height = height * level * 0.7\n"
        "            y = (height - bar_height) / 2\n"
        "            \n"
        "            # Gradient based on level\n"
        "            bar_gradient = cairo.LinearGradient(x, y, x, y + bar_height)\n"
        "            if level > 0.7:\n"
        "                bar_gradient.add_color_stop_rgba(0, 1.0, 0.3, 0.3, 0.9)\n"
        "                bar_gradient.add_color_stop_rgba(1, 0.8, 0.1, 0.1, 0.7)\n"
        "            elif level > 0.4:\n"
        "                bar_gradient.add_color_stop_rgba(0, 0.9, 0.3, 1.0, 0.9)\n"
        "                bar_gradient.add_color_stop_rgba(1, 0.6, 0.1, 0.8, 0.7)\n"
        "            else:\n"
        "                bar_gradient.add_color_stop_rgba(0, 0.3, 0.7, 1.0, 0.9)\n"
        "                bar_gradient.add_color_stop_rgba(1, 0.1, 0.4, 0.8, 0.7)\n"
        "            \n"
        "            cr.set_source(bar_gradient)\n"
        "            cr.rectangle(x + 1, y, bar_width - 2, bar_height)\n"
        "            cr.fill()\n"
        "        \n"
        "        # Draw status text with background\n"
        "        cr.select_font_face('Sans')\n"
        "        \n"
        "        # Status message in center-bottom\n"
        "        status_text = {\n"
        "            'CONNECTING': 'Connecting to microphone...',\n"
        "            'READY': 'Microphone ready',\n"
        "            'RECORDING': 'Recording...',\n"
        "            'PROCESSING': 'Processing...',\n"
        "            'UPLOADING': 'Uploading to OpenAI...',\n"
        "            'COPIED': 'Copied to clipboard!',\n"
        "            'FAILED': 'Transcription failed',\n"
        "            'NO_AUDIO': 'No audio recorded',\n"
        "            'MAX_TIME': 'Max time reached',\n"
        "            'ERROR': 'Error occurred'\n"
        "        }.get(self.status, self.status)\n"
        "        \n"
        "        cr.set_font_size(13)\n"
        "        text_extents = cr.text_extents(status_text)\n"
        "        text_x = (width - text_extents.width) / 2\n"
        "        text_y = height - 10\n"
        "        \n"
        "        # Text background\n"
        "        cr.set_source_rgba(0.0, 0.0, 0.0, 0.6)\n"
        "        cr.rectangle(text_x - 5, text_y - text_extents.height - 2, text_extents.width + 10, text_extents.height + 4)\n"
        "        cr.fill()\n"
        "        \n"
        "        # Status text\n"
        "        if self.status == 'COPIED':\n"
        "            cr.set_source_rgba(0.0, 1.0, 0.5, 1.0)\n"
        "        elif self.status in ['FAILED', 'ERROR']:\n"
        "            cr.set_source_rgba(1.0, 0.3, 0.3, 1.0)\n"
        "        elif self.status in ['UPLOADING', 'PROCESSING']:\n"
        "            cr.set_source_rgba(1.0, 0.8, 0.2, 1.0)\n"
        "        else:\n"
        "            cr.set_source_rgba(1.0, 1.0, 1.0, 0.9)\n"
        "        cr.move_to(text_x, text_y)\n"
        "        cr.show_text(status_text)\n"
        "        \n"
        "        # Time in top-right\n"
        "        if self.status == 'RECORDING':\n"
        "            cr.set_font_size(11)\n"
        "            cr.set_source_rgba(0.8, 0.8, 0.8, 0.7)\n"
        "            cr.move_to(width - 45, 15)\n"
        "            cr.show_text(self.time_str)\n"
        "            \n"
        "            # Recording dot animation\n"
        "            cr.set_source_rgba(1.0, 0.0, 0.0, 0.5 + 0.5 * math.sin(time.time() * 5))\n"
        "            cr.arc(15, 15, 4, 0, 2 * math.pi)\n"
        "            cr.fill()\n"
        "\n"
        "if os.path.exists('/tmp/voice_transcribe.status'):\n"
        "    window = AudioVisualizer()\n"
        "    window.connect('destroy', Gtk.main_quit)\n"
        "    Gtk.main()\n";

    // Write visualization script
    FILE *script_file = fopen("/tmp/voice_viz.py", "w");
    if (script_file) {
        fprintf(script_file, "%s", viz_script);
        fclose(script_file);
        chmod("/tmp/voice_viz.py", 0755);

        // Launch the visualizer in background with low priority
        system("nice -n 10 python3 /tmp/voice_viz.py > /dev/null 2>&1 &");
    }

    // Keep updating status while recording or processing
    while (!g_stop_recording) {
        update_status("RECORDING", g_current_level);
        usleep(50000); // 20 FPS
    }

    // Don't immediately stop - wait for final status
    usleep(100000);
    unlink("/tmp/voice_viz.py");

    return NULL;
}

// Recording thread
static void *recording_thread(void *arg) {
    snd_pcm_t *capture_handle;
    snd_pcm_hw_params_t *hw_params;
    int err;
    short buffer[BUFFER_SIZE];

    // Try direct hardware access first for faster startup, fallback to default
    if ((err = snd_pcm_open(&capture_handle, "plughw:0,0", SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK)) < 0) {
        // Fallback to default (might be PulseAudio)
        if ((err = snd_pcm_open(&capture_handle, "default", SND_PCM_STREAM_CAPTURE, 0)) < 0) {
            fprintf(stderr, "Cannot open audio device: %s\n", snd_strerror(err));
            update_status("ERROR: Audio device failed", 0.0);
            return NULL;
        }
    }

    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(capture_handle, hw_params);
    snd_pcm_hw_params_set_access(capture_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(capture_handle, hw_params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_rate(capture_handle, hw_params, SAMPLE_RATE, 0);
    snd_pcm_hw_params_set_channels(capture_handle, hw_params, CHANNELS);

    if ((err = snd_pcm_hw_params(capture_handle, hw_params)) < 0) {
        fprintf(stderr, "Cannot set hardware parameters: %s\n", snd_strerror(err));
        snd_pcm_close(capture_handle);
        return NULL;
    }

    snd_pcm_prepare(capture_handle);

    // Initialize buffer BEFORE any delays
    init_audio_buffer(&g_audio_buffer, SAMPLE_RATE * 2 * 10);

    // Signal that mic is ready
    update_status("READY", 0.0);
    usleep(200000); // Brief pause to show ready status
    update_status("RECORDING", 0.0);

    // Start recording immediately
    while (!g_stop_recording) {
        // Check timeout
        if (time(NULL) - g_record_start_time > MAX_RECORDING_TIME) {
            update_status("MAX_TIME", 0.0);
            break;
        }

        int frames = snd_pcm_readi(capture_handle, buffer, BUFFER_SIZE);
        if (frames < 0) {
            frames = snd_pcm_recover(capture_handle, frames, 0);
        }
        if (frames > 0) {
            append_audio_buffer(&g_audio_buffer, buffer, frames * 2);

            // Calculate current audio level
            float max_amp = 0.0f;
            for (int i = 0; i < frames; i++) {
                float amp = fabsf((float)buffer[i] / 32768.0f);
                if (amp > max_amp) max_amp = amp;
            }
            g_current_level = max_amp;
        }
    }

    snd_pcm_close(capture_handle);
    return NULL;
}

// Transcribe audio
static int transcribe_audio(const void *audio_data, size_t audio_size, char **result) {
    CURL *curl;
    CURLcode res;
    struct curl_httppost *formpost = NULL;
    struct curl_httppost *lastptr = NULL;
    CurlResponse response = {0};

    // Create WAV file
    char temp_filename[] = "/tmp/audio_XXXXXX.wav";
    int fd = mkstemps(temp_filename, 4);
    if (fd < 0) return -1;

    // WAV header
    struct {
        char riff[4];
        uint32_t size;
        char wave[4];
        char fmt[4];
        uint32_t fmt_size;
        uint16_t format;
        uint16_t channels;
        uint32_t sample_rate;
        uint32_t byte_rate;
        uint16_t block_align;
        uint16_t bits_per_sample;
        char data[4];
        uint32_t data_size;
    } __attribute__((packed)) wav_header = {
        .riff = {'R', 'I', 'F', 'F'},
        .size = 36 + audio_size,
        .wave = {'W', 'A', 'V', 'E'},
        .fmt = {'f', 'm', 't', ' '},
        .fmt_size = 16,
        .format = 1,
        .channels = CHANNELS,
        .sample_rate = SAMPLE_RATE,
        .byte_rate = SAMPLE_RATE * CHANNELS * 2,
        .block_align = CHANNELS * 2,
        .bits_per_sample = 16,
        .data = {'d', 'a', 't', 'a'},
        .data_size = audio_size
    };

    write(fd, &wav_header, sizeof(wav_header));
    write(fd, audio_data, audio_size);
    close(fd);

    curl = curl_easy_init();
    if (!curl) {
        unlink(temp_filename);
        return -1;
    }

    // Build form
    curl_formadd(&formpost, &lastptr,
                 CURLFORM_COPYNAME, "file",
                 CURLFORM_FILE, temp_filename,
                 CURLFORM_END);

    curl_formadd(&formpost, &lastptr,
                 CURLFORM_COPYNAME, "model",
                 CURLFORM_COPYCONTENTS, "whisper-1",
                 CURLFORM_END);

    // Setup CURL
    struct curl_slist *headers = NULL;
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", g_api_key);
    headers = curl_slist_append(headers, auth_header);

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.openai.com/v1/audio/transcriptions");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_HTTPPOST, formpost);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    res = curl_easy_perform(curl);

    curl_formfree(formpost);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    unlink(temp_filename);

    if (res != CURLE_OK) {
        fprintf(stderr, "CURL error: %s\n", curl_easy_strerror(res));
        free(response.data);
        return -1;
    }

    // Parse JSON response
    char *text_start = strstr(response.data, "\"text\":\"");
    if (text_start) {
        text_start += 8;
        char *text_end = strchr(text_start, '"');
        if (text_end) {
            size_t text_len = text_end - text_start;
            *result = malloc(text_len + 1);
            memcpy(*result, text_start, text_len);
            (*result)[text_len] = '\0';

            // Unescape JSON
            char *src = *result, *dst = *result;
            while (*src) {
                if (*src == '\\' && *(src + 1)) {
                    src++;
                    switch (*src) {
                        case 'n': *dst++ = '\n'; break;
                        case 't': *dst++ = '\t'; break;
                        case 'r': *dst++ = '\r'; break;
                        case '"': *dst++ = '"'; break;
                        case '\\': *dst++ = '\\'; break;
                        default: *dst++ = *src; break;
                    }
                    src++;
                } else {
                    *dst++ = *src++;
                }
            }
            *dst = '\0';
        }
    }

    free(response.data);
    return *result ? 0 : -1;
}

// Removed - not inserting text anymore

// Copy to clipboard
static void copy_to_clipboard(const char *text) {
    FILE *pipe = popen("wl-copy", "w");
    if (pipe) {
        fprintf(pipe, "%s", text);
        pclose(pipe);
    }
}

// Load .env
static void load_env(void) {
    FILE *fp = fopen("/home/zack/work/transcribe/.env", "r");
    if (!fp) {
        fp = fopen(".env", "r");
    }
    if (!fp) {
        fprintf(stderr, "Cannot open .env file\n");
        return;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "OPENAI_API_KEY=", 15) == 0) {
            char *value = line + 15;
            char *end = strchr(value, '\n');
            if (end) *end = '\0';
            if (value[0] == '"' || value[0] == '\'') {
                value++;
                char *last = value + strlen(value) - 1;
                if (*last == '"' || *last == '\'') *last = '\0';
            }
            g_api_key = strdup(value);
            break;
        }
    }
    fclose(fp);
}

// Signal handler
static void signal_handler(int sig) {
    g_stop_recording = 1;
}

// Check if already running
static pid_t check_running() {
    FILE *pf = fopen(PIDFILE, "r");
    if (!pf) return 0;

    pid_t pid;
    fscanf(pf, "%d", &pid);
    fclose(pf);

    // Check if process exists
    if (kill(pid, 0) == 0) {
        return pid;
    }

    // Process doesn't exist, remove stale PID file
    unlink(PIDFILE);
    return 0;
}

int main(int argc, char **argv) {
    // Check if another instance is running
    pid_t existing_pid = check_running();
    if (existing_pid > 0) {
        // Send signal to stop recording
        kill(existing_pid, SIGUSR1);
        return 0;
    }

    // Don't write PID yet - will do after fork

    // Create status file
    g_status_file = fopen(STATUSFILE, "w");
    if (!g_status_file) {
        fprintf(stderr, "Cannot create status file\n");
    }

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGUSR1, signal_handler);

    // Load API key
    load_env();
    if (!g_api_key) {
        fprintf(stderr, "OPENAI_API_KEY not found in .env\n");
        unlink(PIDFILE);
        if (g_status_file) fclose(g_status_file);
        return 1;
    }

    // Initialize CURL
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Fork to background
    pid_t child_pid = fork();
    if (child_pid > 0) {
        // Parent: write child's PID and exit
        FILE *pf = fopen(PIDFILE, "w");
        if (pf) {
            fprintf(pf, "%d\n", child_pid);
            fclose(pf);
        }
        printf("Recording started (PID: %d)\n", child_pid);
        return 0;
    } else if (child_pid < 0) {
        fprintf(stderr, "Fork failed\n");
        return 1;
    }

    // Child process continues
    setsid();  // Create new session

    // Redirect standard streams to /dev/null
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
    }

    // Initialize start time BEFORE threads start
    g_record_start_time = time(NULL);

    // Show connecting status
    update_status("CONNECTING", 0.0);

    // Start recording thread FIRST (no delay)
    pthread_create(&g_record_thread, NULL, recording_thread, NULL);

    // Then start monitor/visualization (can take time to start)
    pthread_create(&g_monitor_thread, NULL, monitor_thread, NULL);

    // Wait for recording thread
    pthread_join(g_record_thread, NULL);

    // Update status to show we're processing
    update_status("PROCESSING", 0.0);
    usleep(200000); // Give UI time to update

    g_stop_recording = 1; // Signal monitor thread to stop
    pthread_join(g_monitor_thread, NULL);

    // Process audio
    if (g_audio_buffer.size > 0) {
        update_status("UPLOADING", 0.0);
        usleep(200000); // Give UI time to update

        char *transcription = NULL;
        if (transcribe_audio(g_audio_buffer.data, g_audio_buffer.size, &transcription) == 0 && transcription) {
            copy_to_clipboard(transcription);
            update_status("COPIED", 0.0);
            usleep(1000000); // Show success for 1 second
            free(transcription);
        } else {
            update_status("FAILED", 0.0);
            usleep(2000000); // Show error for 2 seconds
        }
    } else {
        update_status("NO_AUDIO", 0.0);
        usleep(2000000);
    }

    // Cleanup
    free_audio_buffer(&g_audio_buffer);
    free(g_api_key);
    curl_global_cleanup();
    unlink(PIDFILE);
    if (g_status_file) {
        fclose(g_status_file);
        unlink(STATUSFILE);
    }

    return 0;
}