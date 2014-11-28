/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2012 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <stdio.h>
#include <mntent.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "util.h"
#include "unit-name.h"
#include "path-util.h"
#include "mount-setup.h"
#include "special.h"
#include "mkdir.h"
#include "fileio.h"
#include "generator.h"
#include "strv.h"
#include "virt.h"

static const char *arg_dest = "/tmp";
static bool arg_fstab_enabled = true;
static char *arg_root_what = NULL;
static char *arg_root_fstype = NULL;
static char *arg_root_options = NULL;
static int arg_root_rw = -1;
static char *arg_usr_what = NULL;
static char *arg_usr_fstype = NULL;
static char *arg_usr_options = NULL;

static int mount_find_pri(struct mntent *me, int *ret) {
        char *end, *opt;
        unsigned long r;

        assert(me);
        assert(ret);

        opt = hasmntopt(me, "pri");
        if (!opt)
                return 0;

        opt += strlen("pri");
        if (*opt != '=')
                return -EINVAL;

        errno = 0;
        r = strtoul(opt + 1, &end, 10);
        if (errno > 0)
                return -errno;

        if (end == opt + 1 || (*end != ',' && *end != 0))
                return -EINVAL;

        *ret = (int) r;
        return 1;
}

static int add_swap(
                const char *what,
                struct mntent *me,
                bool noauto,
                bool nofail) {

        _cleanup_free_ char *name = NULL, *unit = NULL, *lnk = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r, pri = -1;

        assert(what);
        assert(me);

        if (detect_container(NULL) > 0) {
                log_info("Running in a container, ignoring fstab swap entry for %s.", what);
                return 0;
        }

        r = mount_find_pri(me, &pri);
        if (r < 0) {
                log_error("Failed to parse priority");
                return r;
        }

        name = unit_name_from_path(what, ".swap");
        if (!name)
                return log_oom();

        unit = strjoin(arg_dest, "/", name, NULL);
        if (!unit)
                return log_oom();

        f = fopen(unit, "wxe");
        if (!f) {
                if (errno == EEXIST)
                        log_error("Failed to create swap unit file %s, as it already exists. Duplicate entry in /etc/fstab?", unit);
                else
                        log_error("Failed to create unit file %s: %m", unit);
                return -errno;
        }

        fprintf(f,
                "# Automatically generated by systemd-fstab-generator\n\n"
                "[Unit]\n"
                "SourcePath=/etc/fstab\n"
                "Documentation=man:fstab(5) man:systemd-fstab-generator(8)\n\n"
                "[Swap]\n"
                "What=%s\n",
                what);

        /* Note that we currently pass the priority field twice, once
         * in Priority=, and once in Options= */
        if (pri >= 0)
                fprintf(f, "Priority=%i\n", pri);

        if (!isempty(me->mnt_opts) && !streq(me->mnt_opts, "defaults"))
                fprintf(f, "Options=%s\n", me->mnt_opts);

        r = fflush_and_check(f);
        if (r < 0) {
                log_error_errno(-r, "Failed to write unit file %s: %m", unit);
                return r;
        }

        /* use what as where, to have a nicer error message */
        r = generator_write_timeouts(arg_dest, what, what, me->mnt_opts, NULL);
        if (r < 0)
                return r;

        if (!noauto) {
                lnk = strjoin(arg_dest, "/" SPECIAL_SWAP_TARGET,
                              nofail ? ".wants/" : ".requires/", name, NULL);
                if (!lnk)
                        return log_oom();

                mkdir_parents_label(lnk, 0755);
                if (symlink(unit, lnk) < 0) {
                        log_error("Failed to create symlink %s: %m", lnk);
                        return -errno;
                }
        }

        return 0;
}

static bool mount_is_network(struct mntent *me) {
        assert(me);

        return
                hasmntopt(me, "_netdev") ||
                fstype_is_network(me->mnt_type);
}

static bool mount_in_initrd(struct mntent *me) {
        assert(me);

        return
                hasmntopt(me, "x-initrd.mount") ||
                streq(me->mnt_dir, "/usr");
}

