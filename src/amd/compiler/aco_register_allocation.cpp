/*
 * Copyright © 2018 Valve Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Daniel Schürmann (daniel.schuermann@campus.tu-berlin.de)
 *    Bas Nieuwenhuizen (bas@basnieuwenhuizen.nl)
 *
 */

#include <algorithm>
#include <array>
#include <map>
#include <unordered_map>

#include "aco_ir.h"
#include "sid.h"
#include "util/u_math.h"

namespace aco {
namespace {

struct ra_ctx;

unsigned get_subdword_operand_stride(chip_class chip, const aco_ptr<Instruction>& instr, unsigned idx, RegClass rc);
void add_subdword_operand(ra_ctx& ctx, aco_ptr<Instruction>& instr, unsigned idx, unsigned byte, RegClass rc);
std::pair<unsigned, unsigned> get_subdword_definition_info(Program *program, const aco_ptr<Instruction>& instr, RegClass rc);
void add_subdword_definition(Program *program, aco_ptr<Instruction>& instr, unsigned idx, PhysReg reg);

struct assignment {
   PhysReg reg;
   RegClass rc;
   uint8_t assigned = 0;
   assignment() = default;
   assignment(PhysReg reg_, RegClass rc_) : reg(reg_), rc(rc_), assigned(-1) {}
};

struct phi_info {
   Instruction* phi;
   unsigned block_idx;
   std::set<Instruction*> uses;
};

struct ra_ctx {
   std::bitset<512> war_hint;
   Program* program;
   std::vector<assignment> assignments;
   std::vector<std::unordered_map<unsigned, Temp>> renames;
   std::vector<std::vector<Instruction*>> incomplete_phis;
   std::vector<bool> filled;
   std::vector<bool> sealed;
   std::unordered_map<unsigned, Temp> orig_names;
   std::unordered_map<unsigned, phi_info> phi_map;
   std::unordered_map<unsigned, unsigned> affinities;
   std::unordered_map<unsigned, Instruction*> vectors;
   std::unordered_map<unsigned, Instruction*> split_vectors;
   aco_ptr<Instruction> pseudo_dummy;
   uint16_t max_used_sgpr = 0;
   uint16_t max_used_vgpr = 0;
   uint16_t sgpr_limit;
   uint16_t vgpr_limit;
   std::bitset<64> defs_done; /* see MAX_ARGS in aco_instruction_selection_setup.cpp */

   ra_test_policy policy;

   ra_ctx(Program* program_, ra_test_policy policy_)
      : program(program_),
        assignments(program->peekAllocationId()),
        renames(program->blocks.size()),
        incomplete_phis(program->blocks.size()),
        filled(program->blocks.size()),
        sealed(program->blocks.size()),
        policy(policy_)
   {
      pseudo_dummy.reset(create_instruction<Instruction>(aco_opcode::p_parallelcopy, Format::PSEUDO, 0, 0));
      sgpr_limit = get_addr_sgpr_from_waves(program, program->min_waves);
      vgpr_limit = get_addr_sgpr_from_waves(program, program->min_waves);
   }
};

/* Iterator type for making PhysRegInterval compatible with range-based for */
struct PhysRegIterator {
   using difference_type = int;
   using value_type = unsigned;
   using reference = const unsigned&;
   using pointer = const unsigned*;
   using iterator_category = std::bidirectional_iterator_tag;

   PhysReg reg;

   PhysReg operator*() const {
      return reg;
   }

   PhysRegIterator& operator++() {
      reg.reg_b += 4;
      return *this;
   }

   PhysRegIterator& operator--() {
      reg.reg_b -= 4;
      return *this;
   }

   bool operator==(PhysRegIterator oth) const {
      return reg == oth.reg;
   }

   bool operator!=(PhysRegIterator oth) const {
      return reg != oth.reg;
   }

   bool operator<(PhysRegIterator oth) const {
      return reg < oth.reg;
   }
};

/* Half-open register interval used in "sliding window"-style for-loops */
struct PhysRegInterval {
   PhysReg lo_;
   unsigned size;

   /* Inclusive lower bound */
   PhysReg lo() const {
      return lo_;
   }

   /* Exclusive upper bound */
   PhysReg hi() const {
      return PhysReg { lo() + size };
   }

   PhysRegInterval& operator+=(uint32_t stride) {
      lo_ = PhysReg { lo_.reg() + stride };
      return *this;
   }

   bool operator!=(const PhysRegInterval& oth) const {
      return lo_ != oth.lo_ || size != oth.size;
   }

   /* Construct a half-open interval, excluding the end register */
   static PhysRegInterval from_until(PhysReg first, PhysReg end) {
      return { first, end - first };
   }

   bool contains(PhysReg reg) const {
       return lo() <= reg && reg < hi();
   }

   bool contains(const PhysRegInterval& needle) const {
       return needle.lo() >= lo() && needle.hi() <= hi();
   }

   PhysRegIterator begin() const {
      return { lo_ };
   }

   PhysRegIterator end() const {
      return { PhysReg { lo_ + size } };
   }
};

bool intersects(const PhysRegInterval& a, const PhysRegInterval& b) {
   return ((a.lo() >= b.lo() && a.lo() < b.hi()) ||
           (a.hi() > b.lo() && a.hi() <= b.hi()));
}

/* Gets the stride for full (non-subdword) registers */
uint32_t get_stride(RegClass rc) {
    if (rc.type() == RegType::vgpr) {
        return 1;
    } else {
        uint32_t size = rc.size();
        if (size == 2) {
            return 2;
        } else if (size >= 4) {
            return 4;
        } else {
            return 1;
        }
    }
}

PhysRegInterval get_reg_bounds(Program* program, RegType type) {
   if (type == RegType::vgpr) {
      return { PhysReg { 256 }, (unsigned)program->max_reg_demand.vgpr };
   } else {
      return { PhysReg { 0 }, (unsigned)program->max_reg_demand.sgpr };
   }
}

struct DefInfo {
   PhysRegInterval bounds;
   uint8_t size;
   uint8_t stride;
   RegClass rc;

   DefInfo(ra_ctx& ctx, aco_ptr<Instruction>& instr, RegClass rc_, int operand) : rc(rc_) {
      size = rc.size();
      stride = get_stride(rc);

      bounds = get_reg_bounds(ctx.program, rc.type());

      if (rc.is_subdword() && operand >= 0) {
         /* stride in bytes */
         stride = get_subdword_operand_stride(ctx.program->chip_class, instr, operand, rc);
      } else if (rc.is_subdword()) {
         std::pair<unsigned, unsigned> info = get_subdword_definition_info(ctx.program, instr, rc);
         stride = info.first;
         if (info.second > rc.bytes()) {
            rc = RegClass::get(rc.type(), info.second);
            size = rc.size();
            /* we might still be able to put the definition in the high half,
             * but that's only useful for affinities and this information isn't
             * used for them */
            stride = align(stride, info.second);
            if (!rc.is_subdword())
               stride = DIV_ROUND_UP(stride, 4);
         }
         assert(stride > 0);
      }
   }
};

class RegisterFile {
public:
   RegisterFile() {regs.fill(0);}

   std::array<uint32_t, 512> regs;
   std::map<uint32_t, std::array<uint32_t, 4>> subdword_regs;

   const uint32_t& operator [] (PhysReg index) const {
      return regs[index];
   }

   uint32_t& operator [] (PhysReg index) {
      return regs[index];
   }

   unsigned count_zero(PhysRegInterval reg_interval) {
      unsigned res = 0;
      for (PhysReg reg : reg_interval)
         res += !regs[reg];
      return res;
   }

   /* Returns true if any of the bytes in the given range are allocated or blocked */
   bool test(PhysReg start, unsigned num_bytes) {
      for (PhysReg i = start; i.reg_b < start.reg_b + num_bytes; i = PhysReg(i + 1)) {
         if (regs[i] & 0x0FFFFFFF)
            return true;
         if (regs[i] == 0xF0000000) {
            assert(subdword_regs.find(i) != subdword_regs.end());
            for (unsigned j = i.byte(); i * 4 + j < start.reg_b + num_bytes && j < 4; j++) {
               if (subdword_regs[i][j])
                  return true;
            }
         }
      }
      return false;
   }

   void block(PhysReg start, RegClass rc) {
      if (rc.is_subdword())
         fill_subdword(start, rc.bytes(), 0xFFFFFFFF);
      else
         fill(start, rc.size(), 0xFFFFFFFF);
   }

   bool is_blocked(PhysReg start) {
      if (regs[start] == 0xFFFFFFFF)
         return true;
      if (regs[start] == 0xF0000000) {
         for (unsigned i = start.byte(); i < 4; i++)
            if (subdword_regs[start][i] == 0xFFFFFFFF)
               return true;
      }
      return false;
   }

   bool is_empty_or_blocked(PhysReg start) {
      /* Empty is 0, blocked is 0xFFFFFFFF, so to check both we compare the
       * incremented value to 1 */
      if (regs[start] == 0xF0000000) {
         return subdword_regs[start][start.byte()] + 1 <= 1;
      }
      return regs[start] + 1 <= 1;
   }

   void clear(PhysReg start, RegClass rc) {
      if (rc.is_subdword())
         fill_subdword(start, rc.bytes(), 0);
      else
         fill(start, rc.size(), 0);
   }

   void fill(Operand op) {
      if (op.regClass().is_subdword())
         fill_subdword(op.physReg(), op.bytes(), op.tempId());
      else
         fill(op.physReg(), op.size(), op.tempId());
   }

   void clear(Operand op) {
      clear(op.physReg(), op.regClass());
   }

   void fill(Definition def) {
      if (def.regClass().is_subdword())
         fill_subdword(def.physReg(), def.bytes(), def.tempId());
      else
         fill(def.physReg(), def.size(), def.tempId());
   }

   void clear(Definition def) {
      clear(def.physReg(), def.regClass());
   }

   unsigned get_id(PhysReg reg) {
      return regs[reg] == 0xF0000000 ? subdword_regs[reg][reg.byte()] : regs[reg];
   }

private:
   void fill(PhysReg start, unsigned size, uint32_t val) {
      for (unsigned i = 0; i < size; i++)
         regs[start + i] = val;
   }

