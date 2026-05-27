/* ======================================================================== */
/*                            MAIN 68K CORE                                 */
/* ======================================================================== */

extern int vdp_68k_irq_ack(int int_level);

#define m68ki_cpu m68k
#define MUL (7)

#ifndef MD_COMPRESSED_CYCLE_TABLE
#define MD_COMPRESSED_CYCLE_TABLE 0
#endif

/* ======================================================================== */
/* ================================ INCLUDES ============================== */
/* ======================================================================== */

#ifndef BUILD_TABLES
  #ifndef TABLES_FULL
    #if MD_COMPRESSED_CYCLE_TABLE
      #include "m68ki_cycles_topbyte.h"
    #else
      #include "m68ki_cycles.h"
    #endif
  #else
    #include "m68ki_cycles_full.h"
  #endif
#endif

#include "m68kconf.h"
#include "m68kcpu.h"
#include "m68kops.h"
#include "gwenesis_savestate.h"
#include "esp_attr.h"
#include <string.h>
/* ======================================================================== */
/* ================================= DATA ================================= */
/* ======================================================================== */

#ifdef BUILD_TABLES
static unsigned char m68ki_cycles[0x10000];
#endif

static int irq_latency;

m68ki_cpu_core m68k;

#ifndef MD_OPCODE_HISTOGRAM
#define MD_OPCODE_HISTOGRAM 0
#endif
#ifndef MD_OPCODE_PROFILING
#define MD_OPCODE_PROFILING 0
#endif

#ifndef MD_TOPBYTE_COMPRESSED_DISPATCH
#define MD_TOPBYTE_COMPRESSED_DISPATCH 0
#endif
#ifndef MD_HYBRID_TOPBYTE_DISPATCH
#define MD_HYBRID_TOPBYTE_DISPATCH 0
#endif

#if MD_OPCODE_PROFILING
#define M68K_OPCODE_PROFILE_SLOTS 2048u
#define M68K_OPCODE_PROFILE_SAMPLE_SHIFT_VALUE 4u
typedef struct m68k_opcode_profile_slot
{
  uint16_t opcode;
  uint16_t used;
  uint32_t count;
} m68k_opcode_profile_slot;

static uint64_t s_m68k_opcode_total;
static uint64_t s_m68k_opcode_sampled_total;
static uint32_t s_m68k_opcode_overflow_events;
static m68k_opcode_profile_slot s_m68k_opcode_slots[M68K_OPCODE_PROFILE_SLOTS];

static inline uint32_t m68k_opcode_profile_hash(uint16_t opcode)
{
  return ((uint32_t)opcode * 40503u) & (M68K_OPCODE_PROFILE_SLOTS - 1u);
}

static inline void m68k_opcode_profile_insert_top(m68k_opcode_profile_entry* top,
                                                  uint16_t opcode,
                                                  uint32_t count)
{
  if (!count) return;
  int insert = -1;
  for (int i = 0; i < M68K_OPCODE_PROFILE_TOP_COUNT; ++i)
  {
    if (count > top[i].count)
    {
      insert = i;
      break;
    }
  }
  if (insert < 0) return;
  for (int i = M68K_OPCODE_PROFILE_TOP_COUNT - 1; i > insert; --i)
  {
    top[i] = top[i - 1];
  }
  top[insert].opcode = opcode;
  top[insert].reserved = 0;
  top[insert].count = count;
}

static inline void m68k_opcode_profile_record(uint16_t opcode)
{
  ++s_m68k_opcode_total;
  if ((s_m68k_opcode_total & ((1ULL << M68K_OPCODE_PROFILE_SAMPLE_SHIFT_VALUE) - 1ULL)) != 0ULL)
  {
    return;
  }

  ++s_m68k_opcode_sampled_total;
  const uint32_t base = m68k_opcode_profile_hash(opcode);
  for (uint32_t probe = 0; probe < M68K_OPCODE_PROFILE_SLOTS; ++probe)
  {
    m68k_opcode_profile_slot* slot = &s_m68k_opcode_slots[(base + probe) & (M68K_OPCODE_PROFILE_SLOTS - 1u)];
    if (!slot->used)
    {
      slot->used = 1;
      slot->opcode = opcode;
      slot->count = 1;
      return;
    }
    if (slot->opcode == opcode)
    {
      ++slot->count;
      return;
    }
  }

  ++s_m68k_opcode_overflow_events;
}
#else
static inline void m68k_opcode_profile_record(uint16_t opcode)
{
  (void)opcode;
}
#endif

