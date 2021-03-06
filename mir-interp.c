/* This file is a part of MIR project.
   Copyright (C) 2018, 2019 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <alloca.h>
#include <inttypes.h>

static void util_error (const char *message);
#define MIR_VARR_ERROR util_error

#include "mir-varr.h"
#include "mir-interp.h"

static void MIR_NO_RETURN util_error (const char *message) { (*MIR_get_error_func ()) (MIR_alloc_error, message); }

#if !defined (MIR_DIRECT_DISPATCH) && defined (__GNUC__)
#define DIRECT_THREADED_DISPATCH 1
#else
#define DIRECT_THREADED_DISPATCH 0
#endif

#if defined (__GNUC__)
#define ALWAYS_INLINE inline __attribute((always_inline))
#else
#define ALWAYS_INLINE inline
#endif

typedef MIR_val_t *code_t;

typedef struct func_desc {
  MIR_reg_t fp_reg, nregs;
  size_t frame_size_in_vals;
  MIR_val_t code[1];
} *func_desc_t;

static MIR_reg_t get_reg (MIR_op_t op, MIR_reg_t *max_nreg) {
  /* We do not interpret code with hard regs */
  mir_assert (op.mode == MIR_OP_REG);
  if (*max_nreg < op.u.reg)
    *max_nreg = op.u.reg;
  return op.u.reg;
}

typedef enum {
  IC_LDI8 = MIR_INSN_BOUND, IC_LDU8, IC_LDI16, IC_LDU16, IC_LDI32, IC_LDU32, IC_LDI64,
  IC_LDF, IC_LDD,
  IC_STI8, IC_STU8, IC_STI16, IC_STU16, IC_STI32, IC_STU32, IC_STI64,
  IC_STF, IC_STD, IC_MOVI, IC_MOVP, IC_MOVF, IC_MOVD,
  IC_INSN_BOUND
} MIR_full_insn_code_t;

#if DIRECT_THREADED_DISPATCH
static void **dispatch_label_tab;
#endif

static MIR_val_t get_icode (MIR_full_insn_code_t code) {
  MIR_val_t v;

#if DIRECT_THREADED_DISPATCH
  v.a = dispatch_label_tab[code];
#else
  v.ic = code;
#endif
  return v;
}

static MIR_full_insn_code_t get_int_mem_insn_code (int load_p, MIR_type_t t) {
  switch (t) {
  case MIR_T_I8: return load_p ? IC_LDI8 : IC_STI8;
  case MIR_T_U8: return load_p ? IC_LDU8 : IC_STU8;
  case MIR_T_I16: return load_p ? IC_LDI16 : IC_STI16;
  case MIR_T_U16: return load_p ? IC_LDU16 : IC_STU16;
  case MIR_T_I32: return load_p ? IC_LDI32 : IC_STI32;
  case MIR_T_U32: return load_p ? IC_LDU32 : IC_STU32;
  case MIR_T_I64: return load_p ? IC_LDI64 : IC_STI64;
    // ??? pointer
  default: mir_assert (FALSE);
  }
}

DEF_VARR (MIR_val_t);
static VARR (MIR_val_t) *code_varr;

DEF_VARR (MIR_insn_t);
static VARR (MIR_insn_t) *branches;

static void push_mem (MIR_op_t op) {
  MIR_val_t v;
  
  mir_assert (op.mode == MIR_OP_MEM && op.u.mem.disp == 0
	      && op.u.mem.index == 0 && op.u.mem.scale == 0);
  v.i = op.u.mem.base; VARR_PUSH (MIR_val_t, code_varr, v);
}

static void redirect_interface_to_interp (MIR_item_t func_item);

