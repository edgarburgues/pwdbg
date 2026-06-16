/* walker_ext.c — extension module that unity-includes pokestroller's
 * walker.c so we can reach its static state without modifying that
 * subproject. The Makefile compiles THIS file instead of walker.c —
 * otherwise we'd get duplicate symbols.
 *
 * The `#include "walker.c"` below drags in the full emulator core: all
 * statics (pc, ER, memory, eeprom, ...) end up in this translation
 * unit, so the accessors at the bottom can read/write them directly. */

#include "walker.c"

#include "walker_ext.h"

#include <stdlib.h>
#include <string.h>

/* --- Registers ---------------------------------------------------- */

static uint8_t pack_ccr(const struct Flags_t *f) {
 uint8_t b = 0;
 if (f->C) b |= 1 << 0;
 if (f->V) b |= 1 << 1;
 if (f->Z) b |= 1 << 2;
 if (f->N) b |= 1 << 3;
 if (f->U) b |= 1 << 4;
 if (f->H) b |= 1 << 5;
 if (f->UI) b |= 1 << 6;
 if (f->I) b |= 1 << 7;
 return b;
}

static void unpack_ccr(struct Flags_t *f, uint8_t b) {
 f->C = (b >> 0) & 1;
 f->V = (b >> 1) & 1;
 f->Z = (b >> 2) & 1;
 f->N = (b >> 3) & 1;
 f->U = (b >> 4) & 1;
 f->H = (b >> 5) & 1;
 f->UI = (b >> 6) & 1;
 f->I = (b >> 7) & 1;
}

void walker_get_regs(WalkerRegs *out) {
 for (int i = 0; i < 8; i++) out->er[i] = ER[i] ? *ER[i] : 0;
 out->pc = pc;
 out->ccr = pack_ccr(&flags);
 out->sleep = sleep;
}

void walker_set_reg(int idx, uint32_t value) {
 if (idx >= 0 && idx < 8) {
 if (ER[idx]) *ER[idx] = value;
 } else if (idx == 8) {
 pc = (uint16_t)value;
 } else if (idx == 9) {
 unpack_ccr(&flags, (uint8_t)value);
 }
}

/* --- Memory ------------------------------------------------------- */

void walker_mem_write(uint16_t addr, uint8_t value) {
 setMemory8(addr, value);
}

uint8_t walker_mem_read(uint16_t addr) {
 /* direct buffer read — avoids the SCI3 RDRF side-effect that
 * getMemory8(0xFF9D) triggers. For the REPL's plain `mem` dump we
 * want an observer, not a consumer. */
 return memory[addr];
}

/* --- EEPROM ------------------------------------------------------- */

uint8_t walker_eeprom_read(uint32_t addr) {
 if (addr >= EEPROM_SIZE) return 0;
 return eeprom.memory[addr];
}

void walker_eeprom_write(uint32_t addr, uint8_t value) {
 if (addr >= EEPROM_SIZE) return;
 eeprom.memory[addr] = value;
}

size_t walker_eeprom_size(void) { return EEPROM_SIZE; }

/* --- LCD framebuffer raw access (for diagnostics) ----------------- */
uint8_t walker_lcd_mem_read(size_t idx) {
 if (idx >= LCD_MEM_SIZE) return 0;
 return lcd.memory[idx];
}

size_t walker_lcd_mem_size(void) { return LCD_MEM_SIZE; }

uint8_t walker_lcd_current_buffer(void) { return lcd.currentBuffer; }
uint8_t walker_lcd_current_page (void) { return lcd.currentPage; }
uint8_t walker_lcd_current_column(void) { return lcd.currentColumn; }

/* --- Snapshot ----------------------------------------------------- */

#define SNAP_MAGIC "PWDBGS01"
#define SNAP_MAGIC_LEN 8

/* What we persist:
 * - memory[] (64 KiB) — full H8 address space incl. MMIO
 * - eeprom.memory[] (64 KiB) — SPI EEPROM contents
 * - accel.memory[] (64 B) — BMA150 register file
 * - lcd.memory[] (LCD_MEM_SIZE) — LCD framebuffer
 * - scalars: pc, flags, interruptSaved*, sleep, entry,
 * quartersEllapsed, subClockCyclesEllapsed
 * - inputQueue, TimerB (non-ptr fields), TimerW (non-ptr fields),
 * SSU (non-ptr fields), SCI3 (non-ptr fields),
 * eeprom (non-ptr fields), accel (non-ptr fields), lcd (non-ptr fields)
 *
 * Pointers into memory[] (ER, SSU.SSCRH, SCI3.SMR3, ...) are NOT saved —
 * they are stable across runNextInstruction calls because memory[] is
 * allocated once in initWalker and never moves. As long as we restore
 * state on the SAME process that already called initWalker, the
 * pointers remain valid. */