   void fill_subdword(PhysReg start, unsigned num_bytes, uint32_t val) {
      fill(start, DIV_ROUND_UP(num_bytes, 4), 0xF0000000);
      for (PhysReg i = start; i.reg_b < start.reg_b + num_bytes; i = PhysReg(i + 1)) {
         /* emplace or get */
         std::array<uint32_t, 4>& sub = subdword_regs.emplace(i, std::array<uint32_t, 4>{0, 0, 0, 0}).first->second;
         for (unsigned j = i.byte(); i * 4 + j < start.reg_b + num_bytes && j < 4; j++)
            sub[j] = val;

         if (sub == std::array<uint32_t, 4>{0, 0, 0, 0}) {
            subdword_regs.erase(i);
            regs[i] = 0;
         }
      }
   }
};


/* helper function for debugging */
UNUSED void print_regs(ra_ctx& ctx, bool vgprs, RegisterFile& reg_file)
{
   unsigned max = vgprs ? ctx.program->max_reg_demand.vgpr : ctx.program->max_reg_demand.sgpr;
   PhysRegInterval regs { vgprs ? PhysReg{256} : PhysReg{0}, max };
   char reg_char = vgprs ? 'v' : 's';

   /* print markers */
   printf("       ");
   for (unsigned i = 0; i < regs.size; i += 3) {
      printf("%.2u ", i);
   }
   printf("\n");

   /* print usage */
   printf("%cgprs: ", reg_char);
   unsigned free_regs = 0;
   unsigned prev = 0;
   bool char_select = false;
   for (auto reg : regs) {
      if (reg_file[reg] == 0xFFFFFFFF) {
         printf("~");
      } else if (reg_file[reg]) {
         if (reg_file[reg] != prev) {
            prev = reg_file[reg];
            char_select = !char_select;
         }
         printf(char_select ? "#" : "@");
      } else {
         free_regs++;
         printf(".");
      }
   }
   printf("\n");

   printf("%u/%u used, %u/%u free\n", max - free_regs, max, free_regs, max);

   /* print assignments */
   prev = 0;
   unsigned size = 0;
   for (auto i : regs) {
      if (reg_file[i] != prev) {
         if (prev && size > 1)
            printf("-%d]\n", i - regs.lo() - 1);
         else if (prev)
            printf("]\n");
         prev = reg_file[i];
         if (prev && prev != 0xFFFFFFFF) {
            if (ctx.orig_names.count(reg_file[i]) && ctx.orig_names[reg_file[i]].id() != reg_file[i])
               printf("%%%u (was %%%d) = %c[%d", reg_file[i], ctx.orig_names[reg_file[i]].id(), reg_char, i - regs.lo());
            else
               printf("%%%u = %c[%d", reg_file[i], reg_char, i - regs.lo());
         }
         size = 1;
      } else {
         size++;
      }
   }
   if (prev && size > 1)
      printf("-%d]\n", regs.size - 1);
   else if (prev)
      printf("]\n");
}


unsigned get_subdword_operand_stride(chip_class chip, const aco_ptr<Instruction>& instr, unsigned idx, RegClass rc)
{
   /* v_readfirstlane_b32 cannot use SDWA */
   if (instr->opcode == aco_opcode::p_as_uniform)
      return 4;
   if (instr->isPseudo() && chip >= GFX8)
      return rc.bytes() % 2 == 0 ? 2 : 1;

   if (instr->opcode == aco_opcode::v_cvt_f32_ubyte0) {
      return 1;
   } else if (can_use_SDWA(chip, instr)) {
      return rc.bytes() % 2 == 0 ? 2 : 1;
   } else if (rc.bytes() == 2 && can_use_opsel(chip, instr->opcode, idx, 1)) {
      return 2;
   } else if (instr->isVOP3P()) {
      return 2;
   }

   switch (instr->opcode) {
   case aco_opcode::ds_write_b8:
   case aco_opcode::ds_write_b16:
      return chip >= GFX8 ? 2 : 4;
   case aco_opcode::buffer_store_byte:
   case aco_opcode::buffer_store_short:
   case aco_opcode::flat_store_byte:
   case aco_opcode::flat_store_short:
   case aco_opcode::scratch_store_byte:
   case aco_opcode::scratch_store_short:
   case aco_opcode::global_store_byte:
   case aco_opcode::global_store_short:
      return chip >= GFX9 ? 2 : 4;
   default:
      break;
   }

   return 4;
}

void update_phi_map(ra_ctx& ctx, Instruction *old, Instruction *instr)
{
   for (Operand& op : instr->operands) {
      if (!op.isTemp())
         continue;
      std::unordered_map<unsigned, phi_info>::iterator phi = ctx.phi_map.find(op.tempId());
      if (phi != ctx.phi_map.end()) {
         phi->second.uses.erase(old);
         phi->second.uses.emplace(instr);
      }
   }
}

void add_subdword_operand(ra_ctx& ctx, aco_ptr<Instruction>& instr, unsigned idx, unsigned byte, RegClass rc)
{
   chip_class chip = ctx.program->chip_class;
   if (instr->isPseudo() || byte == 0)
      return;

   assert(rc.bytes() <= 2);

   if (!instr->usesModifiers() && instr->opcode == aco_opcode::v_cvt_f32_ubyte0) {
      switch (byte) {
      case 0:
         instr->opcode = aco_opcode::v_cvt_f32_ubyte0;
         break;
      case 1:
         instr->opcode = aco_opcode::v_cvt_f32_ubyte1;
         break;
      case 2:
         instr->opcode = aco_opcode::v_cvt_f32_ubyte2;
         break;
      case 3:
         instr->opcode = aco_opcode::v_cvt_f32_ubyte3;
         break;
      }
      return;
   } else if (can_use_SDWA(chip, instr)) {
      aco_ptr<Instruction> tmp = convert_to_SDWA(chip, instr);
      if (tmp)
         update_phi_map(ctx, tmp.get(), instr.get());
      return;
   } else if (rc.bytes() == 2 && can_use_opsel(chip, instr->opcode, idx, byte / 2)) {
      instr->vop3().opsel |= (byte / 2) << idx;
      return;
   } else if (instr->isVOP3P() && byte == 2) {
      VOP3P_instruction& vop3p = instr->vop3p();
      assert(!(vop3p.opsel_lo & (1 << idx)));
      vop3p.opsel_lo |= 1 << idx;
      vop3p.opsel_hi |= 1 << idx;
      return;
   }

   if (chip >= GFX8 && instr->opcode == aco_opcode::ds_write_b8 && byte == 2) {
      instr->opcode = aco_opcode::ds_write_b8_d16_hi;
      return;
   }
   if (chip >= GFX8 && instr->opcode == aco_opcode::ds_write_b16 && byte == 2) {
      instr->opcode = aco_opcode::ds_write_b16_d16_hi;
      return;
   }

   if (chip >= GFX9 && byte == 2) {
      if (instr->opcode == aco_opcode::buffer_store_byte)
         instr->opcode = aco_opcode::buffer_store_byte_d16_hi;
      else if (instr->opcode == aco_opcode::buffer_store_short)
         instr->opcode = aco_opcode::buffer_store_short_d16_hi;
      else if (instr->opcode == aco_opcode::flat_store_byte)
         instr->opcode = aco_opcode::flat_store_byte_d16_hi;
      else if (instr->opcode == aco_opcode::flat_store_short)
         instr->opcode = aco_opcode::flat_store_short_d16_hi;
      else if (instr->opcode == aco_opcode::scratch_store_byte)
         instr->opcode = aco_opcode::scratch_store_byte_d16_hi;
      else if (instr->opcode == aco_opcode::scratch_store_short)
         instr->opcode = aco_opcode::scratch_store_short_d16_hi;
      else if (instr->opcode == aco_opcode::global_store_byte)
         instr->opcode = aco_opcode::global_store_byte_d16_hi;
      else if (instr->opcode == aco_opcode::global_store_short)
         instr->opcode = aco_opcode::global_store_short_d16_hi;
      else
         unreachable("Something went wrong: Impossible register assignment.");
   }
}

/* minimum_stride, bytes_written */
std::pair<unsigned, unsigned> get_subdword_definition_info(Program *program, const aco_ptr<Instruction>& instr, RegClass rc)
{
   chip_class chip = program->chip_class;

   if (instr->isPseudo() && chip >= GFX8)
      return std::make_pair(rc.bytes() % 2 == 0 ? 2 : 1, rc.bytes());
   else if (instr->isPseudo())
      return std::make_pair(4, rc.size() * 4u);

   unsigned bytes_written = chip >= GFX10 ? rc.bytes() : 4u;
   switch (instr->opcode) {
   case aco_opcode::v_mad_f16:
   case aco_opcode::v_mad_u16:
   case aco_opcode::v_mad_i16:
   case aco_opcode::v_fma_f16:
   case aco_opcode::v_div_fixup_f16:
   case aco_opcode::v_interp_p2_f16:
      bytes_written = chip >= GFX9 ? rc.bytes() : 4u;
      break;
   default:
      break;
   }
   bytes_written = bytes_written > 4 ? align(bytes_written, 4) : bytes_written;
   bytes_written = MAX2(bytes_written, instr_info.definition_size[(int)instr->opcode] / 8u);

   if (can_use_SDWA(chip, instr)) {
      return std::make_pair(rc.bytes(), rc.bytes());
   } else if (rc.bytes() == 2 && can_use_opsel(chip, instr->opcode, -1, 1)) {
      return std::make_pair(2u, bytes_written);
   }

   switch (instr->opcode) {
   case aco_opcode::buffer_load_ubyte_d16:
   case aco_opcode::buffer_load_short_d16:
   case aco_opcode::flat_load_ubyte_d16:
   case aco_opcode::flat_load_short_d16:
   case aco_opcode::scratch_load_ubyte_d16:
   case aco_opcode::scratch_load_short_d16:
   case aco_opcode::global_load_ubyte_d16:
   case aco_opcode::global_load_short_d16:
   case aco_opcode::ds_read_u8_d16:
   case aco_opcode::ds_read_u16_d16:
      if (chip >= GFX9 && !program->dev.sram_ecc_enabled)
         return std::make_pair(2u, 2u);
      else
         return std::make_pair(2u, 4u);
   case aco_opcode::v_fma_mixlo_f16:
      return std::make_pair(2u, 2u);
   default:
      break;
   }

   return std::make_pair(4u, bytes_written);
}

void add_subdword_definition(Program *program, aco_ptr<Instruction>& instr, unsigned idx, PhysReg reg)
{
   RegClass rc = instr->definitions[idx].regClass();
   chip_class chip = program->chip_class;

   if (instr->isPseudo()) {
      return;
   } else if (can_use_SDWA(chip, instr)) {
      unsigned def_size = instr_info.definition_size[(int)instr->opcode];
      if (reg.byte() || chip < GFX10 || def_size > rc.bytes() * 8u)
         convert_to_SDWA(chip, instr);
      return;
   } else if (reg.byte() && rc.bytes() == 2 && can_use_opsel(chip, instr->opcode, -1, reg.byte() / 2)) {
      VOP3_instruction& vop3 = instr->vop3();
      if (reg.byte() == 2)
         vop3.opsel |= (1 << 3); /* dst in high half */
      return;
   }

   if (reg.byte() == 2) {
      if (instr->opcode == aco_opcode::v_fma_mixlo_f16)
         instr->opcode = aco_opcode::v_fma_mixhi_f16;
      else if (instr->opcode == aco_opcode::buffer_load_ubyte_d16)
         instr->opcode = aco_opcode::buffer_load_ubyte_d16_hi;
      else if (instr->opcode == aco_opcode::buffer_load_short_d16)
         instr->opcode = aco_opcode::buffer_load_short_d16_hi;
      else if (instr->opcode == aco_opcode::flat_load_ubyte_d16)
         instr->opcode = aco_opcode::flat_load_ubyte_d16_hi;
      else if (instr->opcode == aco_opcode::flat_load_short_d16)
         instr->opcode = aco_opcode::flat_load_short_d16_hi;
      else if (instr->opcode == aco_opcode::scratch_load_ubyte_d16)
         instr->opcode = aco_opcode::scratch_load_ubyte_d16_hi;
      else if (instr->opcode == aco_opcode::scratch_load_short_d16)
         instr->opcode = aco_opcode::scratch_load_short_d16_hi;
      else if (instr->opcode == aco_opcode::global_load_ubyte_d16)
         instr->opcode = aco_opcode::global_load_ubyte_d16_hi;
      else if (instr->opcode == aco_opcode::global_load_short_d16)
         instr->opcode = aco_opcode::global_load_short_d16_hi;
      else if (instr->opcode == aco_opcode::ds_read_u8_d16)
         instr->opcode = aco_opcode::ds_read_u8_d16_hi;
      else if (instr->opcode == aco_opcode::ds_read_u16_d16)
         instr->opcode = aco_opcode::ds_read_u16_d16_hi;
      else
         unreachable("Something went wrong: Impossible register assignment.");
   }
}

void adjust_max_used_regs(ra_ctx& ctx, RegClass rc, unsigned reg)
{
   uint16_t max_addressible_sgpr = ctx.sgpr_limit;
   unsigned size = rc.size();
   if (rc.type() == RegType::vgpr) {
      assert(reg >= 256);
      uint16_t hi = reg - 256 + size - 1;
      ctx.max_used_vgpr = std::max(ctx.max_used_vgpr, hi);
   } else if (reg + rc.size() <= max_addressible_sgpr) {
      uint16_t hi = reg + size - 1;
      ctx.max_used_sgpr = std::max(ctx.max_used_sgpr, std::min(hi, max_addressible_sgpr));
   }
}


void update_renames(ra_ctx& ctx, RegisterFile& reg_file,
                    std::vector<std::pair<Operand, Definition>>& parallelcopies,
                    aco_ptr<Instruction>& instr, bool rename_not_killed_ops)
{
   /* clear operands */
   for (std::pair<Operand, Definition>& copy : parallelcopies) {
      /* the definitions with id are not from this function and already handled */
      if (copy.second.isTemp())
         continue;
      reg_file.clear(copy.first);
   }

   /* allocate id's and rename operands: this is done transparently here */
   for (std::pair<Operand, Definition>& copy : parallelcopies) {
      /* the definitions with id are not from this function and already handled */
      if (copy.second.isTemp())
         continue;

      /* check if we we moved another parallelcopy definition */
      for (std::pair<Operand, Definition>& other : parallelcopies) {
         if (!other.second.isTemp())
            continue;
         if (copy.first.getTemp() == other.second.getTemp()) {
            copy.first.setTemp(other.first.getTemp());
            copy.first.setFixed(other.first.physReg());
         }
      }
      // FIXME: if a definition got moved, change the target location and remove the parallelcopy
      copy.second.setTemp(ctx.program->allocateTmp(copy.second.regClass()));
      ctx.assignments.emplace_back(copy.second.physReg(), copy.second.regClass());
      assert(ctx.assignments.size() == ctx.program->peekAllocationId());

      /* check if we moved an operand */
      bool first = true;
      bool fill = true;
      for (unsigned i = 0; i < instr->operands.size(); i++) {
         Operand& op = instr->operands[i];
         if (!op.isTemp())
            continue;
         if (op.tempId() == copy.first.tempId()) {
            bool omit_renaming = !rename_not_killed_ops && !op.isKillBeforeDef();
            for (std::pair<Operand, Definition>& pc : parallelcopies) {
               PhysReg def_reg = pc.second.physReg();
               omit_renaming &= def_reg > copy.first.physReg() ?
                                (copy.first.physReg() + copy.first.size() <= def_reg.reg()) :
                                (def_reg + pc.second.size() <= copy.first.physReg().reg());
            }
            if (omit_renaming) {
               if (first)
                  op.setFirstKill(true);
               else
                  op.setKill(true);
               first = false;
               continue;
            }
            op.setTemp(copy.second.getTemp());
            op.setFixed(copy.second.physReg());

            fill = !op.isKillBeforeDef();
         }
      }

      if (fill)
         reg_file.fill(copy.second);
   }
}

std::pair<PhysReg, bool> get_reg_simple(ra_ctx& ctx,
                                        RegisterFile& reg_file,
                                        DefInfo info)
{
   const PhysRegInterval& bounds = info.bounds;
   uint32_t size = info.size;
   uint32_t stride = info.rc.is_subdword() ? DIV_ROUND_UP(info.stride, 4) : info.stride;
   RegClass rc = info.rc;

   DefInfo new_info = info;
   new_info.rc = RegClass(rc.type(), size);
   for (unsigned new_stride = 16; new_stride > stride; new_stride /= 2) {
      if (size % new_stride)
         continue;
      new_info.stride = new_stride;
      std::pair<PhysReg, bool> res = get_reg_simple(ctx, reg_file, new_info);
      if (res.second)
         return res;
   }

   auto is_free = [&](PhysReg reg_index) { return reg_file[reg_index] == 0 && !ctx.war_hint[reg_index]; };

   if (stride == 1) {
      /* best fit algorithm: find the smallest gap to fit in the variable */
      PhysRegInterval best_gap { PhysReg { 0 }, UINT_MAX };
      const unsigned max_gpr = (rc.type() == RegType::vgpr) ? (256 + ctx.max_used_vgpr) : ctx.max_used_sgpr;

      PhysRegIterator reg_it = bounds.begin();
      const PhysRegIterator end_it = std::min(bounds.end(), std::max(PhysRegIterator { PhysReg { max_gpr + 1 } }, reg_it));
      while (reg_it != bounds.end()) {
         /* Find the next chunk of available register slots */
         reg_it = std::find_if(reg_it, end_it, is_free);
         auto next_nonfree_it = std::find_if_not(reg_it, end_it, is_free);
         if (reg_it == bounds.end()) {
            break;
         }

         if (next_nonfree_it == end_it) {
            /* All registers past max_used_gpr are free */
            next_nonfree_it = bounds.end();
         }

         PhysRegInterval gap = PhysRegInterval::from_until(*reg_it, *next_nonfree_it);

         /* early return on exact matches */
         if (size == gap.size) {
            adjust_max_used_regs(ctx, rc, gap.lo());
            return {gap.lo(), true};
         }

         /* check if it fits and the gap size is smaller */
         if (size < gap.size && gap.size < best_gap.size) {
            best_gap = gap;
         }

         /* Move past the processed chunk */
         reg_it = next_nonfree_it;
      }

      if (best_gap.size == UINT_MAX)
         return {{}, false};

      /* find best position within gap by leaving a good stride for other variables*/
      unsigned buffer = best_gap.size - size;
      if (buffer > 1) {
         if (((best_gap.lo() + size) % 8 != 0 && (best_gap.lo() + buffer) % 8 == 0) ||
             ((best_gap.lo() + size) % 4 != 0 && (best_gap.lo() + buffer) % 4 == 0) ||
             ((best_gap.lo() + size) % 2 != 0 && (best_gap.lo() + buffer) % 2 == 0))
            best_gap = { PhysReg { best_gap.lo() + buffer }, best_gap.size - buffer };
      }

      adjust_max_used_regs(ctx, rc, best_gap.lo());
      return {best_gap.lo(), true};
   }

   for (PhysRegInterval reg_win = { bounds.lo(), size }; reg_win.hi() <= bounds.hi(); reg_win += stride) {
      if (reg_file[reg_win.lo()] != 0) {
         continue;
      }

      bool is_valid = std::all_of(std::next(reg_win.begin()), reg_win.end(), is_free);
      if (is_valid) {
         adjust_max_used_regs(ctx, rc, reg_win.lo());
         return {reg_win.lo(), true};
      }
   }

   /* do this late because using the upper bytes of a register can require
    * larger instruction encodings or copies
    * TODO: don't do this in situations where it doesn't benefit */
   if (rc.is_subdword()) {
      for (std::pair<const uint32_t, std::array<uint32_t, 4>>& entry : reg_file.subdword_regs) {
         assert(reg_file[PhysReg{entry.first}] == 0xF0000000);
         if (!bounds.contains(PhysReg{entry.first}))
            continue;

         for (unsigned i = 0; i < 4; i+= info.stride) {
            /* check if there's a block of free bytes large enough to hold the register */
            bool reg_found = std::all_of(&entry.second[i], &entry.second[std::min(4u, i + rc.bytes())],
                                         [](unsigned v) { return v == 0; });

            /* check if also the neighboring reg is free if needed */
            if (reg_found && i + rc.bytes() > 4)
                reg_found = (reg_file[PhysReg{entry.first + 1}] == 0);

            if (reg_found) {
               PhysReg res{entry.first};
               res.reg_b += i;
               adjust_max_used_regs(ctx, rc, entry.first);
               return {res, true};
            }
         }
      }
   }

   return {{}, false};
}

/* collect variables from a register area and clear reg_file */
std::set<std::pair<unsigned, unsigned>> find_vars(ra_ctx& ctx, RegisterFile& reg_file,
                                                  const PhysRegInterval reg_interval)
{
   std::set<std::pair<unsigned, unsigned>> vars;
   for (PhysReg j : reg_interval) {
      if (reg_file.is_blocked(j))
         continue;
      if (reg_file[j] == 0xF0000000) {
         for (unsigned k = 0; k < 4; k++) {
            unsigned id = reg_file.subdword_regs[j][k];
            if (id) {
               assignment& var = ctx.assignments[id];
               vars.emplace(var.rc.bytes(), id);
            }
         }
      } else if (reg_file[j] != 0) {
         unsigned id = reg_file[j];
         assignment& var = ctx.assignments[id];
         vars.emplace(var.rc.bytes(), id);
      }
   }
   return vars;
}

/* collect variables from a register area and clear reg_file */
std::set<std::pair<unsigned, unsigned>> collect_vars(ra_ctx& ctx, RegisterFile& reg_file,
                                                     const PhysRegInterval reg_interval)
{
   std::set<std::pair<unsigned, unsigned>> vars = find_vars(ctx, reg_file, reg_interval);
   for (std::pair<unsigned, unsigned> size_id : vars) {
      assignment& var = ctx.assignments[size_id.second];
      reg_file.clear(var.reg, var.rc);
   }
   return vars;
}

bool get_regs_for_copies(ra_ctx& ctx,
                         RegisterFile& reg_file,
                         std::vector<std::pair<Operand, Definition>>& parallelcopies,
                         const std::set<std::pair<unsigned, unsigned>> &vars,
                         const PhysRegInterval bounds,
                         aco_ptr<Instruction>& instr,
                         const PhysRegInterval def_reg)
{
   /* variables are sorted from small sized to large */
   /* NOTE: variables are also sorted by ID. this only affects a very small number of shaders slightly though. */
   for (std::set<std::pair<unsigned, unsigned>>::const_reverse_iterator it = vars.rbegin(); it != vars.rend(); ++it) {
      unsigned id = it->second;
      assignment& var = ctx.assignments[id];
      DefInfo info = DefInfo(ctx, ctx.pseudo_dummy, var.rc, -1);
      uint32_t size = info.size;

      /* check if this is a dead operand, then we can re-use the space from the definition
       * also use the correct stride for sub-dword operands */
      bool is_dead_operand = false;
      for (unsigned i = 0; !is_phi(instr) && i < instr->operands.size(); i++) {
         if (instr->operands[i].isTemp() && instr->operands[i].tempId() == id) {
            if (instr->operands[i].isKillBeforeDef())
               is_dead_operand = true;
            info = DefInfo(ctx, instr, var.rc, i);
            break;
         }
      }

      std::pair<PhysReg, bool> res;
      if (is_dead_operand) {
         if (instr->opcode == aco_opcode::p_create_vector) {
            PhysReg reg(def_reg.lo());
            for (unsigned i = 0; i < instr->operands.size(); i++) {
               if (instr->operands[i].isTemp() && instr->operands[i].tempId() == id) {
                  res = {reg, (!var.rc.is_subdword() || (reg.byte() % info.stride == 0)) && !reg_file.test(reg, var.rc.bytes())};
                  break;
               }
               reg.reg_b += instr->operands[i].bytes();
            }
            if (!res.second)
               res = {var.reg, !reg_file.test(var.reg, var.rc.bytes())};
         } else {
            info.bounds = def_reg;
            res = get_reg_simple(ctx, reg_file, info);
         }
      } else {
         /* Try to find space within the bounds but outside of the definition */
         info.bounds = PhysRegInterval::from_until(bounds.lo(), MIN2(def_reg.lo(), bounds.hi()));
         res = get_reg_simple(ctx, reg_file, info);
         if (!res.second && def_reg.hi() <= bounds.hi()) {
            unsigned lo = (def_reg.hi() + info.stride - 1) & ~(info.stride - 1);
            info.bounds = PhysRegInterval::from_until(PhysReg{lo}, bounds.hi());
            res = get_reg_simple(ctx, reg_file, info);
         }
      }

      if (res.second) {
         /* mark the area as blocked */
         reg_file.block(res.first, var.rc);

         /* create parallelcopy pair (without definition id) */
         Temp tmp = Temp(id, var.rc);
         Operand pc_op = Operand(tmp);
         pc_op.setFixed(var.reg);
         Definition pc_def = Definition(res.first, pc_op.regClass());
         parallelcopies.emplace_back(pc_op, pc_def);
         continue;
      }

      PhysReg best_pos = bounds.lo();
      unsigned num_moves = 0xFF;
      unsigned num_vars = 0;

      /* we use a sliding window to find potential positions */
      unsigned stride = var.rc.is_subdword() ? 1 : info.stride;
      for (PhysRegInterval reg_win { bounds.lo(), size };
           reg_win.hi() <= bounds.hi(); reg_win += stride) {
         if (!is_dead_operand && intersects(reg_win, def_reg))
            continue;

         /* second, check that we have at most k=num_moves elements in the window
          * and no element is larger than the currently processed one */
         unsigned k = 0;
         unsigned n = 0;
         unsigned last_var = 0;
         bool found = true;
         for (PhysReg j : reg_win) {
            if (reg_file[j] == 0 || reg_file[j] == last_var)
               continue;

            if (reg_file.is_blocked(j) || k > num_moves) {
               found = false;
               break;
            }
            if (reg_file[j] == 0xF0000000) {
               k += 1;
               n++;
               continue;
            }
            /* we cannot split live ranges of linear vgprs */
            if (ctx.assignments[reg_file[j]].rc & (1 << 6)) {
               found = false;
               break;
            }
            bool is_kill = false;
            for (const Operand& op : instr->operands) {
               if (op.isTemp() && op.isKillBeforeDef() && op.tempId() == reg_file[j]) {
                  is_kill = true;
                  break;
               }
            }
            if (!is_kill && ctx.assignments[reg_file[j]].rc.size() >= size) {
               found = false;
               break;
            }

            k += ctx.assignments[reg_file[j]].rc.size();
            last_var = reg_file[j];
            n++;
            if (k > num_moves || (k == num_moves && n <= num_vars)) {
               found = false;
               break;
            }
         }

         if (found) {
            best_pos = reg_win.lo();
            num_moves = k;
            num_vars = n;
         }
      }

      /* FIXME: we messed up and couldn't find space for the variables to be copied */
      if (num_moves == 0xFF)
         return false;

      PhysRegInterval reg_win { best_pos, size };

      /* collect variables and block reg file */
      std::set<std::pair<unsigned, unsigned>> new_vars = collect_vars(ctx, reg_file, reg_win);

      /* mark the area as blocked */
      reg_file.block(reg_win.lo(), var.rc);
      adjust_max_used_regs(ctx, var.rc, reg_win.lo());

      if (!get_regs_for_copies(ctx, reg_file, parallelcopies, new_vars, bounds, instr, def_reg))
         return false;

      /* create parallelcopy pair (without definition id) */
      Temp tmp = Temp(id, var.rc);
      Operand pc_op = Operand(tmp);
      pc_op.setFixed(var.reg);
      Definition pc_def = Definition(reg_win.lo(), pc_op.regClass());
      parallelcopies.emplace_back(pc_op, pc_def);
   }

   return true;
}


std::pair<PhysReg, bool> get_reg_impl(ra_ctx& ctx,
                                      RegisterFile& reg_file,
                                      std::vector<std::pair<Operand, Definition>>& parallelcopies,
                                      const DefInfo& info,
                                      aco_ptr<Instruction>& instr)
{
   const PhysRegInterval& bounds = info.bounds;
   uint32_t size = info.size;
   uint32_t stride = info.stride;
   RegClass rc = info.rc;

   /* check how many free regs we have */
   unsigned regs_free = reg_file.count_zero(bounds);

   /* mark and count killed operands */
   unsigned killed_ops = 0;
   std::bitset<256> is_killed_operand; /* per-register */
   for (unsigned j = 0; !is_phi(instr) && j < instr->operands.size(); j++) {
      Operand& op = instr->operands[j];
      if (op.isTemp() &&
          op.isFirstKillBeforeDef() &&
          bounds.contains(op.physReg()) &&
          !reg_file.test(PhysReg{op.physReg().reg()}, align(op.bytes() + op.physReg().byte(), 4))) {
         assert(op.isFixed());

         for (unsigned i = 0; i < op.size(); ++i) {
            is_killed_operand[(op.physReg() & 0xff) + i] = true;
         }

         killed_ops += op.getTemp().size();
      }
   }

   assert(regs_free >= size);
   /* we might have to move dead operands to dst in order to make space */
   unsigned op_moves = 0;

   if (size > (regs_free - killed_ops))
      op_moves = size - (regs_free - killed_ops);

   /* find the best position to place the definition */
   PhysRegInterval best_win = { bounds.lo(), size };
   unsigned num_moves = 0xFF;
   unsigned num_vars = 0;

   /* we use a sliding window to check potential positions */
   for (PhysRegInterval reg_win = { bounds.lo(), size }; reg_win.hi() <= bounds.hi(); reg_win += stride) {
      /* first check if the register window starts in the middle of an
       * allocated variable: this is what we have to fix to allow for
       * num_moves > size */
      if (reg_win.lo() > bounds.lo() && !reg_file.is_empty_or_blocked(reg_win.lo()) &&
          reg_file.get_id(reg_win.lo()) == reg_file.get_id(reg_win.lo().advance(-1)))
         continue;
      if (reg_win.hi() < bounds.hi() && !reg_file.is_empty_or_blocked(reg_win.hi().advance(-1)) &&
          reg_file.get_id(reg_win.hi().advance(-1)) == reg_file.get_id(reg_win.hi()))
         continue;

      /* second, check that we have at most k=num_moves elements in the window
       * and no element is larger than the currently processed one */
      unsigned k = op_moves;
      unsigned n = 0;
      unsigned remaining_op_moves = op_moves;
      unsigned last_var = 0;
      bool found = true;
      bool aligned = rc == RegClass::v4 && reg_win.lo() % 4 == 0;
      for (const PhysReg j : reg_win) {
         /* dead operands effectively reduce the number of estimated moves */
         if (is_killed_operand[j & 0xFF]) {
            if (remaining_op_moves) {
               k--;
               remaining_op_moves--;
            }
            continue;
         }

         if (reg_file[j] == 0 || reg_file[j] == last_var)
            continue;

         if (reg_file[j] == 0xF0000000) {
            k += 1;
            n++;
            continue;
         }

         if (ctx.assignments[reg_file[j]].rc.size() >= size) {
            found = false;
            break;
         }

         /* we cannot split live ranges of linear vgprs */
         if (ctx.assignments[reg_file[j]].rc & (1 << 6)) {
            found = false;
            break;
         }

         k += ctx.assignments[reg_file[j]].rc.size();
         n++;
         last_var = reg_file[j];
      }

      if (!found || k > num_moves)
         continue;
      if (k == num_moves && n < num_vars)
         continue;
      if (!aligned && k == num_moves && n == num_vars)
         continue;

      if (found) {
         best_win = reg_win;
         num_moves = k;
         num_vars = n;
      }
   }

   if (num_moves == 0xFF)
      return {{}, false};

   /* now, we figured the placement for our definition */
   RegisterFile tmp_file(reg_file);
   std::set<std::pair<unsigned, unsigned>> vars = collect_vars(ctx, tmp_file, best_win);

   if (instr->opcode == aco_opcode::p_create_vector) {
      /* move killed operands which aren't yet at the correct position (GFX9+)
       * or which are in the definition space */
      PhysReg reg = best_win.lo();
      for (Operand& op : instr->operands) {
         if (op.isTemp() && op.isFirstKillBeforeDef() &&
             op.getTemp().type() == rc.type()) {
            if (op.physReg() != reg &&
                (ctx.program->chip_class >= GFX9 ||
                 (op.physReg().advance(op.bytes()) > best_win.lo() &&
                  op.physReg() < best_win.hi()))) {
               vars.emplace(op.bytes(), op.tempId());
               tmp_file.clear(op);
            } else {
               tmp_file.fill(op);
            }
         }
         reg.reg_b += op.bytes();
      }
   } else if (!is_phi(instr)) {
      /* re-enable killed operands */
      for (Operand& op : instr->operands) {
         if (op.isTemp() && op.isFirstKillBeforeDef())
            tmp_file.fill(op);
      }
   }

   std::vector<std::pair<Operand, Definition>> pc;
   if (!get_regs_for_copies(ctx, tmp_file, pc, vars, bounds, instr, best_win))
      return {{}, false};

   parallelcopies.insert(parallelcopies.end(), pc.begin(), pc.end());

   adjust_max_used_regs(ctx, rc, best_win.lo());
   return {best_win.lo(), true};
}

bool get_reg_specified(ra_ctx& ctx,
                       RegisterFile& reg_file,
                       RegClass rc,
                       aco_ptr<Instruction>& instr,
                       PhysReg reg)
{
   std::pair<unsigned, unsigned> sdw_def_info;
   if (rc.is_subdword())
      sdw_def_info = get_subdword_definition_info(ctx.program, instr, rc);

   if (rc.is_subdword() && reg.byte() % sdw_def_info.first)
      return false;
   if (!rc.is_subdword() && reg.byte())
      return false;

   if (rc.type() == RegType::sgpr && reg % get_stride(rc) != 0)
         return false;

   PhysRegInterval reg_win = { reg, rc.size() };
   PhysRegInterval bounds = get_reg_bounds(ctx.program, rc.type());
   PhysRegInterval vcc_win = { vcc, 2 };
   /* VCC is outside the bounds */
   bool is_vcc = rc.type() == RegType::sgpr && vcc_win.contains(reg_win);
   if (!bounds.contains(reg_win) && !is_vcc)
      return false;

   if (rc.is_subdword()) {
      PhysReg test_reg;
      test_reg.reg_b = reg.reg_b & ~(sdw_def_info.second - 1);
      if (reg_file.test(test_reg, sdw_def_info.second))
         return false;
   } else {
      if (reg_file.test(reg, rc.bytes()))
         return false;
   }

   adjust_max_used_regs(ctx, rc, reg_win.lo());
   return true;
}

bool increase_register_file(ra_ctx& ctx, RegType type) {
   if (type == RegType::vgpr && ctx.program->max_reg_demand.vgpr < ctx.vgpr_limit) {
      update_vgpr_sgpr_demand(ctx.program, RegisterDemand(ctx.program->max_reg_demand.vgpr + 1, ctx.program->max_reg_demand.sgpr));
   } else if (type == RegType::sgpr && ctx.program->max_reg_demand.sgpr < ctx.sgpr_limit) {
      update_vgpr_sgpr_demand(ctx.program,  RegisterDemand(ctx.program->max_reg_demand.vgpr, ctx.program->max_reg_demand.sgpr + 1));
   } else {
      return false;
   }
   return true;
}

struct IDAndRegClass {
   IDAndRegClass(unsigned id_, RegClass rc_) : id(id_), rc(rc_) {}

