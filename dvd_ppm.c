#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <getopt.h>
#include <limits.h>
#include <sys/mman.h>
#include <inttypes.h>
#include <errno.h>
#ifdef __linux__
#include <linux/cdrom.h>
#include "dvd_drive.h"
#endif
#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_read.h>
#include "mpeg2dec/mpeg2.h"
#include "mpeg2dec/mpeg2convert.h"
#include "dvd_info.h"
#include "dvd_device.h"
#include "dvd_drive.h"
#include "dvd_vmg_ifo.h"
#include "dvd_track.h"
#include "dvd_chapter.h"
#include "dvd_cell.h"
#include "dvd_video.h"
#include "dvd_audio.h"
#include "dvd_subtitles.h"
#include "dvd_time.h"

#ifndef DVD_VIDEO_LB_LEN
#define DVD_VIDEO_LB_LEN 2048
#endif

// libmpeg2 reads 4096 bytes at a time, do the same here
#define DVD_READ_BLOCKS 1
#define DVD_BUFFER_SIZE ( DVD_VIDEO_LB_LEN * DVD_READ_BLOCKS )

// #define DVD_COPY_BLOCK_LIMIT 512

// 2048 * 512 = 1 MB
// #define DVD_COPY_BYTES_LIMIT ( DVD_COPY_BLOCK_LIMIT * DVD_VIDEO_LB_LEN )

int main(int, char **);
void dvd_track_info(struct dvd_track *dvd_track, const uint16_t track_number, const ifo_handle_t *vmg_ifo, const ifo_handle_t *vts_ifo);
void print_usage(char *binary);

static void save_ppm (unsigned int width, unsigned int height, uint8_t * buf, int num, bool dvd)
{
    char filename[100];
    FILE * ppmfile;

    if(dvd)
    	sprintf (filename, "ppm/%d.ppm.dvd", num);
    else
    	sprintf (filename, "ppm/%d.ppm.vob", num);
    ppmfile = fopen (filename, "wb");
    if (!ppmfile) {
	fprintf (stderr, "Could not open file \"%s\".\n", filename);
	exit (1);
    }
    fprintf (ppmfile, "P6\n%d %d\n255\n", width, height);
    fwrite (buf, 3 * width, height, ppmfile);
    fclose (ppmfile);
}

struct dvd_copy {
	uint16_t track;
	uint8_t first_chapter;
	uint8_t last_chapter;
	uint8_t first_cell;
	uint8_t last_cell;
	ssize_t blocks;
	ssize_t filesize;
	char filename[PATH_MAX];
	int fd;
};

