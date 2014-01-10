/*  trackcutter: Automatically splices multi-song analogue recordings
    Copyright (C) 2011-2014 Bryan Rodgers <rodgersb@it.net.au>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or (at
    your option) any later version.

    This program is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>. */

/** @file trackcutter.c

    Utility for automating the splicing of audio recordings that contain
    multiple songs concatenated together, into individual stand-alone
    audio files, one per track.

    Handy when digitising music from an analogue source (vinyl LP
    record, cassette tape, etc), so the user doesn't have to supervise
    and intervene during the capture; they can simply leave the capture
    process unattended and record into one big long file.

    Track delimeters are deemed to be passages of silence that are a
    minimum length. Silence is determined by a user-specified
    signal-to-noise ratio.

    Requires <a href="http://www.mega-nerd.com/libsndfile/">libsndfile</a> and
    and GNU libc (for getopt_long(), error() and program_invocation_short_name). */

#ifdef HAVE_CONFIG_H
#   include <config.h>
#endif

#include <features.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sndfile.h>
#include <getopt.h>
#include <libgen.h>
#include <errno.h>
#include <error.h>
#include <limits.h>
#include <math.h>
#include <ctype.h>
#include <stdarg.h>

/** Definition for boolean constant @e false */
#define FALSE 0
/** Definition for boolean constant @e true */
#define TRUE (!FALSE)

/** Window length (in milliseconds) for computing RMS volume */
#define RMS_WINDOW_PERIOD 50
/** Maximum number of channels this program is prepared to handle */
#define MAX_CHANNELS 8

/** Default minimum silence period between tracks (in milliseconds) */
#define DFL_MIN_SILENCE_PERIOD 2000
/** Default minimum non-silence period constituting start of new track (in milliseconds) */
#define DFL_MIN_SIGNAL_PERIOD 100
/** Default minimum length for a track; silence will only be tested for
    after this period (in seconds). */
#define DFL_MIN_TRACK_LENGTH 40
/** Default signal-to-noise ratio used to discriminate non-silence from silence (in dBFS) */
#define DFL_NOISE_FLOOR -48.0

/** Corner frequency for high-pass filter in Hz */
#define HIGH_PASS_CORNER_FREQ 20.0
/** Time constant for high-pass filter */
#define HIGH_PASS_TAU (1.0 / (2.0 * M_PI * HIGH_PASS_CORNER_FREQ))

/** Length of a timecode string buffer, in characters (incl. terminator) */
#define TIMECODE_STR_SZ 20

/** Main task descriptor */
typedef enum {
    TCT_CUTTING,         /**< Default mode, cutting up a recording. */
    TCT_ANALYSIS         /**< Analysis mode, print maximum/minimum amplitude. */
} tc_task_t;

/** Current mode with respect to cutting the audio recording */
typedef enum {
    CCTX_SILENCE,         /**< In a passage of prolongued silence between tracks */
    CCTX_TRACK,           /**< Currently in the middle of a track */
    CCTX_TRACK_STARTING,  /**< We suspect we may be commencing a new track */
    CCTX_TRACK_ENDING     /**< We suspect the current track may be ending */
} cut_context_t;

/** Action to take when encountering new tracks */
typedef enum {
    CPA_LOG_POINT,      /**< Write to a cut point file */
    CPA_EXTRACT_TRACK   /**< Extract the actual waveform to a new file */
} cut_point_action_t;

/** Method of describing cut points */
typedef enum {
    CPF_FRAME_INDEX,   /**< Number of samples since the start */
    CPF_TIME_INDEX,    /**< Time index (hours:mins:secs.fracsecs) */
    CPF_SEC_INDEX      /**< Absolute number of seconds */
} cut_point_format_t;

/** File extension-format mapping */
typedef struct {
    int sf_format;      /**< sndfile format code (0 for terminating entry) */
    const char *ext;    /**< file extension */
    const char *desc;   /**< brief description */
} file_format_t;

/** Command-line argument structure */
typedef struct
{
    int argc;               /**< from main() */
    char **argv;            /**< from main() */
    int cur_shortopt;       /**< returned from a call to getopt_long(); current short argument */
    /** returned from a call to getopt_long(); index into #longopts of
        current long argument, or negative if current argument is a
        short argument. */
    int cur_longopt_idx;

    /** Task mode */
    tc_task_t task;

    /** Action performed for each track */
    cut_point_action_t cut_point_action;

    /** Name of source audio recording file; @c NULL if standard input. */
    const char *in_file_name;

    /** Cut point file name (@c NULL means standard output) */
    const char *cuts_file_name;

    /** Directory where extracted tracks are written (@c NULL means not given) */
    const char *track_directory;

    /** List file containing track names (@c NULL means not specified; use numbers instead.) */
    const char *track_names_file_name;

    /** Print format for cutting points in cuts file (frame offsets or time indices) */
    cut_point_format_t cut_point_format;

    /** Minimum silence period that must exist between successive tracks
        (in milliseconds). The user should set this long enough so that
        any actual rests during the middle of the song are not
        considered the end of it. */
    int min_silence_period;

    /** Minimum non-silence period that constitutes start of new track
        (in milliseconds). This is to prevent short transient glitches
        in the source (clicks, pops etc) from being interpreted as the
        start of a new song). */
    int min_signal_period;

    /** Signal-to-noise ratio used for determining which passages of
        audio constitute a song, versus which constitute intervening
        silence. Given in dBFS; must be negative. */
    float noise_floor_dbfs;

    /** Minimum length for a track; this is used to prevent tracks that
        start off with mostly rests (e.g. a lone high-hat, other
        percussion, staccato notes, musique concrete) from being
        mis-identified as multiple tracks. Given in seconds. */
    int min_track_length;

    /** Set this flag if a time range was given (stored in @c start_time
        and @c end_time). The sample rate is not yet known at argument
        parsing and @c start_frame_idx and @c end_frame_idx need to be
        derived. */
    int time_range_given;

    /** Starting time (given in absolute seconds) */
    double start_time;

    /** Ending time (given in absolute seconds) */
    double end_time;

    /** Start processing from this frame offset */
    sf_count_t start_frame_idx;

    /** End processing at this frame offset */
    sf_count_t end_frame_idx;

    /** Start numbering tracks from (useful in multiple pass invocation) */
    int track_num_start;

    /** Maximum track number that should be identified before exiting
        (useful in multiple pass invocation) */
    int track_num_end;

    /** Set this flag if input is raw (headerless) audio */
    int input_is_raw;

    /** Input audio stream attributes (use for specifying raw audio parameters) */
    SF_INFO in_sfinfo;

    /** Output file format (if set to zero, use input format) */
    int out_sfinfo_format;

    /** DC offset adjust for left/right channels */
    double dc_offset[MAX_CHANNELS];

    /** High-pass filter option */
    int high_pass_filter_enabled;

    /** Set this flag to suppress cuts file header */
    int no_cuts_file_header;
    
    /** Verbose flag */
    int verbose;
} options_t;

typedef struct 
{
    SNDFILE *in_file;           /**< Input file containing audio to process */
    char *out_file_name;        /**< Current output file name (extraction mode only) */
    SNDFILE *out_file;          /**< Current output file (extraction mode only) */
    SF_INFO *out_sfinfo;        /**< Format options for current output file (extraction mode only)*/
    FILE *cuts_file;            /**< Cut point destination (may point to stdout); NULL in extraction mode. */
    FILE *track_names_file;     /**< Track names source (may point to stdin); NULL if absent. */
    int in_eof;                 /**< Set this flag once EOF is reached on input audio stream */
    sf_count_t frames_remaining;/**< Number of frames remaining yet to be processed */
    cut_context_t cut_context;  /**< Current track-cutting context state */
    sf_count_t time_to_live;    /**< Time-to-live for current state, in frames (when relevant). */
    
    sf_count_t frames_read_ttl; /**< Number of frames read to date (for HPF) */
    sf_count_t frames_proc_ttl; /**< Number of frames processed to date (for RMS) */
    sf_count_t cur_frame_pos;   /**< Current frame position in input file */
    int cur_track_num;          /**< Current track number in input file */
    char *cur_track_name;       /**< Current track name (if supplied; NULL otherwise) */
    size_t cur_track_name_sz;   /**< Current malloc() allocation size of @a cur_track_name */
    sf_count_t cur_track_start; /**< Current track starting frame index */
    int numchannels;            /**< Number of channels in input file */
    int samplerate;             /**< Sampling rate in Hz */
    int rms_window_len;         /**< Number of samples in RMS/prefilter/postfilter buffers */
    int min_signal_len;         /**< Number of frames of non-silence needed to start a new track */
    int min_silence_len;        /**< Number of frames of silence needed to end a track */
    int min_track_len;          /**< Minimum number of frames that constitute a track */
    int leadin_buf_len;         /**< Length of lead-in buffer, in frames. */
    int frame_sz;               /**< Size of each frame in bytes */
    /** Number of frames that input file position is ahead of current processing position by */
    sf_count_t ra_frame_cnt;

    double alpha;                         /**< Scaling factor in high-pass filter */
    double n_x_nf_sq;                    /**< n(x_nf)^2 precomputed for RMS comparisons */
    double x_sq_ttl[MAX_CHANNELS];        /**< Current sum(x_i^2) for RMS comparisons */
    double hpf_rej[MAX_CHANNELS];         /**< Previous shunt-to-earth level for high-pass filter */
    double hpf_rej_ttl[MAX_CHANNELS];     /**< Cumulative total of reject levels, used for computing DC offset */
    double hpf_out[MAX_CHANNELS];         /**< HPF output for current frame, used for computing DC offset */
    double hpf_prev_out[MAX_CHANNELS];    /**< HPF output for previous frame, used for computing DC offset */
    double hpf_prev_rej[MAX_CHANNELS];    /**< HPF residual for previous frame, used for computing DC offset */
    double min_rms[MAX_CHANNELS];         /**< Lowest RMS level encountered so far */
    double max_rms[MAX_CHANNELS];         /**< Highest RMS level encountered so far */
    double rms_ttl[MAX_CHANNELS];         /**< Cumulative total of RMS values, used for computing average RMS. */
    double cur_rms[MAX_CHANNELS];         /**< Current RMS levels per channel at this point in time */
    double pos_peak[MAX_CHANNELS];        /**< Global positive-side peak level */
    double neg_peak[MAX_CHANNELS];        /**< Global negative-side peak level */
    
    /* The following buffers are all of size: sizeof(double)*rms_window_len*numchannels */
    double *sq_buf;         /**< x-squared circular queue of samples (used for computing RMS) */
    double *main_buf;       /**< Circular queue buffer of [filtered] frames */
    
    double *sq_buf_head;    /**< Index of latest frame in @a sq_buf */
    double *sq_buf_tail;    /**< Index of earliest frame in @a sq_buf */
    double *sq_buf_cen;     /**< Index of central (current frame) in @a sq_buf */
    /** Points to memory location where @a sq_buf ends. Do NOT
        dereference this pointer; rather use it for testing when the
        head/tail/centre pointers need to be wrapped around. */
    double *sq_buf_edge;    
    
    double *main_buf_head;  /**< Index of latest frame in @a main_buf */
    double *main_buf_tail;  /**< Index of earliest frame in @a main_buf */
    double *main_buf_cen;   /**< Index of central (current frame) in @a main_buf */
    /** Points to memory location where @a main_buf ends. Do NOT
        dereference this pointer; rather use it for testing when the
        head/tail/centre pointers need to be wrapped around. */
    double *main_buf_edge;

    /* The following buffers are all of size: sizeof(double)*min_signal_len*numchannels */

    /** Collects samples during state CCTX_TRACK_STARTING. Note that
        this is a regular array buffer, not a circular queue like above;
        it either gets flushed to disk and/or then discarded in
        entirety. */
    double *leadin_buf;     
    double *leadin_buf_end; /**< Points to next available element of @a leadin_buf */
    double *leadin_buf_edge;/**< End of leadin buffer; used to check for capacity */
    
} state_t;

