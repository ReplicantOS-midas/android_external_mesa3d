/*
 * Copyright (c) 2017 Lima Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#include "util/ralloc.h"
#include "util/register_allocate.h"
#include "util/u_debug.h"

#include "ppir.h"
#include "lima_context.h"

#define PPIR_FULL_REG_NUM  6

#define PPIR_VEC1_REG_NUM       (PPIR_FULL_REG_NUM * 4) /* x, y, z, w */
#define PPIR_VEC2_REG_NUM       (PPIR_FULL_REG_NUM * 3) /* xy, yz, zw */
#define PPIR_VEC3_REG_NUM       (PPIR_FULL_REG_NUM * 2) /* xyz, yzw */
#define PPIR_VEC4_REG_NUM       PPIR_FULL_REG_NUM       /* xyzw */
#define PPIR_HEAD_VEC1_REG_NUM  PPIR_FULL_REG_NUM       /* x */
#define PPIR_HEAD_VEC2_REG_NUM  PPIR_FULL_REG_NUM       /* xy */
#define PPIR_HEAD_VEC3_REG_NUM  PPIR_FULL_REG_NUM       /* xyz */
#define PPIR_HEAD_VEC4_REG_NUM  PPIR_FULL_REG_NUM       /* xyzw */

#define PPIR_VEC1_REG_BASE       0
#define PPIR_VEC2_REG_BASE       (PPIR_VEC1_REG_BASE + PPIR_VEC1_REG_NUM)
#define PPIR_VEC3_REG_BASE       (PPIR_VEC2_REG_BASE + PPIR_VEC2_REG_NUM)
#define PPIR_VEC4_REG_BASE       (PPIR_VEC3_REG_BASE + PPIR_VEC3_REG_NUM)
#define PPIR_HEAD_VEC1_REG_BASE  (PPIR_VEC4_REG_BASE + PPIR_VEC4_REG_NUM)
#define PPIR_HEAD_VEC2_REG_BASE  (PPIR_HEAD_VEC1_REG_BASE + PPIR_HEAD_VEC1_REG_NUM)
#define PPIR_HEAD_VEC3_REG_BASE  (PPIR_HEAD_VEC2_REG_BASE + PPIR_HEAD_VEC2_REG_NUM)
#define PPIR_HEAD_VEC4_REG_BASE  (PPIR_HEAD_VEC3_REG_BASE + PPIR_HEAD_VEC3_REG_NUM)
#define PPIR_REG_COUNT           (PPIR_HEAD_VEC4_REG_BASE + PPIR_HEAD_VEC4_REG_NUM)

enum ppir_ra_reg_class {
   ppir_ra_reg_class_vec1,
   ppir_ra_reg_class_vec2,
   ppir_ra_reg_class_vec3,
   ppir_ra_reg_class_vec4,

   /* 4 reg class for load/store instr regs:
    * load/store instr has no swizzle field, so the (virtual) register
    * must be allocated at the beginning of a (physical) register,
    */
   ppir_ra_reg_class_head_vec1,
   ppir_ra_reg_class_head_vec2,
   ppir_ra_reg_class_head_vec3,
   ppir_ra_reg_class_head_vec4,

   ppir_ra_reg_class_num,
};

static const int ppir_ra_reg_base[ppir_ra_reg_class_num + 1] = {
   [ppir_ra_reg_class_vec1]       = PPIR_VEC1_REG_BASE,
   [ppir_ra_reg_class_vec2]       = PPIR_VEC2_REG_BASE,
   [ppir_ra_reg_class_vec3]       = PPIR_VEC3_REG_BASE,
   [ppir_ra_reg_class_vec4]       = PPIR_VEC4_REG_BASE,
   [ppir_ra_reg_class_head_vec1]  = PPIR_HEAD_VEC1_REG_BASE,
   [ppir_ra_reg_class_head_vec2]  = PPIR_HEAD_VEC2_REG_BASE,
   [ppir_ra_reg_class_head_vec3]  = PPIR_HEAD_VEC3_REG_BASE,
   [ppir_ra_reg_class_head_vec4]  = PPIR_HEAD_VEC4_REG_BASE,
   [ppir_ra_reg_class_num]        = PPIR_REG_COUNT,
};

