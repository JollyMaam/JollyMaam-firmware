/* sdlpad: what SDL makes of the gamepads it sees.
 *
 *   nix shell nixpkgs#sdl3 nixpkgs#gcc nixpkgs#pkg-config -c sh -c \
 *     'gcc -O2 -o build/host/sdlpad tools/sdlpad.c $(pkg-config --cflags --libs sdl3)'
 *   build/host/sdlpad [SECONDS]
 *
 * For each gamepad: name, type, driver path, VID:PID, then once a second the
 * buttons held, the axes, how many reports and sensor samples arrived, and
 * the newest gyro and accelerometer.  It rumbles for half a second, sets the
 * player LED/lightbar where the pad has one, and enables both sensors.
 * Exit status 0 when at least one gamepad sent input. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX 8

struct seen {
    SDL_JoystickID id;
    SDL_Gamepad *pad;
    unsigned updates, gyro, accel;
    float g[3], a[3];
};

static struct seen pads[MAX];
static int count;

static struct seen *find(SDL_JoystickID id)
{
    for (int i = 0; i < count; i++)
        if (pads[i].id == id) return &pads[i];
    return NULL;
}

static void add(SDL_JoystickID id)
{
    struct seen *s;
    SDL_Gamepad *g;

    if (find(id) || count == MAX) return;
    g = SDL_OpenGamepad(id);
    if (!g) {
        printf("open %u: %s\n", (unsigned)id, SDL_GetError());
        return;
    }
    s = &pads[count++];
    SDL_memset(s, 0, sizeof(*s));
    s->id = id;
    s->pad = g;
    printf("pad %u: \"%s\" type %s vid:pid %04x:%04x path %s\n", (unsigned)id, SDL_GetGamepadName(g),
           SDL_GetGamepadStringForType(SDL_GetGamepadType(g)), SDL_GetGamepadVendor(g),
           SDL_GetGamepadProduct(g), SDL_GetGamepadPath(g) ? SDL_GetGamepadPath(g) : "-");
    printf("  sensors: gyro %d (%.0f Hz) accel %d (%.0f Hz); rumble %s; mapping %s\n",
           SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO), SDL_GetGamepadSensorDataRate(g, SDL_SENSOR_GYRO),
           SDL_GamepadHasSensor(g, SDL_SENSOR_ACCEL), SDL_GetGamepadSensorDataRate(g, SDL_SENSOR_ACCEL),
           SDL_GetBooleanProperty(SDL_GetGamepadProperties(g), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false) ? "yes" : "no",
           SDL_GetGamepadMapping(g) ? "yes" : "no");
    SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_GYRO, true);
    SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_ACCEL, true);
    SDL_SetGamepadPlayerIndex(g, 1);
    SDL_SetGamepadLED(g, 0, 0, 255);
    if (!SDL_RumbleGamepad(g, 0x8000, 0x8000, 500)) printf("  rumble: %s\n", SDL_GetError());
}

static void show(struct seen *s)
{
    SDL_Gamepad *g = s->pad;
    char held[256] = "";

    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
        if (SDL_GetGamepadButton(g, (SDL_GamepadButton)b)) {
            SDL_strlcat(held, SDL_GetGamepadStringForButton((SDL_GamepadButton)b), sizeof(held));
            SDL_strlcat(held, " ", sizeof(held));
        }
    printf("pad %u: %u updates, %u gyro, %u accel | lx %6d ly %6d rx %6d ry %6d lt %5d rt %5d | "
           "gyro %6.2f %6.2f %6.2f accel %6.2f %6.2f %6.2f | %s\n",
           (unsigned)s->id, s->updates, s->gyro, s->accel,
           SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY),
           SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX), SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY),
           SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER), SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER),
           s->g[0], s->g[1], s->g[2], s->a[0], s->a[1], s->a[2], held[0] ? held : "-");
    s->updates = s->gyro = s->accel = 0;
}

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 10, any = 0;
    Uint64 end, next;
    SDL_Event e;

    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        printf("SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
    end = SDL_GetTicks() + (Uint64)seconds * 1000;
    next = SDL_GetTicks() + 1000;
    while (SDL_GetTicks() < end) {
        while (SDL_PollEvent(&e)) {
            struct seen *s;

            switch (e.type) {
            case SDL_EVENT_GAMEPAD_ADDED: add(e.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: printf("pad %u removed\n", (unsigned)e.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_UPDATE_COMPLETE:
                if ((s = find(e.gdevice.which))) { s->updates++; any = 1; }
                break;
            case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
                if (!(s = find(e.gsensor.which))) break;
                if (e.gsensor.sensor == SDL_SENSOR_GYRO) { s->gyro++; SDL_memcpy(s->g, e.gsensor.data, sizeof(s->g)); }
                if (e.gsensor.sensor == SDL_SENSOR_ACCEL) { s->accel++; SDL_memcpy(s->a, e.gsensor.data, sizeof(s->a)); }
                break;
            }
        }
        if (SDL_GetTicks() >= next) {
            next += 1000;
            for (int i = 0; i < count; i++) show(&pads[i]);
        }
        SDL_Delay(1);
    }
    if (!count) printf("no gamepad\n");
    SDL_Quit();
    return any ? 0 : 1;
}