static int add_mount(
                const char *what,
                const char *where,
                const char *fstype,
                const char *opts,
                int passno,
                bool noauto,
                bool nofail,
                bool automount,
                const char *post,
                const char *source) {

        _cleanup_free_ char
                *name = NULL, *unit = NULL, *lnk = NULL,
                *automount_name = NULL, *automount_unit = NULL,
                *filtered = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(what);
        assert(where);
        assert(opts);
        assert(source);

        if (streq_ptr(fstype, "autofs"))
                return 0;

        if (!is_path(where)) {
                log_warning("Mount point %s is not a valid path, ignoring.", where);
                return 0;
        }

        if (mount_point_is_api(where) ||
            mount_point_ignore(where))
                return 0;

        if (path_equal(where, "/")) {
                /* The root disk is not an option */
                automount = false;
                noauto = false;
                nofail = false;
        }

        name = unit_name_from_path(where, ".mount");
        if (!name)
                return log_oom();

        unit = strjoin(arg_dest, "/", name, NULL);
        if (!unit)
                return log_oom();

        f = fopen(unit, "wxe");
        if (!f) {
                if (errno == EEXIST)
                        log_error("Failed to create mount unit file %s, as it already exists. Duplicate entry in /etc/fstab?", unit);
                else
                        log_error("Failed to create unit file %s: %m", unit);
                return -errno;
        }

        fprintf(f,
                "# Automatically generated by systemd-fstab-generator\n\n"
                "[Unit]\n"
                "SourcePath=%s\n"
                "Documentation=man:fstab(5) man:systemd-fstab-generator(8)\n",
                source);

        if (post && !noauto && !nofail && !automount)
                fprintf(f, "Before=%s\n", post);

        if (passno != 0) {
                r = generator_write_fsck_deps(f, arg_dest, what, where, fstype);
                if (r < 0)
                        return r;
        }

        fprintf(f,
                "\n"
                "[Mount]\n"
                "What=%s\n"
                "Where=%s\n",
                what,
                where);

        if (!isempty(fstype) && !streq(fstype, "auto"))
                fprintf(f, "Type=%s\n", fstype);

        r = generator_write_timeouts(arg_dest, what, where, opts, &filtered);
        if (r < 0)
                return r;

        if (!isempty(filtered) && !streq(filtered, "defaults"))
                fprintf(f, "Options=%s\n", filtered);

        fflush(f);
        if (ferror(f)) {
                log_error("Failed to write unit file %s: %m", unit);
                return -errno;
        }

        if (!noauto && post) {
                lnk = strjoin(arg_dest, "/", post, nofail || automount ? ".wants/" : ".requires/", name, NULL);
                if (!lnk)
                        return log_oom();

                mkdir_parents_label(lnk, 0755);
                if (symlink(unit, lnk) < 0) {
                        log_error("Failed to create symlink %s: %m", lnk);
                        return -errno;
                }
        }

        if (automount) {
                automount_name = unit_name_from_path(where, ".automount");
                if (!automount_name)
                        return log_oom();

                automount_unit = strjoin(arg_dest, "/", automount_name, NULL);
                if (!automount_unit)
                        return log_oom();

                fclose(f);
                f = fopen(automount_unit, "wxe");
                if (!f) {
                        log_error("Failed to create unit file %s: %m", automount_unit);
                        return -errno;
                }

                fprintf(f,
                        "# Automatically generated by systemd-fstab-generator\n\n"
                        "[Unit]\n"
                        "SourcePath=%s\n"
                        "Documentation=man:fstab(5) man:systemd-fstab-generator(8)\n",
                        source);

                if (post)
                        fprintf(f,
                                "Before=%s\n",
                                post);

                fprintf(f,
                        "[Automount]\n"
                        "Where=%s\n",
                        where);

                fflush(f);
                if (ferror(f)) {
                        log_error("Failed to write unit file %s: %m", automount_unit);
                        return -errno;
                }

                free(lnk);
                lnk = strjoin(arg_dest, "/", post, nofail ? ".wants/" : ".requires/", automount_name, NULL);
                if (!lnk)
                        return log_oom();

                mkdir_parents_label(lnk, 0755);
                if (symlink(automount_unit, lnk) < 0) {
                        log_error("Failed to create symlink %s: %m", lnk);
                        return -errno;
                }
        }

        return 0;
}