/** Short option list for @c getopt() */
static const char shortopts[] = "hCaf:PpAo:d:i:s:n:l:S:t:I:T:rR:c:b:xuXEeD:HNVv";

/** This must be no less than the length of the longest name in #longopts */
#define MAX_LONG_OPTION_NAME_LEN 32

/** Long option array to supply to @c getopt_long() when parsing the
    command-line arguments. */
static const struct option longopts[] =
{
    { "help", no_argument, NULL, 'h' },
    { "cut", no_argument, NULL, 'C' },
    { "analyse", no_argument, NULL, 'a' },
    { "output-format", required_argument, NULL, 'f' },
    { "print-frame-indices", no_argument, NULL, 'P' },
    { "print-time-indices", no_argument, NULL, 'p' },
    { "print-sec-indices", no_argument, NULL, 'A' },
    { "cuts-file", required_argument, NULL, 'o' },
    { "extract-dir", required_argument, NULL, 'd' },
    { "track-names-file", required_argument, NULL, 'i' },
    { "min-silence-period", required_argument, NULL, 's' },
    { "min-signal-period", required_argument, NULL, 'n' },
    { "min-track-length", required_argument, NULL, 'l' },
    { "noise-floor", required_argument, NULL, 'S' },
    { "time-range", required_argument, NULL, 't' },
    { "frame-range", required_argument, NULL, 'I' },
    { "track-range", required_argument, NULL, 'T' },
    { "raw", no_argument, NULL, 'r' },
    { "rate", required_argument, NULL, 'R' },
    { "channels", required_argument, NULL, 'c' },
    { "bits", required_argument, NULL, 'b' },
    { "signed", no_argument, NULL, 'x' },
    { "unsigned", no_argument, NULL, 'u' },
    { "floating-point", no_argument, NULL, 'X' },
    { "big-endian", no_argument, NULL, 'E' },
    { "little-endian", no_argument, NULL, 'e' },
    { "dc-offset", required_argument, NULL, 'D' },
    { "high-pass", no_argument, NULL, 'H' },
    { "no-cuts-file-header", no_argument, NULL, 'N' },
    { "version", no_argument, NULL, 'V' },
    { "verbose", no_argument, NULL, 'v' },
    { NULL },
};

/** File name used to represent standard input */
static const char stdin_file_name[] = "-";

/** File name used to represent standard output */
static const char stdout_file_name[] = "-";

/** String used to describe standard input to user */
static const char stdin_description[] = "<standard input>";

/** String used to describe standard output to user */
static const char stdout_description[] = "<standard output>";

/** Human-readable representations of #tc_task_t values */
static const char *tc_task_t_s[] =
{
    "TCT_CUTTING",
    "TCT_ANALYSIS",
    NULL,
};

/** Human-readable representations of #cut_point_action_t */
static const char *cut_point_action_t_s[] =
{
    "CPA_LOG_POINT",
    "CPA_EXTRACT_TRACK",
    NULL,
};

/** Human-readable representations of #cut_point_format_t values */
static const char *cut_point_format_t_s[] =
{
    "CPF_FRAME_INDEX",
    "CPF_TIME_INDEX",
    "CPF_SEC_INDEX",
    NULL,
};


/* Oh dear, this isn't necessary. Look at sf_command(SFC_GET_FORMAT_MAJOR_COUNT) */

/*  Lookup table of file formats. It'd be nice if libsndfile had a way
    of allowing application programs to query the supported list at
    runtime (avoiding need to hard-code). */
/*static const file_format_t file_formats[] =
{
    { SF_FORMAT_WAV, "wav", "Microsoft WAVe format" },
    { SF_FORMAT_AIFF, "aiff", "Apple/SGI AIFF format" },
    { SF_FORMAT_AU, "au", "Sun/NeXT AU format" },
    { SF_FORMAT_RAW, "raw", "Raw PCM data (no header)" },
    { SF_FORMAT_VOC, "voc", "Creative Labs VOiCe format" },
    { SF_FORMAT_FLAC, "flac", "Free Lossless Audio Codec" },
    { SF_FORMAT_VORBIS, "ogg", "Xiph Ogg Vorbis" },
    { 0 },
};*/

/** Program options, as parsed from command line. */
static options_t options;

/** Program state */
static state_t state;

/** Sets default program options */
static void init_options(void)
{
    memset(&options, 0, sizeof(options));
    options.task = TCT_CUTTING;
    options.cut_point_action = CPA_LOG_POINT;
    options.cut_point_format = CPF_TIME_INDEX;
    options.min_silence_period = DFL_MIN_SILENCE_PERIOD;
    options.min_signal_period = DFL_MIN_SIGNAL_PERIOD;
    options.min_track_length = DFL_MIN_TRACK_LENGTH;
    options.noise_floor_dbfs = DFL_NOISE_FLOOR;
    options.end_frame_idx = SF_COUNT_MAX;
    options.track_num_end = INT_MAX;
}

/** Prints "try `progname --help' for more information" message to
    standard error. The best way to use this function when exiting via
    error() is to register it through atexit().  */
static void print_get_help_msg(void)
{
    fprintf(stderr, "Try `%s --help' for more information.\n",
        program_invocation_short_name);
}

/** Prints the help message to standard output */
static void print_help_msg(void)
{         /*00000000011111111112222222222333333333344444444445555555555666666666677777777778*/
          /*12345678901234567890123456789012345678901234567890123456789012345678901234567890*/
    printf("Usage: %s [--cut] [--cuts-file=CUTSFILE] [OPTION...] FILE\n", program_invocation_short_name);
    printf("   or: %s [--cut] --extract-dir=DIR [OPTION...] FILE\n", program_invocation_short_name);
    printf("   or: %s --analyse [OPTION...] FILE\n", program_invocation_short_name);
    printf("Divides an audio recording into multiple tracks delimited by silence.\n");
    printf("\n");
    printf("Mode switches:\n");
    printf("  -C, --cut                 Search for track delimiters (default mode)\n");
    printf("  -a, --analyse             Perform statistical analysis on FILE\n");
    printf("\n");
    printf("Options applicable in all modes:\n");
    printf("  -t, --time-range=S-F   Only process input file between given bounds.\n");
    printf("                         Time indices are given as follows:\n");
    printf("                         HH:MM:SS.SSS-HH:MM:SS.SSS\n");
    printf("                         MM:SS.SSS-MM:SS.SSS\n");
    printf("                         SS.SSS-SS.SSS\n");
    printf("                         If start time omitted, beginning of file implied.\n");
    printf("                         If finish time omitted, end of file implied.\n");
    printf("  -I, --frame-range=S-F  Only process input file between given frame bounds.\n");
    printf("                         A frame is a group of samples, 1 for each channel, for\n");
    printf("                         a particular instant in time.\n");
    printf("                         Frame zero is considered start of recording.\n");
    printf("                         If start frame omitted, start of recording implied\n");
    printf("                         If end frame omitted, end of recording implied\n");
    printf("  -D, --dc-offset=N,N... Apply DC offset correction to audio signal\n");
    printf("                         before processing, one offset per channel.\n");
    printf("                         Offsets given as real numbers within\n");
    printf("                         [-1.0, +1.0]; these bounds representing\n");
    printf("                         digital full scale.\n");
    printf("  -H, --high-pass        Run audio signal thru high-pass filter with\n");
    printf("                         corner frequency (3dB att.) at %.1fHz\n", HIGH_PASS_CORNER_FREQ);
    printf("                         before processing.\n");
    printf("  -r, --raw              Indicates input recording is raw (headerless) audio.\n");
    printf("\n");
    printf("For raw audio the following options must be given; no defaults are presumed.\n");
    printf("  -R, --rate=N          Sampling rate in Hz\n");
    printf("  -c, --channels=N      Number of channels (max %d)\n", MAX_CHANNELS);
    printf("  -b, --bits=N          Bits per sample (8, 16, 24, 32 or 64)\n");
    printf("  -x, --signed          Samples are signed integers (8, 16, 24 or 32-bit)\n");
    printf("  -u, --unsigned        Samples are unsigned integers (8-bit only)\n");
    printf("  -X, --floating-point  Samples are floating point (32 or 64-bit)\n");
    printf("  -E, --big-endian      Sample words are big-endian\n");
    printf("  -e, --little-endian   Sample words are little-endian\n");
    printf("\n");
    printf("In cutting mode, specify one of the following actions:\n");
    printf("  -o, --cuts-file=CUTSFILE  Write track indices/durations to file CUTSFILE.\n");
    printf("                            This is the default action. If omitted or `-' is\n");
    printf("                            given, cuts list is sent to standard output.\n");
    printf("  -d, --extract-dir=DIR     Extract tracks to individual files in directory DIR\n");
    printf("\n");
    printf("Options applicable in cutting mode (--cut):\n");
    printf("  -i, --track-names-file=LISTFILE  Text file containing track names.\n");
    printf("                                   One line per track; used for naming files in\n");
    printf("                                   extraction mode. If omitted, track files\n");
    printf("                                   will be numbered incrementally. Specify `-'\n");
    printf("                                   to read list from standard input.\n");
    printf("  -s, --min-silence-period=N       Minimum period of silence that delimit tracks.\n");
    printf("                                   Given in milliseconds. Default is %d.\n", DFL_MIN_SILENCE_PERIOD);
    printf("  -n, --min-signal-period=N        Minimum period of non-silence that signifies\n");
    printf("                                   start of new track. Given in milliseconds.\n");
    printf("                                   Default is %d.\n", DFL_MIN_SIGNAL_PERIOD);
    printf("  -l, --min-track-length=N         Minimum length, in seconds, of each track.\n");
    printf("                                   Default is %d.\n", DFL_MIN_TRACK_LENGTH);
    printf("  -S, --noise-floor=N              Noise floor used for discriminating actual\n");
    printf("                                   signal from silence. Given in decibels\n");
    printf("                                   full scale (dBFS), must be a negative real\n");
    printf("                                   number. Default is %.2f.\n", DFL_NOISE_FLOOR);
    printf("  -T, --track-range=A-B            Signifies track numbering will start from A\n");
    printf("                                   and processing will stop at track number B.\n");
    printf("                                   Track numbers must be positive integers.\n");
    printf("                                   If A is omitted, start from track 1.\n");
    printf("                                   If B is omitted, will continue indefinitely\n");
    printf("                                   until end of recording reached.\n");
    printf("                                   Will skip first A lines in LISTFILE given by\n");
    printf("                                   -i, however corresponding start point in\n");
    printf("                                   input recording must be given by -t or -I.\n");
    printf("\n");
    printf("Options applicable in cuts file mode (--cuts-file):\n");
    printf("  -P, --print-frame-indices   Cut points & track durations given in frames.\n");
    printf("  -p, --print-time-indices    Cut points & track durations given in hrs:min:sec.\n");
    printf("  -A, --print-sec-indices     Cut points & track durations given in seconds.\n");
    printf("                              The default is to print indices in hrs:min:sec.\n");
    printf("  -N, --no-cuts-file-header   Suppress printing a header in the output.\n");
    printf("\n");
    printf("Options applicable in extraction mode (--extract-dir):\n");
    printf("  -f, --output-format=EXT   Format for output files. See list below.\n");
    printf("                            If not given, input file format will be used.\n");
    printf("\n");
    printf("List of available output file formats:\n");
    {
        /*const file_format_t *cur_ff = file_formats;
        while(cur_ff->sf_format)
        {
            printf("\t%s\t%s\n", cur_ff->ext, cur_ff->desc);
            cur_ff++;
        }*/
        int cur_format;
        int num_formats;
    
        sf_command(NULL, SFC_GET_FORMAT_MAJOR_COUNT, &num_formats, sizeof(int));
        for(cur_format = 0;  cur_format <= num_formats; cur_format++)
        {
            SF_FORMAT_INFO sf_format_info;
            sf_format_info.format = cur_format;
            if(sf_command(NULL, SFC_GET_FORMAT_MAJOR, &sf_format_info, sizeof(SF_FORMAT_INFO)) == 0)
            {
                printf("\t%s\t%s\n", sf_format_info.extension, sf_format_info.name);
            }
        }
    }
    printf("\n");
    printf("Specify a input file name of `-' to read audio data from standard input.\n");
    printf("\n");
}

