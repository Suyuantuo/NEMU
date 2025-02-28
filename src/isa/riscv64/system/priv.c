/***************************************************************************************
* Copyright (c) 2014-2021 Zihao Yu, Nanjing University
* Copyright (c) 2020-2022 Institute of Computing Technology, Chinese Academy of Sciences
*
* NEMU is licensed under Mulan PSL v2.
* You can use this software according to the terms and conditions of the Mulan PSL v2.
* You may obtain a copy of Mulan PSL v2 at:
*          http://license.coscl.org.cn/MulanPSL2
*
* THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
* EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
* MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
*
* See the Mulan PSL v2 for more details.
***************************************************************************************/

#include "../local-include/csr.h"
#include "../local-include/rtl.h"
#include "../local-include/intr.h"
#include "../local-include/trigger.h"
#include <cpu/cpu.h>
#include <cpu/difftest.h>
#include <memory/paddr.h>
#include <stdlib.h>

int update_mmu_state();
uint64_t clint_uptime();
void fp_set_dirty();
void fp_update_rm_cache(uint32_t rm);
void vp_set_dirty();

uint64_t get_abs_instr_count();

rtlreg_t csr_array[4096] = {};

#define CSRS_DEF(name, addr) \
  concat(name, _t)* const name = (concat(name, _t) *)&csr_array[addr];

MAP(CSRS, CSRS_DEF)

#define CSRS_EXIST(name, addr) csr_exist[addr] = 1;
static bool csr_exist[4096] = {};
void init_csr() {
  MAP(CSRS, CSRS_EXIST)
  #ifdef CONFIG_RVH
  cpu.v = 0;
  #endif
};

#ifdef CONFIG_RVSDTRIG
void init_trigger() {
  cpu.TM = (TriggerModule*) malloc(sizeof (TriggerModule));
  for (int i = 0; i < CONFIG_TRIGGER_NUM; i++)
    cpu.TM->triggers[i].tdata1.common.type = TRIG_TYPE_DISABLE;
}
#endif // CONFIG_RVSDTRIG

// check s/h/mcounteren for counters, throw exception if counter is not enabled.
static inline void csr_counter_enable_check(uint32_t addr) {
  int count_bit = 1 << (addr - 0xC00);

  // priv-mode & counter-enable -> exception-type
  // | MODE         | VU    | VS    | U     | S/HS  | M     |
  // | ~mcounteren  | EX_II | EX_II | EX_II | EX_II | OK    |
  // | ~hcounteren  | EX_VI | EX_VI | OK    | OK    | OK    |
  // | ~scounteren  | EX_VI | OK    | EX_II | OK    | OK    |

  if (cpu.mode < MODE_M && !(count_bit & mcounteren->val)) {
    Logti("Illegal CSR accessing (0x%X): the bit in mcounteren is not set", addr);
    longjmp_exception(EX_II);
  }

  #ifdef CONFIG_RVH
    if (cpu.v && !(count_bit & hcounteren->val)) {
      Logti("Illegal CSR accessing (0x%X): the bit in hcounteren is not set", addr);
      longjmp_exception(EX_VI);
    }
  #endif // CONFIG_RVH
  
  if (cpu.mode < MODE_S && !(count_bit & scounteren->val)) {
    Logti("Illegal CSR accessing (0x%X): the bit in scounteren is not set", addr);
    #ifdef CONFIG_RVH
      if (cpu.v) {
        longjmp_exception(EX_VI);
      }
    #endif // CONFIG_RVH
    longjmp_exception(EX_II);
  }
}

static inline bool csr_is_legal(uint32_t addr, bool need_write) {
  assert(addr < 4096);
  // Attempts to access a non-existent CSR raise an illegal instruction exception.
  if(!csr_exist[addr]) {
#ifdef CONFIG_PANIC_ON_UNIMP_CSR
    panic("[NEMU] unimplemented CSR 0x%x", addr);
#endif
    return false;
  }
  // Attempts to access a CSR without appropriate privilege level
  int lowest_access_priv_level = (addr & 0b11 << 8) >> 8; // addr(9,8)
#ifdef CONFIG_RVH
  int priv = cpu.mode == MODE_S ? MODE_HS : cpu.mode;
  if(priv < lowest_access_priv_level){
    if(cpu.v && lowest_access_priv_level <= MODE_HS)
      longjmp_exception(EX_VI);
    return false;
  }
#else
  if (!(cpu.mode >= lowest_access_priv_level)) {
    return false;
  }
#endif
  // or to write a read-only register also raise illegal instruction exceptions.
  if (need_write && (addr >> 10) == 0x3) {
    return false;
  }

  // Attempts to access unprivileged counters without s/h/mcounteren
  if (addr >= 0xC00 && addr <= 0xC1F) {
    csr_counter_enable_check(addr);
  }

  return true;
}

static inline word_t* csr_decode(uint32_t addr) {
  assert(addr < 4096);
  // Now we check if CSR is implemented / legal to access in csr_is_legal()
  // Assert(csr_exist[addr], "unimplemented CSR 0x%x at pc = " FMT_WORD, addr, cpu.pc);

  return &csr_array[addr];
}

// WPRI, SXL, UXL cannot be written

// base mstatus wmask
#define MSTATUS_WMASK_BASE (0x7e19aaUL) | (1UL << 63) | (3UL << 36)

// FS
#if !defined(CONFIG_FPU_NONE) || defined(CONFIG_RV_MSTATUS_FS_WRITABLE)
#define MSTATUS_WMASK_FS (0x3UL << 13)
#else
#define MSTATUS_WMASK_FS 0x0
#endif

// rvh fields of mstatus
#if defined(CONFIG_RVH)
#define MSTATUS_WMASK_RVH (3UL << 38)
#else
#define MSTATUS_WMASK_RVH 0
#endif

// rvv fields of mstatus
#if defined(CONFIG_RVV)
#define MSTATUS_WMASK_RVV (3UL << 9)
#else
#define MSTATUS_WMASK_RVV 0
#endif

// final mstatus wmask: dependent of the ISA extensions
#define MSTATUS_WMASK (MSTATUS_WMASK_BASE | MSTATUS_WMASK_FS | MSTATUS_WMASK_RVH | MSTATUS_WMASK_RVV)

// wmask of sstatus is given by masking the valid fields in sstatus
#define SSTATUS_WMASK (MSTATUS_WMASK & SSTATUS_RMASK)