static unsigned int *
ppir_ra_reg_q_values[ppir_ra_reg_class_num] = {
   (unsigned int []) {1, 2, 3, 4, 1, 2, 3, 4},
   (unsigned int []) {2, 3, 3, 3, 1, 2, 3, 3},
   (unsigned int []) {2, 2, 2, 2, 1, 2, 2, 2},
   (unsigned int []) {1, 1, 1, 1, 1, 1, 1, 1},
   (unsigned int []) {1, 1, 1, 1, 1, 1, 1, 1},
   (unsigned int []) {1, 1, 1, 1, 1, 1, 1, 1},
   (unsigned int []) {1, 1, 1, 1, 1, 1, 1, 1},
   (unsigned int []) {1, 1, 1, 1, 1, 1, 1, 1},
};

struct ra_regs *ppir_regalloc_init(void *mem_ctx)
{
   struct ra_regs *ret = ra_alloc_reg_set(mem_ctx, PPIR_REG_COUNT, false);
   if (!ret)
      return NULL;

   /* (x, y, z, w) (xy, yz, zw) (xyz, yzw) (xyzw) (x) (xy) (xyz) (xyzw) */
   static const int class_reg_num[ppir_ra_reg_class_num] = {
      4, 3, 2, 1, 1, 1, 1, 1,
   };
   /* base reg (x, y, z, w) confliction with other regs */
   for (int h = 0; h < 4; h++) {
      int base_reg_mask = 1 << h;
      for (int i = 1; i < ppir_ra_reg_class_num; i++) {
         int class_reg_base_mask = (1 << ((i % 4) + 1)) - 1;
         for (int j = 0; j < class_reg_num[i]; j++) {
            if (base_reg_mask & (class_reg_base_mask << j)) {
               for (int k = 0; k < PPIR_FULL_REG_NUM; k++) {
                  ra_add_reg_conflict(ret, k * 4 + h,
                     ppir_ra_reg_base[i] + k * class_reg_num[i] + j);
               }
            }
         }
      }
   }
   /* build all other confliction by the base reg confliction */
   for (int i = 0; i < PPIR_VEC1_REG_NUM; i++)
      ra_make_reg_conflicts_transitive(ret, i);

   for (int i = 0; i < ppir_ra_reg_class_num; i++)
      ra_alloc_reg_class(ret);

   int reg_index = 0;
   for (int i = 0; i < ppir_ra_reg_class_num; i++) {
      while (reg_index < ppir_ra_reg_base[i + 1])
         ra_class_add_reg(ret, i, reg_index++);
   }

   ra_set_finalize(ret, ppir_ra_reg_q_values);
   return ret;
}

static void ppir_regalloc_update_reglist_ssa(ppir_compiler *comp)
{
   list_for_each_entry(ppir_block, block, &comp->block_list, list) {
      list_for_each_entry(ppir_node, node, &block->node_list, list) {
         if (node->is_end)
            continue;

         if (!node->instr || node->op == ppir_op_const)
            continue;

         ppir_dest *dest = ppir_node_get_dest(node);
         if (dest) {
            ppir_reg *reg = NULL;

            if (dest->type == ppir_target_ssa) {
               reg = &dest->ssa;
               list_addtail(&reg->list, &comp->reg_list);
               comp->reg_num++;
            }
         }
      }
   }
}

static int get_phy_reg_index(int reg)
{
   int i;

   for (i = 0; i < ppir_ra_reg_class_num; i++) {
      if (reg < ppir_ra_reg_base[i + 1]) {
         reg -= ppir_ra_reg_base[i];
         break;
      }
   }

   if (i < ppir_ra_reg_class_head_vec1)
      return reg / (4 - i) * 4 + reg % (4 - i);
   else
      return reg * 4;
}