/** Returns a visual string representation of the most recent option
    name that was parsed by getopt_long (preceded by 1-2 dashes as
    needed); useful for error messages. The string is contained in a
    private statically allocated buffer. */
static const char *render_current_option(void)
{
    static char optname[MAX_LONG_OPTION_NAME_LEN + 3];

    if(options.cur_longopt_idx >= 0)
    {
        optname[0] = '-';
        optname[1] = '-';
        strcpy(optname + 2, longopts[options.cur_longopt_idx].name);
    }
    else
    {
        optname[0] = '-';
        optname[1] = options.cur_shortopt;
        optname[2] = 0;
    }
    return optname;
}

/** Parses the current argument as an audio output format, given with @c
    -f. Will terminate the program if an invalid format is given. */
static void parse_audio_output_format_arg(void)
{
    /*const file_format_t *cur_ff = file_formats;

    while(cur_ff->sf_format && strcasecmp(cur_ff->ext, optarg) != 0)
    {
        cur_ff++;
    }

    if(cur_ff->sf_format)
    {
        options.out_sfinfo_format = cur_ff->sf_format;
    }
    else
    {
        atexit(print_get_help_msg);
        error(EXIT_FAILURE, 0, "Unrecognised output file format extension: `%s'", optarg);
    }*/
    
    /* cur_format: Current format code being tested against argument */
    /* num_formats: Number of formats libsndfile supports */
    int cur_format;
    int num_formats;
    
    options.out_sfinfo_format = 0;
    sf_command(NULL, SFC_GET_FORMAT_MAJOR_COUNT, &num_formats, sizeof(int));
    /*fprintf(stderr, "num_formats = %d\n", num_formats);*/
    for(cur_format = 0; 
        cur_format <= num_formats && !options.out_sfinfo_format; 
        cur_format++)
    {
        /* sf_format_info: Extension/description fields for currently tested format */
        SF_FORMAT_INFO sf_format_info;
        
        sf_format_info.format = cur_format;
        if(sf_command(NULL, SFC_GET_FORMAT_MAJOR, &sf_format_info, sizeof(SF_FORMAT_INFO)) == 0
            && strcasecmp(sf_format_info.extension, optarg) == 0)
        {
            options.out_sfinfo_format = sf_format_info.format & SF_FORMAT_TYPEMASK;
        }
        /*fprintf(stderr, "Comparing extension `%s' against `%s'\n", sf_format_info.extension, optarg);*/
    }
    if(!options.out_sfinfo_format)
    {
        error(EXIT_FAILURE, 0, "Unrecognised output file format extension: `%s'", optarg);
    }
}

/** Parses current option argument (as indicated by getopt) as a
    positive integer value. Terminates program with an error message if
    an invalid argument is given (non-positive or non-numeric).

    @returns Parsed integer value; guranteed to be positive. */
static int parse_positive_int_arg(void)
{
    char *optarg_str_tail;
    int n = strtol(optarg, &optarg_str_tail, 0);

    if(optarg_str_tail == optarg || n <= 0)
    {
        atexit(print_get_help_msg);
        error(EXIT_FAILURE, 0, "Argument `%s' for option `%s' must be a positive integer",
            optarg, render_current_option());
    }
    else if(errno)
    {
        atexit(print_get_help_msg);
        error(EXIT_FAILURE, errno, "Bad argument `%s' for option `%s'",
            optarg, render_current_option());
    }
    return n;
}

/** Parses current option argument as a noise floor quantity,
    given in decibels full-scale (dbFS). Must be negative. Terminates
    program with an error message if an invalid argument is given
    (non-negative or non-numeric).

    @returns Parsed noise floor value, guaranteed to be negative. */
static double parse_noise_floor_arg(void)
{
    char *optarg_str_tail;
    double n = strtod(optarg, &optarg_str_tail);

    if(optarg_str_tail == optarg || n >= 0.0)
    {
        atexit(print_get_help_msg);
        error(EXIT_FAILURE, 0, "Argument `%s' for option `%s' must be a negative real number",
            optarg, render_current_option());
    }
    else if(errno)
    {
        atexit(print_get_help_msg);
        error(EXIT_FAILURE, errno, "Bad argument `%s' for option `%s'",
            optarg, render_current_option());
    }
    return n;
}

/** Parses a time code string; returns absolute number of seconds.
    Terminates program with error message if string is malformed.

    @param s String to parse
    @param dfl Default value to use if string is empty or only contains
    whitespace.
    @returns Parsed numeric value of @a s in absolute seconds, if valid. */
static double parse_time_code(const char *s, double dfl)
{
    /* hrs,min,sec: Used for storing hrs:min:sec components from sscanf() */
    /* s_tail_idx: Index into s of last character parsed */
    int hrs;
    int min;
    double sec;
    int s_tail_idx;

    if(sscanf(s, "%d:%d:%lf %n", &hrs, &min, &sec, &s_tail_idx) == 3)
    {
        sec += (double)min * 60.0 + (double)hrs * 3600.0;
    }
    else if(sscanf(s, "%d:%lf %n", &min, &sec, &s_tail_idx) == 2)
    {
        sec += (double)min * 60.0;
    }
    else if(sscanf(s, "%lf %n", &sec, &s_tail_idx) == 1)
    {
        /* This block intentionally empty */
    }
    else
    {
        /* One way of testing if a string is empty or contains only whitespace */
        sscanf(s, " %n", &s_tail_idx);
        sec = dfl;
    }

    if(s[s_tail_idx])
    {
        /* String is blatantly malformed or contains spurious junk at the end */
        atexit(print_get_help_msg);
        error(EXIT_FAILURE, 0, "Timecode `%s' specified in argument for option `%s' is malformed",
            s, render_current_option());
    }

    return sec;
}

/** Parses current time range argument. Results stored in @c
    options.start_time and options.end_time, and the @c
    options.time_range_given flag will be set. Terminates program if
    malformed or invalid (non-positive duration) with an error message.

    A time range argument should have one of the following forms:

    <ul>
    <li>ssss.sssss-ssss.sssss</li>
    <li>m:ss.sssss-m:ss.sssss</li>
    <li>h:m:s.sssss-m:ss.sssss</li>
    </ul>

    That is, two timecodes separated by a hyphen; the first timecode is
    the start index, the second is the terminating one. If the first
    timecode is omitted it is presumed to be zero (start of recording).
    If the second timecode is omitted, it is presumed to be the end of
    the recording.

    If a timecode contains no colons, it's presumed to be an absolute
    number of seconds. One colon implies minutes and seconds. Two colons
    (maximum) imply hours, minutes and seconds. Minutes and seconds may
    exceed 59, in which case they will carry over to hours and minutes
    respectively. */
static void parse_time_range(void)
{
    /* hyphen_ptr: Address of hyphen in argument */
    char *hyphen_ptr = strchr(optarg, '-');

    if(!hyphen_ptr || hyphen_ptr != strrchr(optarg, '-'))
    {
        atexit(print_get_help_msg);
        error(EXIT_FAILURE, 0,
            "Time range `%s' given with %s must be two timecodes separated by a hyphen.",
            optarg, render_current_option());
    }
    /* Modifies argv[] (need to put null byte in place of hyphen because there's no snscanf()) */
    *hyphen_ptr = 0;
    options.start_time = parse_time_code(optarg, 0.0);
    options.end_time = parse_time_code(hyphen_ptr + 1, INFINITY);
    *hyphen_ptr = '-';
    if(options.end_time < options.start_time)
    {
        atexit(print_get_help_msg);
        error(EXIT_FAILURE, 0,
            "Time range `%s' given by `%s' has end point before start.",
            optarg, render_current_option());
    }
    options.time_range_given = TRUE;
}

