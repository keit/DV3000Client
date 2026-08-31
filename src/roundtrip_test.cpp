// Step 1 of the ThumbDV/XLX client build order: verify PCM -> AMBE -> PCM
// round-tripping through a real ThumbDV, standalone, before any networking.
//
// Usage: roundtrip_test <tty device> <input raw 8kHz/16-bit-LE PCM> <output raw PCM>

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>

#include "dvcontroller.h"

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s <tty device> <input.raw> <output.raw>\n", argv[0]);
        return 1;
    }

    const char *device = argv[1];
    const char *inPath = argv[2];
    const char *outPath = argv[3];

    SerialDV::DVController dv;
    if (!dv.open(device)) {
        fprintf(stderr, "failed to open %s\n", device);
        return 1;
    }

    FILE *in = fopen(inPath, "rb");
    FILE *out = fopen(outPath, "wb");
    if (!in || !out) {
        fprintf(stderr, "failed to open input/output files\n");
        return 1;
    }

    short audioIn[SerialDV::MBE_AUDIO_BLOCK_SIZE];
    short audioOut[SerialDV::MBE_AUDIO_BLOCK_SIZE];
    unsigned char ambe[SerialDV::MBE_FRAME_MAX_LENGTH_BYTES];

    unsigned int frames = 0;
    double sumSqOrig = 0.0, sumSqOut = 0.0;

    while (fread(audioIn, sizeof(short), SerialDV::MBE_AUDIO_BLOCK_SIZE, in)
           == SerialDV::MBE_AUDIO_BLOCK_SIZE) {
        if (!dv.encode(audioIn, ambe, SerialDV::DVRate3600x2400)) {
            fprintf(stderr, "encode failed at frame %u\n", frames);
            return 1;
        }
        if (!dv.decode(audioOut, ambe, SerialDV::DVRate3600x2400)) {
            fprintf(stderr, "decode failed at frame %u\n", frames);
            return 1;
        }
        fwrite(audioOut, sizeof(short), SerialDV::MBE_AUDIO_BLOCK_SIZE, out);

        for (unsigned int i = 0; i < SerialDV::MBE_AUDIO_BLOCK_SIZE; i++) {
            sumSqOrig += double(audioIn[i]) * audioIn[i];
            sumSqOut += double(audioOut[i]) * audioOut[i];
        }
        frames++;
    }

    fclose(in);
    fclose(out);
    dv.close();

    unsigned int totalSamples = frames * SerialDV::MBE_AUDIO_BLOCK_SIZE;
    fprintf(stderr, "frames=%u rms_in=%.1f rms_out=%.1f\n", frames,
            sqrt(sumSqOrig / totalSamples), sqrt(sumSqOut / totalSamples));

    return 0;
}