static void ppir_regalloc_print_result(ppir_compiler *comp)
{
   printf("======ppir regalloc result======\n");
   list_for_each_entry(ppir_block, block, &comp->block_list, list) {
      list_for_each_entry(ppir_instr, instr, &block->instr_list, list) {
         printf("%03d:", instr->index);
         for (int i = 0; i < PPIR_INSTR_SLOT_NUM; i++) {
            ppir_node *node = instr->slots[i];
            if (!node)
               continue;

            printf(" (%d|", node->index);

            ppir_dest *dest = ppir_node_get_dest(node);
            if (dest)
               printf("%d", ppir_target_get_dest_reg_index(dest));

            printf("|");

            for (int i = 0; i < ppir_node_get_src_num(node); i++) {
               if (i)
                  printf(" ");
               printf("%d", ppir_target_get_src_reg_index(ppir_node_get_src(node, i)));
            }

            printf(")");
         }
         printf("\n");
      }
   }
   printf("--------------------------\n");
}

static bool create_new_instr_after(ppir_block *block, ppir_instr *ref,
                                   ppir_node *node)
{
   ppir_instr *newinstr = ppir_instr_create(block);
   if (unlikely(!newinstr))
      return false;

   list_del(&newinstr->list);
   list_add(&newinstr->list, &ref->list);

   if (!ppir_instr_insert_node(newinstr, node))
      return false;

   list_for_each_entry_from(ppir_instr, instr, ref, &block->instr_list, list) {
      instr->seq++;
   }
   newinstr->seq = ref->seq+1;
   newinstr->scheduled = true;
   return true;
}

static bool create_new_instr_before(ppir_block *block, ppir_instr *ref,
                                    ppir_node *node)
{
   ppir_instr *newinstr = ppir_instr_create(block);
   if (unlikely(!newinstr))
      return false;

   list_del(&newinstr->list);
   list_addtail(&newinstr->list, &ref->list);

   if (!ppir_instr_insert_node(newinstr, node))
      return false;

   list_for_each_entry_from(ppir_instr, instr, ref, &block->instr_list, list) {
      instr->seq++;
   }
   newinstr->seq = ref->seq-1;
   newinstr->scheduled = true;
   return true;
}

