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
#ifdef __linux__
#include <linux/cdrom.h>
#include "dvd_drive.h"
#endif
#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_read.h>
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
#ifndef VERSION
#define VERSION "1.2"
#endif

#define DVD_INFO_PROGRAM "dvd_copy"
#define DVD_COPY_FILENAME 16

#ifndef DVD_VIDEO_LB_LEN
#define DVD_VIDEO_LB_LEN 2048
#endif

#define DVD_COPY_BLOCK_LIMIT 512
#define DVD_CAT_BLOCK_LIMIT 1

// 2048 * 512 = 1 MB
#define DVD_COPY_BYTES_LIMIT ( DVD_COPY_BLOCK_LIMIT * DVD_VIDEO_LB_LEN )
#define DVD_CAT_BYTES_LIMIT ( DVD_CAT_BLOCK_LIMIT * DVD_VIDEO_LB_LEN )

int main(int, char **);
void dvd_track_info(struct dvd_track *dvd_track, const uint16_t track_number, const ifo_handle_t *vmg_ifo, const ifo_handle_t *vts_ifo);
void print_usage(char *binary);
void print_version(char *binary);

struct dvd_copy {
	uint16_t track;
	uint8_t first_chapter;
	uint8_t last_chapter;
	uint8_t first_cell;
	uint8_t last_cell;
	ssize_t blocks;
	ssize_t filesize;
	char *filename;
	int fd;
};

