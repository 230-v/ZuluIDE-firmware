/**
 * Copyright (C) 2023 saybur
 * Copyright (C) 2024 Rabbit Hole Computing LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
**/

#ifdef ENABLE_AUDIO_OUTPUT

#include <SdFat.h>
#include <stdbool.h>
#include <hardware/dma.h>
#include <hardware/irq.h>
#include <pico/multicore.h>
#include "audio.h"
#include <CUEParser.h>
#include "ZuluIDE_audio.h"
#include "ZuluIDE_config.h"
#include "ZuluIDE_log.h"
#include "ZuluIDE_platform.h"
#include "ide_imagefile.h"
#include "ide_atapi.h"
#include <ZuluI2S.h>


extern SdFs SD;

I2S i2s;

static FsFile audio_parent;
static FsFile audio_file;
static CUEParser * g_cue_parser = nullptr;
// True is using the same filenames for the bin/cue, false if using a directory with multiple bin/wav files
static bool single_bin_file = false;
// DMA configuration info
static dma_channel_config snd_dma_a_cfg;
static dma_channel_config snd_dma_b_cfg;

// some chonky buffers to store audio samples,
// output and sample buffers are the same memory
#define AUDIO_OUT_BUFFER_SIZE (AUDIO_BUFFER_SIZE / 4)
static uint32_t out_len_a = AUDIO_OUT_BUFFER_SIZE;
static uint32_t out_len_b = AUDIO_OUT_BUFFER_SIZE;
static uint32_t * out_len = &out_len_a;
static uint32_t output_buf_a[AUDIO_OUT_BUFFER_SIZE];
static uint32_t output_buf_b[AUDIO_OUT_BUFFER_SIZE];

static uint8_t *sample_buf_a = (uint8_t*) output_buf_a;
static uint8_t *sample_buf_b = (uint8_t*) output_buf_b;

// tracking for the state of the above buffers
enum bufstate { STALE, FILLING, PROCESSING, READY };
static volatile bufstate sbufst_a = STALE;
static volatile bufstate sbufst_b = STALE;
enum bufselect { A, B };
static bufselect sbufsel = A;


// tracking for audio playback
static bool audio_idle = true;
static bool audio_playing = false;
static volatile bool audio_paused = false;
static uint64_t fpos;
static uint32_t fleft;
static uint64_t gap_length = 0;
static bool last_track_reached = false;
static bool within_gap = false;
static uint32_t gap_read = 0;
static CUETrackInfo current_track = {0};

// historical playback status information
static audio_status_code audio_last_status = ASC_NO_STATUS;
// volume information for targets
static volatile uint8_t volume[2] = {DEFAULT_VOLUME_LEVEL, DEFAULT_VOLUME_LEVEL};
static volatile uint16_t channel = AUDIO_CHANNEL_ENABLE_MASK;

// mechanism for cleanly stopping DMA units
static volatile bool audio_stopping = false;

/*
 * I2S format is directly compatible to CD 16-bit audio with left and right channels
 * The only encoding needed is adjusting the volume and muting if one of the channels
 * is disabled.
 */
static void snd_encode(int16_t* samples, int16_t* output_buf, uint16_t len) {
    uint8_t vol[2] = {volume[0], volume[1]};
    uint16_t chn = channel & AUDIO_CHANNEL_ENABLE_MASK;
    if (!(chn >> 8))   vol[1] = 0;   // right
    if (!(chn & 0xFF)) vol[0] = 0; // left
    int16_t temp = 0;
    for (uint16_t i = 0; i < len; i++ )
    {
        if (samples == nullptr)
            output_buf[i] = 0;
        else
        {

            if (i % 2 == 0)
            {
                temp = output_buf[i+1];
                output_buf[i+1] = (int16_t)(((int32_t)samples[i]) * (vol[0]) / 255);
            }
            else
                output_buf[i-1] = (int16_t)(((int32_t)temp) * (vol[1]) / 255);
        }
    }
}

// functions for passing to Core1
static void snd_process_a() {
    snd_encode((int16_t *)(sample_buf_a), (int16_t*)(output_buf_a), AUDIO_BUFFER_SIZE/2);
}
static void snd_process_b() {
    snd_encode((int16_t *)sample_buf_b, (int16_t*)(output_buf_b), AUDIO_BUFFER_SIZE/2);
}