struct snap_scalars {
 uint16_t pc;
 struct Flags_t flags;
 uint16_t interruptSavedAddress;
 struct Flags_t interruptSavedFlags;
 uint8_t sleep;
 int32_t entry;
 uint8_t quartersEllapsed;
 uint64_t subClockCyclesEllapsed;
 struct Queue inputQueue;
 /* TimerB / TimerW non-ptr state */
 bool tb_on;
 uint8_t tb_TLBvalue;
 bool tw_on;
 /* SSU non-ptr */
 uint8_t ssu_SSTRSR;
 uint8_t ssu_progress;
 /* SCI3 non-ptr */
 uint32_t sci_txCountdown;
 uint8_t sci_txPending;
 bool sci_txHasPending;
 uint8_t sci_rxBuf[256];
 uint16_t sci_rxLen;
 uint16_t sci_rxPos;
 uint32_t sci_rxCountdown;
 uint32_t sci_txIdleCountdown;
 /* Eeprom non-ptr */
 uint8_t eeprom_status;
 uint8_t eeprom_hiAddr;
 uint8_t eeprom_loAddr;
 int32_t eeprom_state;
 uint16_t eeprom_offset;
 /* Accel non-ptr */
 uint8_t accel_addr;
 uint8_t accel_offset;
 int32_t accel_state;
 /* Lcd non-ptr */
 uint8_t lcd_contrast;
 int32_t lcd_state;
 uint8_t lcd_currentColumn;
 uint8_t lcd_currentPage;
 uint8_t lcd_currentByte;
 bool lcd_currentBuffer;
 uint8_t lcd_displayStartLine;
 bool lcd_startLineSet;
 bool lcd_startLineActive;
 uint8_t sci_lastReadSSR3;
};

static void gather_scalars(struct snap_scalars *s) {
 memset(s, 0, sizeof(*s));
 s->pc = pc;
 s->flags = flags;
 s->interruptSavedAddress = interruptSavedAddress;
 s->interruptSavedFlags = interruptSavedFlags;
 s->sleep = sleep ? 1 : 0;
 s->entry = entry;
 s->quartersEllapsed = quartersEllapsed;
 s->subClockCyclesEllapsed = subClockCyclesEllapsed;
 s->inputQueue = inputQueue;
 s->tb_on = TimerB.on; s->tb_TLBvalue = TimerB.TLBvalue;
 s->tw_on = TimerW.on;
 s->ssu_SSTRSR = SSU.SSTRSR; s->ssu_progress = SSU.progress;
 s->sci_txCountdown = SCI3.txCountdown;
 s->sci_txPending = SCI3.txPending;
 s->sci_txHasPending = SCI3.txHasPending;
 memcpy(s->sci_rxBuf, SCI3.rxBuf, sizeof(SCI3.rxBuf));
 s->sci_rxLen = SCI3.rxLen;
 s->sci_rxPos = SCI3.rxPos;
 s->sci_rxCountdown = SCI3.rxCountdown;
 s->sci_txIdleCountdown = SCI3.txIdleCountdown;
 s->eeprom_status = eeprom.status;
 s->eeprom_hiAddr = eeprom.buffer.hiAddress;
 s->eeprom_loAddr = eeprom.buffer.loAddress;
 s->eeprom_state = eeprom.buffer.state;
 s->eeprom_offset = eeprom.buffer.offset;
 s->accel_addr = accel.buffer.address;
 s->accel_offset = accel.buffer.offset;
 s->accel_state = accel.buffer.state;
 s->lcd_contrast = lcd.contrast;
 s->lcd_state = lcd.state;
 s->lcd_currentColumn = lcd.currentColumn;
 s->lcd_currentPage = lcd.currentPage;
 s->lcd_currentByte = lcd.currentByte;
 s->lcd_currentBuffer = lcd.currentBuffer;
 s->lcd_displayStartLine = lcd.displayStartLine;
 s->lcd_startLineSet = lcd.startLineSet;
 s->lcd_startLineActive = lcd.startLineActive;
 s->sci_lastReadSSR3 = lastReadSSR3;
}

