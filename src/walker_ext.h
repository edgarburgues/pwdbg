#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Extension API over walker.c.
 *
 * walker_ext.c unity-includes the pokestroller walker.c (via the -I
 * path) and exposes the static state it needs via these accessors.
 * This lets pwdbg read/write the full CPU + peripheral state without
 * modifying pokestroller sources. */

/* Registers snapshot — matches the H8/300H programmer's view. */
typedef struct WalkerRegs {
 uint32_t er[8]; /* general-purpose 32-bit regs (ER0..ER7 ; ER7 aliases SP) */
 uint16_t pc;
 uint8_t ccr; /* [I UI H U N Z V C] packed */
 bool sleep; /* SLEEP instruction pending */
} WalkerRegs;

void walker_get_regs(WalkerRegs *out);
void walker_set_reg(int idx, uint32_t value); /* idx 0..7 → ER, 8 → PC (u16), 9 → CCR (u8) */

/* Memory helpers that stay inside the emulator's mapping (so MMIO edges
 * and LCD/EEPROM/SSU side-effects fire exactly as the ROM would see). */
void walker_mem_write(uint16_t addr, uint8_t value);
uint8_t walker_mem_read (uint16_t addr);

/* Raw EEPROM read/write bypassing the SPI emulation — for inspection
 * dumps and restoring saved state. */
uint8_t walker_eeprom_read (uint32_t addr);
void walker_eeprom_write(uint32_t addr, uint8_t value);
size_t walker_eeprom_size (void);

/* LCD framebuffer raw access (diagnostic). Reads the internal
 * lcd.memory[] array maintained by the SSU emulation when the ROM
 * writes pixel data in LCD-data mode. Distinct from fillVideoBuffer,
 * which interprets the same memory through the SSD1854 page+stripe
 * layout and toggles currentBuffer; that toggling can mask whether
 * recent writes actually landed. Use walker_lcd_mem_read for raw
 * "did the SSU write what I expected" inspection. */
uint8_t walker_lcd_mem_read (size_t idx);
size_t walker_lcd_mem_size (void);
uint8_t walker_lcd_current_buffer(void);
uint8_t walker_lcd_current_page (void);
uint8_t walker_lcd_current_column(void);

/* Snapshot. The returned buffer is heap-allocated; caller frees.
 * Format is private to the build — version-tagged, not portable across
 * walker.c revisions. */
int walker_save_state(void **buf_out, size_t *len_out);
int walker_load_state(const void *buf, size_t len);

/* Peer-play test hook.
 *
 * When enabled, patches RAM at the entry of handle_CMD_10 / handle_CMD_12
 * (the post-handshake identity-exchange handlers in the ROM) so the
 * walker's identity-validation checks pass even with a fresh/unpaired
 * EEPROM. Writes:
 *
 * peer_identity[0x5B] (@ 0xF841) = 0x03 // paired + has poke
 * peer_identity[0x5C] (@ 0xF842) = 0x02 // protoVer
 * peer_identity[0x5E] (@ 0xF844) = 0x00 // protoSubver
 * our_identity[0x5B] (@ 0xF931) = 0x03
 * our_identity[0x5C] (@ 0xF932) = 0x02
 *
 * Without this patch, two walkers with identical or fresh EEPROMs hit
 * the "cannot complete" error at handle_CMD_10 because their identity
 * blocks have flags byte = 0. A real pair of paired walkers would
 * have valid identity blocks in their respective EEPROMs. pwdbg fakes
 * that state via this hook for test purposes. */
void walker_set_peer_patch(bool on);

/* Force this peer to always take the "become SLAVE" branch of
 * handle_SYN (ROM 0x0C3C). Without this, two peers racing through
 * the symmetric beacon/SYN exchange both reach peerRole=2 and get
 * stuck in mutual random_backoff because neither is at peerRole=1
 * when the other's SYN arrives. Enabling force-slave on one peer
 * (and leaving the other as normal master) gives deterministic
 * handshake completion: the forced peer becomes SLAVE on any SYN,
 * sends ACK, and the unforced peer advances to CONNECTED. */
void walker_set_force_slave(bool on);

/* Pre-execution hook — call before each runNextInstruction. Checks
 * the current PC against pwdbg's runtime patches (peer_patch,
 * force_slave, etc.) and applies RAM writes if a match fires. No-op
 * if no patches are enabled. Safe to call every instruction. */
void walker_preexec_hook(void);