/** Parses a integer range boundary string. Must be a non-negative
    integer; if string is empty or contains whitespace then a supplied
    default value will be presumed. Terminates program with an error
    message if numeric string is malformed.

    @param s String to parse
    @param dfl Default value to use if string is empty or only contains
    whitespace.
    @returns Parsed numeric value of @a s, if valid. */
static sf_count_t parse_int_boundary(const char *s, sf_count_t dfl)
{
    /* n: Parsed value of s */
    /* s_tail_idx: Index into s of last character parsed */
    sf_count_t n;
    int s_tail_idx;

    if(sscanf(s, "%lld %n", &n, &s_tail_idx) == 1)
    {
        if(n < 0)
        {
            atexit(print_get_help_msg);
            error(EXIT_FAILURE, 0,
                "Boundary `%s' given in argument to option `%s' must not be negative",
                s, render_current_option());
        }
    }
    else
    {
        /* One way of testing if a string is empty or contains only whitespace */
        sscanf(s, " %n", &s_tail_idx);
        n = dfl;
    }

    if(s[s_tail_idx])
    {
        /* String is blatantly malformed or contains spurious junk at the end */
        atexit(print_get_help_msg);
        error(EXIT_FAILURE, 0, "Boundary `%s' specified in argument for option `%s' is malformed",
            s, render_current_option());
    }

    return n;
}

/** Parses current frame range argument. Results stored in @c
    options.start_frame_ofs and options.end_frame_ofs, and the @c
    options.time_range_given flag will be cleared. Terminates program if
    malformed or invalid (non-positive duration) with an error message.

    A frame range argument is two integers separated by a hyphen; the
    first offset is the start index, the second is the terminating one.
    If the first offset is omitted it is presumed to be zero (start of
    recording). If the second offset is omitted, it is presumed to be
    the end of the recording.*/
static void parse_frame_range(void)
{
    /* hyphen_ptr: Address of hyphen in argument */
    char *hyphen_ptr = strchr(optarg, '-');

    if(!hyphen_ptr || hyphen_ptr != strrchr(optarg, '-'))
    {
        atexit(print_get_help_msg);
        error(EXIT_FAILURE, 0,
            "Frame range `%s' given with `%s' must be two positive integers separated by a hyphen.",
            optarg, render_current_option());
    }
    /* Modifies argv[] (need to put null byte in place of hyphen because there's no snscanf()) */
    *hyphen_ptr = 0;
    options.start_frame_idx = parse_int_boundary(optarg, 0);
    options.end_frame_idx = parse_int_boundary(hyphen_ptr + 1, SF_COUNT_MAX);
    *hyphen_ptr = '-';
    if(options.end_frame_idx < options.start_frame_idx)
    {
        atexit(print_get_help_msg);
        error(EXIT_FAILURE, 0,
            "Frame range `%s' given by `%s' has end point before start.",
            optarg, render_current_option());
    }
    options.time_range_given = FALSE;
}

/** Parses current track range argument. Results stored in @c
    options.track_num_start and options.track_num_end. Terminates
    program if malformed or invalid (non-positive duration) with an
    error message.

    A track range argument is two integers separated by a hyphen; the
    first is the start index, the second is the terminating one. If the
    first offset is omitted it is presumed to be @c 1. If the second
    offset is omitted, it is presumed to be @c INT_MAX. */
static void parse_track_range(void)
{
    /* hyphen_ptr: Address of hyphen in argument */
    char *hyphen_ptr = strchr(optarg, '-');

    if(!hyphen_ptr || hyphen_ptr != strrchr(optarg, '-'))
    {
        atexit(print_get_help_msg);
        error(EXIT_FAILURE, 0,
            "Track range `%s' given with `%s' must be two positive integers separated by a hyphen.",
            optarg, render_current_option());
    }
    /* Modifies argv[] (need to put null byte in place of hyphen because there's no snscanf()) */
    *hyphen_ptr = 0;
    options.track_num_start = parse_int_boundary(optarg, 1);
    options.track_num_end = parse_int_boundary(hyphen_ptr + 1, INT_MAX);
    *hyphen_ptr = '-';
    if(options.end_frame_idx < options.start_frame_idx)
    {
        atexit(print_get_help_msg);
        error(EXIT_FAILURE, 0,
            "Track range `%s' given by `%s' has end point before start.",
            optarg, render_current_option());
    }
}

/** Parses DC offset argument, given by @c -D option. Note that the
    string pointed to by @a optarg will be munged, and that the number
    of channels is not verified here, since it may not be known yet. Any
    channel offsets not specified will be left alone from last
    invocation, or left at zero default if this is the first. Program
    will terminate with error message if out-of-range or non-numeric
    value given. */
static void parse_dc_offset_arg(void)
{
    /* c: Current channel number we are parsing offset token for */
    /* n_str: String version of current offset value */
    int c = 0;
    char *n_str = strtok(optarg, ",");

    while(c < MAX_CHANNELS && n_str)
    {
        /* n_tail: Last valid character parsed in n_str */
        /* n: Parsed value of n_str */
        char *n_tail;
        double n = strtod(n_str, &n_tail);

        if(n_tail == n_str)
        {
            atexit(print_get_help_msg);
            error(EXIT_FAILURE, 0, "DC offset value `%s' given by `%s' is non-numeric",
                n_str, render_current_option());
        }
        else if(errno)
        {
            atexit(print_get_help_msg);
            error(EXIT_FAILURE, errno, "DC offset value `%s' given by `%s'",
                n_str, render_current_option());
        }
        else if(n > 1.0 || n < -1.0)
        {
            atexit(print_get_help_msg);
            error(EXIT_FAILURE, 0, "DC offset value `%f' given by `%s' is outside [-1.0, +1.0]",
                n, render_current_option());
        }

        options.dc_offset[c] = n;
        c++;
        n_str = strtok(NULL, ",");
    }
}

/** Parses command line arguments and stores them in the #options
    structure. Note that this function will terminate the program if any
    errors are encountered, or the user help message is requested while
    parsing the command line arguments. If this function returns, then
    the parsing the command line arguments was successful. */
static void parse_options(void)
{
    /* raw_*_given: Set these flags when the corresponding raw format parameter is found */
    /* raw_bits: Number of bits in raw input format */
    /* raw_is_fp: Set flag if raw input format has floating point samples */
    /* raw_is_signed: Set flag if raw input format is has signed integer samples */
    /* raw_is_little_endian: Set flag if raw input format is little endian */
    int raw_rate_given = FALSE;
    int raw_channels_given = FALSE;
    int raw_bits_given = FALSE;
    int raw_sign_given = FALSE;
    int raw_endian_given = FALSE;
    int raw_bits = 0;
    int raw_is_fp = FALSE;
    int raw_is_signed = FALSE;
    int raw_is_little_endian = FALSE;

    do
    {
        options.cur_longopt_idx = -1;
        options.cur_shortopt = getopt_long(options.argc, options.argv, shortopts, longopts, &options.cur_longopt_idx);

        switch(options.cur_shortopt)
        {
            case 'h':
                print_help_msg();
                exit(EXIT_SUCCESS);
            case 'a':
                options.task = TCT_ANALYSIS;
                break;
            case 'C':
                options.task = TCT_CUTTING;
                break;
            case 'f':
                parse_audio_output_format_arg();
                break;
            case 'P':
                options.cut_point_format = CPF_FRAME_INDEX;
                break;
            case 'p':
                options.cut_point_format = CPF_TIME_INDEX;
                break;
            case 'A':
                options.cut_point_format = CPF_SEC_INDEX;
                break;
            case 'o':
                options.cuts_file_name = optarg;
                break;
            case 'd':
                /** Implies track extraction mode */
                options.track_directory = optarg;
                options.cut_point_action = CPA_EXTRACT_TRACK;
                break;
            case 'i':
                /* Can be used in either mode */
                options.track_names_file_name = optarg;
                break;
            case 'l':
                options.min_track_length = parse_positive_int_arg();
                break;
            case 's':
                options.min_silence_period = parse_positive_int_arg();
                break;
            case 'n':
                options.min_signal_period = parse_positive_int_arg();
                break;
            case 'S':
                options.noise_floor_dbfs = parse_noise_floor_arg();
                break;
            case 't':
                parse_time_range();
                break;
            case 'I':
                parse_frame_range();
                break;
            case 'T':
                parse_track_range();
                break;
            case 'r':
                options.input_is_raw = TRUE;
                break;
            case 'R':
                options.in_sfinfo.samplerate = parse_positive_int_arg();
                raw_rate_given = TRUE;
                break;
            case 'c':
                options.in_sfinfo.channels = parse_positive_int_arg();
                raw_channels_given = TRUE;
                if(options.in_sfinfo.channels > MAX_CHANNELS)
                {
                    error(EXIT_FAILURE, 0,
                        "This program only supports raw audio files up to %d channels, sorry.",
                        MAX_CHANNELS);
                }
                break;
            case 'b':
                raw_bits = parse_positive_int_arg();
                raw_bits_given = TRUE;
                if(raw_bits != 8 && raw_bits != 16 && raw_bits != 24 && raw_bits != 32 && raw_bits != 64)
                {
                    error(EXIT_FAILURE, 0,
                        "This program only supports raw audio files with 8, 16, 24, 32 or 64-bit samples, sorry.");
                }
                break;
            case 'x':
                raw_is_signed = TRUE;
                raw_is_fp = FALSE;
                raw_sign_given = TRUE;
                break;
            case 'u':
                raw_is_signed = FALSE;
                raw_is_fp = FALSE;
                raw_sign_given = TRUE;
                break;
            case 'X':
                raw_is_fp = TRUE;
                raw_sign_given = TRUE;
                break;
            case 'E':
                raw_is_little_endian = FALSE;
                raw_endian_given = TRUE;
                break;
            case 'e':
                raw_is_little_endian = TRUE;
                raw_endian_given = TRUE;
                break;
            case 'D':
                parse_dc_offset_arg();
                break;
            case 'H':
                options.high_pass_filter_enabled = TRUE;
                break;
            case 'N':
                options.no_cuts_file_header = TRUE;
                break;
            case 'V':
                puts(VERSION);
                exit(EXIT_SUCCESS);
            case 'v':
                options.verbose = TRUE;
                break;
            case '?':
            case ':':
                /* Invalid/missing argument. The getopt() call will print
                   an error message describing to the user what the
                   problem is. Print an additional message informing the
                   user on how to get option help and terminate. */
                print_get_help_msg();
                exit(EXIT_FAILURE);
                break;
        }
    }
    while(options.cur_shortopt >= 0);

    if(optind + 1 == options.argc)
    {
        /* Only one file name given in arguments */
        if(strcmp(stdin_file_name, options.argv[optind]) != 0)
        {
            /* Audio data to be read from a file */
            options.in_file_name = options.argv[optind];
        }
        else if(options.track_names_file_name &&
            strcmp(stdin_file_name, options.track_names_file_name) == 0)
        {
            atexit(print_get_help_msg);
            error(EXIT_FAILURE, 0, "Can't read both audio data and track names from standard input");
        }
    }
    else if(optind + 1 < options.argc)
    {
        /* Multiple file names given in arguments */
        atexit(print_get_help_msg);
        error(EXIT_FAILURE, 0, "Multiple input files not permitted: `%s'", options.argv[optind + 1]);
    }
    else
    {
        /* No file names given in arguments */
        atexit(print_get_help_msg);
        error(EXIT_FAILURE, 0, "No input file was specified");
    }

    if(options.input_is_raw)
    {
        /* Validate raw audio parameters */
        if(!raw_rate_given)
        {
            atexit(print_get_help_msg);
            error(EXIT_FAILURE, 0, "Raw audio sampling rate must be given with `--rate'");
        }
        else if(!raw_channels_given)
        {
            atexit(print_get_help_msg);
            error(EXIT_FAILURE, 0, "Raw audio number of channels must be given with `--channels'");
        }
        else if(!raw_bits_given)
        {
            atexit(print_get_help_msg);
            error(EXIT_FAILURE, 0, "Raw audio sample bit size must be given with `--bits'");
        }
        else if(!raw_sign_given)
        {
            atexit(print_get_help_msg);
            error(EXIT_FAILURE, 0,
                "Raw audio sample type must be given with either `--signed', `--unsigned' or `--floating-point'");
        }
        else if(!raw_endian_given)
        {
            atexit(print_get_help_msg);
            error(EXIT_FAILURE, 0,
                "Raw audio endian direction must be given with either `--big-endian' or `--little-endian'");
        }
        /* Assertions already made: raw_bits must be 8, 16, 24, 32 or 64. */
        else if(!raw_is_fp && raw_is_signed && raw_bits == 64)
        {
            atexit(print_get_help_msg);
            error(EXIT_FAILURE, 0,
                "Raw audio mode only allows 8, 16, 24 or 32-bit signed integer samples, sorry.");
        }
        else if(!raw_is_fp && !raw_is_signed && raw_bits != 8)
        {
            atexit(print_get_help_msg);
            error(EXIT_FAILURE, 0,
                "Raw audio mode only allows 8-bit unsigned integer samples, sorry.");
        }
        else if(raw_is_fp && raw_bits < 32)
        {
            atexit(print_get_help_msg);
            error(EXIT_FAILURE, 0,
                "Raw audio mode only supports 32 and 64-bit floating point samples, sorry.");
        }

        /* Piece together the format code for libsndfile */
        options.in_sfinfo.format = SF_FORMAT_RAW;
        options.in_sfinfo.format |= raw_is_little_endian ? SF_ENDIAN_LITTLE : SF_ENDIAN_BIG;
        if(raw_is_fp && raw_bits == 32)
        {
            options.in_sfinfo.format |= SF_FORMAT_FLOAT;
        }
        else if(raw_is_fp && raw_bits == 64)
        {
            options.in_sfinfo.format |= SF_FORMAT_DOUBLE;
        }
        else if(!raw_is_signed)
        {
            options.in_sfinfo.format |= SF_FORMAT_PCM_U8;
        }
        else if(raw_bits == 8)
        {
            options.in_sfinfo.format |= SF_FORMAT_PCM_S8;
        }
        else if(raw_bits == 16)
        {
            options.in_sfinfo.format |= SF_FORMAT_PCM_16;
        }
        else if(raw_bits == 24)
        {
            options.in_sfinfo.format |= SF_FORMAT_PCM_24;
        }
        else
        {
            options.in_sfinfo.format |= SF_FORMAT_PCM_32;
        }
    }
}