int main(int argc, char **argv) {

	/**
	 * Parse options
	 */

	bool opt_track_number = false;
	bool opt_chapter_number = false;
	bool p_dvd_copy = true;
	bool p_dvd_cat = false;
	bool opt_filename = false;
	uint16_t arg_track_number = 0;
	int long_index = 0;
	int opt = 0;
	opterr = 1;
	const char *str_options;
	str_options = "c:ho:t:V";
	uint8_t arg_first_chapter = 1;
	uint8_t arg_last_chapter = 99;
	char *token = NULL;
	struct dvd_copy dvd_copy;

	struct option long_options[] = {

		{ "chapters", required_argument, 0, 'c' },
		{ "dvd_copy.filename", required_argument, 0, 'o' },
		{ "track", required_argument, 0, 't' },
		{ "help", no_argument, 0, 'h' },
		{ "version", no_argument, 0, 'V' },
		{ 0, 0, 0, 0 }

	};

	dvd_copy.track = 1;
	dvd_copy.first_chapter = 1;
	dvd_copy.last_chapter = 99;
	dvd_copy.first_cell = 1;
	dvd_copy.last_cell = 1;
	dvd_copy.blocks = 0;
	dvd_copy.filesize = 0;
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
				print_usage(DVD_INFO_PROGRAM);
				return 0;

			case 'o':
				if(strlen(optarg) == 1 && strncmp("-", optarg, 1) == 0) {
					p_dvd_copy = false;
					p_dvd_cat = true;
					fprintf(stderr, "[%s] outputting stream to stdout\n", DVD_INFO_PROGRAM);
				} else {
					p_dvd_copy = true;
					p_dvd_cat = false;
					opt_filename = true;
					dvd_copy.filename = optarg;
				}
				break;

			case 't':
				opt_track_number = true;
				arg_track_number = (uint16_t)strtoumax(optarg, NULL, 0);
				break;

			case 'V':
				print_version("dvd_copy");
				return 0;

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
		fprintf(stderr, "* dvdread could not open %s\n", device_filename);
		return 1;
	}

	ifo_handle_t *vmg_ifo = NULL;
	vmg_ifo = ifoOpen(dvdread_dvd, 0);

	if(vmg_ifo == NULL) {
		fprintf(stderr, "* Could not open IFO zero\n");
		DVDClose(dvdread_dvd);
		return 1;
	}

	// DVD
	struct dvd_info dvd_info;
	memset(dvd_info.dvdread_id, '\0', sizeof(dvd_info.dvdread_id));
	dvd_info.video_title_sets = dvd_video_title_sets(vmg_ifo);
	dvd_info.side = 1;
	memset(dvd_info.title, '\0', sizeof(dvd_info.title));
	memset(dvd_info.provider_id, '\0', sizeof(dvd_info.provider_id));
	memset(dvd_info.vmg_id, '\0', sizeof(dvd_info.vmg_id));
	dvd_info.tracks = dvd_tracks(vmg_ifo);
	dvd_info.longest_track = 1;

	dvd_title(dvd_info.title, device_filename);
	if(p_dvd_copy)
		printf("Disc title: %s\n", dvd_info.title);

	uint16_t num_ifos = 1;
	num_ifos = vmg_ifo->vts_atrt->nr_of_vtss;

	if(num_ifos < 1) {
		fprintf(stderr, "* DVD has no title IFOs?!\n");
		fprintf(stderr, "* Most likely a bug in libdvdread or a bad master or problems reading the disc\n");
		ifoClose(vmg_ifo);
		DVDClose(dvdread_dvd);
		return 1;
	}


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
		fprintf(stderr, "* Could not open VTS_IFO for track %u\n", 1);
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
	
	// Set default filename
	if(!opt_filename) {
		dvd_copy.filename = calloc(DVD_COPY_FILENAME + 1, sizeof(unsigned char));
		snprintf(dvd_copy.filename, DVD_COPY_FILENAME + 1, "dvd_track_%02u.vob", dvd_copy.track);
	}

	/**
	 * Integers for numbers of blocks read, copied, counters
	 */
	int offset = 0;
	ssize_t dvdread_read_blocks = 0; // num blocks passed by dvdread function
	ssize_t cell_blocks_written = 0;
	ssize_t track_blocks_written = 0;
	ssize_t bytes_written = 0;
	uint32_t cell_sectors = 0;

	// Copy size
	// For p_dvd_copy, the amount is variable, regarding the code
	// For p_dvd_cat, limit the blocks to one so it is reading the minimum that
	// dvdread will provide.
	ssize_t read_blocks = 1;
	if(p_dvd_copy)
		read_blocks = DVD_COPY_BLOCK_LIMIT; // this can be changed, if you want to to do testing on different block sizes (grab more / less data on each read)
	else if(p_dvd_cat)
		read_blocks = DVD_CAT_BLOCK_LIMIT;
	else
		read_blocks = 1;

	unsigned char *dvd_read_buffer = NULL;
	if(p_dvd_copy)
		dvd_read_buffer = (unsigned char *)calloc(1, (uint64_t)DVD_COPY_BYTES_LIMIT * sizeof(unsigned char));
	else if (p_dvd_cat)
		dvd_read_buffer = (unsigned char *)calloc(1, (uint64_t)DVD_CAT_BYTES_LIMIT * sizeof(unsigned char));
	else
		dvd_read_buffer = (unsigned char *)calloc(1, 1 * sizeof(unsigned char));
	if(dvd_read_buffer == NULL) {
		fprintf(stderr, "Couldn't allocate memory\n");
		return 1;
	}

	/**
	 * File descriptors and filenames
	 */
	dvd_file_t *dvdread_vts_file = NULL;

	vts = dvd_vts_ifo_number(vmg_ifo, dvd_copy.track);
	vts_ifo = vts_ifos[vts];

	// Open the VTS VOB
	dvdread_vts_file = DVDOpenFile(dvdread_dvd, vts, DVD_READ_TITLE_VOBS);

	if(p_dvd_copy)
		printf("Track: %02u, Length: %s, Chapters: %02u, Cells: %02u, Audio streams: %02u, Subpictures: %02u, Filesize: %lu, Blocks: %lu\n", dvd_track.track, dvd_track.length, dvd_track.chapters, dvd_track.cells, dvd_track.audio_tracks, dvd_track.subtitles, dvd_track.filesize, dvd_track.blocks);

	if(p_dvd_copy) {
		dvd_copy.fd = open(dvd_copy.filename, O_WRONLY | O_CREAT | O_APPEND | O_TRUNC, 0644);
		if(dvd_copy.fd == -1) {
			fprintf(stderr, "Couldn't create file %s\n", dvd_copy.filename);
			return 1;
		}
	}

	track_blocks_written = 0;

	dvd_copy.first_cell = dvd_chapter_first_cell(vmg_ifo, vts_ifo, dvd_copy.track, dvd_copy.first_chapter);
	dvd_copy.last_cell = dvd_chapter_last_cell(vmg_ifo, vts_ifo, dvd_copy.track, dvd_copy.last_chapter);

	// Get limits of copy
	for(dvd_cell.cell = dvd_copy.first_cell; dvd_cell.cell < dvd_copy.last_cell + 1; dvd_cell.cell++) {
		
		dvd_copy.blocks += dvd_cell_blocks(vmg_ifo, vts_ifo, dvd_track.track, dvd_cell.cell);
		dvd_copy.filesize += dvd_cell_filesize(vmg_ifo, vts_ifo, dvd_track.track, dvd_cell.cell);

	}

	struct dvd_chapter dvd_chapter;

	for(dvd_chapter.chapter = dvd_copy.first_chapter; dvd_chapter.chapter < dvd_copy.last_chapter + 1; dvd_chapter.chapter++) {

		dvd_chapter.first_cell = dvd_chapter_first_cell(vmg_ifo, vts_ifo, dvd_copy.track, dvd_chapter.chapter);
		dvd_chapter.last_cell = dvd_chapter_last_cell(vmg_ifo, vts_ifo, dvd_copy.track, dvd_chapter.chapter);
		dvd_chapter_length(dvd_chapter.length, vmg_ifo, vts_ifo, dvd_copy.track, dvd_chapter.chapter);

		for(dvd_cell.cell = dvd_chapter.first_cell; dvd_cell.cell < dvd_chapter.last_cell + 1; dvd_cell.cell++) {

			dvd_cell.blocks = dvd_cell_blocks(vmg_ifo, vts_ifo, dvd_track.track, dvd_cell.cell);
			dvd_cell.filesize = dvd_cell_filesize(vmg_ifo, vts_ifo, dvd_track.track, dvd_cell.cell);
			dvd_cell.first_sector = dvd_cell_first_sector(vmg_ifo, vts_ifo, dvd_track.track, dvd_cell.cell);
			dvd_cell.last_sector = dvd_cell_last_sector(vmg_ifo, vts_ifo, dvd_track.track, dvd_cell.cell);
			dvd_cell_length(dvd_cell.length, vmg_ifo, vts_ifo, dvd_track.track, dvd_cell.cell);
			cell_sectors = dvd_cell.last_sector - dvd_cell.first_sector;

			if(p_dvd_copy)
				printf("        Chapter: %02u, Cell: %02u, VTS: %u, Filesize: %lu, Blocks: %lu, Sectors: %i to %i\n", dvd_chapter.chapter, dvd_cell.cell, vts, dvd_cell.filesize, dvd_cell.blocks, dvd_cell.first_sector, dvd_cell.last_sector);

			cell_blocks_written = 0;

			if(dvd_cell.last_sector > dvd_cell.first_sector)
				cell_sectors++;

			if(dvd_cell.last_sector < dvd_cell.first_sector) {
				fprintf(stderr, "* DEBUG Someone doing something nasty? The last sector is listed before the first; skipping cell\n");
				continue;
			}
			
			offset = (int)dvd_cell.first_sector;

			// This is where you would change the boundaries -- are you dumping to a track file (no boundaries) or a VOB (boundaries)
			while(cell_blocks_written < dvd_cell.blocks) {

				// Reset to the defaults
				if(p_dvd_copy)
					read_blocks = DVD_COPY_BLOCK_LIMIT;
				else if(p_dvd_cat)
					read_blocks = DVD_CAT_BLOCK_LIMIT;
				else
					read_blocks = 1;

				if(read_blocks > (dvd_cell.blocks - cell_blocks_written)) {
					read_blocks = dvd_cell.blocks - cell_blocks_written;
				}

				dvdread_read_blocks = DVDReadBlocks(dvdread_vts_file, offset, (uint64_t)read_blocks, dvd_read_buffer);
				if(!dvdread_read_blocks) {
					fprintf(stderr, "* Could not read data from cell %u\n", dvd_cell.cell);
					return 1;
				}

				// Check to make sure the amount read was what we wanted
				if(dvdread_read_blocks != read_blocks) {
					fprintf(stderr, "*** Asked for %ld and only got %ld\n", read_blocks, dvdread_read_blocks);
					return 1;
				}

				// Increment the offsets
				offset += dvdread_read_blocks;

				// Write the buffer to the track file
				if(p_dvd_copy)
					bytes_written = write(dvd_copy.fd, dvd_read_buffer, (uint64_t)(read_blocks * DVD_VIDEO_LB_LEN));
				else if(p_dvd_cat)
					bytes_written = write(1, dvd_read_buffer, (uint64_t)(read_blocks * DVD_VIDEO_LB_LEN));
				else
					bytes_written = 0;

				if(!bytes_written) {
					fprintf(stderr, "* Could not write data from cell %u\n", dvd_cell.cell);
					return 1;
				}

				// Check to make sure we wrote as much as we asked for
				if(bytes_written != dvdread_read_blocks * DVD_VIDEO_LB_LEN) {
					fprintf(stderr, "*** Tried to write %ld bytes and only wrote %ld instead\n", dvdread_read_blocks * DVD_VIDEO_LB_LEN, bytes_written);
					return 1;
				}

				// Increment the amount of blocks written
				cell_blocks_written += dvdread_read_blocks;
				track_blocks_written += dvdread_read_blocks;

				if(p_dvd_copy) {
					printf("Progress %lu%%\r", track_blocks_written * 100 / dvd_copy.blocks);
					fflush(stdout);
				}

			}

		}

	}

	close(dvd_copy.fd);

	DVDCloseFile(dvdread_vts_file);

	if(p_dvd_copy)
		printf("\n");
	
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
	dvd_track_length(dvd_track->length, vmg_ifo, vts_ifo, track_number);
	dvd_track->msecs = dvd_track_msecs(vmg_ifo, vts_ifo, track_number);
	dvd_track->chapters = dvd_track_chapters(vmg_ifo, vts_ifo, track_number);
	dvd_track->audio_tracks = dvd_track_audio_tracks(vts_ifo);
	dvd_track->subtitles = dvd_track_subtitles(vts_ifo);
	dvd_track->active_audio_streams = dvd_audio_active_tracks(vmg_ifo, vts_ifo, track_number);
	dvd_track->active_subs = dvd_track_active_subtitles(vmg_ifo, vts_ifo, track_number);
	dvd_track->cells = dvd_track_cells(vmg_ifo, vts_ifo, track_number);
	dvd_track->blocks = dvd_track_blocks(vmg_ifo, vts_ifo, track_number);
	dvd_track->filesize = dvd_track_filesize(vmg_ifo, vts_ifo, track_number);

}