static int parse_fstab(bool initrd) {
        _cleanup_endmntent_ FILE *f = NULL;
        const char *fstab_path;
        struct mntent *me;
        int r = 0;

        fstab_path = initrd ? "/sysroot/etc/fstab" : "/etc/fstab";
        f = setmntent(fstab_path, "re");
        if (!f) {
                if (errno == ENOENT)
                        return 0;

                log_error("Failed to open %s: %m", fstab_path);
                return -errno;
        }

        while ((me = getmntent(f))) {
                _cleanup_free_ char *where = NULL, *what = NULL;
                bool noauto, nofail;
                int k;

                if (initrd && !mount_in_initrd(me))
                        continue;

                what = fstab_node_to_udev_node(me->mnt_fsname);
                if (!what)
                        return log_oom();

                if (detect_container(NULL) > 0 && is_device_path(what)) {
                        log_info("Running in a container, ignoring fstab device entry for %s.", what);
                        continue;
                }

                where = initrd ? strappend("/sysroot/", me->mnt_dir) : strdup(me->mnt_dir);
                if (!where)
                        return log_oom();

                if (is_path(where))
                        path_kill_slashes(where);

                noauto = !!hasmntopt(me, "noauto");
                nofail = !!hasmntopt(me, "nofail");
                log_debug("Found entry what=%s where=%s type=%s nofail=%s noauto=%s",
                          what, where, me->mnt_type,
                          yes_no(noauto), yes_no(nofail));

                if (streq(me->mnt_type, "swap"))
                        k = add_swap(what, me, noauto, nofail);
                else {
                        bool automount;
                        const char *post;

                        automount =
                                  hasmntopt(me, "comment=systemd.automount") ||
                                  hasmntopt(me, "x-systemd.automount");

                        if (initrd)
                                post = SPECIAL_INITRD_FS_TARGET;
                        else if (mount_in_initrd(me))
                                post = SPECIAL_INITRD_ROOT_FS_TARGET;
                        else if (mount_is_network(me))
                                post = SPECIAL_REMOTE_FS_TARGET;
                        else
                                post = SPECIAL_LOCAL_FS_TARGET;

                        k = add_mount(what,
                                      where,
                                      me->mnt_type,
                                      me->mnt_opts,
                                      me->mnt_passno,
                                      noauto,
                                      nofail,
                                      automount,
                                      post,
                                      fstab_path);
                }

                if (k < 0)
                        r = k;
        }

        return r;
}

static int add_root_mount(void) {
        _cleanup_free_ char *what = NULL;
        const char *opts;

        if (isempty(arg_root_what)) {
                log_debug("Could not find a root= entry on the kernel command line.");
                return 0;
        }

        what = fstab_node_to_udev_node(arg_root_what);
        if (!path_is_absolute(what)) {
                log_debug("Skipping entry what=%s where=/sysroot type=%s", what, strna(arg_root_fstype));
                return 0;
        }

        if (!arg_root_options)
                opts = arg_root_rw > 0 ? "rw" : "ro";
        else if (arg_root_rw >= 0 ||
                 (!mount_test_option(arg_root_options, "ro") &&
                  !mount_test_option(arg_root_options, "rw")))
                opts = strappenda(arg_root_options, ",", arg_root_rw > 0 ? "rw" : "ro");
        else
                opts = arg_root_options;

        log_debug("Found entry what=%s where=/sysroot type=%s", what, strna(arg_root_fstype));
        return add_mount(what,
                         "/sysroot",
                         arg_root_fstype,
                         opts,
                         1,
                         false,
                         false,
                         false,
                         SPECIAL_INITRD_ROOT_FS_TARGET,
                         "/proc/cmdline");
}

