/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

#if defined (__APPLE__)
#include <stdio.h>
#endif

#include "common.h"
#include "types.h"
#include "interface.h"
#include "timer.h"
#include "memory.h"
#include "logging.h"
#include "ext_OpenCL.h"
#include "ext_ADL.h"
#include "ext_nvapi.h"
#include "ext_nvml.h"
#include "ext_xnvctrl.h"
#include "convert.h"
#include "tuningdb.h"
#include "thread.h"
#include "opencl.h"
#include "hwmon.h"
#include "restore.h"
#include "hash_management.h"
#include "rp_cpu.h"
#include "terminal.h"
#include "mpsp.h"
#include "rp_cpu.h"
#include "outfile.h"
#include "potfile.h"
#include "debugfile.h"
#include "loopback.h"
#include "data.h"
#include "shared.h"
#include "rp_cpu.h"

static const char grp_op_nop[] =
{
  RULE_OP_MANGLE_LREST,
  RULE_OP_MANGLE_UREST,
  RULE_OP_MANGLE_LREST_UFIRST,
  RULE_OP_MANGLE_UREST_LFIRST,
  RULE_OP_MANGLE_TREST,
  RULE_OP_MANGLE_REVERSE,
  RULE_OP_MANGLE_DUPEWORD,
  RULE_OP_MANGLE_REFLECT,
  RULE_OP_MANGLE_DELETE_FIRST,
  RULE_OP_MANGLE_DELETE_LAST,
  RULE_OP_MANGLE_ROTATE_LEFT,
  RULE_OP_MANGLE_ROTATE_RIGHT,
  RULE_OP_MANGLE_SWITCH_FIRST,
  RULE_OP_MANGLE_SWITCH_LAST,
  RULE_OP_MANGLE_DUPECHAR_ALL,
  RULE_OP_MANGLE_TITLE,
  RULE_OP_MANGLE_APPEND_MEMORY,
  RULE_OP_MANGLE_PREPEND_MEMORY,
};

static const char grp_op_pos_p0[] =
{
  RULE_OP_MANGLE_TOGGLE_AT,
  RULE_OP_MANGLE_DELETE_AT,
  RULE_OP_MANGLE_TRUNCATE_AT,
  RULE_OP_MANGLE_CHR_INCR,
  RULE_OP_MANGLE_CHR_DECR,
  RULE_OP_MANGLE_CHR_SHIFTL,
  RULE_OP_MANGLE_CHR_SHIFTR,
  RULE_OP_MANGLE_REPLACE_NP1,
  RULE_OP_MANGLE_REPLACE_NM1
};

static const char grp_op_pos_p1[] =
{
  RULE_OP_MANGLE_DUPEWORD_TIMES,
  RULE_OP_MANGLE_DUPECHAR_FIRST,
  RULE_OP_MANGLE_DUPECHAR_LAST,
  RULE_OP_MANGLE_DUPEBLOCK_FIRST,
  RULE_OP_MANGLE_DUPEBLOCK_LAST
};

static const char grp_op_chr[] =
{
  RULE_OP_MANGLE_APPEND,
  RULE_OP_MANGLE_PREPEND,
  RULE_OP_MANGLE_PURGECHAR
};

static const char grp_op_chr_chr[] =
{
  RULE_OP_MANGLE_REPLACE
};

static const char grp_op_pos_chr[] =
{
  RULE_OP_MANGLE_INSERT,
  RULE_OP_MANGLE_OVERSTRIKE
};

static const char grp_op_pos_pos0[] =
{
  RULE_OP_MANGLE_SWITCH_AT
};

static const char grp_op_pos_pos1[] =
{
  RULE_OP_MANGLE_EXTRACT,
  RULE_OP_MANGLE_OMIT
};

static const char grp_op_pos1_pos2_pos3[] =
{
  RULE_OP_MANGLE_EXTRACT_MEMORY
};

static const char grp_pos[] =
{
  '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B'
};

static bool class_num (const u8 c)
{
  return ((c >= '0') && (c <= '9'));
}

static bool class_lower (const u8 c)
{
  return ((c >= 'a') && (c <= 'z'));
}

static bool class_upper (const u8 c)
{
  return ((c >= 'A') && (c <= 'Z'));
}

static bool class_alpha (const u8 c)
{
  return (class_lower (c) || class_upper (c));
}

static int conv_ctoi (const u8 c)
{
  if (class_num (c))
  {
    return c - '0';
  }
  else if (class_upper (c))
  {
    return c - 'A' + 10;
  }

  return -1;
}

static int conv_itoc (const u8 c)
{
  if (c < 10)
  {
    return c + '0';
  }
  else if (c < 37)
  {
    return c + 'A' - 10;
  }

  return -1;
}

