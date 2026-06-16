/* test_bridge.c — smoke test of the IR bridge pipe.
 *
 * Creates a socketpair, attaches one end via ir_bridge_attach, and
 * exercises ir_tx_byte / ir_recv_poll to confirm bytes flow correctly
 * in both directions. Walker is NOT initialized — getPC() returns
 * whatever (0), and that's fine for this test. */

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/ir.h"
#include "../src/ir_bridge.h"

int main(void) {
 int sv[2];
 assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

 /* Set BOTH ends non-blocking so our recv() on sv[1] doesn't hang. */
 fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL) | O_NONBLOCK);

 ir_bridge_attach(sv[0]);
 assert(ir_bridge_fd() == sv[0]);

 /* Direction 1: walker TX → other end receives. */
 ir_tx_byte(0xFC);
 ir_tx_byte(0xFA);
 ir_tx_byte(0x01);

 uint8_t rx[8];
 ssize_t n = recv(sv[1], rx, sizeof rx, 0);
 if (n != 3) { fprintf(stderr, "expected 3 bytes, got %zd\n", n); return 1; }
 if (rx[0] != 0xFC || rx[1] != 0xFA || rx[2] != 0x01) {
 fprintf(stderr, "bad content: %02X %02X %02X\n", rx[0], rx[1], rx[2]);
 return 1;
 }
 printf("TX path ok (3 bytes delivered)\n");

 /* Direction 2: other end writes → walker ir_recv_poll reads. */
 uint8_t out[] = {0xAA, 0xBB, 0xCC};
 write(sv[1], out, sizeof out);

 uint8_t pollbuf[16];
 size_t got = ir_recv_poll(pollbuf, sizeof pollbuf);
 if (got != 3) { fprintf(stderr, "expected 3 on poll, got %zu\n", got); return 1; }
 if (pollbuf[0] != 0xAA || pollbuf[1] != 0xBB || pollbuf[2] != 0xCC) {
 fprintf(stderr, "bad poll content\n");
 return 1;
 }
 printf("RX path ok (3 bytes received)\n");

 /* Empty poll returns zero cleanly. */
 got = ir_recv_poll(pollbuf, sizeof pollbuf);
 if (got != 0) { fprintf(stderr, "expected empty poll, got %zu\n", got); return 1; }
 printf("empty-poll ok\n");

 ir_bridge_attach(-1);
 close(sv[0]); close(sv[1]);
 printf("PASS\n");
 return 0;
}