// hstatus wmask
#if defined(CONFIG_RVH)
#define HSTATUS_WMASK ((1 << 22) | (1 << 21) | (1 << 20) | (1 << 18) | (0x3f << 12) | (1 << 9) | (1 << 8) | (1 << 7) | (1 << 6) | (1 << 5))
#else
#define HSTATUS_WMASK 0
#endif

#ifdef CONFIG_RV_Zicntr
  #define COUNTEREN_ZICNTR_MASK (0x7UL)
#else // CONFIG_RV_Zicntr
  #define COUNTEREN_ZICNTR_MASK (0x0)
#endif // CONFIG_RV_Zicntr

#ifdef CONFIG_RV_Zihpm
  #define COUNTEREN_ZIHPM_MASK (0xfffffff8UL)
#else // CONFIG_RV_Zihpm
  #define COUNTEREN_ZIHPM_MASK (0x0)
#endif // CONFIG_RV_Zihpm

#define COUNTEREN_MASK (COUNTEREN_ZICNTR_MASK | COUNTEREN_ZIHPM_MASK)


#ifdef CONFIG_RV_CSR_MCOUNTINHIBIT_CNTR
  #define MCOUNTINHIBIT_CNTR_MASK (0x5UL)
#else // CONFIG_RV_CSR_MCOUNTINHIBIT_CNTR
  #define MCOUNTINHIBIT_CNTR_MASK (0x0)
#endif // CONFIG_RV_CSR_MCOUNTINHIBIT_CNTR

#ifdef CONFIG_RV_CSR_MCOUNTINHIBIT_HPM
  #define MCOUNTINHIBIT_HPM_MASK (0xFFFFFFF8UL)
#else // CONFIG_RV_CSR_MCOUNTINHIBIT_HPM
  #define MCOUNTINHIBIT_HPM_MASK (0x0)
#endif // CONFIG_RV_CSR_MCOUNTINHIBIT_CNTR

#define MCOUNTINHIBIT_MASK (MCOUNTINHIBIT_CNTR_MASK | MCOUNTINHIBIT_HPM_MASK)

#ifdef CONFIG_RVH
#define MIDELEG_FORCED_MASK ((1 << 12) | (1 << 10) | (1 << 6) | (1 << 2)) // mideleg bits 2、6、10、12 are read_only one
#define MEDELEG_MASK (0xf0b7ff)
#define VSI_MASK (((1 << 12) | (1 << 10) | (1 << 6) | (1 << 2)) & hideleg->val)
#define VS_MASK ((1 << 10) | (1 << 6) | (1 << 2))
#define VSSIP (1 << 2)
#define SSIP (1 << 1)
#define HVIP_MASK ((1 << 10) | (1 << 6) | (1 << 2))
#define HS_MASK   ((1 << 12) | VS_MASK)
#define HIP_RMASK HS_MASK
#define HIP_WMASK VSSIP
#define HIE_RMASK HS_MASK
#define HIE_WMASK HS_MASK
#endif

#define MIE_MASK_BASE 0xaaa
#define MIP_MASK_BASE ((1 << 9) | (1 << 5) | (1 << 1))
#ifdef CONFIG_RVH
#define MIE_MASK_H ((1 << 2) | (1 << 6) | (1 << 10) | (1 << 12))
#define MIP_MASK_H VSSIP
#else
#define MIE_MASK_H 0
#define MIP_MASK_H 0
#endif // CONFIG_RVH

#define SIE_MASK (0x222 & mideleg->val)
#define SIP_MASK (0x222 & mideleg->val)
#define SIP_WMASK_S 0x2
#define MTIE_MASK (1 << 7)

#define FFLAGS_MASK 0x1f
#define FRM_MASK 0x07
#define FCSR_MASK 0xff
#define SATP_SV39_MASK 0xf000000000000000ULL
#define is_read(csr) (src == (void *)(csr))
#define is_write(csr) (dest == (void *)(csr))
#define mask_bitset(old, mask, new) (((old) & ~(mask)) | ((new) & (mask)))

#define is_pmpcfg(p) (p >= &(csr_array[CSR_PMPCFG_BASE]) && p < &(csr_array[CSR_PMPCFG_BASE + CSR_PMPCFG_MAX_NUM]))
#define is_pmpaddr(p) (p >= &(csr_array[CSR_PMPADDR_BASE]) && p < &(csr_array[CSR_PMPADDR_BASE + CSR_PMPADDR_MAX_NUM]))
#define is_hpmcounter(p) (p >= &(csr_array[CSR_HPMCOUNTER_BASE]) && p < &(csr_array[CSR_HPMCOUNTER_BASE + CSR_HPMCOUNTER_NUM]))
#define is_mhpmcounter(p) (p >= &(csr_array[CSR_MHPMCOUNTER_BASE]) && p < &(csr_array[CSR_MHPMCOUNTER_BASE + CSR_MHPMCOUNTER_NUM]))
#define is_mhpmevent(p) (p >= &(csr_array[CSR_MHPMEVENT_BASE]) && p < &(csr_array[CSR_MHPMEVENT_BASE + CSR_MHPMEVENT_NUM]))

// get 8-bit config of one PMP entries by index.
uint8_t pmpcfg_from_index(int idx) {
  // Nemu support up to 64 pmp entries in a XLEN=64 machine.
  int xlen = 64;
  // Configuration register of one entry is 8-bit.
  int bits_per_cfg = 8;
  // For RV64, one pmpcfg CSR contains configuration of 8 entries (64 / 8 = 8).
  int cfgs_per_csr = xlen / bits_per_cfg;
  // For RV64, only 8 even-numbered pmpcfg CSRs hold the configuration.
  int pmpcfg_csr_addr = CSR_PMPCFG_BASE + idx / cfgs_per_csr * 2;

  uint8_t *cfg_reg = (uint8_t *)&csr_array[pmpcfg_csr_addr];
  return *(cfg_reg + (idx % cfgs_per_csr));
}

word_t pmpaddr_from_index(int idx) {
  return csr_array[CSR_PMPADDR_BASE + idx];
}

word_t inline pmp_tor_mask() {
  return -((word_t)1 << (CONFIG_PMP_GRANULARITY - PMP_SHIFT));
}

