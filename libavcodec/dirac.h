/*
 * Copyright (C) 2007 Marco Gerards <marco@gnu.org>
 * Copyright (C) 2009 David Conrad
 * Copyright (C) 2011 Jordi Ortiz
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVCODEC_DIRAC_H
#define AVCODEC_DIRAC_H

/**
 * @file
 * Interface to Dirac Decoder/Encoder
 * Added data structure definitions
 * @author Marco Gerards <marco@gnu.org>
 * @author Jordi Ortiz <nenjordi@gmail.com>
 */

#include "avcodec.h"
#include "get_bits.h"

#include "dwt.h"
#include "dsputil.h"
#include "diracdsp.h"

#include "dirac_arith.h"

typedef struct {
    unsigned width;
    unsigned height;
    uint8_t chroma_format;          ///< 0: 444  1: 422  2: 420

    uint8_t interlaced;
    uint8_t top_field_first;

    uint8_t frame_rate_index;       ///< index into dirac_frame_rate[]
    uint8_t aspect_ratio_index;     ///< index into dirac_aspect_ratio[]

    uint16_t clean_width;
    uint16_t clean_height;
    uint16_t clean_left_offset;
    uint16_t clean_right_offset;

    uint8_t pixel_range_index;      ///< index into dirac_pixel_range_presets[]
    uint8_t color_spec_index;       ///< index into dirac_color_spec_presets[]

  /*dirac encoder*/
  AVRational frame_rate;
  AVRational aspect_ratio;
} dirac_source_params;

int ff_dirac_parse_sequence_header(AVCodecContext *avctx, GetBitContext *gb,
                                   dirac_source_params *source);

/*Jordi*/

#define MAX_DWT_LEVELS 5

/**
 * The spec limits this to 3 for frame coding, but in practice can be as high as 6
 */
#define MAX_REFERENCE_FRAMES 8
#define MAX_DELAY 5         ///< limit for main profile for frame coding (TODO: field coding)
#define MAX_FRAMES (MAX_REFERENCE_FRAMES + MAX_DELAY + 1)
#define MAX_QUANT 68        ///< max quant for VC-2
#define MAX_BLOCKSIZE 32    ///< maximum xblen/yblen we support

/**
 * DiracBlock->ref flags, if set then the block does MC from the given ref
 */
#define DIRAC_REF_MASK_REF1   1
#define DIRAC_REF_MASK_REF2   2
#define DIRAC_REF_MASK_GLOBAL 4


typedef struct {
    FF_COMMON_FRAME

    int interpolated[3];    ///< 1 if hpel[] is valid
    uint8_t *hpel[3][4];
    uint8_t *hpel_base[3][4];
} DiracFrame;

typedef struct {
    union {
        int16_t mv[2][2];
        int16_t dc[3];
    } u; // anonymous unions aren't in C99 :(
    uint8_t ref;
} DiracBlock;

typedef struct SubBand {
    int level;
    int orientation;
    int stride;
    int width;
    int height;
    int quant;
    IDWTELEM *ibuf;
    struct SubBand *parent;

    // for low delay
    unsigned length;
    const uint8_t *coeff_data;
} SubBand;

typedef struct Plane {
    int width;
    int height;
    int stride;


  /*Encoder vars removed by Yuvi and needed for encoder*/
    int padded_width;
    int padded_height;
  /*       ---             */
  


    int idwt_width;
    int idwt_height;
    int idwt_stride;
    IDWTELEM *idwt_buf;
    IDWTELEM *idwt_buf_base;
    IDWTELEM *idwt_tmp;

    // block length
    uint8_t xblen;
    uint8_t yblen;
    // block separation (block n+1 starts after this many pixels in block n)
    uint8_t xbsep;
    uint8_t ybsep;
    // amount of overspill on each edge (half of the overlap between blocks)
    uint8_t xoffset;
    uint8_t yoffset;

    SubBand band[MAX_DWT_LEVELS][4];
} Plane;