static bool ppir_update_spilled_src(ppir_compiler *comp, ppir_block *block,
                                    ppir_node *node, ppir_src *src,
                                    ppir_node **fill_node)
{
   /* nodes might have multiple references to the same value.
    * avoid creating unnecessary loads for the same fill by
    * saving the node resulting from the temporary load */
   if (*fill_node)
      goto update_src;

   int num_components = src->reg->num_components;

   /* alloc new node to load value */
   ppir_node *load_node = ppir_node_create(block, ppir_op_load_temp, -1, 0);
   if (!load_node)
      return false;
   list_addtail(&load_node->list, &node->list);
   comp->num_fills++;

   ppir_load_node *load = ppir_node_to_load(load_node);

   load->index = -comp->prog->stack_size; /* index sizes are negative */
   load->num_components = num_components;

   ppir_dest *ld_dest = &load->dest;
   ld_dest->type = ppir_target_pipeline;
   ld_dest->pipeline = ppir_pipeline_reg_uniform;
   ld_dest->write_mask = u_bit_consecutive(0, num_components);

   /* If the uniform slot is empty, we can insert the load_temp
    * there and use it directly. Exceptionally, if the node is in the
    * varying or texld slot, this doesn't work. */
   if (!node->instr->slots[PPIR_INSTR_SLOT_UNIFORM] &&
        node->instr_pos != PPIR_INSTR_SLOT_VARYING &&
        node->instr_pos != PPIR_INSTR_SLOT_TEXLD) {
      ppir_node_target_assign(src, load_node);
      *fill_node = load_node;
      return ppir_instr_insert_node(node->instr, load_node);
   }

   /* Uniform slot was taken, so fall back to a new instruction with a mov */
   if (!create_new_instr_before(block, node->instr, load_node))
      return false;

   /* Create move node */
   ppir_node *move_node = ppir_node_create(block, ppir_op_mov, -1 , 0);
   if (unlikely(!move_node))
      return false;
   list_addtail(&move_node->list, &node->list);

   ppir_alu_node *move_alu = ppir_node_to_alu(move_node);

   move_alu->num_src = 1;
   move_alu->src->type = ppir_target_pipeline;
   move_alu->src->pipeline = ppir_pipeline_reg_uniform;
   for (int i = 0; i < 4; i++)
      move_alu->src->swizzle[i] = i;

   ppir_dest *alu_dest = &move_alu->dest;
   alu_dest->type = ppir_target_ssa;
   alu_dest->ssa.num_components = num_components;
   alu_dest->ssa.spilled = true;
   alu_dest->write_mask = u_bit_consecutive(0, num_components);

   list_addtail(&alu_dest->ssa.list, &comp->reg_list);
   comp->reg_num++;

   if (!ppir_instr_insert_node(load_node->instr, move_node))
      return false;

   /* insert the new node as predecessor */
   ppir_node_foreach_pred_safe(node, dep) {
      ppir_node *pred = dep->pred;
      ppir_node_remove_dep(dep);
      ppir_node_add_dep(load_node, pred, ppir_dep_src);
   }
   ppir_node_add_dep(node, move_node, ppir_dep_src);
   ppir_node_add_dep(move_node, load_node, ppir_dep_src);

   *fill_node = move_node;

update_src:
   /* switch node src to use the fill node dest */
   ppir_node_target_assign(src, *fill_node);

   return true;
}

static bool ppir_update_spilled_dest_load(ppir_compiler *comp, ppir_block *block,
                                          ppir_node *node)
{
   ppir_dest *dest = ppir_node_get_dest(node);
   assert(dest != NULL);
   assert(dest->type == ppir_target_register);
   ppir_reg *reg = dest->reg;
   int num_components = reg->num_components;

   /* alloc new node to load value */
   ppir_node *load_node = ppir_node_create(block, ppir_op_load_temp, -1, 0);
   if (!load_node)
      return NULL;
   list_addtail(&load_node->list, &node->list);
   comp->num_fills++;

   ppir_load_node *load = ppir_node_to_load(load_node);

   load->index = -comp->prog->stack_size; /* index sizes are negative */
   load->num_components = num_components;

   load->dest.type = ppir_target_pipeline;
   load->dest.pipeline = ppir_pipeline_reg_uniform;
   load->dest.write_mask = u_bit_consecutive(0, num_components);

   /* New instruction is needed since we're updating a dest register
    * and we can't write to the uniform pipeline reg */
   if (!create_new_instr_before(block, node->instr, load_node))
      return false;

   /* Create move node */
   ppir_node *move_node = ppir_node_create(block, ppir_op_mov, -1 , 0);
   if (unlikely(!move_node))
      return false;
   list_addtail(&move_node->list, &node->list);

   ppir_alu_node *move_alu = ppir_node_to_alu(move_node);

   move_alu->num_src = 1;
   move_alu->src->type = ppir_target_pipeline;
   move_alu->src->pipeline = ppir_pipeline_reg_uniform;
   for (int i = 0; i < 4; i++)
      move_alu->src->swizzle[i] = i;

   move_alu->dest.type = ppir_target_register;
   move_alu->dest.reg = reg;
   move_alu->dest.write_mask = u_bit_consecutive(0, num_components);

   if (!ppir_instr_insert_node(load_node->instr, move_node))
      return false;

   ppir_node_foreach_pred_safe(node, dep) {
      ppir_node *pred = dep->pred;
      ppir_node_remove_dep(dep);
      ppir_node_add_dep(load_node, pred, ppir_dep_src);
   }
   ppir_node_add_dep(node, move_node, ppir_dep_src);
   ppir_node_add_dep(move_node, load_node, ppir_dep_src);

   return true;
}

