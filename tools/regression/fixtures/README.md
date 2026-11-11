# Synthetic audio fixtures

These files contain generated sine waves, with no game or mod audio:

- tone.ogg: 500 Hz, 24 kHz mono, 6,000 frames (0.25 seconds).
- tone-alt.ogg: 730 Hz, same duration and format.
- tone-long.ogg: 500 Hz, 768,000 frames (32 seconds); 1,536,000 decoded
  Int16 bytes exceed the precache lookahead's 1 MiB PCM cap.

They are provided under the repository's license. Test runs use these committed
files and need no encoder. Optional regeneration uses make_tone.py and a
locally installed libsndfile with Vorbis support:

~~~sh
python3 tools/regression/fixtures/make_tone.py \
  --library /path/to/libsndfile --output /tmp/tone.ogg
~~~

Use --frequency 730 for the alternate tone and --frames 768000 for the long
one. Encoder versions may change container bytes; record fixture hashes.
