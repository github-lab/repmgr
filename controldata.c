/*
 * controldata.c
 * Copyright (c) 2ndQuadrant, 2010-2018
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "postgres_fe.h"

#include "repmgr.h"
#include "controldata.h"

static ControlFileInfo *get_controlfile(const char *DataDir);

uint64
get_system_identifier(const char *data_directory)
{
	ControlFileInfo *control_file_info = NULL;
	uint64		system_identifier = UNKNOWN_SYSTEM_IDENTIFIER;

	control_file_info = get_controlfile(data_directory);
	system_identifier = control_file_info->system_identifier;

	pfree(control_file_info);

	return system_identifier;
}

DBState
get_db_state(const char *data_directory)
{
	ControlFileInfo *control_file_info = NULL;
	DBState		state;

	control_file_info = get_controlfile(data_directory);

	state = control_file_info->state;

	pfree(control_file_info);

	return state;
}


extern XLogRecPtr
get_latest_checkpoint_location(const char *data_directory)
{
	ControlFileInfo *control_file_info = NULL;
	XLogRecPtr	checkPoint = InvalidXLogRecPtr;

	control_file_info = get_controlfile(data_directory);

	checkPoint = control_file_info->checkPoint;

	pfree(control_file_info);

	return checkPoint;
}


int
get_data_checksum_version(const char *data_directory)
{
	ControlFileInfo *control_file_info = NULL;
	int			data_checksum_version = -1;

	control_file_info = get_controlfile(data_directory);

	data_checksum_version = (int) control_file_info->data_checksum_version;

	pfree(control_file_info);

	return data_checksum_version;
}


const char *
describe_db_state(DBState state)
{
	switch (state)
	{
		case DB_STARTUP:
			return _("starting up");
		case DB_SHUTDOWNED:
			return _("shut down");
		case DB_SHUTDOWNED_IN_RECOVERY:
			return _("shut down in recovery");
		case DB_SHUTDOWNING:
			return _("shutting down");
		case DB_IN_CRASH_RECOVERY:
			return _("in crash recovery");
		case DB_IN_ARCHIVE_RECOVERY:
			return _("in archive recovery");
		case DB_IN_PRODUCTION:
			return _("in production");
	}
	return _("unrecognized status code");
}


/*
 * We maintain our own version of get_controlfile() as we need cross-version
 * compatibility, and also don't care if the file isn't readable.
 */
static ControlFileInfo *
get_controlfile(const char *DataDir)
{
	ControlFileInfo *control_file_info;
	FILE	   *fp = NULL;
	int			fd, ret, version_num;
	char		PgVersionPath[MAXPGPATH] = "";
	char		ControlFilePath[MAXPGPATH] = "";
	char		file_version_string[64] = "";
	long		file_major, file_minor;
	char	   *endptr = NULL;
	void	   *ControlFileDataPtr = NULL;
	int			expected_size = 0;

	control_file_info = palloc0(sizeof(ControlFileInfo));

	/* set default values */
	control_file_info->control_file_processed = false;
	control_file_info->system_identifier = UNKNOWN_SYSTEM_IDENTIFIER;
	control_file_info->state = DB_SHUTDOWNED;
	control_file_info->checkPoint = InvalidXLogRecPtr;
	control_file_info->data_checksum_version = -1;

	/*
	 * Read PG_VERSION, as we'll need to determine which struct to read
	 * the control file contents into
	 */
	snprintf(PgVersionPath, MAXPGPATH, "%s/PG_VERSION", DataDir);

	fp = fopen(PgVersionPath, "r");

	if (fp == NULL)
	{
		log_warning(_("could not open file \"%s\" for reading"),
					PgVersionPath);
		log_detail("%s", strerror(errno));
		return control_file_info;
	}

	file_version_string[0] = '\0';

	ret = fscanf(fp, "%63s", file_version_string);
	fclose(fp);

	if (ret != 1 || endptr == file_version_string)
	{
		log_warning(_("unable to determine major version number from PG_VERSION"));

		return control_file_info;
	}

	file_major = strtol(file_version_string, &endptr, 10);
	file_minor = 0;

	if (*endptr == '.')
		file_minor = strtol(endptr + 1, NULL, 10);

	version_num = ((int) file_major * 10000) + ((int) file_minor * 100);

	if (version_num < 90300)
	{
		log_warning(_("Data directory appears to be initialised for %s"), file_version_string);
		return control_file_info;
	}


	snprintf(ControlFilePath, MAXPGPATH, "%s/global/pg_control", DataDir);

	if ((fd = open(ControlFilePath, O_RDONLY | PG_BINARY, 0)) == -1)
	{
		log_warning(_("could not open file \"%s\" for reading"),
					ControlFilePath);
		log_detail("%s", strerror(errno));
		return control_file_info;
	}


	if (version_num >= 90500)
	{
		expected_size = sizeof(ControlFileData95);
		ControlFileDataPtr = palloc0(expected_size);
	}
	else if (version_num >= 90400)
	{
		expected_size = sizeof(ControlFileData94);
		ControlFileDataPtr = palloc0(expected_size);
	}
	else if (version_num >= 90300)
	{
		expected_size = sizeof(ControlFileData93);
		ControlFileDataPtr = palloc0(expected_size);
	}


	if (read(fd, ControlFileDataPtr, expected_size) != expected_size)
	{
		log_warning(_("could not read file \"%s\""),
					ControlFilePath);
		log_detail("%s", strerror(errno));

		return control_file_info;
	}

	close(fd);

	control_file_info->control_file_processed = true;

	if (version_num >= 90500)
	{
		ControlFileData95 *ptr = (struct ControlFileData95 *)ControlFileDataPtr;
		control_file_info->system_identifier = ptr->system_identifier;
		control_file_info->state = ptr->state;
		control_file_info->checkPoint = ptr->checkPoint;
		control_file_info->data_checksum_version = ptr->data_checksum_version;
	}
	else if (version_num >= 90400)
	{
		ControlFileData94 *ptr = (struct ControlFileData94 *)ControlFileDataPtr;
		control_file_info->system_identifier = ptr->system_identifier;
		control_file_info->state = ptr->state;
		control_file_info->checkPoint = ptr->checkPoint;
		control_file_info->data_checksum_version = ptr->data_checksum_version;
	}
	else if (version_num >= 90300)
	{
		ControlFileData93 *ptr = (struct ControlFileData93 *)ControlFileDataPtr;
		control_file_info->system_identifier = ptr->system_identifier;
		control_file_info->state = ptr->state;
		control_file_info->checkPoint = ptr->checkPoint;
		control_file_info->data_checksum_version = ptr->data_checksum_version;
	}

	pfree(ControlFileDataPtr);

	/*
	 * We don't check the CRC here as we're potentially checking a pg_control
	 * file from a different PostgreSQL version to the one repmgr was compiled
	 * against. However we're only interested in the first few fields, which
	 * should be constant across supported versions
	 *
	 */

	return control_file_info;
}