static bool ppir_update_spilled_dest(ppir_compiler *comp, ppir_block *block,
                                     ppir_node *node)
{
   ppir_dest *dest = ppir_node_get_dest(node);
   assert(dest != NULL);
   ppir_reg *reg = ppir_dest_get_reg(dest);

   /* alloc new node to store value */
   ppir_node *store_node = ppir_node_create(block, ppir_op_store_temp, -1, 0);
   if (!store_node)
      return false;
   list_addtail(&store_node->list, &node->list);
   comp->num_spills++;

   ppir_store_node *store = ppir_node_to_store(store_node);

   store->index = -comp->prog->stack_size; /* index sizes are negative */

   ppir_node_target_assign(&store->src, node);
   store->num_components = reg->num_components;

   /* insert the new node as successor */
   ppir_node_foreach_succ_safe(node, dep) {
      ppir_node *succ = dep->succ;
      ppir_node_remove_dep(dep);
      ppir_node_add_dep(succ, store_node, ppir_dep_src);
   }
   ppir_node_add_dep(store_node, node, ppir_dep_src);

   /* If the store temp slot is empty, we can insert the store_temp
    * there and use it directly. Exceptionally, if the node is in the
    * combine slot, this doesn't work. */
   if (!node->instr->slots[PPIR_INSTR_SLOT_STORE_TEMP] &&
        node->instr_pos != PPIR_INSTR_SLOT_ALU_COMBINE)
      return ppir_instr_insert_node(node->instr, store_node);

   /* Not possible to merge store, so fall back to a new instruction */
   return create_new_instr_after(block, node->instr, store_node);
}

static bool ppir_regalloc_spill_reg(ppir_compiler *comp, ppir_reg *chosen)
{
   list_for_each_entry(ppir_block, block, &comp->block_list, list) {
      list_for_each_entry(ppir_node, node, &block->node_list, list) {

         ppir_dest *dest = ppir_node_get_dest(node);
         if (dest && ppir_dest_get_reg(dest) == chosen) {
            /* If dest is a register, it might be updating only some its
             * components, so need to load the existing value first */
            if (dest->type == ppir_target_register) {
               if (!ppir_update_spilled_dest_load(comp, block, node))
                  return false;
            }
            if (!ppir_update_spilled_dest(comp, block, node))
               return false;
         }

         ppir_node *fill_node = NULL;
         /* nodes might have multiple references to the same value.
          * avoid creating unnecessary loads for the same fill by
          * saving the node resulting from the temporary load */
         for (int i = 0; i < ppir_node_get_src_num(node); i++) {
            ppir_src *src = ppir_node_get_src(node, i);
            ppir_reg *reg = ppir_src_get_reg(src);
            if (reg == chosen) {
               if (!ppir_update_spilled_src(comp, block, node, src, &fill_node))
                  return false;
            }
         }
      }
   }

   return true;
}

static ppir_reg *ppir_regalloc_choose_spill_node(ppir_compiler *comp,
                                                 struct ra_graph *g)
{
   float spill_costs[comp->reg_num];
   /* experimentally determined, it seems to be worth scaling cost of
    * regs in instructions that have used uniform/store_temp slots,
    * but not too much as to offset the num_components base cost. */
   const float slot_scale = 1.1f;

   list_for_each_entry(ppir_reg, reg, &comp->reg_list, list) {
      if (reg->spilled) {
         /* not considered for spilling */
         spill_costs[reg->regalloc_index] = 0.0f;
         continue;
      }

      /* It is beneficial to spill registers with higher component number,
       * so increase the cost of spilling registers with few components */
      float spill_cost = 4.0f / (float)reg->num_components;
      spill_costs[reg->regalloc_index] = spill_cost;
   }