typedef struct DiracContext {
    AVCodecContext *avctx;
    DSPContext dsp;
    DiracDSPContext diracdsp;
    GetBitContext gb;
    dirac_source_params source;
    int seen_sequence_header;
    int frame_number;           ///< number of the next frame to display
    Plane plane[3];
    int chroma_x_shift;
    int chroma_y_shift;

    int zero_res;               ///< zero residue flag
    int is_arith;               ///< whether coeffs use arith or golomb coding
    int low_delay;              ///< use the low delay syntax
    int globalmc_flag;          ///< use global motion compensation
    int num_refs;               ///< number of reference pictures

    // wavelet decoding
    unsigned wavelet_depth;     ///< depth of the IDWT
    unsigned wavelet_idx;

    /**
     * schroedinger older than 1.0.8 doesn't store
     * quant delta if only one codebook exists in a band
     */
    unsigned old_delta_quant;
    unsigned codeblock_mode;

    struct {
        unsigned width;
        unsigned height;
    } codeblock[MAX_DWT_LEVELS+1];

    struct {
        unsigned num_x;         ///< number of horizontal slices
        unsigned num_y;         ///< number of vertical slices
        AVRational bytes;       ///< average bytes per slice
      uint8_t quant[MAX_DWT_LEVELS][4]; //[DIRAC_STD] E.1
    } lowdelay;

    struct {
        int pan_tilt[2];        ///< pan/tilt vector
        int zrs[2][2];          ///< zoom/rotate/shear matrix
        int perspective[2];     ///< perspective vector
        unsigned zrs_exp;
        unsigned perspective_exp;
    } globalmc[2];

    // motion compensation
  uint8_t mv_precision; //[DIRAC_STD] REFS_WT_PRECISION
  int16_t weight[2]; ////[DIRAC_STD] REF1_WT and REF2_WT
  unsigned weight_log2denom; ////[DIRAC_STD] REFS_WT_PRECISION

    int blwidth;            ///< number of blocks (horizontally)
    int blheight;           ///< number of blocks (vertically)
    int sbwidth;            ///< number of superblocks (horizontally)
    int sbheight;           ///< number of superblocks (vertically)

    uint8_t *sbsplit;
    DiracBlock *blmotion;

    uint8_t *edge_emu_buffer[4];
    uint8_t *edge_emu_buffer_base;

    uint16_t *mctmp;        ///< buffer holding the MC data multipled by OBMC weights
    uint8_t *mcscratch;

    DECLARE_ALIGNED(16, uint8_t, obmc_weight)[3][MAX_BLOCKSIZE*MAX_BLOCKSIZE];

    void (*put_pixels_tab[4])(uint8_t *dst, const uint8_t *src[5], int stride, int h);
    void (*avg_pixels_tab[4])(uint8_t *dst, const uint8_t *src[5], int stride, int h);
    void (*add_obmc)(uint16_t *dst, const uint8_t *src, int stride, const uint8_t *obmc_weight, int yblen);
    dirac_weight_func weight_func;
    dirac_biweight_func biweight_func;

    DiracFrame *current_picture;
    DiracFrame *ref_pics[2];

    DiracFrame *ref_frames[MAX_REFERENCE_FRAMES+1];
    DiracFrame *delay_frames[MAX_DELAY+1];
    DiracFrame all_frames[MAX_FRAMES];


  /*Encoder variables*/
  uint8_t *encodebuf;
  DiracArith arith;

  struct sequence_parameters
  {
    /* Information about the frames.  */
    unsigned int luma_width;                ///< the luma component width
    unsigned int luma_height;               ///< the luma component height
    /** Choma format: 0: 4:4:4, 1: 4:2:2, 2: 4:2:0 */
    unsigned int chroma_format;
    unsigned char video_depth;              ///< depth in bits
    
    /* Calculated:  */
    unsigned int chroma_width;              ///< the chroma component width
    unsigned int chroma_height;             ///< the chroma component height
  }sequence;
} DiracContext;

// [DIRAC_STD] Parse code values. 9.6.1 Table 9.1
enum dirac_parse_code {
    pc_seq_header         = 0x00,
    pc_eos                = 0x10,
    pc_aux_data           = 0x20,
    pc_padding            = 0x30,
};

enum dirac_subband {
    subband_ll = 0,
    subband_hl = 1,
    subband_lh = 2,
    subband_hh = 3
};

static const uint8_t default_qmat[][4][4] = {
    { { 5,  3,  3,  0}, { 0,  4,  4,  1}, { 0,  5,  5,  2}, { 0,  6,  6,  3} },
    { { 4,  2,  2,  0}, { 0,  4,  4,  2}, { 0,  5,  5,  3}, { 0,  7,  7,  5} },
    { { 5,  3,  3,  0}, { 0,  4,  4,  1}, { 0,  5,  5,  2}, { 0,  6,  6,  3} },
    { { 8,  4,  4,  0}, { 0,  4,  4,  0}, { 0,  4,  4,  0}, { 0,  4,  4,  0} },
    { { 8,  4,  4,  0}, { 0,  4,  4,  0}, { 0,  4,  4,  0}, { 0,  4,  4,  0} },
    { { 0,  4,  4,  8}, { 0,  8,  8, 12}, { 0, 13, 13, 17}, { 0, 17, 17, 21} },
    { { 3,  1,  1,  0}, { 0,  4,  4,  2}, { 0,  6,  6,  5}, { 0,  9,  9,  7} },
};

