#pragma once

/* IR peer bridge.
 *
 * When attached, each byte the emulator transmits via SCI3 is written to
 * the bridge fd, and ir_recv_poll() pulls pending bytes from it. Used
 * by `pwdbg duo` to connect two walker instances over a socketpair so
 * one's TX becomes the other's RX, modelling the IR link between two
 * physical Pokewalkers.
 *
 * A single fd is supported — the bridge is per-process, matching the
 * "one walker per process" model. */

/* Attach an fd. Pass -1 to detach. The fd should be non-blocking;
 * ir_bridge_attach() will set O_NONBLOCK and disable SIGPIPE. */
void ir_bridge_attach(int fd);

/* Current fd, or -1 if unattached. */
int ir_bridge_fd(void);