static void generate_icode (MIR_item_t func_item) {
  MIR_func_t func = func_item->u.func;
  MIR_insn_t insn, label;
  MIR_val_t v;
  size_t i;
  MIR_reg_t max_nreg = 0;
  func_desc_t func_desc;
  
  VARR_TRUNC (MIR_insn_t, branches, 0);
  VARR_TRUNC (MIR_val_t, code_varr, 0);
  for (insn = DLIST_HEAD (MIR_insn_t, func->insns); insn != NULL; insn = DLIST_NEXT (MIR_insn_t, insn)) {
    MIR_insn_code_t code = insn->code;
    size_t nops = MIR_insn_nops (insn);
    MIR_op_t *ops = insn->ops;
        
    insn->data = (void *) VARR_LENGTH (MIR_val_t, code_varr);
    switch (code) {
    case MIR_MOV: /* loads, imm moves */
      if (ops[0].mode == MIR_OP_MEM) {
	v = get_icode (get_int_mem_insn_code (FALSE, ops[0].u.mem.type)); VARR_PUSH (MIR_val_t, code_varr, v);
	v.i = get_reg (ops[1], &max_nreg); VARR_PUSH (MIR_val_t, code_varr, v);
	push_mem (ops[0]);
      } else if (ops[1].mode == MIR_OP_MEM) {
	v = get_icode (get_int_mem_insn_code (TRUE, ops[1].u.mem.type)); VARR_PUSH (MIR_val_t, code_varr, v);
	v.i = get_reg (ops[0], &max_nreg); VARR_PUSH (MIR_val_t, code_varr, v);
	push_mem (ops[1]);
      } else if (ops[1].mode == MIR_OP_INT) {
	v = get_icode (IC_MOVI); VARR_PUSH (MIR_val_t, code_varr, v);
	v.i = get_reg (ops[0], &max_nreg); VARR_PUSH (MIR_val_t, code_varr, v);
	v.i = ops[1].u.i; VARR_PUSH (MIR_val_t, code_varr, v);
      } else if (ops[1].mode == MIR_OP_REF) {
	MIR_item_t item = ops[1].u.ref;
	
	if (item->item_type == MIR_import_item && item->ref_def != NULL && item->ref_def != NULL
	    && _MIR_get_thunk_func (item->ref_def->addr) == _MIR_undefined_interface) {
	  mir_assert (item->ref_def->item_type == MIR_func_item);
	  redirect_interface_to_interp (item->ref_def);
	  item->addr = item->ref_def->addr;
	}
	v = get_icode (IC_MOVP); VARR_PUSH (MIR_val_t, code_varr, v);
	v.i = get_reg (ops[0], &max_nreg); VARR_PUSH (MIR_val_t, code_varr, v);
	v.a = item->addr; VARR_PUSH (MIR_val_t, code_varr, v);
      } else {
	mir_assert (ops[1].mode == MIR_OP_REG);
	v = get_icode (code); VARR_PUSH (MIR_val_t, code_varr, v);
	v.i = get_reg (ops[0], &max_nreg); VARR_PUSH (MIR_val_t, code_varr, v);
	v.i = ops[1].u.reg; VARR_PUSH (MIR_val_t, code_varr, v);
      }
      break;
    case MIR_FMOV:
      if (ops[0].mode == MIR_OP_MEM) {
	v = get_icode (IC_STF); VARR_PUSH (MIR_val_t, code_varr, v);
	v.i = get_reg (ops[1], &max_nreg); VARR_PUSH (MIR_val_t, code_varr, v);
	push_mem (ops[0]);
      } else if (ops[1].mode == MIR_OP_MEM) {
	v = get_icode (IC_LDF); VARR_PUSH (MIR_val_t, code_varr, v);
	v.i = get_reg (ops[0], &max_nreg); VARR_PUSH (MIR_val_t, code_varr, v);
	push_mem (ops[1]);
      } else if (ops[1].mode == MIR_OP_FLOAT) {
	v = get_icode (IC_MOVF); VARR_PUSH (MIR_val_t, code_varr, v);
	v.i = get_reg (ops[0], &max_nreg); VARR_PUSH (MIR_val_t, code_varr, v);
	v.i = ops[1].u.f; VARR_PUSH (MIR_val_t, code_varr, v);
      } else {
	mir_assert (ops[1].mode == MIR_OP_REG);
	v = get_icode (code); VARR_PUSH (MIR_val_t, code_varr, v);
	v.i = get_reg (ops[0], &max_nreg); VARR_PUSH (MIR_val_t, code_varr, v);
	v.i = ops[1].u.reg; VARR_PUSH (MIR_val_t, code_varr, v);
      }
      break;
    case MIR_DMOV:
      if (ops[0].mode == MIR_OP_MEM) {
	v = get_icode (IC_STD); VARR_PUSH (MIR_val_t, code_varr, v);
	v.i = get_reg (ops[1], &max_nreg); VARR_PUSH (MIR_val_t, code_varr, v);
	push_mem (ops[0]);
      } else if (ops[1].mode == MIR_OP_MEM) {
	v = get_icode (IC_LDD); VARR_PUSH (MIR_val_t, code_varr, v);
	v.i = get_reg (ops[0], &max_nreg); VARR_PUSH (MIR_val_t, code_varr, v);
	push_mem (ops[1]);
      } else if (ops[1].mode == MIR_OP_DOUBLE) {
	v = get_icode (IC_MOVD); VARR_PUSH (MIR_val_t, code_varr, v);
	v.i = get_reg (ops[0], &max_nreg); VARR_PUSH (MIR_val_t, code_varr, v);
	v.i = ops[1].u.d; VARR_PUSH (MIR_val_t, code_varr, v);
      } else {
	mir_assert (ops[1].mode == MIR_OP_REG);
	v = get_icode (code); VARR_PUSH (MIR_val_t, code_varr, v);
	v.i = get_reg (ops[0], &max_nreg); VARR_PUSH (MIR_val_t, code_varr, v);
	v.i = ops[1].u.reg; VARR_PUSH (MIR_val_t, code_varr, v);
      }
      break;
    case MIR_LABEL:
      break;
    case MIR_INVALID_INSN:
      (*MIR_get_error_func ()) (MIR_invalid_insn_error, "invalid insn for interpreter");
      break;
    case MIR_JMP:
      VARR_PUSH (MIR_insn_t, branches, insn);
      v = get_icode (code); VARR_PUSH (MIR_val_t, code_varr, v);
      v.i = 0; VARR_PUSH (MIR_val_t, code_varr, v);
      break;
    case MIR_BT: case MIR_BTS: case MIR_BF: case MIR_BFS:
      VARR_PUSH (MIR_insn_t, branches, insn);
      v = get_icode (code); VARR_PUSH (MIR_val_t, code_varr, v);
      v.i = 0; VARR_PUSH (MIR_val_t, code_varr, v);
      v.i = get_reg (ops[1], &max_nreg); VARR_PUSH (MIR_val_t, code_varr, v);
      break;
    case MIR_BEQ: case MIR_FBEQ: case MIR_DBEQ: case MIR_BNE: case MIR_FBNE: case MIR_DBNE:
    case MIR_BLT: case MIR_UBLT: case MIR_FBLT: case MIR_DBLT:
    case MIR_BLE: case MIR_UBLE: case MIR_FBLE: case MIR_DBLE:
    case MIR_BGT: case MIR_UBGT: case MIR_FBGT: case MIR_DBGT:
    case MIR_BGE: case MIR_UBGE: case MIR_FBGE: case MIR_DBGE:
      VARR_PUSH (MIR_insn_t, branches, insn);
      v = get_icode (code); VARR_PUSH (MIR_val_t, code_varr, v);
      v.i = 0; VARR_PUSH (MIR_val_t, code_varr, v);
      v.i = get_reg (ops[1], &max_nreg); VARR_PUSH (MIR_val_t, code_varr, v);
      v.i = get_reg (ops[2], &max_nreg); VARR_PUSH (MIR_val_t, code_varr, v);
      break;
    default:
      v = get_icode (code); VARR_PUSH (MIR_val_t, code_varr, v);
      if (code == MIR_CALL) {
	v.i = nops; VARR_PUSH (MIR_val_t, code_varr, v);
      }
      for (i = 0; i < nops; i++) {
	if (code == MIR_CALL && i == 0) { /* prototype ??? */
	  mir_assert (ops[i].mode == MIR_OP_REF && ops[i].u.ref->item_type == MIR_proto_item);
	  v.a = ops[i].u.ref->u.proto;
	} else {
	  mir_assert (ops[i].mode == MIR_OP_REG);
	  v.i = get_reg (ops[i], &max_nreg);
	}
	VARR_PUSH (MIR_val_t, code_varr, v);
      }
    }
  }
  for (i = 0; i < VARR_LENGTH (MIR_insn_t, branches); i++) {
    insn = VARR_GET (MIR_insn_t, branches, i);
    label = insn->ops[0].u.label;
    v.i = (size_t) label->data;
    VARR_SET (MIR_val_t, code_varr, (size_t) insn->data + 1, v);
  }
  func_item->data = func_desc = malloc (sizeof (struct func_desc)
					+ VARR_LENGTH (MIR_val_t, code_varr) * sizeof (MIR_val_t));
  if (func_desc == NULL)
    (*MIR_get_error_func ()) (MIR_alloc_error, "no memory");
  memmove (func_desc->code, VARR_ADDR (MIR_val_t, code_varr), VARR_LENGTH (MIR_val_t, code_varr) * sizeof (MIR_val_t));
  mir_assert (max_nreg < MIR_MAX_REG_NUM);
  func_desc->nregs = max_nreg + 1;
  func_desc->frame_size_in_vals = (func->frame_size + sizeof (MIR_val_t) - 1) / sizeof (MIR_val_t);
  func_desc->fp_reg = MIR_reg (FP_NAME, func);
}

static ALWAYS_INLINE void *get_a (MIR_val_t *v) { return v->a;}
static ALWAYS_INLINE int64_t get_i (MIR_val_t *v) { return v->i;}
static ALWAYS_INLINE float get_f (MIR_val_t *v) { return v->f;}
static ALWAYS_INLINE double get_d (MIR_val_t *v) { return v->d;}

static ALWAYS_INLINE void **get_aop (MIR_val_t *bp, code_t c) { return &bp [get_i (c)].a; }
static ALWAYS_INLINE int64_t *get_iop (MIR_val_t *bp, code_t c) { return &bp [get_i (c)].i; }
static ALWAYS_INLINE uint64_t *get_uop (MIR_val_t *bp, code_t c) { return &bp [get_i (c)].u; }
static ALWAYS_INLINE float *get_fop (MIR_val_t *bp, code_t c) { return &bp [get_i (c)].f; }
static ALWAYS_INLINE double *get_dop (MIR_val_t *bp, code_t c) { return &bp [get_i (c)].d; }

static ALWAYS_INLINE int64_t *get_2iops (MIR_val_t *bp, code_t c, int64_t *p) {
  *p = *get_iop (bp, c + 1); return get_iop (bp, c);
}

static ALWAYS_INLINE int64_t *get_2isops (MIR_val_t *bp, code_t c, int32_t *p) {
  *p = *get_iop (bp, c + 1); return get_iop (bp, c);
}

static ALWAYS_INLINE int64_t *get_3iops (MIR_val_t *bp, code_t c, int64_t *p1, int64_t *p2) {
  *p1 = *get_iop (bp, c + 1); *p2 = *get_iop (bp, c + 2); return get_iop (bp, c);
}

static ALWAYS_INLINE int64_t *get_3isops (MIR_val_t *bp, code_t c, int32_t *p1, int32_t *p2) {
  *p1 = *get_iop (bp, c + 1); *p2 = *get_iop (bp, c + 2); return get_iop (bp, c);
}

static ALWAYS_INLINE uint64_t *get_3uops (MIR_val_t *bp, code_t c, uint64_t *p1, uint64_t *p2) {
  *p1 = *get_uop (bp, c + 1); *p2 = *get_uop (bp, c + 2); return get_uop (bp, c);
}

static ALWAYS_INLINE uint64_t *get_3usops (MIR_val_t *bp, code_t c, uint32_t *p1, uint32_t *p2) {
  *p1 = *get_uop (bp, c + 1); *p2 = *get_uop (bp, c + 2); return get_uop (bp, c);
}

