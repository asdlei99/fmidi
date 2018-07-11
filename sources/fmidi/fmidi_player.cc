//          Copyright Jean Pierre Cimalando 2018.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE.md or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include "fmidi.h"
#include <ev.h>
#include <memory>
#include <algorithm>
#include <assert.h>

struct fmidi_player_context {
    fmidi_player_t *plr;
    fmidi_seq_u seq;
    double timepos;
    double speed;
    ev_tstamp prev_tick;
    bool have_tick;
    bool have_event;
    fmidi_seq_event_t sqevt;
    void (*cbfn)(const fmidi_event_t *, void *);
    void *cbdata;
    void (*finifn)(void *);
    void *finidata;
};

struct fmidi_player {
    struct ev_loop *loop;
    ev_timer timer;
    bool running;
    fmidi_player_context ctx;
};

static void fmidi_player_tick(struct ev_loop *loop, ev_timer *timer, int)
{
    fmidi_player_context &ctx = *(fmidi_player_context *)timer->data;
    fmidi_player_t &plr = *ctx.plr;
    fmidi_seq_t &seq = *ctx.seq;
    void (*cbfn)(const fmidi_event_t *, void *) = ctx.cbfn;
    void *cbdata = ctx.cbdata;

    double timepos = ctx.timepos;
    bool have_event = ctx.have_event;
    fmidi_seq_event_t &sqevt = ctx.sqevt;
    ev_tstamp now = ev_now(loop);

    if (ctx.have_tick)
        timepos += ctx.speed * (now - ctx.prev_tick);

    bool more = have_event || fmidi_seq_next_event(&seq, &sqevt);
    if (more) {
        have_event = true;
        while (more && timepos > sqevt.time) {
            const fmidi_event_t &event = *sqevt.event;
            if (cbfn)
                cbfn(&event, cbdata);
            have_event = more = fmidi_seq_next_event(&seq, &sqevt);
        }
    }

    ctx.have_event = have_event;
    ctx.have_tick = true;
    ctx.prev_tick = now;
    ctx.timepos = timepos;

    if (!more) {
        ev_timer_stop(loop, timer);
        plr.running = false;
        if (ctx.finifn)
            ctx.finifn(ctx.finidata);
    }
}

fmidi_player_t *fmidi_player_new(fmidi_smf_t *smf, struct ev_loop *loop)
{
    fmidi_player_u plr(new fmidi_player_t);
    plr->loop = loop;

    fmidi_player_context &ctx = plr->ctx;
    ctx.plr = plr.get();
    ctx.seq.reset(fmidi_seq_new(smf));
    ctx.timepos = 0;
    ctx.speed = 1;
    ctx.have_tick = false;
    ctx.have_event = false;
    ctx.cbfn = nullptr;
    ctx.cbdata = nullptr;
    ctx.finifn = nullptr;
    ctx.finidata = nullptr;

    constexpr double seq_interval = 1e-3;
    ev_timer_init(&plr->timer, &fmidi_player_tick, 0.0, seq_interval);
    plr->timer.data = &ctx;
    plr->running = false;

    return plr.release();
}

void fmidi_player_free(fmidi_player_t *plr)
{
    if (plr->running) {
        struct ev_loop *loop = plr->loop;
        ev_timer_stop(loop, &plr->timer);
    }
    delete plr;
}

void fmidi_player_start(fmidi_player_t *plr)
{
    if (plr->running)
        return;

    fmidi_player_context &ctx = plr->ctx;
    ctx.have_tick = false;

    struct ev_loop *loop = plr->loop;
    ev_timer_start(loop, &plr->timer);

    plr->running = true;
}

void fmidi_player_stop(fmidi_player_t *plr)
{
    if (!plr->running)
        return;

    fmidi_player_context &ctx = plr->ctx;
    ctx.have_tick = false;

    struct ev_loop *loop = plr->loop;
    ev_timer_stop(loop, &plr->timer);

    plr->running = false;
}