/** This is analogous to GNU's @a error() function; it will only
    print informative messages to standard error if the verbose flag has
    been set, otherwise nothing happens. The arguments are exactly the
    same as @a printf(), with the difference that a newline will be
    implicitly tacked on the end of the message for convenience. */
static void verbose(const char *fmt, ...)
{
    if(options.verbose)
    {
        /* ap: Variable argument pointer */
        va_list ap;
        
        va_start(ap, fmt);
        fprintf(stderr, "%s: info: ", program_invocation_short_name);
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
        va_end(ap);
    }
}

static void dump_options(void)
{
    verbose("options.task = %s", tc_task_t_s[options.task]);
    verbose("options.cut_point_action = %s", cut_point_action_t_s[options.cut_point_action]);
    verbose("options.in_file_name = %s", options.in_file_name);
    verbose("options.cuts_file_name = %s", options.cuts_file_name);
    verbose("options.track_directory = %s", options.track_directory);
    verbose("options.track_names_file_name = %s", options.track_names_file_name);
    verbose("options.cut_point_format = %s", cut_point_format_t_s[options.cut_point_format]);
    verbose("options.min_silence_period = %d", options.min_silence_period);
    verbose("options.min_signal_period = %d", options.min_signal_period);
    verbose("options.noise_floor_dbfs = %f", options.noise_floor_dbfs);
    verbose("options.min_track_length = %d", options.min_track_length);
    verbose("options.time_range_given = %d", options.time_range_given);
    verbose("options.start_time = %f", options.start_time);
    verbose("options.end_time = %f", options.end_time);
    verbose("options.start_frame_idx = %lld", options.start_frame_idx);
    verbose("options.end_frame_idx = %lld", options.end_frame_idx);
    verbose("options.track_num_start = %d", options.track_num_start);
    verbose("options.track_num_end = %d", options.track_num_end);
    verbose("options.input_is_raw = %d", options.input_is_raw);
    {
        char s[MAX_CHANNELS * 16];
        char *s_end;
        int c;
        
        s_end = s + sprintf(s, "%f", options.dc_offset[0]);
        for(c = 1; c < MAX_CHANNELS; c++)
        {
            s_end += sprintf(s_end, ", %f", options.dc_offset[c]);
        }
        verbose("options.dc_offset = %s", s);
    }
    verbose("options.high_pass_filter_enabled = %d", options.high_pass_filter_enabled);
    verbose("options.verbose = %d", options.verbose);
    verbose("options.in_sfinfo.samplerate = %d", options.in_sfinfo.samplerate);
    verbose("options.in_sfinfo.channels = %d", options.in_sfinfo.channels);
    verbose("options.in_sfinfo.format = 0x%08x", options.in_sfinfo.format);
    verbose("options.out_sfinfo_format = 0x%08x", options.out_sfinfo_format);
}


/** Converts a frame index to a human-readable timecode index, using @a
    state.samplerate as the conversion factor. The fractional seconds
    part will be given to five decimal places, sufficient precision for
    recordings up to 100kHz sampling rate.
    
    @param s Points to a character buffer where the rendered string will
    be stored. This buffer must be at least @c TIMECODE_STR_SZ characters long
    (including null terminator).
    @param frame_idx Frame index to convert.
    @return @a s, so this function can be called in an argument for @c printf(). */
static const char *render_frame_idx_as_timecode(char *s, sf_count_t frame_idx)
{
    /* sec: seconds field */
    /* min: minutes field */
    /* hrs: hours field */
    double sec;
    int min;
    int hrs;
    int whole_sec;
    int frac_sec;
    
    sec = fmod((double)frame_idx / (double)state.samplerate, 60.0);
    whole_sec = (int)(floor(sec));
    frac_sec = (int)(fmod(floor(sec * 100000.0), 100000.0));
    min = (frame_idx / state.samplerate / 60) % 60;
    hrs = frame_idx / state.samplerate / 3600;
    snprintf(s, TIMECODE_STR_SZ, "%d:%02d:%02d.%05d", hrs, min, whole_sec, frac_sec);
    return s;
}

/** Converts a frame index to a human-readable absolute seconds index,
    using @a state.samplerate as the conversion factor. The fractional
    seconds part will be given to five decimal places, sufficient
    precision for recordings up to 100kHz sampling rate.
    
    @param s Points to a character buffer where the rendered string will
    be stored. This buffer must be at least @c TIMECODE_STR_SZ characters long
    (including null terminator).
    @param frame_idx Frame index to convert.
    @return @a s, so this function can be called in an argument for @c printf(). */
static const char *render_frame_idx_as_sec(char *s, sf_count_t frame_idx)
{
    snprintf(s, TIMECODE_STR_SZ, "%2.5f", (double)frame_idx / (double)state.samplerate);
    return s;
}

/** Determines if at least one of the channels in the current frame has
    a RMS level above the SNR threshold. Uses @a state.x_sq_ttl and @a
    state.n_x_nf_sq to make comparisons.
    
    @return @c TRUE if at least one of the channels in current frame has
    RMS level above SNR; @c FALSE if all channels in current frame
    contain noise or silence. */
static int we_have_signal(void)
{
    /* res: Return result */
    /* c: Current channel iteration variable */
    int res = FALSE;
    int c;
    
    for(c = 0; !res && c < state.numchannels; c++)
    {
        res = state.n_x_nf_sq < state.x_sq_ttl[c];
    }
    return res;
}

/** Prints the cuts file header (if enabled) */
static void print_cuts_header(void)
{
    if(!options.no_cuts_file_header && state.cuts_file)
    {
        char *start_s;
        char *end_s;
        char *duration_s;

        switch(options.cut_point_format)
        {   
            case CPF_FRAME_INDEX:
                start_s = "start_frame";
                end_s = "end_frame";
                duration_s = "duration_frames";
                break;
            case CPF_TIME_INDEX:
                start_s = "start_time";
                end_s = "end_time";
                duration_s = "duration_time";
                break;
            case CPF_SEC_INDEX:
                start_s = "start_sec";
                end_s = "end_sec";
                duration_s = "duration_secs";
                break;
        }
        
        fprintf(state.cuts_file, 
           /*00000000001111111111222222222233333333334444444444555555555566666666667777777777*/
           /*01234567890123456789012345678901234567890123456789012345678901234567890123456789*/
           /*track_num   start_frame     end_frame       duration_frames     name            */
           /*1               2147483647      2147483647          2147483647                  */
           /*track_num   start_time      end_time        duration_time       name            */
           /*1           hh:mm:ss.sssss  hh:mm:ss.sssss      hh:mm:ss.sssss                  */
           /*track_num   start_sec       end_sec         duration_secs       name            */
           /*1             ssssss.sssss    ssssss.sssss        ssssss.sssss                  */
            "track_num   %-16s"          "%-16s"          "%-20s%s\n",
            start_s, end_s, duration_s,
            options.track_names_file_name ? "name" : "");
        if(ferror(state.cuts_file))
        {
            error(EXIT_FAILURE, errno, "Unable to write header to cuts file `%s'",
                options.cuts_file_name);
        }
    }
}