static ALWAYS_INLINE float *get_2fops (MIR_val_t *bp, code_t c, float *p) {
  *p = *get_fop (bp, c + 1); return get_fop (bp, c);
}

static ALWAYS_INLINE float *get_3fops (MIR_val_t *bp, code_t c, float *p1, float *p2) {
  *p1 = *get_fop (bp, c + 1); *p2 = *get_fop (bp, c + 2); return get_fop (bp, c);
}

static ALWAYS_INLINE int64_t *get_fcmp_ops (MIR_val_t *bp, code_t c, float *p1, float *p2) {
  *p1 = *get_fop (bp, c + 1); *p2 = *get_fop (bp, c + 2); return get_iop (bp, c);
}

static ALWAYS_INLINE double *get_2dops (MIR_val_t *bp, code_t c, double *p) {
  *p = *get_dop (bp, c + 1); return get_dop (bp, c);
}

static ALWAYS_INLINE double *get_3dops (MIR_val_t *bp, code_t c, double *p1, double *p2) {
  *p1 = *get_dop (bp, c + 1); *p2 = *get_dop (bp, c + 2); return get_dop (bp, c);
}

static ALWAYS_INLINE int64_t *get_dcmp_ops (MIR_val_t *bp, code_t c, double *p1, double *p2) {
  *p1 = *get_dop (bp, c + 1); *p2 = *get_dop (bp, c + 2); return get_iop (bp, c);
}

static ALWAYS_INLINE int64_t get_mem_addr (MIR_val_t *bp, code_t c) { return bp [get_i (c)].i; }

#define EXT(tp) do {int64_t *r = get_iop (bp, ops); tp s = *get_iop (bp, ops + 1); *r = s;} while (0)
#define IOP2(op) do {int64_t *r, p; r = get_2iops (bp, ops, &p); *r = op p;} while (0)
#define IOP2S(op) do {int64_t *r; int32_t p; r = get_2isops (bp, ops, &p); *r = op p;} while (0)
#define IOP3(op) do {int64_t *r, p1, p2; r = get_3iops (bp, ops, &p1, &p2); *r = p1 op p2; } while (0)
#define IOP3S(op) do {int64_t *r; int32_t p1, p2; r = get_3isops (bp, ops, &p1, &p2); *r = p1 op p2; } while (0)
#define ICMP(op) do {int64_t *r, p1, p2; r = get_3iops (bp, ops, &p1, &p2); *r = p1 op p2; } while (0)
#define ICMPS(op) do {int64_t *r; int32_t p1, p2; r = get_3isops (bp, ops, &p1, &p2); *r = p1 op p2; } while (0)
#define BICMP(op) do {int64_t op1 = *get_iop (bp, ops + 1), op2 = *get_iop (bp, ops + 2); if (op1 op op2) pc = code + get_i (ops); } while (0)
#define BICMPS(op) do {int32_t op1 = *get_iop (bp, ops + 1), op2 = *get_iop (bp, ops + 2); if (op1 op op2) pc = code + get_i (ops); } while (0)
#define UOP3(op) do {uint64_t *r, p1, p2; r = get_3uops (bp, ops, &p1, &p2); *r = p1 op p2; } while (0)
#define UOP3S(op) do {uint64_t *r; uint32_t p1, p2; r = get_3usops (bp, ops, &p1, &p2); *r = p1 op p2; } while (0)
#define UIOP3(op) do {uint64_t *r, p1, p2; r = get_3uops (bp, ops, &p1, &p2); *r = p1 op p2; } while (0)
#define UIOP3S(op) do {uint64_t *r; uint32_t p1, p2; r = get_3usops (bp, ops, &p1, &p2); *r = p1 op p2; } while (0)
#define UCMP(op) do {uint64_t *r, p1, p2; r = get_3uops (bp, ops, &p1, &p2); *r = p1 op p2; } while (0)
#define UCMPS(op) do {uint64_t *r; uint32_t p1, p2; r = get_3usops (bp, ops, &p1, &p2); *r = p1 op p2; } while (0)
#define BUCMP(op) do {uint64_t op1 = *get_uop (bp, ops + 1), op2 = *get_uop (bp, ops + 2); if (op1 op op2) pc = code + get_i (ops); } while (0)
#define BUCMPS(op) do {uint32_t op1 = *get_uop (bp, ops + 1), op2 = *get_uop (bp, ops + 2); if (op1 op op2) pc = code + get_i (ops); } while (0)

#define FOP2(op) do {float *r, p; r = get_2fops (bp, ops, &p); *r = op p;} while (0)
#define FOP3(op) do {float *r, p1, p2; r = get_3fops (bp, ops, &p1, &p2); *r = p1 op p2; } while (0)
#define FCMP(op) do {int64_t *r; float p1, p2; r = get_fcmp_ops (bp, ops, &p1, &p2); *r = p1 op p2; } while (0)
#define BFCMP(op) do {float op1 = *get_fop (bp, ops + 1), op2 = *get_fop (bp, ops + 2); if (op1 op op2) pc = code + get_i (ops); } while (0)

#define DOP2(op) do {double *r, p; r = get_2dops (bp, ops, &p); *r = op p;} while (0)
#define DOP3(op) do {double *r, p1, p2; r = get_3dops (bp, ops, &p1, &p2); *r = p1 op p2; } while (0)
#define DCMP(op) do {int64_t *r; double p1, p2; r = get_dcmp_ops (bp, ops, &p1, &p2); *r = p1 op p2; } while (0)
#define BDCMP(op) do {double op1 = *get_dop (bp, ops + 1), op2 = *get_dop (bp, ops + 2); if (op1 op op2) pc = code + get_i (ops); } while (0)

#define LD(op, val_type, mem_type) do {					       \
    val_type *r = get_## op (bp, ops); int64_t a = get_mem_addr (bp, ops + 1); \
    *r = *((mem_type *) a);                                                    \
  } while (0)
#define ST(op, val_type, mem_type) do {					       \
    val_type v = *get_## op (bp, ops); int64_t a = get_mem_addr (bp, ops + 1); \
    *((mem_type *) a) = v;                                                     \
  } while (0)

#ifdef __GNUC__
#define OPTIMIZE __attribute__((__optimize__ ("O3")))
#else
#define OPTIMIZE
#endif

static VARR (MIR_val_t) *args_varr;

static MIR_val_t *args;
 
static void call (MIR_proto_t proto, void *addr, MIR_val_t *res, size_t nargs, MIR_val_t *args);

