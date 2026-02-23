#include "ui.h"
#include "backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>

/* ── Restore environment variables from --env-VAR=VALUE args ────────── */

static int restore_env(int *argc, char ***argv)
{
    int new_argc = 0;
    char **new_argv = malloc(*argc * sizeof(char *));
    if (!new_argv) return -1;

    for (int i = 0; i < *argc; i++) {
        if (strncmp((*argv)[i], "--env-", 6) == 0) {
            const char *eq = strchr((*argv)[i] + 6, '=');
            if (eq && eq > (*argv)[i] + 6) {
                int namelen = (int)(eq - ((*argv)[i] + 6));
                char name[256];
                if (namelen < (int)sizeof(name)) {
                    memcpy(name, (*argv)[i] + 6, namelen);
                    name[namelen] = '\0';
                    setenv(name, eq + 1, 1);
                }
            }
        } else {
            new_argv[new_argc++] = (*argv)[i];
        }
    }

    *argc = new_argc;
    *argv = new_argv;
    return 0;
}

/* ── Elevate to root via pkexec if not already root ─────────────────── */

static void elevate_if_necessary(int argc, char **argv)
{
    if (geteuid() == 0) return;

    /* Build argv for pkexec: pkexec <self> --env-VAR=VALUE ... [original args] */
    const char *env_vars[] = {
        "DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "XAUTHORITY",
        "DBUS_SESSION_BUS_ADDRESS", "XDG_CONFIG_HOME", "HOME",
        "AVALONIA_SCREEN_SCALE_FACTORS", NULL
    };

    /* Count env args */
    int env_count = 0;
    for (int i = 0; env_vars[i]; i++) {
        if (getenv(env_vars[i])) env_count++;
    }

    /* +3: pkexec, self, NULL; +argc for original args */
    int total = 2 + env_count + (argc - 1) + 1;
    char **new_argv = malloc(total * sizeof(char *));
    if (!new_argv) return;

    int n = 0;
    new_argv[n++] = "pkexec";

    /* Get self path */
    char self[4096];
    ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (len <= 0) { free(new_argv); return; }
    self[len] = '\0';

    /* Prefer installed path for polkit policy match */
    const char *installed = "/opt/TuxTimings/bin/tuxtimings";
    if (access(installed, X_OK) == 0)
        new_argv[n++] = (char *)installed;
    else
        new_argv[n++] = self;

    /* Add env forwarding args */
    char env_bufs[16][512];
    int ei = 0;
    for (int i = 0; env_vars[i] && ei < 16; i++) {
        const char *val = getenv(env_vars[i]);
        if (val) {
            snprintf(env_bufs[ei], sizeof(env_bufs[ei]), "--env-%s=%s", env_vars[i], val);
            new_argv[n++] = env_bufs[ei];
            ei++;
        }
    }

    /* If on Wayland, forward that hint */
    if (getenv("WAYLAND_DISPLAY")) {
        snprintf(env_bufs[ei], sizeof(env_bufs[ei]), "--env-GDK_BACKEND=wayland");
        new_argv[n++] = env_bufs[ei++];
    }

    /* Copy original args (skip argv[0]) */
    for (int i = 1; i < argc; i++)
        new_argv[n++] = argv[i];
    new_argv[n] = NULL;

    execvp("pkexec", new_argv);
    /* If execvp returns, pkexec failed */
    perror("pkexec");
    free(new_argv);
    exit(1);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    restore_env(&argc, &argv);
    elevate_if_necessary(argc, argv);

    /* Force C locale for numeric formatting (dots, not commas) */
    setlocale(LC_NUMERIC, "C");

    if (!backend_is_supported()) {
        fprintf(stderr, "TuxTimings: ryzen_smu driver not found at /sys/kernel/ryzen_smu_drv/\n"
                        "Please install the ryzen_smu kernel module.\n");
        return 1;
    }

    GtkApplication *app = ui_create(argc, argv);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