/** Writes cutting parameters for current track to the cuts file. */
static void print_track_cut(void)
{
    if(state.cuts_file)
    {
        char start_s[TIMECODE_STR_SZ];
        char end_s[TIMECODE_STR_SZ];
        char duration_s[TIMECODE_STR_SZ];
    
        sf_count_t duration = state.cur_frame_pos - state.cur_track_start;
        switch(options.cut_point_format)
        {
            case CPF_FRAME_INDEX:
                snprintf(start_s, TIMECODE_STR_SZ, "%lld", state.cur_track_start);
                snprintf(end_s, TIMECODE_STR_SZ, "%lld", state.cur_frame_pos);
                snprintf(duration_s, TIMECODE_STR_SZ, "%lld", duration);
                break;
            case CPF_TIME_INDEX:
                render_frame_idx_as_timecode(start_s, state.cur_track_start);
                render_frame_idx_as_timecode(end_s, state.cur_frame_pos);
                render_frame_idx_as_timecode(duration_s, duration);
                break;
            case CPF_SEC_INDEX:
                render_frame_idx_as_sec(start_s, state.cur_track_start);
                render_frame_idx_as_sec(end_s, state.cur_frame_pos);
                render_frame_idx_as_sec(duration_s, duration);
                break;
        }
    
        fprintf(state.cuts_file, "%10d  %14s  %14s  %18s  %s\n",
            state.cur_track_num, start_s, end_s, duration_s,
            state.cur_track_name ? state.cur_track_name : "");
        if(ferror(state.cuts_file))
        {
            error(EXIT_FAILURE, errno, "Unable to write entry to cuts file `%s'",
                options.cuts_file_name);
        }
    }
}

/** Advances head/tail/central queue pointers in sq_buf[] and main_buf[]
    by one frame, wrapping them around as necessary. */
static void advance_buf_ptrs(void)
{
    state.main_buf_head = state.main_buf_tail;
    state.sq_buf_head = state.sq_buf_tail;
    state.main_buf_tail += state.numchannels;
    state.main_buf_cen += state.numchannels;
    state.sq_buf_tail += state.numchannels;
    state.sq_buf_cen += state.numchannels;
    if(state.main_buf_tail >= state.main_buf_edge)
    {
        state.main_buf_tail = state.main_buf;
    }
    if(state.main_buf_cen >= state.main_buf_edge)
    {
        state.main_buf_cen = state.main_buf;
    }
    if(state.sq_buf_tail >= state.sq_buf_edge)
    {
        state.sq_buf_tail = state.sq_buf;
    }
    if(state.sq_buf_cen >= state.sq_buf_edge)
    {
        state.sq_buf_cen = state.sq_buf;
    }
}

/** Compensates the newly-inserted head frame in the main buffer for DC
    offset (if specified), runs it through the high-pass filter (if
    enabled),  also updates @c sq_buf[] (for computing the running RMS
    level). @a state.main_buf_head[] should point to the sample to be
    processed. Call this function once per processing cycle. */
static void filter_new_frame(void)
{
    /* c: Current channel during loop iteration */
    int c;

    for(c = 0; c < state.numchannels; c++)
    {
        state.x_sq_ttl[c] -= state.sq_buf_head[c];
        state.main_buf_head[c] += options.dc_offset[c];
        state.hpf_out[c] = state.alpha * (state.main_buf_head[c] - state.hpf_prev_rej[c]);
        state.hpf_rej[c] = state.main_buf_head[c] - state.hpf_out[c];
        state.hpf_prev_out[c] = state.hpf_out[c];
        state.hpf_prev_rej[c] = state.hpf_rej[c];
        if(options.high_pass_filter_enabled)
        {
            /* While we still compute HPF output anyway, it remains
               "on the side" unless the user has actually asked for
               HPF to be enabled, in which case we munge the audio.
               This allows analyser to compute real DC offset. */
            state.main_buf_head[c] = state.hpf_out[c];
        }
        else
        {
            /* Collect residuals of low frequency components to compute DC offset */
            state.hpf_rej_ttl[c] += state.hpf_rej[c];
        }
        state.sq_buf_head[c] = state.main_buf_head[c] * state.main_buf_head[c];
        state.x_sq_ttl[c] += state.sq_buf_head[c];
    }
    state.frames_proc_ttl++;
}

/** Computes running RMS level for the newly-inserted head frame in the
    main buffer. @a state.main_buf_head[] should point to the sample to
    be processed. x_sq_ttl[] should be already updated through
    #filter_new_frame. Call this function once per processing cycle. */
static void analyse_new_frame(void)
{
    /* c: Current channel during loop iteration */
    int c;

    for(c = 0; c < state.numchannels; c++)
    {
        state.cur_rms[c] = sqrt(state.x_sq_ttl[c] / (double)state.rms_window_len);
        state.rms_ttl[c] += state.cur_rms[c];
        state.min_rms[c] = fmin(state.min_rms[c], state.cur_rms[c]);
        state.max_rms[c] = fmax(state.max_rms[c], state.cur_rms[c]);
        state.pos_peak[c] = fmax(state.pos_peak[c], state.main_buf_cen[c]);
        state.neg_peak[c] = fmin(state.neg_peak[c], state.main_buf_cen[c]);
    }
}
    
/** Retrieves another frame from the input file, and places it into the
    head of the main buffer. The tail entry in the buffer will be
    discarded. The @a sq_buf buffer will be updated accordingly, and the
    incoming frame will be filtered through the high-pass filter if
    enabled. If analysis mode is selected, the statistics will be
    updated as well.

    @return @c TRUE if more frames remaining in the input stream; @c
    FALSE otherwise (any further attempts to call this function will
    only produce padded-out silence). */
static int fetch_next_frame(void)
{
    /* eof: Return result, set flag if no more frames left to process. */
    /* rdcnt: Number of frames successfully read (1 if OK, 0 if EOF or -1 if error) */
    int eof = FALSE;
    int rdcnt;

    /* Replace tail frame in buffer with new incoming frame */
    if(!state.in_eof && state.frames_remaining > 0)
    {
        /* Attempt to read next frame */
        state.frames_remaining--;
        rdcnt = sf_readf_double(state.in_file, state.main_buf_tail, 1);
        if(rdcnt < 0)
        {
            error(EXIT_FAILURE, errno, "Error while reading input file `%s'",
                options.in_file_name);
        }
        else if(rdcnt == 0)
        {
            /* End of input file; pad out buffer with zero-silence frames. */
            state.in_eof = TRUE;
            if(state.ra_frame_cnt < state.frames_remaining)
            {
                state.frames_remaining = state.ra_frame_cnt;
            }
            memset(state.main_buf_tail, 0, state.frame_sz);
        }
        else
        {
            state.frames_read_ttl++;
        }
    }
    else
    {
        /* Pad out EOF overshoot with silence */
        memset(state.main_buf_tail, 0, state.frame_sz);
        if(state.frames_remaining > 0)
        {
            state.frames_remaining--;
        }
        else
        {
            eof = TRUE;
        }
    }
    
    state.cur_frame_pos++;
    advance_buf_ptrs();
    filter_new_frame();
    return !eof;
}

/** Appends central frame in main buffer to lead-in buffer */
static void leadin_buf_add(void)
{
    if(options.cut_point_action == CPA_EXTRACT_TRACK)
    {
        if(state.leadin_buf_end < state.leadin_buf_edge)
        {
            memcpy(state.leadin_buf_end, state.main_buf_cen, state.frame_sz);
            state.leadin_buf_end += state.numchannels;
        }
        else
        {
            error(0, 0, "warning: lead-in buffer is overflowing");
        }
    }
}

/** Commits contents of the lead-in buffer to the current output file */
static void leadin_buf_commit(void)
{
    if(options.cut_point_action == CPA_EXTRACT_TRACK)
    {
        int num_frames = (state.leadin_buf_end - state.leadin_buf) / state.numchannels;
        if(sf_writef_double(state.out_file, state.leadin_buf, num_frames) < num_frames)
        {
            error(EXIT_FAILURE, 0, "Unable to write to output file `%s': %s",
                state.out_file_name, sf_strerror(state.out_file));
        }
    }
}

/** Purges the lead-in buffer (when a stint of non-silence after a long
    gap of silence is found to be an audio glitch instead). */
static void leadin_buf_purge(void)
{
    if(options.cut_point_action == CPA_EXTRACT_TRACK)
    {
        state.leadin_buf_end = state.leadin_buf;
    }
}

/** Opens the input recording file, and seeks to the starting position if necessary. */
static void open_input_file(void)
{
    if(options.in_file_name)
    {
        state.in_file = sf_open(options.in_file_name, SFM_READ, &options.in_sfinfo);
        if(!state.in_file)
        {
            error(EXIT_FAILURE, 0, "Unable to open `%s': %s", 
                options.in_file_name, sf_strerror(NULL));
        }
    }
    else
    {
        options.in_file_name = stdin_description;
        state.in_file = sf_open_fd(STDIN_FILENO, SFM_READ, &options.in_sfinfo, TRUE);
        if(!state.in_file)
        {
            error(EXIT_FAILURE, 0, "Unable to identify file structure on standard input: %s", 
                sf_strerror(NULL));
        }
    }
    verbose("Opened input file `%s'", options.in_file_name);
    state.samplerate = options.in_sfinfo.samplerate;
    state.numchannels = options.in_sfinfo.channels;
    state.frame_sz = sizeof(double) * state.numchannels;
    verbose("libsndfile file format code: %#08x", options.in_sfinfo.format);
    verbose("Sampling rate: %dHz", options.in_sfinfo.samplerate);
    verbose("Number of channels: %d", options.in_sfinfo.channels);
    if(options.time_range_given)
    {
        /* Convert time range to frame range, now that sample rate is known. */
        options.start_frame_idx = (sf_count_t)(options.start_time * (double)state.samplerate);
        options.end_frame_idx = (options.end_time < INFINITY)
            ? (sf_count_t)(options.end_time * (double)state.samplerate)
            : SF_COUNT_MAX;
        verbose("Translated time range %.5f-%.5f to frame indices %lld-%lld",
            options.start_time, options.end_time, options.start_frame_idx, options.end_frame_idx);
    }
    if(options.start_frame_idx > 0)
    {
        /* Reposition input file to starting frame if not zero */
        if(sf_seek(state.in_file, options.start_frame_idx, SEEK_SET) < 0)
        {
            error(EXIT_FAILURE, 0, "Unable to reposition input to frame %lld: %s",
                options.start_frame_idx, sf_strerror(state.in_file));
        }
        verbose("Repositioned input to frame %lld", options.start_frame_idx);
    }
    state.cur_frame_pos = options.start_frame_idx;
    state.frames_remaining = options.end_frame_idx - options.start_frame_idx;
}