static MIR_val_t OPTIMIZE eval (code_t code, MIR_val_t *bp) {
  code_t pc, ops;
  
#define START_INSN(nops) do {ops = pc + 1; pc += nops + 1;} while (0)
#if DIRECT_THREADED_DISPATCH
  static void *ltab [IC_INSN_BOUND];

  if (bp == NULL)
    {
      MIR_val_t v;
      
      ltab [MIR_MOV] = &&L_MIR_MOV; ltab [MIR_FMOV] = &&L_MIR_FMOV; ltab [MIR_DMOV] = &&L_MIR_DMOV;
      ltab [MIR_EXT8] = &&L_MIR_EXT8; ltab [MIR_EXT16] = &&L_MIR_EXT16;
      ltab [MIR_EXT32] = &&L_MIR_EXT32;
      ltab [MIR_UEXT8] = &&L_MIR_UEXT8; ltab [MIR_UEXT16] = &&L_MIR_UEXT16;
      ltab [MIR_UEXT32] = &&L_MIR_UEXT32;
      ltab [MIR_I2F] = &&L_MIR_I2F; ltab [MIR_I2D] = &&L_MIR_I2D;
      ltab [MIR_F2I] = &&L_MIR_F2I; ltab [MIR_D2I] = &&L_MIR_D2I;
      ltab [MIR_F2D] = &&L_MIR_F2D; ltab [MIR_D2F] = &&L_MIR_D2F;
      ltab [MIR_NEG] = &&L_MIR_NEG; ltab [MIR_NEGS] = &&L_MIR_NEGS;
      ltab [MIR_FNEG] = &&L_MIR_FNEG; ltab [MIR_DNEG] = &&L_MIR_DNEG;
      ltab [MIR_ADD] = &&L_MIR_ADD; ltab [MIR_ADDS] = &&L_MIR_ADDS;
      ltab [MIR_FADD] = &&L_MIR_FADD; ltab [MIR_DADD] = &&L_MIR_DADD;
      ltab [MIR_SUB] = &&L_MIR_SUB; ltab [MIR_SUBS] = &&L_MIR_SUBS;
      ltab [MIR_FSUB] = &&L_MIR_FSUB; ltab [MIR_DSUB] = &&L_MIR_DSUB;
      ltab [MIR_MUL] = &&L_MIR_MUL; ltab [MIR_UMUL] = &&L_MIR_UMUL;
      ltab [MIR_MULS] = &&L_MIR_MULS; ltab [MIR_UMULS] = &&L_MIR_UMULS;
      ltab [MIR_FMUL] = &&L_MIR_FMUL; ltab [MIR_DMUL] = &&L_MIR_DMUL;
      ltab [MIR_DIV] = &&L_MIR_DIV; ltab [MIR_DIVS] = &&L_MIR_DIVS;
      ltab [MIR_UDIV] = &&L_MIR_UDIV; ltab [MIR_UDIVS] = &&L_MIR_UDIVS;
      ltab [MIR_FDIV] = &&L_MIR_FDIV; ltab [MIR_DDIV] = &&L_MIR_DDIV;
      ltab [MIR_MOD] = &&L_MIR_MOD; ltab [MIR_MODS] = &&L_MIR_MODS;
      ltab [MIR_UMOD] = &&L_MIR_UMOD; ltab [MIR_UMODS] = &&L_MIR_UMODS;
      ltab [MIR_AND] = &&L_MIR_AND; ltab [MIR_ANDS] = &&L_MIR_ANDS;
      ltab [MIR_OR] = &&L_MIR_OR; ltab [MIR_ORS] = &&L_MIR_ORS;
      ltab [MIR_XOR] = &&L_MIR_XOR; ltab [MIR_XORS] = &&L_MIR_XORS;
      ltab [MIR_LSH] = &&L_MIR_LSH; ltab [MIR_LSHS] = &&L_MIR_LSHS;
      ltab [MIR_RSH] = &&L_MIR_RSH; ltab [MIR_RSHS] = &&L_MIR_RSHS;
      ltab [MIR_URSH] = &&L_MIR_URSH;  ltab [MIR_URSHS] = &&L_MIR_URSHS;
      ltab [MIR_EQ] = &&L_MIR_EQ; ltab [MIR_EQS] = &&L_MIR_EQS;
      ltab [MIR_FEQ] = &&L_MIR_FEQ; ltab [MIR_DEQ] = &&L_MIR_DEQ;
      ltab [MIR_NE] = &&L_MIR_NE; ltab [MIR_NES] = &&L_MIR_NES;
      ltab [MIR_FNE] = &&L_MIR_FNE; ltab [MIR_DNE] = &&L_MIR_DNE;
      ltab [MIR_LT] = &&L_MIR_LT; ltab [MIR_LTS] = &&L_MIR_LTS;
      ltab [MIR_ULT] = &&L_MIR_ULT; ltab [MIR_ULTS] = &&L_MIR_ULTS;
      ltab [MIR_FLT] = &&L_MIR_FLT; ltab [MIR_DLT] = &&L_MIR_DLT;
      ltab [MIR_LE] = &&L_MIR_LE; ltab [MIR_LES] = &&L_MIR_LES;
      ltab [MIR_ULE] = &&L_MIR_ULE;  ltab [MIR_ULES] = &&L_MIR_ULES;
      ltab [MIR_FLE] = &&L_MIR_FLE; ltab [MIR_DLE] = &&L_MIR_DLE;
      ltab [MIR_GT] = &&L_MIR_GT; ltab [MIR_GTS] = &&L_MIR_GTS;
      ltab [MIR_UGT] = &&L_MIR_UGT; ltab [MIR_UGTS] = &&L_MIR_UGTS;
      ltab [MIR_FGT] = &&L_MIR_FGT; ltab [MIR_DGT] = &&L_MIR_DGT;
      ltab [MIR_GE] = &&L_MIR_GE; ltab [MIR_GES] = &&L_MIR_GES;
      ltab [MIR_UGE] = &&L_MIR_UGE;  ltab [MIR_UGES] = &&L_MIR_UGES;
      ltab [MIR_FGE] = &&L_MIR_FGE; ltab [MIR_DGE] = &&L_MIR_DGE;
      ltab [MIR_JMP] = &&L_MIR_JMP;
      ltab [MIR_BT] = &&L_MIR_BT; ltab [MIR_BTS] = &&L_MIR_BTS;
      ltab [MIR_BFS] = &&L_MIR_BF; ltab [MIR_BFS] = &&L_MIR_BF;
      ltab [MIR_BEQ] = &&L_MIR_BEQ; ltab [MIR_BEQS] = &&L_MIR_BEQS;
      ltab [MIR_FBEQ] = &&L_MIR_FBEQ; ltab [MIR_DBEQ] = &&L_MIR_DBEQ;
      ltab [MIR_BNE] = &&L_MIR_BNE; ltab [MIR_BNES] = &&L_MIR_BNES;
      ltab [MIR_FBNE] = &&L_MIR_FBNE; ltab [MIR_DBNE] = &&L_MIR_DBNE;
      ltab [MIR_BLT] = &&L_MIR_BLT; ltab [MIR_BLTS] = &&L_MIR_BLTS;
      ltab [MIR_UBLT] = &&L_MIR_UBLT;  ltab [MIR_UBLTS] = &&L_MIR_UBLTS;
      ltab [MIR_FBLT] = &&L_MIR_FBLT; ltab [MIR_DBLT] = &&L_MIR_DBLT;
      ltab [MIR_BLE] = &&L_MIR_BLE; ltab [MIR_BLES] = &&L_MIR_BLES;
      ltab [MIR_UBLE] = &&L_MIR_UBLE;  ltab [MIR_UBLES] = &&L_MIR_UBLES;
      ltab [MIR_FBLE] = &&L_MIR_FBLE; ltab [MIR_DBLE] = &&L_MIR_DBLE;
      ltab [MIR_BGT] = &&L_MIR_BGT; ltab [MIR_BGTS] = &&L_MIR_BGTS;
      ltab [MIR_UBGT] = &&L_MIR_UBGT; ltab [MIR_UBGTS] = &&L_MIR_UBGTS;
      ltab [MIR_FBGT] = &&L_MIR_FBGT; ltab [MIR_DBGT] = &&L_MIR_DBGT;
      ltab [MIR_BGE] = &&L_MIR_BGE; ltab [MIR_BGES] = &&L_MIR_BGES;
      ltab [MIR_UBGE] = &&L_MIR_UBGE; ltab [MIR_UBGES] = &&L_MIR_UBGES;
      ltab [MIR_FBGE] = &&L_MIR_FBGE; ltab [MIR_DBGE] = &&L_MIR_DBGE;
      ltab [MIR_CALL] = &&L_MIR_CALL;
      ltab [MIR_RET] = &&L_MIR_RET; ltab [MIR_FRET] = &&L_MIR_FRET; ltab [MIR_DRET] = &&L_MIR_DRET;
      ltab [IC_LDI8] = &&L_IC_LDI8; ltab [IC_LDU8] = &&L_IC_LDU8;
      ltab [IC_LDI16] = &&L_IC_LDI16; ltab [IC_LDU16] = &&L_IC_LDU16;
      ltab [IC_LDI32] = &&L_IC_LDI32; ltab [IC_LDU32] = &&L_IC_LDU32;
      ltab [IC_LDI64] = &&L_IC_LDI64;
      ltab [IC_LDF] = &&L_IC_LDF; ltab [IC_LDD] = &&L_IC_LDD;
      ltab [IC_STI8] = &&L_IC_STI8; ltab [IC_STU8] = &&L_IC_STU8;
      ltab [IC_STI16] = &&L_IC_STI16; ltab [IC_STU16] = &&L_IC_STU16;
      ltab [IC_STI32] = &&L_IC_STI32; ltab [IC_STU32] = &&L_IC_STU32;
      ltab [IC_STI64] = &&L_IC_STI64;
      ltab [IC_STF] = &&L_IC_STF; ltab [IC_STD] = &&L_IC_STD;
      ltab [IC_MOVI] = &&L_IC_MOVI; ltab [IC_MOVP] = &&L_IC_MOVP;
      ltab [IC_MOVF] = &&L_IC_MOVF; ltab [IC_MOVD] = &&L_IC_MOVD;
      v.a = ltab; return v;
    }
  
#define CASE(value, nops) L_ ## value: START_INSN(nops)
#define END_INSN goto *pc->a

#else

#define CASE(value, nops) case value: START_INSN(nops)
#define END_INSN break
#endif
  pc = code;

#if DIRECT_THREADED_DISPATCH
  goto *pc->a;
#else
  for (;;) {
    MIR_insn_code_t insn_code = pc->ic;
    switch (insn_code) {
#endif
      
      CASE (MIR_MOV, 2);  {int64_t p, *r = get_2iops (bp, ops, &p); *r = p;} END_INSN;
      CASE (MIR_FMOV, 2); {float p, *r = get_2fops (bp, ops, &p); *r = p;} END_INSN;
      CASE (MIR_DMOV, 2); {double p, *r = get_2dops (bp, ops, &p); *r = p;} END_INSN;
      CASE (MIR_EXT8, 2); EXT (int8_t); END_INSN;
      CASE (MIR_EXT16, 2); EXT (int16_t); END_INSN;
      CASE (MIR_EXT32, 2); EXT (int32_t); END_INSN;
      CASE (MIR_UEXT8, 2); EXT (uint8_t); END_INSN;
      CASE (MIR_UEXT16, 2); EXT (uint16_t); END_INSN;
      CASE (MIR_UEXT32, 2); EXT (uint32_t); END_INSN;
      CASE (MIR_I2F, 2);  {float *r = get_fop (bp, ops); int64_t i = *get_iop (bp, ops + 1); *r = i;} END_INSN;
      CASE (MIR_I2D, 2);  {double *r = get_dop (bp, ops); int64_t i = *get_iop (bp, ops + 1); *r = i;} END_INSN;
      CASE (MIR_F2I, 2);  {int64_t *r = get_iop (bp, ops); float f = *get_fop (bp, ops + 1); *r = f;} END_INSN;
      CASE (MIR_D2I, 2);  {int64_t *r = get_iop (bp, ops); double d = *get_dop (bp, ops + 1); *r = d;} END_INSN;
      CASE (MIR_F2D, 2);  {double *r = get_dop (bp, ops); float f = *get_fop (bp, ops + 1); *r = f;} END_INSN;
      CASE (MIR_D2F, 2);  {float *r = get_fop (bp, ops); double d = *get_dop (bp, ops + 1); *r = d;} END_INSN;
      
      CASE (MIR_NEG, 2);  IOP2(-); END_INSN;
      CASE (MIR_NEGS, 2); IOP2S(-); END_INSN;
      CASE (MIR_FNEG, 2); FOP2(-); END_INSN;
      CASE (MIR_DNEG, 2); DOP2(-); END_INSN;
       
      CASE (MIR_ADD, 3);  IOP3(+); END_INSN;
      CASE (MIR_ADDS, 3); IOP3S(+); END_INSN;
      CASE (MIR_FADD, 3); FOP3(+); END_INSN;
      CASE (MIR_DADD, 3); DOP3(+); END_INSN;
      
      CASE (MIR_SUB, 3);  IOP3(-); END_INSN; 
      CASE (MIR_SUBS, 3); IOP3S(-); END_INSN; 
      CASE (MIR_FSUB, 3); FOP3(-); END_INSN;
      CASE (MIR_DSUB, 3); DOP3(-); END_INSN;
      
      CASE (MIR_MUL, 3);   IOP3(*); END_INSN;
      CASE (MIR_MULS, 3);  IOP3S(*); END_INSN;
      CASE (MIR_UMUL, 3);  UOP3(*); END_INSN;
      CASE (MIR_UMULS, 3); UOP3S(*); END_INSN;
      CASE (MIR_FMUL, 3);  FOP3(*); END_INSN;
      CASE (MIR_DMUL, 3);  DOP3(*); END_INSN;
      
      CASE (MIR_DIV, 3);   IOP3(/); END_INSN;
      CASE (MIR_DIVS, 3);  IOP3S(/); END_INSN;
      CASE (MIR_UDIV, 3);  UOP3(/); END_INSN;
      CASE (MIR_UDIVS, 3); UOP3S(/); END_INSN;
      CASE (MIR_FDIV, 3);  FOP3(/); END_INSN;
      CASE (MIR_DDIV, 3);  DOP3(/); END_INSN;
      
      CASE (MIR_MOD, 3);   IOP3(%); END_INSN;
      CASE (MIR_MODS, 3);  IOP3S(%); END_INSN;
      CASE (MIR_UMOD, 3);  UOP3(%); END_INSN;
      CASE (MIR_UMODS, 3); IOP3S(%); END_INSN;
      
      CASE (MIR_AND, 3);  IOP3(&); END_INSN;
      CASE (MIR_ANDS, 3); IOP3S(&); END_INSN;
      CASE (MIR_OR, 3);   IOP3(|); END_INSN;
      CASE (MIR_ORS, 3);  IOP3S(|); END_INSN;
      CASE (MIR_XOR, 3);  IOP3(^); END_INSN;
      CASE (MIR_XORS, 3); IOP3S(^); END_INSN;
      CASE (MIR_LSH, 3);  IOP3(<<); END_INSN;
      CASE (MIR_LSHS, 3); IOP3S(<<); END_INSN;
      
      CASE (MIR_RSH, 3);   IOP3(>>); END_INSN;
      CASE (MIR_RSHS, 3);  IOP3S(>>); END_INSN;
      CASE (MIR_URSH, 3);  UIOP3(>>); END_INSN;
      CASE (MIR_URSHS, 3); UIOP3S(>>); END_INSN;
      
      CASE (MIR_EQ, 3);  ICMP(=); END_INSN;
      CASE (MIR_EQS, 3); ICMPS(=); END_INSN;
      CASE (MIR_FEQ, 3); FCMP(=); END_INSN; 
      CASE (MIR_DEQ, 3); DCMP(=); END_INSN;
      
      CASE (MIR_NE, 3);  ICMP(!=); END_INSN;
      CASE (MIR_NES, 3); ICMPS(!=); END_INSN;
      CASE (MIR_FNE, 3); FCMP(!=); END_INSN;
      CASE (MIR_DNE, 3); DCMP(!=); END_INSN;
      
      CASE (MIR_LT, 3);   ICMP(<); END_INSN;
      CASE (MIR_LTS, 3);  ICMPS(<); END_INSN;
      CASE (MIR_ULT, 3);  UCMP(<); END_INSN;
      CASE (MIR_ULTS, 3); UCMPS(<); END_INSN;
      CASE (MIR_FLT, 3);  FCMP(<); END_INSN;
      CASE (MIR_DLT, 3);  DCMP(<); END_INSN;
      
      CASE (MIR_LE, 3);   ICMP(<=); END_INSN;
      CASE (MIR_LES, 3);  ICMPS(<=); END_INSN;
      CASE (MIR_ULE, 3);  UCMP(<=); END_INSN;
      CASE (MIR_ULES, 3); UCMPS(<=); END_INSN;
      CASE (MIR_FLE, 3);  FCMP(<=); END_INSN;
      CASE (MIR_DLE, 3);  DCMP(<=); END_INSN;
      
      CASE (MIR_GT, 3);   ICMP(>); END_INSN;
      CASE (MIR_GTS, 3);  ICMPS(>); END_INSN;
      CASE (MIR_UGT, 3);  UCMP(>); END_INSN;
      CASE (MIR_UGTS, 3); UCMPS(>); END_INSN;
      CASE (MIR_FGT, 3);  FCMP(>); END_INSN;
      CASE (MIR_DGT, 3);  DCMP(>); END_INSN;
      
      CASE (MIR_GE, 3);   ICMP(>=); END_INSN;
      CASE (MIR_GES, 3);  ICMPS(>=); END_INSN;
      CASE (MIR_UGE, 3);  UCMP(>=); END_INSN;
      CASE (MIR_UGES, 3); UCMPS(>=); END_INSN;
      CASE (MIR_FGE, 3);  FCMP(>=); END_INSN;
      CASE (MIR_DGE, 3);  DCMP(>=); END_INSN;
      
      CASE (MIR_JMP, 1);   pc = code + get_i (ops); END_INSN;
      CASE (MIR_BT, 2);  {int64_t cond = *get_iop (bp, ops + 1); if (cond) pc = code + get_i (ops); END_INSN; }
      CASE (MIR_BF, 2);  {int64_t cond = *get_iop (bp, ops + 1); if (! cond) pc = code + get_i (ops); END_INSN; }
      CASE (MIR_BTS, 2); {int32_t cond = *get_iop (bp, ops + 1); if (cond) pc = code + get_i (ops); END_INSN; }
      CASE (MIR_BFS, 2); {int32_t cond = *get_iop (bp, ops + 1); if (! cond) pc = code + get_i (ops); END_INSN; }
      CASE (MIR_BEQ, 3);   BICMP (==); END_INSN;
      CASE (MIR_BEQS, 3);  BICMPS (==); END_INSN;
      CASE (MIR_FBEQ, 3);  BFCMP (==); END_INSN;
      CASE (MIR_DBEQ, 3);  BDCMP (==); END_INSN;
      CASE (MIR_BNE, 3);   BICMP (!=); END_INSN;
      CASE (MIR_BNES, 3);  BICMPS (!=); END_INSN;
      CASE (MIR_FBNE, 3);  BFCMP (!=); END_INSN;
      CASE (MIR_DBNE, 3);  BDCMP (!=); END_INSN;
      CASE (MIR_BLT, 3);   BICMP (<); END_INSN;
      CASE (MIR_BLTS, 3);  BICMPS (<); END_INSN;
      CASE (MIR_UBLT, 3);  BUCMP (<); END_INSN;
      CASE (MIR_UBLTS, 3); BUCMPS (<); END_INSN;
      CASE (MIR_FBLT, 3);  BFCMP (<); END_INSN;
      CASE (MIR_DBLT, 3);  BDCMP (<); END_INSN;
      CASE (MIR_BLE, 3);   BICMP (<=); END_INSN;
      CASE (MIR_BLES, 3);  BICMPS (<=); END_INSN;
      CASE (MIR_UBLE, 3);  BUCMP (<=); END_INSN;
      CASE (MIR_UBLES, 3); BUCMPS (<=); END_INSN;
      CASE (MIR_FBLE, 3);  BFCMP (<=); END_INSN;
      CASE (MIR_DBLE, 3);  BDCMP (<=); END_INSN;
      CASE (MIR_BGT, 3);   BICMP (>); END_INSN;
      CASE (MIR_BGTS, 3);  BICMPS (>); END_INSN;
      CASE (MIR_UBGT, 3);  BUCMP (>); END_INSN;
      CASE (MIR_UBGTS, 3); BUCMPS (>); END_INSN;
      CASE (MIR_FBGT, 3);  BFCMP (>); END_INSN;
      CASE (MIR_DBGT, 3);  BDCMP (>); END_INSN;
      CASE (MIR_BGE, 3);   BICMP (>=); END_INSN;
      CASE (MIR_BGES, 3);  BICMPS (>=); END_INSN;
      CASE (MIR_UBGE, 3);  BUCMP (>=); END_INSN;
      CASE (MIR_UBGES, 3); BUCMPS (>=); END_INSN;
      CASE (MIR_FBGE, 3);  BFCMP (>=); END_INSN;
      CASE (MIR_DBGE, 3);  BDCMP (>=); END_INSN;

      CASE (MIR_CALL, 0); {
	int64_t nops = get_i (ops);
	MIR_proto_t proto = get_a (ops + 1);
	void *f = *get_aop (bp, ops + 2);
	size_t start = proto->res_type == MIR_T_V ? 2 : 3;
	MIR_val_t *res = &bp [get_i (ops + 3)];
	
	if (VARR_EXPAND (MIR_val_t, args_varr, nops))
	  args = VARR_ADDR (MIR_val_t, args_varr);
	
	for (size_t i = start; i < nops; i++)
	  args[i - start] = bp [get_i (ops + i + 1)];
	
	call (proto, f, res, nops - start, args);
	pc += nops + 1; /* nops itself */
      }
      END_INSN;
      
      CASE (MIR_RET, 1);  return bp [get_i (ops)]; END_INSN;
      CASE (MIR_FRET, 1); return bp [get_i (ops)]; END_INSN;
      CASE (MIR_DRET, 1); return bp [get_i (ops)]; END_INSN;

      CASE (IC_LDI8, 2);  LD (iop, int64_t, int8_t); END_INSN;
      CASE (IC_LDU8, 2);  LD (uop, uint64_t, uint8_t); END_INSN;
      CASE (IC_LDI16, 2); LD (iop, int64_t, int16_t); END_INSN;
      CASE (IC_LDU16, 2); LD (uop, uint64_t, uint16_t); END_INSN;
      CASE (IC_LDI32, 2); LD (iop, int64_t, int32_t); END_INSN;
      CASE (IC_LDU32, 2); LD (uop, uint64_t, uint32_t); END_INSN;
      CASE (IC_LDI64, 2); LD (iop, int64_t, int64_t); END_INSN;
      CASE (IC_LDF, 2); LD (fop, float, float); END_INSN;
      CASE (IC_LDD, 2); LD (dop, double, double); END_INSN;
      CASE (IC_MOVP, 2); {void **r = get_aop (bp, ops), *a = get_a (ops + 1); *r = a;} END_INSN;
      CASE (IC_STI8, 2); ST (iop, int64_t, int8_t); END_INSN;
      CASE (IC_STU8, 2); ST (iop, uint64_t, uint8_t); END_INSN;
      CASE (IC_STI16, 2); ST (iop, int64_t, int16_t); END_INSN;
      CASE (IC_STU16, 2); ST (iop, uint64_t, uint16_t); END_INSN;
      CASE (IC_STI32, 2); ST (iop, int64_t, int32_t); END_INSN;
      CASE (IC_STU32, 2); ST (iop, uint64_t, uint32_t); END_INSN;
      CASE (IC_STI64, 2); ST (iop, int64_t, int64_t); END_INSN;
      CASE (IC_STF, 2); ST (fop, float, float); END_INSN;
      CASE (IC_STD, 2); ST (dop, double, double); END_INSN;
      CASE (IC_MOVI, 2); {int64_t *r = get_iop (bp, ops), imm = get_i (ops + 1); *r = imm;} END_INSN;
      CASE (IC_MOVF, 2); {float *r = get_fop (bp, ops), imm = get_f (ops + 1); *r = imm;} END_INSN;
      CASE (IC_MOVD, 2); {double *r = get_dop (bp, ops), imm = get_d (ops + 1); *r = imm;} END_INSN;
#if ! DIRECT_THREADED_DISPATCH
    default:
      mir_assert (FALSE);
    }
  }
#endif
}