static void apply_scalars(const struct snap_scalars *s) {
 pc = s->pc;
 flags = s->flags;
 interruptSavedAddress = s->interruptSavedAddress;
 interruptSavedFlags = s->interruptSavedFlags;
 sleep = s->sleep ? true : false;
 entry = s->entry;
 quartersEllapsed = s->quartersEllapsed;
 subClockCyclesEllapsed = s->subClockCyclesEllapsed;
 inputQueue = s->inputQueue;
 TimerB.on = s->tb_on; TimerB.TLBvalue = s->tb_TLBvalue;
 TimerW.on = s->tw_on;
 SSU.SSTRSR = s->ssu_SSTRSR; SSU.progress = s->ssu_progress;
 SCI3.txCountdown = s->sci_txCountdown;
 SCI3.txPending = s->sci_txPending;
 SCI3.txHasPending = s->sci_txHasPending;
 memcpy(SCI3.rxBuf, s->sci_rxBuf, sizeof(SCI3.rxBuf));
 SCI3.rxLen = s->sci_rxLen;
 SCI3.rxPos = s->sci_rxPos;
 SCI3.rxCountdown = s->sci_rxCountdown;
 SCI3.txIdleCountdown= s->sci_txIdleCountdown;
 eeprom.status = s->eeprom_status;
 eeprom.buffer.hiAddress = s->eeprom_hiAddr;
 eeprom.buffer.loAddress = s->eeprom_loAddr;
 eeprom.buffer.state = s->eeprom_state;
 eeprom.buffer.offset = s->eeprom_offset;
 accel.buffer.address = s->accel_addr;
 accel.buffer.offset = s->accel_offset;
 accel.buffer.state = s->accel_state;
 lcd.contrast = s->lcd_contrast;
 lcd.state = s->lcd_state;
 lcd.currentColumn = s->lcd_currentColumn;
 lcd.currentPage = s->lcd_currentPage;
 lcd.currentByte = s->lcd_currentByte;
 lcd.currentBuffer = s->lcd_currentBuffer;
 lcd.displayStartLine = s->lcd_displayStartLine;
 lcd.startLineSet = s->lcd_startLineSet;
 lcd.startLineActive = s->lcd_startLineActive;
 lastReadSSR3 = s->sci_lastReadSSR3;
}

int walker_save_state(void **buf_out, size_t *len_out) {
 const size_t head = SNAP_MAGIC_LEN + 4; /* magic + version */
 const size_t mem_sz = MEM_SIZE;
 const size_t eep_sz = EEPROM_SIZE;
 const size_t acc_sz = 64;
 const size_t lcd_sz = LCD_MEM_SIZE;
 const size_t sca_sz = sizeof(struct snap_scalars);
 const size_t total = head + mem_sz + eep_sz + acc_sz + lcd_sz + sca_sz;

 uint8_t *buf = malloc(total);
 if (!buf) return -1;

 uint8_t *p = buf;
 memcpy(p, SNAP_MAGIC, SNAP_MAGIC_LEN); p += SNAP_MAGIC_LEN;
 uint32_t ver = 1;
 memcpy(p, &ver, 4); p += 4;
 memcpy(p, memory, mem_sz); p += mem_sz;
 memcpy(p, eeprom.memory, eep_sz); p += eep_sz;
 memcpy(p, accel.memory, acc_sz); p += acc_sz;
 memcpy(p, lcd.memory, lcd_sz); p += lcd_sz;
 struct snap_scalars sc;
 gather_scalars(&sc);
 memcpy(p, &sc, sca_sz);

 *buf_out = buf;
 *len_out = total;
 return 0;
}

/* --- Peer-play identity patch ------------------------------------- */

static bool peer_patch_enabled = false;
static bool force_slave_enabled = false;

void walker_set_peer_patch(bool on) { peer_patch_enabled = on; }
void walker_set_force_slave(bool on) { force_slave_enabled = on; }

/* Called by our patched runNextInstruction below just before the two
 * identity-check entry points. The pc values 0x0D58 / 0x0E48 are the
 * first instruction after readReliableData returns (our own identity
 * is now in the TX buffer at 0xF8D6, peer identity at 0xF7E6).
 *
 * By installing valid flag bytes here, the ROM's bld/bcs chain takes
 * the "success" branch and the walker replies with its own identity
 * (cmd 0x12) instead of the commsErrorId=3 shutdown path. */
/* fwd decl for event emission from the hook */
extern void ev_log_hook(const char *kind, uint16_t pc);

static void peer_patch_apply(void) {
 /* peer identity (at RAM 0xF7E6 block, offset 0x5B..0x5E) */
 memory[0xF841] = 0x03; /* flags: paired | hasPoke */
 memory[0xF842] = 0x02; /* protoVer */
 memory[0xF844] = 0x00; /* protoSubver */

 /* our own identity in TX payload buffer (0xF8D6 block, offset
 * 0x5B / 0x5C) — readReliableData just loaded it from EEPROM, we
 * patch before the check reads it. */
 memory[0xF931] = 0x03;
 memory[0xF932] = 0x02;

 ev_log_hook("peer_patch", pc);
}

/* Emits an "ir_dispatch" event at the session-check point of
 * irCommsEventLoop so we can see exactly which packets are accepted
 * / rejected. Always on. */
