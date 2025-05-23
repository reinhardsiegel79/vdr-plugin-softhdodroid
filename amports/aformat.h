/*
 * Copyright (C) 2010 Amlogic Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */



/**
* @file aformat.h
* @brief  Porting from decoder driver for audio format
* @author Tim Yao <timyao@amlogic.com>
* @version 1.0.0
* @date 2011-02-24
*/
/* Copyright (C) 2007-2011, Amlogic Inc.
* All right reserved
*
*/

/*
 * AMLOGIC Audio/Video streaming port driver.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the named License,
 * or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA
 *
 * Author:  Tim Yao <timyao@amlogic.com>
 *
 */

#ifndef AFORMAT_H
#define AFORMAT_H

enum aformat_e {
    AFORMAT_UNKNOWN = -1,
    AFORMAT_MPEG   = 0,
    AFORMAT_PCM_S16LE = 1,
    AFORMAT_AAC   = 2,
    AFORMAT_AC3   = 3,
    AFORMAT_ALAW = 4,
    AFORMAT_MULAW = 5,
    AFORMAT_DTS = 6,
    AFORMAT_PCM_S16BE = 7,
    AFORMAT_FLAC = 8,
    AFORMAT_COOK = 9,
    AFORMAT_PCM_U8 = 10,
    AFORMAT_ADPCM = 11,
    AFORMAT_AMR  = 12,
    AFORMAT_RAAC  = 13,
    AFORMAT_WMA  = 14,
    AFORMAT_WMAPRO   = 15,
    AFORMAT_PCM_BLURAY  = 16,
    AFORMAT_ALAC  = 17,
    AFORMAT_VORBIS    = 18,
    AFORMAT_AAC_LATM   = 19,
    AFORMAT_APE   = 20,
    AFORMAT_EAC3   = 21,
    AFORMAT_PCM_WIFIDISPLAY = 22,
    AFORMAT_DRA    = 23,
    AFORMAT_SIPR   = 24,
    AFORMAT_TRUEHD = 25,
    AFORMAT_MPEG1  = 26, //AFORMAT_MPEG-->mp3,AFORMAT_MPEG1-->mp1,AFROMAT_MPEG2-->mp2
    AFORMAT_MPEG2  = 27,
    AFORMAT_WMAVOI = 28,
    AFORMAT_WMALOSSLESS =29,
    AFORMAT_OPUS = 30,
    AFORMAT_UNSUPPORT ,
    AFORMAT_MAX

};

#define AUDIO_EXTRA_DATA_SIZE   (8192)
#define IS_AFMT_VALID(afmt) ((afmt > AFORMAT_UNKNOWN) && (afmt < AFORMAT_MAX))

#define IS_AUIDO_NEED_EXT_INFO(afmt) ((afmt == AFORMAT_ADPCM) \
                                 ||(afmt == AFORMAT_VORBIS) \
                                 ||(afmt == AFORMAT_OPUS) \
                                 ||(afmt == AFORMAT_WMA) \
                                 ||(afmt == AFORMAT_WMAPRO) \
                                 ||(afmt == AFORMAT_PCM_S16BE) \
                                 ||(afmt == AFORMAT_PCM_S16LE) \
                                 ||(afmt == AFORMAT_PCM_U8) \
                                 ||(afmt == AFORMAT_PCM_BLURAY) \
                                 ||(afmt == AFORMAT_AMR)\
                                 ||(afmt == AFORMAT_ALAC)\
                                 ||(afmt == AFORMAT_AC3) \
                                 ||(afmt == AFORMAT_EAC3) \
                                 ||(afmt == AFORMAT_APE) \
                                 ||(afmt == AFORMAT_FLAC)\
                                 ||(afmt == AFORMAT_PCM_WIFIDISPLAY) \
                                 ||(afmt == AFORMAT_COOK) \
                                 ||(afmt == AFORMAT_RAAC)) \
                                 ||(afmt == AFORMAT_TRUEHD) \
                                 ||(afmt == AFORMAT_WMAVOI) \
                                 ||(afmt == AFORMAT_WMALOSSLESS)

#define IS_AUDIO_NOT_SUPPORT_EXCEED_2CH(afmt) ((afmt == AFORMAT_RAAC) \
                                        ||(afmt == AFORMAT_COOK) \
                                        /*||(afmt == AFORMAT_FLAC)*/)

#define IS_AUDIO_NOT_SUPPORT_EXCEED_6CH(afmt) ((afmt == AFORMAT_WMAPRO))
#define IS_AUDIO_NOT_SUPPORT_EXCEED_FS48k(afmt) ((afmt == AFORMAT_WMAPRO))


#define IS_AUIDO_NEED_PREFEED_HEADER(afmt) ((afmt == AFORMAT_VORBIS) )
#define IS_AUDIO_NOT_SUPPORTED_BY_AUDIODSP(afmt,codec)  \
                            ((afmt == AFORMAT_AAC_LATM || afmt == AFORMAT_AAC) \
                             &&codec->profile == 0/* FF_PROFILE_AAC_MAIN*/)

#define IS_SUB_NEED_PREFEED_HEADER(sfmt) ((sfmt == CODEC_ID_DVD_SUBTITLE) )

#endif /* AFORMAT_H */

