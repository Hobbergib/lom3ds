/* The geometry coprocessor (GTE) and the libgte functions built on it.
 *
 * Only the operations the game uses are here. Arithmetic follows the
 * hardware: 12-bit fixed point, results saturated into the IR, SZ and SXY
 * registers, and overflow recorded in the flag register, which the game
 * reads to reject points that fall outside the screen. */
#include "psx.h"

#include <math.h>
#include <string.h>

#define ONE 4096
#define ANGLE_MASK 4095
#define MATRIX_STACK_DEPTH 20

/* Flag register bits. */
#define FLAG_IR1 (1u << 24)
#define FLAG_IR2 (1u << 23)
#define FLAG_IR3 (1u << 22)
#define FLAG_SZ (1u << 18)
#define FLAG_DIVIDE (1u << 17)
#define FLAG_SX (1u << 14)
#define FLAG_SY (1u << 13)
#define FLAG_IR0 (1u << 12)
#define FLAG_ERROR_BITS 0x7F87E000u /* bits summarised in bit 31 */

typedef struct {
    int16_t m[3][3];
    int32_t t[3];
} Matrix;

typedef struct {
    int32_t x, y, z, pad;
} Vector;

typedef struct {
    int16_t x, y, z, pad;
} SVector;

typedef struct {
    SVector v[3];
    uint32_t rgbc;
    int16_t ir[4];
    int32_t mac[4];
    uint32_t sxy[3]; /* x in the low half, y in the high half */
    uint16_t sz[4];
    uint32_t rgb2;
    int16_t rot[3][3], light[3][3], color[3][3];
    int32_t trans[3], back[3];
    int32_t ofx, ofy;
    uint16_t h;
    int16_t dqa;
    int32_t dqb;
    uint32_t flag;
} Gte;

static Gte s_gte;
static Matrix s_stack[MATRIX_STACK_DEPTH];
static int s_stack_top;
static int16_t s_sin[ONE];

static int32_t limit(int64_t v, int32_t lo, int32_t hi, uint32_t flag)
{
    if (v < lo) {
        s_gte.flag |= flag;
        return lo;
    }
    if (v > hi) {
        s_gte.flag |= flag;
        return hi;
    }
    return (int32_t)v;
}

static void set_ir(int lm)
{
    int32_t lo = lm ? 0 : -0x8000;

    s_gte.ir[1] = (int16_t)limit(s_gte.mac[1], lo, 0x7FFF, FLAG_IR1);
    s_gte.ir[2] = (int16_t)limit(s_gte.mac[2], lo, 0x7FFF, FLAG_IR2);
    s_gte.ir[3] = (int16_t)limit(s_gte.mac[3], lo, 0x7FFF, FLAG_IR3);
}

static void finish_flags(void)
{
    if (s_gte.flag & FLAG_ERROR_BITS) {
        s_gte.flag |= 0x80000000u;
    }
}

/* MAC1..3 = (add + m * v) >> 12, with `add` already shifted up by 12. */
static void mul_matrix(int16_t m[3][3], const int32_t *add, int x, int y, int z)
{
    int i;

    for (i = 0; i < 3; i++) {
        int64_t sum = (add ? (int64_t)add[i] << 12 : 0) + (int64_t)m[i][0] * x + (int64_t)m[i][1] * y +
                      (int64_t)m[i][2] * z;
        s_gte.mac[i + 1] = (int32_t)(sum >> 12);
    }
}