static inline func_desc_t
get_func_desc (MIR_item_t func_item) {
  mir_assert (func_item->item_type == MIR_func_item);
  return func_item->data;
}

#include <ffi.h>

typedef ffi_type *ffi_type_ptr_t;
typedef void *void_ptr_t;
typedef union {int16_t i8; uint16_t u8; int16_t i16; uint16_t u16; int32_t i32; uint16_t u32;} int_value_t;

DEF_VARR (ffi_type_ptr_t);
static VARR (ffi_type_ptr_t) *ffi_arg_types_varr;

DEF_VARR (void_ptr_t);
static VARR (void_ptr_t) *ffi_arg_values_varr;

DEF_VARR (int_value_t);
static VARR (int_value_t) *ffi_int_values_varr;

static ffi_type_ptr_t *ffi_arg_types;
static void_ptr_t *ffi_arg_values;
static int_value_t *ffi_int_values;
 
static ffi_type_ptr_t ffi_type_ptr (MIR_type_t type) {
  switch (type) {
  case MIR_T_I8: return &ffi_type_sint8;
  case MIR_T_U8: return &ffi_type_uint8;
  case MIR_T_I16: return &ffi_type_sint16;
  case MIR_T_U16: return &ffi_type_uint16;
  case MIR_T_I32: return &ffi_type_sint32;
  case MIR_T_U32: return &ffi_type_uint32;
  case MIR_T_I64: return &ffi_type_sint64;
  case MIR_T_U64: return &ffi_type_uint64;
  case MIR_T_F: return &ffi_type_float;
  case MIR_T_D: return &ffi_type_double;
  case MIR_T_P: return &ffi_type_pointer;
  case MIR_T_V: return &ffi_type_void;
  default:
    mir_assert (FALSE);
  }
}