/** Opens the track names file given, and skips through leading entries if needed. */
static void open_track_names_file(void)
{
    if(strcmp(options.track_names_file_name, stdin_file_name) != 0)
    {
        state.track_names_file = fopen(options.track_names_file_name, "r");
        if(!state.track_names_file)
        {
            error(EXIT_FAILURE, errno, "Unable to open track names file `%s'",
                options.track_names_file_name);
        }
    }
    else
    {
        state.track_names_file = stdin;
        options.track_names_file_name = stdin_description;
    }
    verbose("Opened track names file `%s'", options.track_names_file_name);

    /* Need to consume lines if starting from track > 1 */
    for(state.cur_track_num = 1; 
        state.cur_track_num < options.track_num_start && !feof(state.track_names_file); 
        state.cur_track_num++)
    {
        int x = fgetc(state.track_names_file);
        while(x != '\n' && x != EOF)
        {
            x = fgetc(state.track_names_file);
        }
        if(ferror(state.track_names_file))
        {
            error(EXIT_FAILURE, errno, "Error while reading track names file `%s'", 
                options.track_names_file_name);
        }
    }
    if(feof(state.track_names_file))
    {
        /* If we've already exhausted all the track names before
           we start, then close off the input file and pretend
           we never heard of it. All further tracks will be numbered only. */
        fclose(state.track_names_file);
        state.cur_track_num = options.track_num_start;
        state.track_names_file = NULL;
    }
}

/** Creates cuts log file */
static void create_cuts_file(void)
{
    if(options.cuts_file_name && strcmp(options.cuts_file_name, stdout_file_name) != 0)
    {
        state.cuts_file = fopen(options.cuts_file_name, "w");
        if(!state.cuts_file)
        {
            error(EXIT_FAILURE, errno, "Unable to create cuts file `%s'",
                options.cuts_file_name);
        }
    }
    else
    {
        state.cuts_file = stdout;
        options.cuts_file_name = stdout_description;
    }
    setvbuf(state.cuts_file, NULL, _IOLBF, BUFSIZ);
    print_cuts_header();
    verbose("Opened cuts file `%s'", options.cuts_file_name);
}

/** Retrieves next track name into @a state.cur_track_name. @a
    state.cur_track_name will be set to @c NULL if no track name is
    available. */
static void fetch_next_track_name(void)
{
    if(state.track_names_file)
    {
        /* name_len: Length of name returned by getline() */
        int name_len = getline(&state.cur_track_name, &state.cur_track_name_sz, state.track_names_file);
        
        if(name_len > 0)
        {
            while(name_len > 0 && isspace(state.cur_track_name[name_len - 1]))
            {
                /* Trim trailing whitespace, including terminating newline. */
                name_len--;
            }
            state.cur_track_name[name_len] = 0;
        }
        else if(feof(state.track_names_file))
        {
            /* No more names left - close track name file, rest of tracks shall be numbered. */
            fclose(state.track_names_file);
            state.track_names_file = NULL;
            if(state.cur_track_name)
            {
                free(state.cur_track_name);
                state.cur_track_name = NULL;
                state.cur_track_name_sz = 0;
            }
        }
        else if(ferror(state.track_names_file))
        {
            error(EXIT_FAILURE, errno, "Unable to read track names file `%s'",
                options.track_names_file_name);
        }
    }
}

/** Creates a new output file for the current track */
static void create_new_out_file(void)
{
    /* sf_info: Used for specifying parameters of output file */
    /* extension: File extension used for output file (minus leading period) */
    SF_INFO sf_info;
    const char *extension = NULL;

    fetch_next_track_name();
    sf_info.format = options.out_sfinfo_format
        ? options.out_sfinfo_format
        : options.in_sfinfo.format;
    sf_info.format = (sf_info.format & SF_FORMAT_TYPEMASK)
        | (options.in_sfinfo.format & (SF_FORMAT_SUBMASK | SF_FORMAT_ENDMASK));
    {
        /* This is silly. There should be an easier way of obtaining an extension from an SF_FORMAT_xxx value.*/
        int cur_format;
        int num_formats;
    
        sf_command(NULL, SFC_GET_FORMAT_MAJOR_COUNT, &num_formats, sizeof(int));
        for(cur_format = 0; cur_format <= num_formats; cur_format++)
        {
            SF_FORMAT_INFO sf_format_info;
            sf_format_info.format = cur_format;
            if(sf_command(NULL, SFC_GET_FORMAT_MAJOR, &sf_format_info, sizeof(SF_FORMAT_INFO)) == 0
                && (sf_format_info.format & SF_FORMAT_TYPEMASK) == (sf_info.format & SF_FORMAT_TYPEMASK))
            {
                extension = sf_format_info.extension;
            }
        }
        if(!extension)
        {
            error(EXIT_FAILURE, 0, "Cannot determine extension for libsndfile filetype %#08x",
                options.in_sfinfo.format);
        }
    }
    
    if(state.cur_track_name && state.cur_track_name[0])
    {
        state.out_file_name = malloc(strlen(state.cur_track_name) + strlen(extension) + 2);
        sprintf(state.out_file_name, "%s.%s", state.cur_track_name, extension);
    }
    else
    {
        state.out_file_name = malloc(strlen(extension) + 10);
        sprintf(state.out_file_name, "%08d.%s", state.cur_track_num, extension);
    }
    sf_info.samplerate = state.samplerate;
    sf_info.channels = state.numchannels;
    state.out_file = sf_open(state.out_file_name, SFM_WRITE, &sf_info);
    if(!state.out_file)
    {
        error(EXIT_FAILURE, 0, "Unable to create new track file `%s': %s",
            state.out_file_name, sf_strerror(NULL));
    }
    leadin_buf_commit();
    leadin_buf_purge();
    if(options.verbose)
    {
        char cur_track_start_s[TIMECODE_STR_SZ];
        verbose("Creating `%s' starting @ frame index %lld (%s)",
            state.out_file_name, state.cur_track_start, 
            render_frame_idx_as_timecode(cur_track_start_s, state.cur_track_start));
    }
}

/** Closes the current track output file */
static void close_out_file(void)
{
    if(state.out_file)
    {
        if(options.verbose)
        {
            sf_count_t duration = state.cur_frame_pos - state.cur_track_start;
            char cur_track_end_s[TIMECODE_STR_SZ];
            char duration_s[TIMECODE_STR_SZ];
            verbose("Completed `%s' ending @ frame index %lld (%s), duration %lld frames (%s)",
                state.out_file_name, state.cur_frame_pos,
                render_frame_idx_as_timecode(cur_track_end_s, state.cur_frame_pos),
                duration,
                render_frame_idx_as_timecode(duration_s, duration));
        }
        sf_close(state.out_file);
        state.out_file = NULL;
        free(state.out_file_name);
        state.out_file_name = NULL;
    }
}

/** Force conclusion of current track (when EOF is encountered, etc) */
static void force_end_of_track(void)
{
    if(options.cut_point_action == CPA_LOG_POINT)
    {
        if(state.cut_context == CCTX_TRACK || state.cut_context == CCTX_TRACK_ENDING)
        {
            print_track_cut();
        }
    }
    else if(options.cut_point_action == CPA_EXTRACT_TRACK)
    {
        close_out_file();
    }
    /* FIXME: Find out why the track name is sometimes re-used if we run out of names. */
    if(state.cur_track_name)
    {
        state.cur_track_name[0] = 0;
    }
}

/** Commit current frame to output file or cutting sheet, depending on cutting mode. */
static void commit_current_frame(void)
{
    if(options.cut_point_action == CPA_LOG_POINT)
    {
        /* This block intentionally left blank */
    }
    else if(options.cut_point_action == CPA_EXTRACT_TRACK)
    {
        if(sf_writef_double(state.out_file, state.main_buf_cen, 1) < 1)
        {
            error(EXIT_FAILURE, 0, "Unable to write to output file `%s': %s",
                state.out_file_name, sf_strerror(state.out_file));
        }
    }
}

/** In cutting mode, makes a decision on the cutting context state,
    based on the RMS level of the current frame. */
static void update_context(void)
{
    switch(state.cut_context)
    {
        case CCTX_SILENCE:
            if(we_have_signal())
            {
                state.cut_context = CCTX_TRACK_STARTING;
                state.time_to_live = state.min_signal_len - 1;
                state.cur_track_start = state.cur_frame_pos;
                leadin_buf_add();
            }
            break;
        case CCTX_TRACK_STARTING:
            if(!we_have_signal())
            {
                leadin_buf_purge();
                state.cut_context = CCTX_SILENCE;
                if(options.verbose)
                {
                    char cur_track_start_time_s[TIMECODE_STR_SZ];
                    char cur_pos_time_s[TIMECODE_STR_SZ];
                    verbose("False positive of %lld frames (%dms) between frame range %lld-%lld (%s-%s)",
                        state.cur_frame_pos - state.cur_track_start,
                        (int)(((state.cur_frame_pos - state.cur_track_start) * 1000) / state.samplerate),
                        state.cur_track_start, state.cur_frame_pos,
                        render_frame_idx_as_timecode(cur_track_start_time_s, state.cur_track_start),
                        render_frame_idx_as_timecode(cur_pos_time_s, state.cur_frame_pos));
                }
            }
            else if(state.time_to_live > 0)
            {
                leadin_buf_add();
                state.time_to_live--;
            }
            /* We've found the start of a track */
            else
            {
                state.cut_context = CCTX_TRACK;
                if(options.cut_point_action == CPA_LOG_POINT)
                {
                    fetch_next_track_name();
                }
                else if(options.cut_point_action == CPA_EXTRACT_TRACK)
                {
                    create_new_out_file();
                }
                commit_current_frame();
            }
            break;
        case CCTX_TRACK_ENDING:
            commit_current_frame();
            if(we_have_signal())
            {
                state.cut_context = CCTX_TRACK;
            }
            else if(state.time_to_live > 0)
            {
                state.time_to_live--;
            }
            /* We've found the end of a track */
            else
            {
                force_end_of_track();
                state.cut_context = CCTX_SILENCE;
                state.cur_track_num++;
            }
            break;
        case CCTX_TRACK:
            commit_current_frame();
            if(!we_have_signal() && state.cur_frame_pos >= state.cur_track_start + state.min_track_len)
            {
                state.cut_context = CCTX_TRACK_ENDING;
                state.time_to_live = state.min_silence_len;
            }
            break;
    }
}