/* The hardware's reciprocal division: H / SZ3 as 1.16 fixed point. */
static uint32_t divide(uint16_t h, uint16_t sz)
{
    uint32_t n, d, u;
    int shift;

    if (h >= sz * 2u) {
        s_gte.flag |= FLAG_DIVIDE;
        return 0x1FFFF;
    }
    for (shift = 0; !((sz << shift) & 0x8000); shift++) {
    }
    n = (uint32_t)h << shift;
    d = (uint32_t)sz << shift;
    /* One Newton-Raphson step from a 257-entry seed table. */
    {
        uint32_t i = (d - 0x7FC0) >> 7;
        int32_t seed = (int32_t)((0x40000 / (i + 0x100) + 1) / 2) - 0x101;
        u = (uint32_t)(seed < 0 ? 0 : seed) + 0x101;
    }
    d = (0x2000080 - d * u) >> 8;
    d = (0x0000080 + d * u) >> 8;
    n = (uint32_t)(((uint64_t)n * d + 0x8000) >> 16);
    return n > 0x1FFFF ? 0x1FFFF : n;
}

static void transform_point(int index, bool depth_cue)
{
    const SVector *v = &s_gte.v[index];
    int64_t x, y;
    uint32_t n;

    mul_matrix(s_gte.rot, s_gte.trans, v->x, v->y, v->z);
    set_ir(0);
    s_gte.sz[0] = s_gte.sz[1];
    s_gte.sz[1] = s_gte.sz[2];
    s_gte.sz[2] = s_gte.sz[3];
    s_gte.sz[3] = (uint16_t)limit(s_gte.mac[3], 0, 0xFFFF, FLAG_SZ);
    n = divide(s_gte.h, s_gte.sz[3]);

    x = (int64_t)n * s_gte.ir[1] + s_gte.ofx;
    y = (int64_t)n * s_gte.ir[2] + s_gte.ofy;
    s_gte.sxy[0] = s_gte.sxy[1];
    s_gte.sxy[1] = s_gte.sxy[2];
    s_gte.sxy[2] = ((uint32_t)limit(x >> 16, -0x400, 0x3FF, FLAG_SX) & 0xFFFF) |
                   ((uint32_t)limit(y >> 16, -0x400, 0x3FF, FLAG_SY) << 16);
    s_gte.mac[0] = (int32_t)y;
    if (depth_cue) {
        int64_t depth = (int64_t)n * s_gte.dqa + s_gte.dqb;
        s_gte.mac[0] = (int32_t)depth;
        s_gte.ir[0] = (int16_t)limit(depth >> 12, 0, 0x1000, FLAG_IR0);
    }
}

/* --- the inline macros (see overrides/include/sdk/inline_c.h) ----------- */

void psx_gte_ldv0(const void *v)
{
    memcpy(&s_gte.v[0], v, 6);
}

void psx_gte_ldv3(const void *v0, const void *v1, const void *v2)
{
    memcpy(&s_gte.v[0], v0, 6);
    memcpy(&s_gte.v[1], v1, 6);
    memcpy(&s_gte.v[2], v2, 6);
}

void psx_gte_ldlvl(const void *v)
{
    const int32_t *w = v;

    s_gte.ir[1] = (int16_t)w[0];
    s_gte.ir[2] = (int16_t)w[1];
    s_gte.ir[3] = (int16_t)w[2];
}

void psx_gte_ldsxy3(uint32_t sxy0, uint32_t sxy1, uint32_t sxy2)
{
    s_gte.sxy[0] = sxy0;
    s_gte.sxy[1] = sxy1;
    s_gte.sxy[2] = sxy2;
}

void psx_gte_rtps(void)
{
    s_gte.flag = 0;
    transform_point(0, true);
    finish_flags();
}

void psx_gte_rtpt(void)
{
    s_gte.flag = 0;
    transform_point(0, false);
    transform_point(1, false);
    transform_point(2, true);
    finish_flags();
}

void psx_gte_rtv0(void)
{
    s_gte.flag = 0;
    mul_matrix(s_gte.rot, NULL, s_gte.v[0].x, s_gte.v[0].y, s_gte.v[0].z);
    set_ir(0);
    finish_flags();
}

void psx_gte_rtv0tr(void)
{
    s_gte.flag = 0;
    mul_matrix(s_gte.rot, s_gte.trans, s_gte.v[0].x, s_gte.v[0].y, s_gte.v[0].z);
    set_ir(0);
    finish_flags();
}

