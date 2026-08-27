# ARMv7-A decoder alias audit

Scope: decoder classification and precedence only. This report does not modify
`jit/interp.c`, and it deliberately avoids semantic claims that are better
covered by the Unicorn differential corpus.

## Sources and verification method

- Arm A-profile A32/T32 Instruction Set Architecture, DDI0597:
  <https://developer.arm.com/documentation/ddi0597/latest/>
- Arm Architecture Reference Manual, Armv7-A and Armv7-R, DDI0406C.
- GNU Binutils 2.45.1 `arm-none-eabi-as` and `arm-none-eabi-objdump`, assembled
  with `.arch armv7-a`, `.fpu vfpv3`, and `.syntax unified`.
- The exact representative words/halfwords are in
  `decode_alias_vectors.json`; each one was assembled and disassembled again.

Bit patterns below are written in architectural order, bit 31 first. `c` is an
A32 condition bit, and named fields are the fields from the Arm encoding.

## Executive result

The decoder already has the right precedence for most aliases that caused the
49.9M-instruction bring-up failures. The audit found one remaining high-risk
class: the VFP dispatcher and the VLDM/VSTM block do not validate that the
coprocessor field is CP10/CP11. Valid non-VFP coprocessor instructions can
therefore enter a valid VFP handler and silently perform memory accesses.

There is also one harmless documentation error: the comment above A32 MOVW/MOVT
says MOVW aliases ADD. Its A32 data-processing opcode field is `1000` (the TST
class), while MOVT uses `1010` (the CMP class). The precedence rule is still
correct.

## Collision table

| ID | Naive family test | Instruction A | Instruction B | Discriminator | Required order | Current state |
|---|---|---|---|---|---|---|
| T32-01 | `hw1 & FE40 == E840` as all LDRD/STRD | TBB `E8D1 F002`: `1110 1000 1101 Rn / 1111 0000 0000 Rm` | LDRD `E9D2 0101`: `1110 100P U1W0 Rn / Rt Rt2 imm8` | TBB/TBH require `hw1[15:4]=E8D` and `hw2[15:5]=11110000000`; `H=hw2[4]` | TBB/TBH before dual-register transfer | Correct: TBB/TBH precede LDRD/STRD. |
| T32-02 | `hw1 & FE00 == E800` as the whole `1110100x` group | LDMIA.W `E8B2 0003` / STMIA.W `E8A2 0003` | LDRD `E9D2 0101` / STRD `E9C2 0101` | `hw1[6]=0` for multiple, `1` for dual/exclusive/table branch subdivision | Split on bit 6 before interpreting `hw2` | Correct: both current masks include bit 6. |
| T32-03 | `hw1 & FE00 == F800` as ordinary single load/store | PLD `F890 F000` | LDRB/LDRH family | `Rt=1111`; for byte/halfword loads this is a preload hint, not a PC load | Hint check before memory access/PC write | Correct for narrow loads. |
| T32-04 | `hw1 & FF00 == FA00` plus broad `hw2 & F080 == F080` | UXTB.W `FA5F F081` | SEL `FAA1 F082`, CLZ `FAB1 F081` | `op=hw1[7:4]`: extend is `{0,1,4,5}`, SEL=`A`, CLZ=`B` | Exact/whitelisted op before a generic extend handler | Correct: the extend handler has an op whitelist. |
| A32-01 | `word & 0C000000 == 00000000` as data processing | CLZ `E16F3F10`: `cccc 0001 0110 1111 Rd 1111 0001 Rm` | CMN `E1700001` and the generic opcode-`1011` path | `S=0` plus `op[3:2]=10` selects miscellaneous space; CMN requires `S=1` | BX/BLX/CLZ/MRS/MSR/BKPT/saturating misc before DP, then reject unhandled `S=0, op=10xx` | Correct. |
| A32-02 | Same generic DP test | MUL `E0020190`: `cccc 0000 00AS Rd Ra Rs 1001 Rm` | AND register-shifted-register `E0002311` | In `bits27:25=000`, `bits7:4=1001` is multiply/synchronization, not DP operand2 | Multiply/extra space before DP | Correct. |
| A32-03 | Same generic DP test | LDRH `E19020B1`: `cccc 000P UIWL Rn Rt imm4H 1SH1 imm4L` | Data-processing register forms | In `bits27:25=000`, `bit7=1 && bit4=1` enters multiply/extra load-store subdivision | Extra load/store before DP | Correct. |
| A32-04 | Same generic DP test or an overbroad miscellaneous mask | SMULBB `E1620180` | CLZ/CMN-class miscellaneous and DP | Halfword multiply has `bit7=1, bit4=0`; this project matches `word & 0F900090 == 01000080` | Halfword multiply before the miscellaneous rejection and DP | Correct. |
| A32-05 | Immediate DP based only on `bits27:26=00` | MOVW `E3012234`: `cccc 0011 0000 imm4 Rd imm12`; MOVT `E3452678`: `cccc 0011 0100 imm4 Rd imm12` | TST-class (`op=1000,S=0`) and CMP-class (`op=1010,S=0`) fallthrough | Fixed `bits27:20` are `00110000` / `00110100` | MOVW/MOVT before immediate DP | Correct; fix comment only if convenient. |
| A32-06 | `word & 0C000000 == 04000000` as single data transfer | UXTB `E6EF2071`, UBFX `E7E621D1`, SSAT `E6A721D1`, UADD8 `E6502F91` | LDR register `E7902001` | With `bits27:25=011`, `bit4=1` is media; register-offset LDR/STR requires `bit4=0` | Media before LDR/STR, and explicitly exclude `I=1 && bit4=1` from LDR/STR | Correct. |
| A32-07 | Same single-transfer test with condition ignored | PLD `F5D0F004` | LDR with `Rd=1111` | `cond=1111` is the unconditional instruction space, not AL; PLD also fixes `Rt=1111` | Decode `cond=1111` before applying ordinary conditional families | Correct. |
| VFP-01 | `hw1 & FE00 == EC00` as VLDM/VSTM/VLDR/VSTR | VMOV `EC51 0B19` / `EC41 0B19`: `1100 010L Rt2 Rt 101x 00M1 Vm` | VLDM `ECB1 8B04` / VSTM `ECA1 8B04` | VMOV requires `(hw1 & 0FE0)=0C40` and `(hw2 & 0E10)=0A10` | 64-bit VMOV core-pair before VFP memory transfer | Correct. |
| VFP-02 | `hw1 & FE00 == EC00` without checking `hw2[11:9]` | Non-VFP MRRC p1 `EC51 0102`, MCRR p1 `EC41 0102`, LDC p1 `ED93 2101`, STC p1 `ED83 2101` | VFP VMOV/VLDM/VSTM/VLDR/VSTR | VFP/Advanced-SIMD scalar encodings use CP10/CP11: `hw2[11:9]=101`, with bit 8 selecting 10/11. p1 has `hw2[11:8]=0001` | Validate `(hw2 & 0E00)==0A00` before every VFP handler; otherwise route to generic coprocessor/UNDEF | **Open, high priority. Valid p1 instructions currently reach the VFP memory block.** |
| VFP-03 | A32 `word & 0C000000 == 0C000000` as all VFP | SVC `EF000123` | EC/ED/EE coprocessor/VFP space | SVC has `bits27:24=1111`; VFP uses EC/ED/EE prefixes and CP10/CP11 | Exclude `bits27:24=1111` and validate CP10/11 before `step_vfp` | Open classification issue; currently becomes a loud UNDEF, not silent corruption. |