/** This function must be called before entering #cutter_loop or #analyser_loop. */
static void init_state(void)
{
    /* c: Current channel in iterative loops */
    /* dt: small-delta-t, interval between frames (in seconds). */
    int c;
    double dt;
    
    memset(&state, 0, sizeof(state));
    state.cur_track_num = 1;

    if(options.verbose)
    {
        char sf_ver_str[128];
        sf_command(NULL, SFC_GET_LIB_VERSION, sf_ver_str, sizeof(sf_ver_str));
        verbose("libsndfile version: %s", sf_ver_str);
    }
    
    open_input_file();
    state.rms_window_len = state.samplerate * RMS_WINDOW_PERIOD / 1000;
    verbose("RMS window is %d frames", state.rms_window_len);
    state.sq_buf = calloc(state.rms_window_len, state.frame_sz);
    state.sq_buf_edge = state.sq_buf + state.rms_window_len * state.numchannels;
    state.sq_buf_cen = state.sq_buf + (state.rms_window_len / 2) * state.numchannels;
    state.main_buf = calloc(state.rms_window_len, state.frame_sz);
    state.main_buf_edge = state.main_buf + state.rms_window_len * state.numchannels;
    state.main_buf_cen = state.main_buf + (state.rms_window_len / 2) * state.numchannels;
    state.ra_frame_cnt = (state.main_buf_edge - state.main_buf_cen) / state.numchannels; 
    verbose("Read-ahead period is %d frames", state.ra_frame_cnt);
    dt = 1.0 / (double)state.samplerate;
    state.alpha = HIGH_PASS_TAU / (HIGH_PASS_TAU + dt);
    verbose("HPF alpha = %lf", state.alpha);
    
    /* Slurp initial half-buffer of frames and prepare RMS buffer and
       filter state. The assertion can be made at this point that no
       wrapping in the queues occur, so we can treat the queues as
       flat arrays. */
    if(sf_readf_double(state.in_file, state.main_buf_cen, state.ra_frame_cnt) < 0)
    {
        error(EXIT_FAILURE, 0, "Cannot read leading part of `%s': %s",
            options.in_file_name, sf_strerror(state.in_file));
    }
    state.frames_read_ttl = state.ra_frame_cnt;
    /* Temporarily borrow the head pointer as a loop counter, since
       filter_new_frame() expects it to point to the newest frame. */
    state.main_buf_head = state.main_buf_cen;
    state.sq_buf_head = state.sq_buf_cen;  
    while(state.main_buf_head < state.main_buf_edge)
    {
        filter_new_frame();
        state.main_buf_head += state.numchannels;
        state.sq_buf_head += state.numchannels;
    }
    state.main_buf_head = state.main_buf_edge - state.numchannels;
    state.main_buf_tail = state.main_buf;
    state.sq_buf_head = state.sq_buf_edge - state.numchannels;
    state.sq_buf_tail = state.sq_buf;
    /* main_buf[] has been DC corrected (and HP-filtered if enabled) */
    /* sq_buf[] is now populated */
    /* x_sq_ttl[] is now available */
    
    if(options.task == TCT_CUTTING)
    {
        double x_nf = exp2(options.noise_floor_dbfs / (20.0 * log10(2)));
        verbose("x_nf = %lf", x_nf);
        state.n_x_nf_sq = x_nf * x_nf * (double)state.rms_window_len;
        verbose("n(x_nf)^2 = %lf", state.n_x_nf_sq);
        state.min_silence_len = state.samplerate * options.min_silence_period / 1000;
        state.min_signal_len = state.samplerate * options.min_signal_period / 1000;
        state.min_track_len = state.samplerate * options.min_track_length;
        verbose("Minimum silence period is %d frames", state.min_silence_len);
        verbose("Minimum signal period is %d frames", state.min_signal_len);
        verbose("Minimum track length is %d frames", state.min_track_len);
        if(options.cut_point_action == CPA_EXTRACT_TRACK)
        {
            state.leadin_buf_len = state.min_signal_len;
            state.leadin_buf = malloc(sizeof(double) * state.numchannels * state.leadin_buf_len);
            state.leadin_buf_edge = state.leadin_buf + state.numchannels * state.leadin_buf_len;
            state.leadin_buf_end = state.leadin_buf;
            verbose("Lead-in buffer is %d frames", state.leadin_buf_len);
        }
        if(options.track_names_file_name)
        {
            open_track_names_file();
        }
        
        if(options.cut_point_action == CPA_LOG_POINT)
        {
            create_cuts_file();
        }
        else if(options.cut_point_action == CPA_EXTRACT_TRACK)
        {
            /* Changing the working directory means that we no longer
               have to worry about path separators in track file names.
               This is only possible once we've finished setting up all
               other input/output files. */
            if(chdir(options.track_directory) < 0)
            {
                error(EXIT_FAILURE, errno, "Unable to change to track directory `%s'",
                    options.track_directory);
            }
            verbose("Changed working directory to `%s'", options.track_directory);
        }
        
        state.cut_context = CCTX_SILENCE;
    }
    else if(options.task == TCT_ANALYSIS)
    {
        for(c = 0; c < state.numchannels; c++)
        {
            state.min_rms[c] = INFINITY;
            state.pos_peak[c] = -INFINITY;
            state.neg_peak[c] = INFINITY;
        }
    }
}

/** Main cutter loop. */
static void cutter_loop(void)
{
    do
    {
        update_context();
    }
    while(fetch_next_frame() && state.cur_track_num <= options.track_num_end);

    if(state.frames_remaining == 0)
    {
        if(options.verbose)
        {
            char cur_time_s[TIMECODE_STR_SZ];
            verbose("End of input reached at frame %lld (%s). Exiting.",
                state.cur_frame_pos, 
                render_frame_idx_as_timecode(cur_time_s, state.cur_frame_pos));
        }
        force_end_of_track();
    }
    else if(state.cur_track_num > options.track_num_end)
    {
        verbose("No more tracks remaining. Exiting.");
    }
}

/** Prints analysis page header, customising it based on number of channels. */
static void print_analysis_header(void)
{
    printf("%-20s", "statistic");
    if(state.numchannels == 1)
    {
        printf("mono_channel\n");
    }
    else if(state.numchannels == 2)
    {
        printf("%20s%20s\n", "left_channel", "right_channel");
    }
    else
    {
        int c;
        for(c = 0; c < state.numchannels; c++)
        {
            printf("channel_%-6d", c);
        }
        printf("\n");
    }
}

static void print_analysis_row(const char *header, const char *format, double *fields)
{
    int c;

    printf("%20s", header);
    for(c = 0; c < state.numchannels; c++)
    {
        char s[21];
        snprintf(s, 21, format, fields[c]);
        printf("%20s", s);
    }
    printf("\n");
}

/** Converts a sample level into a decibel full-scale amount (dbFS).

    @param x Sample level to compute; should be in range [-1.0, +1.0].
    @return dbFS power level (will be negative); negative infinity will
    be returned if @a x is zero. */
static double level_to_dbfs(double x)
{
    return (x != 0.0)
        ? log2(fabs(x)) * (20.0 * log10(2))
        : -INFINITY;
}

/** Prints analysis page to standard output */
static void print_analysis(void)
{
    int c;
    double dc_offset[MAX_CHANNELS];
    double dc_offset_dbfs[MAX_CHANNELS];
    double avg_rms[MAX_CHANNELS];
    double min_rms_dbfs[MAX_CHANNELS];
    double max_rms_dbfs[MAX_CHANNELS];
    double avg_rms_dbfs[MAX_CHANNELS];
    double peak_dbfs[MAX_CHANNELS];

    for(c = 0; c < state.numchannels; c++)
    {
        dc_offset[c] = state.hpf_rej_ttl[c] / (double)state.frames_read_ttl;
        dc_offset_dbfs[c] = level_to_dbfs(dc_offset[c]);
        avg_rms[c] = state.rms_ttl[c] / (double)state.frames_proc_ttl;
        min_rms_dbfs[c] = level_to_dbfs(state.min_rms[c]);
        max_rms_dbfs[c] = level_to_dbfs(state.max_rms[c]);
        avg_rms_dbfs[c] = level_to_dbfs(avg_rms[c]);
        peak_dbfs[c] = fmax(level_to_dbfs(state.pos_peak[c]), level_to_dbfs(state.neg_peak[c]));
    }

    print_analysis_header();
    print_analysis_row("positive_peak", "  %+1.16f", state.pos_peak);
    print_analysis_row("negative_peak", "  %+1.16f", state.neg_peak);
    print_analysis_row("peak_dbfs", "  %+3.14f", peak_dbfs);
    print_analysis_row("min_rms", "  %+1.16f", state.min_rms);
    print_analysis_row("max_rms", "  %+1.16f", state.max_rms);
    print_analysis_row("avg_rms", "  %+1.16f", avg_rms);
    print_analysis_row("min_rms_dbfs", "  %+3.14f", min_rms_dbfs);
    print_analysis_row("max_rms_dbfs", "  %+3.14f", max_rms_dbfs);
    print_analysis_row("avg_rms_dbfs", "  %+3.14f", avg_rms_dbfs);
    print_analysis_row("dc_offset", "  %+1.16f", dc_offset);
    print_analysis_row("dc_offset_dbfs", "  %+3.14f", dc_offset_dbfs);
    {
        char s[MAX_CHANNELS * 16];
        char *s_end;
        int c;
        
        s_end = s + sprintf(s, "%+f", -dc_offset[0]);
        for(c = 1; c < state.numchannels; c++)
        {
            s_end += sprintf(s_end, ",%+f", -dc_offset[c]);
        }
        printf("%20s  --dc-offset=%s\n", "fix_dc_offset_arg", s);
    }
}

/** Main analyser loop */
static void analyser_loop(void)
{
    do
    {
        analyse_new_frame();
    }
    while(fetch_next_frame());
    
    print_analysis();
}

/** This is the main function.

    @param argc Number of command-line arguments, including program name.
    @param argv String array of command-line arguments, including
    program name.
    @returns Program exit status code. */
int main(int argc, char **argv)
{
    /* Parse command-line arguments */
    init_options();
    options.argc = argc;
    options.argv = argv;
    parse_options();

    if(/*options.verbose*/0)
    {
        dump_options();
    }

    init_state();
    if(options.task == TCT_CUTTING)
    {
        cutter_loop();
    }
    else if(options.task == TCT_ANALYSIS)
    {
        analyser_loop();
    }
    
    /* Return success exit status */
    return EXIT_SUCCESS;
}