static inline void update_mstatus_sd() {
  // mstatus.fs is always dirty or off in QEMU 3.1.0
  // When CONFIG_FS_CLEAN_STATE is set (such as for rocket-chip), mstatus.fs is always dirty or off.
  if ((ISDEF(CONFIG_DIFFTEST_REF_QEMU) || ISNDEF(CONFIG_FS_CLEAN_STATE)) && mstatus->fs) {
    mstatus->fs = 3;
  }
  mstatus->sd = (mstatus->fs == 3) || (mstatus->vs == 3);
}
#ifdef CONFIG_RVH
static inline void update_vsstatus_sd() {
  if (hstatus->vsxl == 1)
    vsstatus->_32.sd = (vsstatus->_32.fs == 3);
  else
    vsstatus->_64.sd = (vsstatus->_64.fs == 3);
}
#endif // CONFIG_RVH

static inline word_t get_mcycle() {
  #ifdef CONFIG_RV_CSR_MCOUNTINHIBIT_CNTR
    if (mcountinhibit->val & 0x1) {
      return mcycle->val;
    }
  #endif // CONFIG_RV_CSR_MCOUNTINHIBIT_CNTR
  return mcycle->val + get_abs_instr_count();
}

static inline word_t get_minstret() {
  #ifdef CONFIG_RV_CSR_MCOUNTINHIBIT_CNTR
    if (mcountinhibit->val & 0x4) {
      return minstret->val;
    }
  #endif // CONFIG_RV_CSR_MCOUNTINHIBIT_CNTR
  return minstret->val + get_abs_instr_count();
}

static inline word_t set_mcycle(word_t src) {
  #ifdef CONFIG_RV_CSR_MCOUNTINHIBIT_CNTR
    if (mcountinhibit->val & 0x1) {
      return src;
    }
  #endif // CONFIG_RV_CSR_MCOUNTINHIBIT_CNTR
  return src - get_abs_instr_count();
}

static inline word_t set_minstret(word_t src) {
  #ifdef CONFIG_RV_CSR_MCOUNTINHIBIT_CNTR
    if (mcountinhibit->val & 0x4) {
      return src;
    }
  #endif // CONFIG_RV_CSR_MCOUNTINHIBIT_CNTR
  return src - get_abs_instr_count();
}

static inline void update_counter_mcountinhibit(word_t old, word_t new) {
  #ifdef CONFIG_RV_CSR_MCOUNTINHIBIT_CNTR
    bool old_cy = old & 0x1;
    bool old_ir = old & 0x4;
    bool new_cy = new & 0x1;
    bool new_ir = new & 0x4;

    if (old_cy && !new_cy) { // CY: 1 -> 0
      mcycle->val = mcycle->val - get_abs_instr_count();
    }
    if (!old_cy && new_cy) { // CY: 0 -> 1
      mcycle->val = mcycle->val + get_abs_instr_count();
    }
    if (old_ir && !new_ir) { // IR: 1 -> 0
      minstret->val = minstret->val - get_abs_instr_count();
    }
    if (!old_ir && new_ir) { // IR: 0 -> 1
      minstret->val = minstret->val + get_abs_instr_count();
    }
  #endif // CONFIG_RV_CSR_MCOUNTINHIBIT_CNTR
}

static inline word_t csr_read(word_t *src) {
#ifdef CONFIG_RV_PMP_CSR
  if (is_pmpaddr(src)) {
    int idx = (src - &csr_array[CSR_PMPADDR_BASE]);
    if (idx >= CONFIG_RV_PMP_ACTIVE_NUM) {
      // CSRs of inactive pmp entries are read-only zero.
      return 0;
    }

    uint8_t cfg = pmpcfg_from_index(idx);
#ifdef CONFIG_SHARE
    if(dynamic_config.debug_difftest) {
      fprintf(stderr, "[NEMU] pmp addr read %d : 0x%016lx\n", idx,
        (cfg & PMP_A) >= PMP_NAPOT ? *src | (~pmp_tor_mask() >> 1) : *src & pmp_tor_mask());
    }
#endif // CONFIG_SHARE
    if ((cfg & PMP_A) >= PMP_NAPOT)
      return *src | (~pmp_tor_mask() >> 1);
    else
      return *src & pmp_tor_mask();
  }
  
  // No need to handle read pmpcfg specifically, because
  // - pmpcfg CSRs are all initialized to zero.
  // - writing to inactive pmpcfg CSRs is handled.
#endif // CONFIG_RV_PMP_CSR

#ifdef CONFIG_RVH
 if (cpu.v == 1) {

  if (is_read(sstatus))      { update_vsstatus_sd(); return vsstatus->val & SSTATUS_RMASK; }
  else if (is_read(sie))     { return (mie->val & VS_MASK) >> 1;}
  else if (is_read(stvec))   { return vstvec->val; }
  else if (is_read(sscratch)){ return vsscratch->val;}
  else if (is_read(sepc))    { return vsepc->val;}
  else if (is_read(scause))  { return vscause->val;}
  else if (is_read(stval))   { return vstval->val;}
  else if (is_read(sip))     { return (mip->val & VS_MASK) >> 1;}
  else if (is_read(satp))    {
    if (cpu.mode == MODE_S && hstatus->vtvm == 1) {
      longjmp_exception(EX_VI);
    }else
      return vsatp->val;
  }
}
if (is_read(mideleg))        { return mideleg->val | MIDELEG_FORCED_MASK;}
if (is_read(hideleg))        { return hideleg->val & (mideleg->val | MIDELEG_FORCED_MASK);}
if (is_read(hgeip))          { return hgeip->val & ~(0x1UL);}
if (is_read(hgeie))          { return hgeie->val & ~(0x1UL);}
if (is_read(hip))            { return mip->val & HIP_RMASK & (mideleg->val | MIDELEG_FORCED_MASK);}
if (is_read(hie))            { return mie->val & HIE_RMASK & (mideleg->val | MIDELEG_FORCED_MASK);}
if (is_read(hvip))           { return mip->val & HVIP_MASK;}
if (is_read(vsstatus))       { return vsstatus->val & SSTATUS_RMASK; }
if (is_read(vsip))           { return (mip->val & (hideleg->val & (mideleg->val | MIDELEG_FORCED_MASK)) & VS_MASK) >> 1; }
if (is_read(vsie))           { return (mie->val & (hideleg->val & (mideleg->val | MIDELEG_FORCED_MASK)) & VS_MASK) >> 1;}
#endif

  if (is_read(mstatus) || is_read(sstatus)) { update_mstatus_sd(); }

  if (is_read(sstatus))     { return mstatus->val & SSTATUS_RMASK; }
  else if (is_read(sie))    { return mie->val & SIE_MASK; }
  else if (is_read(mtvec))  { return mtvec->val & ~(0x2UL); }
  else if (is_read(stvec))  { return stvec->val & ~(0x2UL); }
  else if (is_read(sip))    {
#ifndef CONFIG_RVH
    difftest_skip_ref();
#endif
    return mip->val & SIP_MASK;
  }
#ifdef CONFIG_RVV
  else if (is_read(vcsr))   { return (vxrm->val & 0x3) << 1 | (vxsat->val & 0x1); }
  else if (is_read(vlenb))  { return VLEN >> 3; }
#endif
#ifndef CONFIG_FPU_NONE
  else if (is_read(fcsr))   {
    return fcsr->val & FCSR_MASK;
  }
  else if (is_read(fflags)) {
    return fcsr->fflags.val & FFLAGS_MASK;
  }
  else if (is_read(frm))    {
    return fcsr->frm & FRM_MASK;
  }
#endif // CONFIG_FPU_NONE
  else if (is_read(mcycle)) {
    // NEMU emulates a hart with CPI = 1.
    difftest_skip_ref();
    return get_mcycle();
  }
  else if (is_read(minstret)) {
    // The number of retired instruction should be the same between dut and ref.
    // But instruction counter of NEMU is not accurate when enabling Performance optimization.
    difftest_skip_ref();
    return get_minstret();
  }
#ifdef CONFIG_RV_Zicntr
  else if (is_read(cycle)) {
    // NEMU emulates a hart with CPI = 1.
    difftest_skip_ref();
    return get_mcycle();
  }
  #ifdef CONFIG_RV_CSR_TIME
    else if (is_read(csr_time)) {
      difftest_skip_ref();
      return clint_uptime();
    }
  #endif // CONFIG_RV_CSR_TIME
  else if (is_read(instret)) {
    // The number of retired instruction should be the same between dut and ref.
    // But instruction counter of NEMU is not accurate when enabling Performance optimization.
    difftest_skip_ref();
    return get_minstret();
  }
#endif // CONFIG_RV_Zicntr
#ifndef CONFIG_RVH
  if (is_read(mip)) { difftest_skip_ref(); }
#endif
  if (is_read(satp) && cpu.mode == MODE_S && mstatus->tvm == 1) { longjmp_exception(EX_II); }
#ifdef CONFIG_RVSDTRIG
  if (is_read(tdata1)) { return cpu.TM->triggers[tselect->val].tdata1.val ^
    (cpu.TM->triggers[tselect->val].tdata1.mcontrol.hit << 20); }
  if (is_read(tdata2)) { return cpu.TM->triggers[tselect->val].tdata2.val; }
  if (is_read(tdata3)) { return cpu.TM->triggers[tselect->val].tdata3.val; }
#endif // CONFIG_RVSDTRIG
  return *src;
}