void m68k_opcode_profile_reset(void)
{
#if MD_OPCODE_PROFILING
  s_m68k_opcode_total = 0;
  s_m68k_opcode_sampled_total = 0;
  s_m68k_opcode_overflow_events = 0;
  memset(s_m68k_opcode_slots, 0, sizeof(s_m68k_opcode_slots));
#endif
}

int m68k_opcode_profile_get_snapshot(m68k_opcode_profile_snapshot *out, int reset)
{
#if MD_OPCODE_PROFILING
  const int has_data = (s_m68k_opcode_total != 0);
  if (out)
  {
    memset(out, 0, sizeof(*out));
    out->total = s_m68k_opcode_total;
    out->sampled_total = s_m68k_opcode_sampled_total;
    out->overflow_events = s_m68k_opcode_overflow_events;
    out->sample_shift = M68K_OPCODE_PROFILE_SAMPLE_SHIFT_VALUE;
    for (uint32_t i = 0; i < M68K_OPCODE_PROFILE_SLOTS; ++i)
    {
      const m68k_opcode_profile_slot* slot = &s_m68k_opcode_slots[i];
      if (!slot->used || slot->count == 0) continue;
      ++out->tracked_opcodes;
      out->top_nibble[(slot->opcode >> 12) & 0x0f] += slot->count;
      out->top_byte[(slot->opcode >> 8) & 0xff] += slot->count;
      m68k_opcode_profile_insert_top(out->top_exact, slot->opcode, slot->count);
    }
  }
  if (reset)
  {
    m68k_opcode_profile_reset();
  }
  return has_data;
#else
  (void)out;
  (void)reset;
  return 0;
#endif
}

void m68k_category_profile_reset(void)
{
#if MD_M68K_CATEGORY_PROFILING
  memset(&s_m68k_category_profile, 0, sizeof(s_m68k_category_profile));
  s_m68k_category_profile.snap.sample_shift = MD_M68K_PROFILE_SAMPLE_SHIFT;
#endif
}

int m68k_category_profile_get_snapshot(m68k_category_profile_snapshot *out, int reset)
{
#if MD_M68K_CATEGORY_PROFILING
  const int has_data = (s_m68k_category_profile.snap.instructions != 0);
  if (out)
  {
    *out = s_m68k_category_profile.snap;
    out->sample_shift = MD_M68K_PROFILE_SAMPLE_SHIFT;
  }
  if (reset)
  {
    m68k_category_profile_reset();
  }
  return has_data;
#else
  (void)out;
  (void)reset;
  return 0;
#endif
}


/* ======================================================================== */
/* =============================== CALLBACKS ============================== */
/* ======================================================================== */

/* Default callbacks used if the callback hasn't been set yet, or if the
 * callback is set to NULL
 */

#if M68K_EMULATE_INT_ACK == OPT_ON
/* Interrupt acknowledge */
static int default_int_ack_callback(int int_level)
{
  CPU_INT_LEVEL = 0;
  return M68K_INT_ACK_AUTOVECTOR;
}
#endif

#if M68K_EMULATE_RESET == OPT_ON
/* Called when a reset instruction is executed */
static void default_reset_instr_callback(void)
{
}
#endif

#if M68K_TAS_HAS_CALLBACK == OPT_ON
/* Called when a tas instruction is executed */
static int default_tas_instr_callback(void)
{
  return 1; // allow writeback
}
#endif

#if M68K_EMULATE_FC == OPT_ON
/* Called every time there's bus activity (read/write to/from memory */
static void default_set_fc_callback(unsigned int new_fc)
{
}
#endif


/* ======================================================================== */
/* ================================= API ================================== */
/* ======================================================================== */