/**********************************************************************************************
 * Sets up playback via side effect for last_track_reached, within_gap, fpos and fleft, gap_read
 * \param start - start of playback in lba
 * \param length - length of playback in lba
 * \param continued - true if updating values while audio is being played
 *                  - false if setting up for the first time
 **********************************************************************************************/
static bool setup_playback(uint32_t start, uint32_t length, bool continued)
{
    static uint32_t last_length = 0;
    static uint32_t last_start = 0;
    static uint8_t last_track_number = 0;

    if (!continued)
    {
        last_start = start;
        last_length = length;
        last_track_number = 0;
    }

    // read in the first track and report errors
    const CUETrackInfo *find_track_info;

    // Init globals
    within_gap = false;
    last_track_reached = false;
    gap_length = 0;
    gap_read = 0;

    uint64_t file_size = 0;
    CUETrackInfo track_info = {0};
    uint32_t start_of_next_track = 0;
    int file_index = -1;

    g_cue_parser->restart();

    while ((find_track_info = g_cue_parser->next_track(file_size)) != nullptr )
    {

        if (!single_bin_file)
        {
            // opening the file for getting file size
            if (find_track_info->file_index != file_index)
            {
                if (!(audio_parent.isDir() && audio_file.open(&audio_parent, find_track_info->filename, O_RDONLY)))
                {
                    dbgmsg("------ Audio playback - could not open the next track's bin file: ", find_track_info->filename);
                    audio_file.close();
                    return false;
                }
                file_index = find_track_info->file_index;
            }
        }
        file_size = audio_file.size();


        if (continued)
        {
            // looking the next track
            if (find_track_info->track_number < last_track_number + 1)
                continue;
            if (find_track_info->track_number == last_track_number + 1)
            {
                // set start to the new track because the last track has finished
                start = find_track_info->track_start;
            }
        }

        if (start < find_track_info->track_start)
        {
            // start began in the last track, stop looping
            start_of_next_track = find_track_info->track_start;
            break;
        }

        track_info = *find_track_info;

    }

    if (!single_bin_file)
    {
        if (!(audio_parent.isDir() && audio_file.open(&audio_parent, track_info.filename, O_RDONLY)))
        {
            dbgmsg("------ Audio playback - could not open the current track's bin file: ", track_info.filename);
            audio_file.close();
            return false;
        }
    }

    if (find_track_info == nullptr)
    {
        // if the loop completed without breaking
        last_track_reached = true;
        if (track_info.track_number == 0)
        {
            dbgmsg("------ Audio continued playback could not find specified track");
            return false;
        }
    }

    // test if the current or new audio file is open or can be opened
    if (single_bin_file && !audio_file.isOpen())
    {
        dbgmsg("------ Audio playback - CD's bin file is not open");
        return false;
    }

    if (track_info.track_mode != CUETrack_AUDIO)
    {
        dbgmsg("------ Audio playback - track not CD Audio");
        return false;
    }

    if (continued)
    {
        // adjust length for new track
        length = last_length - (start - last_start);
        last_length = length;
        last_start = start;
    }
    last_track_number = track_info.track_number;

    //  find the offset within the current audio file
    uint64_t offset = track_info.file_offset;
    if (start >= track_info.data_start)
    {
        // add to the offset the current playback position
        offset += (start - track_info.data_start) * (uint64_t)track_info.sector_length;
    }
    else if (track_info.unstored_pregap_length != 0 && start >= track_info.data_start - track_info.unstored_pregap_length)
    {
        // Start is within the pregap position, offset is not increased due to no file data is being played
        gap_length = (start - track_info.data_start) *(uint64_t) track_info.sector_length;
        // offset += 0;
        within_gap = true;
        gap_read = 0;
    }
    else
    {
        // Get data from stored pregap (INDEX 0), which is in the file before trackinfo.file_offset.
        uint32_t seek_back = (track_info.data_start - start) * track_info.sector_length;
        if (seek_back > offset)
        {
            logmsg("WARNING: Host attempted CD read at sector ", start, "+", length,
                    " pregap request ", (int)seek_back, " exceeded available ", (int)offset, " for track ", track_info.track_number,
                    " (possible .cue file issue)");
            offset = 0;
            return false;
        }
        else
        {
            offset -= seek_back;
        }
    }

    if (start_of_next_track != 0)
    {
        // There is a next track
        if (start + length < start_of_next_track)
        {
            // playback ends before the next track
            if (within_gap)
                // adjust length unplayed file data within gap
                fleft = (length - track_info.unstored_pregap_length) * (uint64_t)track_info.sector_length;
            else
                fleft = length * (uint64_t)track_info.sector_length;

            last_track_reached = true;
        }
        else
        {
            // playback continues after this track
            if (within_gap)
                fleft = (start_of_next_track - track_info.data_start) * (uint64_t)track_info.sector_length;
            else
                fleft = (start_of_next_track - start) * (uint64_t)track_info.sector_length;
            last_track_reached = false;
        }
    }
    else
    {
        // if playback is with current bin file and there are no more tracks
        volatile uint64_t size_of_playback;
        volatile uint32_t start_lba = start;
        size_of_playback = (start_lba + length - track_info.data_start) * (uint64_t)track_info.sector_length ;
        volatile uint64_t last_track_byte_length = audio_file.size() - track_info.file_offset;
        if (size_of_playback <= last_track_byte_length)
        {
            if (within_gap)
                fleft = (length - (track_info.data_start - start)) * track_info.sector_length;
            else
                fleft = length *  track_info.sector_length;
            last_track_reached = true;
        }
        else
        {
            dbgmsg("------ Audio playback - length ", (int) length ,", beyond the last file in cue ");
            return false;
        }
    }
    current_track = track_info;
    fpos = offset;
    return true;
}