void print_usage(char *binary) {

	printf("%s %s - copy a single DVD track to the filesystem\n", binary, VERSION);
	printf("\n");
	printf("Usage: %s [-t track] [-c chapter[-chapter]] [-o filename] [dvd path]\n", binary);
	printf("\n");
	printf("DVD path can be a device name, a single file, or directory.\n");
	printf("\n");
	printf("Examples:\n");
	printf("  dvd_copy		# Read default DVD device (%s)\n", DEFAULT_DVD_DEVICE);
	printf("  dvd_copy /dev/dvd	# Read a specific DVD device\n");
	printf("  dvd_copy video.iso    # Read an image file\n");
	printf("  dvd_copy ~/Videos/DVD	# Read a directory that contains VIDEO_TS\n");
	printf("\n");
	printf("Output filenames:\n");
	printf("  dvd_copy		# Save to \"dvd_track_##.vob\" where ## is longest track\n");
	printf("  dvd_copy -o video.vob	# Save to \"video.vob\" (MPEG2 program stream)\n");
	printf("  dvd_copy -o video.mpg	# Save to \"video.mpg\" (MPEG2 program stream)\n");
	printf("  dvd_copy -o -		# Stream to console output (stdout)\n");

}

void print_version(char *binary) {

	printf("%s %s - http://dvds.beandog.org/ - (c) 2018 Steve Dibb <steve.dibb@gmail.com>, licensed under GPL-2\n", binary, VERSION);

}
