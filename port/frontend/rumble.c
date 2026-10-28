// Rumble: what happens to Wario, as the game reports it (port/src/rumble_snapshot.c reads it
// from the game state after each frame), turned into short effects for the gamepad motors
// and pulses for the device's vibrator. Strengths follow the Windows/Linux port's effects.
#include "port.h"

#include "port/rumble_snapshot.h"

// One effect: motor levels (0..1) for a number of frames.
typedef struct Effect {
    float low, high;
    int frames;
} Effect;

static Effect sCurrent;
static int sCooldown;     // frames before another ordinary effect may start
static int sValid, sHealth, sShakeX, sShakeY;

static void Start(float low, float high, int frames, int cooldown)
{
    sCurrent.low = low;
    sCurrent.high = high;
    sCurrent.frames = frames;
    sCooldown = cooldown;
    Port_HostRumble(low, high, low > high ? low : high, frames * 1000 / 60);
}

void Port_RumbleReset(void)
{
    sCurrent.frames = 0;
    sCooldown = 0;
    sValid = 0;
    Port_HostRumble(0.0f, 0.0f, 0.0f, 0);
}

void Port_RumbleFrame(void)
{
    PortRumbleSnapshot s;
    int damage, impact;

    PortRumbleRead(&s);
    if (!s.active) {
        if (sValid || sCurrent.frames > 0)
            Port_RumbleReset();
        return;
    }
    damage = sValid && s.health < sHealth;
    impact = sValid && (s.shakeX > sShakeX || s.shakeY > sShakeY);
    sHealth = s.health;
    sShakeX = s.shakeX;
    sShakeY = s.shakeY;
    sValid = 1;
    if (sCooldown > 0)
        sCooldown--;

    if (s.events & PORT_RUMBLE_FROG) {
        // The frog switch: the escape timer starts. A long rumble, like the screen shaking.
        Start(0.43f, 0.18f, 300, 300);
    } else if (s.events & (PORT_RUMBLE_TRANSFORM | PORT_RUMBLE_FIRE | PORT_RUMBLE_ASH | PORT_RUMBLE_SNOW)) {
        Start(0.3f, 0.3f, 12, 11);
    } else if (s.events & PORT_RUMBLE_RECOVER) {
        Start(0.14f, 0.21f, 4, 11);
    } else if (s.events & PORT_RUMBLE_FORM_IMPACT) {
        Start(0.3f, 0.18f, 5, 11);
    } else if (sCooldown == 0 && (damage || impact || s.rock || s.boss || s.events)) {
        if (damage)
            Start(0.4f, 0.73f, 7, 11);
        else if (s.boss)
            Start(0.64f, 0.52f, 8, 11);
        else if (s.events & PORT_RUMBLE_HEAD)
            Start(0.46f, 0.4f, 6, 11);
        else if (s.rock)
            Start(0.58f, 0.37f, 5, 11);
        else if (s.events)
            Start(0.67f, 0.4f, 6, 11);
        else
            Start(0.67f, 0.24f, 10, 11);
    }

    if (sCurrent.frames > 0) {
        sCurrent.frames--;
        Port_HostRumble(sCurrent.low, sCurrent.high, 0.0f, 0);
    } else {
        Port_HostRumble(0.0f, 0.0f, 0.0f, 0);
    }
}