/* ------------------------------------------------------------------------ */
/* ---------- VISIBLE FUNCTIONS ------------------------------------------- */
/* ------------------------------------------------------------------------ */
extern "C"
{
static void audio_dma_irq() {
    if (dma_hw->intr & (1 << SOUND_DMA_CHA)) {
        dma_hw->ints0 = (1 << SOUND_DMA_CHA);
        sbufst_a = STALE;
        if (audio_stopping) {
            channel_config_set_chain_to(&snd_dma_a_cfg, SOUND_DMA_CHA);
        }
        dma_channel_configure(SOUND_DMA_CHA,
                &snd_dma_a_cfg,
                i2s.getPioFIFOAddr(),
                output_buf_a,
                out_len_a / 4,
                false);
    } else if (dma_hw->intr & (1 << SOUND_DMA_CHB)) {
        dma_hw->ints0 = (1 << SOUND_DMA_CHB);
        sbufst_b = STALE;
        if (audio_stopping) {
            channel_config_set_chain_to(&snd_dma_b_cfg, SOUND_DMA_CHB);
        }
        dma_channel_configure(SOUND_DMA_CHB,
                &snd_dma_b_cfg,
                i2s.getPioFIFOAddr(),
                output_buf_b,
                out_len_b / 4,
                false);
    }
}
}
bool audio_is_active() {
    return !audio_idle;
}

bool audio_is_playing() {
    return audio_playing;
}

void audio_init() {
    // setup Arduino-Pico I2S library
    i2s.setBCLK(GPIO_I2S_BCLK);
    i2s.setDATA(GPIO_I2S_DOUT);
    i2s.setBitsPerSample(16);
    // 44.1KHz to the nearest integer with a sys clk of 135.43MHz and 2 x 16-bit samples with the pio clock running 2x I2S clock
    // 135.43Mhz / 16 / 2 / 2 / 44.1KHz = 47.98 ~= 48
    i2s.setDivider(48, 0);
    i2s.begin(I2S_PIO_HW, I2S_PIO_SM);
    dma_channel_claim(SOUND_DMA_CHA);
	dma_channel_claim(SOUND_DMA_CHB);

    irq_set_exclusive_handler(DMA_IRQ_0, audio_dma_irq);
    irq_set_enabled(DMA_IRQ_0, true);
}