#ifdef CONFIG_RVV
void vcsr_write(uint32_t addr,  rtlreg_t *src) {
  word_t *dest = csr_decode(addr);
  *dest = *src;
}
void vcsr_read(uint32_t addr,  rtlreg_t *dest) {
  word_t *src = csr_decode(addr);
  *dest = *src;
}
#endif // CONFIG_RVV

void disable_time_intr() {
    Log("Disabled machine time interruption\n");
    mie->val = mask_bitset(mie->val, MTIE_MASK, 0);
}

static inline void csr_write(word_t *dest, word_t src) {
  #ifdef CONFIG_RVH
  if(cpu.v == 1 && (is_write(sstatus) || is_write(sie) || is_write(stvec) || is_write(sscratch)
        || is_write(sepc) || is_write(scause) || is_write(stval) || is_write(sip)
        || is_write(satp) || is_write(stvec))){
    if (is_write(sstatus))      {
      vsstatus->val = mask_bitset(vsstatus->val, SSTATUS_WMASK, src);
      update_vsstatus_sd();
    }
    else if (is_write(sie))     { mie->val = mask_bitset(mie->val, VS_MASK, src << 1); }
    else if (is_write(stvec))   { vstvec->val = src; }
    else if (is_write(sscratch)){ vsscratch->val = src;}
    else if (is_write(sepc))    { vsepc->val = src;}
    else if (is_write(scause))  { vscause->val = src;}
    else if (is_write(stval))   { vstval->val = src;}
    else if (is_write(sip))     { mip->val = mask_bitset(mip->val, VSSIP, src << 1);}
    else if (is_write(satp))    {
      if (cpu.mode == MODE_S && hstatus->vtvm == 1) {
        longjmp_exception(EX_VI);
      }
      if ((src & SATP_SV39_MASK) >> 60 == 8 || (src & SATP_SV39_MASK) >> 60 == 0)
        vsatp->val = MASKED_SATP(src);
    }else if( is_write(stvec))  {vstvec->val = src & ~(0x2UL);}
  }else if (is_write(mideleg)){
    *dest = (src & 0x222) | MIDELEG_FORCED_MASK;
  }else if (is_write(hideleg)){
    hideleg->val = mask_bitset(hideleg->val, VS_MASK, src);
  }else if (is_write(hie)){
    mie->val = mask_bitset(mie->val, HIE_WMASK & (mideleg->val | MIDELEG_FORCED_MASK), src);
  }else if(is_write(hip)){
    mip->val = mask_bitset(mip->val, HIP_WMASK & (mideleg->val | MIDELEG_FORCED_MASK), src);
  }else if(is_write(hvip)){
    mip->val = mask_bitset(mip->val, HVIP_MASK, src);
  }else if(is_write(hstatus)){
    hstatus->val = mask_bitset(hstatus->val, HSTATUS_WMASK, src);
  }else if(is_write(vsstatus)){
    vsstatus->val = mask_bitset(vsstatus->val, SSTATUS_WMASK, src);
  }else if(is_write(vsie)){
    mie->val = mask_bitset(mie->val, VS_MASK & (hideleg->val & (mideleg->val | MIDELEG_FORCED_MASK)), src << 1);
  }else if(is_write(vsip)){
    mip->val = mask_bitset(mip->val, VSSIP & (hideleg->val & (mideleg->val | MIDELEG_FORCED_MASK)), src << 1);
  }else if(is_write(vstvec)){
    vstvec->val = src;
  }else if(is_write(vsscratch)){
    vsscratch->val = src;
  }else if(is_write(vsepc)){
    vsepc->val = src;
  }else if(is_write(vscause)){
    vscause->val = src;
  }else if(is_write(vstval)){
    vstval->val = src;
  }else if(is_write(vsatp)){
    if (cpu.mode == MODE_S && hstatus->vtvm == 1) {
      longjmp_exception(EX_VI);
    }
    if ((src & SATP_SV39_MASK) >> 60 == 8 || (src & SATP_SV39_MASK) >> 60 == 0)
      vsatp->val = MASKED_SATP(src);
  }else if (is_write(mstatus)) { mstatus->val = mask_bitset(mstatus->val, MSTATUS_WMASK, src); }
#else
  if (is_write(mstatus)) {
#ifndef CONFIG_RVH
    unsigned prev_mpp = mstatus->mpp;
#endif // CONFIG_RVH
    mstatus->val = mask_bitset(mstatus->val, MSTATUS_WMASK, src);
#ifndef CONFIG_RVH
    // Need to do an extra check for mstatus.MPP:
    // xPP fields are WARL fields that can hold only privilege mode x
    // and any implemented privilege mode lower than x.
    // M-mode software can determine whether a privilege mode is implemented
    // by writing that mode to MPP then reading it back. If the machine
    // provides only U and M modes, then only a single hardware storage bit
    // is required to represent either 00 or 11 in MPP.
    if (mstatus->mpp == MODE_HS) {
      // MODE_H is not implemented. The write will not take effect.
      mstatus->mpp = prev_mpp;
    }
#endif // CONFIG_RVH
  }
#endif // CONFIG_RVH
#ifdef CONFIG_RVH
  else if(is_write(hcounteren)){
    hcounteren->val = mask_bitset(hcounteren->val, COUNTEREN_MASK, src);
  }
#endif // CONFIG_RVH
  else if(is_write(scounteren)){
    scounteren->val = mask_bitset(scounteren->val, COUNTEREN_MASK, src);
  }
  else if(is_write(mcounteren)){
    mcounteren->val = mask_bitset(mcounteren->val, COUNTEREN_MASK, src);
  }
#ifdef CONFIG_RV_CSR_MCOUNTINHIBIT
  else if (is_write(mcountinhibit)) {
    update_counter_mcountinhibit(mcountinhibit->val, src & MCOUNTINHIBIT_MASK);
    mcountinhibit->val = mask_bitset(mcountinhibit->val, MCOUNTINHIBIT_MASK, src);
  }
#endif // CONFIG_RV_CSR_MCOUNTINHIBIT
  else if (is_write(mcycle)) {
    mcycle->val = set_mcycle(src);
  }
  else if (is_write(minstret)) {
    minstret->val = set_minstret(src);
  }
  else if (is_write(sstatus)) { mstatus->val = mask_bitset(mstatus->val, SSTATUS_WMASK, src); }
  else if (is_write(sie)) { mie->val = mask_bitset(mie->val, SIE_MASK, src); }
  else if (is_write(mie)) {
    mie->val = mask_bitset(mie->val, MIE_MASK_BASE | MIE_MASK_H, src);
  }
  else if (is_write(mip)) {
    mip->val = mask_bitset(mip->val, MIP_MASK_BASE | MIP_MASK_H, src);
  }
  else if (is_write(sip)) { mip->val = mask_bitset(mip->val, ((cpu.mode == MODE_S) ? SIP_WMASK_S : SIP_MASK), src); }
  else if (is_write(mtvec)) {
#ifdef CONFIG_XTVEC_VECTORED_MODE
    *dest = src & ~(0x2UL);
#else
    *dest = src & ~(0x3UL);
#endif // CONFIG_XTVEC_VECTORED_MODE
}
  else if (is_write(stvec)) {
#ifdef CONFIG_XTVEC_VECTORED_MODE
    *dest = src & ~(0x2UL);
#else
    *dest = src & ~(0x3UL);
#endif // CONFIG_XTVEC_VECTORED_MODE
}
#ifdef CONFIG_RVH
  else if (is_write(medeleg)) { medeleg->val = mask_bitset(medeleg->val, MEDELEG_MASK, src); }
#else
  else if (is_write(medeleg)) { *dest = src & 0xb3ff; }
#endif
  else if (is_write(mideleg)) { *dest = src & 0x222; }
#ifdef CONFIG_RVV
  else if (is_write(vcsr)) { *dest = src & 0b111; vxrm->val = (src >> 1) & 0b11; vxsat->val = src & 0b1; }
  else if (is_write(vxrm)) { *dest = src & 0b11; vcsr->val = (vxrm->val) << 1 | vxsat->val; }
  else if (is_write(vxsat)) { *dest = src & 0b1; vcsr->val = (vxrm->val) << 1 | vxsat->val; }
#endif
#ifdef CONFIG_MISA_UNCHANGEABLE
  else if (is_write(misa)) { /* do nothing */ }
#endif
  else if (is_write(mepc)) { *dest = src & (~0x1UL); }
  else if (is_write(sepc)) { *dest = src & (~0x1UL); }
#ifndef CONFIG_FPU_NONE
  else if (is_write(fflags)) {
    *dest = src & FFLAGS_MASK;
    fcsr->val = (frm->val)<<5 | fflags->val;
    // fcsr->fflags.val = src;
  }
  else if (is_write(frm)) {
    *dest = src & FRM_MASK;
    fcsr->val = (frm->val)<<5 | fflags->val;
    // fcsr->frm = src;
  }
  else if (is_write(fcsr)) {
    *dest = src & FCSR_MASK;
    fflags->val = src & FFLAGS_MASK;
    frm->val = ((src)>>5) & FRM_MASK;
    // *dest = src & FCSR_MASK;
  }
#endif // CONFIG_FPU_NONE
#ifdef CONFIG_RV_PMP_CSR
  else if (is_pmpaddr(dest)) {
    Logtr("Writing pmp addr");
    
    int idx = dest - &csr_array[CSR_PMPADDR_BASE];
    if (idx >= CONFIG_RV_PMP_ACTIVE_NUM) {
      // CSRs of inactive pmp entries are read-only zero.
      return;
    }

    word_t cfg = pmpcfg_from_index(idx);
    bool locked = cfg & PMP_L;
    // Note that the last pmp cfg do not have next_locked or next_tor
    bool next_locked = idx < (CONFIG_RV_PMP_ACTIVE_NUM - 1) && (pmpcfg_from_index(idx+1) & PMP_L);
    bool next_tor = idx < (CONFIG_RV_PMP_ACTIVE_NUM - 1) && (pmpcfg_from_index(idx+1) & PMP_A) == PMP_TOR;
    if (idx < CONFIG_RV_PMP_ACTIVE_NUM && !locked && !(next_locked && next_tor)) {
      *dest = src & (((word_t)1 << (CONFIG_PADDRBITS - PMP_SHIFT)) - 1);
    }
#ifdef CONFIG_SHARE
    if(dynamic_config.debug_difftest) {
      fprintf(stderr, "[NEMU] write pmp addr%d to %016lx\n",idx, *dest);
    }
#endif // CONFIG_SHARE

    mmu_tlb_flush(0);
  }
  else if (is_pmpcfg(dest)) {
    // Logtr("Writing pmp config");

    int idx_base = (dest - &csr_array[CSR_PMPCFG_BASE]) * 4;

    int xlen = 64;
    word_t cfg_data = 0;
    for (int i = 0; i < xlen / 8; i ++ ) {
      if (idx_base + i >= CONFIG_RV_PMP_ACTIVE_NUM) {
        // CSRs of inactive pmp entries are read-only zero.
        break;
      }

#ifndef CONFIG_PMPTABLE_EXTENSION
      word_t cfg = ((src >> (i*8)) & 0xff) & (PMP_R | PMP_W | PMP_X | PMP_A | PMP_L);
#endif // CONFIG_PMPTABLE_EXTENSION
#ifdef CONFIG_PMPTABLE_EXTENSION
      /*
       * Consider the T-bit and C-bit of pmptable extension,
       * cancel original pmpcfg bit limit.
       */
      word_t cfg = ((src >> (i*8)) & 0xff);
#endif // CONFIG_PMPTABLE_EXTENSION
      cfg &= ~PMP_W | ((cfg & PMP_R) ? PMP_W : 0); // Disallow R=0 W=1
      if (CONFIG_PMP_GRANULARITY != PMP_SHIFT && (cfg & PMP_A) == PMP_NA4)
        cfg |= PMP_NAPOT; // Disallow A=NA4 when granularity > 4
      cfg_data |= (cfg << (i*8));
    }
#ifdef CONFIG_SHARE
    if(dynamic_config.debug_difftest) {
      int idx = dest - &csr_array[CSR_PMPCFG_BASE];
      Logtr("[NEMU] write pmpcfg%d to %016lx\n", idx, cfg_data);
    }
#endif // CONFIG_SHARE

    *dest = cfg_data;

    mmu_tlb_flush(0);
  }
#endif // CONFIG_RV_PMP_CSR
  else if (is_write(satp)) {
    if (cpu.mode == MODE_S && mstatus->tvm == 1) {
      longjmp_exception(EX_II);
    }
    // Only support Sv39, ignore write that sets other mode
    if ((src & SATP_SV39_MASK) >> 60 == 8 || (src & SATP_SV39_MASK) >> 60 == 0)
      *dest = MASKED_SATP(src);
#ifdef CONFIG_RVSDTRIG
  } else if (is_write(tselect)) {
    *dest = src < CONFIG_TRIGGER_NUM ? src : CONFIG_TRIGGER_NUM;
  } else if (is_write(tdata1)) {
    // not write to dest
    tdata1_t* tdata1_reg = &cpu.TM->triggers[tselect->val].tdata1.common;
    tdata1_t wdata = *(tdata1_t*)&src;
    switch (wdata.type)
    {
    case TRIG_TYPE_NONE: // write type 0 to disable this trigger
    case TRIG_TYPE_DISABLE:
      tdata1_reg->type = TRIG_TYPE_DISABLE;
      tdata1_reg->data = 0;
      break;
    case TRIG_TYPE_MCONTROL:
      mcontrol_checked_write(&cpu.TM->triggers[tselect->val].tdata1.mcontrol, &src, cpu.TM);
      tm_update_timings(cpu.TM);
      break;
    default:
      // do nothing for not supported trigger type
      break;
    }
  } else if (is_write(tdata2)) {
    // not write to dest
    tdata2_t* tdata2_reg = &cpu.TM->triggers[tselect->val].tdata2;
    tdata2_t wdata = *(tdata2_t*)&src;
    tdata2_reg->val = wdata.val;
#endif // CONFIG_RVSDTRIG
  }
#ifdef CONFIG_RVH
  else if (is_write(hgatp)) {
    // Only support Sv39, ignore write that sets other mode
    if ((src & SATP_SV39_MASK) >> 60 == 8 || (src & SATP_SV39_MASK) >> 60 == 0)
      hgatp->val = MASKED_HGATP(src);
  }
#endif// CONFIG_RVH
  else if (is_mhpmcounter(dest) || is_mhpmevent(dest)) {
    // read-only zero in NEMU
    return;
  }
  else { *dest = src; }

  bool need_update_mstatus_sd = false;
#ifndef CONFIG_FPU_NONE
  if (is_write(fflags) || is_write(frm) || is_write(fcsr)) {
    fp_set_dirty();
    fp_update_rm_cache(fcsr->frm);
    need_update_mstatus_sd = true;
  }
#endif // CONFIG_FPU_NONE
#ifdef CONFIG_RVV
  if (is_write(vcsr) || is_write(vstart) || is_write(vxsat) || is_write(vxrm)) {
    vp_set_dirty();
    need_update_mstatus_sd = true;
  }
#endif //CONFIG_RVV
  if (is_write(sstatus) || is_write(mstatus) || need_update_mstatus_sd) {
    update_mstatus_sd();
  }
#ifdef CONFIG_RVH
  if (is_write(mstatus) || is_write(satp) || is_write(vsatp) || is_write(hgatp)) { update_mmu_state(); }
  if (is_write(hstatus)) {
    set_sys_state_flag(SYS_STATE_FLUSH_TCACHE); // maybe change virtualization mode
  }
  if (is_write(vsstatus)){
    update_vsstatus_sd();
  }
#else
  if (is_write(mstatus) || is_write(satp)) { update_mmu_state(); }
#endif
  if (is_write(satp)) { mmu_tlb_flush(0); } // when satp is changed(asid | ppn), flush tlb.
  if (is_write(mstatus) || is_write(sstatus) || is_write(satp) ||
      is_write(mie) || is_write(sie) || is_write(mip) || is_write(sip)) {
    set_sys_state_flag(SYS_STATE_UPDATE);
  }
}