static int sx(int i)
{
    return (int16_t)s_gte.sxy[i];
}

static int sy(int i)
{
    return (int16_t)(s_gte.sxy[i] >> 16);
}

void psx_gte_nclip(void)
{
    s_gte.flag = 0;
    s_gte.mac[0] = sx(0) * sy(1) + sx(1) * sy(2) + sx(2) * sy(0) - sx(0) * sy(2) - sx(1) * sy(0) - sx(2) * sy(1);
}

/* Lights one normal: light matrix, then colour matrix plus background. */
void psx_gte_ncs(void)
{
    int i;

    s_gte.flag = 0;
    mul_matrix(s_gte.light, NULL, s_gte.v[0].x, s_gte.v[0].y, s_gte.v[0].z);
    set_ir(1);
    mul_matrix(s_gte.color, s_gte.back, s_gte.ir[1], s_gte.ir[2], s_gte.ir[3]);
    set_ir(1);
    s_gte.rgb2 = s_gte.rgbc & 0xFF000000u;
    for (i = 0; i < 3; i++) {
        s_gte.rgb2 |= (uint32_t)limit(s_gte.mac[i + 1] >> 4, 0, 255, 0) << (8 * i);
    }
    finish_flags();
}

static void square(int shift)
{
    int i;

    s_gte.flag = 0;
    for (i = 1; i <= 3; i++) {
        s_gte.mac[i] = (s_gte.ir[i] * s_gte.ir[i]) >> shift;
    }
    set_ir(1);
    finish_flags();
}

void psx_gte_sqr0(void)
{
    square(0);
}

void psx_gte_sqr12(void)
{
    square(12);
}

void psx_gte_stflg(void *dst)
{
    *(uint32_t *)dst = s_gte.flag;
}

void psx_gte_stlvnl(void *dst)
{
    memcpy(dst, &s_gte.mac[1], 12);
}

void psx_gte_stopz(void *dst)
{
    *(int32_t *)dst = s_gte.mac[0];
}

void psx_gte_strgb(void *dst)
{
    *(uint32_t *)dst = s_gte.rgb2;
}

void psx_gte_stsv(void *dst)
{
    memcpy(dst, &s_gte.ir[1], 6);
}

void psx_gte_stsxy2(void *dst)
{
    *(uint32_t *)dst = s_gte.sxy[2];
}

void psx_gte_stsxy01(void *dst0, void *dst1)
{
    *(uint32_t *)dst0 = s_gte.sxy[0];
    *(uint32_t *)dst1 = s_gte.sxy[1];
}

void psx_gte_stsxy3(void *dst0, void *dst1, void *dst2)
{
    *(uint32_t *)dst0 = s_gte.sxy[0];
    *(uint32_t *)dst1 = s_gte.sxy[1];
    *(uint32_t *)dst2 = s_gte.sxy[2];
}

void psx_gte_stszotz(void *dst)
{
    *(int32_t *)dst = s_gte.sz[3] >> 2;
}

void psx_gte_set_rot_matrix(const void *m)
{
    memcpy(s_gte.rot, m, sizeof s_gte.rot);
}

void psx_gte_set_trans_matrix(const void *m)
{
    memcpy(s_gte.trans, ((const Matrix *)m)->t, sizeof s_gte.trans);
}

void psx_gte_set_back_color(int r, int g, int b)
{
    s_gte.back[0] = r << 4;
    s_gte.back[1] = g << 4;
    s_gte.back[2] = b << 4;
}

void psx_gte_set_color_matrix(const void *m)
{
    memcpy(s_gte.color, m, sizeof s_gte.color);
}

void psx_gte_set_light_matrix(const void *m)
{
    memcpy(s_gte.light, m, sizeof s_gte.light);
}

/* --- libgte ------------------------------------------------------------- */

int psx_rsin(int angle)
{
    return s_sin[angle & ANGLE_MASK];
}