int main(int argc, char **argv) {

	/**
	 * Parse options
	 */

	bool opt_track_number = false;
	bool opt_chapter_number = false;
	uint16_t arg_track_number = 0;
	int long_index = 0;
	int opt = 0;
	opterr = 1;
	const char *str_options;
	str_options = "c:ht:";
	uint8_t arg_first_chapter = 1;
	uint8_t arg_last_chapter = 99;
	char *token = NULL;
	struct dvd_copy dvd_copy;

	struct option long_options[] = {

		{ "chapters", required_argument, 0, 'c' },
		{ "track", required_argument, 0, 't' },
		{ 0, 0, 0, 0 }

	};

	dvd_copy.track = 1;
	dvd_copy.first_chapter = 1;
	dvd_copy.last_chapter = 99;
	dvd_copy.first_cell = 1;
	dvd_copy.last_cell = 1;
	dvd_copy.blocks = 0;
	dvd_copy.filesize = 0;
	strncpy(dvd_copy.filename, "dvd_track_01.vob", 17);
	dvd_copy.fd = -1;

	while((opt = getopt_long(argc, argv, str_options, long_options, &long_index )) != -1) {

		switch(opt) {

			case 'c':
				opt_chapter_number = true;
				token = strtok(optarg, "-"); {
					if(strlen(token) > 2) {
						fprintf(stderr, "Chapter range must be between 1 and 99\n");
						return 1;
					}
					arg_first_chapter = (uint8_t)strtoumax(token, NULL, 0);
				}

				token = strtok(NULL, "-");
				if(token != NULL) {
					if(strlen(token) > 2) {
						fprintf(stderr, "Chapter range must be between 1 and 99\n");
						return 1;
					}
					arg_last_chapter = (uint8_t)strtoumax(token, NULL, 0);
				}

				if(arg_first_chapter == 0)
					arg_first_chapter = 1;
				if(arg_last_chapter < arg_first_chapter)
					arg_last_chapter = arg_first_chapter;

				break;

			case 'h':
				print_usage("dvd_copy");
				return 0;

			case 't':
				opt_track_number = true;
				arg_track_number = (uint16_t)strtoumax(optarg, NULL, 0);
				break;

			// ignore unknown arguments
			case '?':
				print_usage("dvd_copy");
				return 1;

			// let getopt_long set the variable
			case 0:
			default:
				break;

		}

	}

	const char *device_filename = DEFAULT_DVD_DEVICE;

	if (argv[optind])
		device_filename = argv[optind];

	if(access(device_filename, F_OK) != 0) {
		fprintf(stderr, "cannot access %s\n", device_filename);
		return 1;
	}

	// Check to see if device can be opened
	int dvd_fd = 0;
	dvd_fd = dvd_device_open(device_filename);
	if(dvd_fd < 0) {
		fprintf(stderr, "dvd_copy: error opening %s\n", device_filename);
		return 1;
	}
	dvd_device_close(dvd_fd);


#ifdef __linux__

	// Poll drive status if it is hardware
	if(dvd_device_is_hardware(device_filename)) {

		// Wait for the drive to become ready
		if(!dvd_drive_has_media(device_filename)) {

			fprintf(stderr, "drive status: ");
			dvd_drive_display_status(device_filename);

			return 1;

		}

	}

#endif

	dvd_reader_t *dvdread_dvd = NULL;
	dvdread_dvd = DVDOpen(device_filename);

	if(!dvdread_dvd) {
		printf("* dvdread could not open %s\n", device_filename);
		return 1;
	}

	ifo_handle_t *vmg_ifo = NULL;
	vmg_ifo = ifoOpen(dvdread_dvd, 0);

	if(vmg_ifo == NULL) {
		printf("* Could not open IFO zero\n");
		DVDClose(dvdread_dvd);
		return 1;
	}

	printf("Disc Title: %s\n", dvd_title(device_filename));

	uint16_t num_ifos = 1;
	num_ifos = vmg_ifo->vts_atrt->nr_of_vtss;

	if(num_ifos < 1) {
		printf("* DVD has no title IFOs?!\n");
		printf("* Most likely a bug in libdvdread or a bad master or problems reading the disc\n");
		ifoClose(vmg_ifo);
		DVDClose(dvdread_dvd);
		return 1;
	}

	// DVD
	struct dvd_info dvd_info;
	memset(&dvd_info, 0, sizeof(dvd_info));
	dvd_info.tracks = dvd_tracks(vmg_ifo);
	dvd_info.longest_track = 1;
	dvd_info.video_title_sets = dvd_video_title_sets(vmg_ifo);
	dvd_info.tracks = dvd_tracks(vmg_ifo);

	// Track
	struct dvd_track dvd_track;
	memset(&dvd_track, 0, sizeof(dvd_track));

	struct dvd_track dvd_tracks[DVD_MAX_TRACKS];
	memset(&dvd_tracks, 0, sizeof(dvd_track) * dvd_info.tracks);

	// Cells
	struct dvd_cell dvd_cell;
	dvd_cell.cell = 1;
	memset(dvd_cell.length, '\0', sizeof(dvd_cell.length));
	snprintf(dvd_cell.length, DVD_CELL_LENGTH + 1, "00:00:00.000");
	dvd_cell.msecs = 0;

	// Open first IFO
	uint16_t vts = 1;
	ifo_handle_t *vts_ifo = NULL;

	vts_ifo = ifoOpen(dvdread_dvd, vts);
	if(vts_ifo == NULL) {
		printf("* Could not open VTS_IFO for track %u\n", 1);
		return 1;
	}
	ifoClose(vts_ifo);
	vts_ifo = NULL;

	// Create an array of all the IFOs
	ifo_handle_t *vts_ifos[DVD_MAX_VTS_IFOS];
	vts_ifos[0] = NULL;

	for(vts = 1; vts < dvd_info.video_title_sets + 1; vts++) {

		vts_ifos[vts] = ifoOpen(dvdread_dvd, vts);

		if(!vts_ifos[vts]) {
			vts_ifos[vts] = NULL;
		} else if(!ifo_is_vts(vts_ifos[vts])) {
			ifoClose(vts_ifos[vts]);
			vts_ifos[vts] = NULL;
		}

	}
	
	// Exit if track number requested does not exist
	if(opt_track_number && (arg_track_number > dvd_info.tracks)) {
		fprintf(stderr, "dvd_copy: Invalid track number %d\n", arg_track_number);
		fprintf(stderr, "dvd_copy: Valid track numbers: 1 to %u\n", dvd_info.tracks);
		ifoClose(vmg_ifo);
		DVDClose(dvdread_dvd);
		return 1;
	} else if(opt_track_number) {
		dvd_copy.track = arg_track_number;
	}

	uint16_t ix = 0;
	uint16_t track = 1;
	
	uint32_t longest_msecs = 0;
	
	for(ix = 0, track = 1; ix < dvd_info.tracks; ix++, track++) {
 
		vts = dvd_vts_ifo_number(vmg_ifo, ix + 1);
		vts_ifo = vts_ifos[vts];
		dvd_track_info(&dvd_tracks[ix], track, vmg_ifo, vts_ifo);

		if(dvd_tracks[ix].msecs > longest_msecs) {
			dvd_info.longest_track = track;
			longest_msecs = dvd_tracks[ix].msecs;
		}

	}

	// Set the track number to rip if none is passed as an argument
	if(!opt_track_number)
		dvd_copy.track = dvd_info.longest_track;
	
	dvd_track = dvd_tracks[dvd_copy.track - 1];

	// Set the proper chapter range
	if(opt_chapter_number) {
		if(arg_first_chapter > dvd_track.chapters) {
			dvd_copy.first_chapter = dvd_track.chapters;
			fprintf(stderr, "Resetting first chapter to %u\n", dvd_copy.first_chapter);
		} else
			dvd_copy.first_chapter = arg_first_chapter;
		
		if(arg_last_chapter > dvd_track.chapters) {
			dvd_copy.last_chapter = dvd_track.chapters;
			fprintf(stderr, "Resetting last chapter to %u\n", dvd_copy.last_chapter);
		} else
			dvd_copy.last_chapter = arg_last_chapter;
	} else {
		dvd_copy.first_chapter = 1;
		dvd_copy.last_chapter = dvd_track.chapters;
	}
	
	/**
	 * Integers for numbers of blocks read, copied, counters
	 */
	int offset = 0;
	ssize_t read_blocks = DVD_READ_BLOCKS; // this can be changed, if you want to to do testing on different block sizes (grab more / less data on each read)
	ssize_t dvdread_read_blocks = 0; // num blocks passed by dvdread function
	ssize_t cell_blocks_written = 0;
	ssize_t bytes_written = 0;
	uint32_t cell_sectors = 0;

	uint8_t buffer[DVD_BUFFER_SIZE];
	uint8_t dvd_read_buffer[DVD_BUFFER_SIZE];
	// printf("size of buffer: %u\n", sizeof(buffer));
	// printf("size of dvd_read_buffer: %u\n", sizeof(dvd_read_buffer));

	/**
	 * File descriptors and filenames
	 */
	dvd_file_t *dvdread_vts_file = NULL;

	vts = dvd_vts_ifo_number(vmg_ifo, dvd_copy.track);
	vts_ifo = vts_ifos[vts];

	// Open the VTS VOB
	dvdread_vts_file = DVDOpenFile(dvdread_dvd, vts, DVD_READ_TITLE_VOBS);

	printf("Track: %02u, Length: %s Chapters: %02u, Cells: %02u, Audio streams: %02u, Subpictures: %02u, Filesize: %lu, Blocks: %lu\n", dvd_track.track, dvd_track.length, dvd_track.chapters, dvd_track.cells, dvd_track.audio_tracks, dvd_track.subtitles, dvd_track.filesize, dvd_track.blocks);

	dvd_copy.fd = open(dvd_copy.filename, O_WRONLY | O_CREAT | O_APPEND | O_TRUNC, 0644);

	dvd_copy.first_cell = dvd_chapter_first_cell(vmg_ifo, vts_ifo, dvd_copy.track, dvd_copy.first_chapter);
	dvd_copy.last_cell = dvd_chapter_last_cell(vmg_ifo, vts_ifo, dvd_copy.track, dvd_copy.last_chapter);

	// Get limits of copy
	for(dvd_cell.cell = dvd_copy.first_cell; dvd_cell.cell < dvd_copy.last_cell + 1; dvd_cell.cell++) {
		
		dvd_copy.blocks += dvd_cell_blocks(vmg_ifo, vts_ifo, dvd_track.track, dvd_cell.cell);
		dvd_copy.filesize += dvd_cell_filesize(vmg_ifo, vts_ifo, dvd_track.track, dvd_cell.cell);

	}


	// libmpeg2 :D
	mpeg2dec_t *decoder = NULL;
	const mpeg2_info_t *info = NULL;
	mpeg2_state_t state;
	size_t size = 0;
	int framenum = 0;
	decoder = mpeg2_init();
	if(decoder == NULL) {
		printf("couldn't create an MPEG2 decoder\n");
		return 1;
	}
	info = mpeg2_info(decoder);
	size = (size_t)-1;

	FILE *dvd_stream = NULL;
	dvd_stream = fopen("dvd_stream.vob", "w+");
	if(dvd_stream == NULL) {
		printf("couldn't create dvd_stream.vob");
		return 1;
	}

	FILE *vob_stream = NULL;
	vob_stream = fopen("dvd_track_03.vob", "rb");
	if(vob_stream == NULL) {
		printf("couldn't open vob stream");
		return 1;
	}

	int mdix = 0;

	bool ppm_vob = true;

	if(ppm_vob) {

	// libmpeg2
	do {
		state = mpeg2_parse(decoder);
		switch(state) {
			case STATE_BUFFER:
			// printf("STATE_BUFFER\n");
			size = fread(buffer, 1, DVD_BUFFER_SIZE, vob_stream);
			// printf("size read: %u, sizeof(buffer): %u\n", size, sizeof(buffer));
			mpeg2_buffer(decoder, buffer, buffer + size);
		case STATE_SEQUENCE:
			// printf("STATE_SEQUENCE\n");
			mpeg2_convert(decoder, mpeg2convert_rgb24, NULL);
			break;
		case STATE_SLICE:
			// printf("STATE_SLICE\n");
		case STATE_END:
			// printf("STATE_END\n");
		case STATE_INVALID_END:
			// printf("STATE_INVALID_END\n");
			if(info->display_fbuf) {
				// printf("ready to save ppm\n");
				save_ppm(info->sequence->width, info->sequence->height, info->display_fbuf->buf[0], framenum++, false);
			}
			break;
		default:
			break;
		}
	} while(size);

	}

	mdix = 0;
	
	struct dvd_chapter dvd_chapter;

	for(dvd_chapter.chapter = dvd_copy.first_chapter; dvd_chapter.chapter < dvd_copy.last_chapter + 1; dvd_chapter.chapter++) {

		dvd_chapter.first_cell = dvd_chapter_first_cell(vmg_ifo, vts_ifo, dvd_copy.track, dvd_chapter.chapter);
		dvd_chapter.last_cell = dvd_chapter_last_cell(vmg_ifo, vts_ifo, dvd_copy.track, dvd_chapter.chapter);
		strncpy(dvd_chapter.length, dvd_chapter_length(vmg_ifo, vts_ifo, dvd_copy.track, dvd_chapter.chapter), DVD_CHAPTER_LENGTH);

		for(dvd_cell.cell = dvd_chapter.first_cell; dvd_cell.cell < dvd_chapter.last_cell + 1; dvd_cell.cell++) {

			dvd_cell.blocks = dvd_cell_blocks(vmg_ifo, vts_ifo, dvd_track.track, dvd_cell.cell);
			dvd_cell.filesize = dvd_cell_filesize(vmg_ifo, vts_ifo, dvd_track.track, dvd_cell.cell);
			dvd_cell.first_sector = dvd_cell_first_sector(vmg_ifo, vts_ifo, dvd_track.track, dvd_cell.cell);
			dvd_cell.last_sector = dvd_cell_last_sector(vmg_ifo, vts_ifo, dvd_track.track, dvd_cell.cell);
			strncpy(dvd_cell.length, dvd_cell_length(vmg_ifo, vts_ifo, dvd_track.track, dvd_cell.cell), DVD_CELL_LENGTH);
			cell_sectors = dvd_cell.last_sector - dvd_cell.first_sector;

			printf("        Chapter: %02u, Cell: %02u, VTS: %u, Filesize: %lu, Blocks: %lu, Sectors: %i to %i\n", dvd_chapter.chapter, dvd_cell.cell, vts, dvd_cell.filesize, dvd_cell.blocks, dvd_cell.first_sector, dvd_cell.last_sector);

			cell_blocks_written = 0;

			if(dvd_cell.last_sector > dvd_cell.first_sector)
				cell_sectors++;

			if(dvd_cell.last_sector < dvd_cell.first_sector) {
				printf("* DEBUG Someone doing something nasty? The last sector is listed before the first; skipping cell\n");
				continue;
			}
			
			offset = (int)dvd_cell.first_sector;

			// This is where you would change the boundaries -- are you dumping to a track file (no boundaries) or a VOB (boundaries)
			while(cell_blocks_written < dvd_cell.blocks) {

				// Reset to the defaults
				read_blocks = DVD_READ_BLOCKS;

				if(read_blocks > (dvd_cell.blocks - cell_blocks_written)) {
					read_blocks = dvd_cell.blocks - cell_blocks_written;
				}

				/*
				dvdread_read_blocks = DVDReadBlocks(dvdread_vts_file, offset, (uint64_t)read_blocks, dvd_read_buffer);
				if(!dvdread_read_blocks) {
					printf("* Could not read data from cell %u\n", dvd_cell.cell);
					return 1;
				}

				// Check to make sure the amount read was what we wanted
				if(dvdread_read_blocks != read_blocks) {
					printf("*** Asked for %ld and only got %ld\n", read_blocks, dvdread_read_blocks);
					return 1;
				}
				*/

				framenum = 0;

				// libmpeg2
				do {
					if(read_blocks > (dvd_cell.blocks - cell_blocks_written)) {
						read_blocks = dvd_cell.blocks - cell_blocks_written;
					}
					state = mpeg2_parse(decoder);
					switch(state) {
						case STATE_BUFFER:
						printf("STATE_BUFFER\n");
						dvdread_read_blocks = DVDReadBlocks(dvdread_vts_file, offset, (uint64_t)read_blocks, dvd_read_buffer);
						printf("dvdread_read_blocks: %lu\n", dvdread_read_blocks);
						printf("offset: %i\n", offset);
						offset += dvdread_read_blocks;
						bytes_written = read_blocks * DVD_VIDEO_LB_LEN;
						mpeg2_buffer(decoder, dvd_read_buffer, dvd_read_buffer + DVD_BUFFER_SIZE);
						fwrite(dvd_read_buffer, 1, sizeof(dvd_read_buffer), dvd_stream);
						cell_blocks_written += dvdread_read_blocks;
						break;
					case STATE_SEQUENCE:
						printf("STATE_SEQUENCE\n");
						mpeg2_convert(decoder, mpeg2convert_rgb24, NULL);
						break;
					case STATE_SLICE:
						printf("STATE_SLICE\n");
					case STATE_END:
						printf("STATE_END\n");
					case STATE_INVALID_END:
						printf("STATE_INVALID_END\n");
						if(info->display_fbuf) {
							// printf("ready to save ppm\n");
							save_ppm(info->sequence->width, info->sequence->height, info->display_fbuf->buf[0], framenum++, true);
						}
						break;
					default:
						break;
					}
					printf("cell blocks total, written: %lu, %lu\n", dvd_cell.blocks, cell_blocks_written);
				} while(dvdread_read_blocks && cell_blocks_written < dvd_cell.blocks);


				// Increment the offsets
				// offset += dvdread_read_blocks;

				// Write the buffer to the track file
				// bytes_written = write(dvd_copy.fd, dvd_read_buffer, (uint64_t)(read_blocks * DVD_VIDEO_LB_LEN));
				// bytes_written = read_blocks * DVD_VIDEO_LB_LEN;

				if(!bytes_written) {
					printf("* Could not write data from cell %u\n", dvd_cell.cell);
					return 1;
				}

				// Check to make sure we wrote as much as we asked for
				if(bytes_written != dvdread_read_blocks * DVD_VIDEO_LB_LEN) {
					printf("*** Tried to write %ld bytes and only wrote %ld instead\n", dvdread_read_blocks * DVD_VIDEO_LB_LEN, bytes_written);
					return 1;
				}

				// Increment the amount of blocks written
				// cell_blocks_written += dvdread_read_blocks;

				// fflush(stdout);

			}

		}

	}

	mpeg2_close(decoder);

	if(dvd_stream)
		fclose(dvd_stream);

	// close(dvd_copy.fd);
	// remove(dvd_copy.filename);

	DVDCloseFile(dvdread_vts_file);

	if(vts_ifo)
		ifoClose(vts_ifo);
	
	if(vmg_ifo)
		ifoClose(vmg_ifo);

	if(dvdread_dvd)
		DVDClose(dvdread_dvd);
	
	return 0;

}

