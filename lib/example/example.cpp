/*
Copyright (c) 2024-2026 Rupert Carmichael

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.
*/

#include <fstream>
#include <cstdio>
#include <cstdlib>

#include <SDL3/SDL.h>
#include <samplerate.h>

#include <bsnes.hpp>

#define SCREEN_WIDTH 256
#define SCREEN_HEIGHT 240
#define SAMPLERATE 48000
#define FRAMERATE 60 // Approximately 60Hz
#define CHANNELS 2

// SDL Audio
static SDL_AudioStream *audiostream = NULL;

// SDL Video
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static int rw = 0;
static int rh = 0;
static int pitch = 0;

// Video rendering
static uint32_t *vbuf = NULL;
static unsigned scale = 2;

// Audio buffers
static float inbuf[3200];
static float rsbuf[3200];
static int16_t outbuf[3200];

// Resampler
static SRC_STATE *srcstate = nullptr;
static SRC_DATA srcdata;

// Game related
static std::vector<uint8_t> game;
static std::string gamepath = "";
static std::string gamename = "";

// Data path for BML assets
static std::string datapath = "";

// Keep track of whether the emulator should be running or not
static int running = 1;

static unsigned buttons[5][12] = {{0}};
static unsigned bmap[] = {
    Bsnes::Input::Gamepad::Up, Bsnes::Input::Gamepad::Down,
    Bsnes::Input::Gamepad::Left, Bsnes::Input::Gamepad::Right,
    Bsnes::Input::Gamepad::Select, Bsnes::Input::Gamepad::Start,
    Bsnes::Input::Gamepad::A, Bsnes::Input::Gamepad::B,
    Bsnes::Input::Gamepad::X, Bsnes::Input::Gamepad::Y,
    Bsnes::Input::Gamepad::L, Bsnes::Input::Gamepad::R
};

// Handle SDL input events
static void input_handler(SDL_Event& event) {
    if (event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_KEY_DOWN) {
        switch (event.key.scancode) {
            case SDL_SCANCODE_ESCAPE:
                running = 0;
                break;
            case SDL_SCANCODE_F1:
                Bsnes::power();
                break;
            case SDL_SCANCODE_F2:
                Bsnes::reset();
                break;

            case SDL_SCANCODE_UP:
                buttons[0][0] = event.type == SDL_EVENT_KEY_DOWN;
                break;
            case SDL_SCANCODE_DOWN:
                buttons[0][1] = event.type == SDL_EVENT_KEY_DOWN;
                break;
            case SDL_SCANCODE_LEFT:
                buttons[0][2] = event.type == SDL_EVENT_KEY_DOWN;
                break;
            case SDL_SCANCODE_RIGHT:
                buttons[0][3] = event.type == SDL_EVENT_KEY_DOWN;
                break;

            case SDL_SCANCODE_RSHIFT:
                buttons[0][4] = event.type == SDL_EVENT_KEY_DOWN;
                break;
            case SDL_SCANCODE_RETURN:
                buttons[0][5] = event.type == SDL_EVENT_KEY_DOWN;
                break;

            case SDL_SCANCODE_X:
                buttons[0][6] = event.type == SDL_EVENT_KEY_DOWN;
                break;
            case SDL_SCANCODE_Z:
                buttons[0][7] = event.type == SDL_EVENT_KEY_DOWN;
                break;
            case SDL_SCANCODE_S:
                buttons[0][8] = event.type == SDL_EVENT_KEY_DOWN;
                break;
            case SDL_SCANCODE_A:
                buttons[0][9] = event.type == SDL_EVENT_KEY_DOWN;
                break;

            case SDL_SCANCODE_Q:
                buttons[0][10] = event.type == SDL_EVENT_KEY_DOWN;
                break;
            case SDL_SCANCODE_W:
                buttons[0][11] = event.type == SDL_EVENT_KEY_DOWN;
                break;
            default: break;
        }
    }
}

static void logCallback(void*, int level, std::string& text) {
    if (level)
        fprintf(stderr, "%s\n", text.c_str());
}

static bool fileOpenS(void*, std::string name, std::stringstream& ss) {
    std::string path = datapath + "/" + name;
    std::ifstream filestream(path, std::ios::in | std::ios::binary);

    if (!filestream.is_open()) {
        fprintf(stderr, "Failed to load file: %s\n", path.c_str());
        return false;
    }

    ss << filestream.rdbuf();
    filestream.close();

    return true;
}