/* Access the internals of the CPU */
unsigned int m68k_get_reg(m68k_register_t regnum)
{
  switch(regnum)
  {
    case M68K_REG_D0:  return m68ki_cpu.dar[0];
    case M68K_REG_D1:  return m68ki_cpu.dar[1];
    case M68K_REG_D2:  return m68ki_cpu.dar[2];
    case M68K_REG_D3:  return m68ki_cpu.dar[3];
    case M68K_REG_D4:  return m68ki_cpu.dar[4];
    case M68K_REG_D5:  return m68ki_cpu.dar[5];
    case M68K_REG_D6:  return m68ki_cpu.dar[6];
    case M68K_REG_D7:  return m68ki_cpu.dar[7];
    case M68K_REG_A0:  return m68ki_cpu.dar[8];
    case M68K_REG_A1:  return m68ki_cpu.dar[9];
    case M68K_REG_A2:  return m68ki_cpu.dar[10];
    case M68K_REG_A3:  return m68ki_cpu.dar[11];
    case M68K_REG_A4:  return m68ki_cpu.dar[12];
    case M68K_REG_A5:  return m68ki_cpu.dar[13];
    case M68K_REG_A6:  return m68ki_cpu.dar[14];
    case M68K_REG_A7:  return m68ki_cpu.dar[15];
    case M68K_REG_PC:  return MASK_OUT_ABOVE_32(m68ki_cpu.pc);
    case M68K_REG_SR:  return  m68ki_cpu.t1_flag        |
                  (m68ki_cpu.s_flag << 11)              |
                   m68ki_cpu.int_mask                   |
                  ((m68ki_cpu.x_flag & XFLAG_SET) >> 4) |
                  ((m68ki_cpu.n_flag & NFLAG_SET) >> 4) |
                  ((!m68ki_cpu.not_z_flag) << 2)        |
                  ((m68ki_cpu.v_flag & VFLAG_SET) >> 6) |
                  ((m68ki_cpu.c_flag & CFLAG_SET) >> 8);
    case M68K_REG_SP:  return m68ki_cpu.dar[15];
    case M68K_REG_USP:  return m68ki_cpu.s_flag ? m68ki_cpu.sp[0] : m68ki_cpu.dar[15];
    case M68K_REG_ISP:  return m68ki_cpu.s_flag ? m68ki_cpu.dar[15] : m68ki_cpu.sp[4];
#if M68K_EMULATE_PREFETCH
    case M68K_REG_PREF_ADDR:  return m68ki_cpu.pref_addr;
    case M68K_REG_PREF_DATA:  return m68ki_cpu.pref_data;
#endif
    case M68K_REG_IR:  return m68ki_cpu.ir;
    default:      return 0;
  }
}

void m68k_set_reg(m68k_register_t regnum, unsigned int value)
{
  switch(regnum)
  {
    case M68K_REG_D0:  REG_D[0] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_D1:  REG_D[1] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_D2:  REG_D[2] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_D3:  REG_D[3] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_D4:  REG_D[4] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_D5:  REG_D[5] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_D6:  REG_D[6] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_D7:  REG_D[7] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_A0:  REG_A[0] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_A1:  REG_A[1] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_A2:  REG_A[2] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_A3:  REG_A[3] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_A4:  REG_A[4] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_A5:  REG_A[5] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_A6:  REG_A[6] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_A7:  REG_A[7] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_PC:  m68ki_jump(MASK_OUT_ABOVE_32(value)); return;
    case M68K_REG_SR:  m68ki_set_sr(value); return;
    case M68K_REG_SP:  REG_SP = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_USP:  if(FLAG_S)
                REG_USP = MASK_OUT_ABOVE_32(value);
              else
                REG_SP = MASK_OUT_ABOVE_32(value);
              return;
    case M68K_REG_ISP:  if(FLAG_S)
                REG_SP = MASK_OUT_ABOVE_32(value);
              else
                REG_ISP = MASK_OUT_ABOVE_32(value);
              return;
    case M68K_REG_IR:  REG_IR = MASK_OUT_ABOVE_16(value); return;
#if M68K_EMULATE_PREFETCH
    case M68K_REG_PREF_ADDR:  CPU_PREF_ADDR = MASK_OUT_ABOVE_32(value); return;
#endif
    default:      return;
  }
}

/* Set the callbacks */
#if M68K_EMULATE_INT_ACK == OPT_ON
void m68k_set_int_ack_callback(int  (*callback)(int int_level))
{
  CALLBACK_INT_ACK = callback ? callback : default_int_ack_callback;
}
#endif