void audio_poll() {
    if (audio_idle) return;

    static bool set_pause_buf = true;
    if (audio_paused)
    {
        if (set_pause_buf)
        {
            memset(output_buf_a, 0, sizeof(output_buf_a));
            memset(output_buf_b, 0, sizeof(output_buf_b));
        }
        set_pause_buf = false;
        return;
    }
    set_pause_buf = true;


    if (last_track_reached && fleft == 0 && sbufst_a == STALE && sbufst_b == STALE) {
        // out of data and ready to stop
        audio_stop();
        return;
    } else if (last_track_reached && fleft == 0) {
        // out of data to read but still working on remainder
        return;
    } else if (!audio_file.isOpen()) {
        // closed elsewhere, maybe disk ejected?
        dbgmsg("------ Playback stop due to closed file");
        audio_stop();
        return;
    }


    if (fleft == 0)
    {
        if (!setup_playback(0, 0, true))
        {
            dbgmsg("------ Playback stopped because of error loading next track");
            audio_stop();
            return;
        }
    }


    // are new audio samples needed from the memory card?
    uint8_t* audiobuf;
    if (sbufst_a == STALE) {
        sbufst_a = FILLING;
        audiobuf = sample_buf_a;
        out_len = &out_len_a;
    } else if (sbufst_b == STALE) {
        sbufst_b = FILLING;
        audiobuf = sample_buf_b;
        out_len = &out_len_b;
    } else {
        // no data needed this time
        return;
    }


    platform_set_sd_callback(NULL, NULL);
    uint16_t toRead = AUDIO_BUFFER_SIZE;
    uint16_t gap_to_read = AUDIO_BUFFER_SIZE;
    if (within_gap)
    {
        if (gap_length < gap_to_read) gap_to_read = gap_length;
        memset(audiobuf, 0, AUDIO_BUFFER_SIZE);
        gap_read += gap_to_read;
        *out_len = gap_to_read;
        if (gap_read >= gap_length)
        {
            within_gap = false;
            gap_read = 0;
            gap_length = 0;
        }
    }
    else
    {
        if (fleft < toRead) toRead = fleft;
        if (audio_file.position() != fpos) {
            // should be uncommon due to SCSI command restrictions on devices
            // playing audio; if this is showing up in logs a different approach
            // will be needed to avoid seek performance issues on FAT32 vols
            dbgmsg("------ Audio seek required");
            if (!audio_file.seek(fpos)) {
                logmsg("------ Audio error, unable to seek to ", fpos);
            }
        }
        if (audio_file.read(audiobuf, toRead) != toRead) {
            logmsg("------ Audio sample data read error");
        }
        *out_len = toRead;
        fpos += toRead;
        fleft -= toRead;
    }


    if (sbufst_a == FILLING) {
        sbufst_a = PROCESSING;
        snd_process_a();
        sbufst_a = READY;
    } else if (sbufst_b == FILLING) {
        sbufst_b = PROCESSING;
        snd_process_b();
        sbufst_b = READY;
    }
}

bool audio_play(uint32_t start, uint32_t length, bool swap) {
    // stop any existing playback first
    if (!audio_idle) audio_stop();

    // dbgmsg("Request to play ('", file, "':", start, ":", end, ")");

    // verify audio file is present and inputs are (somewhat) sane
    platform_set_sd_callback(NULL, NULL);

    // truncate playback end to end of file
    // we will not consider this to be an error at the moment
    // \todo reimplement
    // if (end > len) {
    //     dbgmsg("------ Truncate audio play request end ", end, " to file size ", len);
    //     end = len;
    //

    if(!setup_playback(start, length, false))
        return false;

    if (length == 0)
    {
        audio_last_status = ASC_NO_STATUS;
        audio_paused = false;
        audio_playing = false;
        audio_idle = true;
        return true;
    }

    audio_last_status = ASC_PLAYING;
    audio_paused = false;
    audio_playing = true;
    audio_idle = false;

    // read in initial sample buffers
    if (within_gap)
    {
        sbufst_a = READY;
        sbufst_b = READY;
        memset(output_buf_a, 0, sizeof(output_buf_a));
        memset(output_buf_b, 0, sizeof(output_buf_b));
    }
    else
    {
        sbufst_a = STALE;
        sbufst_b = STALE;
        sbufsel = B;
        audio_poll();
        sbufsel = A;
        audio_poll();
    }
    // setup the two DMA units to hand-off to each other
    // to maintain a stable bitstream these need to run without interruption
	snd_dma_a_cfg = dma_channel_get_default_config(SOUND_DMA_CHA);
	channel_config_set_transfer_data_size(&snd_dma_a_cfg, DMA_SIZE_32);
	channel_config_set_dreq(&snd_dma_a_cfg, i2s.getPioDreq());
	channel_config_set_read_increment(&snd_dma_a_cfg, true);
	channel_config_set_chain_to(&snd_dma_a_cfg, SOUND_DMA_CHB);
    // version of pico-sdk lacks channel_config_set_high_priority()
    snd_dma_a_cfg.ctrl |= DMA_CH0_CTRL_TRIG_HIGH_PRIORITY_BITS;
	dma_channel_configure(SOUND_DMA_CHA, &snd_dma_a_cfg, i2s.getPioFIFOAddr(),
			output_buf_a, AUDIO_OUT_BUFFER_SIZE, false);
    dma_channel_set_irq0_enabled(SOUND_DMA_CHA, true);
	snd_dma_b_cfg = dma_channel_get_default_config(SOUND_DMA_CHB);
	channel_config_set_transfer_data_size(&snd_dma_b_cfg, DMA_SIZE_32);
	channel_config_set_dreq(&snd_dma_b_cfg, i2s.getPioDreq());
	channel_config_set_read_increment(&snd_dma_b_cfg, true);
	channel_config_set_chain_to(&snd_dma_b_cfg, SOUND_DMA_CHA);
    snd_dma_b_cfg.ctrl |= DMA_CH0_CTRL_TRIG_HIGH_PRIORITY_BITS;
	dma_channel_configure(SOUND_DMA_CHB, &snd_dma_b_cfg, i2s.getPioFIFOAddr(),
			output_buf_b, AUDIO_OUT_BUFFER_SIZE, false);
    dma_channel_set_irq0_enabled(SOUND_DMA_CHB, true);

    // ready to go
    dma_channel_start(SOUND_DMA_CHA);
    return true;
}