word_t csrid_read(uint32_t csrid) {
  return csr_read(csr_decode(csrid));
}

static void csrrw(rtlreg_t *dest, const rtlreg_t *src, uint32_t csrid) {
  if (!csr_is_legal(csrid, src != NULL)) {
    Logti("Illegal csr id %u", csrid);
    longjmp_exception(EX_II);
    return;
  }
  word_t *csr = csr_decode(csrid);
  // Log("Decoding csr id %u to %p", csrid, csr);
  word_t tmp = (src != NULL ? *src : 0);
  if (dest != NULL) { *dest = csr_read(csr); }
  if (src != NULL) { csr_write(csr, tmp); }
}

static word_t priv_instr(uint32_t op, const rtlreg_t *src) {
  switch (op) {
#ifndef CONFIG_MODE_USER
    case 0x102: // sret
#ifdef CONFIG_RVH
      if (cpu.v == 0){
        cpu.v = hstatus->spv;
        hstatus->spv = 0;
        set_sys_state_flag(SYS_STATE_FLUSH_TCACHE);
      }else if (cpu.v == 1){
        if((cpu.mode == MODE_S && hstatus->vtsr) || cpu.mode < MODE_S){
          longjmp_exception(EX_VI);
        }
        if (hstatus->vsxl == 1){
          cpu.mode = vsstatus->_32.spp;
          vsstatus->_32.spp = MODE_U;
          vsstatus->_32.sie = vsstatus->_32.spie;
          vsstatus->_32.spie = 1;
        }else{
          cpu.mode = vsstatus->_64.spp;
          vsstatus->_64.spp = MODE_U;
          vsstatus->_64.sie = vsstatus->_64.spie;
          vsstatus->_64.spie = 1;
        }
        // cpu.mode = vsstatus->spp;
        // vsstatus->spp = MODE_U;
        // vsstatus->sie = vsstatus->spie;
        // vsstatus->spie = 1;
        return vsepc->val;
      }
#endif // CONFIG_RVH
      if ((cpu.mode == MODE_S && mstatus->tsr) || cpu.mode < MODE_S) {
        longjmp_exception(EX_II);
      }
      mstatus->sie = mstatus->spie;
      mstatus->spie = (ISDEF(CONFIG_DIFFTEST_REF_QEMU) ? 0 // this is bug of QEMU
          : 1);
      cpu.mode = mstatus->spp;
      if (mstatus->spp != MODE_M) { mstatus->mprv = 0; }
      mstatus->spp = MODE_U;
      update_mmu_state();
      return sepc->val;
    case 0x302: // mret
      if (cpu.mode < MODE_M) {
        longjmp_exception(EX_II);
      }
      mstatus->mie = mstatus->mpie;
      mstatus->mpie = (ISDEF(CONFIG_DIFFTEST_REF_QEMU) ? 0 // this is bug of QEMU
          : 1);
      cpu.mode = mstatus->mpp;
#ifdef CONFIG_RVH
      cpu.v = mstatus->mpv;
      mstatus->mpv = 0;
      set_sys_state_flag(SYS_STATE_FLUSH_TCACHE);
#endif // CONFIG_RVH
      if (mstatus->mpp != MODE_M) { mstatus->mprv = 0; }
      mstatus->mpp = MODE_U;
      update_mmu_state();
      Loge("Executing mret to 0x%lx", mepc->val);
      return mepc->val;
      break;
#ifdef CONFIG_RV_SVINVAL
    case 0x180: // sfence.w.inval
      if (!srnctl->svinval) {
        longjmp_exception(EX_II);
      }
      break;
    case 0x181: // sfence.inval.ir
      if (!srnctl->svinval) {
        longjmp_exception(EX_II);
      }
      break;
#endif // CONFIG_RV_SVINVAL
    case 0x105: // wfi
#ifdef CONFIG_RVH
      if((cpu.v && cpu.mode == MODE_S && hstatus->vtw == 1 && mstatus->tw == 0)
          ||(cpu.v && cpu.mode == MODE_U && mstatus->tw == 0)){
        longjmp_exception(EX_VI);
      }
#endif
      if ((cpu.mode < MODE_M && mstatus->tw == 1) || (cpu.mode == MODE_U)){
        longjmp_exception(EX_II);
      } // When S-mode is implemented, then executing WFI in U-mode causes an illegal instruction exception
    break;
#endif // CONFIG_MODE_USER
    case -1: // fence.i
      set_sys_state_flag(SYS_STATE_FLUSH_TCACHE);
      break;
    default:
      switch (op >> 5) { // instr[31:25]
        case 0x09: // sfence.vma
          // Described in 3.1.6.5 Virtualization Support in mstatus Register
          // When TVM=1, attempts to read or write the satp CSR or execute an SFENCE.VMA or SINVAL.VMA instruction
          // while executing in S-mode will raise an illegal instruction exception.

#ifdef CONFIG_RVH
          if(cpu.v == 1 && cpu.mode == MODE_S && hstatus->vtvm == 1){
            longjmp_exception(EX_VI);
          }
          else if (cpu.v == 0 && (cpu.mode == MODE_U || (cpu.mode == MODE_S && mstatus->tvm == 1))) {
            longjmp_exception(EX_II);
          }
#else
          if ((cpu.mode == MODE_S && mstatus->tvm == 1) || cpu.mode == MODE_U)
            longjmp_exception(EX_II);
#endif // CONFIG_RVH
          mmu_tlb_flush(*src);
          break;
#ifdef CONFIG_RV_SVINVAL
        case 0x0b: // sinval.vma
#ifdef CONFIG_RVH
          if (!srnctl->svinval) { // srnctl contrl extension enable or not
            longjmp_exception(EX_II);
          } else if (cpu.v == 0 && cpu.mode == MODE_U) {
            longjmp_exception(EX_II);
          } else if (cpu.v == 0 && cpu.mode == MODE_S && mstatus->tvm == 1){
            longjmp_exception(EX_II);
          } else if (cpu.v == 1 && cpu.mode == MODE_U) {
            longjmp_exception(EX_VI);
          } else if (cpu.v == 1 && cpu.mode == MODE_S && hstatus->vtvm == 1) {
            longjmp_exception(EX_VI);
          }
#else
          if (!srnctl->svinval) { // srnctl contrl extension enable or not
            longjmp_exception(EX_II);
          } else if (cpu.mode == MODE_U) {
            longjmp_exception(EX_II);
          } else if (cpu.mode == MODE_S && mstatus->tvm == 1) {
            longjmp_exception(EX_II);
          }
#endif // CONFIG_RVH
          mmu_tlb_flush(*src);
          break;
#endif // CONFIG_RV_SVINVAL
#ifdef CONFIG_RVH
        case 0x11: // hfence.vvma
          if(cpu.v) longjmp_exception(EX_VI);
          if(cpu.mode == MODE_U) longjmp_exception(EX_II);
          if(!(cpu.mode == MODE_M || (cpu.mode == MODE_S && !cpu.v))) longjmp_exception(EX_II);
          mmu_tlb_flush(*src);
          break;
        case 0x31: // hfence.gvma
          if(cpu.v) longjmp_exception(EX_VI);
          if(cpu.mode == MODE_U) longjmp_exception(EX_II);
          if(!(cpu.mode == MODE_M || (cpu.mode == MODE_S && !cpu.v && mstatus->tvm == 0))) longjmp_exception(EX_II);
          mmu_tlb_flush(*src);
          break;
#ifdef CONFIG_RV_SVINVAL
        case 0x13: // hinval.vvma
          if(cpu.v) longjmp_exception(EX_VI);
          if(cpu.mode == MODE_U) longjmp_exception(EX_II);
          mmu_tlb_flush(*src);
          break;
        case 0x33: // hinval.gvma
          if(cpu.v) longjmp_exception(EX_VI);
          if(cpu.mode == MODE_U || (cpu.mode == MODE_S && !cpu.v && mstatus->tvm)) longjmp_exception(EX_II);
          mmu_tlb_flush(*src);
          break;
#endif // CONFIG_SVINVAL
#endif // CONFIG_RVH
        default:
#ifdef CONFIG_SHARE
          longjmp_exception(EX_II);
#else
          panic("Unsupported privilege operation = %d", op);
#endif // CONFIG_SHARE
      }
  }
  return 0;
}