MIR_val_t MIR_interp_arr (MIR_item_t func_item, size_t nargs, MIR_val_t *vals);

static void call (MIR_proto_t proto, void *addr, MIR_val_t *res, size_t nargs, MIR_val_t *args) {
  MIR_val_t val;
  MIR_type_t type;
  MIR_var_t *arg_vars = NULL;
  ffi_cif cif;
  ffi_status status;
  union { ffi_arg rint; float f; double d; void *a; } u;

  if (proto->args == NULL) {
    mir_assert (nargs == 0);
  } else {
    mir_assert (nargs == VARR_LENGTH (MIR_var_t, proto->args));
    arg_vars = VARR_ADDR (MIR_var_t, proto->args);
  }
  if (VARR_EXPAND (void_ptr_t, ffi_arg_values_varr, nargs)) {
    VARR_EXPAND (ffi_type_ptr_t, ffi_arg_types_varr, nargs);
    VARR_EXPAND (int_value_t, ffi_int_values_varr, nargs);
    ffi_arg_types = VARR_ADDR (ffi_type_ptr_t, ffi_arg_types_varr);
    ffi_arg_values = VARR_ADDR (void_ptr_t, ffi_arg_values_varr);
    ffi_int_values = VARR_ADDR (int_value_t, ffi_int_values_varr);
  }
    
  for (size_t i = 0; i < nargs; i++) {
    type = arg_vars[i].type;
    ffi_arg_types[i] = ffi_type_ptr (type);
    switch (type) {
    case MIR_T_I8:
      ffi_int_values[i].i8 = args[i].i; ffi_arg_values[i] = &ffi_int_values[i].i8;
      break;
    case MIR_T_I16:
      ffi_int_values[i].i16 = args[i].i; ffi_arg_values[i] = &ffi_int_values[i].i16;
      break;
    case MIR_T_I32:
      ffi_int_values[i].i32 = args[i].i; ffi_arg_values[i] = &ffi_int_values[i].i32;
      break;
    case MIR_T_U8:
      ffi_int_values[i].u8 = args[i].u; ffi_arg_values[i] = &ffi_int_values[i].u8;
      break;
    case MIR_T_U16:
      ffi_int_values[i].u16 = args[i].u; ffi_arg_values[i] = &ffi_int_values[i].u16;
      break;
    case MIR_T_U32:
      ffi_int_values[i].u32 = args[i].u; ffi_arg_values[i] = &ffi_int_values[i].u32;
      break;
    case MIR_T_I64: ffi_arg_values[i] = &args[i].i; break;
    case MIR_T_U64: ffi_arg_values[i] = &args[i].u; break;
    case MIR_T_F: ffi_arg_values[i] = &args[i].f; break;
    case MIR_T_D: ffi_arg_values[i] = &args[i].d; break;
    case MIR_T_P: ffi_arg_values[i] = &args[i].a; break;
    default:
      mir_assert (FALSE);
    }
  }
    
  status = ffi_prep_cif (&cif, FFI_DEFAULT_ABI, nargs, ffi_type_ptr (proto->res_type), ffi_arg_types);
  mir_assert (status == FFI_OK);
  
  ffi_call (&cif, addr, &u, ffi_arg_values);
  switch (proto->res_type) {
  case MIR_T_I8: res->i = (int8_t) (u.rint); return;
  case MIR_T_U8: res->u = (uint8_t) (u.rint); return;
  case MIR_T_I16: res->i = (int16_t) (u.rint); return;
  case MIR_T_U16: res->u = (uint16_t) (u.rint); return;
  case MIR_T_I32: res->i = (int32_t) (u.rint); return;
  case MIR_T_U32: res->u = (uint32_t) (u.rint); return;
  case MIR_T_I64: res->i = (int64_t) (u.rint); return;
  case MIR_T_U64: res->u = (uint64_t) (u.rint); return;
  case MIR_T_F: res->f = u.f; return;
  case MIR_T_D: res->d = u.d; return;
  case MIR_T_P: res->a = u.a; return;
  case MIR_T_V: return;
  default:
    mir_assert (FALSE);
  }
}