#if M68K_EMULATE_RESET == OPT_ON
void m68k_set_reset_instr_callback(void  (*callback)(void))
{
  CALLBACK_RESET_INSTR = callback ? callback : default_reset_instr_callback;
}
#endif

#if M68K_TAS_HAS_CALLBACK == OPT_ON
void m68k_set_tas_instr_callback(int  (*callback)(void))
{
  CALLBACK_TAS_INSTR = callback ? callback : default_tas_instr_callback;
}
#endif

#if M68K_EMULATE_FC == OPT_ON
void m68k_set_fc_callback(void  (*callback)(unsigned int new_fc))
{
  CALLBACK_SET_FC = callback ? callback : default_set_fc_callback;
}
#endif

#ifdef LOGERROR

extern void error(char *format, ...);
extern uint16 v_counter;
#endif

/* ASG: rewrote so that the int_level is a mask of the IPL0/IPL1/IPL2 bits */
/* KS: Modified so that IPL* bits match with mask positions in the SR
 *     and cleaned out remenants of the interrupt controller.
 */
void IRAM_ATTR m68k_update_irq(unsigned int mask)
{
  /* Update IRQ level */
  CPU_INT_LEVEL |= (mask << 8);
  
#ifdef LOGERROR
  error("[%d(%d)][%d(%d)] m68k IRQ Level = %d(0x%02x) (%x)\n", v_counter, m68k.cycles/3420, m68k.cycles, m68k.cycles%3420,CPU_INT_LEVEL>>8,FLAG_INT_MASK,m68k_get_reg(M68K_REG_PC));
#endif
}

void IRAM_ATTR m68k_set_irq(unsigned int int_level)
{
  /* Set IRQ level */
  CPU_INT_LEVEL = int_level << 8;
  
#ifdef LOGERROR
  error("[%d(%d)][%d(%d)] m68k IRQ Level = %d(0x%02x) (%x)\n", v_counter, m68k.cycles/3420, m68k.cycles, m68k.cycles%3420,CPU_INT_LEVEL>>8,FLAG_INT_MASK,m68k_get_reg(M68K_REG_PC));
#endif
}

/* IRQ latency (Fatal Rewind, Sesame's Street Counting Cafe)*/
void m68k_set_irq_delay(unsigned int int_level)
{
  /* Prevent reentrance */
  if (!irq_latency)
  {
    /* This is always triggered from MOVE instructions (VDP CTRL port write) */
    /* We just make sure this is not a MOVE.L instruction as we could be in */
    /* the middle of its execution (first memory write).                   */
    if ((REG_IR & 0xF000) != 0x2000)
    {
      /* Finish executing current instruction */
      USE_CYCLES(CYC_INSTRUCTION(REG_IR));

      /* One instruction delay before interrupt */
      irq_latency = 1;
      m68ki_trace_t1() /* auto-disable (see m68kcpu.h) */
      m68ki_use_data_space() /* auto-disable (see m68kcpu.h) */
      m68k_cat_instruction_begin(REG_PC);
      REG_IR = m68ki_read_imm_16();
      m68k_cat_instruction_opcode((uint16_t)REG_IR);
      m68k_opcode_profile_record((uint16_t)REG_IR);
      m68k_cat_handler_begin();
#if MD_HYBRID_TOPBYTE_DISPATCH && !defined(TABLES_FULL)
      m68ki_dispatch_hybrid_topbyte_dispatch((uint16_t)REG_IR);
#elif MD_TOPBYTE_COMPRESSED_DISPATCH && !defined(TABLES_FULL)
      m68ki_dispatch_topbyte_dispatch((uint16_t)REG_IR);
#else
      m68ki_instruction_jump_table[REG_IR]();
#endif
      m68k_cat_handler_end();
      m68ki_exception_if_trace() /* auto-disable (see m68kcpu.h) */
      irq_latency = 0;
    }

    /* Set IRQ level */
    CPU_INT_LEVEL = int_level << 8;
  }
  
#ifdef LOGERROR
  error("[%d(%d)][%d(%d)] m68k IRQ Level = %d(0x%02x) (%x)\n", v_counter, m68k.cycles/3420, m68k.cycles, m68k.cycles%3420,CPU_INT_LEVEL>>8,FLAG_INT_MASK,m68k_get_reg(M68K_REG_PC));
#endif

  /* Check interrupt mask to process IRQ  */
  m68ki_check_interrupts(); /* Level triggered (IRQ) */
}

