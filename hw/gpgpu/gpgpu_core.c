/*
 * QEMU GPGPU - RISC-V SIMT Core Implementation
 *
 * Copyright (c) 2024-2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "gpgpu.h"
#include "gpgpu_core.h"

static int32_t sext(uint32_t val, unsigned bits)
{
    return (int32_t)(val << (32 - bits)) >> (32 - bits);
}

static uint32_t float_to_bits(float f)
{
    uint32_t bits;

    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

static float bits_to_float(uint32_t bits)
{
    float f;

    memcpy(&f, &bits, sizeof(f));
    return f;
}

static float saturate_float(float f, float max)
{
    if (f > max) {
        return max;
    }
    if (f < -max) {
        return -max;
    }
    return f;
}

static uint32_t bf16_round_to_f32_bits(uint32_t bits)
{
    uint32_t lsb = (bits >> 16) & 1;

    return (bits + 0x7fff + lsb) & 0xffff0000U;
}

static bool vram_check(GPGPUState *s, uint32_t addr, unsigned size)
{
    if (addr > s->vram_size || size > s->vram_size - addr) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gpgpu-core: vram access out of bounds addr=0x%"
                      PRIx32 " size=%u\n",
                      addr, size);
        s->error_status |= GPGPU_ERR_VRAM_FAULT;
        s->global_status |= GPGPU_STATUS_ERROR;
        return false;
    }

    return true;
}

static bool vram_ld32(GPGPUState *s, uint32_t addr, uint32_t *val)
{
    if (!vram_check(s, addr, sizeof(*val))) {
        return false;
    }

    *val = ldl_le_p(s->vram_ptr + addr);
    return true;
}

static bool vram_st32(GPGPUState *s, uint32_t addr, uint32_t val)
{
    if (!vram_check(s, addr, sizeof(val))) {
        return false;
    }

    stl_le_p(s->vram_ptr + addr, val);
    return true;
}

static bool exec_float_insn(GPGPULane *lane, uint32_t insn)
{
    uint32_t rd = (insn >> 7) & 0x1f;
    uint32_t rm = (insn >> 12) & 0x7;
    uint32_t rs1 = (insn >> 15) & 0x1f;
    uint32_t rs2 = (insn >> 20) & 0x1f;
    uint32_t funct7 = (insn >> 25) & 0x7f;
    float a = bits_to_float(lane->fpr[rs1]);
    float b = bits_to_float(lane->fpr[rs2]);

    switch (funct7) {
    case 0x00: /* fadd.s */
        lane->fpr[rd] = float_to_bits(a + b);
        return true;

    case 0x08: /* fmul.s */
        lane->fpr[rd] = float_to_bits(a * b);
        return true;

    case 0x22:
        if (rs2 == 1) { /* fcvt.bf16.s */
            lane->fpr[rd] = bf16_round_to_f32_bits(lane->fpr[rs1]);
            return true;
        }
        if (rs2 == 0) { /* fcvt.s.bf16 */
            lane->fpr[rd] = lane->fpr[rs1];
            return true;
        }
        break;

    case 0x24:
        if (rs2 == 1) { /* fcvt.e4m3.s */
            lane->fpr[rd] = float_to_bits(saturate_float(a, 448.0f));
            return true;
        }
        if (rs2 == 0) { /* fcvt.s.e4m3 */
            lane->fpr[rd] = lane->fpr[rs1];
            return true;
        }
        if (rs2 == 3) { /* fcvt.e5m2.s */
            lane->fpr[rd] = float_to_bits(saturate_float(a, 57344.0f));
            return true;
        }
        if (rs2 == 2) { /* fcvt.s.e5m2 */
            lane->fpr[rd] = lane->fpr[rs1];
            return true;
        }
        break;

    case 0x26:
        if (rs2 == 1) { /* fcvt.e2m1.s */
            lane->fpr[rd] = float_to_bits(saturate_float(a, 6.0f));
            return true;
        }
        if (rs2 == 0) { /* fcvt.s.e2m1 */
            lane->fpr[rd] = lane->fpr[rs1];
            return true;
        }
        break;

    case 0x60: /* fcvt.w.s */
        if (rm == 1) {
            lane->gpr[rd] = (uint32_t)(int32_t)a;
            return true;
        }
        break;

    case 0x68: /* fcvt.s.w */
        if (rs2 == 0) {
            lane->fpr[rd] = float_to_bits((float)(int32_t)lane->gpr[rs1]);
            return true;
        }
        break;

    case 0x78: /* fmv.w.x */
        if (rs2 == 0) {
            lane->fpr[rd] = lane->gpr[rs1];
            return true;
        }
        break;
    }

    return false;
}

/* TODO: Implement warp initialization */
void gpgpu_core_init_warp(GPGPUWarp *warp, uint32_t pc,
                          uint32_t thread_id_base, const uint32_t block_id[3],
                          uint32_t num_threads,
                          uint32_t warp_id, uint32_t block_id_linear)
{
    uint32_t active_threads = MIN(num_threads, GPGPU_WARP_SIZE);

    memset(warp, 0, sizeof(*warp));
    warp->thread_id_base = thread_id_base;
    warp->warp_id = warp_id;
    memcpy(warp->block_id, block_id, sizeof(warp->block_id));

    for (uint32_t i = 0; i < active_threads; i++) {
        GPGPULane *lane = &warp->lanes[i];
        uint32_t thread_id = thread_id_base + i;

        lane->pc = pc;
        lane->mhartid = MHARTID_ENCODE(block_id_linear, warp_id, thread_id);
        lane->active = true;
        warp->active_mask |= 1U << i;
    }
}