## Recommended decoder precedence

### A32

1. Apply the condition, but treat `cond=1111` as a separate unconditional
   space first.
2. Branch/exchange and exact miscellaneous encodings.
3. VFP only when both the coprocessor major group and CP10/CP11 match.
4. Branch, media/DSP, and other exact high-specificity families.
5. Multiply/extra load-store and halfword multiply.
6. MOVW/MOVT.
7. Generic LDR/STR and LDM/STM.
8. Generic data processing last, with the `S=0, op=10xx` rejection retained.

### T32

1. Exact control encodings and the VFP CP10/CP11 gate.
2. PLD/PLI aliases within single load/store.
3. TBB/TBH.
4. LDRD/STRD, then LDM/STM using bit 6 as a family discriminator.
5. FAxx register-data families split by the complete op field.
6. Generic family handlers only after reserved/special subspaces are excluded.

## Concrete non-invasive fix target

The following is a design recommendation, not a patch:

```c
/* All VFP encodings supported here target CP10 or CP11. */
if ((hw2 & 0x0E00u) != 0x0A00u)
    UNDEF(g, pc, (hw << 16) | hw2);
```

Place an equivalent gate before any broad VFP dispatch or at the top of
`step_vfp`. The top-of-function gate is the safer invariant because it protects
both A32 and T32 callers and every future VFP sub-handler.

## Regression policy

- Every specific encoding in `decode_alias_vectors.json` must classify as its
  `expected_family` before semantics execute.
- Every listed `forbidden_families` entry is a hard failure even if the final
  register state happens to match.
- Add one positive and one adjacent negative vector whenever a decoder mask is
  widened.
- Run classification tests before the 49.9M Unicorn differential. They should
  fail in milliseconds and localize the bad mask directly.