   list_for_each_entry(ppir_block, block, &comp->block_list, list) {
      list_for_each_entry(ppir_instr, instr, &block->instr_list, list) {
         if (instr->slots[PPIR_INSTR_SLOT_UNIFORM]) {
            for (int i = 0; i < PPIR_INSTR_SLOT_NUM; i++) {
               ppir_node *node = instr->slots[i];
               if (!node)
                  continue;
               for (int j = 0; j < ppir_node_get_src_num(node); j++) {
                  ppir_src *src = ppir_node_get_src(node, j);
                  if (!src)
                     continue;
                  ppir_reg *reg = ppir_src_get_reg(src);
                  if (!reg)
                     continue;

                  spill_costs[reg->regalloc_index] *= slot_scale;
               }
            }
         }
         if (instr->slots[PPIR_INSTR_SLOT_STORE_TEMP]) {
            for (int i = 0; i < PPIR_INSTR_SLOT_NUM; i++) {
               ppir_node *node = instr->slots[i];
               if (!node)
                  continue;
               ppir_dest *dest = ppir_node_get_dest(node);
               if (!dest)
                  continue;
               ppir_reg *reg = ppir_dest_get_reg(dest);
               if (!reg)
                  continue;

               spill_costs[reg->regalloc_index] *= slot_scale;
            }
         }
      }
   }

   for (int i = 0; i < comp->reg_num; i++)
      ra_set_node_spill_cost(g, i, spill_costs[i]);

   int r = ra_get_best_spill_node(g);
   if (r == -1)
      return NULL;

   ppir_reg *chosen = NULL;
   int i = 0;
   list_for_each_entry(ppir_reg, reg, &comp->reg_list, list) {
      if (i++ == r) {
         chosen = reg;
         break;
      }
   }
   assert(chosen);
   chosen->spilled = true;
   chosen->is_head = true; /* store_temp unable to do swizzle */

   return chosen;
}

static void ppir_regalloc_reset_liveness_info(ppir_compiler *comp)
{
   int idx = 0;

   list_for_each_entry(ppir_reg, reg, &comp->reg_list, list) {
      reg->regalloc_index = idx++;
   }

   list_for_each_entry(ppir_block, block, &comp->block_list, list) {

      if (block->live_in)
         ralloc_free(block->live_in);
      block->live_in = rzalloc_array(comp,
            struct ppir_liveness, comp->reg_num);

      if (block->live_in_set)
         _mesa_set_destroy(block->live_in_set, NULL);
      block->live_in_set = _mesa_set_create(comp,
                                            _mesa_hash_pointer,
                                            _mesa_key_pointer_equal);

      if (block->live_out)
         ralloc_free(block->live_out);
      block->live_out = rzalloc_array(comp,
            struct ppir_liveness, comp->reg_num);

      if (block->live_out_set)
         _mesa_set_destroy(block->live_out_set, NULL);
      block->live_out_set = _mesa_set_create(comp,
                                             _mesa_hash_pointer,
                                             _mesa_key_pointer_equal);

      list_for_each_entry(ppir_instr, instr, &block->instr_list, list) {

         if (instr->live_in)
            ralloc_free(instr->live_in);
         instr->live_in = rzalloc_array(comp,
               struct ppir_liveness, comp->reg_num);

         if (instr->live_in_set)
            _mesa_set_destroy(instr->live_in_set, NULL);
         instr->live_in_set = _mesa_set_create(comp,
                                               _mesa_hash_pointer,
                                               _mesa_key_pointer_equal);

         if (instr->live_internal)
            ralloc_free(instr->live_internal);
         instr->live_internal = rzalloc_array(comp,
               struct ppir_liveness, comp->reg_num);

         if (instr->live_internal_set)
            _mesa_set_destroy(instr->live_internal_set, NULL);
         instr->live_internal_set = _mesa_set_create(comp,
                                               _mesa_hash_pointer,
                                               _mesa_key_pointer_equal);

         if (instr->live_out)
            ralloc_free(instr->live_out);
         instr->live_out = rzalloc_array(comp,
               struct ppir_liveness, comp->reg_num);

         if (instr->live_out_set)
            _mesa_set_destroy(instr->live_out_set, NULL);
         instr->live_out_set = _mesa_set_create(comp,
                                                _mesa_hash_pointer,
                                                _mesa_key_pointer_equal);
      }
   }
}