#define INCR_POS           if (++rule_pos == rule_len) return (-1)
#define SET_NAME(rule,val) (rule)->cmds[rule_cnt]  = ((val) & 0xff) <<  0
#define SET_P0(rule,val)   INCR_POS; (rule)->cmds[rule_cnt] |= ((val) & 0xff) <<  8
#define SET_P1(rule,val)   INCR_POS; (rule)->cmds[rule_cnt] |= ((val) & 0xff) << 16
#define MAX_KERNEL_RULES   255
#define GET_NAME(rule)     rule_cmd = (((rule)->cmds[rule_cnt] >>  0) & 0xff)
#define GET_P0(rule)       INCR_POS; rule_buf[rule_pos] = (((rule)->cmds[rule_cnt] >>  8) & 0xff)
#define GET_P1(rule)       INCR_POS; rule_buf[rule_pos] = (((rule)->cmds[rule_cnt] >> 16) & 0xff)

#define SET_P0_CONV(rule,val)  INCR_POS; (rule)->cmds[rule_cnt] |= ((conv_ctoi (val)) & 0xff) <<  8
#define SET_P1_CONV(rule,val)  INCR_POS; (rule)->cmds[rule_cnt] |= ((conv_ctoi (val)) & 0xff) << 16
#define GET_P0_CONV(rule)      INCR_POS; rule_buf[rule_pos] = conv_itoc (((rule)->cmds[rule_cnt] >>  8) & 0xff)
#define GET_P1_CONV(rule)      INCR_POS; rule_buf[rule_pos] = conv_itoc (((rule)->cmds[rule_cnt] >> 16) & 0xff)

#define NEXT_RULEPOS(rp)      if (++(rp) == rule_len) return (RULE_RC_SYNTAX_ERROR)
#define NEXT_RPTOI(r,rp,up)   if (((up) = conv_ctoi ((r)[(rp)])) == -1) return (RULE_RC_SYNTAX_ERROR)

#define MANGLE_TOGGLE_AT(a,p) if (class_alpha ((a)[(p)])) (a)[(p)] ^= 0x20
#define MANGLE_LOWER_AT(a,p)  if (class_upper ((a)[(p)])) (a)[(p)] ^= 0x20
#define MANGLE_UPPER_AT(a,p)  if (class_lower ((a)[(p)])) (a)[(p)] ^= 0x20

/* #define MANGLE_SWITCH(a,l,r)  { char c = (l); arr[(r)] = arr[(l)]; arr[(l)] = c; } */
/* #define MANGLE_SWITCH(a,l,r)  { char c = (l); (a)[(r)] = (a)[(l)]; (a)[(l)] = c; } */
#define MANGLE_SWITCH(a,l,r)  { char c = (a)[(r)]; (a)[(r)] = (a)[(l)]; (a)[(l)] = c; }

static int mangle_lrest (char arr[BLOCK_SIZE], int arr_len)
{
  int pos;

  for (pos = 0; pos < arr_len; pos++) MANGLE_LOWER_AT (arr, pos);

  return (arr_len);
}

static int mangle_urest (char arr[BLOCK_SIZE], int arr_len)
{
  int pos;

  for (pos = 0; pos < arr_len; pos++) MANGLE_UPPER_AT (arr, pos);

  return (arr_len);
}

static int mangle_trest (char arr[BLOCK_SIZE], int arr_len)
{
  int pos;

  for (pos = 0; pos < arr_len; pos++) MANGLE_TOGGLE_AT (arr, pos);

  return (arr_len);
}

static int mangle_reverse (char arr[BLOCK_SIZE], int arr_len)
{
  int l;
  int r;

  for (l = 0; l < arr_len; l++)
  {
    r = arr_len - 1 - l;

    if (l >= r) break;

    MANGLE_SWITCH (arr, l, r);
  }

  return (arr_len);
}

static int mangle_double (char arr[BLOCK_SIZE], int arr_len)
{
  if ((arr_len * 2) >= BLOCK_SIZE) return (arr_len);

  memcpy (&arr[arr_len], arr, (size_t) arr_len);

  return (arr_len * 2);
}

static int mangle_double_times (char arr[BLOCK_SIZE], int arr_len, int times)
{
  if (((arr_len * times) + arr_len) >= BLOCK_SIZE) return (arr_len);

  int orig_len = arr_len;

  int i;

  for (i = 0; i < times; i++)
  {
    memcpy (&arr[arr_len], arr, orig_len);

    arr_len += orig_len;
  }

  return (arr_len);
}

static int mangle_reflect (char arr[BLOCK_SIZE], int arr_len)
{
  if ((arr_len * 2) >= BLOCK_SIZE) return (arr_len);

  mangle_double (arr, arr_len);

  mangle_reverse (arr + arr_len, arr_len);

  return (arr_len * 2);
}

static int mangle_rotate_left (char arr[BLOCK_SIZE], int arr_len)
{
  int l;
  int r;

  for (l = 0, r = arr_len - 1; r > 0; r--)
  {
    MANGLE_SWITCH (arr, l, r);
  }

  return (arr_len);
}