void MIR_interp_init (void) {
#if DIRECT_THREADED_DISPATCH
  MIR_val_t v = eval (NULL, NULL);
  dispatch_label_tab = v.a;
#endif
  VARR_CREATE (MIR_insn_t, branches, 0);
  VARR_CREATE (MIR_val_t, code_varr, 0);
  VARR_CREATE (MIR_val_t, args_varr, 0);
  args = VARR_ADDR (MIR_val_t, args_varr);
  VARR_CREATE (ffi_type_ptr_t, ffi_arg_types_varr, 0);
  ffi_arg_types = VARR_ADDR (ffi_type_ptr_t, ffi_arg_types_varr);
  VARR_CREATE (void_ptr_t, ffi_arg_values_varr, 0);
  ffi_arg_values = VARR_ADDR (void_ptr_t, ffi_arg_values_varr);
  VARR_CREATE (int_value_t, ffi_int_values_varr, 0);
  ffi_int_values = VARR_ADDR (int_value_t, ffi_int_values_varr);
}

void MIR_interp_finish (void) {
  VARR_DESTROY (MIR_insn_t, branches);
  VARR_DESTROY (MIR_val_t, code_varr);
  VARR_DESTROY (MIR_val_t, args_varr);
  VARR_DESTROY (ffi_type_ptr_t, ffi_arg_types_varr);
  VARR_DESTROY (void_ptr_t, ffi_arg_values_varr);
  VARR_DESTROY (int_value_t, ffi_int_values_varr);
  /* Clear func descs???  */
}

MIR_val_t MIR_interp (MIR_item_t func_item, size_t nargs, ...) {
  va_list argp;
  func_desc_t func_desc;
  MIR_val_t *bp;
  size_t i;
  
  mir_assert (func_item->item_type == MIR_func_item);
  if (func_item->data == NULL) {
    MIR_simplify_func (func_item, FALSE);
    generate_icode (func_item);
  }
  func_desc = get_func_desc (func_item);
  bp = alloca ((func_desc->nregs + func_desc->frame_size_in_vals) * sizeof (MIR_val_t));
  if (func_desc->nregs < nargs + 1)
    nargs = func_desc->nregs - 1;
  bp[0].i = 0;
  va_start (argp, nargs);
  for (i = 0; i < nargs; i++)
    bp[i + 1] = va_arg (argp, MIR_val_t);
  va_end(argp);
  bp[func_desc->fp_reg].i = (int64_t) (bp + func_desc->nregs); /* frame address */
  return eval (func_desc->code, bp);
}

