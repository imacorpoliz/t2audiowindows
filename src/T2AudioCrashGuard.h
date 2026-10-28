#ifndef T2AUDIO_CRASH_GUARD_H
#define T2AUDIO_CRASH_GUARD_H

// Crash guard for the persistent ("production") mode, Parameters\EnableProduction=1.
// Pure decision logic, shared with tests/T2AudioCrashGuardTest.c.
//
// The driver writes Parameters\CrashGuardArmed=1 (and flushes the key)
// before it starts the hardware, and writes 0 when the session ends cleanly:
// device stop/removal, power-down to D1..D3 (idle, sleep, hibernate) and
// system shutdown. If a start finds the flag still set, the previous session
// ended without any of those - a bugcheck, a hang or a hard power-off - and
// that counts as a strike. After T2AUDIO_CRASHGUARD_MAX_STRIKES strikes in a
// row the driver refuses to start until the installer resets the counter; a
// clean end of session resets it to 0.

#define T2AUDIO_CRASHGUARD_MAX_STRIKES 2u

// Returns 1 if the device may start; *NewStrikes is the counter to store.
static __inline int
T2AudioCrashGuardAllowStart(unsigned int Armed, unsigned int Strikes, unsigned int *NewStrikes)
{
    unsigned int s = Strikes;

    if (Armed != 0 && s < 0xFFFFu) {
        s++;
    }
    if (NewStrikes != 0) {
        *NewStrikes = s;
    }
    return s < T2AUDIO_CRASHGUARD_MAX_STRIKES;
}

#endif
