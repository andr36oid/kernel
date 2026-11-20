/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The capture policy is shared by open, manual calibration and delayed work. */
#ifndef A36_JOYPAD_CENTRE_H
#define A36_JOYPAD_CENTRE_H
#define A36_CENTRE_DISCARD 4
#define A36_CENTRE_SAMPLES 9
struct a36_centre_sample { int first, median, span; };
static int a36_sample_centre(void *context, int (*read)(void *),
                            void (*pause)(void *), struct a36_centre_sample *out)
{
        int samples[A36_CENTRE_SAMPLES], i, j, value;
        out->first = 0;
        for (i = 0; i < A36_CENTRE_DISCARD; i++) {
                value = read(context);
                if (!i)
                        out->first = value;
        }
        for (i = 0; i < A36_CENTRE_SAMPLES; i++) {
                if (i)
                        pause(context);
                value = read(context);
                if (value <= 0)
                        return -1;
                for (j = i; j && samples[j - 1] > value; j--)
                        samples[j] = samples[j - 1];
                samples[j] = value;
        }
        out->median = samples[A36_CENTRE_SAMPLES / 2];
        out->span = samples[A36_CENTRE_SAMPLES - 1] - samples[0];
        return 0;
}
#endif
