#pragma once

/** Reference-audio analyser: `BitrotTest analyze <in.wav> <outPrefix> [loss speed mode weighting]`
    Writes spectrogram PNGs, optionally renders the file through the plugin, and prints stats. */
int runAnalyzer (int argc, char** argv);