static int mangle_rotate_right (char arr[BLOCK_SIZE], int arr_len)
{
  int l;
  int r;

  for (l = 0, r = arr_len - 1; l < r; l++)
  {
    MANGLE_SWITCH (arr, l, r);
  }

  return (arr_len);
}

static int mangle_append (char arr[BLOCK_SIZE], int arr_len, char c)
{
  if ((arr_len + 1) >= BLOCK_SIZE) return (arr_len);

  arr[arr_len] = c;

  return (arr_len + 1);
}

static int mangle_prepend (char arr[BLOCK_SIZE], int arr_len, char c)
{
  if ((arr_len + 1) >= BLOCK_SIZE) return (arr_len);

  int arr_pos;

  for (arr_pos = arr_len - 1; arr_pos > -1; arr_pos--)
  {
    arr[arr_pos + 1] = arr[arr_pos];
  }

  arr[0] = c;

  return (arr_len + 1);
}

static int mangle_delete_at (char arr[BLOCK_SIZE], int arr_len, int upos)
{
  if (upos >= arr_len) return (arr_len);

  int arr_pos;

  for (arr_pos = upos; arr_pos < arr_len - 1; arr_pos++)
  {
    arr[arr_pos] = arr[arr_pos + 1];
  }

  return (arr_len - 1);
}

static int mangle_extract (char arr[BLOCK_SIZE], int arr_len, int upos, int ulen)
{
  if (upos >= arr_len) return (arr_len);

  if ((upos + ulen) > arr_len) return (arr_len);

  int arr_pos;

  for (arr_pos = 0; arr_pos < ulen; arr_pos++)
  {
    arr[arr_pos] = arr[upos + arr_pos];
  }

  return (ulen);
}

static int mangle_omit (char arr[BLOCK_SIZE], int arr_len, int upos, int ulen)
{
  if (upos >= arr_len) return (arr_len);

  if ((upos + ulen) >= arr_len) return (arr_len);

  int arr_pos;

  for (arr_pos = upos; arr_pos < arr_len - ulen; arr_pos++)
  {
    arr[arr_pos] = arr[arr_pos + ulen];
  }

  return (arr_len - ulen);
}

static int mangle_insert (char arr[BLOCK_SIZE], int arr_len, int upos, char c)
{
  if (upos >= arr_len) return (arr_len);

  if ((arr_len + 1) >= BLOCK_SIZE) return (arr_len);

  int arr_pos;

  for (arr_pos = arr_len - 1; arr_pos > upos - 1; arr_pos--)
  {
    arr[arr_pos + 1] = arr[arr_pos];
  }

  arr[upos] = c;

  return (arr_len + 1);
}

static int mangle_insert_multi (char arr[BLOCK_SIZE], int arr_len, int arr_pos, char arr2[BLOCK_SIZE], int arr2_len, int arr2_pos, int arr2_cpy)
{
  if ((arr_len + arr2_cpy) > BLOCK_SIZE) return (RULE_RC_REJECT_ERROR);

  if (arr_pos > arr_len) return (RULE_RC_REJECT_ERROR);

  if (arr2_pos > arr2_len) return (RULE_RC_REJECT_ERROR);

  if ((arr2_pos + arr2_cpy) > arr2_len) return (RULE_RC_REJECT_ERROR);

  if (arr2_cpy < 1) return (RULE_RC_SYNTAX_ERROR);

  memcpy (arr2, arr2 + arr2_pos, arr2_len - arr2_pos);

  memcpy (arr2 + arr2_cpy, arr + arr_pos, arr_len - arr_pos);

  memcpy (arr + arr_pos, arr2, arr_len - arr_pos + arr2_cpy);

  return (arr_len + arr2_cpy);
}

static int mangle_overstrike (char arr[BLOCK_SIZE], int arr_len, int upos, char c)
{
  if (upos >= arr_len) return (arr_len);

  arr[upos] = c;

  return (arr_len);
}

static int mangle_truncate_at (char arr[BLOCK_SIZE], int arr_len, int upos)
{
  if (upos >= arr_len) return (arr_len);

  memset (arr + upos, 0, arr_len - upos);

  return (upos);
}

static int mangle_replace (char arr[BLOCK_SIZE], int arr_len, char oldc, char newc)
{
  int arr_pos;

  for (arr_pos = 0; arr_pos < arr_len; arr_pos++)
  {
    if (arr[arr_pos] != oldc) continue;

    arr[arr_pos] = newc;
  }

  return (arr_len);
}

static int mangle_purgechar (char arr[BLOCK_SIZE], int arr_len, char c)
{
  int arr_pos;

  int ret_len;

  for (ret_len = 0, arr_pos = 0; arr_pos < arr_len; arr_pos++)
  {
    if (arr[arr_pos] == c) continue;

    arr[ret_len] = arr[arr_pos];

    ret_len++;
  }

  return (ret_len);
}