/* TODO: Implement warp execution (RV32I + RV32F interpreter) */
int gpgpu_core_exec_warp(GPGPUState *s, GPGPUWarp *warp, uint32_t max_cycles)
{
    for (uint32_t lane_idx = 0; lane_idx < GPGPU_WARP_SIZE; lane_idx++) {
        GPGPULane *lane = &warp->lanes[lane_idx];
        uint32_t cycles = 0;

        while (lane->active) {
            uint32_t insn;
            uint32_t opcode;
            uint32_t rd;
            uint32_t funct3;
            uint32_t rs1;
            uint32_t rs2;
            uint32_t funct7;
            uint32_t next_pc = lane->pc + 4;

            if (cycles++ >= max_cycles) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "gpgpu-core: warp execution timeout pc=0x%"
                              PRIx32 "\n",
                              lane->pc);
                return -1;
            }

            if (!vram_ld32(s, lane->pc, &insn)) {
                return -1;
            }

            opcode = insn & 0x7f;
            rd = (insn >> 7) & 0x1f;
            funct3 = (insn >> 12) & 0x7;
            rs1 = (insn >> 15) & 0x1f;
            rs2 = (insn >> 20) & 0x1f;
            funct7 = (insn >> 25) & 0x7f;

            switch (opcode) {
            case 0x13: /* OP-IMM */
                switch (funct3) {
                case 0x0: /* addi */
                    lane->gpr[rd] = lane->gpr[rs1] + sext(insn >> 20, 12);
                    break;
                case 0x1: /* slli */
                    if (funct7 != 0) {
                        goto illegal;
                    }
                    lane->gpr[rd] = lane->gpr[rs1] << rs2;
                    break;
                case 0x7: /* andi */
                    lane->gpr[rd] = lane->gpr[rs1] & sext(insn >> 20, 12);
                    break;
                default:
                    goto illegal;
                }
                break;

            case 0x33: /* OP */
                if (funct3 == 0x0 && funct7 == 0x00) {
                    lane->gpr[rd] = lane->gpr[rs1] + lane->gpr[rs2];
                } else {
                    goto illegal;
                }
                break;

            case 0x37: /* lui */
                lane->gpr[rd] = insn & 0xfffff000U;
                break;

            case 0x03: /* LOAD */
                if (funct3 == 0x2) {
                    uint32_t addr = lane->gpr[rs1] + sext(insn >> 20, 12);

                    if (!vram_ld32(s, addr, &lane->gpr[rd])) {
                        return -1;
                    }
                } else {
                    goto illegal;
                }
                break;

            case 0x23: /* STORE */
                if (funct3 == 0x2) {
                    uint32_t imm = ((insn >> 7) & 0x1f) |
                                   (((insn >> 25) & 0x7f) << 5);
                    uint32_t addr = lane->gpr[rs1] + sext(imm, 12);

                    if (!vram_st32(s, addr, lane->gpr[rs2])) {
                        return -1;
                    }
                } else {
                    goto illegal;
                }
                break;

            case 0x53: /* OP-FP */
                if (!exec_float_insn(lane, insn)) {
                    goto illegal;
                }
                break;

            case 0x73: /* SYSTEM */
                if (insn == 0x00100073) {
                    lane->active = false;
                    warp->active_mask &= ~(1U << lane_idx);
                    break;
                }
                if (funct3 == 0x2 && ((insn >> 20) & 0xfff) == CSR_MHARTID) {
                    lane->gpr[rd] = lane->mhartid;
                    break;
                }
                goto illegal;

            default:
                goto illegal;
            }

            lane->gpr[0] = 0;
            lane->pc = next_pc;
            continue;

illegal:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "gpgpu-core: illegal instruction 0x%" PRIx32
                          " pc=0x%" PRIx32 "\n",
                          insn, lane->pc);
            return -1;
        }
    }

    return 0;
}

/* TODO: Implement kernel dispatch and execution */
int gpgpu_core_exec_kernel(GPGPUState *s)
{
    uint32_t grid_x = MAX(s->kernel.grid_dim[0], 1U);
    uint32_t grid_y = MAX(s->kernel.grid_dim[1], 1U);
    uint32_t grid_z = MAX(s->kernel.grid_dim[2], 1U);
    uint32_t block_x = MAX(s->kernel.block_dim[0], 1U);
    uint32_t block_y = MAX(s->kernel.block_dim[1], 1U);
    uint32_t block_z = MAX(s->kernel.block_dim[2], 1U);
    uint64_t threads_per_block = (uint64_t)block_x * block_y * block_z;
    uint64_t blocks = (uint64_t)grid_x * grid_y * grid_z;

    if (threads_per_block == 0 || threads_per_block > UINT32_MAX ||
        blocks > UINT32_MAX || s->kernel.kernel_addr > UINT32_MAX) {
        return -1;
    }

    for (uint32_t bz = 0; bz < grid_z; bz++) {
        for (uint32_t by = 0; by < grid_y; by++) {
            for (uint32_t bx = 0; bx < grid_x; bx++) {
                uint32_t block_id[3] = { bx, by, bz };
                uint32_t block_linear = (bz * grid_y + by) * grid_x + bx;

                for (uint32_t base = 0; base < threads_per_block;
                     base += GPGPU_WARP_SIZE) {
                    GPGPUWarp warp;
                    uint32_t remaining = threads_per_block - base;
                    uint32_t warp_id = base / GPGPU_WARP_SIZE;

                    gpgpu_core_init_warp(&warp, s->kernel.kernel_addr, base,
                                         block_id, remaining, warp_id,
                                         block_linear);
                    if (gpgpu_core_exec_warp(s, &warp, 100000) < 0) {
                        return -1;
                    }
                }
            }
        }
    }

    return 0;
}