void IRAM_ATTR m68k_run(unsigned int cycles)
{
  const uint32_t prof_run_start = m68k_cat_run_begin();
    //  printf("m68K_run current_cycles=%d add=%d STOP=%x\n",m68k.cycles,cycles,CPU_STOPPED);

  /* Make sure CPU is not already ahead */
  if (m68k.cycles >= cycles)
  {
    m68k_cat_run_early(prof_run_start);
    return;
  }

  /* Check interrupt mask to process IRQ if needed */
  m68ki_check_interrupts();

  /* Make sure we're not stopped */
  if (CPU_STOPPED)
  {
    m68k.cycles = cycles;
    m68k_cat_run_stopped(prof_run_start);
    return;
  }

  /* Save end cycles count for when CPU is stopped */
  m68k.cycle_end = cycles;

  /* Return point for when we have an address error (TODO: use goto) */
  m68ki_set_address_error_trap() /* auto-disable (see m68kcpu.h) */

#ifdef LOGERROR
  error("[%d][%d] m68k run to %d cycles (%x), irq mask = %x (%x)\n", v_counter, m68k.cycles, cycles, m68k.pc,FLAG_INT_MASK, CPU_INT_LEVEL);
#endif

  const uint32_t prof_loop_start = m68k_cat_run_loop_begin(prof_run_start);
  while (m68k.cycles < cycles)
  {
    /* Set tracing accodring to T1. */
    m68ki_trace_t1() /* auto-disable (see m68kcpu.h) */

    /* Set the address space for reads */
    m68ki_use_data_space() /* auto-disable (see m68kcpu.h) */

#ifdef HOOK_CPU
    /* Trigger execution hook */
    if (cpu_hook)
      cpu_hook(HOOK_M68K_E, 0, REG_PC, 0);
#endif

    /* Decode next instruction */
    m68k_cat_instruction_begin(REG_PC);
    REG_IR = m68ki_read_imm_16();
    m68k_cat_instruction_opcode((uint16_t)REG_IR);
    m68k_opcode_profile_record((uint16_t)REG_IR);

//    printf("PC=%x IR=%x CYCLES=%d \n",m68k.pc,REG_IR,CYC_INSTRUCTION(REG_IR));

    /* Execute instruction */
    m68k_cat_handler_begin();
#if MD_HYBRID_TOPBYTE_DISPATCH && !defined(TABLES_FULL)
    m68ki_dispatch_hybrid_topbyte_dispatch((uint16_t)REG_IR);
#elif MD_TOPBYTE_COMPRESSED_DISPATCH && !defined(TABLES_FULL)
    m68ki_dispatch_topbyte_dispatch((uint16_t)REG_IR);
#else
    m68ki_instruction_jump_table[REG_IR]();
#endif
    m68k_cat_handler_end();
    USE_CYCLES(CYC_INSTRUCTION(REG_IR));

    /* Trace m68k_exception, if necessary */
    m68ki_exception_if_trace(); /* auto-disable (see m68kcpu.h) */
  }
  m68k_cat_run_end(prof_run_start, prof_loop_start);
}

int m68k_cycles(void)
{
  return CYC_INSTRUCTION(REG_IR);
}

int m68k_cycles_run(void)
{
	return m68k.cycle_end - m68k.cycles;
}

int m68k_cycles_master(void)
{
	return m68k.cycles;
}

void m68k_init(void)
{
  /* Legacy table-based memory_map path is disabled in this build. */
  m68k.memory_map = NULL;
  
#ifdef BUILD_TABLES
  static uint emulation_initialized = 0;

  /* The first call to this function initializes the opcode handler jump table */
  if(!emulation_initialized)
  {
    m68ki_build_opcode_table();
    emulation_initialized = 1;
  }
#endif

#ifdef M68K_OVERCLOCK_SHIFT
  m68k.cycle_ratio = 1 << M68K_OVERCLOCK_SHIFT;
#endif

#if M68K_EMULATE_INT_ACK == OPT_ON
  m68k_set_int_ack_callback(NULL);
#endif
#if M68K_EMULATE_RESET == OPT_ON
  m68k_set_reset_instr_callback(NULL);
#endif
#if M68K_TAS_HAS_CALLBACK == OPT_ON
  m68k_set_tas_instr_callback(NULL);
#endif
#if M68K_EMULATE_FC == OPT_ON
  m68k_set_fc_callback(NULL);
#endif
}