void fmidi_player_rewind(fmidi_player_t *plr)
{
    fmidi_player_context &ctx = plr->ctx;
    fmidi_seq_rewind(ctx.seq.get());
    ctx.timepos = 0;
    ctx.have_tick = false;
    ctx.have_event = false;
}

bool fmidi_player_running(const fmidi_player_t *plr)
{
    return plr->running;
}

double fmidi_player_current_time(const fmidi_player_t *plr)
{
    return plr->ctx.timepos;
}

void fmidi_player_goto_time(fmidi_player_t *plr, double time)
{
    fmidi_player_context &ctx = plr->ctx;
    fmidi_seq_t &seq = *ctx.seq;

    uint8_t programs[16];
    uint8_t controls[16 * 128];
    std::fill_n(programs, 16, 0);
    std::fill_n(controls, 16 * 128, 255);

    fmidi_player_rewind(plr);

    for (fmidi_seq_event_t sqevt;
         fmidi_seq_peek_event(&seq, &sqevt) && sqevt.time < time;) {
        const fmidi_event_t &evt = *sqevt.event;
        if (evt.type == fmidi_event_message) {
            uint8_t status = evt.data[0];
            if (status >> 4 == 0b1100 && evt.datalen == 2) {  // program change
                uint8_t channel = status & 0xf;
                programs[channel] = evt.data[1] & 127;
            }
            else if (status >> 4 == 0b1011 && evt.datalen == 3) {  // control change
                uint8_t channel = status & 0xf;
                uint8_t id = evt.data[1] & 127;
                controls[channel * 128 + id] = evt.data[2] & 127;
            }
        }
        fmidi_seq_next_event(&seq, nullptr);
    }

    ctx.timepos = time;

    if (ctx.cbfn) {
        uint8_t evtbuf[fmidi_event_sizeof(3)];
        fmidi_event_t *evt = (fmidi_event_t *)evtbuf;
        evt->type = fmidi_event_message;
        evt->delta = 0;

        for (unsigned c = 0; c < 16; ++c) {
            // all sound off
            evt->datalen = 3;
            evt->data[0] = (0b1011 << 4) | c;
            evt->data[1] = 120;
            evt->data[2] = 0;
            ctx.cbfn(evt, ctx.cbdata);
            // reset all controllers
            evt->datalen = 3;
            evt->data[0] = (0b1011 << 4) | c;
            evt->data[1] = 121;
            evt->data[2] = 0;
            ctx.cbfn(evt, ctx.cbdata);
            // program change
            evt->datalen = 2;
            evt->data[0] = (0b1100 << 4) | c;
            evt->data[1] = programs[c];
            ctx.cbfn(evt, ctx.cbdata);
            // control change
            for (unsigned id = 0; id < 128; ++id) {
                uint8_t val = controls[c * 128 + id];
                if (val < 128) {
                    evt->datalen = 3;
                    evt->data[0] = (0b1011 << 4) | c;
                    evt->data[1] = id;
                    evt->data[2] = val;
                    ctx.cbfn(evt, ctx.cbdata);
                }
            }
        }
    }
}

double fmidi_player_current_speed(const fmidi_player_t *plr)
{
    return plr->ctx.speed;
}

void fmidi_player_set_speed(fmidi_player_t *plr, double speed)
{
    plr->ctx.speed = speed;
}

double fmidi_player_current_clock_frequency(const fmidi_player_t *plr)
{
    return 1.0 / plr->timer.repeat;
}

void fmidi_player_set_clock_frequency(fmidi_player_t *plr, double freq)
{
    assert(freq > 0);
    plr->timer.repeat = 1.0 / freq;
}

void fmidi_player_event_callback(
    fmidi_player_t *plr, void (*cbfn)(const fmidi_event_t *, void *), void *cbdata)
{
    fmidi_player_context &ctx = plr->ctx;
    ctx.cbfn = cbfn;
    ctx.cbdata = cbdata;
}

void fmidi_player_finish_callback(
    fmidi_player_t *plr, void (*cbfn)(void *), void *cbdata)
{
    fmidi_player_context &ctx = plr->ctx;
    ctx.finifn = cbfn;
    ctx.finidata = cbdata;
}