static void ppir_all_interference(ppir_compiler *comp, struct ra_graph *g,
                                  struct set *liveness)
{
   set_foreach(liveness, entry1) {
      set_foreach(liveness, entry2) {
         const struct ppir_liveness *r1 = entry1->key;
         const struct ppir_liveness *r2 = entry2->key;
         ra_add_node_interference(g, r1->reg->regalloc_index,
                                     r2->reg->regalloc_index);
      }
      _mesa_set_remove(liveness, entry1);
   }
}

int lima_ppir_force_spilling = 0;

static bool ppir_regalloc_prog_try(ppir_compiler *comp, bool *spilled)
{
   ppir_regalloc_reset_liveness_info(comp);

   struct ra_graph *g = ra_alloc_interference_graph(
      comp->ra, comp->reg_num);

   int n = 0;
   list_for_each_entry(ppir_reg, reg, &comp->reg_list, list) {
      int c = ppir_ra_reg_class_vec1 + (reg->num_components - 1);
      if (reg->is_head)
         c += 4;
      ra_set_node_class(g, n++, c);
   }

   ppir_liveness_analysis(comp);

   list_for_each_entry(ppir_block, block, &comp->block_list, list) {
      list_for_each_entry(ppir_instr, instr, &block->instr_list, list) {
         set_foreach(instr->live_internal_set, entry) {
            _mesa_set_add(instr->live_in_set, entry->key);
            _mesa_set_add(instr->live_out_set, entry->key);
         }
         ppir_all_interference(comp, g, instr->live_in_set);
         ppir_all_interference(comp, g, instr->live_out_set);
      }
   }

   *spilled = false;
   bool ok = ra_allocate(g);
   if (!ok || (comp->force_spilling-- > 0)) {
      ppir_reg *chosen = ppir_regalloc_choose_spill_node(comp, g);
      if (chosen) {
         /* stack_size will be used to assemble the frame reg in lima_draw.
          * It is also be used in the spilling code, as negative indices
          * starting from -1, to create stack addresses. */
         comp->prog->stack_size++;
         if (!ppir_regalloc_spill_reg(comp, chosen))
            goto err_out;
         /* Ask the outer loop to call back in. */
         *spilled = true;

         ppir_debug("spilled register %d/%d, num_components: %d\n",
                    chosen->regalloc_index, comp->reg_num,
                    chosen->num_components);
         goto err_out;
      }

      ppir_error("regalloc fail\n");
      goto err_out;
   }

   n = 0;
   list_for_each_entry(ppir_reg, reg, &comp->reg_list, list) {
      int reg_index = ra_get_node_reg(g, n++);
      reg->index = get_phy_reg_index(reg_index);
   }

   ralloc_free(g);

   if (lima_debug & LIMA_DEBUG_PP)
      ppir_regalloc_print_result(comp);

   return true;

err_out:
   ralloc_free(g);
   return false;
}

bool ppir_regalloc_prog(ppir_compiler *comp)
{
   bool spilled = false;
   comp->prog->stack_size = 0;

   /* Set from an environment variable to force spilling
    * for debugging purposes, see lima_screen.c */
   comp->force_spilling = lima_ppir_force_spilling;

   ppir_regalloc_update_reglist_ssa(comp);

   /* No registers? Probably shader consists of discard instruction */
   if (list_is_empty(&comp->reg_list))
      return true;

   /* this will most likely succeed in the first
    * try, except for very complicated shaders */
   while (!ppir_regalloc_prog_try(comp, &spilled))
      if (!spilled)
         return false;

   return true;
}
