/* 4x4 transforms -- see transform.h. */
#include "transform.h"

#include <math.h>
#include <string.h>

void comp_transform_identity(CompTransform *t)
{
    memset(t, 0, sizeof(*t));
    for (int i = 0; i < 4; i++)
        t->m[i][i] = 1.0f;
}

bool comp_transform_is_identity(const CompTransform *t)
{
    /* Exact comparison on purpose: the identity here is always one this
     * code wrote itself, never the result of accumulated arithmetic, and
     * an epsilon would only hide an effect that thinks it's doing
     * nothing while it isn't. */
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            if (t->m[i][j] != (i == j ? 1.0f : 0.0f))
                return false;
    return true;
}

void comp_transform_multiply(CompTransform *out, const CompTransform *a, const CompTransform *b)
{
    CompTransform r;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++)
                s += a->m[i][k] * b->m[k][j];
            r.m[i][j] = s;
        }
    }
    *out = r;
}

void comp_transform_translate(CompTransform *t, float dx, float dy)
{
    CompTransform op;
    comp_transform_identity(&op);
    op.m[0][3] = dx;
    op.m[1][3] = dy;
    comp_transform_multiply(t, &op, t);
}

void comp_transform_scale(CompTransform *t, float sx, float sy)
{
    CompTransform op;
    comp_transform_identity(&op);
    op.m[0][0] = sx;
    op.m[1][1] = sy;
    comp_transform_multiply(t, &op, t);
}

void comp_transform_point(const CompTransform *t, float x, float y, float *ox, float *oy)
{
    float w = t->m[3][0] * x + t->m[3][1] * y + t->m[3][3];
    if (w == 0.0f)
        w = 1.0f;
    *ox = (t->m[0][0] * x + t->m[0][1] * y + t->m[0][3]) / w;
    *oy = (t->m[1][0] * x + t->m[1][1] * y + t->m[1][3]) / w;
}

bool comp_transform_invert_affine(const CompTransform *t, CompTransform *out)
{
    /* Anything with a perspective row isn't affine, and nothing in this
     * compositor produces one yet -- when the cube does, it will need the
     * GL renderer anyway. */
    if (t->m[3][0] != 0.0f || t->m[3][1] != 0.0f || t->m[3][3] != 1.0f)
        return false;

    float a = t->m[0][0], b = t->m[0][1], tx = t->m[0][3];
    float c = t->m[1][0], d = t->m[1][1], ty = t->m[1][3];

    float det = a * d - b * c;
    if (fabsf(det) < 1e-9f)
        return false;

    comp_transform_identity(out);
    out->m[0][0] =  d / det;
    out->m[0][1] = -b / det;
    out->m[1][0] = -c / det;
    out->m[1][1] =  a / det;
    out->m[0][3] = (b * ty - d * tx) / det;
    out->m[1][3] = (c * tx - a * ty) / det;
    return true;
}