static void maybe_log_ir_dispatch(void) {
 extern void ev_log_ir_dispatch(uint32_t pktSession, uint32_t sessionId,
 uint8_t cmd, uint8_t peerRole);
 extern void ev_log_regs(const char *kind, uint16_t pc, uint32_t er0, uint32_t er1);

 if (pc == 0x0AA4) {
 uint32_t pkt_s = *ER[0];
 uint32_t sess = *(uint32_t *)&memory[0xF8BA];
 uint8_t cmd_b = *RL[3];
 ev_log_ir_dispatch(pkt_s, sess, cmd_b, memory[0xF8BE]);
 }
 /* XOR.L in handle_ACK (pre) / post */
 if (pc == 0x0CFC) ev_log_regs("pre_xor_ack", pc, *ER[0], *ER[1]);
 if (pc == 0x0D00) ev_log_regs("post_xor_ack", pc, *ER[0], *ER[1]);
 /* XOR.L in handle_SYN case 1 */
 if (pc == 0x0C7A) ev_log_regs("pre_xor_syn", pc, *ER[0], *ER[1]);
 if (pc == 0x0C7E) ev_log_regs("post_xor_syn", pc, *ER[0], *ER[1]);
}

void walker_preexec_hook(void) {
 maybe_log_ir_dispatch();
 if (peer_patch_enabled) {
 /* Apply identity patches just BEFORE the mov.b that loads
 * our_ident[0x5B] into r0l (then the ROM's bld reads r0l).
 * handle_CMD_10 reads at 0x0D58; handle_CMD_12 at 0x0E46.
 * Patching at 0x0E48 (the bld) is too late — r0l was already
 * loaded with pre-patch value and the check would still fail. */
 if (pc == 0x0D58 || pc == 0x0E46) peer_patch_apply();

 /* Skip seeIfWeSawThisTrainerBefore (ROM 0x6784) — with empty
 * EEPROM, the all-zero trainer slots match the peer's all-zero
 * uniq identity block and the ROM thinks we've already met. */
 if (pc == 0x0E02) { pc = 0x0E1E; ev_log_hook("skip_seen", pc); }
 if (pc == 0x0EB4) { pc = 0x0ED0; ev_log_hook("skip_seen", pc); }
 }
 if (force_slave_enabled) {
 /* 0x0C3C = handle_SYN entry (ORIGINAL ROM). Overwrite peerRole = 1
 * so the switch takes the "become SLAVE" branch even if we were
 * already at peerRole=2/3/4. */
 if (pc == 0x0C3C) {
 memory[0xF8BE] = 0x01;
 ev_log_hook("force_slave", pc);
 }
 /* the custom ROM (entry 0x0080): its FA handler is a static inlined into the
 * dispatcher, so there is no symbol to anchor a PC hook on. Instead
 * the hook is RAM-driven — the custom ROM's RX buffer (0xF8CE, XOR-decoded,
 * faithful to the ROM RX assembler at 0x90e-0x922; shared half-duplex
 * with the TX header) and peer-role byte (0xF8BE) are stable
 * ROM-mirror addresses that survive every rebuild. When a decoded FA
 * (peer's master announce) sits in the buffer while we are contesting
 * the role (F8BE==2 = we announced too — the deterministic-twins
 * deadlock), flip to 1 so the FA handler takes its slave branch.
 * Fires before the dispatch reads the role (this hook runs before
 * every instruction). Only needed to force the orig-master pairing
 * deterministically — with the custom ROM's SSR3-race fix the natural role war
 * resolves on its own, usually with the custom ROM as master. */
 if (entry == 0x0080 &&
 memory[0xF8CE] == 0xFA && memory[0xF8BE] == 0x02) {
 memory[0xF8BE] = 0x01;
 ev_log_hook("force_slave_v2", pc);
 }
 }
}

int walker_load_state(const void *buf_in, size_t len) {
 const size_t head = SNAP_MAGIC_LEN + 4;
 const size_t mem_sz = MEM_SIZE;
 const size_t eep_sz = EEPROM_SIZE;
 const size_t acc_sz = 64;
 const size_t lcd_sz = LCD_MEM_SIZE;
 const size_t sca_sz = sizeof(struct snap_scalars);
 const size_t expected = head + mem_sz + eep_sz + acc_sz + lcd_sz + sca_sz;

 if (len != expected) return -1;
 const uint8_t *p = buf_in;
 if (memcmp(p, SNAP_MAGIC, SNAP_MAGIC_LEN) != 0) return -2;
 p += SNAP_MAGIC_LEN;
 uint32_t ver; memcpy(&ver, p, 4); p += 4;
 if (ver != 1) return -3;

 memcpy(memory, p, mem_sz); p += mem_sz;
 memcpy(eeprom.memory, p, eep_sz); p += eep_sz;
 memcpy(accel.memory, p, acc_sz); p += acc_sz;
 memcpy(lcd.memory, p, lcd_sz); p += lcd_sz;
 struct snap_scalars sc;
 memcpy(&sc, p, sca_sz);
 apply_scalars(&sc);
 return 0;
}