static int add_usr_mount(void) {
        _cleanup_free_ char *what = NULL;
        const char *opts;

        if (!arg_usr_what && !arg_usr_fstype && !arg_usr_options)
                return 0;

        if (arg_root_what && !arg_usr_what) {
                arg_usr_what = strdup(arg_root_what);

                if (!arg_usr_what)
                        return log_oom();
        }

        if (arg_root_fstype && !arg_usr_fstype) {
                arg_usr_fstype = strdup(arg_root_fstype);

                if (!arg_usr_fstype)
                        return log_oom();
        }

        if (arg_root_options && !arg_usr_options) {
                arg_usr_options = strdup(arg_root_options);

                if (!arg_usr_options)
                        return log_oom();
        }

        if (!arg_usr_what || !arg_usr_options)
                return 0;

        what = fstab_node_to_udev_node(arg_usr_what);
        if (!path_is_absolute(what)) {
                log_debug("Skipping entry what=%s where=/sysroot/usr type=%s", what, strna(arg_usr_fstype));
                return -1;
        }

        opts = arg_usr_options;

        log_debug("Found entry what=%s where=/sysroot/usr type=%s", what, strna(arg_usr_fstype));
        return add_mount(what,
                         "/sysroot/usr",
                         arg_usr_fstype,
                         opts,
                         1,
                         false,
                         false,
                         false,
                         SPECIAL_INITRD_ROOT_FS_TARGET,
                         "/proc/cmdline");
}

static int parse_proc_cmdline_item(const char *key, const char *value) {
        int r;

        /* root=, usr=, usrfstype= and roofstype= may occur more than once, the last
         * instance should take precedence.  In the case of multiple rootflags=
         * or usrflags= the arguments should be concatenated */

        if (STR_IN_SET(key, "fstab", "rd.fstab") && value) {

                r = parse_boolean(value);
                if (r < 0)
                        log_warning("Failed to parse fstab switch %s. Ignoring.", value);
                else
                        arg_fstab_enabled = r;

        } else if (streq(key, "root") && value) {

                if (free_and_strdup(&arg_root_what, value) < 0)
                        return log_oom();

        } else if (streq(key, "rootfstype") && value) {

                if (free_and_strdup(&arg_root_fstype, value) < 0)
                        return log_oom();

        } else if (streq(key, "rootflags") && value) {
                char *o;

                o = arg_root_options ?
                        strjoin(arg_root_options, ",", value, NULL) :
                        strdup(value);
                if (!o)
                        return log_oom();

                free(arg_root_options);
                arg_root_options = o;

        } else if (streq(key, "mount.usr") && value) {

                if (free_and_strdup(&arg_usr_what, value) < 0)
                        return log_oom();

        } else if (streq(key, "mount.usrfstype") && value) {

                if (free_and_strdup(&arg_usr_fstype, value) < 0)
                        return log_oom();

        } else if (streq(key, "mount.usrflags") && value) {
                char *o;

                o = arg_usr_options ?
                        strjoin(arg_usr_options, ",", value, NULL) :
                        strdup(value);
                if (!o)
                        return log_oom();

                free(arg_usr_options);
                arg_usr_options = o;

        } else if (streq(key, "rw") && !value)
                arg_root_rw = true;
        else if (streq(key, "ro") && !value)
                arg_root_rw = false;

        return 0;
}

int main(int argc, char *argv[]) {
        int r = 0;

        if (argc > 1 && argc != 4) {
                log_error("This program takes three or no arguments.");
                return EXIT_FAILURE;
        }

        if (argc > 1)
                arg_dest = argv[1];

        log_set_target(LOG_TARGET_SAFE);
        log_parse_environment();
        log_open();

        umask(0022);

        r = parse_proc_cmdline(parse_proc_cmdline_item);
        if (r < 0)
                log_warning_errno(-r, "Failed to parse kernel command line, ignoring: %m");

        /* Always honour root= and usr= in the kernel command line if we are in an initrd */
        if (in_initrd()) {
                r = add_root_mount();
                if (r == 0)
                        r = add_usr_mount();
        }

        /* Honour /etc/fstab only when that's enabled */
        if (arg_fstab_enabled) {
                int k;

                log_debug("Parsing /etc/fstab");

                /* Parse the local /etc/fstab, possibly from the initrd */
                k = parse_fstab(false);
                if (k < 0)
                        r = k;

                /* If running in the initrd also parse the /etc/fstab from the host */
                if (in_initrd()) {
                        log_debug("Parsing /sysroot/etc/fstab");

                        k = parse_fstab(true);
                        if (k < 0)
                                r = k;
                }
        }

        free(arg_root_what);

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
