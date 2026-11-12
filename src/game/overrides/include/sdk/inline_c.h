/* Port replacement for the Psy-Q inline GTE macros. The originals are MIPS
 * coprocessor instructions; here each one is a call into the port's GTE
 * (src/psx/gte.c). Only the macros the game uses are provided, so a new use
 * shows up as an undefined symbol instead of silently doing nothing. */
#ifndef SDK_INLINE_C_H
#define SDK_INLINE_C_H

void psx_gte_ldv0(const void *v);
void psx_gte_ldv3(const void *v0, const void *v1, const void *v2);
void psx_gte_ldlvl(const void *v);
void psx_gte_ldsxy3(unsigned int sxy0, unsigned int sxy1, unsigned int sxy2);
void psx_gte_rtps(void);
void psx_gte_rtpt(void);
void psx_gte_rtv0(void);
void psx_gte_rtv0tr(void);
void psx_gte_nclip(void);
void psx_gte_ncs(void);
void psx_gte_sqr0(void);
void psx_gte_sqr12(void);
void psx_gte_stflg(void *dst);
void psx_gte_stlvnl(void *dst);
void psx_gte_stopz(void *dst);
void psx_gte_strgb(void *dst);
void psx_gte_stsv(void *dst);
void psx_gte_stsxy2(void *dst);
void psx_gte_stsxy01(void *dst0, void *dst1);
void psx_gte_stsxy3(void *dst0, void *dst1, void *dst2);
void psx_gte_stszotz(void *dst);
void psx_gte_set_rot_matrix(const void *m);
void psx_gte_set_trans_matrix(const void *m);
void psx_gte_set_back_color(int r, int g, int b);
void psx_gte_set_color_matrix(const void *m);
void psx_gte_set_light_matrix(const void *m);

#define gte_ldv0(r0) psx_gte_ldv0(r0)
#define gte_ldv3(r0, r1, r2) psx_gte_ldv3(r0, r1, r2)
#define gte_ldlvl(r0) psx_gte_ldlvl(r0)
#define gte_ldsxy3(r0, r1, r2) psx_gte_ldsxy3((unsigned int)(r0), (unsigned int)(r1), (unsigned int)(r2))
#define gte_rtps() psx_gte_rtps()
#define gte_rtpt() psx_gte_rtpt()
#define gte_rtv0() psx_gte_rtv0()
#define gte_rtv0tr() psx_gte_rtv0tr()
#define gte_nclip() psx_gte_nclip()
#define gte_ncs() psx_gte_ncs()
#define gte_sqr0() psx_gte_sqr0()
#define gte_sqr12() psx_gte_sqr12()
#define gte_stflg(r0) psx_gte_stflg(r0)
#define gte_stlvnl(r0) psx_gte_stlvnl(r0)
#define gte_stopz(r0) psx_gte_stopz(r0)
#define gte_strgb(r0) psx_gte_strgb(r0)
#define gte_stsv(r0) psx_gte_stsv(r0)
#define gte_stsxy(r0) psx_gte_stsxy2(r0)
#define gte_stsxy2(r0) psx_gte_stsxy2(r0)
#define gte_stsxy01(r0, r1) psx_gte_stsxy01(r0, r1)
#define gte_stsxy3(r0, r1, r2) psx_gte_stsxy3(r0, r1, r2)
#define gte_stszotz(r0) psx_gte_stszotz(r0)
#define gte_SetRotMatrix(r0) psx_gte_set_rot_matrix(r0)
#define gte_SetTransMatrix(r0) psx_gte_set_trans_matrix(r0)
#define gte_SetBackColor(r0, r1, r2) psx_gte_set_back_color(r0, r1, r2)
#define gte_SetColorMatrix(r0) psx_gte_set_color_matrix(r0)
#define gte_SetLightMatrix(r0) psx_gte_set_light_matrix(r0)

#endif