static int mangle_dupeblock_prepend (char arr[BLOCK_SIZE], int arr_len, int ulen)
{
  if (ulen > arr_len) return (arr_len);

  if ((arr_len + ulen) >= BLOCK_SIZE) return (arr_len);

  char cs[100] = { 0 };

  memcpy (cs, arr, ulen);

  int i;

  for (i = 0; i < ulen; i++)
  {
    char c = cs[i];

    arr_len = mangle_insert (arr, arr_len, i, c);
  }

  return (arr_len);
}

static int mangle_dupeblock_append (char arr[BLOCK_SIZE], int arr_len, int ulen)
{
  if (ulen > arr_len) return (arr_len);

  if ((arr_len + ulen) >= BLOCK_SIZE) return (arr_len);

  int upos = arr_len - ulen;

  int i;

  for (i = 0; i < ulen; i++)
  {
    char c = arr[upos + i];

    arr_len = mangle_append (arr, arr_len, c);
  }

  return (arr_len);
}

static int mangle_dupechar_at (char arr[BLOCK_SIZE], int arr_len, int upos, int ulen)
{
  if ( arr_len         ==  0) return (arr_len);
  if ((arr_len + ulen) >= BLOCK_SIZE) return (arr_len);

  char c = arr[upos];

  int i;

  for (i = 0; i < ulen; i++)
  {
    arr_len = mangle_insert (arr, arr_len, upos, c);
  }

  return (arr_len);
}

static int mangle_dupechar (char arr[BLOCK_SIZE], int arr_len)
{
  if ( arr_len            ==  0) return (arr_len);
  if ((arr_len + arr_len) >= BLOCK_SIZE) return (arr_len);

  int arr_pos;

  for (arr_pos = arr_len - 1; arr_pos > -1; arr_pos--)
  {
    int new_pos = arr_pos * 2;

    arr[new_pos] = arr[arr_pos];

    arr[new_pos + 1] = arr[arr_pos];
  }

  return (arr_len * 2);
}

static int mangle_switch_at_check (char arr[BLOCK_SIZE], int arr_len, int upos, int upos2)
{
  if (upos  >= arr_len) return (arr_len);
  if (upos2 >= arr_len) return (arr_len);

  MANGLE_SWITCH (arr, upos, upos2);

  return (arr_len);
}

static int mangle_switch_at (char arr[BLOCK_SIZE], int arr_len, int upos, int upos2)
{
  MANGLE_SWITCH (arr, upos, upos2);

  return (arr_len);
}

static int mangle_chr_shiftl (char arr[BLOCK_SIZE], int arr_len, int upos)
{
  if (upos >= arr_len) return (arr_len);

  arr[upos] <<= 1;

  return (arr_len);
}

static int mangle_chr_shiftr (char arr[BLOCK_SIZE], int arr_len, int upos)
{
  if (upos >= arr_len) return (arr_len);

  arr[upos] >>= 1;

  return (arr_len);
}

static int mangle_chr_incr (char arr[BLOCK_SIZE], int arr_len, int upos)
{
  if (upos >= arr_len) return (arr_len);

  arr[upos] += 1;

  return (arr_len);
}

static int mangle_chr_decr (char arr[BLOCK_SIZE], int arr_len, int upos)
{
  if (upos >= arr_len) return (arr_len);

  arr[upos] -= 1;

  return (arr_len);
}

static int mangle_title (char arr[BLOCK_SIZE], int arr_len)
{
  int upper_next = 1;

  int pos;

  for (pos = 0; pos < arr_len; pos++)
  {
    if (arr[pos] == ' ')
    {
      upper_next = 1;

      continue;
    }

    if (upper_next)
    {
      upper_next = 0;

      MANGLE_UPPER_AT (arr, pos);
    }
    else
    {
      MANGLE_LOWER_AT (arr, pos);
    }
  }

  return (arr_len);
}

