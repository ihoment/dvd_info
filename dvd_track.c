/**
 * Functions used to get information about a DVD track
 */

#include <string.h>
#include <stdbool.h>
#include "dvdread/ifo_types.h"
#include "dvdread/ifo_read.h"

int dvd_track_mpeg_version(const ifo_handle_t *track_ifo) {

	int version = track_ifo->vtsi_mat->vts_video_attr.mpeg_version;

	if(version == 1)
		return 1;
	else if(version == 2)
		return 2;
	else
		return 0;

}

bool dvd_track_mpeg1(const ifo_handle_t *track_ifo) {

	int version = track_ifo->vtsi_mat->vts_video_attr.mpeg_version;

	if(version == 1)
		return true;
	else
		return false;

}

bool dvd_track_mpeg2(const ifo_handle_t *track_ifo) {

	int version = track_ifo->vtsi_mat->vts_video_attr.mpeg_version;

	if(version == 2)
		return true;
	else
		return false;

}

bool dvd_track_ntsc_video(const ifo_handle_t *track_ifo) {

	int format = track_ifo->vtsi_mat->vts_video_attr.video_format;

	if(format == 0)
		return true;
	else
		return false;

}

bool dvd_track_pal_video(const ifo_handle_t *track_ifo) {

	int format = track_ifo->vtsi_mat->vts_video_attr.video_format;

	if(format == 1)
		return true;
	else
		return false;

}

int dvd_track_video_height(const ifo_handle_t *track_ifo) {

	int video_height = 0;
	int picture_size = track_ifo->vtsi_mat->vts_video_attr.picture_size;

	if(dvd_track_ntsc_video(track_ifo))
		video_height = 480;
	else if(dvd_track_pal_video(track_ifo))
		video_height = 576;

	if(picture_size == 3 && video_height > 0)
		video_height = video_height / 2;

	return video_height;

}

int dvd_track_video_width(ifo_handle_t *track_ifo) {

	int video_width = 0;
	int picture_size = track_ifo->vtsi_mat->vts_video_attr.picture_size;
	int video_height = dvd_track_video_height(track_ifo);

	if(video_height == 0)
		return 0;

	switch(picture_size) {

		case 0:
			video_width = 720;

		case 1:
			video_width = 704;

		case 2:
		case 3:
			video_width = 352;

		default:
			video_width = 0;

	}

	return video_width;

}

bool dvd_track_letterbox_video(const ifo_handle_t *track_ifo) {

	int letterbox = track_ifo->vtsi_mat->vts_video_attr.letterboxed;

	if(letterbox == 0)
		return false;
	else
		return true;

}

int dvd_track_video_codec(ifo_handle_t *track_ifo, char *video_codec) {

	char *codec;

	if(track_ifo->vtsi_mat->vts_video_attr.mpeg_version == 0)
		codec = "MPEG1";
	else if(track_ifo->vtsi_mat->vts_video_attr.mpeg_version == 1)
		codec = "MPEG2";
	else
		codec = "";

	strncpy(video_codec, codec, 6);

	return 0;

}

int dvd_track_msec(dvd_time_t *dvd_time) {

	int framerates[4] = {0, 2500, 0, 2997};
	int framerate;
	int msec;

	framerate = framerates[(dvd_time->frame_u & 0xc0) >> 6];
	msec = (((dvd_time->hour & 0xf0) >> 3) * 5 + (dvd_time->hour & 0x0f)) * 3600000;
	msec += (((dvd_time->minute & 0xf0) >> 3) * 5 + (dvd_time->minute & 0x0f)) * 60000;
	msec += (((dvd_time->second & 0xf0) >> 3) * 5 + (dvd_time->second & 0x0f)) * 1000;

	if(framerate > 0)
		msec += (((dvd_time->frame_u & 0x30) >> 3) * 5 + (dvd_time->frame_u & 0x0f)) * 100000 / framerate;

	return msec;

}
