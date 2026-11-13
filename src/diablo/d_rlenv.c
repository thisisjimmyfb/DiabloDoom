// d_rlenv.c - RL environment mode (-rlenv). See d_rlenv.h and
// agent/training/SPEC.md.

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "doomtype.h"
#include "m_argv.h"
#include "d_loop.h"

#include "d_rlenv.h"

boolean rlenv = false;

static const char *rl_cmdpipe = "/tmp/doom_cmd.txt";
static int rl_statefd = -1;     // -1 = stdout with "[RL] " prefix

void RL_Init(void)
{
    extern int show_endoom;
    int p;

    if (M_CheckParm("-rlenv") <= 0)
        return;
    rlenv = true;

    p = M_CheckParmWithArgs("-cmdpipe", 1);
    if (p > 0)
        rl_cmdpipe = myargv[p + 1];

    p = M_CheckParmWithArgs("-statepipe", 1);
    if (p > 0)
    {
        const char *path = myargv[p + 1];
        struct stat st;
        // A dead reader must not kill the game with SIGPIPE: writes
        // then fail with EPIPE and RL_Emit stops writing.
        signal(SIGPIPE, SIG_IGN);
        if (stat(path, &st) == 0 && S_ISFIFO(st.st_mode))
        {
            // Blocking open: waits until the env opens the read end.
            printf("[RL] waiting for reader on state pipe %s\n", path);
            fflush(stdout);
            rl_statefd = open(path, O_WRONLY);
        }
        else
        {
            rl_statefd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        }
        if (rl_statefd < 0)
            printf("[RL] cannot open state pipe %s: %s (using stdout)\n",
                   path, strerror(errno));
    }

    // Fixed tics, as fast as possible: one tic per TryRunTics call,
    // never waiting on the wall clock.
    singletics = true;
    // ENDOOM waits for a keypress nobody will send.
    show_endoom = 0;

    printf("[RL] rlenv on: cmdpipe %s, state %s\n", rl_cmdpipe,
           rl_statefd >= 0 ? "pipe" : "stdout");
}

const char *RL_CmdPipePath(void)
{
    return rl_cmdpipe;
}

void RL_Emit(const char *fmt, ...)
{
    char buf[4096];
    va_list args;
    int n;

    if (!rlenv)
        return;

    va_start(args, fmt);
    n = vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    if (n < 0)
        return;
    if (n > (int)sizeof(buf) - 2)
        n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    buf[n] = '\0';

    if (rl_statefd < 0)
    {
        printf("[RL] %s", buf);
        fflush(stdout);
        return;
    }
    {
        // One write per line: lines under PIPE_BUF (4096) are atomic.
        const char *s = buf;
        while (n > 0)
        {
            ssize_t w = write(rl_statefd, s, n);
            if (w < 0)
            {
                if (errno == EINTR)
                    continue;
                printf("[RL] state pipe write failed: %s (using stdout)\n",
                       strerror(errno));
                close(rl_statefd);
                rl_statefd = -1;
                return;
            }
            s += w;
            n -= (int)w;
        }
    }
}