int generate_random_rule (char rule_buf[RP_RULE_BUFSIZ], u32 rp_gen_func_min, u32 rp_gen_func_max)
{
  u32 rp_gen_num = get_random_num (rp_gen_func_min, rp_gen_func_max);

  u32 j;

  u32 rule_pos = 0;

  for (j = 0; j < rp_gen_num; j++)
  {
    u32 r  = 0;
    u32 p1 = 0;
    u32 p2 = 0;
    u32 p3 = 0;

    switch ((char) get_random_num (0, 9))
    {
      case 0:
        r = get_random_num (0, sizeof (grp_op_nop));
        rule_buf[rule_pos++] = grp_op_nop[r];
        break;

      case 1:
        r = get_random_num (0, sizeof (grp_op_pos_p0));
        rule_buf[rule_pos++] = grp_op_pos_p0[r];
        p1 = get_random_num (0, sizeof (grp_pos));
        rule_buf[rule_pos++] = grp_pos[p1];
        break;

      case 2:
        r = get_random_num (0, sizeof (grp_op_pos_p1));
        rule_buf[rule_pos++] = grp_op_pos_p1[r];
        p1 = get_random_num (1, 6);
        rule_buf[rule_pos++] = grp_pos[p1];
        break;

      case 3:
        r = get_random_num (0, sizeof (grp_op_chr));
        rule_buf[rule_pos++] = grp_op_chr[r];
        p1 = get_random_num (0x20, 0x7e);
        rule_buf[rule_pos++] = (char) p1;
        break;

      case 4:
        r = get_random_num (0, sizeof (grp_op_chr_chr));
        rule_buf[rule_pos++] = grp_op_chr_chr[r];
        p1 = get_random_num (0x20, 0x7e);
        rule_buf[rule_pos++] = (char) p1;
        p2 = get_random_num (0x20, 0x7e);
        while (p1 == p2)
        p2 = get_random_num (0x20, 0x7e);
        rule_buf[rule_pos++] = (char) p2;
        break;

      case 5:
        r = get_random_num (0, sizeof (grp_op_pos_chr));
        rule_buf[rule_pos++] = grp_op_pos_chr[r];
        p1 = get_random_num (0, sizeof (grp_pos));
        rule_buf[rule_pos++] = grp_pos[p1];
        p2 = get_random_num (0x20, 0x7e);
        rule_buf[rule_pos++] = (char) p2;
        break;

      case 6:
        r = get_random_num (0, sizeof (grp_op_pos_pos0));
        rule_buf[rule_pos++] = grp_op_pos_pos0[r];
        p1 = get_random_num (0, sizeof (grp_pos));
        rule_buf[rule_pos++] = grp_pos[p1];
        p2 = get_random_num (0, sizeof (grp_pos));
        while (p1 == p2)
        p2 = get_random_num (0, sizeof (grp_pos));
        rule_buf[rule_pos++] = grp_pos[p2];
        break;

      case 7:
        r = get_random_num (0, sizeof (grp_op_pos_pos1));
        rule_buf[rule_pos++] = grp_op_pos_pos1[r];
        p1 = get_random_num (0, sizeof (grp_pos));
        rule_buf[rule_pos++] = grp_pos[p1];
        p2 = get_random_num (1, sizeof (grp_pos));
        while (p1 == p2)
        p2 = get_random_num (1, sizeof (grp_pos));
        rule_buf[rule_pos++] = grp_pos[p2];
        break;

      case 8:
        r = get_random_num (0, sizeof (grp_op_pos1_pos2_pos3));
        rule_buf[rule_pos++] = grp_op_pos1_pos2_pos3[r];
        p1 = get_random_num (0, sizeof (grp_pos));
        rule_buf[rule_pos++] = grp_pos[p1];
        p2 = get_random_num (1, sizeof (grp_pos));
        rule_buf[rule_pos++] = grp_pos[p1];
        p3 = get_random_num (0, sizeof (grp_pos));
        rule_buf[rule_pos++] = grp_pos[p3];
        break;
    }
  }

  return (rule_pos);
}