/* Pulse the RESET line on the CPU */
void m68k_pulse_reset(void)
{
  /* Clear all stop levels */
  CPU_STOPPED = 0;
#if M68K_EMULATE_ADDRESS_ERROR
  CPU_RUN_MODE = RUN_MODE_BERR_AERR_RESET;
#endif

  /* Turn off tracing */
  FLAG_T1 = 0;
  m68ki_clear_trace()

  /* Interrupt mask to level 7 */
  FLAG_INT_MASK = 0x0700;
  CPU_INT_LEVEL = 0;
  irq_latency = 0;

  /* Go to supervisor mode */
  m68ki_set_s_flag(SFLAG_SET);

  /* Invalidate the prefetch queue */
#if M68K_EMULATE_PREFETCH
  /* Set to arbitrary number since our first fetch is from 0 */
  CPU_PREF_ADDR = 0x1000;
#endif /* M68K_EMULATE_PREFETCH */

  /* Read the initial stack pointer and program counter */
  m68ki_jump(0);
  REG_SP = m68ki_read_imm_32();
  REG_PC = m68ki_read_imm_32();
  m68ki_jump(REG_PC);

#if M68K_EMULATE_ADDRESS_ERROR
  CPU_RUN_MODE = RUN_MODE_NORMAL;
#endif

  USE_CYCLES(CYC_EXCEPTION[EXCEPTION_RESET]);
}

void m68k_pulse_halt(void)
{
  /* Pulse the HALT line on the CPU */
  CPU_STOPPED |= STOP_LEVEL_HALT;
}

void m68k_clear_halt(void)
{
  /* Clear the HALT line on the CPU */
  CPU_STOPPED &= ~STOP_LEVEL_HALT;
}

void gwenesis_m68k_save_state() {
  SaveState *state;
  state = saveGwenesisStateOpenForWrite("m68k");
  
  saveGwenesisStateSetBuffer(state, "REG_D", REG_D, sizeof(REG_D));
  saveGwenesisStateSet(state, "SR", m68ki_get_sr());
  saveGwenesisStateSet(state, "REG_PC", REG_PC);
  saveGwenesisStateSet(state, "REG_SP", REG_SP);
  saveGwenesisStateSet(state, "REG_USP", REG_USP);
  saveGwenesisStateSet(state, "REG_ISP", REG_ISP);
  saveGwenesisStateSet(state, "REG_IR", REG_IR);

  saveGwenesisStateSet(state, "m68k_cycle_end", m68k.cycle_end);
  saveGwenesisStateSet(state, "m68k_cycles", m68k.cycles);
  saveGwenesisStateSet(state, "m68k_int_level", m68k.int_level);
  saveGwenesisStateSet(state, "m68k_stopped", m68k.stopped);
}

void gwenesis_m68k_load_state() {
  SaveState *state = saveGwenesisStateOpenForRead("m68k");
  saveGwenesisStateGetBuffer(state, "REG_D", REG_D, sizeof(REG_D));

  m68ki_set_sr(saveGwenesisStateGet(state, "SR"));
  REG_PC = saveGwenesisStateGet(state, "REG_PC");
  REG_SP = saveGwenesisStateGet(state, "REG_SP");
  REG_USP = saveGwenesisStateGet(state, "REG_USP");
  REG_ISP = saveGwenesisStateGet(state, "REG_ISP");
  REG_IR = saveGwenesisStateGet(state, "REG_IR");

  m68k.cycle_end = saveGwenesisStateGet(state, "m68k_cycle_end");
  m68k.cycles = saveGwenesisStateGet(state, "m68k_cycles");
  m68k.int_level = saveGwenesisStateGet(state, "m68k_int_level");
  m68k.stopped = saveGwenesisStateGet(state, "m68k_stopped");

}

/* ======================================================================== */
/* ============================== END OF FILE ============================= */
/* ======================================================================== */