static bool fileOpenV(void*, std::string name, std::vector<uint8_t>& v) {
    std::string path;

    if (name == "save.ram")
        path = std::string("./") + gamename + ".srm";
    else if (name == "time.rtc")
        path = std::string("./") + gamename + ".rtc";
    else
        return false;

    std::ifstream filestream(path, std::ios::in | std::ios::binary);
    if (!filestream.is_open()) {
        fprintf(stderr, "Failed to load file: %s\n", path.c_str());
        return false;
    }

    v = std::vector<uint8_t>((std::istreambuf_iterator<char>(filestream)),
        std::istreambuf_iterator<char>());
    filestream.close();

    fprintf(stderr, "Loaded %s (%ld bytes)\n", path.c_str(), v.size());

    return true;
}

static bool fileOpenMsu(void*, std::string, std::istream**) {
    return false;
}

static void fileWrite(void*, std::string name, const uint8_t *data,
    unsigned size) {

    std::string path;

    if (name == "save.ram")
        path = std::string("./") + gamename + ".srm";
    else if (name == "time.rtc")
        path = std::string("./") + gamename + ".rtc";

    if (path.empty()) {
        fprintf(stderr, "No file routines for: %s\n", name.c_str());
        return;
    }

    std::ofstream filestream(path, std::ios::out | std::ios::binary);
    if (filestream.is_open()) {
        filestream.write((const char*)data, size);
        filestream.close();
        fprintf(stderr, "File saved %s\n", path.c_str());
    }
    else {
        fprintf(stderr, "Failed to save file: %s\n", path.c_str());
    }
}

static bool loadRom(void*, unsigned id) {
    if (id == Bsnes::GameType::SuperFamicom) {
        if (game.size() < 0x8000) return false;
        Bsnes::setRomSuperFamicom(game, gamepath);
        return true;
    }
    return false;
}

static void videoFrame(const void*, unsigned w, unsigned h, unsigned p) {
    // Update renderer properties
    rw = w;
    rh = h;
    pitch = p;
}

static void audioFrame(const void*, size_t numsamps) {
    if (!numsamps)
        return;

    // Manage audio output queue size by resampling
    int qbytes = SDL_GetAudioStreamAvailable(audiostream);
    unsigned esamps = 4; // Up to 4 extra samples

    if (qbytes > (int)(SAMPLERATE * sizeof(int16_t) * CHANNELS))
        SDL_ClearAudioStream(audiostream);
    else if (qbytes < 6400)
        esamps = (qbytes / 1600);

    esamps = 4 - esamps;
    double ratio = (SAMPLERATE + (esamps * FRAMERATE)) / double(SAMPLERATE);

    // Perform resampling
    srcdata.src_ratio = ratio;
    srcdata.data_in = inbuf;
    srcdata.data_out = rsbuf;
    srcdata.input_frames = numsamps >> 1;
    srcdata.output_frames = (SAMPLERATE / FRAMERATE) + esamps;
    src_process(srcstate, &srcdata);
    src_float_to_short_array(rsbuf, outbuf, srcdata.output_frames_gen << 1);

    /* Multiply output_frames_gen by 4 because SDL_PutAudioStreamData wants
       the number in bytes, and there are two channels:
       2 channels * sizeof(int16_t) = 4
    */
    SDL_PutAudioStreamData(audiostream, outbuf,
        srcdata.output_frames_gen << 2);
}

static int pollGamepad(const void*, unsigned port, unsigned) {
    int b = 0;
    for (int i = 0; i < 12; ++i)
        if (buttons[port][i]) b |= bmap[i];
    return b;
}