int psx_rcos(int angle)
{
    return s_sin[(angle + ONE / 4) & ANGLE_MASK];
}

int psx_csin(int angle)
{
    return psx_rsin(angle);
}

int psx_ccos(int angle)
{
    return psx_rcos(angle);
}

long psx_ratan2(long y, long x)
{
    return lround(atan2((double)y, (double)x) * (ONE / 2) / M_PI);
}

long psx_SquareRoot0(long value)
{
    uint32_t v = (uint32_t)value, root = 0, bit = 1u << 30;

    while (bit > v) {
        bit >>= 2;
    }
    for (; bit; bit >>= 2) {
        if (v >= root + bit) {
            v -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
    }
    return (long)root;
}

void psx_InitGeom(void)
{
    int i;

    memset(&s_gte, 0, sizeof s_gte);
    s_gte.h = 1000;
    s_gte.dqa = -98;
    s_gte.dqb = 340;
    s_stack_top = 0;
    for (i = 0; i < ONE; i++) {
        s_sin[i] = (int16_t)lround(sin(i * 2.0 * M_PI / ONE) * ONE);
    }
}

void psx_SetGeomOffset(long ofx, long ofy)
{
    s_gte.ofx = (int32_t)ofx << 16;
    s_gte.ofy = (int32_t)ofy << 16;
}

void psx_SetGeomScreen(long h)
{
    s_gte.h = (uint16_t)h;
}

void psx_SetRotMatrix(const Matrix *m)
{
    psx_gte_set_rot_matrix(m);
}

void psx_SetTransMatrix(const Matrix *m)
{
    psx_gte_set_trans_matrix(m);
}

void psx_PushMatrix(void)
{
    if (s_stack_top >= MATRIX_STACK_DEPTH) {
        psx_fatal("matrix stack overflow");
    }
    memcpy(s_stack[s_stack_top].m, s_gte.rot, sizeof s_gte.rot);
    memcpy(s_stack[s_stack_top].t, s_gte.trans, sizeof s_gte.trans);
    s_stack_top++;
}

void psx_PopMatrix(void)
{
    if (s_stack_top == 0) {
        psx_fatal("matrix stack underflow");
    }
    s_stack_top--;
    memcpy(s_gte.rot, s_stack[s_stack_top].m, sizeof s_gte.rot);
    memcpy(s_gte.trans, s_stack[s_stack_top].t, sizeof s_gte.trans);
}

/* Rotation about X, then Y, then Z: m = Rx * Ry * Rz. */
Matrix *psx_RotMatrix(const SVector *r, Matrix *m)
{
    int sx_ = psx_rsin(r->x), cx = psx_rcos(r->x);
    int sy_ = psx_rsin(r->y), cy = psx_rcos(r->y);
    int sz = psx_rsin(r->z), cz = psx_rcos(r->z);
    int sxsy = (sx_ * sy_) >> 12, cxsy = (cx * sy_) >> 12;

    m->m[0][0] = (int16_t)((cy * cz) >> 12);
    m->m[0][1] = (int16_t)(-((cy * sz) >> 12));
    m->m[0][2] = (int16_t)sy_;
    m->m[1][0] = (int16_t)(((cx * sz) >> 12) + ((sxsy * cz) >> 12));
    m->m[1][1] = (int16_t)(((cx * cz) >> 12) - ((sxsy * sz) >> 12));
    m->m[1][2] = (int16_t)(-((sx_ * cy) >> 12));
    m->m[2][0] = (int16_t)(((sx_ * sz) >> 12) - ((cxsy * cz) >> 12));
    m->m[2][1] = (int16_t)(((sx_ * cz) >> 12) + ((cxsy * sz) >> 12));
    m->m[2][2] = (int16_t)((cx * cy) >> 12);
    return m;
}

Matrix *psx_RotMatrix_gte(const SVector *r, Matrix *m)
{
    return psx_RotMatrix(r, m);
}

/* Rotates rows `a` and `b` of m by an angle: the single-axis rotations. */
static Matrix *rotate_rows(Matrix *m, int a, int b, long angle)
{
    int s = psx_rsin((int)angle), c = psx_rcos((int)angle), i;

    for (i = 0; i < 3; i++) {
        int ta = m->m[a][i], tb = m->m[b][i];

        m->m[a][i] = (int16_t)((c * ta - s * tb) >> 12);
        m->m[b][i] = (int16_t)((s * ta + c * tb) >> 12);
    }
    return m;
}

Matrix *psx_RotMatrixX(long r, Matrix *m)
{
    return rotate_rows(m, 1, 2, r);
}

Matrix *psx_RotMatrixY(long r, Matrix *m)
{
    return rotate_rows(m, 2, 0, r);
}

Matrix *psx_RotMatrixZ(long r, Matrix *m)
{
    return rotate_rows(m, 0, 1, r);
}

Matrix *psx_TransMatrix(Matrix *m, const Vector *v)
{
    m->t[0] = v->x;
    m->t[1] = v->y;
    m->t[2] = v->z;
    return m;
}

/* m = m * diag(v): each column is scaled. (Scaling rows is ScaleMatrixL.) */
Matrix *psx_ScaleMatrix(Matrix *m, const Vector *v)
{
    const int32_t scale[3] = {v->x, v->y, v->z};
    int i, j;

    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            m->m[i][j] = (int16_t)(((int64_t)m->m[i][j] * scale[j]) >> 12);
        }
    }
    return m;
}

