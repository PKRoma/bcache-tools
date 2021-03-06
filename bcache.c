/*
 * Authors: Kent Overstreet <kent.overstreet@gmail.com>
 *	    Gabriel de Perthuis <g2p.code@gmail.com>
 *	    Jacob Malevich <jam@datera.io>
 *
 * GPLv2
 */

#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "cmds.h"

static void usage(void)
{
	puts("bcache - tool for managing bcache volumes/filesystems\n"
	     "usage: bcache <command> [<args>]\n"
	     "\n"
	     "Superblock commands:\n"
	     "  format           Format a new filesystem\n"
	     "  show-super       Dump superblock information to stdout\n"
	     "\n"
	     "Repair:\n"
	     "  bcache fsck      Check an existing filesystem for errors\n"
	     "\n"
	     "Startup/shutdown, assembly of multi device filesystems:\n"
	     "  unlock           Unlock an encrypted filesystem prior to running/mounting\n"
	     "  assemble         Assemble an existing multi device filesystem\n"
	     "  incremental      Incrementally assemble an existing multi device filesystem\n"
	     "  run              Start a partially assembled filesystem\n"
	     "  stop	         Stop a running filesystem\n"

	     "Commands for managing a running filesystem:\n"
	     "  fs show          Show various information about a filesystem\n"
	     "  fs set           Modify filesystem options\n"
	     "\n"
	     "Commands for managing devices within a running filesystem:\n"
	     "  device add       Add a new device to an existing filesystem\n"
	     "  device remove    Remove a device from an existing filesystem\n"
	     "  device online    Readd an existing member to a filesystem\n"
	     "  device offline   Take a device offline, without removing it\n"
	     "  device evacuate  Migrate data off of a specific device\n"
	     "  device set-state Mark a device as failed\n"
	     "\n"
	     "Migrate:\n"
	     "  migrate          Migrate an existing filesystem to bcachefs, in place\n"
	     "  migrate-superblock\n"
	     "                   Add default superblock, after bcache migrate\n"
	     "\n"
	     "Debug:\n"
	     "These commands work on offline, unmounted filesystems\n"
	     "  dump             Dump filesystem metadata to a qcow2 image\n"
	     "  list             List filesystem metadata in textual form\n");
}

static char *full_cmd;

static char *pop_cmd(int *argc, char *argv[])
{
	if (*argc < 2) {
		printf("%s: missing command\n", argv[0]);
		usage();
		exit(EXIT_FAILURE);
	}

	char *cmd = argv[1];
	memmove(&argv[1], &argv[2], *argc * sizeof(argv[0]));
	(*argc)--;

	full_cmd = mprintf("%s %s", full_cmd, cmd);
	return cmd;
}

static int fs_cmds(int argc, char *argv[])
{
	char *cmd = pop_cmd(&argc, argv);

	if (!strcmp(cmd, "show"))
		return cmd_fs_show(argc, argv);
	if (!strcmp(cmd, "set"))
		return cmd_fs_set(argc, argv);

	usage();
	return 0;
}

static int device_cmds(int argc, char *argv[])
{
	char *cmd = pop_cmd(&argc, argv);

	if (!strcmp(cmd, "add"))
		return cmd_device_add(argc, argv);
	if (!strcmp(cmd, "remove"))
		return cmd_device_remove(argc, argv);
	if (!strcmp(cmd, "online"))
		return cmd_device_online(argc, argv);
	if (!strcmp(cmd, "offline"))
		return cmd_device_offline(argc, argv);
	if (!strcmp(cmd, "evacuate"))
		return cmd_device_offline(argc, argv);
	if (!strcmp(cmd, "set-state"))
		return cmd_device_set_state(argc, argv);

	usage();
	return 0;
}

int main(int argc, char *argv[])
{
	full_cmd = argv[0];

	setvbuf(stdout, NULL, _IOLBF, 0);

	char *cmd = pop_cmd(&argc, argv);

	if (!strcmp(cmd, "format"))
		return cmd_format(argc, argv);
	if (!strcmp(cmd, "show-super"))
		return cmd_show_super(argc, argv);

	if (!strcmp(cmd, "fsck"))
		return cmd_fsck(argc, argv);

	if (!strcmp(cmd, "unlock"))
		return cmd_unlock(argc, argv);
	if (!strcmp(cmd, "assemble"))
		return cmd_assemble(argc, argv);
	if (!strcmp(cmd, "incremental"))
		return cmd_incremental(argc, argv);
	if (!strcmp(cmd, "run"))
		return cmd_run(argc, argv);
	if (!strcmp(cmd, "stop"))
		return cmd_stop(argc, argv);

	if (!strcmp(cmd, "fs"))
		return fs_cmds(argc, argv);

	if (!strcmp(cmd, "device"))
		return device_cmds(argc, argv);

	if (!strcmp(cmd, "migrate"))
		return cmd_migrate(argc, argv);
	if (!strcmp(cmd, "migrate-superblock"))
		return cmd_migrate_superblock(argc, argv);

	if (!strcmp(cmd, "dump"))
		return cmd_dump(argc, argv);
	if (!strcmp(cmd, "list"))
		return cmd_list(argc, argv);

	usage();
	return 0;
}