int main (int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: ./%s [FILE]\n", argv[0]);
        exit(1);
    }

    // Find the relative path to the executable
    std::string relpath = argv[0];
    relpath = relpath.substr(0, relpath.find_last_of('/'));

    // Load the uncompressed game data into memory
    std::ifstream filestream(argv[1], std::ios::in | std::ios::binary);
    if (!filestream.is_open()) {
        fprintf(stderr, "Failed to open requested file\n");
        return 1;
    }

    game = std::vector<uint8_t>((std::istreambuf_iterator<char>(filestream)),
        std::istreambuf_iterator<char>());
    filestream.close();

    gamepath = argv[1];
    gamename = gamepath.substr(gamepath.find_last_of('/') + 1);
    gamename = gamename.substr(0, gamename.find_last_of('.'));

    // Set data path for BML assets
    std::ifstream bmlstream(relpath + "/boards.bml");
    if (bmlstream.is_open()) {
        datapath = relpath;
    }
    else {
        datapath = DATADIR;
        bmlstream = std::ifstream(datapath + "/boards.bml");
    }

    if (bmlstream.is_open()) {
        bmlstream.close();
    }
    else {
        fprintf(stderr, "Failed to deduce BML asset location\n");
        return 1;
    }

    // Allow joystick input when the window is not focused
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    // Keep window fullscreen if the window manager tries to iconify it
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");

    // Initialize SDL
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK)) {
        fprintf(stderr, "Failed to initialize SDL: %s\n", SDL_GetError());
        return EXIT_FAILURE;
    }

    // Set up the window
    SDL_WindowFlags windowflags = SDL_WINDOW_RESIZABLE |
        SDL_WINDOW_HIGH_PIXEL_DENSITY;

    int windowwidth = SCREEN_WIDTH * scale;
    int windowheight = SCREEN_HEIGHT * scale;

    window = SDL_CreateWindow("bsnes-example",
        windowwidth, windowheight, windowflags);

    renderer = SDL_CreateRenderer(window, NULL);
    SDL_SetRenderDrawColor(renderer, 0x0, 0x0, 0x0, 0xff);
    SDL_SetRenderLogicalPresentation(renderer,
        windowwidth, windowheight, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderVSync(renderer, 1);

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, SCREEN_WIDTH * 2, SCREEN_HEIGHT * 2);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);

    SDL_HideCursor();

    // Get current display mode
    const SDL_DisplayMode *dm =
        SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));

    // Allocate video buffer
    size_t vbufsize = SCREEN_WIDTH * (SCREEN_HEIGHT + 8) * 4 * sizeof(uint32_t);
    vbuf = (uint32_t*)calloc(vbufsize, 1);

    // Set up SDL Audio using AudioStream API
    const SDL_AudioSpec audiospec = { SDL_AUDIO_S16, CHANNELS, SAMPLERATE };
    audiostream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audiospec, NULL, NULL);

    // Set up resampling
    int err;
    memset(&srcdata, 0, sizeof(SRC_DATA));
    srcstate = src_new(SRC_SINC_FASTEST, 2, &err);
    src_reset(srcstate);

    // Allow audio playback
    SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(audiostream));

    // Set up the emulator
    Bsnes::setOpenFileCallback(nullptr, fileOpenV);
    Bsnes::setOpenStreamCallback(nullptr, fileOpenS);
    Bsnes::setOpenMsuCallback(nullptr, fileOpenMsu);
    Bsnes::setRomLoadCallback(nullptr, loadRom);
    Bsnes::setWriteCallback(nullptr, fileWrite);
    Bsnes::setLogCallback(nullptr, logCallback);

    Bsnes::setAudioSpec({double(SAMPLERATE), (SAMPLERATE / FRAMERATE) << 1, 0,
        inbuf, nullptr, &audioFrame});
    Bsnes::setVideoSpec({(uint32_t*)vbuf, nullptr, &videoFrame});

    if (!Bsnes::load())
      fprintf(stderr, "Failed to load ROM\n");
    Bsnes::power();

    Bsnes::setInputSpec({0, Bsnes::Input::Device::Gamepad,
        nullptr, pollGamepad});
    Bsnes::setInputSpec({1, Bsnes::Input::Device::Gamepad,
        nullptr, pollGamepad});

    int runframes = 0;
    int collector = 0;

    int refresh_rate = dm->refresh_rate + 0.5;

    SDL_Event event;

    while (running) {
        // Divide core framerate by screen framerate and collect remainders
        runframes = FRAMERATE / refresh_rate;
        collector += FRAMERATE % refresh_rate;

        if (collector >= refresh_rate) {
            ++runframes;
            collector -= refresh_rate;
        }

        for (int i = 0; i < runframes; ++i)
            Bsnes::run();

        SDL_FRect srcrect = {0, 0, (float)rw, (float)rh};
        SDL_RenderClear(renderer);
        SDL_UpdateTexture(texture, NULL, vbuf, pitch * sizeof(uint32_t));
        SDL_RenderTexture(renderer, texture, &srcrect, NULL);
        SDL_RenderPresent(renderer);

        // Poll for events
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT: {
                    running = 0;
                    break;
                }
                default: {
                    input_handler(event);
                    break;
                }
            }
        }
    }

    // Save SRAM and unload the game
    Bsnes::save();
    Bsnes::unload();

    // Bring down audio and video
    srcstate = src_delete(srcstate);
    srcstate = nullptr;

    if (audiostream)
        SDL_DestroyAudioStream(audiostream);

    if (vbuf)
        free(vbuf);

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    return 0;
}