Vector *psx_ApplyMatrixLV(const Matrix *m, const Vector *v0, Vector *v1)
{
    Vector in = *v0;

    v1->x = (int32_t)(((int64_t)m->m[0][0] * in.x + (int64_t)m->m[0][1] * in.y + (int64_t)m->m[0][2] * in.z) >> 12);
    v1->y = (int32_t)(((int64_t)m->m[1][0] * in.x + (int64_t)m->m[1][1] * in.y + (int64_t)m->m[1][2] * in.z) >> 12);
    v1->z = (int32_t)(((int64_t)m->m[2][0] * in.x + (int64_t)m->m[2][1] * in.y + (int64_t)m->m[2][2] * in.z) >> 12);
    return v1;
}

/* m2 = m0 * m1, translation included. */
Matrix *psx_CompMatrix(const Matrix *m0, const Matrix *m1, Matrix *m2)
{
    Matrix out;
    int i, j;

    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            out.m[i][j] = (int16_t)((m0->m[i][0] * m1->m[0][j] + m0->m[i][1] * m1->m[1][j] + m0->m[i][2] * m1->m[2][j]) >> 12);
        }
        out.t[i] = (int32_t)(((int64_t)m0->m[i][0] * m1->t[0] + (int64_t)m0->m[i][1] * m1->t[1] +
                              (int64_t)m0->m[i][2] * m1->t[2]) >> 12) + m0->t[i];
    }
    *m2 = out;
    return m2;
}

long psx_NormalClip(uint32_t sxy0, uint32_t sxy1, uint32_t sxy2)
{
    psx_gte_ldsxy3(sxy0, sxy1, sxy2);
    psx_gte_nclip();
    return s_gte.mac[0];
}

/* Scales v0 to length 4096; returns the squared length of the input. */
long psx_VectorNormal(const Vector *v0, Vector *v1)
{
    int64_t sq = (int64_t)v0->x * v0->x + (int64_t)v0->y * v0->y + (int64_t)v0->z * v0->z;
    double len = sqrt((double)sq);

    if (len > 0) {
        v1->x = (int32_t)(v0->x * (double)ONE / len);
        v1->y = (int32_t)(v0->y * (double)ONE / len);
        v1->z = (int32_t)(v0->z * (double)ONE / len);
    } else {
        v1->x = v1->y = v1->z = 0;
    }
    return (long)sq;
}