static const int qscale_tab[MAX_QUANT+1] = {
        4,     5,     6,     7,     8,    10,    11,    13,
       16,    19,    23,    27,    32,    38,    45,    54,
       64,    76,    91,   108,   128,   152,   181,   215,
      256,   304,   362,   431,   512,   609,   724,   861,
     1024,  1218,  1448,  1722,  2048,  2435,  2896,  3444,
     4096,  4871,  5793,  6889,  8192,  9742, 11585, 13777,
    16384, 19484, 23170, 27554, 32768, 38968, 46341, 55109,
    65536, 77936
};

static const int qoffset_intra_tab[MAX_QUANT+1] = {
        1,     2,     3,     4,     4,     5,     6,     7,
        8,    10,    12,    14,    16,    19,    23,    27,
       32,    38,    46,    54,    64,    76,    91,   108,
      128,   152,   181,   216,   256,   305,   362,   431,
      512,   609,   724,   861,  1024,  1218,  1448,  1722,
     2048,  2436,  2897,  3445,  4096,  4871,  5793,  6889,
     8192,  9742, 11585, 13777, 16384, 19484, 23171, 27555,
    32768, 38968
};

static const int qoffset_inter_tab[MAX_QUANT+1] = {
        1,     2,     2,     3,     3,     4,     4,     5,
        6,     7,     9,    10,    12,    14,    17,    20,
       24,    29,    34,    41,    48,    57,    68,    81,
       96,   114,   136,   162,   192,   228,   272,   323,
      384,   457,   543,   646,   768,   913,  1086,  1292,
     1536,  1827,  2172,  2583,  3072,  3653,  4344,  5166,
     6144,  7307,  8689, 10333, 12288, 14613, 17378, 20666,
    24576, 29226
};

const struct sequence_parameters dirac_sequence_parameters_defaults[13] = {
  /* Width   Height   Chroma format   Depth  */
  {  640,    480,     2,              8  },
  {  176,    120,     2,              8  },
  {  176,    144,     2,              8  },
  {  352,    240,     2,              8  },
  {  352,    288,     2,              8  },
  {  704,    480,     2,              8  },
  {  704,    576,     2,              8  },
  {  720,    480,     2,              8  },
  {  720,    576,     2,              8  },
  {  1280,   720,     2,              8  },
  {  1920,   1080,    2,              8  },
  {  2048,   1556,    0,              16 },
  {  4096,   3112,    0,              16 }
};

// defaults for source parameters
static const dirac_source_params dirac_source_parameters_defaults[] = {
    { 640,  480,  2, 0, 0, 1,  1, 640,  480,  0, 0, 1, 0 },
    { 176,  120,  2, 0, 0, 9,  2, 176,  120,  0, 0, 1, 1 },
    { 176,  144,  2, 0, 1, 10, 3, 176,  144,  0, 0, 1, 2 },
    { 352,  240,  2, 0, 0, 9,  2, 352,  240,  0, 0, 1, 1 },
    { 352,  288,  2, 0, 1, 10, 3, 352,  288,  0, 0, 1, 2 },
    { 704,  480,  2, 0, 0, 9,  2, 704,  480,  0, 0, 1, 1 },
    { 704,  576,  2, 0, 1, 10, 3, 704,  576,  0, 0, 1, 2 },
    { 720,  480,  1, 1, 0, 4,  2, 704,  480,  8, 0, 3, 1 },
    { 720,  576,  1, 1, 1, 3,  3, 704,  576,  8, 0, 3, 2 },

    { 1280, 720,  1, 0, 1, 7,  1, 1280, 720,  0, 0, 3, 3 },
    { 1280, 720,  1, 0, 1, 6,  1, 1280, 720,  0, 0, 3, 3 },
    { 1920, 1080, 1, 1, 1, 4,  1, 1920, 1080, 0, 0, 3, 3 },
    { 1920, 1080, 1, 1, 1, 3,  1, 1920, 1080, 0, 0, 3, 3 },
    { 1920, 1080, 1, 0, 1, 7,  1, 1920, 1080, 0, 0, 3, 3 },
    { 1920, 1080, 1, 0, 1, 6,  1, 1920, 1080, 0, 0, 3, 3 },
    { 2048, 1080, 0, 0, 1, 2,  1, 2048, 1080, 0, 0, 4, 4 },
    { 4096, 2160, 0, 0, 1, 2,  1, 4096, 2160, 0, 0, 4, 4 },

    { 3840, 2160, 1, 0, 1, 7,  1, 3840, 2160, 0, 0, 3, 3 },
    { 3840, 2160, 1, 0, 1, 6,  1, 3840, 2160, 0, 0, 3, 3 },
    { 7680, 4320, 1, 0, 1, 7,  1, 3840, 2160, 0, 0, 3, 3 },
    { 7680, 4320, 1, 0, 1, 6,  1, 3840, 2160, 0, 0, 3, 3 },
};


#endif /* AVCODEC_DIRAC_H */