void isa_hostcall(uint32_t id, rtlreg_t *dest, const rtlreg_t *src1,
    const rtlreg_t *src2, word_t imm) {
  word_t ret = 0;
  switch (id) {
    case HOSTCALL_CSR: csrrw(dest, src1, imm); return;
#ifdef CONFIG_MODE_USER
    case HOSTCALL_TRAP:
      Assert(imm == 0x8, "Unsupported exception = %ld", imm);
      uintptr_t host_syscall(uintptr_t id, uintptr_t arg1, uintptr_t arg2,
          uintptr_t arg3, uintptr_t arg4, uintptr_t arg5, uintptr_t arg6);
      cpu.gpr[10]._64 = host_syscall(cpu.gpr[17]._64, cpu.gpr[10]._64, cpu.gpr[11]._64,
          cpu.gpr[12]._64, cpu.gpr[13]._64, cpu.gpr[14]._64, cpu.gpr[15]._64);
      ret = *src1 + 4;
      break;
#else
    case HOSTCALL_TRAP: ret = raise_intr(imm, *src1); break;
#endif
    case HOSTCALL_PRIV: ret = priv_instr(imm, src1); break;
    default: panic("Unsupported hostcall ID = %d", id);
  }
  if (dest) *dest = ret;
}

#ifdef CONFIG_RVH
int rvh_hlvx_check(struct Decode *s, int type){
  extern bool hlvx;
  hlvx = (s->isa.instr.i.opcode6_2 == 0x1c && s->isa.instr.i.funct3 == 0x4
                  && (s->isa.instr.i.simm11_0 == 0x643 || s->isa.instr.i.simm11_0 == 0x683));
  return hlvx;
}
extern bool hld_st;
int hload(Decode *s, rtlreg_t *dest, const rtlreg_t * src1, uint32_t id){
  hld_st = true;
  if(!(cpu.mode == MODE_M || cpu.mode == MODE_S || (cpu.mode == MODE_U && hstatus->hu))){
    longjmp_exception(EX_II);
  }
  if(cpu.v) longjmp_exception(EX_VI);
  int mmu_mode = get_h_mmu_state();
  switch (id) {
    case 0x600: // hlv.b
      rtl_lms(s, dest, src1, 0, 1, mmu_mode);
      break;
    case 0x601: // hlv.bu
      rtl_lm(s, dest, src1, 0, 1, mmu_mode);
      break;
    case 0x640: // hlv.h
      rtl_lms(s, dest, src1, 0, 2, mmu_mode);
      break;
    case 0x641: // hlv.hu
      rtl_lm(s, dest, src1, 0, 2, mmu_mode);
      break;
    case 0x643: // hlvx.hu
      rtl_lm(s, dest, src1, 0, 2, mmu_mode);
      break;
    case 0x680: // hlv.w
      rtl_lms(s, dest, src1, 0, 4, mmu_mode);
      break;
    case 0x681: // hlv.wu
      rtl_lm(s, dest, src1, 0, 4, mmu_mode);
      break;
    case 0x683: // hlvx.wu
      rtl_lm(s, dest, src1, 0, 4, mmu_mode);
      break;
    case 0x6c0: // hlv.d
      rtl_lms(s, dest, src1, 0, 8, mmu_mode);
      break;
    default:
#ifdef CONFIG_SHARE
      longjmp_exception(EX_II);
#else
      panic("Unsupported hypervisor vm load store operation = %d", id);
#endif // CONFIG_SHARE
  }
  hld_st = false;
  return 0;
}

int hstore(Decode *s, rtlreg_t *dest, const rtlreg_t * src1, const rtlreg_t * src2){
  hld_st = true;
  if(!(cpu.mode == MODE_M || cpu.mode == MODE_S || (cpu.mode == MODE_U && hstatus->hu))){
    longjmp_exception(EX_II);
  }
  if(cpu.v) longjmp_exception(EX_VI);
  uint32_t op = s->isa.instr.r.funct7;
  int mmu_mode = get_h_mmu_state();
  int len;
  switch (op) {
  case 0x31: // hsv.b
    len = 1;
    break;
  case 0x33: // hsv.h
    len = 2;
    break;
  case 0x35: // hsv.w
    len = 4;
    break;
  case 0x37: // hsv.d
    len = 8;
    break;
  default:
    #ifdef CONFIG_SHARE
      longjmp_exception(EX_II);
#else
      panic("Unsupported hypervisor vm load store operation = %d", op);
#endif // CONFIG_SHARE
  }
  rtl_sm(s, src2, src1, 0, len, mmu_mode);
  hld_st = false;
  return 0;
}
#endif