bool audio_set_paused(bool paused) {
    if (audio_idle) return false;
    else if (audio_paused && paused) return false;
    else if (!audio_paused && !paused) return false;

    audio_paused = paused;

    if (paused) {
        audio_last_status = ASC_PAUSED;
    } else {
        audio_last_status = ASC_PLAYING;
    }
    return true;
}

void audio_stop() {
    if (audio_idle) return;

    memset(&current_track, 0, sizeof(current_track));
    memset(output_buf_a, 0, sizeof(output_buf_a));
    memset(output_buf_b, 0, sizeof(output_buf_b));

    // then indicate that the streams should no longer chain to one another
    // and wait for them to shut down naturally
    audio_stopping = true;
    while (dma_channel_is_busy(SOUND_DMA_CHA)) tight_loop_contents();
    while (dma_channel_is_busy(SOUND_DMA_CHB)) tight_loop_contents();
    // \todo check if I2S pio is done
    // The way to check is the I2S pio is done would be to check
    // if the fifo is empty and the PIO's program counter is at the first instruction
    // while (spi_is_busy(AUDIO_SPI)) tight_loop_contents();
    audio_stopping = false;
    dma_channel_abort(SOUND_DMA_CHA);
    dma_channel_abort(SOUND_DMA_CHB);
    // idle the subsystem
    audio_last_status = ASC_COMPLETED;
    audio_paused = false;
    audio_playing = false;
    audio_idle = true;
}

audio_status_code audio_get_status_code() {
    audio_status_code tmp = audio_last_status;
    if (tmp == ASC_COMPLETED || tmp == ASC_ERRORED) {
        audio_last_status = ASC_NO_STATUS;
    }
    return tmp;
}

uint16_t audio_get_volume() {
    return volume[0] | (volume[1] << 8);
}

void audio_set_volume(uint8_t lvol, uint8_t rvol) {
    volume[0] = lvol;
    volume[1] = rvol;
}

uint16_t audio_get_channel() {
    return channel;
}

void audio_set_channel(uint16_t chn) {
    channel = chn;
}

uint32_t audio_get_lba_position()
{
    if (audio_is_active() && current_track.track_number != 0 && audio_file.isOpen())
    {
        return current_track.data_start + (audio_file.position() - current_track.file_offset) / current_track.sector_length;
    }
    else
    {
        return 0;
    }
}


void audio_set_cue_parser(CUEParser *cue_parser, FsFile* file)
{
    g_cue_parser = cue_parser;
    if (file != nullptr)
    {
        char filename[MAX_FILE_PATH] = {0};
        if (file->isFile())
        {
            file->getName(filename, sizeof(filename));
            audio_file.open(filename, O_RDONLY);
            single_bin_file = true;
        }
        else if (file->isDir())
        {
            file->getName(filename, sizeof(filename));
            audio_parent.open(filename, O_RDONLY);
            single_bin_file = false;
        }
    }
}
#endif // ENABLE_AUDIO_OUTPUT