int _old_apply_rule (char *rule, int rule_len, char in[BLOCK_SIZE], int in_len, char out[BLOCK_SIZE])
{
  char mem[BLOCK_SIZE] = { 0 };

  if (in == NULL) return (RULE_RC_REJECT_ERROR);

  if (out == NULL) return (RULE_RC_REJECT_ERROR);

  if (in_len < 1 || in_len > BLOCK_SIZE) return (RULE_RC_REJECT_ERROR);

  if (rule_len < 1) return (RULE_RC_REJECT_ERROR);

  int out_len = in_len;
  int mem_len = in_len;

  memcpy (out, in, out_len);

  int rule_pos;

  for (rule_pos = 0; rule_pos < rule_len; rule_pos++)
  {
    int upos, upos2;
    int ulen;

    switch (rule[rule_pos])
    {
      case ' ':
        break;

      case RULE_OP_MANGLE_NOOP:
        break;

      case RULE_OP_MANGLE_LREST:
        out_len = mangle_lrest (out, out_len);
        break;

      case RULE_OP_MANGLE_UREST:
        out_len = mangle_urest (out, out_len);
        break;

      case RULE_OP_MANGLE_LREST_UFIRST:
        out_len = mangle_lrest (out, out_len);
        if (out_len) MANGLE_UPPER_AT (out, 0);
        break;

      case RULE_OP_MANGLE_UREST_LFIRST:
        out_len = mangle_urest (out, out_len);
        if (out_len) MANGLE_LOWER_AT (out, 0);
        break;

      case RULE_OP_MANGLE_TREST:
        out_len = mangle_trest (out, out_len);
        break;

      case RULE_OP_MANGLE_TOGGLE_AT:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        if (upos < out_len) MANGLE_TOGGLE_AT (out, upos);
        break;

      case RULE_OP_MANGLE_REVERSE:
        out_len = mangle_reverse (out, out_len);
        break;

      case RULE_OP_MANGLE_DUPEWORD:
        out_len = mangle_double (out, out_len);
        break;

      case RULE_OP_MANGLE_DUPEWORD_TIMES:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, ulen);
        out_len = mangle_double_times (out, out_len, ulen);
        break;

      case RULE_OP_MANGLE_REFLECT:
        out_len = mangle_reflect (out, out_len);
        break;

      case RULE_OP_MANGLE_ROTATE_LEFT:
        mangle_rotate_left (out, out_len);
        break;

      case RULE_OP_MANGLE_ROTATE_RIGHT:
        mangle_rotate_right (out, out_len);
        break;

      case RULE_OP_MANGLE_APPEND:
        NEXT_RULEPOS (rule_pos);
        out_len = mangle_append (out, out_len, rule[rule_pos]);
        break;

      case RULE_OP_MANGLE_PREPEND:
        NEXT_RULEPOS (rule_pos);
        out_len = mangle_prepend (out, out_len, rule[rule_pos]);
        break;

      case RULE_OP_MANGLE_DELETE_FIRST:
        out_len = mangle_delete_at (out, out_len, 0);
        break;

      case RULE_OP_MANGLE_DELETE_LAST:
        out_len = mangle_delete_at (out, out_len, (out_len) ? out_len - 1 : 0);
        break;

      case RULE_OP_MANGLE_DELETE_AT:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        out_len = mangle_delete_at (out, out_len, upos);
        break;

      case RULE_OP_MANGLE_EXTRACT:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, ulen);
        out_len = mangle_extract (out, out_len, upos, ulen);
        break;

      case RULE_OP_MANGLE_OMIT:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, ulen);
        out_len = mangle_omit (out, out_len, upos, ulen);
        break;

      case RULE_OP_MANGLE_INSERT:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        NEXT_RULEPOS (rule_pos);
        out_len = mangle_insert (out, out_len, upos, rule[rule_pos]);
        break;

      case RULE_OP_MANGLE_OVERSTRIKE:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        NEXT_RULEPOS (rule_pos);
        out_len = mangle_overstrike (out, out_len, upos, rule[rule_pos]);
        break;

      case RULE_OP_MANGLE_TRUNCATE_AT:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        out_len = mangle_truncate_at (out, out_len, upos);
        break;

      case RULE_OP_MANGLE_REPLACE:
        NEXT_RULEPOS (rule_pos);
        NEXT_RULEPOS (rule_pos);
        out_len = mangle_replace (out, out_len, rule[rule_pos - 1], rule[rule_pos]);
        break;

      case RULE_OP_MANGLE_PURGECHAR:
        NEXT_RULEPOS (rule_pos);
        out_len = mangle_purgechar (out, out_len, rule[rule_pos]);
        break;

      case RULE_OP_MANGLE_TOGGLECASE_REC:
        /* todo */
        break;

      case RULE_OP_MANGLE_DUPECHAR_FIRST:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, ulen);
        out_len = mangle_dupechar_at (out, out_len, 0, ulen);
        break;

      case RULE_OP_MANGLE_DUPECHAR_LAST:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, ulen);
        out_len = mangle_dupechar_at (out, out_len, out_len - 1, ulen);
        break;

      case RULE_OP_MANGLE_DUPECHAR_ALL:
        out_len = mangle_dupechar (out, out_len);
        break;

      case RULE_OP_MANGLE_DUPEBLOCK_FIRST:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, ulen);
        out_len = mangle_dupeblock_prepend (out, out_len, ulen);
        break;

      case RULE_OP_MANGLE_DUPEBLOCK_LAST:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, ulen);
        out_len = mangle_dupeblock_append (out, out_len, ulen);
        break;

      case RULE_OP_MANGLE_SWITCH_FIRST:
        if (out_len >= 2) mangle_switch_at (out, out_len, 0, 1);
        break;

      case RULE_OP_MANGLE_SWITCH_LAST:
        if (out_len >= 2) mangle_switch_at (out, out_len, out_len - 1, out_len - 2);
        break;

      case RULE_OP_MANGLE_SWITCH_AT:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos2);
        out_len = mangle_switch_at_check (out, out_len, upos, upos2);
        break;

      case RULE_OP_MANGLE_CHR_SHIFTL:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        mangle_chr_shiftl (out, out_len, upos);
        break;

      case RULE_OP_MANGLE_CHR_SHIFTR:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        mangle_chr_shiftr (out, out_len, upos);
        break;

      case RULE_OP_MANGLE_CHR_INCR:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        mangle_chr_incr (out, out_len, upos);
        break;

      case RULE_OP_MANGLE_CHR_DECR:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        mangle_chr_decr (out, out_len, upos);
        break;

      case RULE_OP_MANGLE_REPLACE_NP1:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        if ((upos >= 0) && ((upos + 1) < out_len)) mangle_overstrike (out, out_len, upos, out[upos + 1]);
        break;

      case RULE_OP_MANGLE_REPLACE_NM1:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        if ((upos >= 1) && ((upos + 0) < out_len)) mangle_overstrike (out, out_len, upos, out[upos - 1]);
        break;

      case RULE_OP_MANGLE_TITLE:
        out_len = mangle_title (out, out_len);
        break;

      case RULE_OP_MANGLE_EXTRACT_MEMORY:
        if (mem_len < 1) return (RULE_RC_REJECT_ERROR);
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, ulen);
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos2);
        if ((out_len = mangle_insert_multi (out, out_len, upos2, mem, mem_len, upos, ulen)) < 1) return (out_len);
        break;

      case RULE_OP_MANGLE_APPEND_MEMORY:
        if (mem_len < 1) return (RULE_RC_REJECT_ERROR);
        if ((out_len + mem_len) > BLOCK_SIZE) return (RULE_RC_REJECT_ERROR);
        memcpy (out + out_len, mem, mem_len);
        out_len += mem_len;
        break;

      case RULE_OP_MANGLE_PREPEND_MEMORY:
        if (mem_len < 1) return (RULE_RC_REJECT_ERROR);
        if ((mem_len + out_len) > BLOCK_SIZE) return (RULE_RC_REJECT_ERROR);
        memcpy (mem + mem_len, out, out_len);
        out_len += mem_len;
        memcpy (out, mem, out_len);
        break;

      case RULE_OP_MEMORIZE_WORD:
        memcpy (mem, out, out_len);
        mem_len = out_len;
        break;

      case RULE_OP_REJECT_LESS:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        if (out_len > upos) return (RULE_RC_REJECT_ERROR);
        break;

      case RULE_OP_REJECT_GREATER:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        if (out_len < upos) return (RULE_RC_REJECT_ERROR);
        break;

      case RULE_OP_REJECT_CONTAIN:
        NEXT_RULEPOS (rule_pos);
        if (strchr (out, rule[rule_pos]) != NULL) return (RULE_RC_REJECT_ERROR);
        break;

      case RULE_OP_REJECT_NOT_CONTAIN:
        NEXT_RULEPOS (rule_pos);
        if (strchr (out, rule[rule_pos]) == NULL) return (RULE_RC_REJECT_ERROR);
        break;

      case RULE_OP_REJECT_EQUAL_FIRST:
        NEXT_RULEPOS (rule_pos);
        if (out[0] != rule[rule_pos]) return (RULE_RC_REJECT_ERROR);
        break;

      case RULE_OP_REJECT_EQUAL_LAST:
        NEXT_RULEPOS (rule_pos);
        if (out[out_len - 1] != rule[rule_pos]) return (RULE_RC_REJECT_ERROR);
        break;

      case RULE_OP_REJECT_EQUAL_AT:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        if ((upos + 1) > out_len) return (RULE_RC_REJECT_ERROR);
        NEXT_RULEPOS (rule_pos);
        if (out[upos] != rule[rule_pos]) return (RULE_RC_REJECT_ERROR);
        break;

      case RULE_OP_REJECT_CONTAINS:
        NEXT_RULEPOS (rule_pos);
        NEXT_RPTOI (rule, rule_pos, upos);
        if ((upos + 1) > out_len) return (RULE_RC_REJECT_ERROR);
        NEXT_RULEPOS (rule_pos);
        int c; int cnt; for (c = 0, cnt = 0; c < out_len; c++) if (out[c] == rule[rule_pos]) cnt++;
        if (cnt < upos) return (RULE_RC_REJECT_ERROR);
        break;

      case RULE_OP_REJECT_MEMORY:
        if ((out_len == mem_len) && (memcmp (out, mem, out_len) == 0)) return (RULE_RC_REJECT_ERROR);
        break;

      default:
        return (RULE_RC_SYNTAX_ERROR);
    }
  }

  memset (out + out_len, 0, BLOCK_SIZE - out_len);

  return (out_len);
}