MIR_val_t MIR_interp_arr (MIR_item_t func_item, size_t nargs, MIR_val_t *vals) {
  func_desc_t func_desc;
  MIR_val_t *bp;
  
  mir_assert (func_item->item_type == MIR_func_item);
  if (func_item->data == NULL) {
    MIR_simplify_func (func_item, FALSE);
    generate_icode (func_item);
  }
  func_desc = get_func_desc (func_item);
  bp = alloca ((func_desc->nregs + func_desc->frame_size_in_vals) * sizeof (MIR_val_t));
  if (func_desc->nregs < nargs + 1)
    nargs = func_desc->nregs - 1;
  bp[0].i = 0;
  memcpy (&bp[1], vals, sizeof (MIR_val_t) * nargs);
  bp[func_desc->fp_reg].i = (int64_t) (bp + func_desc->nregs); /* frame address */
  return eval (func_desc->code, bp);
}

/* C call interface to interpreter.  It is based on knowledge of
   common vararg implementation.  For some targets it might not
   work.  */

static MIR_item_t called_func;

static MIR_val_t interp (MIR_item_t func_item, MIR_val_t a0, va_list va) {
  size_t nargs;
  MIR_var_t *arg_vars;
  MIR_func_t func = func_item->u.func;

  mir_assert (sizeof (int32_t) == sizeof (int));
  nargs = func->nargs;
  arg_vars = VARR_ADDR (MIR_var_t, func->vars);
  if (VARR_EXPAND (MIR_val_t, args_varr, nargs))
    args = VARR_ADDR (MIR_val_t, args_varr);
  args[0] = a0;
  for (size_t i = 1; i < nargs; i++) {
    MIR_type_t type = arg_vars[i].type;
    switch (type) {
    case MIR_T_I8: args[i].i = (int8_t) va_arg (va, int32_t); break;
    case MIR_T_I16: args[i].i = (int16_t) va_arg (va, int32_t); break;
    case MIR_T_I32: args[i].i = va_arg (va, int32_t); break;
    case MIR_T_I64: args[i].i = va_arg (va, int64_t); break;
    case MIR_T_U8: args[i].i = (uint8_t) va_arg (va, uint32_t); break;
    case MIR_T_U16: args[i].i = (uint16_t) va_arg (va, uint32_t); break;
    case MIR_T_U32: args[i].i = va_arg (va, uint32_t); break;
    case MIR_T_F: {
      union {double d; float f;} u;
      u.d = va_arg (va, double);
      args[i].f = u.f;
      break;
    }
    case MIR_T_D: args[i].d = va_arg (va, double); break;
    case MIR_T_P: args[i].a = va_arg (va, void *); break;
    default:
      mir_assert (FALSE);
    }
  }
  return MIR_interp_arr (func_item, nargs, args);
}

static int64_t i_shim (MIR_val_t a0, va_list args) {MIR_val_t v = interp (called_func, a0, args); return v.i;}
static float f_shim (MIR_val_t a0, va_list args) {MIR_val_t v = interp (called_func, a0, args); return v.f;}
static double d_shim (MIR_val_t a0, va_list args) {MIR_val_t v = interp (called_func, a0, args); return v.d;}
static void *a_shim (MIR_val_t a0, va_list args) {MIR_val_t v = interp (called_func, a0, args); return v.a;}

#define define_shim(rtype, pref, suf, partype, valsuf) \
  rtype pref ## _shim_ ## suf (partype p, ...) {       \
    MIR_val_t v; va_list args; v.valsuf = p; va_start (args, p); return pref ## _shim (v, args); \
  }

#define define_3shims(suf, partype, valsuf)	 \
  define_shim (int64_t, i, suf, partype, valsuf) \
  define_shim (float, f, suf, partype, valsuf)   \
  define_shim (double, d, suf, partype, valsuf)  \
  define_shim (void *, a, suf, partype, valsuf)
  
define_3shims (i8, int8_t, i)
define_3shims (u8, uint8_t, i)
define_3shims (i16, int16_t, i)
define_3shims (u16, uint16_t, i)
define_3shims (i32, int32_t, i)
define_3shims (u32, uint32_t, i)

define_3shims (i64, int64_t, i)
define_3shims (f, float, f)
define_3shims (d, double, d)
define_3shims (a, void *, a)

int64_t i_shim_v (void) { MIR_val_t v = MIR_interp_arr (called_func, 0, NULL); return v.i; }
float f_shim_v (void) { MIR_val_t v = MIR_interp_arr (called_func, 0, NULL); return v.f; }
double d_shim_v (void) { MIR_val_t v = MIR_interp_arr (called_func, 0, NULL); return v.d; }
void *a_shim_v (void) { MIR_val_t v = MIR_interp_arr (called_func, 0, NULL); return v.a; }

static void *get_call_shim (MIR_item_t func_item) {
  MIR_func_t func = func_item->u.func;
  MIR_type_t rtp = func->res_type == MIR_T_V ? MIR_T_I64 : func->res_type;
  MIR_type_t atp = func->nargs == 0 ? MIR_T_V : VARR_GET (MIR_var_t, func->vars, 0).type;

  switch (atp) {
  case MIR_T_I8: return (rtp == MIR_T_F ? (void *) f_shim_i8 : rtp == MIR_T_D ? (void *) d_shim_i8
			 : rtp == MIR_T_P ? (void *) a_shim_i8 : (void *) i_shim_i8);
  case MIR_T_U8: return (rtp == MIR_T_F ? (void *) f_shim_u8 : rtp == MIR_T_D ? (void *) d_shim_u8
			 : rtp == MIR_T_P ? (void *) a_shim_u8 : (void *) i_shim_u8);
  case MIR_T_I16: return (rtp == MIR_T_F ? (void *) f_shim_i16 : rtp == MIR_T_D ? (void *) d_shim_i16
			  : rtp == MIR_T_P ? (void *) a_shim_i16 : (void *) i_shim_i16);
  case MIR_T_U16: return (rtp == MIR_T_F ? (void *) f_shim_u16 : rtp == MIR_T_D ? (void *) d_shim_u16
			  : rtp == MIR_T_P ? (void *) a_shim_u16 : (void *) i_shim_u16);
  case MIR_T_I32: return (rtp == MIR_T_F ? (void *) f_shim_i32 : rtp == MIR_T_D ? (void *) d_shim_i32
			  : rtp == MIR_T_P ? (void *) a_shim_i32 : (void *) i_shim_i32);
  case MIR_T_U32: return (rtp == MIR_T_F ? (void *) f_shim_u32 : rtp == MIR_T_D ? (void *) d_shim_u32
			  : rtp == MIR_T_P ? (void *) a_shim_u32 : (void *) i_shim_u32);
  case MIR_T_I64: return (rtp == MIR_T_F ? (void *) f_shim_i64 : rtp == MIR_T_D ? (void *) d_shim_i64
			  : rtp == MIR_T_P ? (void *) a_shim_i64 : (void *) i_shim_i64);
  case MIR_T_F: return (rtp == MIR_T_F ? (void *) f_shim_f : rtp == MIR_T_D ? (void *) d_shim_f
			: rtp == MIR_T_P ? (void *) a_shim_f : (void *) i_shim_f);
  case MIR_T_D: return (rtp == MIR_T_F ? (void *) f_shim_d : rtp == MIR_T_D ? (void *) d_shim_d
			: rtp == MIR_T_P ? (void *) a_shim_d : (void *) i_shim_d);
  case MIR_T_P: return (rtp == MIR_T_F ? (void *) f_shim_a : rtp == MIR_T_D ? (void *) d_shim_a
			: rtp == MIR_T_P ? (void *) a_shim_a : (void *) i_shim_a);
  case MIR_T_V: return (rtp == MIR_T_F ? (void *) f_shim_v : rtp == MIR_T_D ? (void *) d_shim_v
			: rtp == MIR_T_P ? (void *) a_shim_v : (void *) i_shim_v);
  default:
    mir_assert (FALSE);
  }
}

void MIR_set_C_interp_interface (MIR_item_t func_item) {
  func_item->addr = _MIR_get_interp_shim (func_item, &called_func, get_call_shim (func_item));
}

static void redirect_interface_to_interp (MIR_item_t func_item) {
  _MIR_redirect_thunk (func_item->addr,
		       _MIR_get_interp_shim (func_item, &called_func, get_call_shim (func_item)));
}