   unsigned id;
   RegClass rc;
};

struct IDAndInfo {
   IDAndInfo(unsigned id_, DefInfo info_) : id(id_), info(info_) {}

   unsigned id;
   DefInfo info;
};

/* Reallocates vars by sorting them and placing each variable after the previous
 * one. If one of the variables has 0xffffffff as an ID, the register assigned
 * for that variable will be returned.
 */
PhysReg compact_relocate_vars(ra_ctx& ctx, const std::vector<IDAndRegClass>& vars,
                              std::vector<std::pair<Operand, Definition>>& parallelcopies,
                              PhysReg start)
{
   /* This function assumes RegisterDemand/live_var_analysis rounds up sub-dword
    * temporary sizes to dwords.
    */
   std::vector<IDAndInfo> sorted;
   for (IDAndRegClass var : vars) {
      DefInfo info(ctx, ctx.pseudo_dummy, var.rc, -1);
      sorted.emplace_back(var.id, info);
   }

   std::sort(sorted.begin(), sorted.end(), [&ctx](const IDAndInfo& a,
                                                  const IDAndInfo& b) {
      unsigned a_stride = a.info.stride * (a.info.rc.is_subdword() ? 1 : 4);
      unsigned b_stride = b.info.stride * (b.info.rc.is_subdword() ? 1 : 4);
      if (a_stride > b_stride)
         return true;
      if (a_stride < b_stride)
         return false;
      if (a.id == 0xffffffff || b.id == 0xffffffff)
         return a.id == 0xffffffff; /* place 0xffffffff before others if possible, not for any reason */
      return ctx.assignments[a.id].reg < ctx.assignments[b.id].reg;
   });

   PhysReg next_reg = start;
   PhysReg space_reg;
   for (IDAndInfo& var : sorted) {
      unsigned stride = var.info.rc.is_subdword() ? var.info.stride : var.info.stride * 4;
      next_reg.reg_b = align(next_reg.reg_b, MAX2(stride, 4));

      /* 0xffffffff is a special variable ID used reserve a space for killed
       * operands and definitions.
       */
      if (var.id != 0xffffffff) {
         if (next_reg != ctx.assignments[var.id].reg) {
            RegClass rc = ctx.assignments[var.id].rc;
            Temp tmp(var.id, rc);

            Operand pc_op(tmp);
            pc_op.setFixed(ctx.assignments[var.id].reg);
            Definition pc_def(next_reg, rc);
            parallelcopies.emplace_back(pc_op, pc_def);
         }
      } else {
         space_reg = next_reg;
      }

      next_reg = next_reg.advance(var.info.rc.size() * 4);
   }

   return space_reg;
}

bool is_mimg_vaddr_intact(ra_ctx& ctx, RegisterFile& reg_file, Instruction *instr)
{
   PhysReg first{512};
   for (unsigned i = 0; i < instr->operands.size() - 3u; i++) {
      Operand op = instr->operands[i + 3];

      if (ctx.assignments[op.tempId()].assigned) {
         PhysReg reg = ctx.assignments[op.tempId()].reg;

         if (first.reg() != 512 && reg != first.advance(i * 4))
            return false; /* not at the best position */

         if ((reg.reg() - 256) < i)
            return false; /* no space for previous operands */

         first = reg.advance(i * -4);
      } else if (first.reg() != 512) {
         /* If there's an unexpected temporary, this operand is unlikely to be
          * placed in the best position.
          */
         unsigned id = reg_file.get_id(first.advance(i * 4));
         if (id && id != op.tempId())
            return false;
      }
   }

   return true;
}

PhysReg get_reg(ra_ctx& ctx,
                RegisterFile& reg_file,
                Temp temp,
                std::vector<std::pair<Operand, Definition>>& parallelcopies,
                aco_ptr<Instruction>& instr,
                int operand_index=-1)
{
   auto split_vec = ctx.split_vectors.find(temp.id());
   if (split_vec != ctx.split_vectors.end()) {
      unsigned offset = 0;
      for (Definition def : split_vec->second->definitions) {
         auto affinity_it = ctx.affinities.find(def.tempId());
         if (affinity_it != ctx.affinities.end() && ctx.assignments[affinity_it->second].assigned) {
            PhysReg reg = ctx.assignments[affinity_it->second].reg;
            reg.reg_b -= offset;
            if (get_reg_specified(ctx, reg_file, temp.regClass(), instr, reg))
               return reg;
         }
         offset += def.bytes();
      }
   }

   if (ctx.affinities.find(temp.id()) != ctx.affinities.end() &&
       ctx.assignments[ctx.affinities[temp.id()]].assigned) {
      PhysReg reg = ctx.assignments[ctx.affinities[temp.id()]].reg;
      if (get_reg_specified(ctx, reg_file, temp.regClass(), instr, reg))
         return reg;
   }

   if (ctx.vectors.find(temp.id()) != ctx.vectors.end()) {
      Instruction* vec = ctx.vectors[temp.id()];
      unsigned first_operand = vec->format == Format::MIMG ? 3 : 0;
      unsigned byte_offset = 0;
      for (unsigned i = first_operand; i < vec->operands.size(); i++) {
         Operand& op = vec->operands[i];
         if (op.isTemp() && op.tempId() == temp.id())
            break;
         else
            byte_offset += op.bytes();
      }

      if (vec->format != Format::MIMG || is_mimg_vaddr_intact(ctx, reg_file, vec)) {
         unsigned k = 0;
         for (unsigned i = first_operand; i < vec->operands.size(); i++) {
            Operand& op = vec->operands[i];
            if (op.isTemp() &&
                op.tempId() != temp.id() &&
                op.getTemp().type() == temp.type() &&
                ctx.assignments[op.tempId()].assigned) {
               PhysReg reg = ctx.assignments[op.tempId()].reg;
               reg.reg_b += (byte_offset - k);
               if (get_reg_specified(ctx, reg_file, temp.regClass(), instr, reg))
                  return reg;
            }
            k += op.bytes();
         }

         RegClass vec_rc = RegClass::get(temp.type(), k);
         DefInfo info(ctx, ctx.pseudo_dummy, vec_rc, -1);
         std::pair<PhysReg, bool> res = get_reg_simple(ctx, reg_file, info);
         PhysReg reg = res.first;
         if (res.second) {
            reg.reg_b += byte_offset;
            /* make sure to only use byte offset if the instruction supports it */
            if (get_reg_specified(ctx, reg_file, temp.regClass(), instr, reg))
               return reg;
         }
      }
   }

   DefInfo info(ctx, instr, temp.regClass(), operand_index);

   std::pair<PhysReg, bool> res;

   if (!ctx.policy.skip_optimistic_path) {
      /* try to find space without live-range splits */
      res = get_reg_simple(ctx, reg_file, info);

      if (res.second)
         return res.first;
   }

   /* try to find space with live-range splits */
   res = get_reg_impl(ctx, reg_file, parallelcopies, info, instr);

   if (res.second)
      return res.first;

   /* try using more registers */

   /* We should only fail here because keeping under the limit would require
    * too many moves. */
   assert(reg_file.count_zero(info.bounds) >= info.size);

   if (!increase_register_file(ctx, info.rc.type())) {
      /* fallback algorithm: reallocate all variables at once */
      unsigned def_size = info.rc.size();
      for (Definition def : instr->definitions) {
         if (ctx.assignments[def.tempId()].assigned && def.regClass().type() == info.rc.type())
            def_size += def.regClass().size();
      }

      unsigned killed_op_size = 0;
      for (Operand op : instr->operands) {
         if (op.isTemp() && op.isKillBeforeDef() && op.regClass().type() == info.rc.type())
            killed_op_size += op.regClass().size();
      }

      const PhysRegInterval regs = get_reg_bounds(ctx.program, info.rc.type());

      /* reallocate passthrough variables and non-killed operands */
      std::vector<IDAndRegClass> vars;
      for (const std::pair<unsigned, unsigned>& var : find_vars(ctx, reg_file, regs))
         vars.emplace_back(var.second, ctx.assignments[var.second].rc);
      vars.emplace_back(0xffffffff, RegClass(info.rc.type(), MAX2(def_size, killed_op_size)));

      PhysReg space = compact_relocate_vars(ctx, vars, parallelcopies, regs.lo());

      /* reallocate killed operands */
      std::vector<IDAndRegClass> killed_op_vars;
      for (Operand op : instr->operands) {
         if (op.isKillBeforeDef() && op.regClass().type() == info.rc.type())
            killed_op_vars.emplace_back(op.tempId(), op.regClass());
      }
      compact_relocate_vars(ctx, killed_op_vars, parallelcopies, space);

      /* reallocate definitions */
      std::vector<IDAndRegClass> def_vars;
      for (Definition def : instr->definitions) {
         if (ctx.assignments[def.tempId()].assigned && def.regClass().type() == info.rc.type())
            def_vars.emplace_back(def.tempId(), def.regClass());
      }
      def_vars.emplace_back(0xffffffff, info.rc);
      return compact_relocate_vars(ctx, def_vars, parallelcopies, space);
   }

   return get_reg(ctx, reg_file, temp, parallelcopies, instr, operand_index);
}

PhysReg get_reg_create_vector(ra_ctx& ctx,
                              RegisterFile& reg_file,
                              Temp temp,
                              std::vector<std::pair<Operand, Definition>>& parallelcopies,
                              aco_ptr<Instruction>& instr)
{
   RegClass rc = temp.regClass();
   /* create_vector instructions have different costs w.r.t. register coalescing */
   uint32_t size = rc.size();
   uint32_t bytes = rc.bytes();
   uint32_t stride = get_stride(rc);
   PhysRegInterval bounds = get_reg_bounds(ctx.program, rc.type());

   //TODO: improve p_create_vector for sub-dword vectors

   PhysReg best_pos { 0xFFF };
   unsigned num_moves = 0xFF;
   bool best_war_hint = true;

   /* test for each operand which definition placement causes the least shuffle instructions */
   for (unsigned i = 0, offset = 0; i < instr->operands.size(); offset += instr->operands[i].bytes(), i++) {
      // TODO: think about, if we can alias live operands on the same register
      if (!instr->operands[i].isTemp() || !instr->operands[i].isKillBeforeDef() || instr->operands[i].getTemp().type() != rc.type())
         continue;

      if (offset > instr->operands[i].physReg().reg_b)
         continue;

      unsigned reg_lower = instr->operands[i].physReg().reg_b - offset;
      if (reg_lower % 4)
         continue;
      PhysRegInterval reg_win = { PhysReg { reg_lower / 4 }, size };
      unsigned k = 0;

      /* no need to check multiple times */
      if (reg_win.lo() == best_pos)
         continue;

      /* check borders */
      // TODO: this can be improved */
      if (!bounds.contains(reg_win) || reg_win.lo() % stride != 0)
         continue;
      if (reg_win.lo() > bounds.lo() && reg_file[reg_win.lo()] != 0 && reg_file.get_id(reg_win.lo()) == reg_file.get_id(reg_win.lo().advance(-1)))
         continue;
      if (reg_win.hi() < bounds.hi() && reg_file[reg_win.hi().advance(-4)] != 0 && reg_file.get_id(reg_win.hi().advance(-1)) == reg_file.get_id(reg_win.hi()))
         continue;

      /* count variables to be moved and check war_hint */
      bool war_hint = false;
      bool linear_vgpr = false;
      for (PhysReg j : reg_win) {
         if (linear_vgpr) {
            break;
         }

         if (reg_file[j] != 0) {
            if (reg_file[j] == 0xF0000000) {
               PhysReg reg;
               reg.reg_b = j * 4;
               unsigned bytes_left = bytes - ((unsigned)j - reg_win.lo()) * 4;
               for (unsigned byte_idx = 0; byte_idx < MIN2(bytes_left, 4); byte_idx++, reg.reg_b++)
                  k += reg_file.test(reg, 1);
            } else {
               k += 4;
               /* we cannot split live ranges of linear vgprs */
               if (ctx.assignments[reg_file[j]].rc & (1 << 6))
                  linear_vgpr = true;
            }
         }
         war_hint |= ctx.war_hint[j];
      }
      if (linear_vgpr || (war_hint && !best_war_hint))
         continue;

      /* count operands in wrong positions */
      for (unsigned j = 0, offset2 = 0; j < instr->operands.size(); offset2 += instr->operands[j].bytes(), j++) {
         if (j == i ||
             !instr->operands[j].isTemp() ||
             instr->operands[j].getTemp().type() != rc.type())
            continue;
         if (instr->operands[j].physReg().reg_b != reg_win.lo() * 4 + offset2)
            k += instr->operands[j].bytes();
      }
      bool aligned = rc == RegClass::v4 && reg_win.lo() % 4 == 0;
      if (k > num_moves || (!aligned && k == num_moves))
         continue;

      best_pos = reg_win.lo();
      num_moves = k;
      best_war_hint = war_hint;
   }

   if (num_moves >= bytes)
      return get_reg(ctx, reg_file, temp, parallelcopies, instr);

   /* re-enable killed operands which are in the wrong position */
   RegisterFile tmp_file(reg_file);
   for (unsigned i = 0, offset = 0; i < instr->operands.size(); offset += instr->operands[i].bytes(), i++) {
      if (instr->operands[i].isTemp() &&
          instr->operands[i].isFirstKillBeforeDef() &&
          instr->operands[i].physReg().reg_b != best_pos.reg_b + offset)
         tmp_file.fill(instr->operands[i]);
   }

   /* collect variables to be moved */
   std::set<std::pair<unsigned, unsigned>> vars = collect_vars(ctx, tmp_file, PhysRegInterval { best_pos, size });

   for (unsigned i = 0, offset = 0; i < instr->operands.size(); offset += instr->operands[i].bytes(), i++) {
      if (!instr->operands[i].isTemp() || !instr->operands[i].isFirstKillBeforeDef() ||
          instr->operands[i].getTemp().type() != rc.type())
         continue;
      bool correct_pos = instr->operands[i].physReg().reg_b == best_pos.reg_b + offset;
      /* GFX9+: move killed operands which aren't yet at the correct position
       * Moving all killed operands generally leads to more register swaps.
       * This is only done on GFX9+ because of the cheap v_swap instruction.
       */
      if (ctx.program->chip_class >= GFX9 && !correct_pos) {
         vars.emplace(instr->operands[i].bytes(), instr->operands[i].tempId());
         tmp_file.clear(instr->operands[i]);
      /* fill operands which are in the correct position to avoid overwriting */
      } else if (correct_pos) {
         tmp_file.fill(instr->operands[i]);
      }
   }
   bool success = false;
   std::vector<std::pair<Operand, Definition>> pc;
   success = get_regs_for_copies(ctx, tmp_file, pc, vars, bounds, instr, PhysRegInterval { best_pos, size });

   if (!success) {
      if (!increase_register_file(ctx, temp.type())) {
         /* use the fallback algorithm in get_reg() */
         return get_reg(ctx, reg_file, temp, parallelcopies, instr);
      }
      return get_reg_create_vector(ctx, reg_file, temp, parallelcopies, instr);
   }

   parallelcopies.insert(parallelcopies.end(), pc.begin(), pc.end());
   adjust_max_used_regs(ctx, rc, best_pos);

   return best_pos;
}

void handle_pseudo(ra_ctx& ctx,
                   const RegisterFile& reg_file,
                   Instruction* instr)
{
   if (instr->format != Format::PSEUDO)
      return;

   /* all instructions which use handle_operands() need this information */
   switch (instr->opcode) {
   case aco_opcode::p_extract_vector:
   case aco_opcode::p_create_vector:
   case aco_opcode::p_split_vector:
   case aco_opcode::p_parallelcopy:
   case aco_opcode::p_wqm:
      break;
   default:
      return;
   }

   /* if all definitions are vgpr, no need to care for SCC */
   bool writes_sgpr = false;
   for (Definition& def : instr->definitions) {
      if (def.getTemp().type() == RegType::sgpr) {
         writes_sgpr = true;
         break;
      }
   }
   /* if all operands are constant, no need to care either */
   bool reads_sgpr = false;
   bool reads_subdword = false;
   for (Operand& op : instr->operands) {
      if (op.isTemp() && op.getTemp().type() == RegType::sgpr) {
         reads_sgpr = true;
         break;
      }
      if (op.isTemp() && op.regClass().is_subdword())
         reads_subdword = true;
   }
   bool needs_scratch_reg = (writes_sgpr && reads_sgpr) ||
                            (ctx.program->chip_class <= GFX7 && reads_subdword);
   if (!needs_scratch_reg)
      return;

   if (reg_file[scc]) {
      instr->pseudo().tmp_in_scc = true;

      int reg = ctx.max_used_sgpr;
      for (; reg >= 0 && reg_file[PhysReg{(unsigned)reg}]; reg--)
         ;
      if (reg < 0) {
         reg = ctx.max_used_sgpr + 1;
         for (; reg < ctx.program->max_reg_demand.sgpr && reg_file[PhysReg{(unsigned)reg}]; reg++)
            ;
         if (reg == ctx.program->max_reg_demand.sgpr) {
            assert(reads_subdword && reg_file[m0] == 0);
            reg = m0;
         }
      }

      adjust_max_used_regs(ctx, s1, reg);
      instr->pseudo().scratch_sgpr = PhysReg{(unsigned)reg};
   } else {
      instr->pseudo().tmp_in_scc = false;
   }
}

bool operand_can_use_reg(chip_class chip, aco_ptr<Instruction>& instr, unsigned idx, PhysReg reg, RegClass rc)
{
   if (instr->operands[idx].isFixed())
      return instr->operands[idx].physReg() == reg;

   bool is_writelane = instr->opcode == aco_opcode::v_writelane_b32 ||
                       instr->opcode == aco_opcode::v_writelane_b32_e64;
   if (chip <= GFX9 && is_writelane && idx <= 1) {
      /* v_writelane_b32 can take two sgprs but only if one is m0. */
      bool is_other_sgpr = instr->operands[!idx].isTemp() &&
                           (!instr->operands[!idx].isFixed() ||
                            instr->operands[!idx].physReg() != m0);
      if (is_other_sgpr && instr->operands[!idx].tempId() != instr->operands[idx].tempId()) {
         instr->operands[idx].setFixed(m0);
         return reg == m0;
      }
   }

   if (reg.byte()) {
      unsigned stride = get_subdword_operand_stride(chip, instr, idx, rc);
      if (reg.byte() % stride)
         return false;
   }

   switch (instr->format) {
   case Format::SMEM:
      return reg != scc &&
             reg != exec &&
             (reg != m0 || idx == 1 || idx == 3) && /* offset can be m0 */
             (reg != vcc || (instr->definitions.empty() && idx == 2) || chip >= GFX10); /* sdata can be vcc */
   default:
      // TODO: there are more instructions with restrictions on registers
      return true;
   }
}

void get_reg_for_operand(ra_ctx& ctx, RegisterFile& register_file,
                         std::vector<std::pair<Operand, Definition>>& parallelcopy,
                         aco_ptr<Instruction>& instr, Operand& operand, unsigned operand_index)
{
   /* check if the operand is fixed */
   PhysReg dst;
   bool blocking_var = false;
   if (operand.isFixed()) {
      assert(operand.physReg() != ctx.assignments[operand.tempId()].reg);

      /* check if target reg is blocked, and move away the blocking var */
      if (register_file[operand.physReg()]) {
         assert(register_file[operand.physReg()] != 0xF0000000);
         uint32_t blocking_id = register_file[operand.physReg()];
         RegClass rc = ctx.assignments[blocking_id].rc;
         Operand pc_op = Operand(Temp{blocking_id, rc});
         pc_op.setFixed(operand.physReg());

         /* find free reg */
         PhysReg reg = get_reg(ctx, register_file, pc_op.getTemp(), parallelcopy, ctx.pseudo_dummy);
         update_renames(ctx, register_file, parallelcopy, ctx.pseudo_dummy, true);
         Definition pc_def = Definition(reg, pc_op.regClass());
         parallelcopy.emplace_back(pc_op, pc_def);
         blocking_var = true;
      }
      dst = operand.physReg();

   } else {
      dst = get_reg(ctx, register_file, operand.getTemp(), parallelcopy, instr, operand_index);
      update_renames(ctx, register_file, parallelcopy, instr, instr->opcode != aco_opcode::p_create_vector);
   }

   Operand pc_op = operand;
   pc_op.setFixed(ctx.assignments[operand.tempId()].reg);
   Definition pc_def = Definition(dst, pc_op.regClass());
   parallelcopy.emplace_back(pc_op, pc_def);
   update_renames(ctx, register_file, parallelcopy, instr, true);

   if (operand.isKillBeforeDef())
      register_file.fill(parallelcopy.back().second);
   /* fill in case the blocking var is a killed operand (update_renames() will not fill it) */
   if (blocking_var)
      register_file.fill(parallelcopy[parallelcopy.size() - 2].second);
}

Temp read_variable(ra_ctx& ctx, Temp val, unsigned block_idx)
{
   std::unordered_map<unsigned, Temp>::iterator it = ctx.renames[block_idx].find(val.id());
   if (it == ctx.renames[block_idx].end())
      return val;
   else
      return it->second;
}

Temp handle_live_in(ra_ctx& ctx, Temp val, Block* block)
{
   std::vector<unsigned>& preds = val.is_linear() ? block->linear_preds : block->logical_preds;
   if (preds.size() == 0 || val.regClass() == val.regClass().as_linear())
      return val;

   assert(preds.size() > 0);

   Temp new_val;
   if (!ctx.sealed[block->index]) {
      /* consider rename from already processed predecessor */
      Temp tmp = read_variable(ctx, val, preds[0]);

      /* if the block is not sealed yet, we create an incomplete phi (which might later get removed again) */
      new_val = ctx.program->allocateTmp(val.regClass());
      ctx.assignments.emplace_back();
      aco_opcode opcode = val.is_linear() ? aco_opcode::p_linear_phi : aco_opcode::p_phi;
      aco_ptr<Instruction> phi{create_instruction<Pseudo_instruction>(opcode, Format::PSEUDO, preds.size(), 1)};
      phi->definitions[0] = Definition(new_val);
      for (unsigned i = 0; i < preds.size(); i++)
         phi->operands[i] = Operand(val);
      if (tmp.regClass() == new_val.regClass())
         ctx.affinities[new_val.id()] = tmp.id();

      ctx.phi_map.emplace(new_val.id(), phi_info{phi.get(), block->index});
      ctx.incomplete_phis[block->index].emplace_back(phi.get());
      block->instructions.insert(block->instructions.begin(), std::move(phi));

   } else if (preds.size() == 1) {
      /* if the block has only one predecessor, just look there for the name */
      new_val = read_variable(ctx, val, preds[0]);
   } else {
      /* there are multiple predecessors and the block is sealed */
      Temp *const ops = (Temp *)alloca(preds.size() * sizeof(Temp));

      /* get the rename from each predecessor and check if they are the same */
      bool needs_phi = false;
      for (unsigned i = 0; i < preds.size(); i++) {
         ops[i] = read_variable(ctx, val, preds[i]);
         if (i == 0)
            new_val = ops[i];
         else
            needs_phi |= !(new_val == ops[i]);
      }

      if (needs_phi) {
         /* the variable has been renamed differently in the predecessors: we need to insert a phi */
         aco_opcode opcode = val.is_linear() ? aco_opcode::p_linear_phi : aco_opcode::p_phi;
         aco_ptr<Instruction> phi{create_instruction<Pseudo_instruction>(opcode, Format::PSEUDO, preds.size(), 1)};
         new_val = ctx.program->allocateTmp(val.regClass());
         phi->definitions[0] = Definition(new_val);
         for (unsigned i = 0; i < preds.size(); i++) {
            phi->operands[i] = Operand(ops[i]);
            phi->operands[i].setFixed(ctx.assignments[ops[i].id()].reg);
            if (ops[i].regClass() == new_val.regClass())
               ctx.affinities[new_val.id()] = ops[i].id();
            /* make sure the operand gets it's original name in case
             * it comes from an incomplete phi */
            std::unordered_map<unsigned, phi_info>::iterator it = ctx.phi_map.find(ops[i].id());
            if (it != ctx.phi_map.end())
               it->second.uses.emplace(phi.get());
         }
         ctx.assignments.emplace_back();
         assert(ctx.assignments.size() == ctx.program->peekAllocationId());
         ctx.phi_map.emplace(new_val.id(), phi_info{phi.get(), block->index});
         block->instructions.insert(block->instructions.begin(), std::move(phi));
      }
   }

   if (new_val != val) {
      ctx.renames[block->index][val.id()] = new_val;
      ctx.orig_names[new_val.id()] = val;
   }
   return new_val;
}

void try_remove_trivial_phi(ra_ctx& ctx, Temp temp)
{
   std::unordered_map<unsigned, phi_info>::iterator info = ctx.phi_map.find(temp.id());

   if (info == ctx.phi_map.end() || !ctx.sealed[info->second.block_idx])
      return;

   assert(info->second.block_idx != 0);
   Instruction* phi = info->second.phi;
   Temp same = Temp();
   Definition def = phi->definitions[0];

   /* a phi node is trivial if all operands are the same as the definition of the phi */
   for (const Operand& op : phi->operands) {
      const Temp t = op.getTemp();
      if (t == same || t == def.getTemp()) {
         assert(t == same || op.physReg() == def.physReg());
         continue;
      }
      if (same != Temp() || op.physReg() != def.physReg())
         return;

      same = t;
   }
   assert(same != Temp() || same == def.getTemp());

   /* reroute all uses to same and remove phi */
   std::vector<Temp> phi_users;
   std::unordered_map<unsigned, phi_info>::iterator same_phi_info = ctx.phi_map.find(same.id());
   for (Instruction* instr : info->second.uses) {
      assert(phi != instr);
      /* recursively try to remove trivial phis */
      if (is_phi(instr)) {
         /* ignore if the phi was already flagged trivial */
         if (instr->definitions.empty())
            continue;

         if (instr->definitions[0].getTemp() != temp)
            phi_users.emplace_back(instr->definitions[0].getTemp());
      }
      for (Operand& op : instr->operands) {
         if (op.isTemp() && op.tempId() == def.tempId()) {
            op.setTemp(same);
            if (same_phi_info != ctx.phi_map.end())
               same_phi_info->second.uses.emplace(instr);
         }
      }
   }

   auto it = ctx.orig_names.find(same.id());
   unsigned orig_var = it != ctx.orig_names.end() ? it->second.id() : same.id();
   for (unsigned i = 0; i < ctx.program->blocks.size(); i++) {
      auto rename_it = ctx.renames[i].find(orig_var);
      if (rename_it != ctx.renames[i].end() && rename_it->second == def.getTemp())
         ctx.renames[i][orig_var] = same;
   }

   phi->definitions.clear(); /* this indicates that the phi can be removed */
   ctx.phi_map.erase(info);
   for (Temp t : phi_users)
      try_remove_trivial_phi(ctx, t);

   return;
}

} /* end namespace */


void register_allocation(Program *program, std::vector<IDSet>& live_out_per_block, ra_test_policy policy)
{
   ra_ctx ctx(program, policy);
   std::vector<std::vector<Temp>> phi_ressources;
   std::unordered_map<unsigned, unsigned> temp_to_phi_ressources;

   for (auto block_rit = program->blocks.rbegin(); block_rit != program->blocks.rend(); block_rit++) {
      Block& block = *block_rit;

      /* first, compute the death points of all live vars within the block */
      IDSet& live = live_out_per_block[block.index];

      std::vector<aco_ptr<Instruction>>::reverse_iterator rit;
      for (rit = block.instructions.rbegin(); rit != block.instructions.rend(); ++rit) {
         aco_ptr<Instruction>& instr = *rit;
         if (is_phi(instr)) {
            if (instr->definitions[0].isKill() || instr->definitions[0].isFixed()) {
               live.erase(instr->definitions[0].tempId());
               continue;
            }
            /* collect information about affinity-related temporaries */
            std::vector<Temp> affinity_related;
            /* affinity_related[0] is the last seen affinity-related temp */
            affinity_related.emplace_back(instr->definitions[0].getTemp());
            affinity_related.emplace_back(instr->definitions[0].getTemp());
            for (const Operand& op : instr->operands) {
               if (op.isTemp() && op.regClass() == instr->definitions[0].regClass()) {
                  affinity_related.emplace_back(op.getTemp());
                  temp_to_phi_ressources[op.tempId()] = phi_ressources.size();
               }
            }
            phi_ressources.emplace_back(std::move(affinity_related));
         } else {
            /* add vector affinities */
            if (instr->opcode == aco_opcode::p_create_vector) {
               for (const Operand& op : instr->operands) {
                  if (op.isTemp() && op.isFirstKill() && op.getTemp().type() == instr->definitions[0].getTemp().type())
                     ctx.vectors[op.tempId()] = instr.get();
               }
            } else if (instr->format == Format::MIMG && instr->operands.size() > 4) {
               for (unsigned i = 3; i < instr->operands.size(); i++)
                  ctx.vectors[instr->operands[i].tempId()] = instr.get();
            }

            if (instr->opcode == aco_opcode::p_split_vector && instr->operands[0].isFirstKillBeforeDef())
               ctx.split_vectors[instr->operands[0].tempId()] = instr.get();

            /* add operands to live variables */
            for (const Operand& op : instr->operands) {
               if (op.isTemp())
                  live.insert(op.tempId());
            }
         }

         /* erase definitions from live */
         for (unsigned i = 0; i < instr->definitions.size(); i++) {
            const Definition& def = instr->definitions[i];
            if (!def.isTemp())
               continue;
            live.erase(def.tempId());
            /* mark last-seen phi operand */
            std::unordered_map<unsigned, unsigned>::iterator it = temp_to_phi_ressources.find(def.tempId());
            if (it != temp_to_phi_ressources.end() && def.regClass() == phi_ressources[it->second][0].regClass()) {
               phi_ressources[it->second][0] = def.getTemp();
               /* try to coalesce phi affinities with parallelcopies */
               Operand op = Operand();
               if (!def.isFixed() && instr->opcode == aco_opcode::p_parallelcopy)
                  op = instr->operands[i];
               else if ((instr->opcode == aco_opcode::v_mad_f32 ||
                        (instr->opcode == aco_opcode::v_fma_f32 && program->chip_class >= GFX10) ||
                        instr->opcode == aco_opcode::v_mad_f16 ||
                        instr->opcode == aco_opcode::v_mad_legacy_f16 ||
                        (instr->opcode == aco_opcode::v_fma_f16 && program->chip_class >= GFX10)) && !instr->usesModifiers())
                  op = instr->operands[2];

               if (op.isTemp() && op.isFirstKillBeforeDef() && def.regClass() == op.regClass()) {
                  phi_ressources[it->second].emplace_back(op.getTemp());
                  temp_to_phi_ressources[op.tempId()] = it->second;
               }
            }
         }
      }
   }
   /* create affinities */
   for (std::vector<Temp>& vec : phi_ressources) {
      assert(vec.size() > 1);
      for (unsigned i = 1; i < vec.size(); i++)
         if (vec[i].id() != vec[0].id())
            ctx.affinities[vec[i].id()] = vec[0].id();
   }

   /* state of register file after phis */
   std::vector<std::bitset<128>> sgpr_live_in(program->blocks.size());

   for (Block& block : program->blocks) {
      IDSet& live = live_out_per_block[block.index];
      /* initialize register file */
      assert(block.index != 0 || live.empty());
      RegisterFile register_file;
      ctx.war_hint.reset();

      for (unsigned t : live) {
         Temp renamed = handle_live_in(ctx, Temp(t, program->temp_rc[t]), &block);
         assignment& var = ctx.assignments[renamed.id()];
         /* due to live-range splits, the live-in might be a phi, now */
         if (var.assigned)
            register_file.fill(Definition(renamed.id(), var.reg, var.rc));
      }

      std::vector<aco_ptr<Instruction>> instructions;
      std::vector<aco_ptr<Instruction>>::iterator instr_it;

      /* this is a slight adjustment from the paper as we already have phi nodes:
       * We consider them incomplete phis and only handle the definition. */

      /* look up the affinities */
      for (instr_it = block.instructions.begin(); instr_it != block.instructions.end(); ++instr_it) {
         aco_ptr<Instruction>& phi = *instr_it;
         if (!is_phi(phi))
            break;
         Definition& definition = phi->definitions[0];
         if (definition.isKill() || definition.isFixed())
             continue;

         if (ctx.affinities.find(definition.tempId()) != ctx.affinities.end() &&
             ctx.assignments[ctx.affinities[definition.tempId()]].assigned) {
            assert(ctx.assignments[ctx.affinities[definition.tempId()]].rc == definition.regClass());
            PhysReg reg = ctx.assignments[ctx.affinities[definition.tempId()]].reg;
            bool try_use_special_reg = reg == scc || reg == exec;
            if (try_use_special_reg) {
               for (const Operand& op : phi->operands) {
                  if (!(op.isTemp() && ctx.assignments[op.tempId()].assigned &&
                        ctx.assignments[op.tempId()].reg == reg)) {
                     try_use_special_reg = false;
                     break;
                  }
               }
               if (!try_use_special_reg)
                  continue;
            }
            /* only assign if register is still free */
            if (!register_file.test(reg, definition.bytes())) {
               definition.setFixed(reg);
               register_file.fill(definition);
               ctx.assignments[definition.tempId()] = {definition.physReg(), definition.regClass()};
            }
         }
      }

      /* find registers for phis without affinity or where the register was blocked */
      for (instr_it = block.instructions.begin();instr_it != block.instructions.end(); ++instr_it) {
         aco_ptr<Instruction>& phi = *instr_it;
         if (!is_phi(phi))
            break;

         Definition& definition = phi->definitions[0];
         if (definition.isKill())
            continue;

         if (!definition.isFixed()) {
            std::vector<std::pair<Operand, Definition>> parallelcopy;
            /* try to find a register that is used by at least one operand */
            for (const Operand& op : phi->operands) {
               if (!(op.isTemp() && ctx.assignments[op.tempId()].assigned))
                  continue;
               PhysReg reg = ctx.assignments[op.tempId()].reg;
               /* we tried this already on the previous loop */
               if (reg == scc || reg == exec)
                  continue;
               if (get_reg_specified(ctx, register_file, definition.regClass(), phi, reg)) {
                  definition.setFixed(reg);
                  break;
               }
            }
            if (!definition.isFixed()) {
               definition.setFixed(get_reg(ctx, register_file, definition.getTemp(), parallelcopy, phi));
               update_renames(ctx, register_file, parallelcopy, phi, true);
            }

            /* process parallelcopy */
            for (std::pair<Operand, Definition> pc : parallelcopy) {
               /* see if it's a copy from a different phi */
               //TODO: prefer moving some previous phis over live-ins
               //TODO: somehow prevent phis fixed before the RA from being updated (shouldn't be a problem in practice since they can only be fixed to exec)
               Instruction *prev_phi = NULL;
               std::vector<aco_ptr<Instruction>>::iterator phi_it;
               for (phi_it = instructions.begin(); phi_it != instructions.end(); ++phi_it) {
                  if ((*phi_it)->definitions[0].tempId() == pc.first.tempId())
                     prev_phi = phi_it->get();
               }
               phi_it = instr_it;
               while (!prev_phi && is_phi(*++phi_it)) {
                  if ((*phi_it)->definitions[0].tempId() == pc.first.tempId())
                     prev_phi = phi_it->get();
               }
               if (prev_phi) {
                  /* if so, just update that phi's register */
                  register_file.clear(prev_phi->definitions[0]);
                  prev_phi->definitions[0].setFixed(pc.second.physReg());
                  ctx.assignments[prev_phi->definitions[0].tempId()] = {pc.second.physReg(), pc.second.regClass()};
                  register_file.fill(prev_phi->definitions[0]);
                  continue;
               }

               /* rename */
               std::unordered_map<unsigned, Temp>::iterator orig_it = ctx.orig_names.find(pc.first.tempId());
               Temp orig = pc.first.getTemp();
               if (orig_it != ctx.orig_names.end())
                  orig = orig_it->second;
               else
                  ctx.orig_names[pc.second.tempId()] = orig;
               ctx.renames[block.index][orig.id()] = pc.second.getTemp();

               /* otherwise, this is a live-in and we need to create a new phi
                * to move it in this block's predecessors */
               aco_opcode opcode = pc.first.getTemp().is_linear() ? aco_opcode::p_linear_phi : aco_opcode::p_phi;
               std::vector<unsigned>& preds = pc.first.getTemp().is_linear() ? block.linear_preds : block.logical_preds;
               aco_ptr<Instruction> new_phi{create_instruction<Pseudo_instruction>(opcode, Format::PSEUDO, preds.size(), 1)};
               new_phi->definitions[0] = pc.second;
               for (unsigned i = 0; i < preds.size(); i++)
                  new_phi->operands[i] = Operand(pc.first);
               instructions.emplace_back(std::move(new_phi));
            }

            register_file.fill(definition);
            ctx.assignments[definition.tempId()] = {definition.physReg(), definition.regClass()};
         }
         live.insert(definition.tempId());

         /* update phi affinities */
         for (const Operand& op : phi->operands) {
            if (op.isTemp() && op.regClass() == phi->definitions[0].regClass())
               ctx.affinities[op.tempId()] = definition.tempId();
         }

         instructions.emplace_back(std::move(*instr_it));
      }

      /* fill in sgpr_live_in */
      for (unsigned i = 0; i <= ctx.max_used_sgpr; i++)
         sgpr_live_in[block.index][i] = register_file[PhysReg{i}];
      sgpr_live_in[block.index][127] = register_file[scc];

      /* Handle all other instructions of the block */
      for (; instr_it != block.instructions.end(); ++instr_it) {
         aco_ptr<Instruction>& instr = *instr_it;

         /* parallelcopies from p_phi are inserted here which means
          * live ranges of killed operands end here as well */
         if (instr->opcode == aco_opcode::p_logical_end) {
            /* no need to process this instruction any further */
            if (block.logical_succs.size() != 1) {
               instructions.emplace_back(std::move(instr));
               continue;
            }

            Block& succ = program->blocks[block.logical_succs[0]];
            unsigned idx = 0;
            for (; idx < succ.logical_preds.size(); idx++) {
               if (succ.logical_preds[idx] == block.index)
                  break;
            }
            for (aco_ptr<Instruction>& phi : succ.instructions) {
               if (phi->opcode == aco_opcode::p_phi) {
                  if (phi->operands[idx].isTemp() &&
                      phi->operands[idx].getTemp().type() == RegType::sgpr &&
                      phi->operands[idx].isFirstKillBeforeDef()) {
                     Definition phi_op(read_variable(ctx, phi->operands[idx].getTemp(), block.index));
                     phi_op.setFixed(ctx.assignments[phi_op.tempId()].reg);
                     register_file.clear(phi_op);
                  }
               } else if (phi->opcode != aco_opcode::p_linear_phi) {
                  break;
               }
            }
            instructions.emplace_back(std::move(instr));
            continue;
         }

         std::vector<std::pair<Operand, Definition>> parallelcopy;

         assert(!is_phi(instr));

         /* handle operands */
         for (unsigned i = 0; i < instr->operands.size(); ++i) {
            auto& operand = instr->operands[i];
            if (!operand.isTemp())
               continue;

            /* rename operands */
            operand.setTemp(read_variable(ctx, operand.getTemp(), block.index));
            assert(ctx.assignments[operand.tempId()].assigned);

            PhysReg reg = ctx.assignments[operand.tempId()].reg;
            if (operand_can_use_reg(program->chip_class, instr, i, reg, operand.regClass()))
               operand.setFixed(reg);
            else
               get_reg_for_operand(ctx, register_file, parallelcopy, instr, operand, i);

            if (instr->isEXP() ||
                (instr->isVMEM() && i == 3 && ctx.program->chip_class == GFX6) ||
                (instr->isDS() && instr->ds().gds)) {
               for (unsigned j = 0; j < operand.size(); j++)
                  ctx.war_hint.set(operand.physReg().reg() + j);
            }

            std::unordered_map<unsigned, phi_info>::iterator phi = ctx.phi_map.find(operand.getTemp().id());
            if (phi != ctx.phi_map.end())
               phi->second.uses.emplace(instr.get());
         }

         /* remove dead vars from register file */
         for (const Operand& op : instr->operands) {
            if (op.isTemp() && op.isFirstKillBeforeDef())
               register_file.clear(op);
         }

         /* try to optimize v_mad_f32 -> v_mac_f32 */
         if ((instr->opcode == aco_opcode::v_mad_f32 ||
              (instr->opcode == aco_opcode::v_fma_f32 && program->chip_class >= GFX10) ||
              instr->opcode == aco_opcode::v_mad_f16 ||
              instr->opcode == aco_opcode::v_mad_legacy_f16 ||
              (instr->opcode == aco_opcode::v_fma_f16 && program->chip_class >= GFX10) ||
              (instr->opcode == aco_opcode::v_pk_fma_f16 && program->chip_class >= GFX10)) &&
             instr->operands[2].isTemp() &&
             instr->operands[2].isKillBeforeDef() &&
             instr->operands[2].getTemp().type() == RegType::vgpr &&
             instr->operands[1].isTemp() &&
             instr->operands[1].getTemp().type() == RegType::vgpr &&
             !instr->usesModifiers() &&
             instr->operands[0].physReg().byte() == 0 &&
             instr->operands[1].physReg().byte() == 0 &&
             instr->operands[2].physReg().byte() == 0) {
            unsigned def_id = instr->definitions[0].tempId();
            auto it = ctx.affinities.find(def_id);
            if (it == ctx.affinities.end() || !ctx.assignments[it->second].assigned ||
                instr->operands[2].physReg() == ctx.assignments[it->second].reg ||
                register_file.test(ctx.assignments[it->second].reg, instr->operands[2].bytes())) {
               instr->format = Format::VOP2;
               switch (instr->opcode) {
               case aco_opcode::v_mad_f32:
                  instr->opcode = aco_opcode::v_mac_f32;
                  break;
               case aco_opcode::v_fma_f32:
                  instr->opcode = aco_opcode::v_fmac_f32;
                  break;
               case aco_opcode::v_mad_f16:
               case aco_opcode::v_mad_legacy_f16:
                  instr->opcode = aco_opcode::v_mac_f16;
                  break;
               case aco_opcode::v_fma_f16:
                  instr->opcode = aco_opcode::v_fmac_f16;
                  break;
               case aco_opcode::v_pk_fma_f16:
                  instr->opcode = aco_opcode::v_pk_fmac_f16;
                  break;
               default:
                  break;
               }
            }
         }

         /* handle definitions which must have the same register as an operand */
         if (instr->opcode == aco_opcode::v_interp_p2_f32 ||
             instr->opcode == aco_opcode::v_mac_f32 ||
             instr->opcode == aco_opcode::v_fmac_f32 ||
             instr->opcode == aco_opcode::v_mac_f16 ||
             instr->opcode == aco_opcode::v_fmac_f16 ||
             instr->opcode == aco_opcode::v_pk_fmac_f16 ||
             instr->opcode == aco_opcode::v_writelane_b32 ||
             instr->opcode == aco_opcode::v_writelane_b32_e64) {
            instr->definitions[0].setFixed(instr->operands[2].physReg());
         } else if (instr->opcode == aco_opcode::s_addk_i32 ||
                    instr->opcode == aco_opcode::s_mulk_i32) {
            instr->definitions[0].setFixed(instr->operands[0].physReg());
         } else if (instr->isMUBUF() &&
                    instr->definitions.size() == 1 &&
                    instr->operands.size() == 4) {
            instr->definitions[0].setFixed(instr->operands[3].physReg());
         } else if (instr->isMIMG() &&
                    instr->definitions.size() == 1 &&
                    !instr->operands[2].isUndefined()) {
            instr->definitions[0].setFixed(instr->operands[2].physReg());
         }

         ctx.defs_done.reset();

         /* handle fixed definitions first */
         for (unsigned i = 0; i < instr->definitions.size(); ++i) {
            auto& definition = instr->definitions[i];
            if (!definition.isFixed())
               continue;

            adjust_max_used_regs(ctx, definition.regClass(), definition.physReg());
            /* check if the target register is blocked */
            if (register_file.test(definition.physReg(), definition.bytes())) {
               const PhysRegInterval def_regs { definition.physReg(), definition.size() };

               /* create parallelcopy pair to move blocking vars */
               std::set<std::pair<unsigned, unsigned>> vars = collect_vars(ctx, register_file, def_regs);

               RegisterFile tmp_file(register_file);
               /* re-enable the killed operands, so that we don't move the blocking vars there */
               for (const Operand& op : instr->operands) {
                  if (op.isTemp() && op.isFirstKillBeforeDef())
                     tmp_file.fill(op);
               }

               ASSERTED bool success = false;
               DefInfo info(ctx, instr, definition.regClass(), -1);
               success = get_regs_for_copies(ctx, tmp_file, parallelcopy,
                                             vars, info.bounds, instr,
                                             def_regs);
               assert(success);

               update_renames(ctx, register_file, parallelcopy, instr, false);
            }
            ctx.defs_done.set(i);

            if (!definition.isTemp())
               continue;

            /* set live if it has a kill point */
            if (!definition.isKill())
               live.insert(definition.tempId());

            ctx.assignments[definition.tempId()] = {definition.physReg(), definition.regClass()};
            register_file.fill(definition);
         }

         /* handle all other definitions */
         for (unsigned i = 0; i < instr->definitions.size(); ++i) {
            Definition *definition = &instr->definitions[i];

            if (definition->isFixed() || !definition->isTemp())
               continue;

            /* find free reg */
            if (definition->hasHint() && get_reg_specified(ctx, register_file, definition->regClass(), instr, definition->physReg())) {
               definition->setFixed(definition->physReg());
            } else if (instr->opcode == aco_opcode::p_split_vector) {
               PhysReg reg = instr->operands[0].physReg();
               for (unsigned j = 0; j < i; j++)
                  reg.reg_b += instr->definitions[j].bytes();
               if (get_reg_specified(ctx, register_file, definition->regClass(), instr, reg))
                  definition->setFixed(reg);
            } else if (instr->opcode == aco_opcode::p_wqm || instr->opcode == aco_opcode::p_parallelcopy) {
               PhysReg reg = instr->operands[i].physReg();
               if (instr->operands[i].isTemp() &&
                   instr->operands[i].getTemp().type() == definition->getTemp().type() &&
                   !register_file.test(reg, definition->bytes()))
                  definition->setFixed(reg);
            } else if (instr->opcode == aco_opcode::p_extract_vector) {
               PhysReg reg = instr->operands[0].physReg();
               reg.reg_b += definition->bytes() * instr->operands[1].constantValue();
               if (get_reg_specified(ctx, register_file, definition->regClass(), instr, reg))
                  definition->setFixed(reg);
            } else if (instr->opcode == aco_opcode::p_create_vector) {
               PhysReg reg = get_reg_create_vector(ctx, register_file, definition->getTemp(),
                                                   parallelcopy, instr);
               update_renames(ctx, register_file, parallelcopy, instr, false);
               definition->setFixed(reg);
            }

            if (!definition->isFixed()) {
               Temp tmp = definition->getTemp();
               if (definition->regClass().is_subdword() && definition->bytes() < 4) {
                  PhysReg reg = get_reg(ctx, register_file, tmp, parallelcopy, instr);
                  definition->setFixed(reg);
                  if (reg.byte() || register_file.test(reg, 4)) {
                     add_subdword_definition(program, instr, i, reg);
                     definition = &instr->definitions[i]; /* add_subdword_definition can invalidate the reference */
                  }
               } else {
                  definition->setFixed(get_reg(ctx, register_file, tmp, parallelcopy, instr));
               }
               update_renames(ctx, register_file, parallelcopy, instr, instr->opcode != aco_opcode::p_create_vector);
            }

            assert(definition->isFixed() && ((definition->getTemp().type() == RegType::vgpr && definition->physReg() >= 256) ||
                                             (definition->getTemp().type() != RegType::vgpr && definition->physReg() < 256)));
            ctx.defs_done.set(i);

            /* set live if it has a kill point */
            if (!definition->isKill())
               live.insert(definition->tempId());

            ctx.assignments[definition->tempId()] = {definition->physReg(), definition->regClass()};
            register_file.fill(*definition);
         }

         handle_pseudo(ctx, register_file, instr.get());

         /* kill definitions and late-kill operands and ensure that sub-dword operands can actually be read */
         for (const Definition& def : instr->definitions) {
             if (def.isTemp() && def.isKill())
                register_file.clear(def);
         }
         for (unsigned i = 0; i < instr->operands.size(); i++) {
            const Operand& op = instr->operands[i];
            if (op.isTemp() && op.isFirstKill() && op.isLateKill())
               register_file.clear(op);
            if (op.isTemp() && op.physReg().byte() != 0)
               add_subdword_operand(ctx, instr, i, op.physReg().byte(), op.regClass());
         }

         /* emit parallelcopy */
         if (!parallelcopy.empty()) {
            aco_ptr<Pseudo_instruction> pc;
            pc.reset(create_instruction<Pseudo_instruction>(aco_opcode::p_parallelcopy, Format::PSEUDO, parallelcopy.size(), parallelcopy.size()));
            bool temp_in_scc = register_file[scc];
            bool sgpr_operands_alias_defs = false;
            uint64_t sgpr_operands[4] = {0, 0, 0, 0};
            for (unsigned i = 0; i < parallelcopy.size(); i++) {
               if (temp_in_scc && parallelcopy[i].first.isTemp() && parallelcopy[i].first.getTemp().type() == RegType::sgpr) {
                  if (!sgpr_operands_alias_defs) {
                     unsigned reg = parallelcopy[i].first.physReg().reg();
                     unsigned size = parallelcopy[i].first.getTemp().size();
                     sgpr_operands[reg / 64u] |= u_bit_consecutive64(reg % 64u, size);

                     reg = parallelcopy[i].second.physReg().reg();
                     size = parallelcopy[i].second.getTemp().size();
                     if (sgpr_operands[reg / 64u] & u_bit_consecutive64(reg % 64u, size))
                        sgpr_operands_alias_defs = true;
                  }
               }

               pc->operands[i] = parallelcopy[i].first;
               pc->definitions[i] = parallelcopy[i].second;
               assert(pc->operands[i].size() == pc->definitions[i].size());

               /* it might happen that the operand is already renamed. we have to restore the original name. */
               std::unordered_map<unsigned, Temp>::iterator it = ctx.orig_names.find(pc->operands[i].tempId());
               Temp orig = it != ctx.orig_names.end() ? it->second : pc->operands[i].getTemp();
               ctx.orig_names[pc->definitions[i].tempId()] = orig;
               ctx.renames[block.index][orig.id()] = pc->definitions[i].getTemp();

               std::unordered_map<unsigned, phi_info>::iterator phi = ctx.phi_map.find(pc->operands[i].tempId());
               if (phi != ctx.phi_map.end())
                  phi->second.uses.emplace(pc.get());
            }

            if (temp_in_scc && sgpr_operands_alias_defs) {
               /* disable definitions and re-enable operands */
               RegisterFile tmp_file(register_file);
               for (const Definition& def : instr->definitions) {
                  if (def.isTemp() && !def.isKill())
                     tmp_file.clear(def);
               }
               for (const Operand& op : instr->operands) {
                  if (op.isTemp() && op.isFirstKill())
                     tmp_file.block(op.physReg(), op.regClass());
               }

               handle_pseudo(ctx, tmp_file, pc.get());
            } else {
               pc->tmp_in_scc = false;
            }

            instructions.emplace_back(std::move(pc));
         }

         /* some instructions need VOP3 encoding if operand/definition is not assigned to VCC */
         bool instr_needs_vop3 = !instr->isVOP3() &&
                                 ((instr->format == Format::VOPC && !(instr->definitions[0].physReg() == vcc)) ||
                                  (instr->opcode == aco_opcode::v_cndmask_b32 && !(instr->operands[2].physReg() == vcc)) ||
                                  ((instr->opcode == aco_opcode::v_add_co_u32 ||
                                    instr->opcode == aco_opcode::v_addc_co_u32 ||
                                    instr->opcode == aco_opcode::v_sub_co_u32 ||
                                    instr->opcode == aco_opcode::v_subb_co_u32 ||
                                    instr->opcode == aco_opcode::v_subrev_co_u32 ||
                                    instr->opcode == aco_opcode::v_subbrev_co_u32) &&
                                   !(instr->definitions[1].physReg() == vcc)) ||
                                  ((instr->opcode == aco_opcode::v_addc_co_u32 ||
                                    instr->opcode == aco_opcode::v_subb_co_u32 ||
                                    instr->opcode == aco_opcode::v_subbrev_co_u32) &&
                                   !(instr->operands[2].physReg() == vcc)));
         if (instr_needs_vop3) {

            /* if the first operand is a literal, we have to move it to a reg */
            if (instr->operands.size() && instr->operands[0].isLiteral() && program->chip_class < GFX10) {
               bool can_sgpr = true;
               /* check, if we have to move to vgpr */
               for (const Operand& op : instr->operands) {
                  if (op.isTemp() && op.getTemp().type() == RegType::sgpr) {
                     can_sgpr = false;
                     break;
                  }
               }
               /* disable definitions and re-enable operands */
               RegisterFile tmp_file(register_file);
               for (const Definition& def : instr->definitions)
                  tmp_file.clear(def);
               for (const Operand& op : instr->operands) {
                  if (op.isTemp() && op.isFirstKill())
                     tmp_file.block(op.physReg(), op.regClass());
               }
               Temp tmp = program->allocateTmp(can_sgpr ? s1 : v1);
               ctx.assignments.emplace_back();
               PhysReg reg = get_reg(ctx, tmp_file, tmp, parallelcopy, instr);
               update_renames(ctx, register_file, parallelcopy, instr, true);

               aco_ptr<Instruction> mov;
               if (can_sgpr)
                  mov.reset(create_instruction<SOP1_instruction>(aco_opcode::s_mov_b32, Format::SOP1, 1, 1));
               else
                  mov.reset(create_instruction<VOP1_instruction>(aco_opcode::v_mov_b32, Format::VOP1, 1, 1));
               mov->operands[0] = instr->operands[0];
               mov->definitions[0] = Definition(tmp);
               mov->definitions[0].setFixed(reg);

               instr->operands[0] = Operand(tmp);
               instr->operands[0].setFixed(reg);
               instr->operands[0].setFirstKill(true);

               instructions.emplace_back(std::move(mov));
            }

            /* change the instruction to VOP3 to enable an arbitrary register pair as dst */
            aco_ptr<Instruction> tmp = std::move(instr);
            Format format = asVOP3(tmp->format);
            instr.reset(create_instruction<VOP3_instruction>(tmp->opcode, format, tmp->operands.size(), tmp->definitions.size()));
            std::copy(tmp->operands.begin(), tmp->operands.end(), instr->operands.begin());
            std::copy(tmp->definitions.begin(), tmp->definitions.end(), instr->definitions.begin());
            update_phi_map(ctx, tmp.get(), instr.get());
         }

         instructions.emplace_back(std::move(*instr_it));

      } /* end for Instr */

      block.instructions = std::move(instructions);

      ctx.filled[block.index] = true;
      for (unsigned succ_idx : block.linear_succs) {
         Block& succ = program->blocks[succ_idx];
         /* seal block if all predecessors are filled */
         bool all_filled = true;
         for (unsigned pred_idx : succ.linear_preds) {
            if (!ctx.filled[pred_idx]) {
               all_filled = false;
               break;
            }
         }
         if (all_filled) {
            ctx.sealed[succ_idx] = true;

            /* finish incomplete phis and check if they became trivial */
            for (Instruction* phi : ctx.incomplete_phis[succ_idx]) {
               std::vector<unsigned> preds = phi->definitions[0].getTemp().is_linear() ? succ.linear_preds : succ.logical_preds;
               for (unsigned i = 0; i < phi->operands.size(); i++) {
                  phi->operands[i].setTemp(read_variable(ctx, phi->operands[i].getTemp(), preds[i]));
                  phi->operands[i].setFixed(ctx.assignments[phi->operands[i].tempId()].reg);
               }
               try_remove_trivial_phi(ctx, phi->definitions[0].getTemp());
            }
            /* complete the original phi nodes, but no need to check triviality */
            for (aco_ptr<Instruction>& instr : succ.instructions) {
               if (!is_phi(instr))
                  break;
               std::vector<unsigned> preds = instr->opcode == aco_opcode::p_phi ? succ.logical_preds : succ.linear_preds;

               for (unsigned i = 0; i < instr->operands.size(); i++) {
                  auto& operand = instr->operands[i];
                  if (!operand.isTemp())
                     continue;
                  operand.setTemp(read_variable(ctx, operand.getTemp(), preds[i]));
                  operand.setFixed(ctx.assignments[operand.tempId()].reg);
                  std::unordered_map<unsigned, phi_info>::iterator phi = ctx.phi_map.find(operand.getTemp().id());
                  if (phi != ctx.phi_map.end())
                     phi->second.uses.emplace(instr.get());
               }
            }
         }
      }
   } /* end for BB */

   /* remove trivial phis */
   for (Block& block : program->blocks) {
      auto end = std::find_if(block.instructions.begin(), block.instructions.end(),
                              [](aco_ptr<Instruction>& instr) { return !is_phi(instr);});
      auto middle = std::remove_if(block.instructions.begin(), end,
                                   [](const aco_ptr<Instruction>& instr) { return instr->definitions.empty();});
      block.instructions.erase(middle, end);
   }

   /* find scc spill registers which may be needed for parallelcopies created by phis */
   for (Block& block : program->blocks) {
      if (block.linear_preds.size() <= 1)
         continue;

      std::bitset<128> regs = sgpr_live_in[block.index];
      if (!regs[127])
         continue;

      /* choose a register */
      int16_t reg = 0;
      for (; reg < ctx.program->max_reg_demand.sgpr && regs[reg]; reg++)
         ;
      assert(reg < ctx.program->max_reg_demand.sgpr);
      adjust_max_used_regs(ctx, s1, reg);

      /* update predecessors */
      for (unsigned& pred_index : block.linear_preds) {
         Block& pred = program->blocks[pred_index];
         pred.scc_live_out = true;
         pred.scratch_sgpr = PhysReg{(uint16_t)reg};
      }
   }

   /* num_gpr = rnd_up(max_used_gpr + 1) */
   program->config->num_vgprs = get_vgpr_alloc(program, ctx.max_used_vgpr + 1);
   program->config->num_sgprs = get_sgpr_alloc(program, ctx.max_used_sgpr + 1);
}

}