void dvd_track_info(struct dvd_track *dvd_track, const uint16_t track_number, const ifo_handle_t *vmg_ifo, const ifo_handle_t *vts_ifo) {

	dvd_track->track = track_number;
	dvd_track->valid = 1;
	dvd_track->vts = dvd_vts_ifo_number(vmg_ifo, track_number);
	dvd_track->ttn = dvd_track_ttn(vmg_ifo, track_number);
	strncpy(dvd_track->length, dvd_track_length(vmg_ifo, vts_ifo, track_number), DVD_TRACK_LENGTH);
	dvd_track->msecs = dvd_track_msecs(vmg_ifo, vts_ifo, track_number);
	dvd_track->chapters = dvd_track_chapters(vmg_ifo, vts_ifo, track_number);
	dvd_track->audio_tracks = dvd_track_audio_tracks(vts_ifo);
	dvd_track->subtitles = dvd_track_subtitles(vts_ifo);
	dvd_track->active_audio = dvd_audio_active_tracks(vmg_ifo, vts_ifo, track_number);
	dvd_track->active_subs = dvd_track_active_subtitles(vmg_ifo, vts_ifo, track_number);
	dvd_track->cells = dvd_track_cells(vmg_ifo, vts_ifo, track_number);
	dvd_track->blocks = dvd_track_blocks(vmg_ifo, vts_ifo, track_number);
	dvd_track->filesize = dvd_track_filesize(vmg_ifo, vts_ifo, track_number);

}

void print_usage(char *binary) {

	printf("%s - Copy a DVD track\n", binary);
	printf("\n");
	printf("Usage: %s [-t track] [dvd path]\n", binary);
	printf("\n");
	printf("DVD path can be a directory, a device filename, or a local file.\n");
	printf("\n");
	printf("Examples:\n");
	printf("  dvd_info " DEFAULT_DVD_DEVICE "	# Read a DVD drive directly\n");
	printf("  dvd_info movie.iso	# Read an image file\n");
	printf("  dvd_info movie/	# Read a directory that contains VIDEO_TS\n");
	printf("\n");
	printf("If no DVD path is given, %s is used in its place.\n", DEFAULT_DVD_DEVICE);

}