int cpu_rule_to_kernel_rule (char *rule_buf, uint rule_len, kernel_rule_t *rule)
{
  uint rule_pos;
  uint rule_cnt;

  for (rule_pos = 0, rule_cnt = 0; rule_pos < rule_len && rule_cnt < MAX_KERNEL_RULES; rule_pos++, rule_cnt++)
  {
    switch (rule_buf[rule_pos])
    {
      case ' ':
        rule_cnt--;
        break;

      case RULE_OP_MANGLE_NOOP:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_LREST:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_UREST:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_LREST_UFIRST:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_UREST_LFIRST:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_TREST:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_TOGGLE_AT:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_REVERSE:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_DUPEWORD:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_DUPEWORD_TIMES:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_REFLECT:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_ROTATE_LEFT:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_ROTATE_RIGHT:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_APPEND:
        SET_NAME (rule, rule_buf[rule_pos]);
        SET_P0   (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_PREPEND:
        SET_NAME (rule, rule_buf[rule_pos]);
        SET_P0   (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_DELETE_FIRST:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_DELETE_LAST:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_DELETE_AT:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_EXTRACT:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        SET_P1_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_OMIT:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        SET_P1_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_INSERT:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        SET_P1      (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_OVERSTRIKE:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        SET_P1      (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_TRUNCATE_AT:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_REPLACE:
        SET_NAME (rule, rule_buf[rule_pos]);
        SET_P0   (rule, rule_buf[rule_pos]);
        SET_P1   (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_PURGECHAR:
        SET_NAME (rule, rule_buf[rule_pos]);
        SET_P0   (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_TOGGLECASE_REC:
        return -1;

      case RULE_OP_MANGLE_DUPECHAR_FIRST:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_DUPECHAR_LAST:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_DUPECHAR_ALL:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_SWITCH_FIRST:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_SWITCH_LAST:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_SWITCH_AT:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        SET_P1_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_CHR_SHIFTL:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_CHR_SHIFTR:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_CHR_INCR:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_CHR_DECR:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_REPLACE_NP1:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_REPLACE_NM1:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_DUPEBLOCK_FIRST:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_DUPEBLOCK_LAST:
        SET_NAME    (rule, rule_buf[rule_pos]);
        SET_P0_CONV (rule, rule_buf[rule_pos]);
        break;

      case RULE_OP_MANGLE_TITLE:
        SET_NAME (rule, rule_buf[rule_pos]);
        break;

      default:
        return -1;
    }
  }

  if (rule_pos < rule_len) return -1;

  return 0;
}

int kernel_rule_to_cpu_rule (char *rule_buf, kernel_rule_t *rule)
{
  uint rule_cnt;
  uint rule_pos;
  uint rule_len = HCBUFSIZ_LARGE - 1; // maximum possible len

  char rule_cmd;

  for (rule_cnt = 0, rule_pos = 0; rule_pos < rule_len && rule_cnt < MAX_KERNEL_RULES; rule_pos++, rule_cnt++)
  {
    GET_NAME (rule);

    if (rule_cnt > 0) rule_buf[rule_pos++] = ' ';

    switch (rule_cmd)
    {
      case RULE_OP_MANGLE_NOOP:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_LREST:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_UREST:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_LREST_UFIRST:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_UREST_LFIRST:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_TREST:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_TOGGLE_AT:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_REVERSE:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_DUPEWORD:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_DUPEWORD_TIMES:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_REFLECT:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_ROTATE_LEFT:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_ROTATE_RIGHT:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_APPEND:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0 (rule);
        break;

      case RULE_OP_MANGLE_PREPEND:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0 (rule);
        break;

      case RULE_OP_MANGLE_DELETE_FIRST:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_DELETE_LAST:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_DELETE_AT:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_EXTRACT:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        GET_P1_CONV (rule);
        break;

      case RULE_OP_MANGLE_OMIT:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        GET_P1_CONV (rule);
        break;

      case RULE_OP_MANGLE_INSERT:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        GET_P1      (rule);
        break;

      case RULE_OP_MANGLE_OVERSTRIKE:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        GET_P1      (rule);
        break;

      case RULE_OP_MANGLE_TRUNCATE_AT:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_REPLACE:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0 (rule);
        GET_P1 (rule);
        break;

      case RULE_OP_MANGLE_PURGECHAR:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0 (rule);
        break;

      case RULE_OP_MANGLE_TOGGLECASE_REC:
        return -1;

      case RULE_OP_MANGLE_DUPECHAR_FIRST:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_DUPECHAR_LAST:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_DUPECHAR_ALL:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_SWITCH_FIRST:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_SWITCH_LAST:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case RULE_OP_MANGLE_SWITCH_AT:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        GET_P1_CONV (rule);
        break;

      case RULE_OP_MANGLE_CHR_SHIFTL:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_CHR_SHIFTR:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_CHR_INCR:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_CHR_DECR:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_REPLACE_NP1:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_REPLACE_NM1:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_DUPEBLOCK_FIRST:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_DUPEBLOCK_LAST:
        rule_buf[rule_pos] = rule_cmd;
        GET_P0_CONV (rule);
        break;

      case RULE_OP_MANGLE_TITLE:
        rule_buf[rule_pos] = rule_cmd;
        break;

      case 0:
        return rule_pos - 1;

      default:
        return -1;
    }
  }

  if (rule_cnt > 0)
  {
    return rule_pos;
  }

  return -1;
}

int run_rule_engine (const int rule_len, const char *rule_buf)
{
  if (rule_len == 0)
  {
    return 0;
  }
  else if (rule_len == 1)
  {
    if (rule_buf[0] == RULE_OP_MANGLE_NOOP) return 0;
  }

  return 1;
}

