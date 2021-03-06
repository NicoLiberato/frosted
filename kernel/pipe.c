/*
 *      This file is part of frosted.
 *
 *      frosted is free software: you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License version 2, as
 *      published by the Free Software Foundation.
 *
 *
 *      frosted is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with frosted.  If not, see <http://www.gnu.org/licenses/>.
 *
 *      Authors: Daniele Lacamera
 *
 */

#include "frosted.h"
#include "cirbuf.h"
#include "string.h"
#include "sys/termios.h"
#include "poll.h"

#define PIPE_BUFSIZE 64

static struct module mod_pipe;


struct pipe_priv {
    struct fnode *fno_r;
    struct fnode *fno_w;
    int pid_r;
    int pid_w;
    int w_off;
    struct cirbuf *cb;
};

static struct fnode PIPE_ROOT = {
};


int sys_pipe2_hdlr(int paddr, int flags)
{
    int *pfd = (int*)paddr;
    struct fnode *rd, *wr;
    struct pipe_priv *pp;
    pp = kalloc(sizeof (struct pipe_priv));
    if (!pp) {
        return -ENOMEM;
    }

    rd = fno_create(&mod_pipe, "", &PIPE_ROOT);
    if (!rd) {
        goto fail_rd;
    }
    wr = fno_create(&mod_pipe, "", &PIPE_ROOT);
    if (!wr) {
        goto fail_wr;
    }

    pfd[0] = task_filedesc_add(rd);
    pfd[1] = task_filedesc_add(wr);

    if (pfd[0] < 0 || pfd[1] < 0) {
        goto fail_all;
    }

    task_fd_setmask(pfd[0], O_RDONLY);
    task_fd_setmask(pfd[1], O_WRONLY);

    rd->priv = pp;
    wr->priv = pp;

    pp->fno_r = rd;
    pp->fno_w = wr;
    pp->pid_r = 0;
    pp->pid_w = 0;
    pp->w_off = 0;
    pp->cb = cirbuf_create(PIPE_BUFSIZE);
    if (!pp->cb) {
        goto fail_all;
    }
    return 0;

fail_all:
        fno_unlink(wr);
fail_wr:
        fno_unlink(rd);
fail_rd:
        kfree(pp);
        return -ENOMEM;

}

static int pipe_poll(struct fnode *f, uint16_t events, uint16_t *revents)
{
    struct pipe_priv *pp;
    *revents = 0;
    if (f->owner != &mod_pipe)
        return -EINVAL;
    pp = (struct pipe_priv *)f->priv;
    if (!pp) {
        return -EINVAL;
    }

    if (f == pp->fno_w) {
        if(pp->fno_r == 0) {
            *revents |= POLLHUP;
            return 1;
        }
        else if ((events & POLLOUT) && (cirbuf_bytesfree(pp->cb) > 0)) {
            *revents = POLLOUT;
            return 1;
        }
    }

    if ((f == pp->fno_r) && (events & POLLIN) && (cirbuf_bytesinuse(pp->cb) > 0)) {
        *revents |= POLLIN;
        return 1;
    }
    return 0;
}


static int pipe_close(struct fnode *f)
{
    struct pipe_priv *pp;
    uint16_t pid;
    pid = scheduler_get_cur_pid();
    if (!f)
        return -EINVAL;

    if (f->owner != &mod_pipe)
        return -EINVAL;

    pp = (struct pipe_priv *)f->priv;
    if (!pp)
        return -EINVAL;


    if ((f == pp->fno_r) && (f->usage == 1)) {
        pp->fno_r = NULL;
        fno_unlink(f);
        if ((pp->pid_w != pid) && (pp->pid_w > 0)) {
            task_resume(pp->pid_w);
        }
    }
    if ((f == pp->fno_w) && (f->usage == 1)) {
        pp->fno_w = NULL;
        fno_unlink(f);
        if ((pp->pid_r != pid) && (pp->pid_r > 0)) {
            task_resume(pp->pid_r);
        }
    }
    if ((!pp->fno_w) && (!pp->fno_r))
        kfree(pp);
    return 0;
}

static int pipe_read(struct fnode *f, void *buf, unsigned int len)
{
    struct pipe_priv *pp;
    int out, len_available;
    uint8_t *ptr = buf;

    if (f->owner != &mod_pipe)
        return -EINVAL;

    pp = (struct pipe_priv *)f->priv;
    if (!pp)
        return -EINVAL;

    if (pp->fno_r != f)
        return -EPERM;

    len_available =  cirbuf_bytesinuse(pp->cb);
    if (len_available <= 0) {
        pp->pid_r = scheduler_get_cur_pid();
        task_suspend();
        return SYS_CALL_AGAIN;
    }

    for(out = 0; out < len; out++) {
        /* read data */
        if (cirbuf_readbyte(pp->cb, ptr) != 0)
            break;
        ptr++;
    }
    pp->pid_r = 0;
    return out;
}

static int pipe_write(struct fnode *f, const void *buf, unsigned int len)
{
    struct pipe_priv *pp;
    int out, len_available;
    const uint8_t *ptr = buf;

    if (f->owner != &mod_pipe)
        return -EINVAL;

    pp = (struct pipe_priv *)f->priv;
    if (!pp)
        return -EINVAL;

    if (pp->fno_w != f)
        return -EPERM;

    out = pp->w_off;

    len_available =  cirbuf_bytesfree(pp->cb);
    if (len_available > (len - out))
        len_available = (len - out);
    for(; out < len_available; out++) {
        /* write data */
        if (cirbuf_writebyte(pp->cb, *(ptr + out)) != 0)
            break;
    }

    if (out < len) {
        pp->pid_w = scheduler_get_cur_pid();
        pp->w_off = out;
        task_suspend();
        return SYS_CALL_AGAIN;
    }

    pp->w_off = 0;
    pp->pid_w = 0;
    return out;
}

void sys_pipe_init(void)
{
    mod_pipe.family = FAMILY_DEV;
    strcpy(mod_pipe.name,"pipe");
    mod_pipe.ops.poll = pipe_poll;
    mod_pipe.ops.close = pipe_close;
    mod_pipe.ops.read = pipe_read;
    mod_pipe.ops.write = pipe_write;


    register_module(&mod_pipe);
}
